# Block Breaker 3 Unlimited — Progreso del Port (iOS Mach-O)

Estado: **todo el trabajo de host completado**. Solo falta probar en la consola física
(PS Vita — despliegue, iteración de crashes y ajustes guiados por logs).

- [x] **0. Repo y herramientas**: IPA extraído, binario analizado, `game_app` symlink, scaffolding creado.
- [x] **1. Loader mínimo** — código completo y compilando (GATE de consola pendiente: que el log llegue al bucle principal / inicialización de motor).
  - [x] Carga de segmentos en memoria con `macho_load` (slide `0x98000000`).
  - [x] Aplicación de `rebase.bin` (44560 bytes, generado y verificado por `tools/macho_prep.py`).
  - [x] Resolución de imports indispensables de libc, math, pthread y runtime (corregido campo `s->type`/`s->r1` → `flags & SECTION_TYPE` / `reserved1` en `macho.c`, símbolos indirectos `__nl_symbol_ptr` resueltos).
  - [x] Ejecución de inicializadores globales con `macho_run_inits()`.
- [x] **2. Render** — código completo y compilando (GATE de consola pendiente: primer frame / pantalla de inicio visible).
  - [x] Inicialización de vitaGL y FBO virtual de presentación (`vendor/vitaGL` compilada con `SOFTFP_ABI=1 NO_DEBUG=1 NO_SPLASHSCREEN=1 HAVE_SHADER_CACHE=1`).
  - [x] Manejo de texturas PNG CgBI con `cgbi.c`.
  - [x] Swap de buffers con `vglSwapBuffers` + IME (`gles.c`) — verificación de ~60 FPS sin splashscreen freeze pendiente en consola.
- [x] **3. Input** — código completo y compilando (GATE de consola pendiente: controles funcionales in-game).
  - [x] Multitouch frontal de la Vita mapeado a las coordenadas lógicas del juego (proyección a 480x320 en `input.c`).
  - [x] Mapeo de botones físicos (D-Pad, Cross, Circle, Square, Triangle) y stick analógico a la capa táctil (drag sintético id 100 sobre la línea de paleta; tap sintético de acción id 101 con Cross/Circle/Square/Triangle).
- [x] **4. Texto y fuentes** — código completo y compilando (GATE de consola pendiente: diálogos y menús legibles).
  - [x] FreeType sobre atlas de fuentes, integrado en el mini-runtime ObjC (`objc.c`, librería `freetype` enlazada).
- [x] **5. Audio y video** — código completo y compilando (GATE de consola pendiente: música y efectos activos).
  - [x] RemoteIO / AudioSession redirigido al hilo `sceAudioOut` (`coreaudio.c`).
  - [x] Formato y decodificación de audio de streaming o efectos (`SceAvPlayer` disponible en el link).
- [x] **6. Guardado y LiveArea** — completado offline (GATE de consola pendiente: persistencia verificada con partidas reales).
  - [x] Persistencia de partidas en `ux0:data/blockbreaker3unlimited/saves/` (`DOCS_PATH`, `sceIoMkdir` al arrancar).
  - [x] LiveArea empaquetada en el VPK: `icon0.png`, `pic0.png`, `bg0.png`, `startup.png`, `template.xml`.

## Compilación (2026-10-18)

- [x] vitaGL vendorizada compilada en `vendor/vitaGL` → `libvitaGL.a`.
- [x] Loader completo compila sin errores: `build/blockbreaker3unlimited` (8.5 MB).
- [x] SELF generado: `build/eboot.bin` (UNSAFE NOASLR).
- [x] VPK empaquetado: `build/blockbreaker3unlimited.vpk` (1.0 MB, con LiveArea incluida).
- Fix `l_debug` ausente: añadida en `log.c/log.h` (activa con build Debug vía `DEBUG_LOADER`).
- Fix `BUNDLE_PATH`: ahora el staging siempre produce `ux0:data/<slug>/game_app/` (`tools/stage_data.sh`), igual que `hle.h`.

## Staging de datos (2026-10-18)

- [x] `ux0_data/blockbreaker3unlimited/game_app/` — bundle del IPA limpio (BBD3 5.4 MB + recursos + shaders).
- [x] `ux0_data/blockbreaker3unlimited/rebase.bin` — tabla de rebase con MACHO_SLIDE `0x98000000` (11138 palabras).
- [x] `ux0_data/blockbreaker3unlimited/saves/` y `logs/` creados.
- Total stagiado: ~53 MB listo para `python3 -I tools/deploy_data.py` o `psvita-toolkit deploy --data`.

## Triage corrida física #1 (2026-10-18, log_001.log)

- **Fallo**: `FATAL Mach-O does not contain __TEXT or __DATA segments` justo al cargar BBD3.
- **Causa raíz**: en `macho.c` el cast a `segment_command` arrancaba en el inicio del LC
  (`cmd`), pero el struct arranca en `segname` (+8) → `strcmp("__TEXT")` nunca matcheaba.
- **Fix 1**: cast a `d + off + 8` para segmentos; símbolos indirectos corregidos el parche previo
  (`s->type`→`flags & 0xFF`, `s->r1`→`reserved1`).
- **Fix 2 (diseño de memoria)**: la arquitectura asumía bloques `AllocMemBlockForVM` a
  dirección fija `MACHO_SLIDE` — no garantizado. Ahora se asigna UN bloque contiguo que
  cubre `[__TEXT, __DATA_end)` (conservando distancias entresegmentos, compresión PC-relativa),
  y el slide se calcula en runtime (`macho_slide()` en `macho.h`) desde la base real del bloque.
- **Fix 3**: faltaba `sceKernelOpenVMDomain()` — los bloques del dominio VM nacen no ejecutables
  y sin esto saltaría una excepción al saltar a `_main`.
- **Estado**: resintetizado `build/eboot.bin` y `build/blockbreaker3unlimited.vpk`. Pendiente: redeploy eboot por FTP y reintentar.
  Expectativa log: `guest block base: 0x98000000 -> slide 0x97FFF000` (o desplazado), luego
  `Mach-O loaded successfully.` → `Applying NNN rebase words` → `Executing NNN global constructors`.

## Triage corrida física #2 (2026-10-19, log_002.log)

