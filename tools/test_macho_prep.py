#!/usr/bin/env python3
"""Regresión del rebase (tools/macho_prep.py) con casos vistos en consola y en el análisis del binario stripped.

Uso: python3 -I tools/test_macho_prep.py [Executable]   (por defecto game_app/BBD3)
"""
import os, struct, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'game_app', 'BBD3')

CASES = {
    0x35F638: True,   # corrida 5: literal absoluto a __DATA (streq en 0x35F628)
    0x4450: False,    # corrida 7: offset PIC (ldr r0,[pc,r1])
    0x37AFC: False,   # corrida 8: offset PIC usado 18 instrucciones después
    0x41BBAC: True,   # literal a __DATA (ldr r2,[r3]) en XPlayerLib::_EventInfo
    0x137E0C: True,   # tabla de switch tras ldrls pc,[pc,r3,lsl #2]
    0x137E18: True,   # último case de esa tabla
    0x137E1C: False,  # primera instrucción tras la tabla
    0x349F10: True,   # corrida 9: literal a objeto impar de __bss (0x5702d9, strb en 0x349e30)
    0x35B00C: True,   # selref cargado en 0x35afe4 -> objc_msgSend (sin símbolo cerca)
    0x1368B8: True,   # LSDA (__gcc_except_tab) guardada en el contexto SjLj
    0xB698: False,    # constante 0x21500 que cae en __text sin ser función
    0x4C9D08: True,   # entrada de vtable a función sin símbolo (prólogo push {r4-r7,lr})
    0x41C1D8: True,   # log_022: callback 0x41cf64 = `b 0x41c9d8` (thunk sin prólogo ni bl), tras otra función
    0x1368BC: True,   # dirección de reanudación SjLj (inicio de la propia función)
    0xC8D70: False,   # constante 0x107ac (tamaño para new) que apunta a datos de un pool
    0x41E38: False,   # log_023: offset PIC cargado antes de `b 0x42198`, usado en `add r1, pc, lr` tras el salto
    0x41E3C: False,   # idem (r2 -> add r3, pc, r2)
    0xCD8C8: False,   # constante impar 0x100005
}

with tempfile.TemporaryDirectory() as d:
    out = os.path.join(d, 'rebase.bin')
    subprocess.run([sys.executable, '-I', os.path.join(ROOT, 'tools', 'macho_prep.py'), BIN, out],
                   check=True, stdout=subprocess.DEVNULL)
    b = open(out, 'rb').read()
    rebase = set(struct.unpack_from(f'<{struct.unpack_from("<I", b, 4)[0]}I', b, 8))
bad = [f'{a:#x} esperado {"en" if want else "fuera de"} rebase.bin' for a, want in CASES.items() if (a in rebase) != want]
print('\n'.join(bad) or f'macho_prep ok ({len(CASES)} casos, {len(rebase)} palabras)')
sys.exit(1 if bad else 0)
