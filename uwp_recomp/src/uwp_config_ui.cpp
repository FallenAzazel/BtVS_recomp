#include "uwp_config_ui.h"
#include "d3d11_bridge.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace uwp_recomp {
namespace config_ui {

static UwpConfig      g_cfg{};
static std::string    g_cfgPath = "recomp\\saves\\uwp_config.ini";
static bool           g_open    = false;
static int            g_sel     = 0;
static DisplayApplyFn g_applyFn = nullptr;

// Previous frame edge-trigger states
static bool g_prevToggle = false;
static bool g_prevUp     = false;
static bool g_prevDown   = false;
static bool g_prevLeft   = false;
static bool g_prevRight  = false;
static bool g_prevAccept = false;
static bool g_prevBack   = false;

struct ResOpt { uint32_t w, h; const char* label; };
static constexpr ResOpt kResOpts[] = {
    { 1280,  720, "1280 x 720 (16:9 720p)"   },
    { 1720,  720, "1720 x 720 (21:9 720p)"   },
    { 1920, 1080, "1920 x 1080 (16:9 1080p)" },
    { 2560, 1080, "2560 x 1080 (21:9 1080p)" },
    { 2560, 1440, "2560 x 1440 (16:9 1440p)" },
    { 3440, 1440, "3440 x 1440 (21:9 UWQHD)" },
    { 3840, 1600, "3840 x 1600 (21:9 QHD+)"  },
    { 3840, 2160, "3840 x 2160 (16:9 4K)"    },
    { 5120, 2160, "5120 x 2160 (21:9 5K2K)"  },
};
static constexpr int kNumResOpts = 9;
static constexpr int kNumMenuItems = 11;

static int FindResIndex(uint32_t w, uint32_t h) {
    for (int i = 0; i < kNumResOpts; ++i) {
        if (kResOpts[i].w == w && kResOpts[i].h == h) return i;
    }
    return 1; // Default 1080p
}

static void SyncBridgeSettings() {
    d3d11_bridge::SetWidescreenHorPlus(g_cfg.widescreenHorPlus);
    d3d11_bridge::SetEnhancedLighting(g_cfg.enhancedLighting);
    static const float kAmbVals[3] = { 0.25f, 0.48f, 0.65f };
    int idx = (g_cfg.ambientLevel < 0) ? 0 : (g_cfg.ambientLevel > 2 ? 2 : g_cfg.ambientLevel);
    d3d11_bridge::SetCharAmbient(kAmbVals[idx]);
    d3d11_bridge::SetSsaoEnabled(g_cfg.ssao);
    d3d11_bridge::SetHdrBloomEnabled(g_cfg.hdrBloom);
    d3d11_bridge::SetCharRimSpecEnabled(g_cfg.charRimSpec);
    d3d11_bridge::SetVsyncEnabled(g_cfg.vsync);
}

void Load(const char* configPath) {
    if (configPath && *configPath) g_cfgPath = configPath;
    // Inherit resolution defaults from recomp\recomp_config.ini first if uwp_config.ini doesn't exist yet
    if (FILE* rf = std::fopen("recomp\\recomp_config.ini", "r")) {
        char line[256]{};
        while (std::fgets(line, sizeof(line), rf)) {
            uint32_t v = 0;
            if (std::sscanf(line, "Width=%u", &v) == 1 && v >= 640 && v <= 7680) g_cfg.width = v;
            else if (std::sscanf(line, "Height=%u", &v) == 1 && v >= 480 && v <= 4320) g_cfg.height = v;
            else if (std::sscanf(line, "Fullscreen=%u", &v) == 1) g_cfg.fullscreen = (v != 0);
            else if (std::sscanf(line, "Widescreen=%u", &v) == 1) g_cfg.widescreenHorPlus = (v != 0);
        }
        std::fclose(rf);
    }
    if (FILE* f = std::fopen(g_cfgPath.c_str(), "r")) {
        char line[256]{};
        while (std::fgets(line, sizeof(line), f)) {
            uint32_t v = 0;
            if (std::sscanf(line, "Width=%u", &v) == 1 && v >= 640 && v <= 7680) g_cfg.width = v;
            else if (std::sscanf(line, "Height=%u", &v) == 1 && v >= 480 && v <= 4320) g_cfg.height = v;
            else if (std::sscanf(line, "Fullscreen=%u", &v) == 1) g_cfg.fullscreen = (v != 0);
            else if (std::sscanf(line, "Widescreen=%u", &v) == 1) g_cfg.widescreenHorPlus = (v != 0);
            else if (std::sscanf(line, "EnhancedLighting=%u", &v) == 1) g_cfg.enhancedLighting = (v != 0);
            else if (std::sscanf(line, "AmbientLevel=%u", &v) == 1 && v <= 2) g_cfg.ambientLevel = static_cast<int>(v);
            else if (std::sscanf(line, "SSAO=%u", &v) == 1) g_cfg.ssao = (v != 0);
            else if (std::sscanf(line, "HDRBloom=%u", &v) == 1) g_cfg.hdrBloom = (v != 0);
            else if (std::sscanf(line, "CharRimSpec=%u", &v) == 1) g_cfg.charRimSpec = (v != 0);
            else if (std::sscanf(line, "VSync=%u", &v) == 1) g_cfg.vsync = (v != 0);
            else if (std::sscanf(line, "ShowFPS=%u", &v) == 1) g_cfg.showFps = (v != 0);
        }
        std::fclose(f);
    }
    SyncBridgeSettings();
}

void Save() {
    std::error_code ec;
    auto parent = std::filesystem::path(g_cfgPath).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    if (FILE* f = std::fopen(g_cfgPath.c_str(), "w")) {
        std::fprintf(f,
            "Width=%u\nHeight=%u\nFullscreen=%u\nWidescreen=%u\n"
            "EnhancedLighting=%u\nAmbientLevel=%d\nSSAO=%u\nHDRBloom=%u\nCharRimSpec=%u\n"
            "VSync=%u\nShowFPS=%u\n",
            g_cfg.width, g_cfg.height, g_cfg.fullscreen ? 1u : 0u,
            g_cfg.widescreenHorPlus ? 1u : 0u, g_cfg.enhancedLighting ? 1u : 0u,
            g_cfg.ambientLevel, g_cfg.ssao ? 1u : 0u, g_cfg.hdrBloom ? 1u : 0u,
            g_cfg.charRimSpec ? 1u : 0u, g_cfg.vsync ? 1u : 0u, g_cfg.showFps ? 1u : 0u);
        std::fclose(f);
    }
}

const UwpConfig& Get() { return g_cfg; }
UwpConfig& GetMutable() { return g_cfg; }
bool IsOpen() { return g_open; }
void SetOpen(bool open) { g_open = open; }
void Toggle() { g_open = !g_open; if (!g_open) Save(); }
void SetDisplayApplyCallback(DisplayApplyFn fn) { g_applyFn = fn; }

static void ApplyItemDelta(int item, int dir) {
    switch (item) {
        case 0: { // Resolution
            int idx = (FindResIndex(g_cfg.width, g_cfg.height) + dir + kNumResOpts) % kNumResOpts;
            g_cfg.width  = kResOpts[idx].w;
            g_cfg.height = kResOpts[idx].h;
            if (g_applyFn) g_applyFn(g_cfg.width, g_cfg.height, g_cfg.fullscreen);
            else d3d11_bridge::Resize(g_cfg.width, g_cfg.height);
            break;
        }
        case 1: // Display Mode
            g_cfg.fullscreen = !g_cfg.fullscreen;
            if (g_applyFn) g_applyFn(g_cfg.width, g_cfg.height, g_cfg.fullscreen);
            break;
        case 2: // Widescreen Hor+
            g_cfg.widescreenHorPlus = !g_cfg.widescreenHorPlus;
            break;
        case 3: // Enhanced Dynamic Lighting (3D Volume Lights)
            g_cfg.enhancedLighting = !g_cfg.enhancedLighting;
            break;
        case 4: // Character Ambient
            g_cfg.ambientLevel = (g_cfg.ambientLevel + dir + 3) % 3;
            break;
        case 5: // SSAO
            g_cfg.ssao = !g_cfg.ssao;
            break;
        case 6: // HDR Bloom
            g_cfg.hdrBloom = !g_cfg.hdrBloom;
            break;
        case 7: // Character Rim & Specular
            g_cfg.charRimSpec = !g_cfg.charRimSpec;
            break;
        case 8: // VSync
            g_cfg.vsync = !g_cfg.vsync;
            break;
        case 9: // Show FPS
            g_cfg.showFps = !g_cfg.showFps;
            break;
        case 10: // Save & Close
            Save();
            g_open = false;
            break;
    }
    SyncBridgeSettings();
    Save();
}

bool UpdateInput(uint16_t xb, int16_t ly, bool keyF1, bool keyUp, bool keyDown,
                 bool keyLeft, bool keyRight, bool keyEnter, bool keyBack) {
    // Xbox controller chord: Back+Start (0x0030) OR LB+RB+Back (0x0320)
    bool padToggle = ((xb & 0x0030u) == 0x0030u) || ((xb & 0x0320u) == 0x0320u);
    bool toggleNow = padToggle || keyF1;
    if (toggleNow && !g_prevToggle) {
        Toggle();
    }
    g_prevToggle = toggleNow;

    if (!g_open) {
        g_prevUp = g_prevDown = g_prevLeft = g_prevRight = g_prevAccept = g_prevBack = true;
        return false;
    }

    bool upNow     = ((xb & 0x0001u) != 0) || (ly >  18000) || keyUp;
    bool downNow   = ((xb & 0x0002u) != 0) || (ly < -18000) || keyDown;
    bool leftNow   = ((xb & 0x0004u) != 0) || keyLeft;
    bool rightNow  = ((xb & 0x0008u) != 0) || keyRight;
    bool acceptNow = ((xb & 0x1000u) != 0) || keyEnter; // XInput A
    bool backNow   = ((xb & 0x2000u) != 0) || keyBack;  // XInput B

    if (upNow     && !g_prevUp)     g_sel = (g_sel + kNumMenuItems - 1) % kNumMenuItems;
    if (downNow   && !g_prevDown)   g_sel = (g_sel + 1) % kNumMenuItems;
    if (leftNow   && !g_prevLeft)   ApplyItemDelta(g_sel, -1);
    if (rightNow  && !g_prevRight)  ApplyItemDelta(g_sel, +1);
    if (acceptNow && !g_prevAccept) ApplyItemDelta(g_sel, +1);
    if (backNow   && !g_prevBack)   { Save(); g_open = false; }

    g_prevUp     = upNow;
    g_prevDown   = downNow;
    g_prevLeft   = leftNow;
    g_prevRight  = rightNow;
    g_prevAccept = acceptNow;
    g_prevBack   = backNow;
    return true;
}

void RenderOverlay(float fps, uint32_t drawCalls) {
    // Optional compact FPS / resolution HUD badge in top-right corner (640x480 virtual coords)
    if (g_cfg.showFps && !g_open) {
        char badge[64]{};
        std::snprintf(badge, sizeof(badge), "%.0f FPS | %ux%u | %u DC",
                      fps, d3d11_bridge::GetWidth(), d3d11_bridge::GetHeight(), drawCalls);
        float bw = static_cast<float>(std::strlen(badge)) * 5.2f + 12.0f;
        float bx = 632.0f - bw;
        d3d11_bridge::DrawOverlayRect(bx, 8.0f, bw, 16.0f, 0xB812081Cu);
        d3d11_bridge::DrawOverlayRect(bx, 8.0f, bw, 1.5f,  0xFF9C27B0u);
        d3d11_bridge::DrawOverlayText(bx + 6.0f, 12.5f, 0.85f, 0xFFF3E5F5u, badge);
    }

    if (!g_open) return;

    // Dim backdrop
    d3d11_bridge::DrawOverlayRect(0.0f, 0.0f, 640.0f, 480.0f, 0x8806030Au);

    // Main gothic config panel (centered in 640x480 virtual canvas)
    const float px = 110.0f, py = 36.0f, pw = 420.0f, ph = 406.0f;
    d3d11_bridge::DrawOverlayRect(px - 2.0f, py - 2.0f, pw + 4.0f, ph + 4.0f, 0xFF8E24AAu); // Violet outer border
    d3d11_bridge::DrawOverlayRect(px, py, pw, ph, 0xEA12081Cu);                              // Deep gothic midnight bg
    d3d11_bridge::DrawOverlayRect(px, py, pw, 38.0f, 0xFF3A0F4Au);                           // Header bar
    d3d11_bridge::DrawOverlayRect(px, py + 38.0f, pw, 2.0f, 0xFFD81B60u);                    // Crimson accent rule

    d3d11_bridge::DrawOverlayText(px + 16.0f, py + 10.0f, 1.35f, 0xFFFFFFFFu, "BUFFY RECOMP - UWP / XBOX CONFIG");
    d3d11_bridge::DrawOverlayText(px + 16.0f, py + 26.0f, 0.80f, 0xFFCE93D8u, "Direct3D 11 x64 Runtime  |  Xbox Dev Mode & PC");

    const char* ambNames[3] = { "LOW (0.25)", "BALANCED (0.48)", "BRIGHT (0.65)" };
    int ambIdx = (g_cfg.ambientLevel < 0) ? 0 : (g_cfg.ambientLevel > 2 ? 2 : g_cfg.ambientLevel);
    uint32_t liveW = d3d11_bridge::GetWidth();
    uint32_t liveH = d3d11_bridge::GetHeight();
    char resBuf[48]{};
    if (g_cfg.fullscreen) {
        std::snprintf(resBuf, sizeof(resBuf), "NATIVE (%u x %u)", liveW, liveH);
    } else {
        bool matched = false;
        for (int i = 0; i < kNumResOpts; ++i) {
            if (kResOpts[i].w == liveW && kResOpts[i].h == liveH) {
                std::snprintf(resBuf, sizeof(resBuf), "%s", kResOpts[i].label);
                matched = true;
                break;
            }
        }
        if (!matched) std::snprintf(resBuf, sizeof(resBuf), "%u x %u (CUSTOM)", liveW, liveH);
    }

    const char* labels[kNumMenuItems] = {
        "Resolution",
        "Display Mode",
        "Widescreen (Hor+ FOV)",
        "Enhanced Dynamic Lighting",
        "Character Ambient",
        "SSAO Ambient Occlusion",
        "HDR Bloom & Tone Map",
        "Character Rim & Specular",
        "VSync",
        "Show FPS Counter",
        "Save & Close Menu",
    };
    const char* values[kNumMenuItems] = {
        resBuf,
        g_cfg.fullscreen        ? "FULLSCREEN" : "WINDOWED",
        g_cfg.widescreenHorPlus ? "ON (HOR+)"  : "OFF (4:3)",
        g_cfg.enhancedLighting  ? "ON"         : "OFF",
        ambNames[ambIdx],
        g_cfg.ssao              ? "ON"         : "OFF",
        g_cfg.hdrBloom          ? "ON"         : "OFF",
        g_cfg.charRimSpec       ? "ON"         : "OFF",
        g_cfg.vsync             ? "ON (VSYNC)" : "OFF (UNCAPPED)",
        g_cfg.showFps           ? "ON"         : "OFF",
        "[ A / ENTER ]",
    };

    float rowY = py + 48.0f;
    const float rowH = 27.0f;
    for (int i = 0; i < kNumMenuItems; ++i) {
        bool sel = (i == g_sel);
        if (sel) {
            d3d11_bridge::DrawOverlayRect(px + 10.0f, rowY - 2.0f, pw - 20.0f, rowH - 2.0f, 0xCC5E1770u);
            d3d11_bridge::DrawOverlayRect(px + 10.0f, rowY - 2.0f, 4.0f, rowH - 2.0f, 0xFFFF4081u);
        } else if ((i & 1) == 0) {
            d3d11_bridge::DrawOverlayRect(px + 10.0f, rowY - 2.0f, pw - 20.0f, rowH - 2.0f, 0x33241033u);
        }
        uint32_t lblCol = sel ? 0xFFFFFFFFu : 0xFFD1C4E9u;
        uint32_t valCol = sel ? 0xFFFFD54Fu : 0xFFB39DDBu;
        d3d11_bridge::DrawOverlayText(px + 22.0f, rowY + 6.5f, 1.0f, lblCol, labels[i]);
        d3d11_bridge::DrawOverlayText(px + 235.0f, rowY + 6.5f, 1.0f, valCol, values[i]);
        rowY += rowH;
    }

    // Footer bar
    d3d11_bridge::DrawOverlayRect(px, py + ph - 50.0f, pw, 1.5f, 0xFF6A1B9Au);
    char perfLine[96]{};
    std::snprintf(perfLine, sizeof(perfLine), "LIVE: %.1f FPS  |  %ux%u  |  %u Draws",
                  fps, d3d11_bridge::GetWidth(), d3d11_bridge::GetHeight(), drawCalls);
    d3d11_bridge::DrawOverlayText(px + 16.0f, py + ph - 43.0f, 0.82f, 0xFF80CBC4u, perfLine);
    d3d11_bridge::DrawOverlayText(px + 16.0f, py + ph - 30.0f, 0.80f, 0xFFFFD54Fu,
        "GITHUB: https://github.com/FallenAzazel/BtVS_recomp");
    d3d11_bridge::DrawOverlayText(px + 16.0f, py + ph - 16.0f, 0.80f, 0xFFB39DDBu,
        "D-Pad/Arrows: Select  |  A/Left/Right: Change  |  B / F1 / Back+Start: Close");
}

} // namespace config_ui
} // namespace uwp_recomp
