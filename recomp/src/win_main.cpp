#include "xboxkrnl_win32.h"
#include "slate_engine.h"
#include "xbe_loader.h"
#include "d3d9_bridge.h"
#include "dsound_bridge.h"
#include "config_ui.h"
#include <shellapi.h>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <share.h>
#include <string>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")

static std::vector<FILE*>    g_logFiles;
static std::string           g_guiLogText;
static HWND                  g_hEditLog    = nullptr;
static HWND                  g_hMainWnd    = nullptr;
static HWND                  g_hRenderWnd  = nullptr;
static HWND                  g_hTopBar     = nullptr;
static HWND                  g_hStatusBar  = nullptr;
static bool                  g_fullscreen  = false;
static WINDOWPLACEMENT       g_savedPlace  = { sizeof(WINDOWPLACEMENT) };
static std::vector<uint32_t> g_gameFramebuffer(640 * 480, 0x00101420u);

static std::string           g_settingsIniPath;
static std::string           g_hdTexDirPath;
static std::string           g_dumpTexDirPath;
static std::string           g_screenshotsDirPath;
static config_ui::AppConfig  g_cfg{};
static config_ui::Callbacks  g_uiCb{};
static DWORD                 g_lastMouseMoveTick = 0;
static bool                  g_cursorHidden      = false;
static bool                  g_mouseLookCaptured = false;

static void ReleaseMouseLookCapture() {
    if (g_mouseLookCaptured) {
        g_mouseLookCaptured = false;
        xbe_loader::SetMouseLookCaptured(false);
        ClipCursor(nullptr);
        while (ShowCursor(TRUE) < 0) {}
        g_cursorHidden = false;
    }
}

