// BuffyWin32Recomp.exe - fixed-base launcher (ImageBase = 0x00010000)
//
// Built with /NODEFAULTLIB /ENTRY:BuffyLauncherEntry and #pragma code_seg(".zhost")
// so that .text contains ONLY g_XboxReservation[0x00920000] at RVA 0x00001000
// (VA 0x00011000..0x00931000), while all launcher code (.zhost) and imports (.rdata)
// live safely above 0x00931000.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static constexpr unsigned kXboxStart = 0x00011000u;
static constexpr unsigned kXboxEnd   = 0x00921000u;
static constexpr unsigned kReserve   = 0x00920000u; // 0x00011000..0x00931000

// Section #1: .text -> VA 0x00011000 .. 0x00931000
#pragma section(".text", read, execute)
__declspec(allocate(".text")) const unsigned char g_XboxReservation[kReserve] = { 0 };

// Place all launcher code in .zhost (alphabetically after .text -> VA >= 0x00931000)
#pragma code_seg(".zhost")

static void PrintStr(const char* s) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD len = 0;
    while (s[len]) ++len;
    DWORD written = 0;
    WriteFile(hOut, s, len, &written, nullptr);
}

static void PrintHex(unsigned v) {
    char buf[11];
    buf[0] = '0';
    buf[1] = 'x';
    const char* hex = "0123456789ABCDEF";
    for (int i = 0; i < 8; ++i) {
        buf[2 + i] = hex[(v >> (28 - i * 4)) & 0xFu];
    }
    buf[10] = '\0';
    PrintStr(buf);
}

static bool HasArg(const char* needle) {
    const char* cl = GetCommandLineA();
    if (!cl) return false;
    for (const char* p = cl; *p; ++p) {
        const char* a = p;
        const char* b = needle;
        while (*a && *b && *a == *b) { ++a; ++b; }
        if (*b == '\0') return true;
    }
    return false;
}

static int Fail(const char* msg, DWORD err) {
    PrintStr("[LAUNCHER-ERROR] ");
    PrintStr(msg);
    PrintStr(" (Win32 error ");
    PrintHex(err);
    PrintStr(")\r\n");
    if (!HasArg("--headless")) {
        PrintStr("Press Enter to exit...\r\n");
        char ch = 0;
        DWORD read = 0;
        ReadFile(GetStdHandle(STD_INPUT_HANDLE), &ch, 1, &read, nullptr);
    }
    return 1;
}

extern "C" void __stdcall BuffyLauncherEntry() {
    const unsigned exeBase = static_cast<unsigned>(reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)));
    const unsigned resBeg  = static_cast<unsigned>(reinterpret_cast<uintptr_t>(&g_XboxReservation[0]));
    const unsigned resEnd  = resBeg + kReserve;
    const unsigned codeVa  = static_cast<unsigned>(reinterpret_cast<uintptr_t>(&BuffyLauncherEntry));

    PrintStr("[LAUNCHER] Exe base ");
    PrintHex(exeBase);
    PrintStr(", Xbox reservation ");
    PrintHex(resBeg);
    PrintStr("..");
    PrintHex(resEnd);
    PrintStr(", launcher code @ ");
    PrintHex(codeVa);
    PrintStr("\r\n");

    if (exeBase != 0x00010000u) {
        ExitProcess(Fail("Executable was not loaded at 0x00010000", 0));
    }
    if (resBeg > kXboxStart || resEnd < kXboxEnd) {
        ExitProcess(Fail("Xbox reservation does not cover 0x00011000..0x00921000", 0));
    }

    // Locate BuffyRecompRuntime.dll next to this exe
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        ExitProcess(Fail("GetModuleFileNameA failed", GetLastError()));
    }
    int slash = -1;
    for (int i = 0; i < static_cast<int>(n); ++i) {
        if (path[i] == '\\' || path[i] == '/') slash = i;
    }
    const char* dllName = "BuffyRecompRuntime.dll";
    int dst = slash + 1;
    for (int i = 0; dllName[i] && dst < MAX_PATH - 1; ++i) {
        path[dst++] = dllName[i];
    }
    path[dst] = '\0';

    HMODULE rt = LoadLibraryA(path);
    if (!rt) {
        ExitProcess(Fail("Could not load BuffyRecompRuntime.dll", GetLastError()));
    }

    using MainFn = int(__cdecl*)();
    auto runtimeMain = reinterpret_cast<MainFn>(GetProcAddress(rt, "BuffyRuntimeMain"));
    if (!runtimeMain) {
        ExitProcess(Fail("BuffyRuntimeMain export not found", GetLastError()));
    }

    PrintStr("[LAUNCHER] BuffyRecompRuntime.dll loaded at ");
    PrintHex(static_cast<unsigned>(reinterpret_cast<uintptr_t>(rt)));
    PrintStr(", entering runtime...\r\n");

    int rc = runtimeMain();
    ExitProcess(static_cast<UINT>(rc));
}
