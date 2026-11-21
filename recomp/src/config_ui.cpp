#include "config_ui.h"
#include "d3d9_bridge.h"
#include "dsound_bridge.h"
#include "xbe_loader.h"
#include <dwmapi.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using std::min;
using std::max;
#include <objidl.h>
#include <gdiplus.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace config_ui {

// ============================================================================
// Theme Palette (System Dark / Light Mode + Lichii-style Pink/Crimson Accent)
// ============================================================================
struct Palette {
    Gdiplus::Color winBg;
    Gdiplus::Color sideBg;
    Gdiplus::Color barBg;
    Gdiplus::Color ctrlBg;
    Gdiplus::Color ctrlHover;
    Gdiplus::Color border;
    Gdiplus::Color text;
    Gdiplus::Color textDim;
    Gdiplus::Color accent;
    Gdiplus::Color accentBg;
    COLORREF       dwmCaption;
    COLORREF       dwmBorder;
};

static bool g_darkMode = true;
static ULONG_PTR g_uiGdipToken = 0;

static void EnsureGdip() {
    if (g_uiGdipToken == 0) {
        Gdiplus::GdiplusStartupInput si{};
        Gdiplus::GdiplusStartup(&g_uiGdipToken, &si, nullptr);
    }
}

bool IsSystemDarkMode() {
    DWORD val = 0, sz = sizeof(val);
    if (RegGetValueA(HKEY_CURRENT_USER,
                     "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     "AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &val, &sz) == ERROR_SUCCESS) {
        return val == 0;
    }
    return true;
}

static Palette GetPalette() {
    g_darkMode = IsSystemDarkMode();
    if (g_darkMode) {
        return Palette{
            Gdiplus::Color(255, 22, 22, 27),    // winBg
            Gdiplus::Color(255, 16, 16, 20),    // sideBg
            Gdiplus::Color(255, 18, 18, 22),    // barBg
            Gdiplus::Color(255, 34, 34, 42),    // ctrlBg
            Gdiplus::Color(255, 48, 48, 58),    // ctrlHover
            Gdiplus::Color(255, 46, 46, 56),    // border
            Gdiplus::Color(255, 236, 236, 242), // text
            Gdiplus::Color(255, 148, 148, 162), // textDim
            Gdiplus::Color(255, 233, 30, 99),   // accent (#E91E63)
            Gdiplus::Color(255, 62, 24, 40),    // accentBg
            RGB(18, 18, 22),                    // dwmCaption
            RGB(38, 38, 48)                     // dwmBorder
        };
    }
    return Palette{
        Gdiplus::Color(255, 245, 245, 248),
        Gdiplus::Color(255, 234, 234, 240),
        Gdiplus::Color(255, 238, 238, 244),
        Gdiplus::Color(255, 225, 225, 232),
        Gdiplus::Color(255, 212, 212, 222),
        Gdiplus::Color(255, 198, 198, 210),
        Gdiplus::Color(255, 24, 24, 30),
        Gdiplus::Color(255, 100, 100, 114),
        Gdiplus::Color(255, 216, 27, 96),
        Gdiplus::Color(255, 248, 216, 228),
        RGB(238, 238, 244),
        RGB(198, 198, 210)
    };
}

void ApplySystemWindowTheme(HWND hWnd) {
    if (!hWnd || !IsWindow(hWnd)) return;
    Palette pal = GetPalette();
    BOOL dark = g_darkMode ? TRUE : FALSE;
    // DWMWA_USE_IMMERSIVE_DARK_MODE = 20 (Win10 2004+ / Win11), fallback 19
    DwmSetWindowAttribute(hWnd, 20, &dark, sizeof(dark));
    DwmSetWindowAttribute(hWnd, 19, &dark, sizeof(dark));
    // DWMWA_BORDER_COLOR = 34, DWMWA_CAPTION_COLOR = 35 (Windows 11)
    DwmSetWindowAttribute(hWnd, 34, &pal.dwmBorder, sizeof(pal.dwmBorder));
    DwmSetWindowAttribute(hWnd, 35, &pal.dwmCaption, sizeof(pal.dwmCaption));
}