static void LoadSettingsIni() {
    if (g_settingsIniPath.empty()) return;
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    uint32_t defAspect = (sh > 0 && sw * 9 > sh * 19) ? 2u : 1u;
    uint32_t defScale  = (sh >= 1400 && defAspect == 2u) ? 4u : 3u;

    g_cfg.resScale            = GetPrivateProfileIntA("Graphics", "ResolutionScale",     defScale,  g_settingsIniPath.c_str());
    g_cfg.msaa                = GetPrivateProfileIntA("Graphics", "MSAA",                4,         g_settingsIniPath.c_str());
    g_cfg.aniso               = GetPrivateProfileIntA("Graphics", "Anisotropic",         16,        g_settingsIniPath.c_str());
    g_cfg.aspectMode          = GetPrivateProfileIntA("Graphics", "AspectMode",          defAspect, g_settingsIniPath.c_str());
    g_cfg.targetFps           = GetPrivateProfileIntA("Graphics", "TargetFPS",           60,        g_settingsIniPath.c_str());
    g_cfg.atmosphericFog      = GetPrivateProfileIntA("Graphics", "AtmosphericFog",      1,         g_settingsIniPath.c_str()) != 0;
    g_cfg.softBloom           = GetPrivateProfileIntA("Graphics", "SoftBloom",           0,         g_settingsIniPath.c_str()) != 0;
    g_cfg.cinemaVignette      = GetPrivateProfileIntA("Graphics", "CinemaVignette",      0,         g_settingsIniPath.c_str()) != 0;
    g_cfg.crtScanlines        = GetPrivateProfileIntA("Graphics", "CrtScanlines",        0,         g_settingsIniPath.c_str()) != 0;
    g_cfg.sharpnessPercent    = GetPrivateProfileIntA("Graphics", "Sharpness",           35,        g_settingsIniPath.c_str());
    g_cfg.fovOffsetDeg        = static_cast<int>(GetPrivateProfileIntA("Graphics", "FovOffset", 0,  g_settingsIniPath.c_str()));
    g_cfg.gammaPercent        = GetPrivateProfileIntA("Graphics", "Gamma",               100,       g_settingsIniPath.c_str());
    g_cfg.brightnessPercent   = GetPrivateProfileIntA("Graphics", "Brightness",          100,       g_settingsIniPath.c_str());
    g_cfg.contrastPercent     = GetPrivateProfileIntA("Graphics", "Contrast",            100,       g_settingsIniPath.c_str());
    g_cfg.enableHDTextures    = GetPrivateProfileIntA("Textures", "EnableHDTextures",    1,         g_settingsIniPath.c_str()) != 0;
    g_cfg.autoResumeQuickSave = GetPrivateProfileIntA("Game",     "AutoResumeQuickSave", 0,         g_settingsIniPath.c_str()) != 0;
    g_cfg.hideMouseInactivity = GetPrivateProfileIntA("Game",     "HideMouseInactivity", 1,         g_settingsIniPath.c_str()) != 0;
    g_cfg.showStatusBar       = GetPrivateProfileIntA("Game",     "ShowStatusBar",       1,         g_settingsIniPath.c_str()) != 0;
    g_cfg.subtitlesEnabled    = GetPrivateProfileIntA("Game",     "Subtitles",           1,         g_settingsIniPath.c_str()) != 0;
    g_cfg.autoPauseUnfocused  = GetPrivateProfileIntA("Game",     "AutoPauseUnfocused",  0,         g_settingsIniPath.c_str()) != 0;
    g_cfg.buffyCostume        = GetPrivateProfileIntA("Game",     "BuffyCostume",        0,         g_settingsIniPath.c_str());
    g_cfg.cheatInfHealth      = GetPrivateProfileIntA("Cheats",   "InfHealth",           0,         g_settingsIniPath.c_str()) != 0;
    g_cfg.cheatInfPower       = GetPrivateProfileIntA("Cheats",   "InfPower",            0,         g_settingsIniPath.c_str()) != 0;
    g_cfg.cheatInfAmmo        = GetPrivateProfileIntA("Cheats",   "InfAmmo",             0,         g_settingsIniPath.c_str()) != 0;
    g_cfg.cheatOneHitDust     = GetPrivateProfileIntA("Cheats",   "OneHitDust",          0,         g_settingsIniPath.c_str()) != 0;
    g_cfg.masterVolume        = GetPrivateProfileIntA("Audio",    "MasterVolume",        100,       g_settingsIniPath.c_str());
    g_cfg.muteAudio           = GetPrivateProfileIntA("Audio",    "MuteAudio",           0,         g_settingsIniPath.c_str()) != 0;
    g_cfg.muteInBackground    = GetPrivateProfileIntA("Audio",    "MuteInBackground",    0,         g_settingsIniPath.c_str()) != 0;
    g_cfg.rumbleEnabled       = GetPrivateProfileIntA("Controls", "RumbleEnabled",       1,         g_settingsIniPath.c_str()) != 0;
    g_cfg.deadzonePercent     = GetPrivateProfileIntA("Controls", "DeadzonePercent",     15,        g_settingsIniPath.c_str());
    g_cfg.mouseLookEnabled    = GetPrivateProfileIntA("Controls", "MouseLook",           0,         g_settingsIniPath.c_str()) != 0;
    g_cfg.mouseSensitivity    = GetPrivateProfileIntA("Controls", "MouseSensitivity",    100,       g_settingsIniPath.c_str());
    g_cfg.invertCameraX       = GetPrivateProfileIntA("Controls", "InvertCameraX",       0,         g_settingsIniPath.c_str()) != 0;
    g_cfg.invertCameraY       = GetPrivateProfileIntA("Controls", "InvertCameraY",       0,         g_settingsIniPath.c_str()) != 0;
    g_cfg.activeSaveSlot      = GetPrivateProfileIntA("Game",     "ActiveSaveSlot",      1,         g_settingsIniPath.c_str());
    {
        static const int kDefKeys[10] = { 'W', 'S', 'A', 'D', VK_SPACE, VK_SHIFT, VK_CONTROL, 'E', 'Q', 'R' };
        for (int i = 0; i < 10; ++i) {
            char keyName[16]{};
            std::snprintf(keyName, sizeof(keyName), "Key%d", i);
            g_cfg.keybinds[i] = GetPrivateProfileIntA("Keybinds", keyName, kDefKeys[i], g_settingsIniPath.c_str());
        }
    }

    if (g_cfg.resScale < 1 || g_cfg.resScale > 4) g_cfg.resScale = defScale;
    if (g_cfg.aspectMode > 2) g_cfg.aspectMode = defAspect;
    if (g_cfg.msaa != 0 && g_cfg.msaa != 2 && g_cfg.msaa != 4) g_cfg.msaa = 4;
    if (g_cfg.aniso != 1 && g_cfg.aniso != 4 && g_cfg.aniso != 8 && g_cfg.aniso != 16) g_cfg.aniso = 16;
    if (g_cfg.targetFps != 0 && g_cfg.targetFps != 30 && g_cfg.targetFps != 60 && g_cfg.targetFps != 120 && g_cfg.targetFps != 165) g_cfg.targetFps = 60;
    if (g_cfg.masterVolume < 0 || g_cfg.masterVolume > 100) g_cfg.masterVolume = 100;
    if (g_cfg.deadzonePercent < 0 || g_cfg.deadzonePercent > 40) g_cfg.deadzonePercent = 15;
    if (g_cfg.mouseSensitivity < 25 || g_cfg.mouseSensitivity > 250) g_cfg.mouseSensitivity = 100;
    if (g_cfg.activeSaveSlot < 1 || g_cfg.activeSaveSlot > 5) g_cfg.activeSaveSlot = 1;
    if (g_cfg.fovOffsetDeg < -15 || g_cfg.fovOffsetDeg > 25) g_cfg.fovOffsetDeg = 0;
    if (g_cfg.sharpnessPercent < 0 || g_cfg.sharpnessPercent > 100) g_cfg.sharpnessPercent = 35;
    if (g_cfg.gammaPercent < 50 || g_cfg.gammaPercent > 200) g_cfg.gammaPercent = 100;
    if (g_cfg.brightnessPercent < 50 || g_cfg.brightnessPercent > 150) g_cfg.brightnessPercent = 100;
    if (g_cfg.contrastPercent < 50 || g_cfg.contrastPercent > 150) g_cfg.contrastPercent = 100;
    if (g_cfg.buffyCostume > 4) g_cfg.buffyCostume = 0;
}