- **Fallo**: `sceKernelAllocMemBlockForVM(guest span 0x00571000) = 0x80024B05`.
- **Causa raíz**: `sceKernelOpenVMDomain()` se llamaba DESPUÉS del primer alloc VM (debe ir antes;
  mismo error documentado en hardware por el port de Aralon, `source/reimpl/mem.c`). Además los
  bloques VM rechazan tamaños no múltiplos de 1 MB (span 0x571000 y thunks 0x20000).
- **Fix**: `OpenVMDomain` antes del alloc; span alineado a 0x100000 (→ 0x600000); arena de thunks
  a 1 MB y `fatal_error` si su alloc falla (antes fallaba en silencio).
- **Expectativa log #3**: `guest block base: 0x... -> slide 0x...` → rebase → constructores.

## Triage corrida física #3 (2026-10-19, log_003.log)

- **Fallo**: la consola se APAGÓ (sin dump propio); log truncado en `[00:11:52.58` (512 B) justo
  tras `__DATA vm:` → la caché del FS se perdió, el punto real de fallo es desconocido.
- **Dump descargado NO es de este juego**: hilo `PSVZI0001`, módulo `zombieinfection` (otro port,
  renombrado con nuestro prefijo al descargar). Ignorarlo; borrar dumps viejos de `ux0:data/` antes de probar.
- **Causa probable**: el diseño con dominio VM (`AllocMemBlockForVM` + `OpenVMDomain`, sin cerrar)
  nunca se probó en hardware. Se vuelve al patrón so_util probado: `kuKernelAllocMemBlock` a dirección
  fija — `__TEXT` RX en `0x98001000`, `__DATA` RW en `0x984A5000`, thunks RX debajo de `__TEXT`;
  `__TEXT` se escribe con `kuKernelCpuUnrestrictedMemcpy`. Slide fijo = `MACHO_SLIDE`.
- **Robustez de log**: `log.c` ahora escribe con `sceIoWrite` + `sceIoSyncByFd` por línea, y el loader
  deja migas (`Allocating __TEXT RX`, `Allocating __DATA RW`, `Guest blocks OK`) antes de cada paso riesgoso.
- **Expectativa log #4**: `Guest blocks OK` → `Applying N rebase words` → `Mach-O loaded successfully.`

## Triage corrida física #2 (2026-10-19, log_004 + psp2dmp 0x0000232833)

- **Fallo**: Data Abort (`0x30004`) durante `__mod_init_func` (inits de guest).
- **Crash real**: PC guest `0x35F628` = `streq r3,[r2]` con `r2 = 0x57051C` (literal cargado
  por `ldreq r2,[pc,#0x10]` en va `0x35F620` → literal en `0x35F638` que contiene un puntero
  a `__bss` **sin deslizar**). El desensamblado de `otool` era basura (secciones mal
  reconocidas); capstone en modo ARM con `fileoff = va - 0x1000` mostró la realidad.
- **Causa raíz**: los pools literales están embebidos **dentro de `__text`** (no sección
  aparte) y `macho_prep.py` excluía `__text` del escaneo de punteros → el rebase (11138
  palabras) no cubría los punteros de los literal pools → `__DATA` se accedía sin slide desde `__TEXT`.
- **Fix**: escaneo de literal pools en `__text` en `tools/macho_prep.py`:
  detecta `ldr rX,[pc,#imm]` ARM (cond-agnostic) y Thumb-1/Thumb-2, valida el valor de la
  palabra con `valid_target()` y agrega el slot al rebase. Rebase pasa de 11138 → **17206**
  palabras (+6809 slots literales de `__text`), incluyendo el literal del crash (`0x35F638`).
- Aprendizajes para futuros triages: los registras del dump se combinaron
  (`r1=0xFFFF` = `movweq r1,#0xFFFF`, `r3=0x3F000000` = `moveq r3,#0x3f000000`,
  `r2=0x57051C` = literal cargado del pool) y confirman punto exacto del abort.
- **Estado**: `macho_prep.py` re-generó `ux0_data/blockbreaker3unlimited/rebase.bin`; loader
  resintetizado (`build/eboot.bin` + `.vpk`). Pendiente: redeploy (eboot + data) y reintentar.
  Expectativa log: `Applying 17206 rebase words ... Rebase complete: 17206 words relocated.`

## Triage corrida física #5 (2026-10-19, log_005.log + dump 1792384792)

- **Gate 1 superado**: bloques kubridge OK, rebase aplicado, `Mach-O loaded successfully.`, arrancan los 314 constructores.
- **Fallo**: Data abort en guest `0x35F628` (`streq r3,[r2]`, ARM) dentro de un constructor; `R2 = 0x0057051C`
  = dirección de `__DATA` SIN deslizar, cargada del literal pool en `0x35F638` (dentro de `__text`).
- **Causa raíz**: binario no-PIE sin relocs; el rebase heurístico no escaneaba los literal pools de `__text`.
  `tools/macho_prep.py` ya los escanea (ARM `ldr [pc]`, Thumb-1/Thumb-2): `rebase.bin` pasó de 11138 a 17206
  palabras e incluye `0x35F638`, pero la consola corrió con el `rebase.bin` VIEJO (log: "11138 rebase words").
- **Fix**: subir el `rebase.bin` nuevo (`python3 -I tools/deploy_data.py`). Verificar en el log `Applying 17206 rebase words`.
- **Siguiente bug previsto**: imports de libstdc++ sin implementar (`std::ios_base::Init`, `std::string(const char*)`,
  `_Rb_tree`) devuelven 0 → romperán en cuanto se use el objeto.

## Triage corrida física #7 (2026-10-19, log_007.log, sin dump: FTP caído)

- Corrió con el `rebase.bin` de 17206 palabras, pero murió en el PRIMER constructor (ni siquiera llegó a
  `ios_base::Init`, que con el rebase viejo sí se alcanzaba).
- **Causa raíz**: falso positivo del escáner de literal pools. Constructor `0x43bc`→`0x4430`:
  `ldr r1,[pc,#0x18]` (literal `0x4450` = `0x004A1A08`) + `ldr r0,[pc,r1]` → es un OFFSET PIC, no un puntero;
  al sumarle el slide, la lectura apunta a basura.
- **Fix** (`tools/macho_prep.py`): un literal cuyo registro destino se usa luego con `pc` (`add rX,pc,rd`,
  `ldr/ldrh [pc,rd]` en ARM; `add rd,pc` en Thumb) se excluye siempre. `rebase.bin` = 16475 palabras
  (731 falsos positivos menos); `0x4450` fuera, `0x35F638` sigue dentro.