// ============================================================================
// GDI+ Rounded Rect & Text Drawing Helpers
// ============================================================================
static void AddRoundedRectPath(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r) {
    float d = r * 2.0f;
    if (d > w) d = w;
    if (d > h) d = h;
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

static void FillRoundRect(Gdiplus::Graphics& g, float x, float y, float w, float h, float r, const Gdiplus::Color& c) {
    Gdiplus::GraphicsPath path;
    AddRoundedRectPath(path, x, y, w, h, r);
    Gdiplus::SolidBrush br(c);
    g.FillPath(&br, &path);
}

static void DrawRoundRect(Gdiplus::Graphics& g, float x, float y, float w, float h, float r, const Gdiplus::Color& c, float width = 1.0f) {
    Gdiplus::GraphicsPath path;
    AddRoundedRectPath(path, x, y, w, h, r);
    Gdiplus::Pen pen(c, width);
    g.DrawPath(&pen, &path);
}

static std::wstring ToWide(const char* s) {
    if (!s || !s[0]) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return L"";
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

static void DrawTextStr(Gdiplus::Graphics& g, const char* txt, const Gdiplus::RectF& rc,
                        const Gdiplus::Color& col, float sizePt = 9.5f, bool bold = false,
                        Gdiplus::StringAlignment hAlign = Gdiplus::StringAlignmentNear) {
    Gdiplus::FontFamily ff(L"Segoe UI");
    Gdiplus::Font font(&ff, sizePt, bold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
    Gdiplus::StringFormat sf;
    sf.SetAlignment(hAlign);
    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    sf.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    sf.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    Gdiplus::SolidBrush br(col);
    std::wstring w = ToWide(txt);
    g.DrawString(w.c_str(), -1, &font, rc, &sf, &br);
}

static void ComputeRes(uint32_t scale, uint32_t aspect, uint32_t& outW, uint32_t& outH) {
    if (aspect == 2) {
        if (scale == 1)      { outW = 1148; outH = 480;  }
        else if (scale == 2) { outW = 1720; outH = 720;  }
        else if (scale == 3) { outW = 2580; outH = 1080; }
        else                 { outW = 3440; outH = 1440; }
    } else if (aspect == 1) {
        if (scale == 1)      { outW = 854;  outH = 480;  }
        else if (scale == 2) { outW = 1280; outH = 720;  }
        else if (scale == 3) { outW = 1920; outH = 1080; }
        else                 { outW = 2560; outH = 1440; }
    } else {
        outW = 640u * scale;
        outH = 480u * scale;
    }
}

// ============================================================================
// Shared UI Pointers
// ============================================================================
static AppConfig* g_cfg        = nullptr;
static Callbacks  g_cb{};
static HWND       g_hTopBar    = nullptr;
static HWND       g_hStatusBar = nullptr;
static HWND       g_hCfgWnd    = nullptr;

void RefreshBars() {
    if (g_hTopBar && IsWindow(g_hTopBar))       InvalidateRect(g_hTopBar, nullptr, FALSE);
    if (g_hStatusBar && IsWindow(g_hStatusBar)) InvalidateRect(g_hStatusBar, nullptr, FALSE);
}

// ============================================================================
// 1. Custom Dark Top Menu Bar (Game | Config | View)
// ============================================================================
struct TopMenuItem {
    const char* label;
    RECT        rc;
};
static TopMenuItem g_topItems[3] = {
    { "Game",   {6,   2, 62,  26} },
    { "Config", {66,  2, 130, 26} },
    { "View",   {134, 2, 188, 26} }
};
static int g_topHover = -1;

static LRESULT CALLBACK TopBarWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_MOUSEMOVE: {
            int x = static_cast<short>(LOWORD(lParam));
            int y = static_cast<short>(HIWORD(lParam));
            int h = -1;
            for (int i = 0; i < 3; ++i) {
                if (x >= g_topItems[i].rc.left && x < g_topItems[i].rc.right &&
                    y >= g_topItems[i].rc.top  && y < g_topItems[i].rc.bottom) {
                    h = i;
                    break;
                }
            }
            if (h != g_topHover) {
                g_topHover = h;
                InvalidateRect(hWnd, nullptr, FALSE);
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hWnd, 0 };
                TrackMouseEvent(&tme);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (g_topHover != -1) {
                g_topHover = -1;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP: {
            int x = static_cast<short>(LOWORD(lParam));
            int y = static_cast<short>(HIWORD(lParam));
            int clicked = -1;
            for (int i = 0; i < 3; ++i) {
                if (x >= g_topItems[i].rc.left && x < g_topItems[i].rc.right &&
                    y >= g_topItems[i].rc.top  && y < g_topItems[i].rc.bottom) {
                    clicked = i;
                    break;
                }
            }
            if (clicked == 1) {
                OpenConfigDialog(GetParent(hWnd), g_cfg, &g_cb, 0);
            } else if (clicked == 0 || clicked == 2) {
                POINT pt{ g_topItems[clicked].rc.left, g_topItems[clicked].rc.bottom };
                ClientToScreen(hWnd, &pt);
                HMENU hPop = CreatePopupMenu();
                if (clicked == 0) {
                    AppendMenuA(hPop, MF_STRING, 101, "Quick Save Exact Spot\tF5");
                    AppendMenuA(hPop, MF_STRING, 102, "Quick Load Last Spot\tF6");
                    AppendMenuA(hPop, MF_STRING | ((g_cfg && g_cfg->autoResumeQuickSave) ? MF_CHECKED : 0),
                                103, "Auto-Resume Quick Save on Launch");
                    AppendMenuA(hPop, MF_SEPARATOR, 0, nullptr);
                    AppendMenuA(hPop, MF_STRING, 104, "Configuration...\tF1");
                    AppendMenuA(hPop, MF_SEPARATOR, 0, nullptr);
                    AppendMenuA(hPop, MF_STRING, 105, "Exit\tEsc");
                } else {
                    AppendMenuA(hPop, MF_STRING, 201, "Fullscreen\tF11");
                    AppendMenuA(hPop, MF_STRING | ((g_cfg && g_cfg->showStatusBar) ? MF_CHECKED : 0),
                                202, "Show Bottom Status Bar");
                    AppendMenuA(hPop, MF_STRING | ((g_cfg && g_cfg->showDebugLog) ? MF_CHECKED : 0),
                                203, "Debug Telemetry Log\tF2");
                    AppendMenuA(hPop, MF_SEPARATOR, 0, nullptr);
                    AppendMenuA(hPop, MF_STRING, 204, "Trace 1 Frame to Log\tF7");
                }
                UINT cmd = TrackPopupMenu(hPop, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD, pt.x, pt.y, 0, hWnd, nullptr);
                DestroyMenu(hPop);
                if (cmd == 101 && g_cb.onQuickSave) g_cb.onQuickSave();
                else if (cmd == 102 && g_cb.onQuickLoad) g_cb.onQuickLoad();
                else if (cmd == 103 && g_cfg && g_cb.onApplyConfig) {
                    g_cfg->autoResumeQuickSave = !g_cfg->autoResumeQuickSave;
                    g_cb.onApplyConfig(*g_cfg, false);
                } else if (cmd == 104) OpenConfigDialog(GetParent(hWnd), g_cfg, &g_cb, 0);
                else if (cmd == 105 && g_cb.onExitApp) g_cb.onExitApp();
                else if (cmd == 201 && g_cb.onToggleFullscreen) g_cb.onToggleFullscreen();
                else if (cmd == 202 && g_cfg && g_cb.onApplyConfig) {
                    g_cfg->showStatusBar = !g_cfg->showStatusBar;
                    g_cb.onApplyConfig(*g_cfg, true);
                } else if (cmd == 203 && g_cfg && g_cb.onApplyConfig) {
                    g_cfg->showDebugLog = !g_cfg->showDebugLog;
                    g_cb.onApplyConfig(*g_cfg, true);
                } else if (cmd == 204 && g_cb.onFrameTrace) g_cb.onFrameTrace();
            }
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            EnsureGdip();
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc{};
            GetClientRect(hWnd, &rc);
            int w = rc.right - rc.left, h = rc.bottom - rc.top;
            if (w > 0 && h > 0) {
                HDC memDC = CreateCompatibleDC(hdc);
                HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
                HGDIOBJ old = SelectObject(memDC, bmp);
                {
                    Gdiplus::Graphics g(memDC);
                    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
                    Palette pal = GetPalette();
                    Gdiplus::SolidBrush bg(pal.barBg);
                    g.FillRectangle(&bg, 0, 0, w, h);
                    Gdiplus::Pen line(pal.border, 1.0f);
                    g.DrawLine(&line, 0, h - 1, w, h - 1);

                    for (int i = 0; i < 3; ++i) {
                        const RECT& ir = g_topItems[i].rc;
                        if (g_topHover == i) {
                            FillRoundRect(g, static_cast<float>(ir.left), static_cast<float>(ir.top),
                                          static_cast<float>(ir.right - ir.left), static_cast<float>(ir.bottom - ir.top),
                                          5.0f, pal.ctrlHover);
                        }
                        DrawTextStr(g, g_topItems[i].label,
                                    Gdiplus::RectF(static_cast<float>(ir.left), static_cast<float>(ir.top),
                                                   static_cast<float>(ir.right - ir.left), static_cast<float>(ir.bottom - ir.top)),
                                    pal.text, 9.0f, false, Gdiplus::StringAlignmentCenter);
                    }
                }
                BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
                SelectObject(memDC, old);
                DeleteObject(bmp);
                DeleteDC(memDC);
            }
            EndPaint(hWnd, &ps);
            return 0;
        }
    }
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

// ============================================================================
// 2. Clickable Color-Coded Bottom Status Bar
// ============================================================================
struct StatusPill {
    int            id;
    std::string    text;
    Gdiplus::Color bg;
    Gdiplus::Color fg;
    RECT           rc;
};
static std::vector<StatusPill> g_pills;
static int g_pillHover = -1;

static void BuildStatusPills(std::vector<StatusPill>& out) {
    out.clear();
    if (!g_cfg) return;

    uint32_t rw = 0, rh = 0;
    ComputeRes(g_cfg->resScale, g_cfg->aspectMode, rw, rh);
    const char* asp = (g_cfg->aspectMode == 2) ? "21:9" : ((g_cfg->aspectMode == 1) ? "16:9" : "4:3");

    char resBuf[32]{}, msaaBuf[24]{}, afBuf[24]{}, volBuf[32]{};
    std::snprintf(resBuf, sizeof(resBuf), "%ux%u", rw, rh);
    if (g_cfg->msaa > 0) std::snprintf(msaaBuf, sizeof(msaaBuf), "%ux MSAA", g_cfg->msaa);
    else std::snprintf(msaaBuf, sizeof(msaaBuf), "MSAA OFF");
    if (g_cfg->aniso > 1) std::snprintf(afBuf, sizeof(afBuf), "%ux AF", g_cfg->aniso);
    else std::snprintf(afBuf, sizeof(afBuf), "AF OFF");
    if (g_cfg->muteAudio) std::snprintf(volBuf, sizeof(volBuf), "VOL: MUTE");
    else std::snprintf(volBuf, sizeof(volBuf), "VOL: %d%%", g_cfg->masterVolume);

    out.push_back({ 0, "D3D9", Gdiplus::Color(255, 68, 26, 36), Gdiplus::Color(255, 255, 118, 148), {} });
    out.push_back({ 1, asp,    Gdiplus::Color(255, 24, 46, 76), Gdiplus::Color(255, 112, 186, 255), {} });
    out.push_back({ 7, resBuf, Gdiplus::Color(255, 26, 54, 72), Gdiplus::Color(255, 130, 210, 255), {} });
    out.push_back({ 2, msaaBuf, Gdiplus::Color(255, 20, 62, 66), Gdiplus::Color(255, 96, 224, 224), {} });
    out.push_back({ 3, afBuf, Gdiplus::Color(255, 52, 34, 78), Gdiplus::Color(255, 192, 140, 255), {} });
    out.push_back({ 4, g_cfg->atmosphericFog ? "FOG: ON" : "FOG: OFF",
                    g_cfg->atmosphericFog ? Gdiplus::Color(255, 72, 54, 20) : Gdiplus::Color(255, 38, 38, 44),
                    g_cfg->atmosphericFog ? Gdiplus::Color(255, 255, 204, 92) : Gdiplus::Color(255, 140, 140, 152), {} });
    out.push_back({ 5, g_cfg->enableHDTextures ? "HD TEX: ON" : "HD TEX: OFF",
                    g_cfg->enableHDTextures ? Gdiplus::Color(255, 24, 64, 38) : Gdiplus::Color(255, 38, 38, 44),
                    g_cfg->enableHDTextures ? Gdiplus::Color(255, 108, 232, 144) : Gdiplus::Color(255, 140, 140, 152), {} });
    out.push_back({ 6, volBuf,
                    g_cfg->muteAudio ? Gdiplus::Color(255, 38, 38, 44) : Gdiplus::Color(255, 74, 26, 58),
                    g_cfg->muteAudio ? Gdiplus::Color(255, 140, 140, 152) : Gdiplus::Color(255, 255, 128, 208), {} });

    int x = 8;
    for (auto& p : out) {
        int pw = static_cast<int>(p.text.size()) * 7 + 16;
        p.rc = { x, 4, x + pw, kStatusBarHeight - 4 };
        x += pw + 6;
    }
}

static LRESULT CALLBACK StatusBarWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            SetTimer(hWnd, 1, 500, nullptr); // Refresh live FPS & controller badge every 500ms
            return 0;
        case WM_DESTROY:
            KillTimer(hWnd, 1);
            return 0;
        case WM_TIMER:
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE: {
            int x = static_cast<short>(LOWORD(lParam));
            int y = static_cast<short>(HIWORD(lParam));
            int h = -1;
            for (int i = 0; i < static_cast<int>(g_pills.size()); ++i) {
                if (x >= g_pills[i].rc.left && x < g_pills[i].rc.right &&
                    y >= g_pills[i].rc.top  && y < g_pills[i].rc.bottom) {
                    h = i;
                    break;
                }
            }
            if (h != g_pillHover) {
                g_pillHover = h;
                SetCursor(LoadCursor(nullptr, (h >= 0) ? IDC_HAND : IDC_ARROW));
                InvalidateRect(hWnd, nullptr, FALSE);
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hWnd, 0 };
                TrackMouseEvent(&tme);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (g_pillHover != -1) {
                g_pillHover = -1;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            return 0;
        case WM_MOUSEWHEEL: {
            if (g_cfg && g_cb.onApplyConfig) {
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                int step = (delta > 0) ? 5 : -5;
                g_cfg->masterVolume = std::max(0, std::min(100, g_cfg->masterVolume + step));
                if (g_cfg->masterVolume > 0) g_cfg->muteAudio = false;
                g_cb.onApplyConfig(*g_cfg, false);
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (!g_cfg || !g_cb.onApplyConfig) return 0;
            int x = static_cast<short>(LOWORD(lParam));
            int y = static_cast<short>(HIWORD(lParam));
            for (const auto& p : g_pills) {
                if (x >= p.rc.left && x < p.rc.right && y >= p.rc.top && y < p.rc.bottom) {
                    bool relayout = false;
                    switch (p.id) {
                        case 0: OpenConfigDialog(GetParent(hWnd), g_cfg, &g_cb, 1); return 0;
                        case 1: g_cfg->aspectMode = (g_cfg->aspectMode + 1) % 3; relayout = true; break;
                        case 7: g_cfg->resScale = (g_cfg->resScale % 4) + 1; break;
                        case 2: g_cfg->msaa = (g_cfg->msaa == 0) ? 2 : ((g_cfg->msaa == 2) ? 4 : 0); break;
                        case 3: g_cfg->aniso = (g_cfg->aniso == 1) ? 4 : ((g_cfg->aniso == 4) ? 8 : ((g_cfg->aniso == 8) ? 16 : 1)); break;
                        case 4: g_cfg->atmosphericFog = !g_cfg->atmosphericFog; break;
                        case 5: g_cfg->enableHDTextures = !g_cfg->enableHDTextures; break;
                        case 6: g_cfg->muteAudio = !g_cfg->muteAudio; break;
                    }
                    g_cb.onApplyConfig(*g_cfg, relayout);
                    InvalidateRect(hWnd, nullptr, FALSE);
                    break;
                }
            }
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            EnsureGdip();
            BuildStatusPills(g_pills);
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc{};
            GetClientRect(hWnd, &rc);
            int w = rc.right - rc.left, h = rc.bottom - rc.top;
            if (w > 0 && h > 0) {
                HDC memDC = CreateCompatibleDC(hdc);
                HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
                HGDIOBJ old = SelectObject(memDC, bmp);
                {
                    Gdiplus::Graphics g(memDC);
                    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
                    Palette pal = GetPalette();
                    Gdiplus::SolidBrush bg(pal.barBg);
                    g.FillRectangle(&bg, 0, 0, w, h);
                    Gdiplus::Pen line(pal.border, 1.0f);
                    g.DrawLine(&line, 0, 0, w, 0);

                    for (int i = 0; i < static_cast<int>(g_pills.size()); ++i) {
                        const auto& p = g_pills[i];
                        float px = static_cast<float>(p.rc.left);
                        float py = static_cast<float>(p.rc.top);
                        float pw = static_cast<float>(p.rc.right - p.rc.left);
                        float ph = static_cast<float>(p.rc.bottom - p.rc.top);
                        FillRoundRect(g, px, py, pw, ph, 4.0f, p.bg);
                        if (g_pillHover == i) {
                            DrawRoundRect(g, px, py, pw, ph, 4.0f, p.fg, 1.0f);
                        }
                        DrawTextStr(g, p.text.c_str(), Gdiplus::RectF(px, py, pw, ph),
                                    p.fg, 8.0f, true, Gdiplus::StringAlignmentCenter);
                    }

                    // Right-aligned live telemetry: Quick status + Pad 0 + Live FPS
                    uint32_t fps = xbe_loader::GetMeasuredFps();
                    const char* qtxt = xbe_loader::GetQuickStatusText();
                    bool padOk = xbe_loader::IsGamepadConnected();
                    char rightBuf[160]{};
                    if (qtxt && qtxt[0]) {
                        std::snprintf(rightBuf, sizeof(rightBuf), "%s  |  %s  |  %u FPS",
                                      qtxt, padOk ? "Xbox Pad 0" : "Keyboard", fps);
                    } else {
                        std::snprintf(rightBuf, sizeof(rightBuf), "%s  |  %u FPS",
                                      padOk ? "Xbox Pad 0" : "Keyboard", fps);
                    }
                    DrawTextStr(g, rightBuf, Gdiplus::RectF(static_cast<float>(w - 280), 0.0f, 270.0f, static_cast<float>(h)),
                                (qtxt && qtxt[0]) ? pal.accent : pal.textDim, 8.5f, (qtxt && qtxt[0]), Gdiplus::StringAlignmentFar);
                }
                BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
                SelectObject(memDC, old);
                DeleteObject(bmp);
                DeleteDC(memDC);
            }
            EndPaint(hWnd, &ps);
            return 0;
        }
    }
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

HWND CreateTopMenuBar(HWND hParent, AppConfig* cfg, const Callbacks* cb) {
    g_cfg = cfg;
    if (cb) g_cb = *cb;
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXA wc{ sizeof(wc) };
        wc.lpfnWndProc   = TopBarWndProc;
        wc.hInstance     = GetModuleHandleA(nullptr);
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = "BuffyDarkTopBar";
        RegisterClassExA(&wc);
        reg = true;
    }
    g_hTopBar = CreateWindowExA(0, "BuffyDarkTopBar", "", WS_CHILD | WS_VISIBLE,
                                0, 0, 800, kTopBarHeight, hParent, nullptr, GetModuleHandleA(nullptr), nullptr);
    return g_hTopBar;
}

HWND CreateStatusBar(HWND hParent, AppConfig* cfg, const Callbacks* cb) {
    g_cfg = cfg;
    if (cb) g_cb = *cb;
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXA wc{ sizeof(wc) };
        wc.lpfnWndProc   = StatusBarWndProc;
        wc.hInstance     = GetModuleHandleA(nullptr);
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = "BuffyDarkStatusBar";
        RegisterClassExA(&wc);
        reg = true;
    }
    g_hStatusBar = CreateWindowExA(0, "BuffyDarkStatusBar", "", WS_CHILD | WS_VISIBLE,
                                   0, 0, 800, kStatusBarHeight, hParent, nullptr, GetModuleHandleA(nullptr), nullptr);
    return g_hStatusBar;
}

// ============================================================================
// 3. Modern Dark Buffy Configuration Window (Sidebar + Toggles + Sliders + Dropdowns)
// ============================================================================
static const char* kTabNames[6] = { "General", "Graphics", "Textures", "Audio", "Controls", "Debug" };

enum class CtrlKind { Toggle, Dropdown, Slider, Button, Label };
struct UiCtrl {
    int         id;
    CtrlKind    kind;
    std::string label;
    std::string valueText;
    bool        bVal   = false;
    int         iVal   = 0;
    int         iMin   = 0;
    int         iMax   = 100;
    RECT        rc{};
    RECT        hitRc{};
};

static int                 g_cfgTab      = 0;
static AppConfig           g_backupCfg{};
static std::vector<UiCtrl> g_ctrls;
static int                 g_dragSliderId = -1;
static int                 g_hoverSide    = -1;
static int                 g_hoverCtrlId  = -1;
static RECT                g_rcBtnOk{};
static RECT                g_rcBtnCancel{};

static void RebuildConfigControls(int w, int h) {
    g_ctrls.clear();
    if (!g_cfg) return;

    const int leftX   = 186;
    const int contentW = std::max(300, w - leftX - 24);
    int y = 58;

    auto addToggle = [&](int id, const char* lbl, bool val) {
        UiCtrl c{};
        c.id = id; c.kind = CtrlKind::Toggle; c.label = lbl; c.bVal = val;
        c.rc = { leftX, y, leftX + contentW, y + 32 };
        c.hitRc = { leftX, y + 4, leftX + 260, y + 28 };
        g_ctrls.push_back(c);
        y += 38;
    };
    auto addDropdown = [&](int id, const char* lbl, const char* valTxt) {
        UiCtrl c{};
        c.id = id; c.kind = CtrlKind::Dropdown; c.label = lbl; c.valueText = valTxt;
        c.rc = { leftX, y, leftX + contentW, y + 32 };
        c.hitRc = { leftX + contentW - 240, y + 2, leftX + contentW, y + 30 };
        g_ctrls.push_back(c);
        y += 40;
    };
    auto addSlider = [&](int id, const char* lbl, int val, int mn, int mx, const char* valTxt) {
        UiCtrl c{};
        c.id = id; c.kind = CtrlKind::Slider; c.label = lbl; c.iVal = val; c.iMin = mn; c.iMax = mx; c.valueText = valTxt;
        c.rc = { leftX, y, leftX + contentW, y + 32 };
        c.hitRc = { leftX + contentW - 240, y + 4, leftX + contentW - 54, y + 28 };
        g_ctrls.push_back(c);
        y += 40;
    };
    auto addButton = [&](int id, const char* lbl, int bx, int bw) {
        UiCtrl c{};
        c.id = id; c.kind = CtrlKind::Button; c.label = lbl;
        c.rc = { bx, y, bx + bw, y + 30 };
        c.hitRc = c.rc;
        g_ctrls.push_back(c);
    };
    auto addLabel = [&](const char* lbl) {
        UiCtrl c{};
        c.id = -1; c.kind = CtrlKind::Label; c.label = lbl;
        c.rc = { leftX, y, leftX + contentW, y + 24 };
        g_ctrls.push_back(c);
        y += 28;
    };

    if (g_cfgTab == 0) { // General
        const char* cosStr = (g_cfg->buffyCostume == 1) ? "Slayer (Blue Top)"
                           : ((g_cfg->buffyCostume == 2) ? "Cheerleader"
                           : ((g_cfg->buffyCostume == 3) ? "The Bronze (Club)"
                           : ((g_cfg->buffyCostume == 4) ? "Workout / Sport" : "Level Default")));
        addDropdown(107, "Buffy Costume:", cosStr);
        addLabel("  (Automatically reloads your current spot with the new outfit)");
        y += 4;
        addToggle(101, "Auto-Resume Quick Save on Launch", g_cfg->autoResumeQuickSave);
        addToggle(102, "Hide Mouse Cursor on Inactivity",  g_cfg->hideMouseInactivity);
        addToggle(103, "Show Bottom Status Bar",           g_cfg->showStatusBar);
        y += 12;
        addButton(104, "Quick Save Now (F5)", leftX, 165);
        addButton(105, "Quick Load (F6)",     leftX + 177, 150);
        y += 44;
        addButton(106, "Reset All Settings",  leftX, 165);
    } else if (g_cfgTab == 1) { // Graphics
        const char* aspStr = (g_cfg->aspectMode == 2) ? "21:9 Ultrawide (3440x1440)"
                           : ((g_cfg->aspectMode == 1) ? "16:9 Widescreen" : "4:3 Classic");
        const char* resStr = (g_cfg->resScale == 1) ? "1x Native (480p)"
                           : ((g_cfg->resScale == 2) ? "2x HD (720p / 960p)"
                           : ((g_cfg->resScale == 3) ? "3x Full HD (1080p / 1440p)" : "4x Max (3440x1440 / 4K)"));
        const char* fpsStr = (g_cfg->targetFps == 30) ? "30 FPS (Original Xbox)"
                           : ((g_cfg->targetFps == 60) ? "60 FPS (Smooth HD)"
                           : ((g_cfg->targetFps == 120) ? "120 FPS (High Refresh)"
                           : ((g_cfg->targetFps == 165) ? "165 FPS (Alienware OLED)" : "Uncapped")));
        const char* msaaStr = (g_cfg->msaa == 4) ? "4x MSAA" : ((g_cfg->msaa == 2) ? "2x MSAA" : "Off");
        const char* afStr   = (g_cfg->aniso == 16) ? "16x" : ((g_cfg->aniso == 8) ? "8x" : ((g_cfg->aniso == 4) ? "4x" : "Off"));

        addDropdown(201, "Aspect Ratio:",          aspStr);
        addDropdown(202, "Resolution Scale:",      resStr);
        addDropdown(203, "Frame Rate Limit:",      fpsStr);
        addDropdown(204, "Anti-Aliasing:",         msaaStr);
        addDropdown(205, "Anisotropic Filtering:", afStr);
        y += 6;
        addToggle(206, "Atmospheric Distance Fog", g_cfg->atmosphericFog);
    } else if (g_cfgTab == 2) { // Textures
        addToggle(301, "Enable Custom HD Textures",        g_cfg->enableHDTextures);
        addToggle(302, "Dump Visible Textures to PNG (F8)", g_cfg->dumpTextures);
        y += 12;
        addButton(303, "Reload HD Textures (F9)", leftX, 185);
        y += 42;
        addButton(304, "Open HD Textures Folder...",     leftX, 200);
        addButton(305, "Open Dumped Textures Folder...", leftX + 212, 215);
    } else if (g_cfgTab == 3) { // Audio
        char volTxt[16]{};
        std::snprintf(volTxt, sizeof(volTxt), "%d%%", g_cfg->masterVolume);
        addSlider(401, "Master Volume:", g_cfg->masterVolume, 0, 100, volTxt);
        y += 6;
        addToggle(402, "Mute Audio", g_cfg->muteAudio);
        addToggle(403, "Mute Audio When in Background", g_cfg->muteInBackground);
    } else if (g_cfgTab == 4) { // Controls
        char padTxt[96]{};
        if (xbe_loader::IsGamepadConnected()) {
            std::snprintf(padTxt, sizeof(padTxt), "Controller Status: Connected (Xbox Controller %u)", xbe_loader::GetActivePadSlot());
        } else {
            std::snprintf(padTxt, sizeof(padTxt), "Controller Status: No Xbox Pad detected (Keyboard Active)");
        }
        addLabel(padTxt);
        y += 6;
        addToggle(501, "Controller Vibration / Rumble", g_cfg->rumbleEnabled);
        char dzTxt[16]{};
        std::snprintf(dzTxt, sizeof(dzTxt), "%d%%", g_cfg->deadzonePercent);
        addSlider(502, "Analog Stick Deadzone:", g_cfg->deadzonePercent, 0, 40, dzTxt);
        y += 12;
        addLabel("Keyboard: WASD Move | Space/J=A | Shift/K=B | Ctrl/L=X | E/I=Y | Q=LT | R=RT | Enter=Start");
        addLabel("Hotkeys:  F1 Config | F5 Quick Save | F6 Quick Load | F11 Fullscreen | Esc Exit");
    } else if (g_cfgTab == 5) { // Debug
        addToggle(601, "Show Debug Telemetry Log (F2)", g_cfg->showDebugLog);
        y += 12;
        addButton(602, "Trace 1 Frame to Log (F7)", leftX, 200);
    }

    g_rcBtnCancel = { w - 104, h - 42, w - 16,  h - 12 };
    g_rcBtnOk     = { w - 200, h - 42, w - 112, h - 12 };
}

static void HandleDropdownClick(HWND hWnd, const UiCtrl& c) {
    if (!g_cfg || !g_cb.onApplyConfig) return;
    POINT pt{ c.hitRc.left, c.hitRc.bottom };
    ClientToScreen(hWnd, &pt);
    HMENU hPop = CreatePopupMenu();
    if (c.id == 107) {
        AppendMenuA(hPop, MF_STRING, 1, "Level Default");
        AppendMenuA(hPop, MF_STRING, 2, "Slayer (Blue Top)");
        AppendMenuA(hPop, MF_STRING, 3, "Cheerleader");
        AppendMenuA(hPop, MF_STRING, 4, "The Bronze (Club)");
        AppendMenuA(hPop, MF_STRING, 5, "Workout / Sport");
    } else if (c.id == 201) {
        AppendMenuA(hPop, MF_STRING, 1, "4:3 Classic");
        AppendMenuA(hPop, MF_STRING, 2, "16:9 Widescreen");
        AppendMenuA(hPop, MF_STRING, 3, "21:9 Ultrawide (3440x1440)");
    } else if (c.id == 202) {
        AppendMenuA(hPop, MF_STRING, 1, "1x Native (480p)");
        AppendMenuA(hPop, MF_STRING, 2, "2x HD (720p / 960p)");
        AppendMenuA(hPop, MF_STRING, 3, "3x Full HD (1080p / 1440p)");
        AppendMenuA(hPop, MF_STRING, 4, "4x Max (3440x1440 / 4K)");
    } else if (c.id == 203) {
        AppendMenuA(hPop, MF_STRING, 30,  "30 FPS (Original Xbox)");
        AppendMenuA(hPop, MF_STRING, 60,  "60 FPS (Smooth HD)");
        AppendMenuA(hPop, MF_STRING, 120, "120 FPS (High Refresh)");
        AppendMenuA(hPop, MF_STRING, 165, "165 FPS (Alienware OLED)");
        AppendMenuA(hPop, MF_STRING, 999, "Uncapped");
    } else if (c.id == 204) {
        AppendMenuA(hPop, MF_STRING, 1, "Off");
        AppendMenuA(hPop, MF_STRING, 2, "2x MSAA");
        AppendMenuA(hPop, MF_STRING, 4, "4x MSAA");
    } else if (c.id == 205) {
        AppendMenuA(hPop, MF_STRING, 1,  "Off");
        AppendMenuA(hPop, MF_STRING, 4,  "4x");
        AppendMenuA(hPop, MF_STRING, 8,  "8x");
        AppendMenuA(hPop, MF_STRING, 16, "16x");
    }
    UINT sel = TrackPopupMenu(hPop, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD, pt.x, pt.y, 0, hWnd, nullptr);
    DestroyMenu(hPop);
    if (sel == 0) return;

    bool relayout = false;
    if (c.id == 107)      { g_cfg->buffyCostume = sel - 1; }
    else if (c.id == 201) { g_cfg->aspectMode = sel - 1; relayout = true; }
    else if (c.id == 202) { g_cfg->resScale   = sel; }
    else if (c.id == 203) { g_cfg->targetFps  = (sel == 999) ? 0u : sel; }
    else if (c.id == 204) { g_cfg->msaa       = (sel == 1) ? 0u : sel; }
    else if (c.id == 205) { g_cfg->aniso      = sel; }

    g_cb.onApplyConfig(*g_cfg, relayout);
    InvalidateRect(hWnd, nullptr, FALSE);
}

static void UpdateSliderFromX(const UiCtrl& c, int mx) {
    if (!g_cfg || !g_cb.onApplyConfig) return;
    int w = std::max(1, static_cast<int>(c.hitRc.right - c.hitRc.left));
    float t = static_cast<float>(mx - c.hitRc.left) / static_cast<float>(w);
    t = std::max(0.0f, std::min(1.0f, t));
    int v = c.iMin + static_cast<int>(t * static_cast<float>(c.iMax - c.iMin) + 0.5f);
    if (c.id == 401) {
        g_cfg->masterVolume = v;
        if (v > 0) g_cfg->muteAudio = false;
    } else if (c.id == 502) {
        g_cfg->deadzonePercent = v;
    }
    g_cb.onApplyConfig(*g_cfg, false);
}

static LRESULT CALLBACK ConfigDlgWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            ApplySystemWindowTheme(hWnd);
            return 0;
        case WM_MOUSEMOVE: {
            int x = static_cast<short>(LOWORD(lParam));
            int y = static_cast<short>(HIWORD(lParam));
            if (g_dragSliderId >= 0) {
                for (const auto& c : g_ctrls) {
                    if (c.id == g_dragSliderId) {
                        UpdateSliderFromX(c, x);
                        InvalidateRect(hWnd, nullptr, FALSE);
                        break;
                    }
                }
                return 0;
            }
            int ns = -1;
            if (x >= 12 && x < 152) {
                for (int i = 0; i < 6; ++i) {
                    int sy = 16 + i * 38;
                    if (y >= sy && y < sy + 32) { ns = i; break; }
                }
            }
            int nc = -1;
            for (const auto& c : g_ctrls) {
                if (c.id >= 0 && x >= c.hitRc.left && x < c.hitRc.right && y >= c.hitRc.top && y < c.hitRc.bottom) {
                    nc = c.id; break;
                }
            }
            if (ns != g_hoverSide || nc != g_hoverCtrlId) {
                g_hoverSide = ns;
                g_hoverCtrlId = nc;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            int x = static_cast<short>(LOWORD(lParam));
            int y = static_cast<short>(HIWORD(lParam));
            for (const auto& c : g_ctrls) {
                if (c.kind == CtrlKind::Slider && x >= c.hitRc.left - 8 && x <= c.hitRc.right + 8 &&
                    y >= c.hitRc.top && y <= c.hitRc.bottom) {
                    g_dragSliderId = c.id;
                    SetCapture(hWnd);
                    UpdateSliderFromX(c, x);
                    InvalidateRect(hWnd, nullptr, FALSE);
                    return 0;
                }
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (g_dragSliderId >= 0) {
                g_dragSliderId = -1;
                ReleaseCapture();
                return 0;
            }
            int x = static_cast<short>(LOWORD(lParam));
            int y = static_cast<short>(HIWORD(lParam));

            // Sidebar tabs
            if (x >= 12 && x < 152) {
                for (int i = 0; i < 6; ++i) {
                    int sy = 16 + i * 38;
                    if (y >= sy && y < sy + 32) {
                        g_cfgTab = i;
                        InvalidateRect(hWnd, nullptr, FALSE);
                        return 0;
                    }
                }
            }
            // OK / Cancel buttons
            if (x >= g_rcBtnOk.left && x < g_rcBtnOk.right && y >= g_rcBtnOk.top && y < g_rcBtnOk.bottom) {
                if (g_cfg && g_cb.onApplyConfig) g_cb.onApplyConfig(*g_cfg, true);
                DestroyWindow(hWnd);
                return 0;
            }
            if (x >= g_rcBtnCancel.left && x < g_rcBtnCancel.right && y >= g_rcBtnCancel.top && y < g_rcBtnCancel.bottom) {
                if (g_cfg && g_cb.onApplyConfig) {
                    *g_cfg = g_backupCfg;
                    g_cb.onApplyConfig(*g_cfg, true);
                }
                DestroyWindow(hWnd);
                return 0;
            }
            // Active page controls
            for (const auto& c : g_ctrls) {
                if (c.id < 0) continue;
                if (x >= c.hitRc.left && x < c.hitRc.right && y >= c.hitRc.top && y < c.hitRc.bottom) {
                    if (c.kind == CtrlKind::Dropdown) {
                        HandleDropdownClick(hWnd, c);
                    } else if (c.kind == CtrlKind::Toggle && g_cfg && g_cb.onApplyConfig) {
                        bool relayout = false;
                        if (c.id == 101) g_cfg->autoResumeQuickSave = !g_cfg->autoResumeQuickSave;
                        else if (c.id == 102) g_cfg->hideMouseInactivity = !g_cfg->hideMouseInactivity;
                        else if (c.id == 103) { g_cfg->showStatusBar = !g_cfg->showStatusBar; relayout = true; }
                        else if (c.id == 206) g_cfg->atmosphericFog = !g_cfg->atmosphericFog;
                        else if (c.id == 301) g_cfg->enableHDTextures = !g_cfg->enableHDTextures;
                        else if (c.id == 302) g_cfg->dumpTextures = !g_cfg->dumpTextures;
                        else if (c.id == 402) g_cfg->muteAudio = !g_cfg->muteAudio;
                        else if (c.id == 403) g_cfg->muteInBackground = !g_cfg->muteInBackground;
                        else if (c.id == 501) g_cfg->rumbleEnabled = !g_cfg->rumbleEnabled;
                        else if (c.id == 601) { g_cfg->showDebugLog = !g_cfg->showDebugLog; relayout = true; }
                        g_cb.onApplyConfig(*g_cfg, relayout);
                        InvalidateRect(hWnd, nullptr, FALSE);
                    } else if (c.kind == CtrlKind::Button) {
                        if (c.id == 104 && g_cb.onQuickSave) g_cb.onQuickSave();
                        else if (c.id == 105 && g_cb.onQuickLoad) g_cb.onQuickLoad();
                        else if (c.id == 106 && g_cfg && g_cb.onApplyConfig) {
                            *g_cfg = AppConfig{};
                            g_cb.onApplyConfig(*g_cfg, true);
                            InvalidateRect(hWnd, nullptr, FALSE);
                        }
                        else if (c.id == 303 && g_cb.onReloadHD) g_cb.onReloadHD();
                        else if (c.id == 304 && g_cb.onOpenHDFolder) g_cb.onOpenHDFolder();
                        else if (c.id == 305 && g_cb.onOpenDumpFolder) g_cb.onOpenDumpFolder();
                        else if (c.id == 602 && g_cb.onFrameTrace) g_cb.onFrameTrace();
                    }
                    return 0;
                }
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                DestroyWindow(hWnd);
                return 0;
            }
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            EnsureGdip();
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc{};
            GetClientRect(hWnd, &rc);
            int w = rc.right - rc.left, h = rc.bottom - rc.top;
            if (w > 0 && h > 0) {
                RebuildConfigControls(w, h);
                HDC memDC = CreateCompatibleDC(hdc);
                HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
                HGDIOBJ old = SelectObject(memDC, bmp);
                {
                    Gdiplus::Graphics g(memDC);
                    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
                    Palette pal = GetPalette();

                    // Main bg & Left sidebar
                    Gdiplus::SolidBrush brWin(pal.winBg), brSide(pal.sideBg);
                    g.FillRectangle(&brWin, 0, 0, w, h);
                    g.FillRectangle(&brSide, 0, 0, 164, h - 54);
                    Gdiplus::Pen sep(pal.border, 1.0f);
                    g.DrawLine(&sep, 164, 0, 164, h - 54);
                    g.DrawLine(&sep, 0, h - 54, w, h - 54);

                    // Sidebar tab buttons
                    for (int i = 0; i < 6; ++i) {
                        float sy = static_cast<float>(16 + i * 38);
                        bool sel = (i == g_cfgTab);
                        if (sel) {
                            FillRoundRect(g, 12.0f, sy, 140.0f, 32.0f, 6.0f, pal.accentBg);
                        } else if (g_hoverSide == i) {
                            FillRoundRect(g, 12.0f, sy, 140.0f, 32.0f, 6.0f, pal.ctrlHover);
                        }
                        DrawTextStr(g, kTabNames[i], Gdiplus::RectF(26.0f, sy, 120.0f, 32.0f),
                                    sel ? pal.text : pal.textDim, 9.5f, sel);
                    }

                    // Active section header with pink underline
                    DrawTextStr(g, kTabNames[g_cfgTab], Gdiplus::RectF(186.0f, 14.0f, 200.0f, 28.0f),
                                pal.text, 11.0f, true);
                    Gdiplus::Pen accPen(pal.accent, 2.0f);
                    g.DrawLine(&accPen, 186, 44, 266, 44);
                    g.DrawLine(&sep, 186, 45, w - 24, 45);

                    // Controls
                    for (const auto& c : g_ctrls) {
                        if (c.kind == CtrlKind::Toggle) {
                            float tx = static_cast<float>(c.rc.left);
                            float ty = static_cast<float>(c.rc.top + 6);
                            FillRoundRect(g, tx, ty, 38.0f, 20.0f, 10.0f, c.bVal ? pal.accent : pal.ctrlHover);
                            float kx = c.bVal ? (tx + 20.0f) : (tx + 2.0f);
                            FillRoundRect(g, kx, ty + 2.0f, 16.0f, 16.0f, 8.0f, Gdiplus::Color(255, 255, 255, 255));
                            DrawTextStr(g, c.label.c_str(),
                                        Gdiplus::RectF(tx + 48.0f, static_cast<float>(c.rc.top), 380.0f, 32.0f),
                                        pal.text, 9.5f);
                        } else if (c.kind == CtrlKind::Dropdown) {
                            DrawTextStr(g, c.label.c_str(),
                                        Gdiplus::RectF(static_cast<float>(c.rc.left), static_cast<float>(c.rc.top), 240.0f, 32.0f),
                                        pal.text, 9.5f);
                            float dx = static_cast<float>(c.hitRc.left);
                            float dy = static_cast<float>(c.hitRc.top);
                            float dw = static_cast<float>(c.hitRc.right - c.hitRc.left);
                            float dh = static_cast<float>(c.hitRc.bottom - c.hitRc.top);
                            FillRoundRect(g, dx, dy, dw, dh, 5.0f, (g_hoverCtrlId == c.id) ? pal.ctrlHover : pal.ctrlBg);
                            DrawRoundRect(g, dx, dy, dw, dh, 5.0f, pal.border);
                            DrawTextStr(g, c.valueText.c_str(), Gdiplus::RectF(dx + 10.0f, dy, dw - 28.0f, dh), pal.text, 9.0f);
                            DrawTextStr(g, "v", Gdiplus::RectF(dx + dw - 22.0f, dy, 16.0f, dh), pal.textDim, 8.5f, false, Gdiplus::StringAlignmentCenter);
                        } else if (c.kind == CtrlKind::Slider) {
                            DrawTextStr(g, c.label.c_str(),
                                        Gdiplus::RectF(static_cast<float>(c.rc.left), static_cast<float>(c.rc.top), 220.0f, 32.0f),
                                        pal.text, 9.5f);
                            float sx = static_cast<float>(c.hitRc.left);
                            float sy = static_cast<float>((c.hitRc.top + c.hitRc.bottom) / 2);
                            float sw = static_cast<float>(c.hitRc.right - c.hitRc.left);
                            float t = static_cast<float>(c.iVal - c.iMin) / static_cast<float>(std::max(1, c.iMax - c.iMin));
                            FillRoundRect(g, sx, sy - 2.0f, sw, 4.0f, 2.0f, pal.ctrlHover);
                            FillRoundRect(g, sx, sy - 2.0f, sw * t, 4.0f, 2.0f, pal.accent);
                            FillRoundRect(g, sx + sw * t - 7.0f, sy - 7.0f, 14.0f, 14.0f, 7.0f, Gdiplus::Color(255, 255, 255, 255));
                            DrawTextStr(g, c.valueText.c_str(),
                                        Gdiplus::RectF(sx + sw + 10.0f, static_cast<float>(c.rc.top), 46.0f, 32.0f),
                                        pal.text, 9.0f, false, Gdiplus::StringAlignmentFar);
                        } else if (c.kind == CtrlKind::Button) {
                            float bx = static_cast<float>(c.rc.left);
                            float by = static_cast<float>(c.rc.top);
                            float bw = static_cast<float>(c.rc.right - c.rc.left);
                            float bh = static_cast<float>(c.rc.bottom - c.rc.top);
                            FillRoundRect(g, bx, by, bw, bh, 5.0f, (g_hoverCtrlId == c.id) ? pal.ctrlHover : pal.ctrlBg);
                            DrawRoundRect(g, bx, by, bw, bh, 5.0f, pal.border);
                            DrawTextStr(g, c.label.c_str(), Gdiplus::RectF(bx, by, bw, bh),
                                        pal.text, 9.0f, false, Gdiplus::StringAlignmentCenter);
                        } else if (c.kind == CtrlKind::Label) {
                            DrawTextStr(g, c.label.c_str(),
                                        Gdiplus::RectF(static_cast<float>(c.rc.left), static_cast<float>(c.rc.top),
                                                       static_cast<float>(c.rc.right - c.rc.left), 24.0f),
                                        pal.textDim, 8.8f);
                        }
                    }

                    // Footer text + OK (accent) & Cancel buttons
                    DrawTextStr(g, "Changes apply live and save to recomp\\settings.ini",
                                Gdiplus::RectF(16.0f, static_cast<float>(h - 46), 380.0f, 36.0f), pal.textDim, 8.5f);

                    FillRoundRect(g, static_cast<float>(g_rcBtnOk.left), static_cast<float>(g_rcBtnOk.top),
                                  static_cast<float>(g_rcBtnOk.right - g_rcBtnOk.left), static_cast<float>(g_rcBtnOk.bottom - g_rcBtnOk.top),
                                  5.0f, pal.accent);
                    DrawTextStr(g, "OK", Gdiplus::RectF(static_cast<float>(g_rcBtnOk.left), static_cast<float>(g_rcBtnOk.top),
                                                        static_cast<float>(g_rcBtnOk.right - g_rcBtnOk.left), static_cast<float>(g_rcBtnOk.bottom - g_rcBtnOk.top)),
                                Gdiplus::Color(255, 255, 255, 255), 9.5f, true, Gdiplus::StringAlignmentCenter);

                    FillRoundRect(g, static_cast<float>(g_rcBtnCancel.left), static_cast<float>(g_rcBtnCancel.top),
                                  static_cast<float>(g_rcBtnCancel.right - g_rcBtnCancel.left), static_cast<float>(g_rcBtnCancel.bottom - g_rcBtnCancel.top),
                                  5.0f, pal.ctrlBg);
                    DrawRoundRect(g, static_cast<float>(g_rcBtnCancel.left), static_cast<float>(g_rcBtnCancel.top),
                                  static_cast<float>(g_rcBtnCancel.right - g_rcBtnCancel.left), static_cast<float>(g_rcBtnCancel.bottom - g_rcBtnCancel.top),
                                  5.0f, pal.border);
                    DrawTextStr(g, "Cancel", Gdiplus::RectF(static_cast<float>(g_rcBtnCancel.left), static_cast<float>(g_rcBtnCancel.top),
                                                            static_cast<float>(g_rcBtnCancel.right - g_rcBtnCancel.left), static_cast<float>(g_rcBtnCancel.bottom - g_rcBtnCancel.top)),
                                pal.text, 9.5f, false, Gdiplus::StringAlignmentCenter);
                }
                BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
                SelectObject(memDC, old);
                DeleteObject(bmp);
                DeleteDC(memDC);
            }
            EndPaint(hWnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            g_hCfgWnd = nullptr;
            g_dragSliderId = -1;
            return 0;
    }
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

void OpenConfigDialog(HWND hOwner, AppConfig* cfg, const Callbacks* cb, int initialTab) {
    if (cfg) g_cfg = cfg;
    if (cb)  g_cb  = *cb;
    if (initialTab >= 0 && initialTab < 6) g_cfgTab = initialTab;

    if (g_hCfgWnd && IsWindow(g_hCfgWnd)) {
        SetForegroundWindow(g_hCfgWnd);
        InvalidateRect(g_hCfgWnd, nullptr, FALSE);
        return;
    }
    if (g_cfg) g_backupCfg = *g_cfg;

    static bool reg = false;
    if (!reg) {
        WNDCLASSEXA wc{ sizeof(wc) };
        wc.lpfnWndProc   = ConfigDlgWndProc;
        wc.hInstance     = GetModuleHandleA(nullptr);
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = "BuffyConfigDialogWnd";
        RegisterClassExA(&wc);
        reg = true;
    }

    int dw = 740, dh = 490;
    RECT rcOwner{};
    int px = CW_USEDEFAULT, py = CW_USEDEFAULT;
    if (hOwner && GetWindowRect(hOwner, &rcOwner)) {
        px = rcOwner.left + ((rcOwner.right - rcOwner.left) - dw) / 2;
        py = rcOwner.top  + ((rcOwner.bottom - rcOwner.top) - dh) / 2;
    }
    g_hCfgWnd = CreateWindowExA(
        WS_EX_DLGMODALFRAME, "BuffyConfigDialogWnd", "Buffy Configuration",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        px, py, dw, dh, hOwner, nullptr, GetModuleHandleA(nullptr), nullptr
    );
    ApplySystemWindowTheme(g_hCfgWnd);
}

void CloseConfigDialog() {
    if (g_hCfgWnd && IsWindow(g_hCfgWnd)) {
        DestroyWindow(g_hCfgWnd);
    }
}

void Shutdown() {
    CloseConfigDialog();
    if (g_uiGdipToken != 0) {
        Gdiplus::GdiplusShutdown(g_uiGdipToken);
        g_uiGdipToken = 0;
    }
}

} // namespace config_ui
