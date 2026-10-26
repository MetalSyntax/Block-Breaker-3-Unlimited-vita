#!/usr/bin/env python3
"""Prepara el ejecutable iOS (Mach-O 32 bits, MH_EXECUTE no PIE, armv6/armv7) para el loader de la PS Vita.

Soporta binarios thin (0xFEEDFACE) y fat/universal (0xCAFEBABE) seleccionando el slice ARM óptimo.
Detecta cifrado FairPlay (LC_ENCRYPTION_INFO).
Calcula los punteros absolutos que requieren reubicación en memoria física (rebase.bin)
y extrae el inventario completo de imports dinámicos.

Uso: python3 -I macho_prep.py <Executable> <rebase.bin> [--report report.txt] [--arch armv7|armv6]
"""
import bisect
import os
import struct
import sys
from collections import Counter

LC_SEGMENT = 0x1
LC_SYMTAB = 0x2
LC_DYSYMTAB = 0xB
LC_ENCRYPTION_INFO = 0x21
S_ZEROFILL = 0x1
S_NON_LAZY = 0x6
S_LAZY = 0x7
INDIRECT_LOCAL = 0x80000000
INDIRECT_ABS = 0x40000000
N_STAB = 0xE0
N_TYPE = 0x0E
N_SECT = 0x0E
N_EXT = 0x01
N_ARM_THUMB_DEF = 0x0008

CPU_TYPE_ARM = 12
CPU_SUBTYPE_ARM_V6 = 6
CPU_SUBTYPE_ARM_V7 = 9
CPU_SUBTYPE_ARM_V7S = 11


