# Buffy the Vampire Slayer — Portable x64 / Direct3D 11 Static Recomp (`uwp_recomp`)

A portable **64-bit (`x64`) Direct3D 11 + XAudio2** static recompilation runtime for Original Xbox *Buffy the Vampire Slayer (Europe)* (`EA-018`), designed for both **Windows x64** and **Xbox Series X|S / Xbox One Dev Mode (UWP)**.

> **Legal Notice:** This repository contains **zero copyrighted game assets, executables, or lifted game code**. You must supply your own legally dumped copy of *Buffy the Vampire Slayer (Europe)* and run the local lifter + compiler to build `BtVSRecompUWP.exe`.

---

## Highlights

- **Native 64-bit Static Recompilation**: Lifts Original Xbox x86 game code (`Default.xbe`) into portable C++ executing against a flat 64 MB guest RAM buffer (no 32-bit fixed-base address constraints).
- **Direct3D 11 Hardware Renderer**:
  - High-DPI resolutions up to **4K & 21:9 Ultrawide** (`1920×1080`, `2560×1440`, `3440×1440`, `3840×2160`, `5120×2160`).
  - **Uncapped High-Refresh-Rate FPS** (`DXGI_SWAP_EFFECT_FLIP_DISCARD` + `DXGI_PRESENT_ALLOW_TEARING` when VSync is OFF).
  - Native Slate Engine **Hor+ Widescreen FOV & frustum culling**, plus **4:3 aspect-preserving edge-anchored HUD** (unstretched health bars, circular reticle, centered TIP banners & weapon ring).
  - **3D Volume Light (`Texture3D`)** dynamic lighting, animated dual-layer translucent water with ripples & reflections, and ring-buffered vertex/index streaming.
- **Full XAudio2 + Windows Media Foundation Audio**:
  - Native **XAudio2** sound effects, Xbox **IMA-ADPCM** voice dialogue, **3D positional panning & distance attenuation**, and hardware-clocked **WMA background music** streaming via Windows Media Foundation (`IMFSourceReader`).
  - Automatic first-run `extracted\install.dat` unpacking into `recomp\cache\` (`Z:\`).
- **Seamless In-Process Stage Transitions**: `XLaunchNewImageA` reloads guest RAM in-memory inside the same window with zero flicker (UWP sandbox compatible).
- **Controller-First In-Game Config Menu**: Pure D3D11 overlay (no Win32 dialogs) usable with an Xbox controller (`Back + Start`) or keyboard (`F1`).

---

## Requirements

1. A legal dump of *Buffy the Vampire Slayer (Europe)* extracted into `extracted\` at the repository root (containing `extracted\Default.xbe`, `extracted\install.dat`, `extracted\pak\`, `extracted\sound\`, etc.):
   ```cmd
   python tools\extract_xiso.py "Buffy the Vampire Slayer (Europe).xiso.iso" extracted
   ```
2. **Python 3.10+** with `capstone` installed:
   ```cmd
   pip install capstone
   ```
3. **Visual Studio 2022 / 2026** (or Build Tools) with:
   - *Desktop development with C++* (for Windows x64)
   - *Universal Windows Platform development* + **Windows 10/11 SDK** (only needed if packaging `.msix` / `.appx` for Xbox Dev Mode)

---

## Quick Start — Windows x64 PC

1. Place your extracted game files in `extracted\` at the repository root.
2. Run the one-click lift & build script from the repository root:
   ```cmd
   uwp_recomp\build.bat
   ```
   - On the first run, `build.bat` lifts `extracted\Default.xbe` into `uwp_recomp\src\generated\` (`recomp_chunk_*.cpp`) and compiles all chunks in parallel (`/MP /O2`).
   - Subsequent runtime-only rebuilds take ~4 seconds. Pass `--rebuild-all` to recompile all lifted chunks.
3. Launch the game:
   ```cmd
   uwp_recomp\build\BtVSRecompUWP.exe --boot-main
   ```
   *(Or run `uwp_recomp\build_and_run_x64.bat` to lift, build, and launch in one step.)*

---

## Building & Deploying for Xbox Series X|S / Xbox One (Dev Mode UWP)

Xbox Dev Mode runs sandboxed **UWP x64** packages (`.msix` / `.appx`) using Direct3D 11, XAudio2, Media Foundation, and XInput — all of which `uwp_recomp` targets natively.

### Step 1 — Lift `Default.xbe` on PC
From the repository root on Windows:
```cmd
python uwp_recomp\tools\lift_xbe_to_cpp.py
```
This generates `uwp_recomp\src\generated\recomp_chunk_*.cpp` locally.

### Step 2 — Configure & Build UWP x64 via CMake
Open a **Developer Command Prompt for VS** and run from the repository root:
```cmd
cmake -S uwp_recomp -B uwp_recomp\build_uwp ^
  -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_SYSTEM_NAME=WindowsStore ^
  -DCMAKE_SYSTEM_VERSION=10.0 ^
  -DUWP_BUILD=ON

