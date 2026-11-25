#include "x86_cpu.h"
#include "d3d11_bridge.h"
#include "uwp_config_ui.h"
#include "xaudio2_bridge.h"
#include "host_bridges.h"
#include "generated/recomp_decls.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace uwp_recomp { namespace host_bridges {
void SetScriptedPressFrames(const uint32_t* frames, uint32_t count);
void SetOverrideMap(const char* mapName);
} }

// Phase 1 Win64 Desktop Test Harness for the Portable x64 / UWP Recomp Runtime.
// Verifies flat 64 MB guest RAM loading of Default.xbe, Direct3D 11 FLIP_SEQUENTIAL swapchain,
// and XAudio2 48 kHz mastering voice — all using 100% UWP-compatible APIs!

struct ResPreset { uint32_t w, h; };
static constexpr ResPreset kResPresets[] = {
    { 1280,  720 }, { 1720,  720 },
    { 1920, 1080 }, { 2560, 1080 },
    { 2560, 1440 }, { 3440, 1440 },
    { 3840, 1600 }, { 3840, 2160 },
    { 5120, 2160 }
};
static constexpr int kNumResPresets = 9;
static int  g_resPresetIdx = 2;
static bool g_isFullscreen = false;
static RECT g_savedWinRect{ 100, 100, 1380, 820 };
static DWORD g_savedWinStyle = WS_OVERLAPPEDWINDOW;

static void UpdateWindowTitle(HWND hWnd, uint32_t w, uint32_t h) {
    char buf[160]{};
    std::snprintf(buf, sizeof(buf),
        "BtVS Recomp x64 D3D11 [%ux%u | F5:Res | F11:Fullscreen | F6:Hor+ %s]",
        w, h, uwp_recomp::d3d11_bridge::GetWidescreenHorPlus() ? "ON" : "OFF");
    SetWindowTextA(hWnd, buf);
}

static void SetWindowClientSize(HWND hWnd, uint32_t w, uint32_t h) {
    if (g_isFullscreen) return;
    RECT rc{ 0, 0, static_cast<LONG>(w), static_cast<LONG>(h) };
    AdjustWindowRect(&rc, GetWindowLongA(hWnd, GWL_STYLE), FALSE);
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int x = (sw > ww) ? (sw - ww) / 2 : 0;
    int y = (sh > wh) ? (sh - wh) / 2 : 0;
    SetWindowPos(hWnd, nullptr, x, y, ww, wh, SWP_NOZORDER | SWP_FRAMECHANGED);
    uwp_recomp::d3d11_bridge::Resize(w, h);
    UpdateWindowTitle(hWnd, w, h);
}