static void SaveSettingsIni() {
    if (g_settingsIniPath.empty()) return;
    auto writeU = [&](const char* sec, const char* key, int val) {
        char buf[32]{};
        std::snprintf(buf, sizeof(buf), "%d", val);
        WritePrivateProfileStringA(sec, key, buf, g_settingsIniPath.c_str());
    };
    writeU("Graphics", "ResolutionScale",     static_cast<int>(g_cfg.resScale));
    writeU("Graphics", "MSAA",                static_cast<int>(g_cfg.msaa));
    writeU("Graphics", "Anisotropic",         static_cast<int>(g_cfg.aniso));
    writeU("Graphics", "AspectMode",          static_cast<int>(g_cfg.aspectMode));
    writeU("Graphics", "TargetFPS",           static_cast<int>(g_cfg.targetFps));
    writeU("Graphics", "AtmosphericFog",      g_cfg.atmosphericFog ? 1 : 0);
    writeU("Graphics", "SoftBloom",           g_cfg.softBloom ? 1 : 0);
    writeU("Graphics", "CinemaVignette",      g_cfg.cinemaVignette ? 1 : 0);
    writeU("Graphics", "CrtScanlines",        g_cfg.crtScanlines ? 1 : 0);
    writeU("Graphics", "Sharpness",           g_cfg.sharpnessPercent);
    writeU("Graphics", "FovOffset",           g_cfg.fovOffsetDeg);
    writeU("Graphics", "Gamma",               g_cfg.gammaPercent);
    writeU("Graphics", "Brightness",          g_cfg.brightnessPercent);
    writeU("Graphics", "Contrast",            g_cfg.contrastPercent);
    writeU("Textures", "EnableHDTextures",    g_cfg.enableHDTextures ? 1 : 0);
    writeU("Game",     "AutoResumeQuickSave", g_cfg.autoResumeQuickSave ? 1 : 0);
    writeU("Game",     "HideMouseInactivity", g_cfg.hideMouseInactivity ? 1 : 0);
    writeU("Game",     "ShowStatusBar",       g_cfg.showStatusBar ? 1 : 0);
    writeU("Game",     "Subtitles",           g_cfg.subtitlesEnabled ? 1 : 0);
    writeU("Game",     "AutoPauseUnfocused",  g_cfg.autoPauseUnfocused ? 1 : 0);
    writeU("Game",     "BuffyCostume",        static_cast<int>(g_cfg.buffyCostume));
    writeU("Cheats",   "InfHealth",           g_cfg.cheatInfHealth ? 1 : 0);
    writeU("Cheats",   "InfPower",            g_cfg.cheatInfPower ? 1 : 0);
    writeU("Cheats",   "InfAmmo",             g_cfg.cheatInfAmmo ? 1 : 0);
    writeU("Cheats",   "OneHitDust",          g_cfg.cheatOneHitDust ? 1 : 0);
    writeU("Audio",    "MasterVolume",        g_cfg.masterVolume);
    writeU("Audio",    "MuteAudio",           g_cfg.muteAudio ? 1 : 0);
    writeU("Audio",    "MuteInBackground",    g_cfg.muteInBackground ? 1 : 0);
    writeU("Controls", "RumbleEnabled",       g_cfg.rumbleEnabled ? 1 : 0);
    writeU("Controls", "DeadzonePercent",     g_cfg.deadzonePercent);
    writeU("Controls", "MouseLook",           g_cfg.mouseLookEnabled ? 1 : 0);
    writeU("Controls", "MouseSensitivity",    g_cfg.mouseSensitivity);
    writeU("Controls", "InvertCameraX",       g_cfg.invertCameraX ? 1 : 0);
    writeU("Controls", "InvertCameraY",       g_cfg.invertCameraY ? 1 : 0);
    writeU("Game",     "ActiveSaveSlot",      g_cfg.activeSaveSlot);
    for (int i = 0; i < 10; ++i) {
        char keyName[16]{};
        std::snprintf(keyName, sizeof(keyName), "Key%d", i);
        writeU("Keybinds", keyName, g_cfg.keybinds[i]);
    }
}

static void LayoutMainWindow(HWND hWnd);

static void ApplyAllConfig(const config_ui::AppConfig& cfg, bool relayout) {
    uint32_t prevCostume = xbe_loader::GetBuffyCostume();
    g_cfg = cfg;
    d3d9_bridge::RequestGraphicsSettings(g_cfg.resScale, g_cfg.msaa, g_cfg.aniso, g_cfg.aspectMode);
    d3d9_bridge::SetFogEnabled(g_cfg.atmosphericFog);
    d3d9_bridge::SetBloomEnabled(g_cfg.softBloom);
    d3d9_bridge::SetSharpnessPercent(g_cfg.sharpnessPercent);
    d3d9_bridge::SetPostEffects(g_cfg.cinemaVignette, g_cfg.crtScanlines);
    d3d9_bridge::SetColorCorrection(g_cfg.gammaPercent, g_cfg.brightnessPercent, g_cfg.contrastPercent);
    xbe_loader::SetFovOffsetDeg(g_cfg.fovOffsetDeg);
    d3d9_bridge::SetHDTexturesEnabled(g_cfg.enableHDTextures);
    d3d9_bridge::SetTextureDumpEnabled(g_cfg.dumpTextures);
    xbe_loader::SetAutoResumeQuickSave(g_cfg.autoResumeQuickSave);
    xbe_loader::SetSubtitlesEnabled(g_cfg.subtitlesEnabled);
    xbe_loader::SetAutoPauseUnfocused(g_cfg.autoPauseUnfocused);
    xbe_loader::SetCheats(g_cfg.cheatInfHealth, g_cfg.cheatInfPower, g_cfg.cheatInfAmmo, g_cfg.cheatOneHitDust);
    xbe_loader::SetBuffyCostume(g_cfg.buffyCostume);
    xbe_loader::SetTargetFps(g_cfg.targetFps);
    xbe_loader::SetRumbleEnabled(g_cfg.rumbleEnabled);
    xbe_loader::SetDeadzonePercent(g_cfg.deadzonePercent);
    xbe_loader::SetCameraControlSettings(g_cfg.mouseLookEnabled, g_cfg.mouseSensitivity, g_cfg.invertCameraX, g_cfg.invertCameraY);
    xbe_loader::SetKeybinds(g_cfg.keybinds);
    xbe_loader::SetActiveQuickSaveSlot(g_cfg.activeSaveSlot);
    dsound_bridge::SetMasterVolume(g_cfg.masterVolume);
    dsound_bridge::SetMuted(g_cfg.muteAudio);
    dsound_bridge::SetMuteInBackground(g_cfg.muteInBackground);
    SaveSettingsIni();
    if (g_hMainWnd && IsWindow(g_hMainWnd) && prevCostume != g_cfg.buffyCostume) {
        xbe_loader::RequestCostumeReloadInPlace();
    }
    if (relayout && g_hMainWnd && IsWindow(g_hMainWnd)) {
        LayoutMainWindow(g_hMainWnd);
        InvalidateRect(g_hMainWnd, nullptr, TRUE);
    }
    config_ui::RefreshBars();
}

