# Block Breaker 3 Unlimited (iOS Mach-O) → PS Vita

Port de Block Breaker 3 Unlimited (iOS Mach-O) a PS Vita con loader de Mach-O propio.
Leer `PORTING_PLAN.md` antes de modificar el loader.

## Estructura

- `source/macho.c`: Carga, parseo Mach-O, mapeo de segmentos, rebase y resolución de imports.
- `source/darwin.c`: Capa de compatibilidad libc/libgcc/pthread/semáforos de Darwin.
- `source/objc.c`, `source/uikit.c`: runtime ObjC y UIKit/EAGL (traídos de Zombie-Infection-vita, port de referencia).
- `source/cxx.c`: GENERADO por `tools/gen_cxx.py` (no editar a mano).
- `source/hle.h`: rutas e identidad del bundle (`EXE_NAME`, `BUNDLE_ID`, `APP_DELEGATE`).
- `game_app` → symlink a la carpeta `.app` original del IPA.
- `ux0_data/blockbreaker3unlimited/`: Datos extraídos y assets del juego en la consola.
- `PORTING_PLAN.md`: Plan de port y estado actual de fases.
- `port_progress.md`: Registro de progreso.

## Reglas

- ABI iOS ARM: softfp en ARMv6/v7 (floats en registros enteros), r9 temporal (preservar en callbacks).
- Nada del IPA original en el repositorio git (.gitignore anti-DMCA).
- Un bug por corrida de consola física, guiado por logs.
- BBD3 es vertical (Portrait) y stripped: no copiar rotación/widescreen ni supuestos de símbolos de Zombie.
- Tras tocar `tools/macho_prep.py`: `python3 -I tools/test_macho_prep.py`.