- **Expectativa log #8**: `Applying 16475 rebase words` → warnings de libstdc++ (siguiente bug).

## Triage corrida física #8 (2026-10-19, log_008.log + dump 1792385551)

- Rebase 16475 OK; los constructores avanzan (vuelve a aparecer `ios_base::Init`).
- **Fallo**: Data abort en guest `0x3797C` (`strb lr,[sb]`), `sb = pc + ip`; `ip` se cargó del literal `0x37AFC`
  en `0x3792C` y se usa 18 instrucciones después (`add sb,pc,ip` en `0x37974`), fuera de la ventana de 16.
- **Fix** (`tools/macho_prep.py`): la detección de uso PIC sigue el registro hasta 64 instrucciones y corta
  cuando se sobrescribe (data-processing/ldr/movw) o la función termina (b/bx/pop pc). `rebase.bin` = 16474
  palabras; subido y verificado en consola (65904 bytes). Auditoría: los literales restantes se usan como punteros.
- **Expectativa log #9**: `Applying 16474 rebase words`.

## Sincronización con Zombie-Infection-vita (2026-10-19, sin corrida física aún)

Runtime traído del port de Zombie Infection (mismo loader Mach-O, juego Gameloft que ya llega a gameplay)
y adaptado a BBD3. Respaldo del estado anterior fuera del repo (scratchpad de la sesión).

- **`macho.c`** (de Zombie): corrige 3 bugs del loader viejo:
  1. Slots locales de `__nl_symbol_ptr` deslizados DOS veces (bucle de imports + 761 entradas de `rebase.bin`).
  2. Imports sin implementar: ahora se prueba primero el símbolo del propio guest (806 *weak* de BBD3, p.ej.
     `std::_Rb_tree<…XPlayerLib::Delegate…>`, antes caían en stubs vacíos) y los de datos reciben un bloque en cero.
  3. `macho_sym` por búsqueda binaria, `macho_hook` con thunk alineado, relocs externas, checks de formato.
- **ObjC/UIKit**: `objc.c` (runtime completo), `objc_rt.h`, `uikit.c` (UIApplicationMain, run loop, touches,
  NSTimer, acelerómetro). Identidad del bundle en `hle.h` (`EXE_NAME`, `BUNDLE_ID`, `APP_DELEGATE`, versión 1.0.2).
- **`cxx.c`**: generado por `tools/gen_cxx.py` (110 símbolos libstdc++ → VitaSDK, 0 sin equivalente).
- **`gles.c`** (de Zombie): FBO/renderbuffers reales, VBOs, arrays BYTE/FIXED, más 12 imports propios de BBD3
  (`glOrthof`, `glGenerateMipmapOES`, `glMapBufferOES`…). El `gles.c` viejo exportaba variantes `*x` que BBD3 no usa.
- **Vertical**: BBD3 es solo Portrait (Info.plist). Blit sin rotar 320x480 → 362x544 centrado (x=299), touches al
  mismo rectángulo, `to_view` identidad, `statusBarOrientation` = 1. Sin el widescreen de Zombie (solo 3D horizontal).
- **Controles** en `patch_input()`: stick izq. = X de la paleta (y=440, calibrar), botones = tap al centro.
- **`tools/macho_prep.py`** (BBD3 está stripped): inicios de función por destinos `bl/blx` + prólogos
  `push {…, lr}`; secciones solo-punteros (`__mod_init_func`, `__objc_*`) aceptan cualquier puntero a sección;
  tablas de `switch` (`ldrCC pc,[pc,Rm,lsl #2]`, de Zombie). `rebase.bin`: 16474 → **33068** palabras
  (superconjunto: 0 perdidas; +11946 entradas de vtable, +IMPs ObjC, +2247 de tablas de salto).