cmake --build uwp_recomp\build_uwp --config Release --parallel
```
*(If using Visual Studio 2026, adjust the `-G` generator name accordingly.)*

### Step 3 — Stage & Package the `.msix` / `.appx`
1. Create a staging folder (e.g. `uwp_recomp\pkg\`) containing:
   - `BtVSRecompUWP.exe` (from `uwp_recomp\build_uwp\Release\`)
   - `AppxManifest.xml` (from `uwp_recomp\AppxManifest.xml`)
   - `Assets\` (standard UWP tile PNGs: `Logo.png`, `SmallLogo.png`, `WideLogo.png`, `SplashScreen.png`, `StoreLogo.png`)
   - *(Optional)* Your `extracted\` game data folder inside the package, **or** place `extracted\` on a Dev Mode USB drive / the app's `LocalState` folder so the package stays small.
2. Pack and sign with `makeappx.exe` and `signtool.exe` (included in the Windows SDK):
   ```cmd
   makeappx pack /d uwp_recomp\pkg /p BtVSRecompUWP_x64.msix /o
   ```
   Sign the `.msix` with a self-signed test certificate (or deploy unpacked directly from Visual Studio via **Remote Machine** debugging to your Xbox IP).

### Step 4 — Deploy to Xbox Dev Mode
1. Switch your Xbox Series X|S / Xbox One into **Dev Mode** and note the **Xbox Device Portal** URL (e.g. `https://192.168.1.50:11443`).
2. Open the Device Portal in your PC browser → **My games & apps** → **Add** → upload `BtVSRecompUWP_x64.msix` (plus the VC++ UWP x64 runtime dependency `.appx` from your Windows SDK if prompted).
3. If you didn't bundle `extracted\` inside the `.msix`, open **File Explorer** in the Device Portal → `LocalAppData\BtVSRecompUWP\LocalState\` and upload your `extracted\` folder there (or connect an NTFS USB drive with full permissions).
4. In Dev Home on the Xbox, highlight **Buffy the Vampire Slayer Recomp**, press **View (`Back`) → View details**, set the app type from **App** to **Game** (unlocks full CPU/RAM/GPU resources on Xbox!), and launch!

---

## Controls & In-Game Config Menu

| Action | Xbox Controller | Keyboard |
|---|---|---|
| **Open / Close Config Menu** | `Back + Start` (View + Menu) or `LB + RB + Back` | `F1` or `Insert` |
| **Navigate Menu** | `D-Pad` / `Left Stick` | `Arrow Keys` |
| **Change Setting** | `A` / `D-Pad Left/Right` | `Enter` / `Left/Right` |
| **Save & Close Menu** | `B` | `Backspace` / `F1` |
| **Cycle Resolution (16:9 & 21:9)** | Config Menu | `F5` |
| **Toggle Hor+ Widescreen FOV** | Config Menu | `F6` |
| **Capture 1-Frame D3D11 Draw Trace** | — | `F7` |
| **Toggle Borderless Fullscreen** | Config Menu | `F11` or `Alt+Enter` |

Settings are automatically persisted to `recomp\saves\uwp_config.ini`.

### Optional CLI Flags
- `--boot-main` — Skip startup self-test and boot directly into the game
- `--res <W>x<H>` — Set initial window size (e.g. `--res 2560x1440`, `--res 3440x1440`, `--res 3840x2160`)
- `--fullscreen` / `--windowed` — Override display mode
- `--no-widescreen` — Force 4:3 FOV & HUD
- `--map <map_name>` — Boot directly into a map (e.g. `--map m00_spanishmission_1`)