static void ToggleFullscreen(HWND hWnd) {
    if (!g_isFullscreen) {
        g_savedWinStyle = static_cast<DWORD>(GetWindowLongA(hWnd, GWL_STYLE));
        GetWindowRect(hWnd, &g_savedWinRect);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoA(MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST), &mi);
        uint32_t mw = static_cast<uint32_t>(mi.rcMonitor.right - mi.rcMonitor.left);
        uint32_t mh = static_cast<uint32_t>(mi.rcMonitor.bottom - mi.rcMonitor.top);
        SetWindowLongA(hWnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hWnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mw, mh,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_isFullscreen = true;
        uwp_recomp::d3d11_bridge::Resize(mw, mh);
        UpdateWindowTitle(hWnd, mw, mh);
    } else {
        SetWindowLongA(hWnd, GWL_STYLE, g_savedWinStyle);
        int ww = g_savedWinRect.right - g_savedWinRect.left;
        int wh = g_savedWinRect.bottom - g_savedWinRect.top;
        SetWindowPos(hWnd, nullptr, g_savedWinRect.left, g_savedWinRect.top, ww, wh,
                     SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_isFullscreen = false;
        RECT crc{}; GetClientRect(hWnd, &crc);
        uint32_t cw = static_cast<uint32_t>(crc.right - crc.left);
        uint32_t ch = static_cast<uint32_t>(crc.bottom - crc.top);
        if (cw > 0 && ch > 0) {
            uwp_recomp::d3d11_bridge::Resize(cw, ch);
            UpdateWindowTitle(hWnd, cw, ch);
        }
    }
}

static LRESULT CALLBACK HarnessWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SIZE:
            if (uwp_recomp::d3d11_bridge::IsReady() && wParam != SIZE_MINIMIZED) {
                uint32_t w = LOWORD(lParam), h = HIWORD(lParam);
                if (w > 0 && h > 0) {
                    uwp_recomp::d3d11_bridge::Resize(w, h);
                    UpdateWindowTitle(hWnd, w, h);
                }
            }
            return 0;
        case WM_SYSKEYDOWN:
            if (wParam == VK_RETURN && (lParam & (1 << 29))) {
                ToggleFullscreen(hWnd);
                uwp_recomp::config_ui::GetMutable().fullscreen = g_isFullscreen;
                uwp_recomp::config_ui::Save();
                return 0;
            }
            break;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) DestroyWindow(hWnd);
            else if (wParam == VK_F11) {
                ToggleFullscreen(hWnd);
                uwp_recomp::config_ui::GetMutable().fullscreen = g_isFullscreen;
                uwp_recomp::config_ui::Save();
            } else if (wParam == VK_F5) {
                if (g_isFullscreen) ToggleFullscreen(hWnd);
                g_resPresetIdx = (g_resPresetIdx + 1) % kNumResPresets;
                SetWindowClientSize(hWnd, kResPresets[g_resPresetIdx].w, kResPresets[g_resPresetIdx].h);
                auto& cfg = uwp_recomp::config_ui::GetMutable();
                cfg.width = kResPresets[g_resPresetIdx].w;
                cfg.height = kResPresets[g_resPresetIdx].h;
                cfg.fullscreen = false;
                uwp_recomp::config_ui::Save();
            } else if (wParam == VK_F6) {
                bool next = !uwp_recomp::d3d11_bridge::GetWidescreenHorPlus();
                uwp_recomp::d3d11_bridge::SetWidescreenHorPlus(next);
                uwp_recomp::config_ui::GetMutable().widescreenHorPlus = next;
                uwp_recomp::config_ui::Save();
                UpdateWindowTitle(hWnd, uwp_recomp::d3d11_bridge::GetWidth(), uwp_recomp::d3d11_bridge::GetHeight());
            } else if (wParam == VK_F7) {
                uwp_recomp::d3d11_bridge::RequestFrameTrace();
            } else if (wParam == VK_F8) {
                std::printf("[UWP-F8] Live guest call trace:\n");
                uwp_recomp::DumpRecentGuestCalls();
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

static LONG WINAPI UwpCrashHandler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    if (code == 0xE06D7363u || code == 0x406D1388u) return EXCEPTION_CONTINUE_SEARCH;
    void* addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
    std::printf("\n[UWP-CRASH] Exception 0x%08lX at host %p (TID=%lu)!\n",
                static_cast<unsigned long>(code), addr, static_cast<unsigned long>(GetCurrentThreadId()));
    uwp_recomp::DumpRecentGuestCalls();
    std::fflush(stdout);
    Sleep(100);
    return EXCEPTION_CONTINUE_SEARCH;
}

