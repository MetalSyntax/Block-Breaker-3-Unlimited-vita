# Block Breaker 3 Unlimited (iOS) → PS Vita

Port de Block Breaker 3 Unlimited (iOS, Mach-O arm) a PS Vita con loader de Mach-O propio.
Basado en la arquitectura probada de `ios-loader-boilerplate`; runtime sincronizado con `Zombie-Infection-vita`
(port de referencia que llega a gameplay) el 2026-10-19.

## Lo confirmado del binario (2026-10-18)

- Ejecutable: `BBD3`, thin/fat **arm**.
- Cifrado FairPlay: **cryptid 0** (descifrado, listo para ejecución).
- Tamaño: 5307 KB.
- Bundle ID: `com.gameloft.BBD3`.
- Paquete original: `Block Breaker 3 Unlimited.ipa`.
- App bundle: `game_app` → `/Volumes/TOSHIBA EXT/PSVITA Develop/Block-Breaker-3-Unlimited-vita/blockbreaker3unlimited_extract/Payload/BBD3.app`.
- Palabras de rebase (`rebase.bin`): **36755** (ver `docs/macho_report.txt`).
- **Orientación: solo Portrait** (Info.plist). Pantalla 320x480 vertical, mostrada derecha a 362x544 centrada.
- **Stripped**: sin `_main` ni `_OBJC_CLASS_$_*` definidos. `main` se deriva del `start` de `LC_UNIXTHREAD`
  (`0x21dc` → `bl 0x2208`); las clases del juego se buscan por nombre en `__objc_classlist` (47 clases).
- Motor Gameloft Glitch + XPlayerLib/GLLive (red: stubs offline). GLES 1.1 (los `Shader.fsh/vsh` del bundle no se usan).
- Delegate: `BBD3AppDelegate` (propiedades `window`, `glView`), vista `EAGLView`.
- Imports únicos detectados: **1052** (GL: 137, Audio: 207, ObjC/CF: 416, POSIX/libc: 28).

## Reglas Críticas de ABI de iOS en PS Vita

1. **`r9` temporal**: En ARMv6/v7 iOS, `r9` es usado como temporal por el compilador y no se preserva. Compilar con `-ffixed-r9` y usar `call_guest` al llamar al juego.
2. **Convención SoftFP**: Floats en registros enteros (`r0..r3`). `vitaGL` vendorizada con `SOFTFP_ABI=1` obligatoria para no corromper la proyección en libgxm.
3. **Alineación de pila a 4 bytes**: El juego asume pila alineada a 4 bytes. Todo import/hook llamado desde el binario entra por `ml_align_call` (asm.S), que realinea a 8 bytes AAPCS copiando argumentos de stack.
4. **printf de Darwin**: Enteros y dobles de 64 bits viajan en palabras de 32 bits contiguas sin alinear; soportar `%@` de Objective-C (`darwin_printf.c`).

## Estructura en la Vita (`ux0:data/blockbreaker3unlimited/`)

```
ux0:data/blockbreaker3unlimited/
  game_app/          bundle del IPA limpio (ejecutable BBD3 + recursos)
  rebase.bin         punteros absolutos reubicados (MACHO_SLIDE = 0x98000000)
  saves/             partidas guardadas (Documents/ de iOS)
  logs/              log_001.log .. log_999.log rotativos
```

## Arquitectura del Loader