static void LayoutMainWindow(HWND hWnd) {
    RECT rc{};
    GetClientRect(hWnd, &rc);
    int cw = rc.right - rc.left;
    int ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) return;

    int topH = g_fullscreen ? 0 : config_ui::kTopBarHeight;
    int botH = (g_fullscreen || !g_cfg.showStatusBar) ? 0 : config_ui::kStatusBarHeight;
    int availH = ch - topH - botH;
    if (availH < 120) availH = ch;

    if (g_hTopBar) {
        if (topH > 0) {
            ShowWindow(g_hTopBar, SW_SHOW);
            MoveWindow(g_hTopBar, 0, 0, cw, topH, TRUE);
        } else {
            ShowWindow(g_hTopBar, SW_HIDE);
        }
    }
    if (g_hStatusBar) {
        if (botH > 0) {
            ShowWindow(g_hStatusBar, SW_SHOW);
            MoveWindow(g_hStatusBar, 0, ch - botH, cw, botH, TRUE);
        } else {
            ShowWindow(g_hStatusBar, SW_HIDE);
        }
    }

    int gameAreaW = g_cfg.showDebugLog ? (cw - 440) : cw;
    if (gameAreaW < 320) gameAreaW = cw;

    // Fit 21:9 (43:18 = 3440x1440), 16:9, or 4:3 game viewport cleanly inside gameAreaW x availH
    int num = (g_cfg.aspectMode == 2) ? 43 : ((g_cfg.aspectMode == 1) ? 16 : 4);
    int den = (g_cfg.aspectMode == 2) ? 18 : ((g_cfg.aspectMode == 1) ? 9  : 3);
    int vpW = gameAreaW;
    int vpH = (vpW * den) / num;
    if (vpH > availH) {
        vpH = availH;
        vpW = (vpH * num) / den;
    }
    int vpX = (gameAreaW - vpW) / 2;
    int vpY = topH + (availH - vpH) / 2;

    if (g_hRenderWnd) {
        MoveWindow(g_hRenderWnd, vpX, vpY, vpW, vpH, TRUE);
    }
    if (g_hEditLog) {
        if (g_cfg.showDebugLog && cw > 500) {
            ShowWindow(g_hEditLog, SW_SHOW);
            MoveWindow(g_hEditLog, gameAreaW + 4, topH + 4, cw - gameAreaW - 8, availH - 8, TRUE);
        } else {
            ShowWindow(g_hEditLog, SW_HIDE);
        }
    }
}