int main(int argc, char** argv) {
    SetProcessDPIAware();
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AddVectoredExceptionHandler(1, &UwpCrashHandler);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    std::string xbePath = "extracted\\Default.xbe";
    std::string capDir, overrideMap;
    std::vector<uint32_t> capFrames, pressFrames;
    uint32_t exitAfter = 0, traceFrame = 0;
    uwp_recomp::config_ui::Load();
    uint32_t initW = uwp_recomp::config_ui::Get().width;
    uint32_t initH = uwp_recomp::config_ui::Get().height;
    bool startFullscreen = uwp_recomp::config_ui::Get().fullscreen;
    bool horPlus = uwp_recomp::config_ui::Get().widescreenHorPlus;
    bool openMenuOnBoot = false;
    bool bootMain = false;
    auto parseCsvU32 = [](const char* s, std::vector<uint32_t>& out) {
        while (s && *s) {
            uint32_t v = static_cast<uint32_t>(std::strtoul(s, const_cast<char**>(&s), 10));
            if (v > 0) out.push_back(v);
            if (*s == ',') ++s;
        }
    };
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--boot-main") == 0) bootMain = true;
        else if (std::strcmp(argv[i], "--open-menu") == 0) openMenuOnBoot = true;
        else if (std::strcmp(argv[i], "--map") == 0 && i + 1 < argc) overrideMap = argv[++i];
        else if (std::strcmp(argv[i], "--res") == 0 && i + 1 < argc) {
            uint32_t rw = 0, rh = 0;
            if (std::sscanf(argv[++i], "%ux%u", &rw, &rh) == 2 && rw >= 640 && rh >= 480) { initW = rw; initH = rh; }
        } else if (std::strcmp(argv[i], "--fullscreen") == 0) startFullscreen = true;
        else if (std::strcmp(argv[i], "--windowed") == 0) startFullscreen = false;
        else if (std::strcmp(argv[i], "--no-widescreen") == 0) horPlus = false;
        else if (std::strcmp(argv[i], "--capture-dir") == 0 && i + 1 < argc) capDir = argv[++i];
        else if (std::strcmp(argv[i], "--capture-frames") == 0 && i + 1 < argc) parseCsvU32(argv[++i], capFrames);
        else if (std::strcmp(argv[i], "--press-start") == 0 && i + 1 < argc) parseCsvU32(argv[++i], pressFrames);
        else if (std::strcmp(argv[i], "--exit-after") == 0 && i + 1 < argc) exitAfter = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--trace-frame") == 0 && i + 1 < argc) traceFrame = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (argv[i][0] != '-') xbePath = argv[i];
    }
    uwp_recomp::d3d11_bridge::SetWidescreenHorPlus(horPlus);
    if (exitAfter == 0) {
        auto& cfg = uwp_recomp::config_ui::GetMutable();
        cfg.width = initW;
        cfg.height = initH;
        cfg.fullscreen = startFullscreen;
        cfg.widescreenHorPlus = horPlus;
        uwp_recomp::config_ui::Save();
    }
    if (openMenuOnBoot) uwp_recomp::config_ui::SetOpen(true);
    if (!capDir.empty() || !capFrames.empty() || exitAfter > 0 || traceFrame > 0) {
        uwp_recomp::d3d11_bridge::ConfigureCapture(capDir.c_str(), capFrames.data(), static_cast<uint32_t>(capFrames.size()), exitAfter, traceFrame);
    }
    if (!pressFrames.empty()) {
        uwp_recomp::host_bridges::SetScriptedPressFrames(pressFrames.data(), static_cast<uint32_t>(pressFrames.size()));
    }
    if (!overrideMap.empty()) {
        uwp_recomp::host_bridges::SetOverrideMap(overrideMap.c_str());
    }
    uint32_t entryVA = 0;
    if (!uwp_recomp::InitGuestMemory() || !uwp_recomp::LoadXbeIntoGuestRam(xbePath.c_str(), entryVA)) {
        std::printf("[UWP-MAIN] Could not load Default.xbe from '%s'\n", xbePath.c_str());
        CoUninitialize();
        return 1;
    }

    WNDCLASSEXA wc{ sizeof(wc) };
    wc.lpfnWndProc   = HarnessWndProc;
    wc.hInstance     = GetModuleHandleA(nullptr);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "BuffyUwpRecompHarness";
    RegisterClassExA(&wc);

    RECT wr{ 0, 0, static_cast<LONG>(initW), static_cast<LONG>(initH) };
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hWnd = CreateWindowExA(
        0, wc.lpszClassName, "BtVS Recomp x64 D3D11",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top,
        nullptr, nullptr, wc.hInstance, nullptr
    );
    ShowWindow(hWnd, SW_SHOWNORMAL);
    SetForegroundWindow(hWnd);

    if (!uwp_recomp::d3d11_bridge::Init(hWnd, initW, initH, false)) {
        std::printf("[UWP-MAIN] D3D11 initialization failed!\n");
        CoUninitialize();
        return 1;
    }
    static HWND s_mainHwnd = nullptr;
    s_mainHwnd = hWnd;
    uwp_recomp::config_ui::SetDisplayApplyCallback([](uint32_t w, uint32_t h, bool fs) {
        if (!s_mainHwnd) return;
        if (fs != g_isFullscreen) ToggleFullscreen(s_mainHwnd);
        if (!g_isFullscreen) SetWindowClientSize(s_mainHwnd, w, h);
    });
    UpdateWindowTitle(hWnd, initW, initH);
    if (startFullscreen && exitAfter == 0) ToggleFullscreen(hWnd);
    uwp_recomp::xaudio2_bridge::Init();

    // Phase 2 Live Static-Recomp Proof-of-Concept:
    // Register lifted SlateEngine functions + portable x64 host bridges and execute EqualsI() natively on x64!
    uwp_recomp::RegisterAllLiftedFunctions();
    uwp_recomp::host_bridges::SetDriveRoots("extracted", "recomp\\cache", "recomp\\saves");
    uwp_recomp::host_bridges::RegisterAll();
    {
        uint32_t sA = uwp_recomp::GuestHeapAlloc(32);
        uint32_t sB = uwp_recomp::GuestHeapAlloc(32);
        uint32_t sC = uwp_recomp::GuestHeapAlloc(32);
        uint32_t tA = uwp_recomp::GuestHeapAlloc(8);
        uint32_t tB = uwp_recomp::GuestHeapAlloc(8);
        uint32_t tC = uwp_recomp::GuestHeapAlloc(8);
        uwp_recomp::X86Context ctx{};
        ctx.mem = uwp_recomp::GetGuestRamBase();
        std::strcpy(reinterpret_cast<char*>(ctx.ptr(sA)), "Buffy");
        std::strcpy(reinterpret_cast<char*>(ctx.ptr(sB)), "buffy");
        std::strcpy(reinterpret_cast<char*>(ctx.ptr(sC)), "Spike");
        ctx.w32(tA, sA); ctx.w32(tB, sB); ctx.w32(tC, sC);

        // Call lifted sub_00011000 = EqualsI(tA, tB) -> expect 1
        ctx.push32(tB); ctx.push32(tA); ctx.push32(uwp_recomp::kReturnSentinelVA);
        uwp_recomp::CallGuestVA(ctx, 0x00011000u);
        uint32_t eq1 = ctx.eax & 0xFFu;
        ctx.esp += 8u;

        // Call lifted sub_00011000 = EqualsI(tA, tC) -> expect 0
        ctx.push32(tC); ctx.push32(tA); ctx.push32(uwp_recomp::kReturnSentinelVA);
        uwp_recomp::CallGuestVA(ctx, 0x00011000u);
        uint32_t eq2 = ctx.eax & 0xFFu;
        ctx.esp += 8u;

        std::printf("[UWP-LIFT-TEST] Executed lifted SlateEngine EqualsI() natively on x64: EqualsI(\"Buffy\",\"buffy\")=%u, EqualsI(\"Buffy\",\"Spike\")=%u\n", eq1, eq2);
    }

    // Optional live boot of Xbox C++ global constructors (.CRT$XCU) + lifted _main (0x00064F90)
    static uwp_recomp::X86Context* g_mainCtxPtr = nullptr;
    if (bootMain) {
        std::printf("[UWP-MAIN] Running 470 C++ global constructors (0x00750EB0..0x0075160C) + lifted _main (0x00064F90)...\n");
        CreateThread(nullptr, 8u * 1024u * 1024u, [](LPVOID) -> DWORD {
            for (;;) {
                try {
                    uwp_recomp::X86Context mainCtx{};
                    mainCtx.mem = uwp_recomp::GetGuestRamBase();
                    mainCtx.esp = uwp_recomp::kGuestStackTop;
                    g_mainCtxPtr = &mainCtx;
                    uint32_t ctorCount = 0;
                    for (uint32_t p = 0x00750EB0u; p < 0x0075160Cu; p += 4u) {
                        uint32_t fnVA = mainCtx.r32(p);
                        if (fnVA && fnVA != 0xFFFFFFFFu) {
                            mainCtx.push32(uwp_recomp::kReturnSentinelVA);
                            uwp_recomp::CallGuestVA(mainCtx, fnVA);
                            ++ctorCount;
                        }
                    }
                    std::printf("[UWP-MAIN] Completed %u C++ global constructors! Entering _main (0x00064F90)...\n", ctorCount);
                    mainCtx.push32(uwp_recomp::kReturnSentinelVA);
                    uwp_recomp::CallGuestVA(mainCtx, 0x00064F90u);
                    std::printf("[UWP-MAIN] Lifted _main returned (EAX=0x%08X)\n", mainCtx.eax);
                    break;
                } catch (const uwp_recomp::GuestRelaunchException&) {
                    std::printf("[UWP-RELAUNCH] Seamless in-process stage transition (keeping window, D3D11 & config)...\n");
                    g_mainCtxPtr = nullptr;
                    uwp_recomp::host_bridges::ResetForRelaunch();
                    uwp_recomp::xaudio2_bridge::Shutdown();
                    uwp_recomp::xaudio2_bridge::Init();
                    uwp_recomp::d3d11_bridge::ResetForRelaunch();
                    uwp_recomp::ResetGuestMemoryAndReloadXbe();
                }
            }
            return 0;
        }, nullptr, 0, nullptr);
    }

    std::printf("[UWP-MAIN] Phase 4 x64 UWP-compatible runtime ready! Guest Entry VA = 0x%08X (Press Esc to exit)\n", entryVA);

    MSG msg{};
    bool running = true;
    uint32_t lastFrame = 0;
    ULONGLONG lastFrameTick = GetTickCount64();
    uint32_t stallLogs = 0;
    while (running) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!bootMain) {
            // Subtle gothic crimson-midnight clear + D3D11 vs_4_0/ps_4_0 RHW pipeline self-test quad
            uwp_recomp::d3d11_bridge::Clear(0x000000F3u, 0xFF140A18u, 1.0f);
            struct RhwVert { float x, y, z, rhw; uint32_t color; };
            static const RhwVert kQuad[4] = {
                { 120.0f, 160.0f, 0.5f, 1.0f, 0xFFE91E63u },
                { 520.0f, 160.0f, 0.5f, 1.0f, 0xFF880E4Fu },
                { 120.0f, 320.0f, 0.5f, 1.0f, 0xFF311B92u },
                { 520.0f, 320.0f, 0.5f, 1.0f, 0xFF1A237Eu },
            };
            uwp_recomp::d3d11_bridge::SetVertexShader(0x0044u); // D3DFVF_XYZRHW | D3DFVF_DIFFUSE
            uwp_recomp::d3d11_bridge::SetTexture(0, nullptr, 0);
            uwp_recomp::d3d11_bridge::DrawVerticesUP(6u, 4u, reinterpret_cast<const uint8_t*>(kQuad), sizeof(RhwVert));
            uwp_recomp::d3d11_bridge::Present(true);
        } else {
            uint32_t curFrame = uwp_recomp::d3d11_bridge::GetFrameCount();
            ULONGLONG now = GetTickCount64();
            if (curFrame != lastFrame) {
                lastFrame = curFrame;
                lastFrameTick = now;
                stallLogs = 0;
            } else if (now - lastFrameTick >= (curFrame > 0 ? 3000ULL : 8000ULL) && stallLogs < 5u) {
                ++stallLogs;
                lastFrameTick = now;
                std::printf("[UWP-WATCHDOG] Still loading / no Present() (frame=%u):\n", curFrame);
                if (g_mainCtxPtr && g_mainCtxPtr->mem) {
                    uint32_t sp = g_mainCtxPtr->esp;
                    std::printf("[UWP-STACK] ESP=0x%08X:", sp);
                    int found = 0;
                    for (uint32_t off = 0; off < 1024u && found < 16; off += 4u) {
                        uint32_t v = g_mainCtxPtr->r32(sp + off);
                        if (v >= 0x00011000u && v < 0x00616D80u) {
                            std::printf(" 0x%08X", v);
                            ++found;
                        }
                    }
                    std::printf("\n");
                }
                uwp_recomp::DumpRecentGuestCalls();
            }
            Sleep(16);
        }
    }

    if (bootMain) {
        std::printf("[UWP-EXIT] Exiting at frame=%u, recent guest calls:\n", uwp_recomp::d3d11_bridge::GetFrameCount());
        uwp_recomp::DumpRecentGuestCalls();
        std::fflush(stdout);
        ExitProcess(0); // cleanly terminate background guest thread before tearing down D3D11
    }

    uwp_recomp::xaudio2_bridge::Shutdown();
    uwp_recomp::d3d11_bridge::Shutdown();
    uwp_recomp::ShutdownGuestMemory();
    CoUninitialize();
    return 0;
}
