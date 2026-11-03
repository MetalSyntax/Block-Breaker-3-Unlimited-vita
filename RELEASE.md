# Block Breaker 3 Unlimited (PS Vita) — Release v1.0

First public release of the native iOS **Mach-O loader** port of *Block Breaker 3 Unlimited* (Gameloft) for the PlayStation Vita. The game boots, reaches the menus, the world map and the levels, and is playable on real hardware with touch and physical controls.

---

## 📦 Release Package Contents

1. **`blockbreaker3unlimited.vpk`** (v01.00): PS Vita application package containing the native Mach-O loader, hardware-accelerated OpenGL ES via **vitaGL**, AudioUnit emulation, and physical/touch controls integration.
2. **`rebase.bin`**: Binary relocation table (**48,923** absolute 32-bit memory pointers) generated specifically for the iOS ARM executable of *Block Breaker 3 Unlimited* (`BBD3`, version 1.6.0).

---

## 🛠️ What is `rebase.bin`?

The iOS executable is a non-PIE (`MH_EXECUTE`) image hard-linked to a low virtual base address, and App Store builds ship with `nlocrel = 0` (no local relocation records). The loader maps the image at `MACHO_SLIDE = 0x98000000`, so every absolute pointer in the data segments must be shifted by that constant.

`rebase.bin` is the compact lookup table telling the loader exactly which words to slide at startup. It is generated with the included [`tools/macho_prep.py`](tools/macho_prep.py):

```bash
python3 -I tools/macho_prep.py ux0_data/blockbreaker3unlimited/game_app/BBD3 \
    ux0_data/blockbreaker3unlimited/rebase.bin --report docs/macho_report.txt
```

(or use the automated `tools/stage_data.sh` pipeline).

File layout:
- `0x00` — magic `'RBS1'`
- `0x04` — word count (48,923)
- `0x08` — array of 32-bit `vmaddr` values to relocate

---

## 🚀 Player Installation Guide

1. **PS Vita Requirements**:
   - Custom Firmware (Enso 3.60 / 3.65 recommended).
   - `kubridge.skprx` active under `*KERNEL` in `ur0:tai/config.txt`.
   - `libshacccg.suprx` present in `ur0:data/` (obtainable via ShaRKBR33D).

2. **Game Assets Setup**:
   - Extract `Payload/BBD3.app/` from your decrypted iOS 1.6.0 `.ipa`.
   - Copy its contents to `ux0:data/blockbreaker3unlimited/game_app/`.
   - Copy `rebase.bin` to `ux0:data/blockbreaker3unlimited/rebase.bin`.

3. **Install the VPK**: transfer and install `blockbreaker3unlimited.vpk` using VitaShell.

---

## 🎮 Controls

| PS Vita Input | Action | Status |
| :--- | :--- | :---: |
| **Front Touchscreen** | Full touch interface (menus, map, paddle) | ✅ Working |
| **Left Stick / D-Pad ←→** | Move the paddle | ✅ Working |
| **D-Pad ↑ / ↓ (Tate mode)** | Move the paddle right / left | ✅ Working |
| **Cross (✖) / Circle (●) / Square (◼) / Triangle (▲)** | Launch the ball (tap the play area) | ✅ Working |
| **Select** | Cycle display mode (Centered → Tate → Fullscreen) | ✅ Working |

---

## 📋 Changelog (v1.0)

- **Loader**: fixed-base mapping at `0x98000000`; 48,923 rebase words applied; 8,063 import slots resolved; 314 global constructors run; `swp` instructions rewritten to `ldrex/strex` (26 sites).
- **Textures**: the engine's static libpng 1.2 / zlib failed on the Vita, leaving every PNG texture invisible; its `inflate*` entry points are redirected to the host zlib and libpng errors recover through `longjmp`.
- **Saves**: emulated the iOS 64-bit-inode `stat` layout (`st_size` at `0x3c`); the save file was previously read as 0 bytes and reset on every boot. Progress now persists in `ux0:data/blockbreaker3unlimited/saves/`.
- **Display**: three display modes, all keeping the original aspect except Fullscreen — **Centered** (default, black side bars), **Tate** (console rotated 90°, black bars, D-Pad remapped) and **Fullscreen** (stretched). Touch input follows the active mode.
- **Performance**: CPU at 444 MHz; buffered logger flushed by a background thread (no SD writes during gameplay); cached lookups for missing bundle files; loader built with `-mcpu=cortex-a9 -mfpu=neon`.
- **LiveArea**: placeholder icon, splash and background (no original artwork included).

## ⚠️ Known Issues

- Saves from builds before v1.0 were reset on boot; that progress cannot be recovered.
- `sfx_repulsion.wav` is not shipped in the IPA, so that sound effect is silent.
- Ads, store and networking are bypassed by design.
- Sustained 60 FPS after the latest performance changes still has to be confirmed on hardware.

Installation and build instructions: see [README.md](README.md).
