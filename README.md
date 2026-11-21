# BtVS Windows Recomp
### Buffy the Vampire Slayer (Xbox) — Win32 Recomp & Decomp Toolkit

Recomp of the [XBOX Buffy the Vampire Slayer](https://en.wikipedia.org/wiki/Buffy_the_Vampire_Slayer_(2002_video_game)) game from 2002.

A native Windows x86 in-process execution runtime, hardware Direct3D 9 renderer, audio mixer, and reverse-engineering/decompilation toolkit for the Original Xbox version of **Buffy the Vampire Slayer (Europe)** (Title ID `EA-018` / `0x45410012`, The Collective Slate Engine, XDK 1.0.4432.1).

> **Legal Notice:** This repository contains **zero copyrighted game assets, binaries, symbols, or disassemblies**. You must supply your own legally dumped copy of *Buffy the Vampire Slayer (Europe)* for Original Xbox.

---

## Features & Enhancements

- **Native Win32 Hardware Direct3D 9 Renderer** (`recomp/src/d3d9_bridge.cpp`):
  - Internal resolution scaling up to **4×** (`640×480` up to `2560×1920` in 4:3, `2560×1440` in 16:9, and `3440×1440` in 21:9 ultrawide)
  - True **Hor+ widescreen & 21:9 ultrawide** FOV/frustum expansion in gameplay with **4:3 pillarboxing** on the Main Menu and edge-anchored HUD
  - Up to **4× MSAA**, **16× Anisotropic Filtering**, trilinear mipmapping, and authentic camera-space atmospheric distance fog
  - **HD Texture Replacement & Dumper** (`recomp/hd_textures/`, `recomp/dump_textures/`, live toggle + `F9` hot-reload, supports non-power-of-2 PNGs)
- **Native Windows Audio Bridge** (`recomp/src/dsound_bridge.cpp`):
  - 48 kHz stereo software mixer via `waveOut` supporting PCM, Xbox IMA-ADPCM SFX/voice dialogue, and WMA background music
- **Xbox Kernel, XAPI & Input Shims** (`recomp/src/xboxkrnl_win32.cpp`, `recomp/src/xbe_loader.cpp`):
  - Maps `Default.xbe` directly at its native base (`0x00011000..0x00921000`) via a fixed-base `0x00010000` launcher
  - 120 Xbox kernel thunks, virtual filesystem (`D:\`, `Z:\`, `T:\`, `U:\`), automatic `install.dat` cache unpacking, and `XLaunchNewImage` relaunch support
  - **XInput gamepad** + keyboard controls with configurable deadzone & rumble, plus high-precision QPC timing & target FPS limiter
- **Decompilation & Analysis Pipeline** (`tools/`):
  - XISO extractor, XBE-to-PE converter, symbol/map parser (Ghidra & IDA import scripts), and per-module disassembler

---

## Prerequisites

- **Windows 10 / 11** (64-bit or 32-bit; builds a 32-bit x86 binary)
- **Visual Studio Build Tools / Visual Studio** with the **Desktop development with C++** workload (`cl.exe` x86 / `vcvars32.bat`)
- **Python 3.10+** (for XISO extraction and optional decompilation scripts)
- Your own legal **Buffy the Vampire Slayer (Europe)** Original Xbox XISO or extracted disc files

---

## Quick Start

### 1. Extract Your Xbox Disc / XISO
Place your XISO in the repository root and extract it into `extracted/`:

```bash
python tools/extract_xiso.py "Buffy the Vampire Slayer (Europe).xiso.iso" extracted
```

After extraction, `extracted/Default.xbe`, `extracted/install.dat`, `extracted/pak/`, `extracted/dialogue/`, and `extracted/Sound/` should exist.

### 2. (Optional) Generate Decompilation & Symbol Artifacts
To generate the converted PE image, Ghidra/IDA symbol import scripts, and module disassemblies into `decompile/`:

```bash
python tools/build_pipeline.py
```

### 3. Build & Run
Open `recomp/build_and_run.bat` and verify line 8 points to your local Visual Studio `vcvars32.bat` path, then run:

```bat
recomp\build_and_run.bat
```

On first launch, the runtime automatically unpacks `extracted\install.dat` into `recomp\cache\` (`Z:\`), creates save data in `recomp\saves\` (`T:\` / `U:\`), and launches the game.

---

## Controls & Hotkeys

- **Gamepad:** Standard XInput controller (Xbox 360 / One / Series) mapped to Xbox Port 0
- **Menu Bar / Status Bar:** Live graphics settings (Resolution Scale, Aspect Ratio, MSAA, AF, Fog, HD Textures, Volume)
- **`F7`**: Capture a single-frame D3D9 draw-call trace to `recomp/frame_trace.log`
- **`F9`**: Hot-reload HD textures from `recomp/hd_textures/`

---

## Project Layout

```text
├── recomp/
│   ├── src/                # Win32 launcher, XBE loader, kernel/XAPI shims, D3D9 & DSound bridges, UI
│   ├── include/            # Runtime headers
│   ├── build_and_run.bat   # MSVC x86 build & launch script
│   └── CMakeLists.txt      # CMake build definition
├── tools/                  # Python XISO extractor, XBE->PE converter, symbol & disassembly scripts
├── .gitignore              # Excludes all game data, extracted assets, builds, logs & saves
└── README.md
```


>Note: No support is provided, this has been done for the fun of it :) Enjoy