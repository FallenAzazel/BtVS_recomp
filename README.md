<img src="https://cdn.freebiesupply.com/logos/large/2x/buffy-the-vampire-slayer-logo-png-transparent.png" alt="BtVS" width="200">

# BtVS Windows Recomp
### Buffy the Vampire Slayer (Xbox) — Win32 Recomp & Decomp Toolkit

Recomp of the [XBOX Buffy the Vampire Slayer](https://en.wikipedia.org/wiki/Buffy_the_Vampire_Slayer_(2002_video_game)) game from 2002.

A native Windows x86 in-process execution runtime, hardware Direct3D 9 renderer, audio mixer, and reverse-engineering/decompilation toolkit for the Original Xbox version of **Buffy the Vampire Slayer (Europe)** (Title ID `EA-018` / `0x45410012`, The Collective Slate Engine, XDK 1.0.4432.1).

> **Legal Notice:** This repository contains **zero copyrighted game assets, binaries, symbols, or disassemblies**. You must supply your own legally dumped copy of *Buffy the Vampire Slayer (Europe)* for Original Xbox.

---

## How to Run (Quick Start)

### Option A — Prebuilt GitHub Release (Easiest, No Visual Studio Required)
1. Download and unzip the latest `BtVS_recomp_*_win32.zip` from **GitHub Releases**.
2. Place your legal **Buffy the Vampire Slayer (Europe)** Original Xbox XISO in the unzipped folder and extract it (requires Python 3.10+):
   ```bash
   python tools/extract_xiso.py "Buffy the Vampire Slayer (Europe).xiso.iso" extracted
   ```
3. Double-click **`Play_Buffy.bat`** — on first launch it unpacks `install.dat` automatically and boots straight into the game!

### Option B — Build from Source
1. Clone this repository and extract your Xbox XISO into `extracted/` (same command as Step 2 above).
2. Verify line 8 of `recomp/build_and_run.bat` points to your Visual Studio `vcvars32.bat` path, then run:
   ```bat
   recomp\build_and_run.bat
   ```
3. *(Optional)* To package a clean prebuilt `.zip` for GitHub Releases, run `tools\package_release.bat [version]`.

### Option C — Portable 64-bit Direct3D 11 / UWP Static Recomp (`uwp_recomp/`, PC x64 & Xbox Dev Mode)
A full **64-bit (`x64`) Direct3D 11 + XAudio2 + Media Foundation** static recompilation designed for **Windows x64** (up to 4K / 21:9 ultrawide, uncapped high-refresh-rate FPS, aspect-correct HUD, full 3D audio + WMA music) and **Xbox Series X|S / Xbox One Dev Mode (UWP)**:
1. Extract your Xbox XISO into `extracted/` (same as above) and install `capstone` (`pip install capstone`).
2. Run `uwp_recomp\build_and_run_x64.bat` (lifts `Default.xbe` locally, compiles `BtVSRecompUWP.exe`, and launches).
3. See **[`uwp_recomp/README.md`](uwp_recomp/README.md)** for full **PC x64** and **Xbox Series X|S Dev Mode (`.msix` / Device Portal)** packaging & deployment instructions.

---

## Features & Enhancements

- **Native Win32 Hardware Direct3D 9 Renderer** (`recomp/src/d3d9_bridge.cpp`):
  - Internal resolution scaling up to **4×** (`640×480` up to `2560×1920` in 4:3, `2560×1440` in 16:9, and `3440×1440` in 21:9 ultrawide)
  - True **Hor+ widescreen & 21:9 ultrawide** FOV/frustum expansion in gameplay with **live Camera FOV Offset slider** (`-15°` to `+25°`), **4:3 pillarboxing** on the Main Menu, and widescreen-aligned HUD & 3D weapon selector
  - Programmable Xbox Vertex Shader translation (restoring foliage, trees, and per-vertex lighting), **3D Volume Light** torch/point-light rendering, and **HD dual-layer animated water ripples**
  - **GPU Post-Processing Pipeline** (`ps_2_0`): **Subtle Soft Bloom** (quarter-res bright-pass + separable Gaussian blur), **AMD CAS-style Contrast Adaptive Sharpening** (`Sharpness` slider), **Cinema Vignette**, **Retro 480p CRT Scanlines**, and live **CRT Gamma Lift / Brightness / Contrast** sliders
  - Up to **4× MSAA**, **16× Anisotropic Filtering**, trilinear mipmapping, and authentic camera-space atmospheric distance fog
  - **HD Texture Replacement, Dumper & Local 4× Upscaler** (`recomp/hd_textures/`, `recomp/dump_textures/`, `tools/upscale_textures.py`, live toggle + `F9` hot-reload, supports non-power-of-2 PNGs and optional Real-ESRGAN AI upscaling)
  - **Photo Mode / Free Camera (`F10`)** and clean full-resolution **PNG Screenshot capture (`F12`)** saved to `recomp/screenshots/`
- **Native Windows Audio Bridge** (`recomp/src/dsound_bridge.cpp`):
  - 48 kHz stereo software mixer via `waveOut` supporting PCM, Xbox IMA-ADPCM SFX/voice dialogue, and WMA background music, with master volume, mute, and background-mute toggles
- **PC Controls & Quality-of-Life** (`recomp/src/xbe_loader.cpp`, `recomp/src/config_ui.cpp`):
  - **XInput gamepad** + **Keyboard & Mouse-Look Camera** support (with configurable sensitivity, Invert X/Y, deadzone, rumble, and 10 custom remappable keybinds)
  - **5-Slot Quick Saves** (`F5`/`F6`, `Shift+1..5`, or `Game → Quick Save Slots`) with optional **Auto-Resume on Launch**
  - **Chapter / Arena Launcher** (`Game → Jump to Level / Arena`) to jump straight into any of the 13 chapters, 4 arenas, or *Buffy History 101*
  - **Buffy Costume Mods** (`Config → Costume Mods`): swap between all 4 original Xbox outfits (*Slayer*, *Cheerleader*, *The Bronze*, *Workout*) plus **4 included Custom 1024×1024 HD Costumes** (*Red Leather*, *Silver Sequin Club*, *Black Mini Dress*, and *Lilac Wrap & Black Pants*) — swaps instantly in real time between same-mesh outfits or reloads in-place at your exact position!
  - **Cutscene Subtitles** toggle (`Config → General → Show Cutscene Subtitles`) and **Native DirectShow `.wmv` FMV Playback** (plays the original 2002 Fox / EA / Collective intro videos with synced audio, skippable via `Space`/`Enter`/`Click`/Gamepad, with an optional **Skip Intro Logos** toggle)
  - **Borderless Fullscreen (`F11`)** and **Auto-Pause When Unfocused (Alt-Tab)** toggle
  - Modern dark/light-aware Win32 **Configuration UI (`F1`)** including an **About** tab
- **Xbox Kernel & XAPI Shims** (`recomp/src/xboxkrnl_win32.cpp`, `recomp/src/xbe_loader.cpp`):
  - Maps `Default.xbe` directly at its native base (`0x00011000..0x00921000`) via a fixed-base `0x00010000` launcher
  - 120 Xbox kernel thunks, virtual filesystem (`D:\`, `Z:\`, `T:\`, `U:\`), automatic `install.dat` cache unpacking, and `XLaunchNewImage` relaunch support
- **Decompilation & Analysis Pipeline** (`tools/`):
  - XISO extractor, XBE-to-PE converter, symbol/map parser (Ghidra & IDA import scripts), and per-module disassembler

---

## What's New

### 2026-11-22
- **Costume Mods Tab & 4 Custom 1024×1024 HD Outfits (`Config → Costume Mods`)**: Moved Buffy's costume selector to its own dedicated **Costume Mods** tab and added 4 included custom HD outfits (*Red Leather*, *Silver Sequin Club*, *Black Mini Dress*, and *Lilac Wrap & Black Pants*) stored in `recomp/hd_textures/costume_*/` (leaving all 4 original Xbox outfits intact, with instant zero-reload swapping between same-mesh costumes and optional local inspiration preview cards).
- **Updated About Tab**: Embedded the open-source BtVS logo directly into `Config → About` with automatic Dark/Light mode tinting.

### 2026-11-21
- **Native `.wmv` FMV Playback & HD Movie Overrides (`recomp/hd_movies/`)**: Plays the original 2002 intro videos (`Logo_Legal`, `Logo_EA`, `Logo_Fox`, `Logo_Collective`) and `attract.wmv` trailer natively via DirectShow with synced audio, jet-black 16:9/21:9 pillarboxing, skippable controls, a **Skip Intro Logos on Launch** toggle in `Config → General`, and automatic `.mp4`/`.wmv`/`.avi` HD overrides in `recomp/hd_movies/`.
- **Cinema Vignette & Retro 480p CRT Scanlines**: Added live toggles in `Config → Graphics` inside the upgraded `ps_3_0` post-processing pipeline alongside **Sharpness (CAS)** (`0%–100%`).
- **Local 4× HD Texture Upscaler (`tools/upscale_textures.py`)**: Zero-dependency batch upscaler that converts dumped Xbox textures (`recomp/dump_textures/`) into alpha-safe 4× HD PNGs in `recomp/hd_textures/` (supports optional `realesrgan-ncnn-vulkan` AI upscaling, Pillow, or built-in Windows GDI+ `HighQualityBicubic` with RGB edge-bleed to prevent UI/text halos). Also added a clickable **`DUMP: OFF / ON`** status bar pill and fixed HD texture cache keying by `xtex->Data` + hash.
- **System-Matched Dark/Light Popup Menus**: All expanded Win32 dropdown menus (`Game`, `View`, `Cheats`, `Quick Save Slots`, `Jump to Level`, and Config dropdowns) now follow your Windows Dark/Light theme automatically.
- **Photo Mode / Free Camera (`F10` / `View → Photo Mode`)**: Freezes gameplay cleanly, hides all 2D HUD overlays & 3D weapon icons, and lets you fly freely with `WASD`, `Q`/`E`, and Mouse/Arrows (`Shift` = fast, `Ctrl` = slow).
- **Clean PNG Screenshots (`F12` / `View → Take Screenshot`)**: Captures full-resolution, post-processed, MSAA-resolved PNG screenshots directly to `recomp/screenshots/Buffy_YYYYMMDD_HHMMSS.png`.
- **Fun Cheats / Trainer Menu (`Game → Cheats`)**: Added live toggles for Infinite Health, Infinite Slayer Power, Infinite Weapon Durability / Ammo, and One-Hit Dusting.
- **Auto-Pause on Alt-Tab**: Added `Config → General → Pause Game When Unfocused (Alt-Tab)` to cleanly freeze gameplay when the window loses focus.
- **Cutscene Subtitles**: Added live `Config → General → Show Cutscene Subtitles` toggle wired directly to the engine's subtitle flag.
- **PC Controls & QOL**: Added **Mouse-Look Camera** (with sensitivity & Invert X/Y), **10 custom remappable keybinds**, **5-Slot Quick Saves**, **Chapter / Arena Launcher**, **Buffy Costume Swapper**, and an **About** tab in Config.
- **Rendering & HUD Polish**: Added GPU **Soft Bloom** & **CRT Gamma/Brightness/Contrast** post-processing, live **Camera FOV Offset**, 3D volume torch lighting, restored programmable-VS foliage/trees, HD water ripples, and fixed widescreen/21:9 weapon ring & tooltip alignment.

---

## Fun Cheats

Access optional trainer toggles anytime from the top menu bar under **`Game → Cheats`** (persisted automatically in `recomp/settings.ini`):

- **Infinite Health**: Blocks all incoming damage to Buffy and keeps her health bar topped up.
- **Infinite Slayer Power**: Enables the engine's native infinite Slayer Power flag so special attacks and supernatural abilities never drain your power gauge.
- **Infinite Weapon Durability / Ammo**: Prevents ranged ammo (crossbow bolts, holy water, etc.) from depleting and stops melee weapons (stakes, torches, shovels, axes) from taking durability damage or breaking.
- **One-Hit Dusting**: Multiplies damage dealt to enemies so vampires and demons dust in a single hit.

---

## HD Texture Generation & Sharpening

You can generate your own crisp **4× HD texture pack** locally (for the Main Menu, HUD, characters, and environments) without distributing any copyrighted game assets:

### Step 1 — Dump Textures While Playing
1. Launch the game via `recomp\build_and_run.bat`.
2. Turn texture dumping **ON** using any of these methods:
   - Press **`F8`**
   - Click the **`DUMP: OFF`** pill in the bottom status bar (it highlights red as **`DUMP: ON`**)
   - Enable **Dump Visible Textures to PNG** in `Config → Textures`
3. Browse the **Main Menu**, pause screens, or walk through any level you want to upscale. Every visible Xbox texture is automatically decoded and saved as a hash-named PNG in `recomp/dump_textures/`.
4. Press **`F8`** (or click **`DUMP: ON`**) again to turn dumping **OFF** once finished.

### Step 2 — Batch-Upscale All Dumped Textures to 4× HD
Open a terminal in the **repository root folder** (the folder containing `recomp/`, `tools/`, and `README.md`) and run:

```bash
python tools/upscale_textures.py --scale 4
```

- **Zero extra dependencies required**: Out of the box, it uses built-in **Windows GDI+ `HighQualityBicubic`** with alpha-safe RGB edge-bleed (so transparent menu text and HUD icons never get dark halos).
- **Incremental by default**: Already-upscaled textures in `recomp/hd_textures/` are automatically skipped, so you can dump new levels anytime and re-run the same command. Add `--force` to regenerate all files.
- **Optional AI 4× Upscaling (Real-ESRGAN)**: For full AI-reconstructed detail, download `realesrgan-ncnn-vulkan.exe` (and its `models/` folder) into `tools/` — or pass `--realesrgan path/to/realesrgan-ncnn-vulkan.exe` — and run:
  ```bash
  python tools/upscale_textures.py --scale 4 --force
  ```

### Step 3 — Hot-Reload In-Game & Fine-Tune Sharpness
1. Make sure **`HD TEX: ON`** is enabled in the bottom status bar (or `Config → Textures → Enable Custom HD Textures`).
2. Press **`F9`** in-game (or click **Reload HD Textures** in `Config → Textures`) to hot-reload your new 4× HD textures immediately.
3. Adjust **`Config → Graphics → Sharpness (CAS)`** (`0%–100%`, default `35%`) for real-time halo-free contrast-adaptive sharpening across both 2D menus and 3D gameplay.

### Optional — HD FMV Video Replacements (`recomp/hd_movies/`)
Drop locally upscaled `.mp4`, `.wmv`, or `.avi` replacements (e.g. `Logo_Fox.mp4`, `Logo_EA.mp4`, `Logo_Collective.mp4`, `Logo_Legal.mp4`, `attract.mp4`) into `recomp/hd_movies/` — the DirectShow movie player automatically detects and plays them at full HD/4K instead of the 480p originals.

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

> **Prebuilt Releases:** If you downloaded a prebuilt `.zip` from GitHub Releases, just extract your Xbox XISO into `extracted/` (Step 1) and double-click **`Play_Buffy.bat`** — no Visual Studio required! Maintainers can build a clean release `.zip` anytime via `tools\package_release.bat [version]`.

---

## Controls & Hotkeys

- **Gamepad:** Standard XInput controller (Xbox 360 / One / Series) mapped to Xbox Port 0
- **Mouse-Look Camera:** Enable in `Config → Controls`, click the game viewport to capture mouse (`LMB` = Attack, `RMB` = Aim), press `Alt` or `Esc` to release
- **Menu Bar / Status Bar:** Live graphics, audio, cheats, chapter launcher, and quick-save controls
- **`F1`**: Open Configuration dialog
- **`F2`**: Toggle Debug Telemetry Log panel
- **`F5` / `F6`**: Quick Save / Quick Load active slot (`Shift+1`..`Shift+5` switches active slot)
- **`F7`**: Capture a single-frame D3D9 draw-call trace to `recomp/frame_trace.log`
- **`F8`**: Toggle visible texture dumper (`recomp/dump_textures/`)
- **`F9`**: Hot-reload HD textures from `recomp/hd_textures/`
- **`F10`**: Toggle **Photo Mode / Free Camera** (`WASD` move, `E`/`Space` up, `Q`/`C` down, Click+Mouse or Arrows look, `Shift` fast, `Ctrl` slow, `Esc` exit)
- **`F11`**: Toggle Borderless Fullscreen
- **`F12`**: Save clean PNG screenshot to `recomp/screenshots/`

---

## Project Layout

```text
├── recomp/
│   ├── src/                # Win32 launcher, XBE loader, kernel/XAPI shims, D3D9 & DSound bridges, UI
│   ├── include/            # Runtime headers
│   ├── build_and_run.bat   # MSVC x86 build & launch script
│   └── CMakeLists.txt      # CMake build definition
├── tools/                  # Python XISO extractor, XBE->PE converter, symbol/disassembly & 4x HD texture upscaler
├── .gitignore              # Excludes all game data, extracted assets, builds, logs & saves
└── README.md
```


>Note: No support is provided, this has been done for the fun of it :) Enjoy