def parse(path, preferred_arch=None):
    with open(path, 'rb') as f:
        raw_data = f.read()

    slice_offset = 0
    slice_size = len(raw_data)
    detected_arch = "unknown"

    # Chequear si es FAT Mach-O (big-endian 0xCAFEBABE)
    if len(raw_data) >= 8 and raw_data[:4] in (b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca'):
        magic, nfat = struct.unpack_from('>2I', raw_data, 0)
        archs = []
        for i in range(nfat):
            cputype, cpusubtype, offset, size, align = struct.unpack_from('>5I', raw_data, 8 + i * 20)
            arch_name = "arm"
            if cputype == CPU_TYPE_ARM:
                if cpusubtype == CPU_SUBTYPE_ARM_V6:
                    arch_name = "armv6"
                elif cpusubtype == CPU_SUBTYPE_ARM_V7:
                    arch_name = "armv7"
                elif cpusubtype == CPU_SUBTYPE_ARM_V7S:
                    arch_name = "armv7s"
            archs.append((arch_name, cputype, cpusubtype, offset, size))

        # Seleccionar slice
        chosen = None
        if preferred_arch:
            for a in archs:
                if a[0] == preferred_arch.lower():
                    chosen = a
                    break
        if not chosen:
            # Preferir armv7 sobre armv6
            for a in archs:
                if a[0] in ("armv7", "armv7s"):
                    chosen = a
                    break
        if not chosen:
            for a in archs:
                if a[0] == "armv6":
                    chosen = a
                    break
        if not chosen and archs:
            chosen = archs[0]

        if not chosen:
            raise RuntimeError("No se encontró ningún slice ARM en el binario FAT")

        slice_offset = chosen[3]
        slice_size = chosen[4]
        detected_arch = chosen[0]
        d = raw_data[slice_offset:slice_offset + slice_size]
    else:
        d = raw_data

    magic, cpu, sub, ftype, ncmds, _sz, flags = struct.unpack_from('<7I', d, 0)
    if magic != 0xFEEDFACE:
        raise RuntimeError(f"Se espera Mach-O de 32 bits (magic 0xFEEDFACE), obtenido 0x{magic:08X}")
    if ftype != 2:
        print(f"[!] Advertencia: Mach-O ftype={ftype} (se esperaba 2 = MH_EXECUTE)")

    if cpu == CPU_TYPE_ARM:
        if sub == CPU_SUBTYPE_ARM_V6:
            detected_arch = "armv6"
        elif sub == CPU_SUBTYPE_ARM_V7:
            detected_arch = "armv7"
        elif sub == CPU_SUBTYPE_ARM_V7S:
            detected_arch = "armv7s"
        else:
            detected_arch = f"arm(sub={sub})"

    segs, sects, symtab, dysym = [], [], None, None
    cryptid = 0
    off = 28
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<2I', d, off)
        if cmd == LC_SEGMENT:
            name = d[off + 8:off + 24].rstrip(b'\0').decode('latin1', errors='ignore')
            vmaddr, vmsize, fileoff, filesize, _mp, _ip, nsects, _f = struct.unpack_from('<8I', d, off + 24)
            segs.append(dict(name=name, vmaddr=vmaddr, vmsize=vmsize, fileoff=fileoff, filesize=filesize))
            so = off + 56
            for _ in range(nsects):
                sn = d[so:so + 16].rstrip(b'\0').decode('latin1', errors='ignore')
                gn = d[so + 16:so + 32].rstrip(b'\0').decode('latin1', errors='ignore')
                addr, ssize, soff, _al, _ro, _nr, sflags, r1, r2 = struct.unpack_from('<9I', d, so + 32)
                sects.append(dict(seg=gn, name=sn, addr=addr, size=ssize, offset=soff,
                                  type=sflags & 0xFF, r1=r1))
                so += 68
        elif cmd == LC_SYMTAB:
            symtab = struct.unpack_from('<4I', d, off + 8)
        elif cmd == LC_DYSYMTAB:
            dysym = struct.unpack_from('<18I', d, off + 8)
        elif cmd == LC_ENCRYPTION_INFO:
            cryptoff, cryptsize, cryptid = struct.unpack_from('<3I', d, off + 8)
        off += size

    return d, segs, sects, symtab, dysym, detected_arch, cryptid


def main():
    if len(sys.argv) < 3:
        print("Uso: python3 -I macho_prep.py <Executable> <rebase.bin> [--report report.txt] [--arch armv7|armv6]")
        sys.exit(1)

    src, out = sys.argv[1], sys.argv[2]
    report = sys.argv[sys.argv.index('--report') + 1] if '--report' in sys.argv else None
    preferred_arch = sys.argv[sys.argv.index('--arch') + 1] if '--arch' in sys.argv else None

    d, segs, sects, symtab_cmd, dysym, detected_arch, cryptid = parse(src, preferred_arch)

    print(f"[*] Binario: {src}")
    print(f"[*] Arquitectura Mach-O: {detected_arch}")
    if cryptid != 0:
        print(f"[!] ALERTA CRITICA: El binario esta CIFRADO por FairPlay DRM (cryptid = {cryptid})!")
        print(f"[!] Debe ser descifrado en un dispositivo iOS (con Clutch, frida-ios-dump o bfdecrypt) antes de correr.")
    else:
        print(f"[+] Binario descifrado (cryptid = 0), listo para ejecución.")

    if not symtab_cmd:
        raise RuntimeError("No se encontró tabla de símbolos (LC_SYMTAB)")
    symoff, nsyms, stroff, _strsize = symtab_cmd

    lo = min(s['vmaddr'] for s in segs if s['vmsize'] and s['name'] != '__PAGEZERO')
    hi = max(s['vmaddr'] + s['vmsize'] for s in segs if s['name'] != '__LINKEDIT')

    def read32(va):
        for s in segs:
            if s['vmaddr'] <= va < s['vmaddr'] + s['filesize']:
                return struct.unpack_from('<I', d, s['fileoff'] + va - s['vmaddr'])[0]
        return 0

    def sect_of(va):
        for s in sects:
            if s['addr'] <= va < s['addr'] + s['size']:
                return s
        return None

    # Símbolos definidos
    syms, names = [], []
    for i in range(nsyms):
        strx, ntype, nsect, ndesc, value = struct.unpack_from('<IBBhI', d, symoff + i * 12)
        idx_end = d.find(b'\0', stroff + strx)
        name = d[stroff + strx:idx_end].decode('latin1', errors='ignore') if idx_end != -1 else ""
        names.append(name)
        if ntype & N_STAB or (ntype & N_TYPE) != N_SECT:
            continue
        syms.append((value, bool(ndesc & N_ARM_THUMB_DEF), name))
    syms.sort()
    sym_addrs = [s[0] for s in syms]

    text_sect = next((s for s in sects if s['name'] == '__text'), None)
    func_entry = {}
    if text_sect:
        for a, thumb, n in syms:
            if text_sect['addr'] <= a < text_sect['addr'] + text_sect['size']:
                func_entry[a] = n
                func_entry[a | 1] = n
        # BBD3 está stripped (165 símbolos): sin otros inicios de función se rechazan vtables,
        # IMPs ObjC y constructores. Se agregan destinos de bl/blx y prólogos push {..., lr}.
        t0, toff = text_sect['addr'], text_sect['offset']
        t1 = t0 + text_sect['size']
        for va in range(t0 + (-t0 & 3), t1 - 3, 4):
            w = struct.unpack_from('<I', d, toff + va - t0)[0]
            off = (w & 0xFFFFFF) - ((w & 0x800000) << 1)
            if w & 0x0F000000 == 0x0B000000 and w >> 28 != 0xF:  # bl
                func_entry.setdefault(va + 8 + off * 4, 'bl')
            elif w >> 25 == 0x7D:  # blx imm -> Thumb
                func_entry.setdefault((va + 8 + off * 4 + ((w >> 23) & 2)) | 1, 'blx')
            elif w & 0xFFFF4000 == 0xE92D4000:  # ARM push {..., lr}
                func_entry.setdefault(va, 'prologue')
        for va in range(t0 & ~1, t1 - 1, 2):
            if struct.unpack_from('<H', d, toff + va - t0)[0] >> 8 == 0xB5:  # Thumb push {..., lr}
                func_entry.setdefault(va | 1, 'prologue')
        # Vtables en __DATA,__const: en C++ stripped, las vtables contienen punteros directos
        # a métodos virtuales (incluidas funciones hoja como `bx lr` y getters que no tienen
        # prólogo push {..., lr} ni son llamadas por bl/blx directo, e.g. 0x97594).
        const_sect = next((s for s in sects if s['name'] == '__const' and s['seg'] == '__DATA'), None)
        if const_sect:
            c0, coff, c1 = const_sect['addr'], const_sect['offset'], const_sect['addr'] + const_sect['size']
            for cva in range(c0, c1 - 3, 4):
                cv = struct.unpack_from('<I', d, coff + cva - c0)[0]
                if t0 <= (cv & ~1) < t1:
                    if (cv & 3) == 0 or (cv & 1) == 1:
                        func_entry.setdefault(cv, 'vtable')

    stubs = next((s for s in sects if s['name'] in ('__symbol_stub4', '__symbol_stub', '__symbol_stub1')), None)
    if stubs:
        step = 12 if stubs['name'] == '__symbol_stub4' else 16
        for a in range(stubs['addr'], stubs['addr'] + stubs['size'], step):
            func_entry[a] = 'stub'

    def valid_target(v, unaligned=False):
        if not (lo <= v < hi):
            return None
        s = sect_of(v & ~1)
        if s is None:
            return None
        if s['name'] in ('__text', '__symbol_stub4', '__symbol_stub'):
            return 'func' if v in func_entry else None
        if s['name'] in ('__cstring', '__ustring'):
            ok = v == s['addr'] or (read32(v - 1) & 0xFF == 0 if s['name'] == '__cstring' else True)
            return 'cstr' if ok else None
        # Objetos de bytes en __bss/__common pueden ser impares (log_009: 0x5702d9 cargado en 0x349e20);
        # solo se aceptan desde literales de __text, donde el ldr no-PIC ya confirma que es dirección.
        if v & 3 and s['name'] not in ('__const', '__data') and not (unaligned and s['name'] in ('__bss', '__common')):
            return None
        i = bisect.bisect_right(sym_addrs, v) - 1
        if i >= 0 and sym_addrs[i] == v:
            return 'sym'
        if s['type'] == S_ZEROFILL or s['name'] in ('__common', '__bss'):
            return 'bss-interior' if i >= 0 else None
        return 'interior' if v == s['addr'] or (i >= 0 and v - sym_addrs[i] < 0x10000) else None

    handled, imports = set(), Counter()
    local_ptr_slots = []
    if dysym:
        indirectsymoff, nindirect = dysym[12], dysym[13]
        extreloff, nextrel = dysym[14], dysym[15]
        for s in sects:
            if s['type'] in (S_NON_LAZY, S_LAZY):
                for i in range(s['size'] // 4):
                    idx = struct.unpack_from('<I', d, indirectsymoff + (s['r1'] + i) * 4)[0]
                    slot = s['addr'] + i * 4
                    handled.add(slot)
                    if idx & INDIRECT_LOCAL:
                        local_ptr_slots.append(slot)
                    elif not idx & INDIRECT_ABS:
                        if idx < len(names):
                            imports[names[idx]] += 1
        extrel_slots = []
        for i in range(nextrel):
            addr, info = struct.unpack_from('<iI', d, extreloff + i * 8)
            symnum = info & 0xFFFFFF
            va = addr
            extrel_slots.append((va, names[symnum] if symnum < len(names) else f"sym_{symnum}"))
            handled.add(va)
            if symnum < len(names):
                imports[names[symnum]] += 1
    else:
        extrel_slots = []

    rebase, stats, rejected, rejected_list = [], Counter(), Counter(), []
    scan = [s for s in sects if s['type'] != S_ZEROFILL and s['name'] not in ('__text', '__symbol_stub4', '__symbol_stub', '__cstring', '__ustring')
            and s['seg'] in ('__DATA', '__TEXT')]

    PTR_SECTS = ('__mod_init_func', '__objc_classlist', '__objc_nlclslist', '__objc_catlist', '__objc_protolist',
                 '__objc_classrefs', '__objc_superrefs', '__objc_selrefs', '__objc_protorefs', '__objc_const',
                 '__objc_data')
    owner_ok, owner_bad, owner_hits = Counter(), Counter(), {}
    for s in scan:
        for va in range(s['addr'] + (-s['addr'] & 3), s['addr'] + s['size'] - 3, 4):
            if va in handled:
                continue
            v = read32(va)
            k = valid_target(v)
            if not k and s['name'] in PTR_SECTS and sect_of(v & ~1):
                k = 'ptr'  # metadatos solo-punteros: sin símbolos (stripped) igual son punteros
            elif not k and s['seg'] == '__DATA' and s['name'] == '__const' and sect_of(v & ~1):
                tgt = sect_of(v & ~1)
                if tgt['name'] == '__text':
                    if (v & 3) == 0 or (v & 1) == 1:
                        k = 'func'
                elif tgt['name'] in ('__cstring', '__ustring'):
                    k = 'cstr'
                else:
                    k = 'ptr'
            owner = None
            if s['name'] in ('__data', '__const'):
                i = bisect.bisect_right(sym_addrs, va) - 1
                owner = syms[i][2] if i >= 0 and sym_addrs[i] >= s['addr'] else f'{s["seg"]}.{s["name"]}'
            if k:
                if owner:
                    owner_ok[owner] += 1
                    owner_hits.setdefault(owner, []).append((va, s['name'], k))
                else:
                    rebase.append(va)
                    stats[(s['name'], k)] += 1
            elif lo <= v < hi:
                rejected[s['name']] += 1
                rejected_list.append((va, v))
                if owner:
                    owner_bad[owner] += 1

    dropped = []
    for owner, hits in owner_hits.items():
        if owner_bad[owner] > owner_ok[owner]:
            dropped.append((owner, owner_ok[owner], owner_bad[owner]))
            # Conservar destinos a símbolos reales
            for va, sn, k in hits:
                if k in ('sym', 'func', 'cstr'):
                    rebase.append(va)
                    stats[(sn, k)] += 1
            continue
        for va, sn, k in hits:
            rebase.append(va)
            stats[(sn, k)] += 1

    rebase += local_ptr_slots
    stats[('__nl_symbol_ptr', 'local')] = len(local_ptr_slots)

    if stubs and stubs['name'] == '__symbol_stub4':
        stub_lits = list(range(stubs['addr'] + 8, stubs['addr'] + stubs['size'], 12))
        valid_stubs = [a for a in stub_lits if read32(a - 8) == 0xE59FC000 and lo <= read32(a) < hi]
        rebase += valid_stubs
        stats[('__symbol_stub4', 'la_ptr')] = len(valid_stubs)

    # Pools literales embebidos dentro de __text (en este binario no son una sección
    # aparte): cada `ldr rX,[pc,#imm]` (ARM, condicional incluido, o Thumb) cuya
    # palabra literal contenga un puntero válido al guest debe rebasarse, si no
    # almacena/lee datos de __DATA con la dirección sin deslizar (crash 0x35F628).
    text_sect = next((s for s in sects if s['name'] == '__text'), None)
    if text_sect:
        tbase, toff, tsize = text_sect['addr'], text_sect['offset'], text_sect['size']
        text_end = tbase + tsize
        lit_set, pic_set, lit_rej = set(), set(), set()

        def arm_pc_use(va, rd):
            # ¿rd se usa como offset PC-relativo (add rX,pc,rd / ldr rX,[pc,rd]) antes de
            # ser sobrescrito o de que la función retorne? Entonces el literal es un offset
            # PIC, no un puntero (falsos positivos 0x4450 en log_007 y 0x37AFC en log_008,
            # este último usado 18 instrucciones después).
            # Unconditional `b` is followed (log_023: ldr lr at 0x41a94, `b 0x42198`, `add r1, pc, lr` there).
            nva = va
            for _ in range(64):
                nva += 4
                if not tbase <= nva < text_end:
                    return False
                w = struct.unpack_from('<I', d, toff + nva - tbase)[0]
                if w >> 28 == 0xE and (w & 0x0F000000) == 0x0A000000:
                    nva += 4 + ((w & 0xFFFFFF) - ((w & 0x800000) << 1)) * 4  # +8 - 4: the loop adds 4
                    continue
                rn, rm, rt = (w >> 16) & 0xF, w & 0xF, (w >> 12) & 0xF
                if (w & 0x0FE00010) == 0x00800000 and {rn, rm} == {15, rd}:
                    return True
                if (w & 0x0E000010) == 0x06000000 and rn == 15 and rm == rd:
                    return True
                if (w & 0x0E400F90) == 0x00000090 and (w >> 5) & 3 and rn == 15 and rm == rd:
                    return True
                if w >> 28 == 0xE and (w & 0x0FFFFFF0) == 0x012FFF10:
                    return False  # bx incondicional: fin del bloque
                if (w & 0x0E108000) == 0x08108000:
                    return False  # ldm/pop con pc
                if rt == rd:
                    if (w & 0x0FB00000) in (0x03000000, 0x03400000):
                        return False  # movw/movt
                    if (w & 0x0C000000) == 0 and not 8 <= (w >> 21) & 0xF <= 11 and (w & 0x0E000090) != 0x00000090:
                        return False  # data-processing que escribe rd
                    if (w & 0x0C100000) == 0x04100000 or (w & 0x0E100090) == 0x00100090:
                        return False  # ldr/ldrb/ldrh/ldrsb que escribe rd
            return False

        def thumb_pc_use(va, rd):
            # Thumb: `add rd, pc` tras cargar el literal => offset PIC
            for nva in range(va + 2, min(va + 2 + 32 * 2, text_end - 1), 2):
                h = struct.unpack_from('<H', d, toff + nva - tbase)[0]
                if (h & 0xFF78) == 0x4478 and ((h & 7) | ((h >> 4) & 8)) == rd:
                    return True
                if h == 0x4770 or (h & 0xFF00) == 0xBD00:
                    return False  # bx lr / pop {.., pc}
            return False

        def code_at(va):
            return struct.unpack_from('<I', d, toff + va - tbase)[0]

        def after_function_end(v):
            # Stripped binary: function starts nobody reaches by bl/blx and that have no push prologue
            # (tail-call thunks `b f`, leaf callbacks) only show up as literals. A literal into __text
            # is taken as a function pointer when the word before the target ends a function
            # (b / bx / pop-ldm pc / ldr pc, or non-code) — log_022: 0x41c1d8 -> 0x41cf64 = `b 0x41c9d8`.
            # ARM only, and the target must itself be an unconditional (AL) instruction: odd values
            # (0x100005, 0x40001) and pool-data targets (new(0x107ac)) are integer constants.
            # Round values (low byte 0) are left out too: 0x26500/0x3ff000 are constants.
            if v & 3 or not (tbase + 4 <= v < text_end) or not v & 0xFF or code_at(v) >> 28 != 0xE:
                return False
            t = v
            p = code_at(t - 4)
            if p >> 28 == 0xE and ((p & 0x0F000000) == 0x0A000000 or (p & 0x0FFFFFF0) == 0x012FFF10
                                   or (p & 0x0E108000) == 0x08108000):
                return True
            if (p & 0x0FFF0000) == 0x049D0000 and (p >> 12) & 15 == 15:
                return True  # pop {pc} as ldr pc, [sp], #4
            return p >> 28 == 0xF or tbase <= p < text_end  # literal pool word right before the function

        def add_literal(lit_va, pic=False):
            # Está dentro de __text y contiene un valor-puntero reasentable
            if pic:
                pic_set.add(lit_va)
                return
            if tbase <= lit_va < text_end and lit_va not in handled:
                v = read32(lit_va)
                k = valid_target(v, unaligned=True)
                if not k and lo <= v < hi and sect_of(v & ~1) and sect_of(v & ~1)['name'] != '__text':
                    # Binario stripped: sin símbolos cerca, valid_target rechaza selrefs/classrefs, datos,
                    # LSDAs y sufijos de cadenas fusionadas. Un ldr no-PIC a otra sección es una dirección;
                    # en __text no (ahí caen constantes enteras como 0x21500).
                    k = 'any'
                if not k and after_function_end(v):
                    k = 'func-end'
                    stats[('__text', 'literal-func-end')] += 1
                if k:
                    lit_set.add(lit_va)
                    stats[('__text', 'literal')] += 1
                elif lo <= v < hi:
                    lit_rej.add(lit_va)

        # ARM (cond-agnostic): LDR inmediato offset Rn=PC, L=1
        # U=1 (positivo, 0x59): [pc, #imm]; U=0 (negativo, 0x51): [pc, #-imm]
        for va in range(tbase + (-tbase & 3), text_end, 4):
            w = struct.unpack_from('<I', d, toff + va - tbase)[0]
            b = (w >> 20) & 0xFF
            if (b in (0x59, 0x51)) and ((w >> 16) & 0xF) == 0xF:
                u = (w >> 23) & 1
                imm = w & 0xFFF
                lit_va = ((va + 8 + imm) if u else (va + 8 - imm)) & ~3
                add_literal(lit_va, arm_pc_use(va, (w >> 12) & 0xF))

        # Thumb-1: LDR literal 0x48Rd imm8
        for off in range(0, tsize - 1, 2):
            h = struct.unpack_from('<H', d, toff + off)[0]
            va = tbase + off
            if (h & 0xF800) == 0x4800:
                add_literal(((va + 4) & ~3) + (h & 0xFF) * 4, thumb_pc_use(va, (h >> 8) & 7))
            # Thumb-2 wide: 0xF8DF / 0xF85F xxxx (LDR.W Rt,[pc,#+/-imm12])
            if (h == 0xF8DF or h == 0xF85F) and off + 2 < tsize:
                h2 = struct.unpack_from('<H', d, toff + off + 2)[0]
                u = (h == 0xF8DF)
                imm = h2 & 0xFFF
                lit_va = ((va + 4) & ~3) + imm if u else ((va + 4) & ~3) - imm
                add_literal(lit_va, thumb_pc_use(va + 2, (h2 >> 12) & 0xF))

        stats[('__text', 'literal-pic-skip')] = len(pic_set)
        rebase += lit_set - pic_set
        # Literales cargados como dirección pero sin destino válido: candidatos al próximo crash (ver reporte).
        for lit in sorted(lit_rej - pic_set - lit_set):
            rejected['__text-lit'] += 1
            rejected_list.append((lit, read32(lit)))

        # Tablas de switch non-PIC (de Zombie-Infection): `ldrCC pc, [pc, Rm, lsl #2]` + `b default`
        # + direcciones absolutas de los case. Sin símbolos de función, la tabla termina donde
        # empieza el primer case o al primer valor que no es código cercano.
        for va in range(tbase + (-tbase & 3), text_end - 3, 4):
            w = struct.unpack_from('<I', d, toff + va - tbase)[0]
            if w & 0x0FFFFFF0 != 0x079FF100 or w >> 28 == 0xF:
                continue
            e, first = va + 8, text_end
            while e < first:
                v = read32(e)
                if v & 3 or not (va < v < min(text_end, va + 0x10000)):
                    break
                rebase.append(e)
                stats[('__text', 'jumptable')] += 1
                first = min(first, v)
                e += 4

    rebase.sort()
    # Eliminar duplicados si los hubiera
    rebase = sorted(list(set(rebase)))

    os.makedirs(os.path.dirname(os.path.abspath(out)) or '.', exist_ok=True)
    with open(out, 'wb') as f:
        f.write(struct.pack('<4sI', b'RBS1', len(rebase)))
        f.write(struct.pack(f'<{len(rebase)}I', *rebase))

    lines = [
        f'Binario: {src} ({detected_arch})',
        f'FairPlay cryptid: {cryptid} ({"CIFRADO" if cryptid else "descifrado"})',
        f'Rango imagen: {lo:#x}-{hi:#x}',
        f'Rebase: {len(rebase)} palabras -> {out}',
    ]
    lines += [f'  {sn:20} {k:14} {n}' for (sn, k), n in sorted(stats.items())]
    lines += [f'Rechazadas (en rango, sin símbolo válido): {dict(rejected)}']
    lines += [f'Objetos descartados (tablas mixtas): {len(dropped)}']
    for o, a, b in sorted(dropped, key=lambda x: -x[1])[:30]:
        lines.append(f'  {o} ok={a} malas={b}')
    # Literales de __text rechazados por sección destino: un crash con un registro = dirección sin
    # deslizar suele estar aquí (log_009: __bss impar).
    lit_groups = {}
    for lit, v in rejected_list:
        if text_sect and text_sect['addr'] <= lit < text_sect['addr'] + text_sect['size']:
            s = sect_of(v & ~1)
            lit_groups.setdefault((s['name'] if s else '?', 'impar' if v & 3 else 'alineado'), []).append((lit, v))
    lines += ['Literales __text rechazados (sección destino, alineación: cantidad, ejemplos literal->valor):']
    for (sn, al), hits in sorted(lit_groups.items(), key=lambda x: -len(x[1])):
        lines.append(f'  {sn:18} {al:9} {len(hits):5}  ' + ' '.join(f'{a:#x}->{v:#x}' for a, v in hits[:3]))
    lines += [f'Relocs externas: {len(extrel_slots)}; imports únicos: {len(imports)}']
    lines += [f'  {n}' for n in sorted(imports)]

    print('\n'.join(lines[:len(stats) + 5]))
    if report:
        os.makedirs(os.path.dirname(os.path.abspath(report)) or '.', exist_ok=True)
        with open(report, 'w', encoding='utf-8') as f:
            f.write('\n'.join(lines) + '\n')
        print(f"[+] Reporte completo guardado en {report}")


if __name__ == '__main__':
    main()