static void ToggleFullscreen(HWND hWnd) {
    if (!g_fullscreen) {
        g_savedPlace.length = sizeof(g_savedPlace);
        GetWindowPlacement(hWnd, &g_savedPlace);
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfoA(MonitorFromWindow(hWnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLongA(hWnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            SetWindowPos(hWnd, HWND_TOP,
                         mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            g_fullscreen = true;
        }
    } else {
        SetWindowLongA(hWnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(hWnd, &g_savedPlace);
        SetWindowPos(hWnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = false;
        config_ui::ApplySystemWindowTheme(hWnd);
    }
    LayoutMainWindow(hWnd);
    config_ui::RefreshBars();
}

void LogPrintf(const char* fmt, ...) {
    char buf[2048]{};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    std::fputs(buf, stdout);
    std::fflush(stdout);

    for (FILE* lf : g_logFiles) {
        if (lf) {
            std::fputs(buf, lf);
            std::fflush(lf);
        }
    }

    for (const char* p = buf; *p; ++p) {
        if (*p == '\n' && (g_guiLogText.empty() || g_guiLogText.back() != '\r')) {
            g_guiLogText.push_back('\r');
        }
        g_guiLogText.push_back(*p);
    }
    if (g_hEditLog && IsWindow(g_hEditLog)) {
        SetWindowTextA(g_hEditLog, g_guiLogText.c_str());
        SendMessageA(g_hEditLog, EM_SETSEL, g_guiLogText.size(), g_guiLogText.size());
        SendMessageA(g_hEditLog, EM_SCROLLCARET, 0, 0);
    }
}

static bool FileExists(const std::string& p) {
    DWORD attr = GetFileAttributesA(p.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES) && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static std::string DetectProjectRoot(std::string& outExeDir) {
    char exePath[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string dir(exePath);
    size_t slash = dir.find_last_of("\\/");
    if (slash != std::string::npos) dir = dir.substr(0, slash);
    outExeDir = dir;

    std::string cur = dir;
    for (int i = 0; i < 4; ++i) {
        if (FileExists(cur + "\\decompile\\XGameBuffy_xbox.exe") ||
            FileExists(cur + "\\extracted\\Default.xbe")) {
            return cur;
        }
        size_t p = cur.find_last_of("\\/");
        if (p == std::string::npos) break;
        cur = cur.substr(0, p);
    }
    return dir;
}

// ============================================================================
// MULTITEX (Title.mtx) DXT1 Software Decoder
// ============================================================================
struct DecodedTexture {
    std::string           name;
    uint32_t              width  = 0;
    uint32_t              height = 0;
    std::vector<uint32_t> pixels;
};

static std::vector<DecodedTexture> g_titleTextures;

static void DecodeDxt1Image(const uint8_t* src, uint32_t width, uint32_t height, std::vector<uint32_t>& outPixels) {
    outPixels.assign(width * height, 0x00101018u);
    uint32_t blocksX = (width + 3) / 4;
    uint32_t blocksY = (height + 3) / 4;

    auto rgb565 = [](uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b) {
        r = static_cast<uint8_t>(((c >> 11) & 0x1F) * 255 / 31);
        g = static_cast<uint8_t>(((c >> 5)  & 0x3F) * 255 / 63);
        b = static_cast<uint8_t>((c         & 0x1F) * 255 / 31);
    };

    const uint8_t* p = src;
    for (uint32_t by = 0; by < blocksY; ++by) {
        for (uint32_t bx = 0; bx < blocksX; ++bx) {
            uint16_t c0   = *reinterpret_cast<const uint16_t*>(&p[0]);
            uint16_t c1   = *reinterpret_cast<const uint16_t*>(&p[2]);
            uint32_t bits = *reinterpret_cast<const uint32_t*>(&p[4]);
            p += 8;

            uint8_t r[4], g[4], b[4];
            rgb565(c0, r[0], g[0], b[0]);
            rgb565(c1, r[1], g[1], b[1]);
            if (c0 > c1) {
                r[2] = static_cast<uint8_t>((2 * r[0] + r[1]) / 3);
                g[2] = static_cast<uint8_t>((2 * g[0] + g[1]) / 3);
                b[2] = static_cast<uint8_t>((2 * b[0] + b[1]) / 3);
                r[3] = static_cast<uint8_t>((r[0] + 2 * r[1]) / 3);
                g[3] = static_cast<uint8_t>((g[0] + 2 * g[1]) / 3);
                b[3] = static_cast<uint8_t>((b[0] + 2 * b[1]) / 3);
            } else {
                r[2] = static_cast<uint8_t>((r[0] + r[1]) / 2);
                g[2] = static_cast<uint8_t>((g[0] + g[1]) / 2);
                b[2] = static_cast<uint8_t>((b[0] + b[1]) / 2);
                r[3] = 0; g[3] = 0; b[3] = 0;
            }

            for (uint32_t py = 0; py < 4; ++py) {
                for (uint32_t px = 0; px < 4; ++px) {
                    uint32_t idx = (bits >> (2 * (py * 4 + px))) & 0x3;
                    uint32_t x = bx * 4 + px;
                    uint32_t y = by * 4 + py;
                    if (x < width && y < height) {
                        outPixels[y * width + x] = (static_cast<uint32_t>(r[idx]) << 16) |
                                                   (static_cast<uint32_t>(g[idx]) << 8)  |
                                                   static_cast<uint32_t>(b[idx]);
                    }
                }
            }
        }
    }
}

static void LoadTitleMtx() {
    char mtxPath[512]{};
    xboxkrnl::TranslateXboxPath("D:\\media\\Title.mtx", mtxPath, sizeof(mtxPath));
    FILE* f = std::fopen(mtxPath, "rb");
    if (!f) return;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz < 0x224) {
        std::fclose(f);
        return;
    }
    std::vector<uint8_t> data(sz);
    std::fread(data.data(), 1, sz, f);
    std::fclose(f);

    if (std::memcmp(data.data(), "MULTITEX", 8) != 0) return;
    uint32_t numTex = *reinterpret_cast<uint32_t*>(&data[0x14]);

    for (uint32_t i = 0; i < numTex && (0x24 + (i + 1) * 0x80) <= data.size(); ++i) {
        const uint8_t* entry = &data[0x24 + i * 0x80];
        char nameBuf[33]{};
        std::memcpy(nameBuf, &entry[24], 32);
        nameBuf[32] = '\0';

        uint32_t w   = *reinterpret_cast<const uint32_t*>(&entry[88]);
        uint32_t h   = *reinterpret_cast<const uint32_t*>(&entry[92]);
        uint32_t off = *reinterpret_cast<const uint32_t*>(&entry[120]);

        if (off + 4 + (w * h / 2) <= data.size()) {
            DecodedTexture dt;
            dt.name   = nameBuf;
            dt.width  = w;
            dt.height = h;
            DecodeDxt1Image(&data[off + 4], w, h, dt.pixels);
            g_titleTextures.push_back(std::move(dt));
        }
    }

    // Seed the 640x480 game framebuffer with the Buffy title screen texture centered
    if (!g_titleTextures.empty()) {
        const auto& btex = g_titleTextures.back();
        int ox = (640 - static_cast<int>(btex.width)) / 2;
        int oy = (480 - static_cast<int>(btex.height)) / 2;
        for (uint32_t y = 0; y < btex.height; ++y) {
            for (uint32_t x = 0; x < btex.width; ++x) {
                int dx = ox + static_cast<int>(x);
                int dy = oy + static_cast<int>(y);
                if (dx >= 0 && dx < 640 && dy >= 0 && dy < 480) {
                    g_gameFramebuffer[dy * 640 + dx] = btex.pixels[y * btex.width + x];
                }
            }
        }
    }
}

// ============================================================================
// Win32 Live Game Viewport + Console Window
// ============================================================================
static void EnsureCursorVisible(bool visible) {
    if (visible && g_cursorHidden) {
        ShowCursor(TRUE);
        g_cursorHidden = false;
    } else if (!visible && !g_cursorHidden) {
        ShowCursor(FALSE);
        g_cursorHidden = true;
    }
}

static LRESULT CALLBACK BuffyWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            config_ui::ApplySystemWindowTheme(hWnd);
            g_hTopBar    = config_ui::CreateTopMenuBar(hWnd, &g_cfg, &g_uiCb);
            g_hStatusBar = config_ui::CreateStatusBar(hWnd, &g_cfg, &g_uiCb);
            g_hRenderWnd = CreateWindowExA(
                0, "STATIC", "",
                WS_CHILD | WS_VISIBLE | SS_BLACKRECT,
                0, 0, 1280, 960,
                hWnd, nullptr, GetModuleHandleA(nullptr), nullptr
            );
            g_hEditLog = CreateWindowExA(
                WS_EX_CLIENTEDGE, "EDIT", g_guiLogText.c_str(),
                WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                0, 0, 420, 600,
                hWnd, nullptr, GetModuleHandleA(nullptr), nullptr
            );
            HFONT hFont = CreateFontA(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                      ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
            if (hFont) SendMessageA(g_hEditLog, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
            g_lastMouseMoveTick = GetTickCount();
            SetTimer(hWnd, 2, 500, nullptr);
            LayoutMainWindow(hWnd);
            return 0;
        }
        case WM_SETTINGCHANGE:
            config_ui::ApplySystemWindowTheme(hWnd);
            config_ui::RefreshBars();
            return 0;
        case WM_SIZE: {
            LayoutMainWindow(hWnd);
            InvalidateRect(hWnd, nullptr, TRUE);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if ((g_cfg.mouseLookEnabled || d3d9_bridge::IsPhotoMode()) && g_hRenderWnd && !g_mouseLookCaptured) {
                POINT pt{};
                GetCursorPos(&pt);
                RECT rcRender{};
                if (GetWindowRect(g_hRenderWnd, &rcRender) && PtInRect(&rcRender, pt)) {
                    g_mouseLookCaptured = true;
                    xbe_loader::SetMouseLookCaptured(true);
                    ClipCursor(&rcRender);
                    int cx = (rcRender.left + rcRender.right) / 2;
                    int cy = (rcRender.top  + rcRender.bottom) / 2;
                    SetCursorPos(cx, cy);
                    EnsureCursorVisible(false);
                }
            }
            break;
        }
        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE) ReleaseMouseLookCapture();
            break;
        case WM_KILLFOCUS:
            ReleaseMouseLookCapture();
            break;
        case WM_SYSKEYDOWN:
            if (wParam == VK_MENU) ReleaseMouseLookCapture();
            break;
        case WM_MOUSEMOVE:
            if (g_mouseLookCaptured && (g_cfg.mouseLookEnabled || d3d9_bridge::IsPhotoMode()) && g_hRenderWnd && GetForegroundWindow() == hWnd) {
                RECT rcRender{};
                if (GetWindowRect(g_hRenderWnd, &rcRender)) {
                    int cx = (rcRender.left + rcRender.right) / 2;
                    int cy = (rcRender.top  + rcRender.bottom) / 2;
                    POINT pt{};
                    GetCursorPos(&pt);
                    int dx = pt.x - cx;
                    int dy = pt.y - cy;
                    if (dx != 0 || dy != 0) {
                        if (d3d9_bridge::IsPhotoMode()) {
                            d3d9_bridge::AddPhotoModeMouseDelta(dx, dy);
                        } else {
                            xbe_loader::AddMouseLookDelta(dx, dy);
                        }
                        SetCursorPos(cx, cy);
                    }
                }
                return 0;
            }
            g_lastMouseMoveTick = GetTickCount();
            EnsureCursorVisible(true);
            break;
        case WM_TIMER:
            if (wParam == 2) {
                if (g_cfg.hideMouseInactivity && GetForegroundWindow() == hWnd) {
                    POINT pt{};
                    GetCursorPos(&pt);
                    RECT rcRender{};
                    if (g_hRenderWnd && GetWindowRect(g_hRenderWnd, &rcRender) && PtInRect(&rcRender, pt)) {
                        if (GetTickCount() - g_lastMouseMoveTick >= 2500) {
                            EnsureCursorVisible(false);
                        }
                    } else {
                        EnsureCursorVisible(true);
                    }
                } else {
                    EnsureCursorVisible(true);
                }
            }
            return 0;
        case WM_KEYDOWN: {
            if (wParam == VK_ESCAPE) {
                if (d3d9_bridge::IsPhotoMode()) {
                    d3d9_bridge::SetPhotoMode(false);
                    ReleaseMouseLookCapture();
                    xbe_loader::ShowStatusNotice("Photo Mode OFF");
                    config_ui::RefreshBars();
                    return 0;
                }
                if (g_mouseLookCaptured) {
                    ReleaseMouseLookCapture();
                    return 0;
                }
                DestroyWindow(hWnd);
                return 0;
            }
            if (wParam == VK_F10) {
                bool next = !d3d9_bridge::IsPhotoMode();
                d3d9_bridge::SetPhotoMode(next);
                if (!next) ReleaseMouseLookCapture();
                xbe_loader::ShowStatusNotice(next ? "Photo Mode ON (WASD+Q/E+Click Mouse, F12 Shot)" : "Photo Mode OFF");
                config_ui::RefreshBars();
                return 0;
            }
            if (wParam == VK_F12) {
                d3d9_bridge::RequestScreenshot(g_screenshotsDirPath.c_str());
                return 0;
            }
            if (wParam == VK_F1) {
                ReleaseMouseLookCapture();
                config_ui::OpenConfigDialog(hWnd, &g_cfg, &g_uiCb, 0);
                return 0;
            }
            if ((GetKeyState(VK_SHIFT) & 0x8000) && wParam >= '1' && wParam <= '5') {
                g_cfg.activeSaveSlot = static_cast<int>(wParam - '0');
                xbe_loader::SetActiveQuickSaveSlot(g_cfg.activeSaveSlot);
                SaveSettingsIni();
                config_ui::RefreshBars();
                return 0;
            }
            if (wParam == VK_F11) {
                ToggleFullscreen(hWnd);
                return 0;
            }
            if (wParam == VK_F2) {
                g_cfg.showDebugLog = !g_cfg.showDebugLog;
                LayoutMainWindow(hWnd);
                InvalidateRect(hWnd, nullptr, TRUE);
                config_ui::RefreshBars();
                return 0;
            }
            if (wParam == VK_F5) {
                xbe_loader::RequestQuickSave();
                return 0;
            }
            if (wParam == VK_F6) {
                xbe_loader::RequestQuickLoad();
                return 0;
            }
            if (wParam == VK_F7) {
                d3d9_bridge::RequestFrameTrace();
                return 0;
            }
            if (wParam == VK_F8) {
                g_cfg.dumpTextures = !g_cfg.dumpTextures;
                d3d9_bridge::SetTextureDumpEnabled(g_cfg.dumpTextures);
                config_ui::RefreshBars();
                return 0;
            }
            if (wParam == VK_F9) {
                d3d9_bridge::ReloadHDTextures();
                config_ui::RefreshBars();
                return 0;
            }
            break;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc{};
            GetClientRect(hWnd, &rc);
            HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(hdc, &rc, bg);
            DeleteObject(bg);
            EndPaint(hWnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            KillTimer(hWnd, 2);
            EnsureCursorVisible(true);
            config_ui::CloseConfigDialog();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

extern "C" __declspec(dllexport) int __cdecl BuffyRuntimeMain() {
    std::set_terminate([]() {
        LogPrintf("[CRT-ABORT] std::terminate() called!\n");
        std::_Exit(99);
    });
    _set_invalid_parameter_handler([](const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
        LogPrintf("[CRT-ABORT] _invalid_parameter_handler triggered!\n");
    });
    std::signal(SIGABRT, [](int) {
        LogPrintf("[CRT-ABORT] SIGABRT triggered!\n");
    });
    bool headless = false;
    const char* cmdLine = GetCommandLineA();
    if (cmdLine && std::strstr(cmdLine, "--headless") != nullptr) {
        headless = true;
    }

    std::string exeDir;
    std::string rootDir       = DetectProjectRoot(exeDir);
    std::string xbePath       = rootDir + "\\extracted\\Default.xbe";
    std::string funcTsvPath   = rootDir + "\\decompile\\functions.tsv";
    std::string extractedRoot = rootDir + "\\extracted";
    std::string cacheRoot     = rootDir + "\\recomp\\cache";
    std::string savesRoot     = rootDir + "\\recomp\\saves";
    g_settingsIniPath         = rootDir + "\\recomp\\settings.ini";
    g_hdTexDirPath            = rootDir + "\\recomp\\hd_textures";
    g_dumpTexDirPath          = rootDir + "\\recomp\\dump_textures";
    g_screenshotsDirPath      = rootDir + "\\recomp\\screenshots";

    LoadSettingsIni();
    if (cmdLine) {
        if (const char* s = std::strstr(cmdLine, "--scale=")) {
            int sc = std::atoi(s + 8);
            if (sc >= 1 && sc <= 4) g_cfg.resScale = static_cast<uint32_t>(sc);
        }
        if (std::strstr(cmdLine, "--dump-textures") != nullptr) {
            g_cfg.dumpTextures = true;
        }
    }

    g_uiCb.onApplyConfig      = &ApplyAllConfig;
    g_uiCb.onQuickSave        = []() { xbe_loader::RequestQuickSave(0); };
    g_uiCb.onQuickLoad        = []() { xbe_loader::RequestQuickLoad(0); };
    g_uiCb.onQuickSaveSlot    = [](int slot) {
        g_cfg.activeSaveSlot = slot;
        xbe_loader::SetActiveQuickSaveSlot(slot);
        SaveSettingsIni();
        xbe_loader::RequestQuickSave(slot);
    };
    g_uiCb.onQuickLoadSlot    = [](int slot) {
        g_cfg.activeSaveSlot = slot;
        xbe_loader::SetActiveQuickSaveSlot(slot);
        SaveSettingsIni();
        xbe_loader::RequestQuickLoad(slot);
    };
    g_uiCb.onJumpToLevel      = [](const char* mapName) {
        ReleaseMouseLookCapture();
        xbe_loader::RequestJumpToLevel(mapName);
    };
    g_uiCb.onToggleFullscreen = []() { if (g_hMainWnd) ToggleFullscreen(g_hMainWnd); };
    g_uiCb.onReloadHD         = []() { d3d9_bridge::ReloadHDTextures(); config_ui::RefreshBars(); };
    g_uiCb.onOpenHDFolder     = []() {
        CreateDirectoryA(g_hdTexDirPath.c_str(), nullptr);
        ShellExecuteA(g_hMainWnd, "open", g_hdTexDirPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    };
    g_uiCb.onOpenDumpFolder   = []() {
        CreateDirectoryA(g_dumpTexDirPath.c_str(), nullptr);
        ShellExecuteA(g_hMainWnd, "open", g_dumpTexDirPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    };
    g_uiCb.onFrameTrace       = []() { d3d9_bridge::RequestFrameTrace(); };
    g_uiCb.onTogglePhotoMode  = []() {
        bool next = !d3d9_bridge::IsPhotoMode();
        d3d9_bridge::SetPhotoMode(next);
        if (!next) ReleaseMouseLookCapture();
        xbe_loader::ShowStatusNotice(next ? "Photo Mode ON (WASD+Q/E+Click Mouse, F12 Shot)" : "Photo Mode OFF");
        config_ui::RefreshBars();
    };
    g_uiCb.onTakeScreenshot   = []() { d3d9_bridge::RequestScreenshot(g_screenshotsDirPath.c_str()); };
    g_uiCb.onExitApp          = []() { if (g_hMainWnd) DestroyWindow(g_hMainWnd); };

    d3d9_bridge::SetTexturePackDirs(g_hdTexDirPath.c_str(), g_dumpTexDirPath.c_str());
    ApplyAllConfig(g_cfg, false);

    xboxkrnl::InitVirtualDrives(extractedRoot.c_str(), cacheRoot.c_str(), savesRoot.c_str());

    std::string rootLogPath = rootDir + "\\buffy_recomp.log";
    std::string exeLogPath  = exeDir  + "\\buffy_recomp.log";
    std::string loadLogPath = savesRoot + "\\Load.log";

    FILE* fRootLog = _fsopen(rootLogPath.c_str(), "w", _SH_DENYNO);
    if (fRootLog) g_logFiles.push_back(fRootLog);
    if (_stricmp(rootLogPath.c_str(), exeLogPath.c_str()) != 0) {
        FILE* fExeLog = _fsopen(exeLogPath.c_str(), "w", _SH_DENYNO);
        if (fExeLog) g_logFiles.push_back(fExeLog);
    }
    FILE* fLoadLog = _fsopen(loadLogPath.c_str(), "w", _SH_DENYNO);
    if (fLoadLog) g_logFiles.push_back(fLoadLog);

    std::time_t now = std::time(nullptr);
    LogPrintf("====================================================================\n");
    LogPrintf(" Buffy the Vampire Slayer (Xbox -> Win32 In-Process Execution Runtime)\n");
    LogPrintf(" Title ID: EA-018 (0x45410012) | Engine: The Collective Slate Engine\n");
    LogPrintf(" Original Build: XDK 1.0.4432.1 (Wed Jul 17 12:44:45 2002)\n");
    LogPrintf(" Host Exe Base:    0x%08X\n", static_cast<uint32_t>(reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr))));
    LogPrintf(" Log File (Root):  %s\n", rootLogPath.c_str());
    LogPrintf(" Log File (Exe):   %s\n", exeLogPath.c_str());
    LogPrintf(" Log File (Title): %s (T:\\Load.log)\n", loadLogPath.c_str());
    LogPrintf(" Started At:       %s", std::ctime(&now));
    LogPrintf("====================================================================\n\n");

    LogPrintf("[VFS] Mounted D:\\ -> %s\n", extractedRoot.c_str());
    LogPrintf("[VFS] Mounted Z:\\ -> %s\n", cacheRoot.c_str());
    LogPrintf("[VFS] Mounted T:\\ & U:\\ -> %s\n\n", savesRoot.c_str());

    LoadTitleMtx();

    if (!headless) {
        WNDCLASSEXA wc{};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = BuffyWndProc;
        wc.hInstance     = GetModuleHandleA(nullptr);
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = "BuffyWin32RecompWnd";
        RegisterClassExA(&wc);

        int screenH = GetSystemMetrics(SM_CYSCREEN);
        int initW = (g_cfg.aspectMode == 2) ? 1720 : ((g_cfg.aspectMode == 1) ? 1280 : ((screenH >= 1020) ? 1280 : 1024));
        int initH = (g_cfg.aspectMode == 2) ? 720  : ((g_cfg.aspectMode == 1) ? 720  : ((screenH >= 1020) ? 960  : 768));
        initH += config_ui::kTopBarHeight + (g_cfg.showStatusBar ? config_ui::kStatusBarHeight : 0);
        RECT adj{0, 0, initW, initH};
        AdjustWindowRect(&adj, WS_OVERLAPPEDWINDOW, FALSE);

        g_hMainWnd = CreateWindowExA(
            0, wc.lpszClassName,
            "Buffy the Vampire Slayer",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            CW_USEDEFAULT, CW_USEDEFAULT, adj.right - adj.left, adj.bottom - adj.top,
            nullptr, nullptr, wc.hInstance, nullptr
        );
        config_ui::ApplySystemWindowTheme(g_hMainWnd);
        UpdateWindow(g_hMainWnd);
    }

    xbe_loader::SetPresentationWindow(g_hRenderWnd ? g_hRenderWnd : g_hMainWnd, g_gameFramebuffer.data());
    xbe_loader::LoadSymbolTable(funcTsvPath.c_str());

    if (xbe_loader::LoadAndBridgeDefaultXbe(xbePath.c_str())) {
        xbe_loader::ExecuteXboxMain();
    }

    if (!headless && g_hMainWnd && IsWindow(g_hMainWnd)) {
        MSG msg{};
        while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    config_ui::Shutdown();
    for (FILE* lf : g_logFiles) {
        if (lf) std::fclose(lf);
    }
    g_logFiles.clear();
    return 0;
}

int main(int /*argc*/, char** /*argv*/) {
    return BuffyRuntimeMain();
}