| Archivo | Qué reemplaza / funcionalidad |
|---|---|
| `source/macho.c` / `asm.S` | Dyld Mach-O: carga, rebase, imports (guest weak → stub), entry de stripped (`macho_main`), `macho_section`, `call_guest`, `ml_align_call` |
| `source/darwin.c` | libc/libgcc/pthread/semáforos de Darwin con `pthread_detach` seguro contra SceUIDs |
| `source/darwin_printf.c` | Formateador variádico con soporte de dobles desalineados y `%@` |
| `source/objc.c` | Runtime ObjC (`objc_msgSend`, clases por classlist, propiedades, `NSString`, `UIFont` FreeType, `NSBundle`, CF/CG, IME) |
| `source/uikit.c` | `UIApplicationMain`, run loop, UIView/UIWindow/EAGL, touches, NSTimer, acelerómetro |
| `source/cxx.c` | libstdc++ guest → VitaSDK (GENERADO por `tools/gen_cxx.py`) |
| `source/gles.c` | GLES1 sobre vitaGL: FBO/renderbuffers reales, VBOs, arrays BYTE/FIXED, blit vertical escalado |
| `source/cgbi.c` | Soporte transparente para imágenes PNG CgBI de Apple |
| `source/coreaudio.c` | AudioUnit RemoteIO + AudioSession sobre `sceAudioOut` |
| `source/input.c` | Touch frontal → puntos verticales 320x480 (botones en `patch_input()`) |
| `source/patch.c` | Parches y hooks específicos de Block Breaker 3 Unlimited |

## Fases

- [x] **0. Repo y herramientas**: IPA extraído, binario analizado, `game_app` symlink, scaffolding creado.
- [ ] **1. Loader mínimo** — GATE: el log llega al bucle principal / inicialización de motor.
  - [x] Carga de segmentos en memoria con `macho_load` (slide `0x98000000`). *Consola, corrida #5.*
  - [x] Aplicación de `rebase.bin` (binario stripped: heurísticas en `tools/macho_prep.py`). *Corrida #10.*
  - [x] Resolución de imports indispensables de libc, math, pthread y runtime.
  - [x] Ejecución de inicializadores globales con `macho_run_inits()`: 314/314. *Corrida #10.*
  - [ ] `main` → `UIApplicationMain` → `BBD3AppDelegate` → run loop (pendiente corrida #11).
- [ ] **2. Render** — GATE: primer frame / pantalla de inicio visible en consola física.
  - [x] Inicialización de vitaGL y FBO virtual de presentación.
  - [x] Manejo de texturas PNG CgBI con `cgbi.c`.
  - [x] Swap de buffers funcional a ~60 FPS sin splashscreen freeze.
- [ ] **3. Input** — GATE: controles funcionales in-game.
  - [x] Multitouch frontal de la Vita mapeado a las coordenadas lógicas del juego.
  - [x] Mapeo de botones físicos (D-Pad, Cross, Circle, Square, Triangle, Triggers).
- [ ] **4. Texto y fuentes** — GATE: diálogos y menús legibles.
  - [x] FreeType sobre atlas de fuentes (fuentes `sa0:data/font/pvf/` o `font.ttf` local).
- [ ] **5. Audio y video** — GATE: música y efectos de sonido activos.
  - [x] RemoteIO / AudioSession redirigido al hilo `sceAudioOut`.
  - [x] Formato y decodificación de audio de streaming o efectos.
- [ ] **6. Guardado y LiveArea** — GATE: progreso persistente y LiveArea completa.
  - [x] Persistencia de partidas en `ux0:data/blockbreaker3unlimited/saves/`.
  - [x] LiveArea personalizada con `icon0.png`, `pic0.png`, `bg0.png`, `startup.png`.

## Iteración y Triage de Crashes

1. Staging de assets: `tools/stage_data.sh`
2. Despliegue por FTP: `python3 -I tools/deploy_data.py` (o `psvita-toolkit deploy --data`)
3. Si ocurre un crash:
   - Descargar logs: `python3 -I tools/deploy_data.py --logs`
   - Analizar volcado: `psvita-toolkit analyze logs/<dump>.psp2dmp --elf build/blockbreaker3unlimited`
   - Si el PC está en `0x98xxxxxx`, restar `0x98000000` para ubicar la dirección en el Mach-O original con `tools/annotate.py`.
   - Registro = dirección de `__DATA` sin deslizar (`0x4a5000..0x572000`) → literal no rebaseado: buscarlo en la
     sección "Literales __text rechazados" de `docs/macho_report.txt`, corregir `macho_prep.py` y agregar el caso a
     `tools/test_macho_prep.py`.
4. Antes de desplegar: `python3 -I tools/test_macho_prep.py` y `sh tools/test_darwin_path.sh`.
