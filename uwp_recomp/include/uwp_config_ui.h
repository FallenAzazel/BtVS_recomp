#pragma once
#include <cstdint>

namespace uwp_recomp {
namespace config_ui {

struct UwpConfig {
    uint32_t width               = 1920;
    uint32_t height              = 1080;
    bool     fullscreen          = false;
    bool     widescreenHorPlus   = true;
    bool     enhancedLighting    = true;
    int      ambientLevel        = 1;     // 0=Low(0.25), 1=Balanced(0.48), 2=Bright(0.65)
    bool     ssao                = true;
    bool     hdrBloom            = true;
    bool     charRimSpec         = true;
    bool     vsync               = true;
    bool     showFps             = false;
};

// Load/save config from writable local/saves folder (UWP LocalFolder compatible)
void Load(const char* configPath = "recomp\\saves\\uwp_config.ini");
void Save();
const UwpConfig& Get();
UwpConfig& GetMutable();

// Overlay state & controller/keyboard input
bool IsOpen();
void SetOpen(bool open);
void Toggle();

// Called each frame with current Xbox gamepad state (btns = XInput wButtons, ly = thumbstick Y)
// Returns true if the overlay consumed input (so game should ignore controller this frame).
bool UpdateInput(uint16_t xboxButtons, int16_t thumbLY, bool keyF1, bool keyUp, bool keyDown, bool keyLeft, bool keyRight, bool keyEnter, bool keyBack);

// Optional Win64 host callback when resolution or fullscreen changes from the menu
using DisplayApplyFn = void (*)(uint32_t width, uint32_t height, bool fullscreen);
void SetDisplayApplyCallback(DisplayApplyFn fn);

// Draw the in-game D3D11 config overlay (and optional FPS badge) right before Present()
void RenderOverlay(float fps, uint32_t drawCalls);

} // namespace config_ui
} // namespace uwp_recomp