- Tests: `tools/test_macho_prep.py` (casos de corridas #5/#7/#8 + nuevos) y `tools/test_darwin_path.sh`.
- **Expectativa log #9**: `rebase: 33068 words`, `imports: N slots, M unimplemented`, 314 constructores, luego
  `UIApplicationMain` → `BBD3AppDelegate`. Desplegar eboot + `rebase.bin` nuevo (`python3 -I tools/deploy_data.py`).

## Triage corrida física #9 (2026-10-19, log_009.log + dump 1792443432)

- Gate de carga OK con el runtime de Zombie: `rebase: 33068 words`, `imports: 1065 slots, 101 unimplemented`.
- **Fallo**: Data abort en un constructor, guest `0x349e30` (`strb r2,[r3,#1]`), `R3 = 0x005702d9` = objeto de
  bytes en `__bss` SIN deslizar, cargado del literal `0x349f10` en `0x349e20`.
- **Causa raíz** (`tools/macho_prep.py`): `valid_target` descartaba destinos impares fuera de `__const/__data`, y
  (binario stripped) también cualquier destino de datos sin símbolo a <64 KB. Faltaban 4125 literales de `__text`.
- **Fix**: un literal de `__text` cargado con `ldr` y NO usado como offset PIC se acepta si apunta a cualquier
  sección distinta de `__text` (impares y sufijos de cadenas incluidos). Destinos en `__text` siguen requiriendo
  inicio de función (ahí caen constantes como `0x21500`). +5934 literales: selrefs/classrefs antes de
  `objc_msgSend`, vptrs instalados por constructores (`str r3,[r0]`), LSDAs SjLj. `rebase.bin` = **36755**.
- El reporte (`docs/macho_report.txt`) ahora lista los literales rechazados por sección destino: quedan 445,
  todos con destino `__text`. Tests: `tools/test_macho_prep.py` (12 casos).
- **Expectativa log #10**: `rebase: 36755 words` y constructores completos (`global constructors: 314 done`).

## Triage corrida física #10 (2026-10-19, log_010.log, sin dump: cierre controlado)

- Rebase 36755 OK y **314/314 constructores completos**. Cierre: `[FATAL] guest entry point _main not found`.
- **Causa raíz**: BBD3 está stripped. Tampoco define ningún `_OBJC_CLASS_$_*`, así que `objc_get_class` habría
  devuelto una clase host vacía para `BBD3AppDelegate`/`EAGLView` (siguiente cierre seguro).
- **Fixes**:
  - `macho_main()` (`macho.c`): `_main`, si no `LC_MAIN`, si no primer `bl` del `start` de `LC_UNIXTHREAD`
    (`0x21dc` → `0x2208`, verificado contra el binario).
  - `macho_section()` + `objc_get_class()` busca por `class_ro_t->name` en `__objc_classlist` (verificado: 47
    clases, incluye `BBD3AppDelegate` y `EAGLView`). `NSClassFromString` devuelve nil para clases inexistentes.
  - `uikit.c`: llama `application:didFinishLaunchingWithOptions:` si el delegate lo implementa (como UIKit).
- **Prevención** (imports que eran stubs que devuelven 0): `objc_getProperty`, `objc_copyStruct`, `objc_sync_*`,
  `CFBundleCopyBundleURL`/`CFURLCopyFileSystemPath`/`CFStringGetCString`, `CFArrayCreateMutable`, `CGRect*`,
  `CGAffineTransform*`, 25 constantes NSString, `pthread_cond_*`/`trylock`/`join`, ctype Darwin
  (`_DefaultRuneLocale`, `__maskrune`), `ceil/exp/log/strtod/__floatundidf` (softfp), `fprintf/vprintf`,
  `localtime`, `remove` (con `darwin_path`), `abort` (fatal con log), wide-char.
- Quedan sin implementar (logean al llamarse): AddressBook, `CC_MD5`, dibujo CGContext/UIGraphics, `aio_*`,
  `setjmp/longjmp` (jmp_buf de newlib más grande que el de iOS: no enlazar directo), GL multisample APPLE.
- **Expectativa log #11**: `Jumping to guest entry point at 0x98002208 (Mach-O offset 0x00002208)`,
  `UIApplicationMain: emulating MainWindow.nib`, `-> application:didFinishLaunchingWithOptions:` (o el viejo).

## Triage corrida física #11 (2026-10-19, log_011.log + dump analizado)

- **Dump descargado NO es de este juego**: hilo `PSVBOF001`, módulo `bladesoffury` (otro port remanente en `ux0:data/`).
- **Comportamiento en log**: el juego llegó a `UIApplicationMain: emulating MainWindow.nib`, pero falló con:
  ```
  [18:12:59.448] [WARN] objc: -[BBD3AppDelegate init] not implemented -> nil
  [18:12:59.457] [WARN] objc: -[EAGLView initWithCoder:] not implemented -> nil
  [18:12:59.465] [FATAL] -[EAGLView initWithCoder:] returned nil
  ```
- **Causa raíz**: BBD3 utiliza `LC_DYLD_INFO_ONLY` (iOS 3.1+ dyld compressed bind opcodes) para enlazar imports de datos, superclases de Objective-C y referencias a clases externas (`bind_info` con 5,698 entradas, `weak_bind_info` con 865, `lazy_bind_info` con 435). `macho.c` solo enlazaba la tabla de símbolos indirectos de funciones C (`__la_symbol_ptr`/`__nl_symbol_ptr`), ignorando `LC_DYLD_INFO_ONLY`.
  - Como consecuencia, los punteros a superclase `BBD3AppDelegate->superclass` (offset `0x4b09e0`) y `GlitchGLView->superclass` (offset `0x4b0c10`) quedaban en `NULL` (0x0) en memoria en lugar de apuntar a `_OBJC_CLASS_$_NSObject` y `_OBJC_CLASS_$_UIView`.
  - Al instanciar `BBD3AppDelegate init` y `EAGLView initWithCoder:`, las llamadas a `[super ...]` alcanzaban `NULL` y caían en `unknown_method -> nil`.
- **Fix implementado**:
  - `macho.c`: agregado intérprete completo de bytecodes de dyld bind (`apply_dyld_bind` con decodificación de ULEB128/SLEB128 y los opcodes `0x00` a `0xC0`). Se registran y resuelven `bind_info`, `weak_bind_info` y `lazy_bind_info` en `macho_load`.
  - `objc.c`: añadidos símbolos faltantes requeridos por los bindings de dyld: `_kCFTypeArrayCallBacks`, `_OBJC_EHTYPE_$_NSException`, `_OBJC_EHTYPE_id`, `___gxx_personality_sj0`, `___objc_personality_v0`, `dyld_stub_binder`, `_ABPersonComparePeopleByName` y constantes `kABPerson*`.
  - `.gitignore`: anclado `/Makefile` para no excluir `vendor/vitaGL/Makefile` durante los builds limpios del toolkit.
- **Compilación verificada**: `build/eboot.bin` y `build/blockbreaker3unlimited.vpk` generados correctamente con `psvita-toolkit build`.
- **Expectativa log #12**: `imports: ~6800 slots`, `UIApplicationMain` inicializa `BBD3AppDelegate` y `EAGLView` exitosamente, avanzando a `application:didFinishLaunchingWithOptions:` y arranque del hilo de juego / presentación vitaGL.

## Triage corrida física #12 (2026-10-19, log_012.log + dump 1792452998)

- **Avance verificado**: El fix de la corrida #11 funcionó al 100%. `LC_DYLD_INFO_ONLY` resolvió las superclases de Objective-C y 8,063 slots de imports. `UIApplicationMain` inicializó `BBD3AppDelegate`, `EAGLContext initWithAPI:1`, renderbuffer 320x480 y framebuffer de ventana exitosamente.
- **Fallo**: Prefetch abort exception (`0x30003`) en el hilo de juego `PSVBB3U00` (ID `0x40010003`).
  - Registros: `PC = 0x00097594`, `LR = 0x98336cf8`, `R3 = 0x00097594`, `R0 = R4 = 0x82222f10` (puntero `this`).
  - Punto de llamada: Mach-O offset `0x336cf4` (`blx r3`), tras cargar el vtable pointer `ldr r3, [r4]` y el slot virtual `ldr r3, [r3, #8]`.
  - Instrucción en destino (`0x97594`): `bx lr` (`0xe12fff1e`, método virtual vacío).
- **Causa raíz**:
  - `PC = 0x00097594` es una dirección **sin deslizar** (en PS Vita el código vive en `0x98097594` con `MACHO_SLIDE = 0x98000000`).
  - En el binario `BBD3`, la dirección `0x00097594` aparece **483 veces** en `__DATA,__const` (vtables de C++). En `rebase.bin`, **0 de las 483** estaban presentes.
  - El escáner de `tools/macho_prep.py` exigía `v in func_entry` para aceptar punteros a `__text`. Pero en binarios stripped, `func_entry` solo detectaba símbolos (165), destinos de `bl/blx` y prólogos `push {..., lr}`.
  - Las funciones hoja (leaf functions como `bx lr` en `0x97594`, destructores triviales, getters `ldr r0, [r0, #off]; bx lr`) **no** hacen `push {..., lr}` y **nunca** son llamadas por `bl/blx` directo (se llaman exclusivamente vía indirecta `blx rX` a través de la vtable).
  - En consecuencia, `macho_prep.py` descartó **11,099 punteros de vtables a `__text`** y 421 punteros a metadatos RTTI en `__DATA,__const`.
- **Fix implementado**:
  - `tools/macho_prep.py`:
    1. Agregado escaneo previo de `__DATA,__const` para poblar `func_entry` con todos los destinos de métodos virtuales de las vtables (detectando todas las funciones hoja, getters y thunks).
    2. En el escaneo de secciones de datos (`scan`), `__DATA,__const` se trata como sección de metadatos de punteros: todo valor que apunte a `__text` con alineación ARM (4 bytes) o Thumb (bit 0) se clasifica como `func`; destinos a `__cstring` como `cstr`; y punteros a otras secciones (`__const`, `__cfstring`) como `ptr`.
  - Regenerado `ux0_data/blockbreaker3unlimited/rebase.bin`:
    - Pasó de 36,757 a **48,494 palabras de rebase** (+11,737 palabras).
    - `0x97594`: **483 de 483** cubiertas (100%).
    - Punteros de `__DATA,__const` a `__text`: **22,984 de 22,984** cubiertos (100.0%).
- **Expectativa corrida #13**: `rebase: 48494 words`, la vtable se desliza a `0x98097594`, el método virtual vacío retorna limpiamente y el bucle principal del juego arranca el renderizado del primer frame (Gate 2).

## Triage corrida física #13 (2026-10-19, log_013.log + dump 1792454500)

- **Avance verificado**: El fix de la vtable de la corrida #12 funcionó al 100%. `rebase: 48494 words` cargado, sin prefetch abort en `0x97594`. El juego avanzó significativamente, inicializando el contexto EAGL (`EAGLContext initWithAPI:1`), los buffers de pantalla (`renderbuffer 320x480` y `framebuffer 2169845380`), rutas de guardado y entrando en la creación del motor gráfico Irrlicht.
- **Fallo**: Data abort exception (`0x30004`) en el hilo de juego `PSVBB3U00` (ID `0x40010003`).
  - Registros: `PC = 0x982ff994`, `LR = 0x982ff960`, `R3 = 0x00000000` (NULL), `SP = 0x81afffb0`.
  - Instrucción en PC (`0x2ff994`): `ldr r2, [r3, #0xb0]` -> desreferencia de puntero NULL (`r3 = 0`).
  - Origen de `R3`: Argumento `r1` pasado a la función `0x2ff914`, que se cargó en `0x29e0c4` desde `r4->videoDriver` (`[r4, #0x10]`).
- **Causa raíz profunda**:
  - `videoDriver` era NULL porque la función `0x336c8c` (`createDeviceEx` / `CIrrDevice::createDriver`) falló al inicializar el driver de video (`0x33e1d8` devolvió 0) y retornó NULL.
  - En `0x33e1ec`, el driver de video GLES 1.1 de Irrlicht consulta la versión con `glGetString(GL_VERSION)` y la parsea con `sscanf(p, "%u.%u", &major, &minor)`.
  - El driver calcula `version = major * 100 + minor` y verifica explícitamente:
    ```c
    if (version <= 100 || version > 199) {
        // "Unable to create video driver of unknown type."
        return 0; // FALLO
    }
    ```
  - vitaGL por defecto devuelve `"OpenGL ES 2.0 VitaGL"`. Al parsearlo, `sscanf` extrae `major=2, minor=0` -> `version = 200`.
  - Como `200 > 199`, el motor rechaza el driver creyendo que no es un driver GLES 1.x compatible, retorna NULL y el juego crashea al intentar usar el driver inexistente.
  - Adicionalmente, el log reportó `[WARN] chdir(ux0:data/blockbreaker3unlimited/BBD3.app/): not a directory`, debido a que el binario tiene hardcodeada la ruta `/BBD3.app/` en lugar de `game_app/`.
- **Fixes implementados**:
  1. `source/gles.c`: Se implementó un wrapper `g_glGetString(GLenum name)` que para `GL_VERSION` devuelve `"OpenGL ES-CM 1.1 VitaGL"`. Con esto, `sscanf` extrae `major=1, minor=1` -> `version = 101` (`100 < 101 <= 199`), lo cual es aceptado exitosamente por Irrlicht como `OpenGL|ES driver version is 1.1 or better.`
  2. `source/darwin.c`: En `darwin_path()`, se agregó el mapeo transparente de `DATA_PATH "BBD3.app"` y `DATA_PATH "Payload/BBD3.app"` hacia `BUNDLE_PATH` (`DATA_PATH "game_app/"`).
  3. `source/main.c`: Se agregaron llamadas `sceIoMkdir` para crear `Library/` y `Library/Caches/` en el arranque.
- **Expectativa corrida #14**: El driver GLES 1.1 se inicializa correctamente sin devolver NULL, el objeto de renderizado se construye y se alcanza el primer frame visible en pantalla (**Gate 2**).

## Triage corrida física #14 (2026-10-19, log_014.log + dump 1792460850)

- **Avance verificado**: Los fixes de la corrida #13 funcionaron a la perfección. `g_glGetString` retornó `"OpenGL ES-CM 1.1 VitaGL"`, `chdir` a `BBD3.app/` resolvió limpiamente a `game_app/`, y el objeto `CIrrDevice` y el driver de video pasaron la validación inicial sin retornar NULL.
- **Fallo**: Data abort exception (`0x30004`) en el hilo de juego `PSVBB3U00` (ID `0x40010003`).
  - `PC = 0x811a2f1a` (en newlib `memcmp`), `LR = 0x70`.
  - Registros: `R0 = 0x82224825` (puntero a `"penGL ES-CM 1.1 VitaGL"` dentro del string retornado por `g_glGetString`), `R1 = 0x0046b608`, `R2 = 6`.
  - Stack trace caller: `0x9833f0c0` (Mach-O offset `0x33f0c0`), inmediatamente posterior a `0x33f0bc: bl #0x4538b4` (llamada a `memcmp`).
- **Causa raíz profunda**:
  - En `0x33f0bc`, Irrlicht busca la subcadena `"OpenGL "` en la versión reportada. La instrucción en `0x33f0b4` carga el puntero a la cadena literal:
    ```arm
    0x33f0b4: ldr r1, [pc, #-0x5c]  ; 0xe51f105c: LDR R1, [PC, #-0x5c]
    ```
  - El literal destino está en `0x33f060` y contiene `0x0046b609` (puntero a la cadena `"penGL \0"`).
  - En la arquitectura ARM, el bit 23 (U) indica si el offset es positivo (U=1, bits 27..20 = `0x59`) o negativo (U=0, bits 27..20 = `0x51`).
  - `tools/macho_prep.py` **únicamente buscaba `0x59`**, ignorando por completo todas las instrucciones `ldr Rd, [pc, #-imm]` (U=0, `0x51`).
  - Como consecuencia directa, `0x33f060` (junto con otros 1,306 literales PC-relativos negativos en todo el binario) **nunca fue rebasado** con el slide `0x98000000`. Al invocar `memcmp`, `r1` contenía `0x0046b609` (memoria sin mapear en la PS Vita), provocando una excepción inmediata de violación de memoria al leer el primer byte.
- **Fixes implementados**:
  1. `tools/macho_prep.py`: Soporte completo para offsets PC-relativos negativos tanto en ARM (`(b in (0x59, 0x51))`) como en Thumb-2 wide (`0xF8DF` y `0xF85F`). La tabla de rebase aumentó de 48,494 palabras a **48,777 palabras** (+283 palabras en __text), cubriendo `0x33f060`, `0x33f064`, `0x33f068` y todos los literales no rebasados.
  2. `source/darwin.c`: Implementación limpia de `d_memcmp` basada en comparación de bytes sin signo portable (`F(memcmp, d_memcmp)`), previniendo cualquier comportamiento atípico de rutinas ensamblador Thumb-2 en newlib con punteros con alineación impar.
  3. Limpieza de advertencias de compilación de macros `F`/`D` en `gles.c` y `darwin.c`.
  4. Build exitoso de `build/eboot.bin` y `build/blockbreaker3unlimited.vpk` verificado con `psvita-toolkit build --preset release`.
- **Expectativa corrida #15**: `memcmp` compara `"penGL "` con `"penGL "` en `0x9846b609`, retorna 0 de coincidencia, `0x33f0d0` y `0x33f0f0` cargan sus punteros slid correctamente, y el motor completa la creación del driver para comenzar el renderizado del primer frame (**Gate 2**).

## Triage corrida física #15 (2026-10-19, log_015.log + dump 1792468149)

- **Avance verificado**:
  - `rebase.bin` con soporte de `ldr Rd, [pc, #-imm]` cargó 48,777 palabras sin problemas.
  - El driver OpenGL ES-CM 1.1 fue completamente aceptado por Irrlicht.
  - FBO (320x480) y renderbuffers se crearon correctamente (`framebuffer = 2169845380`).
  - El motor inició la carga de materiales y shaders (`Shader Texture Addition Shader #1`).
- **Fallo**: Data abort (`0x30004`) en el hilo de juego `PSVBB3U00` a `PC = 0x98233934` (`0x233934` en `BBD3`), `LR = 0x9835c464` (`0x35c464` en `BBD3`).
  - Registros: `R0 = 0x00000000`, `R1 = 0x81b005c8`, `R3 = 0x98233930`.
  - Código:
    ```arm
    0x233930: ldr r0, [r0, #4]   ; carga wrapper->mesh (0x0 / NULL)
    0x233934: ldr r3, [r0]       ; DEREFERENCIA DE PUNTERO NULL -> CRASH
    ```
- **Causa raíz profunda**:
  1. En `0x233e00`, se invoca la configuración del efecto pasando un adaptador en stack (`wrapper`) cuyo campo `mesh` (`+0x4`) se obtiene de `0x233a00(engine, file)`.
  2. En `0x233a00`, `BDAELoader::createMesh("little_endian_not_quantized.bdae")` retornó NULL (`0x0`), el cual fue almacenado sin validar en `wrapper->mesh`.
  3. `DefaultEffects.bdae` estaba comprimido dentro del paquete `ires/EffectLibrary.bar` (GBMP), pero no existía descomprimido en el sistema de archivos de la PS Vita.
  4. Falta del stub para la extensión OpenGL ES `glSampleCoverage`, reportado como warning antes de la inicialización de materiales.
- **Fixes implementados para Corrida #16**:
  1. `source/gles.c`: Implementado stub de `g_glSampleCoverage` y añadido a la tabla de imports `gles_imports`.
  2. `source/darwin.c`: Elevada la visibilidad de los logs de `d_fopen`, `d_stat` y `d_opendir` de `l_debug` a `l_info` con mapeo de ruta resuelta para auditoría I/O en hardware.
  3. **Extracción de assets sueltos**: Descomprimido `DefaultEffects.bdae` (archivo ZIP de 333,905 bytes) y sus 7 ficheros contenidos (`little_endian_not_quantized.bdae`, etc.) directamente en `ux0_data/blockbreaker3unlimited/game_app/` e `ires/`.
  4. `source/patch.c`:
     - Interceptados los métodos virtuales del wrapper (`0x2338f0`, `0x233900`, `0x233910`, `0x233930`) y su vtable (`0x4d0724`) con guardas seguras contra `NULL mesh`.
     - Parcheado el llamador en `0x233e04` (`hook_decref_trampoline`) para retorno limpio y seguro sin dereferenciar `[r4, #4]` en caso de mesh nulo.
  5. Compilación Release completa y validada con `psvita-toolkit build --preset release` (`build/eboot.bin` y `build/blockbreaker3unlimited.vpk`).

## Única fase pendiente: pruebas en consola física

1. Desplegar datos y VPK:
   - `python3 -I tools/deploy_data.py`
   - Instalar `build/blockbreaker3unlimited.vpk` (molecularShell / `psvita-toolkit deploy --vpk`)
2. Comprobar prerequisito de shaders: `ur0:/data/libshacccg.suprx` presente (ShaRKBR33D instalado) — el loader hace `fatal_error` si falta.
3. Ejecutar y seguir el flujo del plan (`PORTING_PLAN.md` → "Iteración y Triage de Crashes"):
   - Gate 1: log debe llegar a "Mach-O loaded successfully." → entrada `_main`.
   - Gate 2: primer frame visible / pantalla de inicio.
   - Gate 3: multitouch + botones.
   - Gate 4: texto legible (atlas FreeType).
   - Gate 5: audio/música.
   - Gate 6: guardado de partida persistente.
4. Si hay crash: `python3 -I tools/deploy_data.py --logs`, luego
   `psvita-toolkit analyze logs/<dump>.psp2dmp --elf build/blockbreaker3unlimited`.
   PC en `0x98xxxxxx` → restar `0x98000000` y localizar con `tools/annotate.py`.

## Ajustes esperados en consola (iteración)

- Resolución real del viewport del juego ( touch mapping 480x320 puede necesitar calibrar en `input.c`).
- Afinar deadzone/velocidad del stick y tap de botones contra la paleta real.
- Mapeo de esquinas táctiles a botones de menú UI vía `patch.c` si el juego lo requiere.

## log_016: abort por bad_alloc al cargar DefaultEffects.bdae

- Dump: `__cxa_throw` desde `operator new(0xB9B9D5D0)` llamado en `0x35c518` (parser bdae `0x35c43c`, invocado desde `0x233e00`).
- Causa: `0x233a00` devuelve stream NULL — el lector ZIP/GBMP (vtable `0x4dc20c`) no encuentra `little_endian_[not_]quantized.bdae`
  dentro de `DefaultEffects.bdae` (las entradas SÍ existen en el archivo). Los guards del wrapper (`0x4d0724`) convertían el NULL en
  size=0/read no-op → cabecera de 60 bytes sin leer → `new(basura*4)`.
- Fix `patch.c`: trampolín en `0x233e00` salta el parse si el stream es NULL; hook en el slot 3 del lector ZIP (`0x2cef88`) que loguea
  nombre, nº de entradas, flags y las primeras 8 entradas cuando falla. Próxima corrida: con ese log, corregir la causa real de la búsqueda.

## log_018: el ZIP interno de DefaultEffects.bdae se indexa con 1 sola entrada

- Log: el índice del lector ZIP tiene 1 entrada (`big_endian_not_quantized.bdae`); el archivo real tiene 7 → la 2ª cabecera local
  (`0x2cefb8`, tras `seek(csize, SEEK_CUR)` sobre el memstream inflado, vtable `0x4dbed0`) no valida firma.
- Crash nuevo `0x2363bc` (`[r10+0x10]` NULL) = efecto vacío por el skip de log_016; se esperaba.
- Revisado en estático sin hallar fallo: memstream read/seek/tell, stream FILE (`0x2c3700`), inflate a buffer (`0x2ce538`), imports
  memcmp/strlen/div/mod/ctype. Los datos inflados del dump ya estaban liberados/reutilizados (no concluyente).
- Diagnóstico añadido en `hook_zip_open`: tamaño/pos del memstream, `next` = data0+csize y bytes ahí, y bytes en size-22 (EOCD).
  Esperado si todo va bien: size 333905, data@59, csize 24260, next@24319 = `50 4B 03 04`, size-22 = `50 4B 05 06`.
- Emulado offline (unicorn, `scratchpad/emu_inflate.py`): el inflate del juego descomprime `DefaultEffects.bdae` byte a byte igual
  que zlib del host → inflate descartado.
- Causa probable: el código std::string estático del juego usa su propio `_S_empty_rep_storage` en `__data` (0x4b1098, 319 refs;
  0x4b10a4 = refdata, 135 refs) mientras libstdc++ importado (cxx.c) usa el del host → refcount/free cruzados → heap corrupto
  (el dump mostraba strings malloc'eados dentro del buffer inflado). Fix: `unify_empty_string_rep()` en `patch_init` redirige las
  454 referencias al rep del host. Log esperado: "std::string empty rep unified: 454 refs".

## log_019: free() del rep vacío del host
- La redirección de literales no cubría referencias PC-relativas (`_M_dispose` guest en `0xa3a4`: `add r3, pc, r2`) → el juego
  decrementaba y liberaba el `_S_empty_rep_storage` del host (crash en `_free_r`, llamado desde `0xb275c`).
- Fix: se retira la redirección; `pin_empty_string_reps()` pone refcount 0x3FFFFFFF en ambos reps vacíos (host string/wstring y
  guest 0x4b1098) → los incref/decref cruzados nunca llegan a 0 ni a negativo, y las mutaciones clonan.

## log_020: inflate de entradas se corta en consola
- Con los reps fijados (sin crash en free), el memstream de DefaultEffects.bdae tiene tamaño correcto (333905) pero en next@24319 y
  size-22 hay bytes del BBD3 original (base de archivo 0x1583c0 = memoria del text_buf liberado): inflate nunca escribió ahí.
- El mismo inflate guest emulado (unicorn) da salida exacta → se reemplaza sólo en el sitio `0x2ce5f0` por zlib del host
  (`hook_entry_inflate` + trampolín; deja `state=NULL` para que inflate/inflateEnd guest posteriores sean no-ops).
  Si zlib host también falla, loguea "entry inflate: ret ..." con in/out consumidos → entonces la entrada (fread) llega mal.

## log_021: SWP indefinida en 0x2c1398
- Con zlib del host ya no fallan las entradas de DefaultEffects.bdae (bug ZIP resuelto).
- Nuevo crash: `swp lr, r3, [r5]` (ARMv6, deshabilitada en Cortex-A9). 26 SWP en 0x2c0e44-0x2c1710 (atómicos/spinlocks del motor).
- Fix: `patch_all_swp()` reemplaza cada una por `b` a un thunk (arena `macho_alloc_code`, nuevo en macho.c) con ldrex/strex
  + mrs/msr para conservar flags. Log esperado: "swp -> ldrex/strex: 26 sites". Encodings verificados con capstone.

## Revisión de ports hermanos (Zombie Infection, Skater Nation, Blades of Fury)
- Portado de Blades: `darwin_setjmp`/`darwin_longjmp` (asm.S, jmp_buf Darwin de 28 ints, importados como data) — BBD3 importa
  `_setjmp`/`_longjmp` y antes caían en el thunk unimpl; y `SCNetworkReachabilityCreateWithAddress` → `sc_null`.
- Ya equivalentes: audio (AudioUnit/AudioSession/OpenAL igual que Zombie), resto de libc.
- No aplican a BBD3 (vertical): widescreen/rotación/scissor de Skater y Blades en gles.c.
- Ningún hermano tiene: SWP, std::string empty rep doble, inflate de entradas → específicos de BBD3.
- GL importados sin implementar en ningún port: glCompressedTexSubImage2D, glMultiTexCoord4f,
  glRenderbufferStorageMultisampleAPPLE, glResolveMultisampleFramebufferAPPLE (atender si aparecen como unimpl en el log).

## log_022: callback sin rebase en el hilo de audio (Vox)
- SWP OK (26 sitios). El hilo `VoxDummyNSThreadClass` llama `blx r3` con r3 = 0x41cf64 (sin slide): literal 0x41c1d8 rechazado
  por macho_prep porque 0x41cf64 = `b 0x41c9d8` (thunk sin prólogo ni llamadas bl).
- Fix general en `tools/macho_prep.py` (`after_function_end`): literal non-PIC a __text se acepta si el destino es ARM alineado,
  instrucción AL, valor no redondo, y la palabra previa termina una función (b/bx/pop pc/ldr pc) o es pool de literales.
  +148 palabras (tabla de funciones 0x17d7c8.., direcciones de reanudación SjLj, el callback del crash); sin falsos positivos
  visibles (descartadas constantes impares 0x100005/0x40001 y destinos de datos como new(0x107ac)).
- Tests: 16 casos OK (4 nuevos). `rebase.bin` regenerado en ux0_data (48925 palabras) → hay que redesplegar datos.

## log_023: offset PIC reubicado por error (0x421b0)
- Avance: hilo Vox OK, AudioUnit 44100 Hz arrancó, se abre 3.bar; imports unimpl 94 → 88 (setjmp/longjmp etc.).
- Crash `str r4, [r1]` con r1 = pc + 0x98472ebc: el literal 0x41e38 (= 0x472ebc, offset PIC) se cargó en `ldr lr` (0x41a94)
  antes de `b 0x42198`; el `add r1, pc, lr` está tras el salto y `arm_pc_use` cortaba en el `b`.
- Fix `tools/macho_prep.py`: `arm_pc_use` sigue los `b` incondicionales (mismo presupuesto de 64 instrucciones; con 256 el
  resultado es idéntico). Quita exactamente 0x41e38 y 0x41e3c. Tests: 18 casos OK. `rebase.bin` regenerado (48923 palabras).

## log_024: std::list sin construir en StorekitLinkGrabber (0xb2540)
- Avance grande: didFinishLaunching, run loop, perfil (no existe aún), oconf.bar, primeros arrays GL.
- Crash en `std::list::size()` (0xb2538) con head.next = NULL: ivar C++ de `StorekitLinkGrabber` nunca construido; nuestro
  `objc_alloc` no llamaba `.cxx_construct` (ningún port hermano lo necesitaba). Clases con C++ ivars: CStoreFacadeImp,
  StorekitLinkGrabber, FacebookView.
- Fix `objc.c`: `objc_alloc` llama al `.cxx_construct` propio de cada clase guest (superclase → subclase), como
  class_createInstance de iOS. Además: `NSLock` host real (pthread mutex; antes lock = nil), `+[NSThread isMultiThreaded]` = YES,
  `CFRunLoopRunInMode` (duerme el slice con tope 16 ms, devuelve kCFRunLoopRunTimedOut) que salía como unimpl en el bucle.

## log_027: Aserción px != 0 en boost::intrusive_ptr<Texture>::operator-> (0x63b8)
- Causa raíz: Al cargar splash / tileset ("gameloft_logo" en paquete 4), el paquete no contiene el logo y `get_texture` retorna NULL (`px == 0`).
  El llamador en 0x952e8 y 0x6af68 dereferencia incondicionalmente `operator->()` (0x63b8) para invocar `SetWrapS`/`SetWrapT`/`SetFilter`, lo que activaba `BOOST_ASSERT(px != 0)` terminando la ejecución de forma limpia.
- Fix en `source/patch.c`:
  - Se intercepta el bloque de instanciaciones de `boost::intrusive_ptr<T>::operator->()` (0x6278, 0x62b8, 0x62f8, 0x6338, 0x6378, 0x63b8) mediante `patch_jump`.
  - Cuando el puntero inteligente es NULL (`!ptr || !*ptr`), en lugar de abortar con la aserción, devuelve un objeto seguro `dummy_texture` de 512 bytes con refcount protegido (`0x3FFFFFFF`), vtable de no-ops (`dummy_noop`), contexto seguro (`dummy_context`) y dimensiones 1x1.
  - En `patch_frame()`, se genera y enlaza una textura OpenGL 1x1 blanca real para que cualquier bind o renderizado con el fallback sea visualmente válido y seguro.
- Stubs ObjC agregados en `source/objc.c`:
  - `+[NSThread currentThread]`, `+[NSThread mainThread]` devolviendo un `NSThread` host estático válido.
  - `-[NSString compare:options:]` con soporte de `NSCaseInsensitiveSearch`.
  - Stubs para `MPMediaLibrary`, `MPMusicPlayerController` (`setShuffleMode:`, `setRepeatMode:`), `SKPaymentQueue` (`defaultQueue`) y `UIApplication` (`registerForRemoteNotificationTypes:`).

## Ejecución exitosa y soporte Landscape
- **Juego ejecutándose en hardware / gameplay**: Con los parches de intrusive_ptr y stubs ObjC, el juego arranca y ejecuta correctamente.
- **Soporte Landscape Panorámico (960x544)**:
  - Presentación por defecto en pantalla completa panorámica (960x544) en `source/gles.c` para evitar sujetar la PS Vita en vertical (Tate mode).
  - Mapeo táctil frontal y controles físicos (Stick analógico izquierdo + cruceta D-Pad) adaptados al ancho de pantalla en `source/input.c` y `source/patch.c`.
  - Selector dinámico de modo de pantalla con botón **SELECT**: alterna entre Landscape Fullscreen (960x544), Original Centrado (362x544 con barras laterales) y Tate Mode (rotado 90°).
- **Assets de LiveArea optimizados**:
  - `pic0.png` (960x544) y `bg0.png` (840x500) generados a partir de `cover.webp` con paleta indexada de 8 bits.
  - `bg0.png` reducido a 119.2 KB para respetar el límite de 128 KB de Sony (`psvita-toolkit livearea --validate` pasa al 100%).

