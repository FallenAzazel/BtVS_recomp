#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

namespace config_ui {

struct AppConfig {
    // General
    bool     autoResumeQuickSave = false;
    bool     hideMouseInactivity = true;
    bool     showStatusBar       = true;
    bool     subtitlesEnabled    = true;
    bool     autoPauseUnfocused  = false;
    uint32_t buffyCostume        = 0;    // 0=Level Default, 1=Slayer, 2=Cheerleader, 3=The Bronze, 4=Workout
    // Cheats / Trainer
    bool     cheatInfHealth      = false;
    bool     cheatInfPower       = false;
    bool     cheatInfAmmo        = false;
    bool     cheatOneHitDust     = false;
    // Graphics
    uint32_t aspectMode          = 2;    // 0=4:3, 1=16:9, 2=21:9
    uint32_t resScale            = 4;    // 1..4
    uint32_t targetFps           = 60;   // 30, 60, 120, 165, 0=Uncapped
    uint32_t msaa                = 4;    // 0, 2, 4
    uint32_t aniso               = 16;   // 1, 4, 8, 16
    bool     atmosphericFog      = true;
    bool     softBloom           = false;
    bool     cinemaVignette      = false;
    bool     crtScanlines        = false;
    int      sharpnessPercent    = 35;   // 0..100 (CAS contrast-adaptive sharpening)
    int      fovOffsetDeg        = 0;    // -15..+25 degrees
    int      gammaPercent        = 100;  // 50..200 (100 = 1.00 neutral, >100 lifts dark shadows like CRT)
    int      brightnessPercent   = 100;  // 50..150 (100 = neutral)
    int      contrastPercent     = 100;  // 50..150 (100 = 1.00x)
    // Textures
    bool     enableHDTextures    = true;
    bool     dumpTextures        = false;
    // Audio
    int      masterVolume        = 100;  // 0..100
    bool     muteAudio           = false;
    bool     muteInBackground    = false;
    // Controls
    bool     rumbleEnabled       = true;
    int      deadzonePercent     = 15;   // 0..40
    bool     mouseLookEnabled    = false;
    int      mouseSensitivity    = 100;  // 25..250%
    bool     invertCameraX       = false;
    bool     invertCameraY       = false;
    // Keybinds: 0=Fwd, 1=Back, 2=Left, 3=Right, 4=A, 5=B, 6=X, 7=Y, 8=LT, 9=RT
    int      keybinds[10]        = { 'W', 'S', 'A', 'D', VK_SPACE, VK_SHIFT, VK_CONTROL, 'E', 'Q', 'R' };
    // Quick Save active slot (1..5)
    int      activeSaveSlot      = 1;
    // Debug
    bool     showDebugLog        = false;
};

struct Callbacks {
    void (*onApplyConfig)(const AppConfig& cfg, bool relayout) = nullptr;
    void (*onQuickSave)()       = nullptr;
    void (*onQuickLoad)()       = nullptr;
    void (*onQuickSaveSlot)(int slot) = nullptr;
    void (*onQuickLoadSlot)(int slot) = nullptr;
    void (*onJumpToLevel)(const char* mapName) = nullptr;
    void (*onToggleFullscreen)()= nullptr;
    void (*onReloadHD)()        = nullptr;
    void (*onOpenHDFolder)()    = nullptr;
    void (*onOpenDumpFolder)()  = nullptr;
    void (*onFrameTrace)()      = nullptr;
    void (*onTogglePhotoMode)() = nullptr;
    void (*onTakeScreenshot)()  = nullptr;
    void (*onExitApp)()         = nullptr;
};

// Detect Windows system theme & apply Immersive Dark Mode + dark border/caption via DWM
bool IsSystemDarkMode();
void ApplySystemWindowTheme(HWND hWnd);

// Custom dark top menu bar (28px) & clickable bottom status bar (28px) hosted as child controls
static constexpr int kTopBarHeight    = 28;
static constexpr int kStatusBarHeight = 28;

HWND CreateTopMenuBar(HWND hParent, AppConfig* cfg, const Callbacks* cb);
HWND CreateStatusBar(HWND hParent, AppConfig* cfg, const Callbacks* cb);
void RefreshBars();

// Open (or focus) the modern dark Buffy Configuration window (initialTab: 0..5)
void OpenConfigDialog(HWND hOwner, AppConfig* cfg, const Callbacks* cb, int initialTab = 0);
void CloseConfigDialog();

// Shutdown GDI+ token owned by config_ui
void Shutdown();

} // namespace config_ui
