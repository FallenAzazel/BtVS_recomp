#include "xbe_loader.h"
#include "slate_engine.h"
#include "d3d9_bridge.h"
#include "dsound_bridge.h"
#include <mmsystem.h>
#include <dshow.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "strmiids.lib")

namespace xbe_loader {

static HWND      g_hWnd        = nullptr;
static uint32_t* g_framebuffer = nullptr; // 640x480 0x00RRGGBB
static volatile uint32_t g_frameCount   = 0;
static volatile DWORD    g_lastSwapTick = 0;
static HANDLE            g_hMainGameThread = nullptr;
static uint32_t  g_drawCalls   = 0;
static uint32_t  g_clearColor  = 0x00101420u;
static std::string g_lastMovieName;
static std::string g_lastHudStrings[8];
static int       g_hudStringIdx = 0;

void SetPresentationWindow(HWND hWnd, uint32_t* framebuffer640x480) {
    g_hWnd        = hWnd;
    g_framebuffer = framebuffer640x480;
}

// ============================================================================
// Symbol Lookup & Vectored Exception Handler
// ============================================================================
struct XboxFuncSym {
    uint32_t    va;
    uint32_t    size;
    std::string obj;
    std::string name;
};

static std::vector<XboxFuncSym> g_syms;

void LoadSymbolTable(const char* functionsTsvPath) {
    if (!functionsTsvPath) return;
    FILE* f = std::fopen(functionsTsvPath, "r");
    if (!f) return;
    g_syms.clear();
    g_syms.reserve(42000);
    char line[2048]{};
    // Skip header
    std::fgets(line, sizeof(line), f);
    while (std::fgets(line, sizeof(line), f)) {
        char* cols[10]{};
        int c = 0;
        for (char* tok = std::strtok(line, "\t\r\n"); tok && c < 10; tok = std::strtok(nullptr, "\t\r\n")) {
            cols[c++] = tok;
        }
        if (c >= 9 && cols[0][0] == '0' && cols[0][1] == 'x') {
            XboxFuncSym s{};
            s.va   = static_cast<uint32_t>(std::strtoul(cols[0], nullptr, 16));
            s.size = static_cast<uint32_t>(std::strtoul(cols[2], nullptr, 10));
            s.obj  = cols[5];
            s.name = (c >= 10 && cols[9][0]) ? cols[9] : cols[8];
            g_syms.push_back(std::move(s));
        }
    }
    std::fclose(f);
    std::sort(g_syms.begin(), g_syms.end(), [](const XboxFuncSym& a, const XboxFuncSym& b) {
        return a.va < b.va;
    });
    LogPrintf("[Symbols] Loaded %zu Xbox function symbols for live stack/exception resolution.\n", g_syms.size());
}

void FormatXboxSymbol(uint32_t va, char* outBuf, size_t maxLen) {
    if (!outBuf || maxLen == 0) return;
    if (g_syms.empty() || va < 0x00011000 || va >= 0x00921000) {
        std::snprintf(outBuf, maxLen, "0x%08X", va);
        return;
    }
    auto it = std::upper_bound(g_syms.begin(), g_syms.end(), va, [](uint32_t v, const XboxFuncSym& s) {
        return v < s.va;
    });
    if (it != g_syms.begin()) {
        --it;
        uint32_t off = va - it->va;
        if (off < std::max<uint32_t>(it->size, 0x1000u)) {
            std::snprintf(outBuf, maxLen, "0x%08X (%s!%s + 0x%X)", va, it->obj.c_str(), it->name.c_str(), off);
            return;
        }
    }
    std::snprintf(outBuf, maxLen, "0x%08X", va);
}

static LONG WINAPI XboxVectoredExceptionHandler(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    // Ignore benign Win32 thread naming / OutputDebugString exceptions
    if (code == 0x406D1388 || code == 0x40010006 || code == 0x4001000A) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // Log all fatal NTSTATUS exceptions (0xC0000000..0xCFFFFFFF), C++ exceptions (0xE06D7363),
    // and Xbox int3 breakpoints (0x80000003, e.g. StdBreakpoint).
    bool isFatalNt   = ((code & 0xF0000000u) == 0xC0000000u);
    bool isBreakpoint = (code == EXCEPTION_BREAKPOINT);
    bool isCppExcept  = (code == 0xE06D7363u);
    if (!isFatalNt && !isBreakpoint && !isCppExcept) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    static int s_crashCount = 0;
    if (!isBreakpoint && ++s_crashCount > 5) {
        ExitProcess(1);
    }

    CONTEXT* ctx = ep->ContextRecord;
    static char eipSym[512];
    FormatXboxSymbol(ctx->Eip, eipSym, sizeof(eipSym));

    static char crashPath[512];
    GetModuleFileNameA(nullptr, crashPath, sizeof(crashPath));
    if (char* sl = std::strrchr(crashPath, '\\')) {
        *sl = '\0';
        if (char* sl2 = std::strrchr(crashPath, '\\')) {
            std::strcpy(sl2 + 1, "crash.log");
        } else {
            std::strcpy(crashPath, "crash.log");
        }
    } else {
        std::strcpy(crashPath, "crash.log");
    }
    FILE* cf = std::fopen(crashPath, "w");
    auto logBoth = [&](const char* fmt, ...) {
        static char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        LogPrintf("%s", buf);
        if (cf) {
            std::fputs(buf, cf);
            std::fflush(cf);
        }
    };

    logBoth("\n====================================================================\n");
    logBoth("[EXCEPTION] Code=0x%08lX at EIP=%s\n", code, eipSym);
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        logBoth("            Faulting Address: 0x%08lX (%s)\n",
                static_cast<unsigned long>(ep->ExceptionRecord->ExceptionInformation[1]),
                ep->ExceptionRecord->ExceptionInformation[0] ? "WRITE" : "READ");
    }
    logBoth("  EAX=0x%08lX EBX=0x%08lX ECX=0x%08lX EDX=0x%08lX\n", ctx->Eax, ctx->Ebx, ctx->Ecx, ctx->Edx);
    logBoth("  ESI=0x%08lX EDI=0x%08lX EBP=0x%08lX ESP=0x%08lX\n", ctx->Esi, ctx->Edi, ctx->Ebp, ctx->Esp);

    // Dump instruction bytes at EIP
    if (!IsBadReadPtr(reinterpret_cast<const void*>(ctx->Eip), 16)) {
        const uint8_t* ins = reinterpret_cast<const uint8_t*>(ctx->Eip);
        logBoth("  Bytes @ EIP: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X\n",
                ins[0], ins[1], ins[2], ins[3], ins[4], ins[5], ins[6], ins[7],
                ins[8], ins[9], ins[10], ins[11], ins[12], ins[13], ins[14], ins[15]);
    }

    // Walk EBP/ESP stack frames
    logBoth("  Stack Trace:\n");
    uint32_t* sp = reinterpret_cast<uint32_t*>(ctx->Esp);
    int printed = 0;
    for (int i = 0; i < 64 && printed < 16; ++i) {
        if (IsBadReadPtr(sp + i, 4)) break;
        uint32_t val = sp[i];
        if (val >= 0x00011000 && val < 0x00616D80) {
            char sym[512]{};
            FormatXboxSymbol(val, sym, sizeof(sym));
            logBoth("    [ESP+0x%02X] %s\n", i * 4, sym);
            ++printed;
        }
    }
    logBoth("====================================================================\n");
    if (cf) std::fclose(cf);
    if (isBreakpoint && ctx->Eip >= 0x00011000 && ctx->Eip < 0x00616D80 &&
        !IsBadReadPtr(reinterpret_cast<const void*>(ctx->Eip), 1) &&
        *reinterpret_cast<const uint8_t*>(ctx->Eip) == 0xCC) {
        ctx->Eip += 1;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ============================================================================
// 5-Byte x86 Relative Jump Detour Helper
// ============================================================================
static void InstallDetour(uint32_t xboxVa, void* targetFn) {
    uint8_t* p = reinterpret_cast<uint8_t*>(xboxVa);
    int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(targetFn) - (xboxVa + 5));
    p[0] = 0xE9; // JMP rel32
    *reinterpret_cast<int32_t*>(p + 1) = rel;
}

// ============================================================================
// 1. LIBCMT Per-Thread Data (_tiddata) & Multithreaded Locks
// ============================================================================
static DWORD g_ptdTlsIndex = TLS_OUT_OF_INDEXES;

static void* InitThreadPtd() {
    if (g_ptdTlsIndex == TLS_OUT_OF_INDEXES) {
        g_ptdTlsIndex = TlsAlloc();
    }
    uint32_t* ptd = static_cast<uint32_t*>(TlsGetValue(g_ptdTlsIndex));
    if (!ptd) {
        ptd = static_cast<uint32_t*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 256));
        ptd[0] = GetCurrentThreadId(); // _tid
        ptd[1] = 0xFFFFFFFFu;          // _thandle
        ptd[5] = 1;                    // _holdrand
        ptd[0x54 / 4] = 0x00774A00u;   // _ptmbcinfo -> ___initialmbcinfo in Xbox .data
        TlsSetValue(g_ptdTlsIndex, ptd);
    }
    return ptd;
}

static int __cdecl Detour_mtinit() {
    InitThreadPtd();
    // Initialize the 36 LIBCMT lock table entries at 0x00774240 using preallocated 28-byte slots at 0x00911828
    uint8_t*  preallocCs = reinterpret_cast<uint8_t*>(0x00911828);
    uint32_t* lockTable  = reinterpret_cast<uint32_t*>(0x00774240);
    for (int i = 0; i < 36; ++i) {
        if (lockTable[i * 2 + 1] == 1) {
            lockTable[i * 2] = reinterpret_cast<uint32_t>(preallocCs);
            std::memset(preallocCs, 0, 0x1C);
            *reinterpret_cast<int32_t*>(preallocCs + 0x10) = -1;
            preallocCs += 0x1C;
        }
    }
    return 1;
}

static int __cdecl Detour_mtinitlocknum(int locknum) {
    if (locknum < 0 || locknum >= 36) return 1;
    uint32_t* lockTable = reinterpret_cast<uint32_t*>(0x00774240);
    if (lockTable[locknum * 2] == 0) {
        uint8_t* cs = static_cast<uint8_t*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x1C));
        if (!cs) return 0;
        *reinterpret_cast<int32_t*>(cs + 0x10) = -1;
        lockTable[locknum * 2] = reinterpret_cast<uint32_t>(cs);
    }
    return 1;
}

static void* __cdecl Detour_getptd() {
    return InitThreadPtd();
}

static void __cdecl Detour_freeptd(void* ptd) {
    if (g_ptdTlsIndex != TLS_OUT_OF_INDEXES) {
        if (!ptd) {
            ptd = TlsGetValue(g_ptdTlsIndex);
            TlsSetValue(g_ptdTlsIndex, nullptr);
        }
    }
    if (ptd) HeapFree(GetProcessHeap(), 0, ptd);
}

// ============================================================================
// 2. Heap, Virtual Memory & GlobalMemoryStatus Shims
// ============================================================================
static HANDLE __stdcall Detour_RtlCreateHeap(
    uint32_t /*Flags*/,
    void* /*HeapBase*/,
    uint32_t /*ReserveSize*/,
    uint32_t /*CommitSize*/,
    void* /*Lock*/,
    void* /*Parameters*/
) {
    // Share the single unified process heap so cross-heap frees between _XapiProcessHeap and __crtheap never fault
    return GetProcessHeap();
}

static void* __stdcall Detour_RtlAllocateHeap(HANDLE /*HeapHandle*/, uint32_t Flags, uint32_t Size) {
    if (Size > 64u * 1024u * 1024u) {
        LogPrintf("[HEAP-HUGE] RtlAllocateHeap(Size=%u / 0x%08X)\n", Size, Size);
        uint32_t* sp = reinterpret_cast<uint32_t*>(&Size);
        int printed = 0;
        for (int i = 0; i < 256 && printed < 16; ++i) {
            if (IsBadReadPtr(sp + i, 4)) break;
            uint32_t val = sp[i];
            if (val >= 0x00011000 && val < 0x00616D80) {
                char sym[512]{};
                FormatXboxSymbol(val, sym, sizeof(sym));
                LogPrintf("  [Heap Stack +0x%03X] %s\n", i * 4, sym);
                ++printed;
            }
        }
    }
    DWORD winFlags = (Flags & HEAP_ZERO_MEMORY) ? HEAP_ZERO_MEMORY : 0;
    return HeapAlloc(GetProcessHeap(), winFlags, Size ? Size : 16);
}

static uint8_t __stdcall Detour_RtlFreeHeap(HANDLE /*HeapHandle*/, uint32_t /*Flags*/, void* BaseAddress) {
    if (!BaseAddress) return 1;
    uintptr_t addr = reinterpret_cast<uintptr_t>(BaseAddress);
    if (addr < 0x00940000u) return 1; // Never free static Xbox image addresses
    return HeapFree(GetProcessHeap(), 0, BaseAddress) ? 1 : 0;
}

static void* __stdcall Detour_RtlReAllocateHeap(HANDLE /*HeapHandle*/, uint32_t Flags, void* BaseAddress, uint32_t Size) {
    DWORD winFlags = (Flags & HEAP_ZERO_MEMORY) ? HEAP_ZERO_MEMORY : 0;
    if (!BaseAddress) return HeapAlloc(GetProcessHeap(), winFlags, Size ? Size : 16);
    return HeapReAlloc(GetProcessHeap(), winFlags, BaseAddress, Size ? Size : 16);
}

static uint32_t __stdcall Detour_RtlSizeHeap(HANDLE /*HeapHandle*/, uint32_t /*Flags*/, void* BaseAddress) {
    if (!BaseAddress) return 0;
    return static_cast<uint32_t>(HeapSize(GetProcessHeap(), 0, BaseAddress));
}

static void __stdcall Detour_GlobalMemoryStatus(MEMORYSTATUS* lpBuffer) {
    if (!lpBuffer) return;
    std::memset(lpBuffer, 0, sizeof(*lpBuffer));
    lpBuffer->dwLength        = sizeof(*lpBuffer);
    lpBuffer->dwMemoryLoad    = 25;
    lpBuffer->dwTotalPhys     = 64 * 1024 * 1024; // 64 MB Xbox RAM
    lpBuffer->dwAvailPhys     = 48 * 1024 * 1024; // 48 MB available
    lpBuffer->dwTotalPageFile = 64 * 1024 * 1024;
    lpBuffer->dwAvailPageFile = 48 * 1024 * 1024;
    lpBuffer->dwTotalVirtual  = 64 * 1024 * 1024;
    lpBuffer->dwAvailVirtual  = 48 * 1024 * 1024;
}

static void* __stdcall Detour_VirtualAlloc(void* lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect) {
    DWORD type = flAllocationType & (MEM_COMMIT | MEM_RESERVE);
    if (type == 0) type = MEM_COMMIT | MEM_RESERVE;
    DWORD prot = flProtect ? flProtect : PAGE_READWRITE;
    void* p = VirtualAlloc(lpAddress, dwSize ? dwSize : 4096, type, prot);
    if (!p && lpAddress) {
        p = VirtualAlloc(nullptr, dwSize ? dwSize : 4096, MEM_COMMIT | MEM_RESERVE, prot);
    }
    return p;
}

static BOOL __stdcall Detour_VirtualFree(void* lpAddress, SIZE_T dwSize, DWORD dwFreeType) {
    if (!lpAddress) return TRUE;
    if (dwFreeType & MEM_RELEASE) return VirtualFree(lpAddress, 0, MEM_RELEASE);
    return VirtualFree(lpAddress, dwSize, MEM_DECOMMIT);
}

// ============================================================================
// 3. Win32 / XAPI File System, Threads & OS Shims
// ============================================================================
static std::unordered_set<HANDLE> g_findHandles;

static DWORD __stdcall Detour_GetLastError() { return GetLastError(); }
static void  __stdcall Detour_SetLastError(DWORD err) { SetLastError(err); }
static DWORD __stdcall Detour_GetCurrentThreadId() { return GetCurrentThreadId(); }

// ============================================================================
// Buffy Costume Extractor & Runtime .pak Swapper
// (0=Level Default, 1=Slayer, 2=Cheerleader, 3=The Bronze, 4=Workout/Sport)
// Extracts costumes locally from user's extracted\pak\ into recomp\cache\costumes\
// ============================================================================
static uint32_t g_buffyCostume = 0;

struct CostumeAssets {
    std::vector<uint8_t> mesh;      // raw .msh payload
    std::vector<uint8_t> faceEntry; // full .pak entry (header + name + sub + payload)
    std::vector<uint8_t> bodyEntry; // full .pak entry
    std::vector<uint8_t> legEntry;  // full .pak entry
    std::vector<uint8_t> ban;       // raw .ban payload
    bool valid() const {
        return !mesh.empty() && !faceEntry.empty() && !bodyEntry.empty() && !legEntry.empty() && !ban.empty();
    }
};

static bool ReadWholeFile(const char* path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    if (sz <= 0) { std::fclose(f); return false; }
    std::fseek(f, 0, SEEK_SET);
    out.resize(static_cast<size_t>(sz));
    bool ok = (std::fread(out.data(), 1, static_cast<size_t>(sz), f) == static_cast<size_t>(sz));
    std::fclose(f);
    return ok;
}

static bool WriteWholeFile(const char* path, const void* data, size_t len) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    bool ok = (len == 0 || std::fwrite(data, 1, len, f) == len);
    std::fclose(f);
    return ok;
}

static void EnsureDirTree(const char* dirPath) {
    char buf[512]{};
    std::strncpy(buf, dirPath, sizeof(buf) - 1);
    for (char* p = buf; *p; ++p) {
        if (*p == '\\' || *p == '/') {
            char saved = *p;
            *p = '\0';
            if (p != buf && *(p - 1) != ':') CreateDirectoryA(buf, nullptr);
            *p = saved;
        }
    }
    CreateDirectoryA(buf, nullptr);
}

static bool StrEndsWithI(const std::string& s, const char* suf) {
    size_t sl = std::strlen(suf);
    return s.size() >= sl && _stricmp(s.c_str() + s.size() - sl, suf) == 0;
}

static bool StrStartsWithI(const std::string& s, const char* pre) {
    size_t pl = std::strlen(pre);
    return s.size() >= pl && _strnicmp(s.c_str(), pre, pl) == 0;
}

static size_t FindFirstPakEntry(const uint8_t* d, size_t len) {
    size_t scanMax = (len < 0x200000u) ? len : 0x200000u;
    for (size_t off = 0; off + 32 <= scanMax; ++off) {
        uint32_t typ, fo, tot, pay, flg, nlen;
        std::memcpy(&typ,  d + off + 0,  4);
        if (typ != 1u) continue;
        std::memcpy(&fo,   d + off + 4,  4);
        if (fo != static_cast<uint32_t>(off)) continue;
        std::memcpy(&tot,  d + off + 8,  4);
        std::memcpy(&pay,  d + off + 12, 4);
        std::memcpy(&flg,  d + off + 16, 4);
        std::memcpy(&nlen, d + off + 20, 4);
        if (nlen < 4u || nlen > 128u || tot <= 24u + nlen + 8u || off + tot + 24u > len) continue;
        bool asciiOk = true;
        for (uint32_t k = 0; k < nlen; ++k) {
            uint8_t c = d[off + 24u + k];
            if (c < 32 || c > 126) { asciiOk = false; break; }
        }
        if (!asciiOk) continue;
        // Verify the immediately following entry also chains cleanly
        uint32_t typ2 = 0, fo2 = 0;
        std::memcpy(&typ2, d + off + tot + 0, 4);
        std::memcpy(&fo2,  d + off + tot + 4, 4);
        if (typ2 == 1u && fo2 == static_cast<uint32_t>(off + tot)) {
            return off;
        }
    }
    return static_cast<size_t>(-1);
}

static bool LoadOrExtractCostume(uint32_t costumeId, CostumeAssets& out) {
    static const char* kTags[5] = { "", "slayer", "cheer", "bronze", "workout" };
    static const char* kSrcPaks[5] = {
        "",
        "D:\\pak\\res_m00_spanishmission_1.pak",
        "D:\\pak\\res_m01_highschool_1.pak",
        "D:\\pak\\res_m02_bronze_0.pak",
        "D:\\pak\\res_m06_mansion_1.pak"
    };
    if (costumeId < 1 || costumeId > 4) return false;

    char cacheDir[512]{};
    char xboxDir[128]{};
    std::snprintf(xboxDir, sizeof(xboxDir), "Z:\\costumes\\%s", kTags[costumeId]);
    xboxkrnl::TranslateXboxPath(xboxDir, cacheDir, sizeof(cacheDir));
    EnsureDirTree(cacheDir);

    std::string pMesh = std::string(cacheDir) + "\\mesh.bin";
    std::string pFace = std::string(cacheDir) + "\\face.entry";
    std::string pBody = std::string(cacheDir) + "\\body.entry";
    std::string pLeg  = std::string(cacheDir) + "\\leg.entry";
    std::string pBan  = std::string(cacheDir) + "\\ban.bin";

    if (ReadWholeFile(pMesh.c_str(), out.mesh) &&
        ReadWholeFile(pFace.c_str(), out.faceEntry) &&
        ReadWholeFile(pBody.c_str(), out.bodyEntry) &&
        ReadWholeFile(pLeg.c_str(),  out.legEntry) &&
        ReadWholeFile(pBan.c_str(),  out.ban)) {
        return out.valid();
    }

    char srcPakWin[512]{};
    xboxkrnl::TranslateXboxPath(kSrcPaks[costumeId], srcPakWin, sizeof(srcPakWin));
    std::vector<uint8_t> pak;
    if (!ReadWholeFile(srcPakWin, pak)) {
        LogPrintf("[Costume] Failed to open source pak '%s' for '%s'\n", srcPakWin, kTags[costumeId]);
        return false;
    }
    size_t cur = FindFirstPakEntry(pak.data(), pak.size());
    if (cur == static_cast<size_t>(-1)) return false;

    while (cur + 24u <= pak.size()) {
        uint32_t typ, fo, tot, pay, flg, nlen;
        std::memcpy(&typ,  pak.data() + cur + 0,  4);
        std::memcpy(&fo,   pak.data() + cur + 4,  4);
        std::memcpy(&tot,  pak.data() + cur + 8,  4);
        std::memcpy(&pay,  pak.data() + cur + 12, 4);
        std::memcpy(&flg,  pak.data() + cur + 16, 4);
        std::memcpy(&nlen, pak.data() + cur + 20, 4);
        if (typ != 1u || fo != static_cast<uint32_t>(cur) || tot == 0 || cur + tot > pak.size()) break;
        if (24u + nlen + 8u <= tot) {
            std::string name(reinterpret_cast<const char*>(pak.data() + cur + 24u), nlen);
            uint32_t snlen = 0;
            std::memcpy(&snlen, pak.data() + cur + 24u + nlen, 4);
            size_t hdrBytes = 24u + nlen + 4u + snlen + 4u;
            if (hdrBytes <= tot && StrStartsWithI(name, "meshes\\chars\\slayer\\buffy")) {
                std::string sub(reinterpret_cast<const char*>(pak.data() + cur + 24u + nlen + 4u), snlen);
                if (StrEndsWithI(name, ".msh") && _stricmp(sub.c_str(), "base") == 0 && out.mesh.empty()) {
                    out.mesh.assign(pak.data() + cur + hdrBytes, pak.data() + cur + tot);
                } else if (StrEndsWithI(name, ".mtx")) {
                    if (StrEndsWithI(sub, "_face") && out.faceEntry.empty()) {
                        out.faceEntry.assign(pak.data() + cur, pak.data() + cur + tot);
                    } else if (StrEndsWithI(sub, "_body") && out.bodyEntry.empty()) {
                        out.bodyEntry.assign(pak.data() + cur, pak.data() + cur + tot);
                    } else if (StrEndsWithI(sub, "_leg") && out.legEntry.empty()) {
                        out.legEntry.assign(pak.data() + cur, pak.data() + cur + tot);
                    }
                } else if (StrEndsWithI(name, ".ban") && out.ban.empty()) {
                    out.ban.assign(pak.data() + cur + hdrBytes, pak.data() + cur + tot);
                }
            }
        }
        cur += tot;
    }

    if (!out.valid()) {
        LogPrintf("[Costume] Incomplete extraction for '%s' (msh=%u face=%u body=%u leg=%u ban=%u)\n",
                  kTags[costumeId],
                  static_cast<unsigned>(out.mesh.size()), static_cast<unsigned>(out.faceEntry.size()),
                  static_cast<unsigned>(out.bodyEntry.size()), static_cast<unsigned>(out.legEntry.size()),
                  static_cast<unsigned>(out.ban.size()));
        return false;
    }
    WriteWholeFile(pMesh.c_str(), out.mesh.data(), out.mesh.size());
    WriteWholeFile(pFace.c_str(), out.faceEntry.data(), out.faceEntry.size());
    WriteWholeFile(pBody.c_str(), out.bodyEntry.data(), out.bodyEntry.size());
    WriteWholeFile(pLeg.c_str(),  out.legEntry.data(),  out.legEntry.size());
    WriteWholeFile(pBan.c_str(),  out.ban.data(),       out.ban.size());
    LogPrintf("[Costume] Extracted '%s' to %s (msh=%u B, ban=%u B)\n",
              kTags[costumeId], cacheDir, static_cast<unsigned>(out.mesh.size()), static_cast<unsigned>(out.ban.size()));
    return true;
}

static bool BuildCostumePatchedPak(const char* srcPakWinPath, uint32_t costumeId, char* outPakWinPath, size_t outMax) {
    char cosRoot[512]{};
    xboxkrnl::TranslateXboxPath("Z:\\costumes", cosRoot, sizeof(cosRoot));
    EnsureDirTree(cosRoot);

    std::string activePak = std::string(cosRoot) + "\\active_costume.pak";
    std::string activeTag = std::string(cosRoot) + "\\active_costume.tag";
    std::string wantTag   = std::string(srcPakWinPath) + "|" + std::to_string(costumeId) + "|v3";

    // Fast path: reuse already-built patched pak if same level & costume
    std::vector<uint8_t> existingTag;
    if (ReadWholeFile(activeTag.c_str(), existingTag) &&
        existingTag.size() == wantTag.size() &&
        std::memcmp(existingTag.data(), wantTag.data(), wantTag.size()) == 0 &&
        GetFileAttributesA(activePak.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::strncpy(outPakWinPath, activePak.c_str(), outMax - 1);
        outPakWinPath[outMax - 1] = '\0';
        return true;
    }

    CostumeAssets cd{};
    if (!LoadOrExtractCostume(costumeId, cd)) return false;

    DWORD t0 = GetTickCount();
    std::vector<uint8_t> src;
    if (!ReadWholeFile(srcPakWinPath, src)) return false;
    size_t firstEntry = FindFirstPakEntry(src.data(), src.size());
    if (firstEntry == static_cast<size_t>(-1)) return false;

    std::vector<uint8_t> dst;
    dst.reserve(src.size() + 65536u);
    dst.insert(dst.end(), src.data(), src.data() + firstEntry);

    // Helper: case-insensitive in-place same-length byte replacement (avoids texture-cache collisions
    // in multi-costume paks like res_m00_spanishmission_3.pak which has both buffyslayer & buffyworkout!)
    auto replaceAllSameLenI = [](uint8_t* buf, size_t len, const char* fromStr, const char* toStr) {
        size_t sl = std::strlen(fromStr);
        if (sl == 0 || sl != std::strlen(toStr) || len < sl) return;
        for (size_t i = 0; i + sl <= len; ++i) {
            if (_strnicmp(reinterpret_cast<const char*>(buf + i), fromStr, sl) == 0) {
                std::memcpy(buf + i, toStr, sl);
                i += sl - 1;
            }
        }
    };

    // Check if this pak contains multiple Buffy meshes (e.g. res_m00_spanishmission_3.pak)
    int buffyMeshCount = 0;
    {
        size_t scan = firstEntry;
        while (scan + 24u <= src.size()) {
            uint32_t typ, fo, tot, pay, flg, nlen;
            std::memcpy(&typ,  src.data() + scan + 0,  4);
            std::memcpy(&fo,   src.data() + scan + 4,  4);
            std::memcpy(&tot,  src.data() + scan + 8,  4);
            std::memcpy(&pay,  src.data() + scan + 12, 4);
            std::memcpy(&flg,  src.data() + scan + 16, 4);
            std::memcpy(&nlen, src.data() + scan + 20, 4);
            if (typ != 1u || fo != static_cast<uint32_t>(scan) || tot == 0 || scan + tot > src.size()) break;
            if (24u + nlen + 8u <= tot) {
                std::string name(reinterpret_cast<const char*>(src.data() + scan + 24u), nlen);
                if (StrStartsWithI(name, "meshes\\chars\\slayer\\buffy") && StrEndsWithI(name, ".msh")) {
                    ++buffyMeshCount;
                }
            }
            scan += tot;
        }
    }
    const bool multiCostumePak = (buffyMeshCount > 1);
    int swappedMshCount = 0;
    int swappedMtxCount = 0;

    size_t cur = firstEntry;
    int replaced = 0;
    while (cur + 24u <= src.size()) {
        uint32_t typ, fo, tot, pay, flg, nlen;
        std::memcpy(&typ,  src.data() + cur + 0,  4);
        std::memcpy(&fo,   src.data() + cur + 4,  4);
        std::memcpy(&tot,  src.data() + cur + 8,  4);
        std::memcpy(&pay,  src.data() + cur + 12, 4);
        std::memcpy(&flg,  src.data() + cur + 16, 4);
        std::memcpy(&nlen, src.data() + cur + 20, 4);
        if (typ != 1u || fo != static_cast<uint32_t>(cur) || tot == 0 || cur + tot > src.size()) break;

        uint32_t newOff = static_cast<uint32_t>(dst.size());
        bool swapped = false;
        if (24u + nlen + 8u <= tot) {
            std::string name(reinterpret_cast<const char*>(src.data() + cur + 24u), nlen);
            uint32_t snlen = 0;
            std::memcpy(&snlen, src.data() + cur + 24u + nlen, 4);
            size_t hdrBytes = 24u + nlen + 4u + snlen + 4u;
            if (hdrBytes <= tot && StrStartsWithI(name, "meshes\\chars\\slayer\\buffy")) {
                std::string sub(reinterpret_cast<const char*>(src.data() + cur + 24u + nlen + 4u), snlen);
                if (StrEndsWithI(name, ".msh") && _stricmp(sub.c_str(), "base") == 0 && (!multiCostumePak || swappedMshCount == 0)) {
                    dst.insert(dst.end(), src.data() + cur, src.data() + cur + hdrBytes);
                    size_t payStart = dst.size();
                    dst.insert(dst.end(), cd.mesh.begin(), cd.mesh.end());
                    if (multiCostumePak) {
                        if (costumeId == 1) replaceAllSameLenI(dst.data() + payStart, cd.mesh.size(), "BuffySlayer", "BuffyCstm01");
                        else if (costumeId == 4) replaceAllSameLenI(dst.data() + payStart, cd.mesh.size(), "BuffySport", "BuffyCstm4");
                    }
                    uint32_t newPay = static_cast<uint32_t>(cd.mesh.size());
                    uint32_t newTot = static_cast<uint32_t>(hdrBytes) + newPay;
                    std::memcpy(dst.data() + newOff + 4,  &newOff, 4);
                    std::memcpy(dst.data() + newOff + 8,  &newTot, 4);
                    std::memcpy(dst.data() + newOff + 12, &newPay, 4);
                    swapped = true;
                    ++swappedMshCount;
                    ++replaced;
                } else if (StrEndsWithI(name, ".mtx") && (!multiCostumePak || swappedMtxCount < 3)) {
                    const std::vector<uint8_t>* rep = nullptr;
                    if (StrEndsWithI(sub, "_face")) rep = &cd.faceEntry;
                    else if (StrEndsWithI(sub, "_body")) rep = &cd.bodyEntry;
                    else if (StrEndsWithI(sub, "_leg"))  rep = &cd.legEntry;
                    if (rep) {
                        dst.insert(dst.end(), rep->begin(), rep->end());
                        if (multiCostumePak) {
                            // Rename only the entry's path/subname prefix in the header so it doesn't collide
                            // with the second Buffy mesh's textures later in the same .pak!
                            size_t hdrScan = std::min<size_t>(rep->size(), 128u);
                            if (costumeId == 1) replaceAllSameLenI(dst.data() + newOff + 24u, hdrScan - 24u, "buffyslayer", "buffycstm01");
                            else if (costumeId == 4) replaceAllSameLenI(dst.data() + newOff + 24u, hdrScan - 24u, "buffysport", "buffycstm4");
                        }
                        std::memcpy(dst.data() + newOff + 4, &newOff, 4);
                        swapped = true;
                        ++swappedMtxCount;
                        ++replaced;
                    }
                }
                // Note: keep the level's own .ban intact! Each level's .ban contains level-specific
                // animation bindings (cutscenes, props, etc.) that share the same Slayer skeleton.
            }
        }
        if (!swapped) {
            dst.insert(dst.end(), src.data() + cur, src.data() + cur + tot);
            std::memcpy(dst.data() + newOff + 4, &newOff, 4);
        }
        cur += tot;
    }

    // Copy 32-byte zero terminator / trailer verbatim
    if (cur < src.size()) {
        dst.insert(dst.end(), src.data() + cur, src.data() + src.size());
    }
    if (replaced == 0) return false;
    if (!WriteWholeFile(activePak.c_str(), dst.data(), dst.size())) return false;
    WriteWholeFile(activeTag.c_str(), wantTag.data(), wantTag.size());
    LogPrintf("[Costume] Patched '%s' -> costume=%u (%d entries swapped in %lu ms)\n",
              srcPakWinPath, costumeId, replaced, GetTickCount() - t0);
    std::strncpy(outPakWinPath, activePak.c_str(), outMax - 1);
    outPakWinPath[outMax - 1] = '\0';
    return true;
}

static HANDLE __stdcall Detour_CreateFileA(
    const char* lpFileName,
    DWORD dwDesiredAccess,
    DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes,
    HANDLE hTemplateFile
) {
    char winPath[512]{};
    xboxkrnl::TranslateXboxPath(lpFileName, winPath, sizeof(winPath));
    // Optional Buffy costume override on gameplay level .pak opens
    if (g_buffyCostume >= 1 && g_buffyCostume <= 8 && dwCreationDisposition == OPEN_EXISTING && winPath[0]) {
        // 1,5 -> Slayer mesh (1); 2,6,7,8 -> Cheerleader spaghetti-strap mesh (2); 3 -> Bronze (3); 4 -> Workout (4)
        const uint32_t meshCostume = (g_buffyCostume == 5) ? 1u
                                   : ((g_buffyCostume >= 6 && g_buffyCostume <= 8) ? 2u : g_buffyCostume);
        std::string wpLow(winPath);
        for (char& ch : wpLow) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (StrEndsWithI(wpLow, ".pak") && !StrEndsWithI(wpLow, "res_null.pak") &&
            wpLow.find("\\pak\\res_") != std::string::npos) {
            char patchedPak[512]{};
            if (BuildCostumePatchedPak(winPath, meshCostume, patchedPak, sizeof(patchedPak))) {
                std::strncpy(winPath, patchedPak, sizeof(winPath) - 1);
                winPath[sizeof(winPath) - 1] = '\0';
            }
        }
    }
    // Strip Xbox-specific file flags that Win32 doesn't use
    DWORD cleanFlags = dwFlagsAndAttributes & ~(0x08000000u);
    if (cleanFlags == 0) cleanFlags = FILE_ATTRIBUTE_NORMAL;
    HANDLE h = CreateFileA(winPath, dwDesiredAccess, dwShareMode ? dwShareMode : (FILE_SHARE_READ | FILE_SHARE_WRITE),
                           lpSecurityAttributes, dwCreationDisposition, cleanFlags, hTemplateFile);
    // Fallback between D:\ (extracted disc) and Z:\ (install.dat cache) on read-only misses
    if (h == INVALID_HANDLE_VALUE && lpFileName && dwCreationDisposition == OPEN_EXISTING &&
        (lpFileName[0] == 'd' || lpFileName[0] == 'D' || lpFileName[0] == 'z' || lpFileName[0] == 'Z') &&
        lpFileName[1] == ':') {
        std::string alt(lpFileName);
        alt[0] = (lpFileName[0] == 'd' || lpFileName[0] == 'D') ? 'Z' : 'D';
        char altWinPath[512]{};
        if (xboxkrnl::TranslateXboxPath(alt.c_str(), altWinPath, sizeof(altWinPath))) {
            h = CreateFileA(altWinPath, dwDesiredAccess, dwShareMode ? dwShareMode : (FILE_SHARE_READ | FILE_SHARE_WRITE),
                            lpSecurityAttributes, dwCreationDisposition, cleanFlags, hTemplateFile);
        }
    }
    if (h == INVALID_HANDLE_VALUE) {
        LogPrintf("[VFS-MISS] CreateFileA('%s' -> '%s') failed (err=%lu)\n",
                  lpFileName ? lpFileName : "(null)", winPath, GetLastError());
    }
    return h;
}

static BOOL __stdcall Detour_ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead, LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped) {
    return ReadFile(hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead, lpOverlapped);
}

static BOOL __stdcall Detour_WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite, LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped) {
    return WriteFile(hFile, lpBuffer, nNumberOfBytesToWrite, lpNumberOfBytesWritten, lpOverlapped);
}

static BOOL __stdcall Detour_CloseHandle(HANDLE hObject) {
    if (!hObject || hObject == INVALID_HANDLE_VALUE) return TRUE;
    auto it = g_findHandles.find(hObject);
    if (it != g_findHandles.end()) {
        g_findHandles.erase(it);
        return FindClose(hObject);
    }
    return CloseHandle(hObject);
}

static DWORD __stdcall Detour_SetFilePointer(HANDLE hFile, LONG lDistanceToMove, PLONG lpDistanceToMoveHigh, DWORD dwMoveMethod) {
    return SetFilePointer(hFile, lDistanceToMove, lpDistanceToMoveHigh, dwMoveMethod);
}

static BOOL __stdcall Detour_SetEndOfFile(HANDLE hFile) { return SetEndOfFile(hFile); }
static BOOL __stdcall Detour_FlushFileBuffers(HANDLE hFile) { return FlushFileBuffers(hFile); }
static BOOL __stdcall Detour_GetFileSizeEx(HANDLE hFile, PLARGE_INTEGER lpFileSize) { return GetFileSizeEx(hFile, lpFileSize); }
static DWORD __stdcall Detour_GetFileSize(HANDLE hFile, LPDWORD lpFileSizeHigh) { return GetFileSize(hFile, lpFileSizeHigh); }

static DWORD __stdcall Detour_GetFileAttributesA(const char* lpFileName) {
    char winPath[512]{};
    xboxkrnl::TranslateXboxPath(lpFileName, winPath, sizeof(winPath));
    return GetFileAttributesA(winPath);
}

static BOOL __stdcall Detour_SetFileAttributesA(const char* lpFileName, DWORD dwFileAttributes) {
    char winPath[512]{};
    xboxkrnl::TranslateXboxPath(lpFileName, winPath, sizeof(winPath));
    return SetFileAttributesA(winPath, dwFileAttributes);
}

static BOOL __stdcall Detour_DeleteFileA(const char* lpFileName) {
    char winPath[512]{};
    xboxkrnl::TranslateXboxPath(lpFileName, winPath, sizeof(winPath));
    SetFileAttributesA(winPath, FILE_ATTRIBUTE_NORMAL);
    return DeleteFileA(winPath);
}

static BOOL __stdcall Detour_CopyFileA(const char* lpExistingFileName, const char* lpNewFileName, BOOL bFailIfExists) {
    char srcWin[512]{};
    char dstWin[512]{};
    xboxkrnl::TranslateXboxPath(lpExistingFileName, srcWin, sizeof(srcWin));
    xboxkrnl::TranslateXboxPath(lpNewFileName, dstWin, sizeof(dstWin));
    // Ensure destination parent directories exist
    char dirBuf[512]{};
    std::strncpy(dirBuf, dstWin, sizeof(dirBuf) - 1);
    for (char* p = dirBuf; *p; ++p) {
        if (*p == '\\' || *p == '/') {
            char saved = *p;
            *p = '\0';
            if (p != dirBuf && *(p - 1) != ':') CreateDirectoryA(dirBuf, nullptr);
            *p = saved;
        }
    }
    SetFileAttributesA(dstWin, FILE_ATTRIBUTE_NORMAL);
    BOOL ok = CopyFileA(srcWin, dstWin, bFailIfExists);
    LogPrintf("[VFS] CopyFileA('%s' -> '%s', failIfExists=%d) -> %s (err=%lu)\n",
              lpExistingFileName ? lpExistingFileName : "",
              lpNewFileName ? lpNewFileName : "",
              bFailIfExists ? 1 : 0,
              ok ? "OK" : "FAIL",
              ok ? 0UL : GetLastError());
    return ok;
}

static BOOL __stdcall Detour_CreateDirectoryA(const char* lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes) {
    char winPath[512]{};
    xboxkrnl::TranslateXboxPath(lpPathName, winPath, sizeof(winPath));
    return CreateDirectoryA(winPath, lpSecurityAttributes);
}

static BOOL __stdcall Detour_RemoveDirectoryA(const char* lpPathName) {
    char winPath[512]{};
    xboxkrnl::TranslateXboxPath(lpPathName, winPath, sizeof(winPath));
    return RemoveDirectoryA(winPath);
}

static HANDLE __stdcall Detour_FindFirstFileA(const char* lpFileName, LPWIN32_FIND_DATAA lpFindFileData) {
    char winPath[512]{};
    xboxkrnl::TranslateXboxPath(lpFileName, winPath, sizeof(winPath));
    HANDLE h = FindFirstFileA(winPath, lpFindFileData);
    if (h != INVALID_HANDLE_VALUE) {
        g_findHandles.insert(h);
    }
    return h;
}

static BOOL __stdcall Detour_FindNextFileA(HANDLE hFindFile, LPWIN32_FIND_DATAA lpFindFileData) {
    return FindNextFileA(hFindFile, lpFindFileData);
}

static BOOL __stdcall Detour_GetDiskFreeSpaceExA(
    const char* /*lpDirectoryName*/,
    PULARGE_INTEGER lpFreeBytesAvailableToCaller,
    PULARGE_INTEGER lpTotalNumberOfBytes,
    PULARGE_INTEGER lpTotalNumberOfFreeBytes
) {
    if (lpFreeBytesAvailableToCaller) lpFreeBytesAvailableToCaller->QuadPart = 512ULL * 1024 * 1024;
    if (lpTotalNumberOfBytes)         lpTotalNumberOfBytes->QuadPart         = 1024ULL * 1024 * 1024;
    if (lpTotalNumberOfFreeBytes)     lpTotalNumberOfFreeBytes->QuadPart     = 512ULL * 1024 * 1024;
    return TRUE;
}

static HANDLE __stdcall Detour_CreateThread(
    LPSECURITY_ATTRIBUTES lpThreadAttributes,
    SIZE_T dwStackSize,
    LPTHREAD_START_ROUTINE lpStartAddress,
    LPVOID lpParameter,
    DWORD dwCreationFlags,
    LPDWORD lpThreadId
) {
    return CreateThread(lpThreadAttributes, dwStackSize, lpStartAddress, lpParameter, dwCreationFlags, lpThreadId);
}

static void  __stdcall Detour_Sleep(DWORD dwMilliseconds) { Sleep(dwMilliseconds); }
static DWORD __stdcall Detour_SleepEx(DWORD dwMilliseconds, BOOL bAlertable) { return SleepEx(dwMilliseconds, bAlertable); }
static BOOL  __stdcall Detour_SwitchToThread() { return SwitchToThread(); }
static DWORD __stdcall Detour_GetTickCount() { return GetTickCount(); }

static void __stdcall Detour_OutputDebugStringA(const char* lpOutputString) {
    if (lpOutputString) LogPrintf("[XBOX-DBG] %s", lpOutputString);
}

// ============================================================================
// 4. XAPI Launch, Sections, Video, Input & SaveGames
// ============================================================================
static uint8_t  g_lastLaunchData[3072]{};
static bool     g_hasLastLaunchData  = false;
static bool     g_autoResumeQuick    = false;
static volatile LONG g_quickSaveReq     = 0;
static volatile LONG g_quickLoadReq     = 0;
static volatile LONG g_costumeReloadReq = 0;
static volatile LONG g_jumpLevelReq     = 0;
static int           g_activeSaveSlot   = 1;
static int           g_reqSaveSlot      = 1;
static int           g_reqLoadSlot      = 1;
static char          g_jumpMapName[128] = {};

static bool          g_mouseLookEnabled = false;
static bool          g_subtitlesEnabled = true;
static bool          g_autoPauseUnfocused = false;
static bool          g_skipIntroMovies    = false;
static bool          g_cheatInfHealth     = false;
static bool          g_cheatInfPower      = false;
static bool          g_cheatInfAmmo       = false;
static bool          g_cheatOneHitDust    = false;

static void SyncSubtitlesFlagToEngine() {
    uint32_t gamePtr = *reinterpret_cast<const uint32_t*>(0x007D0090);
    if (gamePtr < 0x00010000u || IsBadReadPtr(reinterpret_cast<const void*>(gamePtr), 0x24)) return;
    uint32_t optsPtr = *reinterpret_cast<const uint32_t*>(gamePtr + 0x20);
    if (optsPtr < 0x00010000u || IsBadWritePtr(reinterpret_cast<void*>(optsPtr + 0x34), 0xB0)) return;
    *reinterpret_cast<uint8_t*>(optsPtr + 0xE2) = g_subtitlesEnabled ? 1u : 0u;
    // Built-in engine Infinite Slayer Power cheat bit (TGameOptions + 0x34 bit 0x02, read by ISlayer::Power_Change)
    uint8_t* pCheatFlags = reinterpret_cast<uint8_t*>(optsPtr + 0x34);
    if (g_cheatInfPower) *pCheatFlags |= 0x02u;
    else                 *pCheatFlags &= ~0x02u;
}

static bool IsGameWindowFocused();
static void* GetLivePlayerObject();

static float __cdecl Detour_QueryTimeScale() {
    if (d3d9_bridge::IsPhotoMode()) {
        return 0.0f;
    }
    if (g_autoPauseUnfocused && !IsGameWindowFocused()) {
        return 0.0f;
    }
    uint32_t gamePtr = *reinterpret_cast<const uint32_t*>(0x007D0090);
    if (gamePtr < 0x00010000u || IsBadReadPtr(reinterpret_cast<const void*>(gamePtr), 0x118)) return 1.0f;
    if (*reinterpret_cast<const uint32_t*>(gamePtr + 0x114) == 1u) {
        return 0.0f;
    }
    uint32_t optsPtr = *reinterpret_cast<const uint32_t*>(gamePtr + 0x20);
    if (optsPtr < 0x00010000u || IsBadReadPtr(reinterpret_cast<const void*>(optsPtr + 0xAC), 4)) return 1.0f;
    return *reinterpret_cast<const float*>(optsPtr + 0xAC);
}

static void __fastcall Detour_ICharacterBtvS_Health_Change(void* thisPtr, void* /*edx*/, float delta, uint32_t flag) {
    if (thisPtr && !IsBadWritePtr(thisPtr, 0x98)) {
        void* player = GetLivePlayerObject();
        if (player && thisPtr == player) {
            if (g_cheatInfHealth && delta < 0.0f) {
                delta = 0.0f;
                float maxHp = *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(thisPtr) + 0x94);
                if (maxHp > 0.0f) {
                    *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(thisPtr) + 0x90) = maxHp;
                }
            }
        } else if (player && thisPtr != player) {
            if (g_cheatOneHitDust && delta < 0.0f) {
                delta = -9999.0f;
            }
        }
    }
    typedef void (__thiscall* FnBaseHealthChange)(void*, float, uint32_t);
    reinterpret_cast<FnBaseHealthChange>(0x0012D450)(thisPtr, delta, flag);
    if (thisPtr && !IsBadWritePtr(thisPtr, 0x94)) {
        float* hp = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(thisPtr) + 0x90);
        if (*hp < -100.0f) *hp = -100.0f;
    }
}

static uint8_t __fastcall Detour_IAmmo_UseAmmo(void* thisPtr, void* /*edx*/, uint32_t amount) {
    if (!thisPtr || IsBadWritePtr(reinterpret_cast<uint8_t*>(thisPtr) + 0xD88, 4)) return 0;
    uint32_t* pAmmo = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(thisPtr) + 0xD88);
    if (g_cheatInfAmmo) {
        if (*pAmmo < amount) *pAmmo = amount;
        return 1;
    }
    if (*pAmmo < amount) return 0;
    *pAmmo -= amount;
    return 1;
}

typedef uint32_t (__thiscall* FnPickupTakeDamage)(void*, void*);
static FnPickupTakeDamage g_origPickupTakeDamage = nullptr;

static uint32_t __fastcall Detour_IPickup_TakeDamage(void* thisPtr, void* /*edx*/, void* dmgInfo) {
    if (g_cheatInfAmmo) {
        return 0;
    }
    if (g_origPickupTakeDamage) {
        return g_origPickupTakeDamage(thisPtr, dmgInfo);
    }
    return 0;
}
static int           g_mouseSensitivity = 100;
static bool          g_invertCameraX    = false;
static bool          g_invertCameraY    = false;
static volatile LONG g_mouseCaptured    = 0;
static volatile LONG g_mouseDeltaX      = 0;
static volatile LONG g_mouseDeltaY      = 0;
static int           g_keybinds[10]     = { 'W', 'S', 'A', 'D', VK_SPACE, VK_SHIFT, VK_CONTROL, 'E', 'Q', 'R' };

struct QuickSaveState {
    uint32_t magic;            // 'BFQS' = 0x53514642
    uint32_t version;          // 1
    uint32_t hasLaunchData;
    uint32_t hasPlayerMtx;
    float    playerMtx[16];    // T_Matrix4x4<float> at IObject + 0x21C (pos at +0x240 = floats 9..11)
    uint8_t  launchData[3072];
};

static bool           g_pendingQuickPosRestore = false;
static uint32_t       g_playerAliveFrames      = 0;
static QuickSaveState g_pendingQuickState{};

static void* GetLivePlayerObject() {
    uint32_t* pGamePtr = reinterpret_cast<uint32_t*>(0x007D0090);
    if (IsBadReadPtr(pGamePtr, 4) || *pGamePtr == 0) return nullptr;
    uint8_t* pGame = reinterpret_cast<uint8_t*>(*pGamePtr);
    if (IsBadReadPtr(pGame + 0xF0, 4)) return nullptr;
    void* player = *reinterpret_cast<void**>(pGame + 0xF0);
    if (!player || IsBadReadPtr(player, 0x260)) return nullptr;
    return player;
}

static void GetQuickSaveSlotPath(int slot, char* outBuf, size_t maxLen) {
    int s = (slot >= 1 && slot <= 5) ? slot : 1;
    char xboxPath[64]{};
    std::snprintf(xboxPath, sizeof(xboxPath), "T:\\quicksave_slot%d.bin", s);
    xboxkrnl::TranslateXboxPath(xboxPath, outBuf, maxLen);
}

static bool ReadQuickSaveState(QuickSaveState& outState, int slot = 1) {
    char qsPath[512]{};
    GetQuickSaveSlotPath(slot, qsPath, sizeof(qsPath));
    FILE* f = std::fopen(qsPath, "rb");
    if (!f && slot == 1) {
        // Backward compatibility with legacy single-slot T:\quicksave.bin
        xboxkrnl::TranslateXboxPath("T:\\quicksave.bin", qsPath, sizeof(qsPath));
        f = std::fopen(qsPath, "rb");
    }
    if (!f) return false;
    bool ok = (std::fread(&outState, sizeof(QuickSaveState), 1, f) == 1) &&
              (outState.magic == 0x53514642u) &&
              (outState.version == 1u);
    std::fclose(f);
    return ok;
}

static bool WriteQuickSaveState(const QuickSaveState& st, int slot = 1) {
    char qsPath[512]{};
    GetQuickSaveSlotPath(slot, qsPath, sizeof(qsPath));
    FILE* f = std::fopen(qsPath, "wb");
    if (!f) return false;
    bool ok = (std::fwrite(&st, sizeof(QuickSaveState), 1, f) == 1);
    std::fclose(f);
    return ok;
}

static void SaveQuickLaunchBlob(const void* buf) {
    char qlPath[512]{};
    xboxkrnl::TranslateXboxPath("T:\\quickload_launch.bin", qlPath, sizeof(qlPath));
    FILE* f = std::fopen(qlPath, "wb");
    if (f) {
        std::fwrite(buf, 1, 3072, f);
        std::fclose(f);
    }
}

static void ApplyPlayerMatrix(void* player, const float mtx[16]) {
    if (!player) return;
    // IObject::Mtx_SetWorld(const T_Matrix4x4<float>&) @ 0x000BC0D0 (__thiscall, ret 4)
    using PFN_MtxSetWorld = void(__fastcall*)(void* thisPtr, void* edx_unused, const float* pMtx);
    reinterpret_cast<PFN_MtxSetWorld>(0x000BC0D0)(player, nullptr, mtx);
}

static DWORD __stdcall Detour_XGetLaunchInfo(DWORD* pdwLaunchDataType, void* pLaunchData) {
    char relaunchPath[512]{};
    xboxkrnl::TranslateXboxPath("T:\\relaunch_data.bin", relaunchPath, sizeof(relaunchPath));
    FILE* f = std::fopen(relaunchPath, "rb");
    if (f) {
        uint8_t buf[3072]{};
        std::fread(buf, 1, sizeof(buf), f);
        std::fclose(f);
        DeleteFileA(relaunchPath);
        std::memcpy(g_lastLaunchData, buf, sizeof(buf));
        g_hasLastLaunchData = true;
        if (pdwLaunchDataType) *pdwLaunchDataType = 0; // LDT_TITLE (launched by Default.xbe!)
        if (pLaunchData) std::memcpy(pLaunchData, buf, sizeof(buf));
        LogPrintf("[XAPI] XGetLaunchInfo -> Loaded relaunch LAUNCH_DATA ('%.128s')\n",
                  reinterpret_cast<const char*>(buf));
        // If this relaunch came from a Quick Load (F6) or in-place costume switch, arm exact player position restore
        char qlFlagPath[512]{};
        xboxkrnl::TranslateXboxPath("T:\\quickload_restore.flag", qlFlagPath, sizeof(qlFlagPath));
        FILE* ff = std::fopen(qlFlagPath, "rb");
        if (ff) {
            int ch = std::fgetc(ff);
            std::fclose(ff);
            DeleteFileA(qlFlagPath);
            int slot = (ch >= '1' && ch <= '5') ? (ch - '0') : 1;
            if (ReadQuickSaveState(g_pendingQuickState, slot) && g_pendingQuickState.hasPlayerMtx) {
                g_pendingQuickPosRestore = true;
                g_playerAliveFrames = 0;
            }
        }
        return ERROR_SUCCESS;
    }
    if (g_autoResumeQuick) {
        QuickSaveState qs{};
        if (ReadQuickSaveState(qs, g_activeSaveSlot) && qs.hasLaunchData) {
            std::memcpy(g_lastLaunchData, qs.launchData, sizeof(qs.launchData));
            g_hasLastLaunchData = true;
            if (qs.hasPlayerMtx) {
                g_pendingQuickState = qs;
                g_pendingQuickPosRestore = true;
                g_playerAliveFrames = 0;
            }
            if (pdwLaunchDataType) *pdwLaunchDataType = 0; // LDT_TITLE
            if (pLaunchData) std::memcpy(pLaunchData, qs.launchData, sizeof(qs.launchData));
            LogPrintf("[QuickLoad] Auto-resuming last Quick Save ('%.128s')!\n",
                      reinterpret_cast<const char*>(qs.launchData));
            return ERROR_SUCCESS;
        }
        char qlPath[512]{};
        xboxkrnl::TranslateXboxPath("T:\\quickload_launch.bin", qlPath, sizeof(qlPath));
        FILE* qf = std::fopen(qlPath, "rb");
        if (qf) {
            uint8_t buf[3072]{};
            std::fread(buf, 1, sizeof(buf), qf);
            std::fclose(qf);
            std::memcpy(g_lastLaunchData, buf, sizeof(buf));
            g_hasLastLaunchData = true;
            if (pdwLaunchDataType) *pdwLaunchDataType = 0; // LDT_TITLE
            if (pLaunchData) std::memcpy(pLaunchData, buf, sizeof(buf));
            LogPrintf("[QuickLoad] Auto-resuming last Quick Save ('%.128s')!\n",
                      reinterpret_cast<const char*>(buf));
            return ERROR_SUCCESS;
        }
    }
    if (pdwLaunchDataType) *pdwLaunchDataType = 3; // LDT_NONE / from Dashboard
    if (pLaunchData) std::memset(pLaunchData, 0, 3072);
    return ERROR_NOT_FOUND; // Normal boot -> _main pushes default args ("root:Z:\", "rpecopy:d:\", etc.)
}

static DWORD __stdcall Detour_XLaunchNewImageA(const char* lpTitlePath, const void* pLaunchData) {
    LogPrintf("[XAPI] XLaunchNewImageA('%s', pLaunchData=0x%08X)\n",
              lpTitlePath ? lpTitlePath : "(null)",
              static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pLaunchData)));

    // Persist 3072-byte Xbox LAUNCH_DATA for the relaunched process (how Buffy transitions between Menu & Levels!)
    if (pLaunchData) {
        LogPrintf("[XAPI] LAUNCH_DATA payload: '%.128s'\n", static_cast<const char*>(pLaunchData));
        char relaunchPath[512]{};
        xboxkrnl::TranslateXboxPath("T:\\relaunch_data.bin", relaunchPath, sizeof(relaunchPath));
        FILE* f = std::fopen(relaunchPath, "wb");
        if (f) {
            std::fwrite(pLaunchData, 1, 3072, f);
            std::fclose(f);
        }
    }

    // Spawn a fresh BuffyWin32Recomp.exe with clean memory & exit this instance
    char selfExe[MAX_PATH]{};
    GetModuleFileNameA(nullptr, selfExe, MAX_PATH);
    dsound_bridge::Shutdown();
    d3d9_bridge::Shutdown();
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessA(selfExe, nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        LogPrintf("[XAPI] Relaunched '%s' (PID=%lu) for level transition. Exiting current instance.\n",
                  selfExe, pi.dwProcessId);
        ExitProcess(0);
    }
    return ERROR_SUCCESS;
}

static BOOL __stdcall Detour_XMountUtilityDrive(BOOL /*fFormatClean*/) { return TRUE; }
static BOOL __stdcall Detour_XFormatUtilityDrive() { return TRUE; }
static DWORD __stdcall Detour_XGetVideoStandard() { return 1; /* XC_VIDEO_STANDARD_NTSC_M */ }
static DWORD __stdcall Detour_XGetVideoFlags() { return 0; }
static DWORD __stdcall Detour_XGetAudioFlags() { return 0; }

// Keep a copy of the 4KB XBE header page in BuffyRecompRuntime.dll so we never overwrite
// the host executable's Win32 PE header at 0x00010000..0x00010FFF!
static uint8_t g_xbeHeaderCopy[0x1000]{};

static HANDLE __stdcall Detour_XGetSectionHandleA(const char* pSectionName) {
    LogPrintf("[XAPI] XGetSectionHandleA('%s')\n", pSectionName ? pSectionName : "");
    return reinterpret_cast<HANDLE>(g_xbeHeaderCopy + 0x0360);
}
static void* __stdcall Detour_XLoadSectionByHandle(HANDLE hSection) { return hSection; }
static BOOL  __stdcall Detour_XFreeSectionByHandle(HANDLE /*hSection*/) { return TRUE; }
static DWORD __stdcall Detour_XGetSectionSize(HANDLE /*hSection*/) { return 4096; }

// Input (Windows XInput Xbox Controller + Keyboard -> Xbox Controller Port 0 Bridge)
struct XINPUT_GAMEPAD_XBOX {
    WORD  wButtons;
    BYTE  bAnalogButtons[8]; // 0=A, 1=B, 2=X, 3=Y, 4=Black, 5=White, 6=LTrigger, 7=RTrigger
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
};

struct XINPUT_STATE_XBOX {
    DWORD               dwPacketNumber;
    XINPUT_GAMEPAD_XBOX Gamepad;
};

#pragma pack(push, 2)
struct XINPUT_FEEDBACK_XBOX {
    DWORD  dwStatus;
    HANDLE hEvent;
    BYTE   Reserved[58];
    WORD   wLeftMotorSpeed;
    WORD   wRightMotorSpeed;
};
#pragma pack(pop)

// Windows XInput ABI (loaded dynamically from xinput1_4.dll / xinput1_3.dll / xinput9_1_0.dll)
struct WIN_XINPUT_GAMEPAD {
    WORD  wButtons;
    BYTE  bLeftTrigger;
    BYTE  bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
};

struct WIN_XINPUT_STATE {
    DWORD              dwPacketNumber;
    WIN_XINPUT_GAMEPAD Gamepad;
};

struct WIN_XINPUT_VIBRATION {
    WORD wLeftMotorSpeed;
    WORD wRightMotorSpeed;
};

using PFN_WinXInputGetState = DWORD(WINAPI*)(DWORD, WIN_XINPUT_STATE*);
using PFN_WinXInputSetState = DWORD(WINAPI*)(DWORD, WIN_XINPUT_VIBRATION*);

static PFN_WinXInputGetState g_winXInputGetState = nullptr;
static PFN_WinXInputSetState g_winXInputSetState = nullptr;
static bool                  g_winXInputInited   = false;
static bool                  g_winPadLoggedConn  = false;
static bool                  g_winPadConnected   = false;
static DWORD                 g_activeWinPadIndex = 0;
static bool                  g_rumbleEnabled     = true;
static int                   g_deadzonePercent   = 15;
static char                  g_quickStatusBuf[64]{};
static DWORD                 g_quickStatusExpireTick = 0;

static void SetQuickStatusNotice(const char* text) {
    if (!text) return;
    std::snprintf(g_quickStatusBuf, sizeof(g_quickStatusBuf), "%s", text);
    g_quickStatusExpireTick = GetTickCount() + 3000;
}

static SHORT ApplyStickDeadzone(SHORT v, int dzPct) {
    if (dzPct <= 0) return v;
    int32_t thresh = (32767 * dzPct) / 100;
    int32_t av = (v < 0) ? -static_cast<int32_t>(v) : static_cast<int32_t>(v);
    if (av <= thresh) return 0;
    int32_t scaled = ((av - thresh) * 32767) / std::max(1, 32767 - thresh);
    if (scaled > 32767) scaled = 32767;
    return static_cast<SHORT>((v < 0) ? -scaled : scaled);
}

static void InitWindowsXInput() {
    if (g_winXInputInited) return;
    g_winXInputInited = true;
    const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    for (const char* name : dlls) {
        HMODULE mod = LoadLibraryA(name);
        if (mod) {
            g_winXInputGetState = reinterpret_cast<PFN_WinXInputGetState>(GetProcAddress(mod, "XInputGetState"));
            g_winXInputSetState = reinterpret_cast<PFN_WinXInputSetState>(GetProcAddress(mod, "XInputSetState"));
            if (g_winXInputGetState) {
                LogPrintf("[XInput] Loaded Windows gamepad backend: %s\n", name);
                break;
            }
        }
    }
}

static void __stdcall Detour_XInitDevices(DWORD /*dwPreallocTypeCount*/, void* /*PreallocTypes*/) {
    InitWindowsXInput();
    LogPrintf("[XInput] XInitDevices initialized (Windows Xbox Controller + Keyboard -> Xbox Port 0).\n");
}

static DWORD __stdcall Detour_XGetDevices(void* /*DeviceType*/) {
    return 1; // Port 0 connected (bit 0 = 1)
}

static BOOL __stdcall Detour_XGetDeviceChanges(void* /*DeviceType*/, DWORD* pdwInsertions, DWORD* pdwRemovals) {
    static bool s_firstCall = true;
    if (pdwInsertions) *pdwInsertions = s_firstCall ? 1 : 0;
    if (pdwRemovals)   *pdwRemovals   = 0;
    bool changed = s_firstCall;
    s_firstCall = false;
    return changed ? TRUE : FALSE;
}

static HANDLE __stdcall Detour_XInputOpen(void* /*DeviceType*/, DWORD dwPort, DWORD /*dwSlot*/, void* /*pPollingParameters*/) {
    InitWindowsXInput();
    return (dwPort == 0) ? reinterpret_cast<HANDLE>(0x1001) : nullptr;
}

static void __stdcall Detour_XInputClose(HANDLE /*hDevice*/) {
    if (g_winXInputSetState) {
        WIN_XINPUT_VIBRATION zero{0, 0};
        g_winXInputSetState(g_activeWinPadIndex, &zero);
    }
}

static bool IsGameWindowFocused() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD fgPid = 0;
    GetWindowThreadProcessId(fg, &fgPid);
    return (fgPid == GetCurrentProcessId());
}

static void FlushPendingFovRefresh();

static DWORD __stdcall Detour_XInputGetState(HANDLE /*hDevice*/, XINPUT_STATE_XBOX* pState) {
    FlushPendingFovRefresh();
    SyncSubtitlesFlagToEngine();
    if (!pState) return ERROR_DEVICE_NOT_CONNECTED;
    static DWORD s_pkt = 1;
    std::memset(pState, 0, sizeof(*pState));
    pState->dwPacketNumber = ++s_pkt;

    // Ignore keyboard & gamepad input whenever another application has focus or Photo Mode is active
    if (!IsGameWindowFocused() || d3d9_bridge::IsPhotoMode()) {
        return ERROR_SUCCESS;
    }

    // 1. Poll real Windows Xbox Controller (supports hot-plugging on slots 0..3)
    if (g_winXInputGetState) {
        WIN_XINPUT_STATE ws{};
        bool found = (g_winXInputGetState(g_activeWinPadIndex, &ws) == ERROR_SUCCESS);
        if (!found) {
            for (DWORD i = 0; i < 4; ++i) {
                if (g_winXInputGetState(i, &ws) == ERROR_SUCCESS) {
                    g_activeWinPadIndex = i;
                    found = true;
                    break;
                }
            }
        }
        g_winPadConnected = found;
        if (found) {
            if (!g_winPadLoggedConn) {
                g_winPadLoggedConn = true;
                LogPrintf("[XInput] Xbox Controller connected on Windows slot %lu!\n", g_activeWinPadIndex);
            }
            const WIN_XINPUT_GAMEPAD& g = ws.Gamepad;
            // Low 8 bits (DPAD U/D/L/R, START, BACK, L3, R3) match Original Xbox XDK bit-for-bit!
            pState->Gamepad.wButtons |= (g.wButtons & 0x00FFu);

            // Face buttons + Xbox BC standard LB=White, RB=Black + Analog Triggers
            if (g.wButtons & 0x1000u) pState->Gamepad.bAnalogButtons[0] = 255; // A
            if (g.wButtons & 0x2000u) pState->Gamepad.bAnalogButtons[1] = 255; // B
            if (g.wButtons & 0x4000u) pState->Gamepad.bAnalogButtons[2] = 255; // X
            if (g.wButtons & 0x8000u) pState->Gamepad.bAnalogButtons[3] = 255; // Y
            if (g.wButtons & 0x0200u) pState->Gamepad.bAnalogButtons[4] = 255; // RB -> Black
            if (g.wButtons & 0x0100u) pState->Gamepad.bAnalogButtons[5] = 255; // LB -> White
            pState->Gamepad.bAnalogButtons[6] = g.bLeftTrigger;                // LT
            pState->Gamepad.bAnalogButtons[7] = g.bRightTrigger;               // RT

            pState->Gamepad.sThumbLX = ApplyStickDeadzone(g.sThumbLX, g_deadzonePercent);
            pState->Gamepad.sThumbLY = ApplyStickDeadzone(g.sThumbLY, g_deadzonePercent);
            pState->Gamepad.sThumbRX = ApplyStickDeadzone(g.sThumbRX, g_deadzonePercent);
            pState->Gamepad.sThumbRY = ApplyStickDeadzone(g.sThumbRY, g_deadzonePercent);
        }
    }

    // 2. Merge Keyboard & Mouse-Look controls seamlessly so either input works at any time
    auto keyDown = [](int vk) -> bool { return (vk > 0) && ((GetAsyncKeyState(vk) & 0x8000) != 0); };

    if (keyDown(VK_UP))     pState->Gamepad.wButtons |= 0x0001;
    if (keyDown(VK_DOWN))   pState->Gamepad.wButtons |= 0x0002;
    if (keyDown(VK_LEFT))   pState->Gamepad.wButtons |= 0x0004;
    if (keyDown(VK_RIGHT))  pState->Gamepad.wButtons |= 0x0008;
    if (keyDown(VK_RETURN)) pState->Gamepad.wButtons |= 0x0010; // START
    if (keyDown(VK_BACK) || keyDown(VK_TAB)) pState->Gamepad.wButtons |= 0x0020; // BACK

    if (keyDown(g_keybinds[4]) || keyDown('J') || keyDown(VK_RETURN)) pState->Gamepad.bAnalogButtons[0] = 255; // A
    if (keyDown(g_keybinds[5]) || keyDown('K'))                       pState->Gamepad.bAnalogButtons[1] = 255; // B
    if (keyDown(g_keybinds[6]) || keyDown('L'))                       pState->Gamepad.bAnalogButtons[2] = 255; // X
    if (keyDown(g_keybinds[7]) || keyDown('I'))                       pState->Gamepad.bAnalogButtons[3] = 255; // Y
    if (keyDown(g_keybinds[8]))                                       pState->Gamepad.bAnalogButtons[6] = 255; // LTrigger
    if (keyDown(g_keybinds[9]) || keyDown('O'))                       pState->Gamepad.bAnalogButtons[7] = 255; // RTrigger

    if (keyDown(g_keybinds[0])) pState->Gamepad.sThumbLY =  32767;
    if (keyDown(g_keybinds[1])) pState->Gamepad.sThumbLY = -32767;
    if (keyDown(g_keybinds[2])) pState->Gamepad.sThumbLX = -32767;
    if (keyDown(g_keybinds[3])) pState->Gamepad.sThumbLX =  32767;

    // 3. Mouse-Look Camera (maps mouse movement to right thumbstick + LMB=A / RMB=LT while captured)
    LONG mdx = InterlockedExchange(&g_mouseDeltaX, 0);
    LONG mdy = InterlockedExchange(&g_mouseDeltaY, 0);
    if (g_mouseLookEnabled && g_mouseCaptured) {
        if (keyDown(VK_LBUTTON)) pState->Gamepad.bAnalogButtons[0] = 255; // Left Click -> A (Attack/Use)
        if (keyDown(VK_RBUTTON)) pState->Gamepad.bAnalogButtons[6] = 255; // Right Click -> LT (Aim/Lock-on)
        if (mdx != 0 || mdy != 0) {
            float scale = 520.0f * (static_cast<float>(g_mouseSensitivity) / 100.0f);
            int rx = static_cast<int>(pState->Gamepad.sThumbRX) + static_cast<int>(static_cast<float>(mdx) * scale);
            int ry = static_cast<int>(pState->Gamepad.sThumbRY) - static_cast<int>(static_cast<float>(mdy) * scale);
            pState->Gamepad.sThumbRX = static_cast<int16_t>(std::max(-32767, std::min(32767, rx)));
            pState->Gamepad.sThumbRY = static_cast<int16_t>(std::max(-32767, std::min(32767, ry)));
        }
    }

    // 4. Camera Invert X / Y (applies cleanly to both Gamepad Right Stick & Mouse-Look)
    if (g_invertCameraX) {
        int rx = -static_cast<int>(pState->Gamepad.sThumbRX);
        pState->Gamepad.sThumbRX = static_cast<int16_t>(std::max(-32767, std::min(32767, rx)));
    }
    if (g_invertCameraY) {
        int ry = -static_cast<int>(pState->Gamepad.sThumbRY);
        pState->Gamepad.sThumbRY = static_cast<int16_t>(std::max(-32767, std::min(32767, ry)));
    }

    return ERROR_SUCCESS;
}

static DWORD __stdcall Detour_XInputSetState(HANDLE /*hDevice*/, void* pFeedback) {
    if (pFeedback) {
        auto* fb = reinterpret_cast<XINPUT_FEEDBACK_XBOX*>(pFeedback);
        if (g_winXInputSetState) {
            WIN_XINPUT_VIBRATION vib{
                g_rumbleEnabled ? fb->wLeftMotorSpeed : static_cast<WORD>(0),
                g_rumbleEnabled ? fb->wRightMotorSpeed : static_cast<WORD>(0)
            };
            g_winXInputSetState(g_activeWinPadIndex, &vib);
        }
        fb->dwStatus = ERROR_SUCCESS;
        if (fb->hEvent) SetEvent(fb->hEvent);
    }
    return ERROR_SUCCESS;
}

static DWORD __stdcall Detour_XMountMUA(DWORD /*dwPort*/, DWORD /*dwSlot*/, char* pchDrive) {
    if (pchDrive) *pchDrive = 'U';
    return ERROR_SUCCESS;
}
static DWORD __stdcall Detour_XUnmountMU(DWORD /*dwPort*/, DWORD /*dwSlot*/) { return ERROR_SUCCESS; }
static HANDLE __stdcall Detour_XFindFirstSaveGame(const char* /*lpRootPathName*/, void* /*lpFindGameData*/) {
    return INVALID_HANDLE_VALUE;
}
static BOOL __stdcall Detour_XFindNextSaveGame(HANDLE /*hFindGame*/, void* /*lpFindGameData*/) { return FALSE; }
static BOOL __stdcall Detour_XFindClose(HANDLE hFindGame) {
    return Detour_CloseHandle(hFindGame);
}
static DWORD __stdcall Detour_XDeleteSaveGame(const char* /*lpRootPathName*/, const wchar_t* /*lpSaveGameName*/) {
    return ERROR_SUCCESS;
}
static DWORD __stdcall Detour_XCreateSaveGame(
    const char* /*lpRootPathName*/,
    const wchar_t* /*lpSaveGameName*/,
    DWORD /*dwCreationDisposition*/,
    DWORD /*dwCreateFlags*/,
    char* lpPathBuffer,
    UINT uSize
) {
    if (lpPathBuffer && uSize > 4) {
        std::snprintf(lpPathBuffer, uSize, "U:\\");
    }
    return ERROR_SUCCESS;
}

static HANDLE __stdcall Detour_XCalculateSignatureBegin(DWORD /*dwFlags*/) {
    uint8_t* ctx = static_cast<uint8_t*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 128));
    if (ctx) xboxkrnl::Shim_XcSHAInit(ctx);
    return ctx ? reinterpret_cast<HANDLE>(ctx) : INVALID_HANDLE_VALUE;
}

static HANDLE __stdcall Detour_XCalculateSignatureBeginEx(DWORD dwFlags, DWORD /*dwAltTitleId*/) {
    return Detour_XCalculateSignatureBegin(dwFlags);
}

static DWORD __stdcall Detour_XCalculateSignatureUpdate(HANDLE hCalcSig, const uint8_t* pbData, ULONG ulDataSize) {
    if (hCalcSig && hCalcSig != INVALID_HANDLE_VALUE && pbData && ulDataSize > 0) {
        xboxkrnl::Shim_XcSHAUpdate(reinterpret_cast<uint8_t*>(hCalcSig), pbData, ulDataSize);
    }
    return ERROR_SUCCESS;
}

static DWORD __stdcall Detour_XCalculateSignatureEnd(HANDLE hCalcSig, void* pSignature) {
    if (hCalcSig && hCalcSig != INVALID_HANDLE_VALUE) {
        if (pSignature) {
            xboxkrnl::Shim_XcSHAFinal(reinterpret_cast<uint8_t*>(hCalcSig), static_cast<uint8_t*>(pSignature));
        }
        HeapFree(GetProcessHeap(), 0, hCalcSig);
    }
    return ERROR_SUCCESS;
}

// ============================================================================
// 5. Slate Engine Logging, Installer & Sound Hooks
// ============================================================================
struct XboxTString {
    const char* m_pStr;
    int16_t     m_len;
    int16_t     m_allocLen;
};

static void __cdecl Detour_ShellLog(const char* msg) {
    if (msg && msg[0]) {
        LogPrintf("[SlateEngine] %s\n", msg);
        g_lastHudStrings[g_hudStringIdx] = msg;
        g_hudStringIdx = (g_hudStringIdx + 1) & 7;
    }
}

static void __cdecl Detour_ShellWarning(const char* msg) {
    if (msg && msg[0]) LogPrintf("[SlateWarning] %s\n", msg);
}

static void __cdecl Detour_ShellSetTitle(const XboxTString& title) {
    if (title.m_pStr && title.m_pStr[0]) {
        LogPrintf("[ShellSetTitle] %s\n", title.m_pStr);
        if (g_hWnd && IsWindow(g_hWnd)) {
            char wndTitle[256]{};
            std::snprintf(wndTitle, sizeof(wndTitle), "Buffy the Vampire Slayer (Win32) - %s", title.m_pStr);
            SetWindowTextA(g_hWnd, wndTitle);
        }
    }
}

static void __cdecl Detour_ShellHandleBug(const char* msg, long code, bool bFatal) {
    LogPrintf("[ShellHandleBug] (%ld, fatal=%d): %s\n", code, bFatal ? 1 : 0, msg ? msg : "(null)");
}

static bool __cdecl Detour_ShellAbort(const char* msg) {
    LogPrintf("[ShellAbort] %s\n", msg ? msg : "(null)");
    uint32_t* sp = reinterpret_cast<uint32_t*>(&msg);
    int printed = 0;
    for (int i = 0; i < 512 && printed < 16; ++i) {
        if (IsBadReadPtr(sp + i, 4)) break;
        uint32_t val = sp[i];
        if (val >= 0x00011000 && val < 0x00616D80) {
            char sym[512]{};
            FormatXboxSymbol(val, sym, sizeof(sym));
            LogPrintf("  [ShellAbort Stack +0x%03X] %s\n", i * 4, sym);
            ++printed;
        }
    }
    return false;
}

static void __cdecl Detour_RenderLog(const char* msg) {
    if (msg && msg[0]) LogPrintf("[RenderLog] %s\n", msg);
}

static bool __cdecl Detour_RenderError(const char* msg) {
    if (msg && msg[0]) LogPrintf("[RenderError] %s\n", msg);
    return false;
}

static void __cdecl Detour_ScriptLog(const char* msg) {
    if (msg && msg[0]) LogPrintf("[ScriptLog] %s\n", msg);
}

static void __cdecl Detour_ScriptWarning(const char* msg) {
    if (msg && msg[0]) LogPrintf("[ScriptWarning] %s\n", msg);
}

static bool __cdecl Detour_ScriptError(const char* msg) {
    if (msg && msg[0]) LogPrintf("[ScriptError] %s\n", msg);
    return false;
}

static void PlayXboxMovieWmv(const char* xboxPath, bool bLoop, bool bSkippable);

static void __cdecl Detour_RunInstaller(bool bForce) {
    LogPrintf("[xinstall] Detour_RunInstaller(bForce=%s) invoked from Xbox _main!\n", bForce ? "true" : "false");
    RunInstaller(bForce);
    // Original Xbox RunInstaller plays the 4 intro logo FMVs on cold boot
    if (!g_skipIntroMovies && !g_hasLastLaunchData && !g_autoResumeQuick) {
        static const char* kIntroLogos[4] = {
            "D:\\Interfc\\Logo_Legal.wmv",
            "D:\\Interfc\\Logo_EA.wmv",
            "D:\\Interfc\\Logo_Fox.wmv",
            "D:\\Interfc\\Logo_Collective.wmv"
        };
        for (int i = 0; i < 4; ++i) {
            if (!g_hWnd || !IsWindow(g_hWnd)) break;
            PlayXboxMovieWmv(kIntroLogos[i], false, true);
        }
    }
}

// Safe host-backed replacement for TResourcePack::ReadIndex (0x002B7250).
// Original Xbox code calls fread(&hdr, 0x20, 1, fp) without checking the return value;
// if the read fails or returns 0 bytes, uninitialized stack garbage in hdr[1] is passed
// to TArray<TIndexEntry>::SetLength(), triggering a ~922 MB OOM abort & crash.
struct TStringXbox { const char* str; };
struct TArrayXbox  { int32_t allocLen; int32_t len; void* data; };

static bool __cdecl Detour_TResourcePack_ReadIndex(const TStringXbox* pathStr, TArrayXbox* outEntries) {
    const char* rawPath = (pathStr && pathStr->str) ? pathStr->str : "";
    auto tryOpenValid = [](const char* path, uint32_t hdr[8]) -> FILE* {
        FILE* f = std::fopen(path, "rb");
        if (!f) return nullptr;
        std::memset(hdr, 0, sizeof(uint32_t) * 8);
        size_t got = std::fread(hdr, 1, sizeof(uint32_t) * 8, f);
        if (got == sizeof(uint32_t) * 8 && hdr[1] <= 100000) {
            return f;
        }
        std::fclose(f);
        return nullptr;
    };

    char winPath[512]{};
    xboxkrnl::TranslateXboxPath(rawPath, winPath, sizeof(winPath));

    uint32_t hdr[8]{};
    FILE* fp = tryOpenValid(winPath, hdr);
    if (!fp && rawPath[0] && rawPath[1] == ':' &&
        (rawPath[0] == 'z' || rawPath[0] == 'Z' || rawPath[0] == 'd' || rawPath[0] == 'D')) {
        std::string alt(rawPath);
        alt[0] = (rawPath[0] == 'd' || rawPath[0] == 'D') ? 'Z' : 'D';
        char altWin[512]{};
        if (xboxkrnl::TranslateXboxPath(alt.c_str(), altWin, sizeof(altWin))) {
            fp = tryOpenValid(altWin, hdr);
            if (fp) std::strncpy(winPath, altWin, sizeof(winPath) - 1);
        }
    }
    if (!fp) {
        LogPrintf("[RPE] ReadIndex('%s' -> '%s') FAILED (missing or invalid header)\n", rawPath, winPath);
        return false;
    }

    using SetLengthFn = void(__thiscall*)(TArrayXbox*, int32_t);
    reinterpret_cast<SetLengthFn>(0x002B71F0)(outEntries, static_cast<int32_t>(hdr[1]));
    if (hdr[1] > 0 && outEntries->data) {
        size_t need = static_cast<size_t>(hdr[1]) * 44;
        size_t rd = std::fread(outEntries->data, 1, need, fp);
        if (rd != need) {
            LogPrintf("[RPE] ReadIndex('%s') short index read (%u/%u)\n",
                      rawPath, static_cast<unsigned>(rd), static_cast<unsigned>(need));
        }
    }
    std::fclose(fp);
    LogPrintf("[RPE] ReadIndex('%s') OK (ver=%u, entries=%u)\n", rawPath, hdr[0], hdr[1]);
    return true;
}

static bool __fastcall Detour_TSoundManager_Init(void* thisPtr, void* /*edx_unused*/) {
    LogPrintf("[Sound] TSoundManager::Init(this=0x%08X) -> Win32 Audio Shim active (CSample/Phoneme lip-sync enabled).\n",
              static_cast<uint32_t>(reinterpret_cast<uintptr_t>(thisPtr)));
    uint8_t* p = static_cast<uint8_t*>(thisPtr);
    *reinterpret_cast<float*>(0x007CFD28)   = 1.0f; // g_fMasterVolume
    *reinterpret_cast<uint8_t*>(0x00787D24) = 0;    // g_bPause
    *reinterpret_cast<uint32_t*>(0x007CFD30)= 0;    // g_pCamera
    *reinterpret_cast<uint8_t*>(0x007CFD4F) = 1;    // g_categorizedMemory
    p[0x20] = 0;
    p[0x21] = 0; // m_bSoundEnabled = false (bypasses Xbox MCPX APU hardware registers while keeping CSample cooking intact)
    *reinterpret_cast<uint32_t*>(p + 0x1C) = 0x28;
    *reinterpret_cast<uint32_t*>(p + 0x34) = 3;
    return true;
}

static bool __fastcall Detour_TSoundManager_Shutdown(void* /*thisPtr*/, void* /*edx*/) {
    return true;
}

static void __fastcall Detour_TSoundManager_QuickShutdown(void* /*thisPtr*/, void* /*edx*/) {}

// Atomic multi-core safe replacements for Slate Engine's 2002 single-core TLocker::Lock (0x002AA870) / Unlock (0x002AA950)
static void __fastcall Detour_TLocker_Lock(void* thisPtr, void* /*edx*/) {
    volatile LONG* lockField = reinterpret_cast<volatile LONG*>(static_cast<uint8_t*>(thisPtr) + 8);
    while (InterlockedCompareExchange(lockField, 1, 0) != 0) {
        SwitchToThread();
    }
}

static void __fastcall Detour_TLocker_Unlock(void* thisPtr, void* /*edx*/) {
    volatile LONG* lockField = reinterpret_cast<volatile LONG*>(static_cast<uint8_t*>(thisPtr) + 8);
    InterlockedExchange(lockField, 0);
}

// ============================================================================
// High-Precision 1 MHz QPC Timer Replacement for Slate Engine's TTimerManager
// Original Xbox TTimerManager used RDTSC>>4 calibrated for a 733 MHz Pentium III,
// causing modern 3.5-5.5 GHz PC CPUs to run gameplay at 5x-7x speed!
// Replacing TTimerManager with QueryPerformanceCounter (1 tick = 1 microsecond)
// gives exact 1.000x real-time Xbox speed at ANY framerate (30/60/120/165+ FPS).
// ============================================================================
static LARGE_INTEGER g_qpcFreq{};
static LARGE_INTEGER g_qpcStart{};
static bool          g_qpcInited    = false;
static uint32_t      g_targetFps    = 60; // Default 60 FPS (0 = uncapped)
static uint32_t      g_measuredFps  = 0;
static LARGE_INTEGER g_lastSwapQpc{};
static LARGE_INTEGER g_fpsWindowQpc{};
static uint32_t      g_fpsWindowFrames = 0;

static void EnsureQpcInit() {
    if (!g_qpcInited) {
        QueryPerformanceFrequency(&g_qpcFreq);
        QueryPerformanceCounter(&g_qpcStart);
        timeBeginPeriod(1);
        g_lastSwapQpc  = g_qpcStart;
        g_fpsWindowQpc = g_qpcStart;
        g_qpcInited    = true;
    }
}

static uint32_t GetQpcMicros32() {
    EnsureQpcInit();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    uint64_t elapsed = static_cast<uint64_t>(now.QuadPart - g_qpcStart.QuadPart);
    return static_cast<uint32_t>((elapsed * 1000000ULL) / static_cast<uint64_t>(g_qpcFreq.QuadPart));
}

static uint32_t __fastcall Detour_TTimerManager_GetTimeStamp(void* /*thisPtr*/, void* /*edx*/) {
    return GetQpcMicros32();
}

static float __fastcall Detour_TTimerManager_ConvertClocksToMs(void* /*thisPtr*/, void* /*edx*/, uint32_t clocks) {
    return static_cast<float>(clocks) * 0.001f;
}

static float __fastcall Detour_TTimerManager_ConvertClocksToSec(void* /*thisPtr*/, void* /*edx*/, uint32_t clocks) {
    float sec = static_cast<float>(clocks) * 0.000001f;
    return (sec > 0.1f) ? 0.1f : sec; // Clamp alt-tab / loading hitches to 100ms max
}

static float __fastcall Detour_TTimerManager_ConvertDeltaClocksToMs(void* /*thisPtr*/, void* /*edx*/, uint32_t now, uint32_t prev) {
    uint32_t delta = now - prev;
    return static_cast<float>(delta) * 0.001f;
}

static float __fastcall Detour_TTimerManager_ConvertDeltaClocksToSec(void* /*thisPtr*/, void* /*edx*/, uint32_t now, uint32_t prev) {
    uint32_t delta = now - prev;
    float sec = static_cast<float>(delta) * 0.000001f;
    return (sec > 0.1f) ? 0.1f : sec;
}

static float __fastcall Detour_TTimerManager_GetClocksDeltaAndConvertMs(void* /*thisPtr*/, void* /*edx*/, uint32_t prev) {
    uint32_t delta = GetQpcMicros32() - prev;
    return static_cast<float>(delta) * 0.001f;
}

// ============================================================================
// Engine-Native Hor+ Widescreen / 21:9 Ultrawide Frustum + Projection Hook
// Hooks TIF_Render_Implement::View_SetFOV (0x002A5110) so BOTH the D3D
// projection matrix AND Slate Engine's 6 CPU frustum/portal clipping planes
// match the true 4:3 / 16:9 / 21:9 aspect ratio (zero side-edge culling!).
// ============================================================================
static void*    g_lastViewRenderObj = nullptr;
static float    g_lastRequestedFovX = 0.0f;
static uint32_t g_lastAppliedAspect = 0xFFFFFFFFu;
static bool     g_fovRefreshPending = false;
static int      g_fovOffsetDeg      = 0;

// Mirrors TGO::Map_IsValid() (0x00093990): true during gameplay levels, false on Main Menu / frontend
static bool IsInGameplayMap() {
    uint32_t gamePtr = *reinterpret_cast<const uint32_t*>(0x007D0090);
    if (gamePtr < 0x00010000u || IsBadReadPtr(reinterpret_cast<const void*>(gamePtr), 0x44)) return false;
    uint32_t mapPtr = *reinterpret_cast<const uint32_t*>(gamePtr + 0x40);
    if (mapPtr < 0x00010000u || IsBadReadPtr(reinterpret_cast<const void*>(mapPtr), 0x40)) return false;
    return (*reinterpret_cast<const uint8_t*>(mapPtr + 0x3E) != 0);
}

static bool __fastcall Detour_View_SetFOV(void* thisPtr, void* /*edx*/, float fovX) {
    g_lastViewRenderObj = thisPtr;
    g_lastRequestedFovX = fovX;

    uint32_t vpW = *reinterpret_cast<uint32_t*>(0x007F425C);
    if (vpW == 0) {
        *reinterpret_cast<float*>(0x007F4248) = fovX * 0.5f;
        return false;
    }

    uint32_t asp = IsInGameplayMap() ? d3d9_bridge::GetAspectMode() : 0u;
    g_lastAppliedAspect = asp;
    float targetAspect = (asp == 2u) ? (3440.0f / 1440.0f) : ((asp == 1u) ? (16.0f / 9.0f) : (4.0f / 3.0f));

    // Preserve exact original 4:3 vertical FOV (true Hor+), widen horizontal FOV & frustum planes to match aspect
    bool inMap = IsInGameplayMap();
    float effFovX = fovX;
    if (inMap && g_fovOffsetDeg != 0) {
        effFovX += static_cast<float>(g_fovOffsetDeg) * 0.0174532925f;
        if (effFovX < 0.45f) effFovX = 0.45f;
        if (effFovX > 2.25f) effFovX = 2.25f;
    }
    float halfFov43 = effFovX * 0.5f;
    float tanHalfY  = std::tan(halfFov43) * 0.75f;
    float halfFovY  = std::atan(tanHalfY);
    float halfFovX  = std::atan(tanHalfY * targetAspect);
    if (halfFovX > 1.48f) halfFovX = 1.48f; // Guard against extreme angles

    *reinterpret_cast<float*>(0x007F4248) = halfFovX;
    *reinterpret_cast<float*>(0x007F424C) = halfFovY;

    // Call TIF_Render_Implement::View_InternalSetup(false, true) @ 0x002A4870
    using PFN_ViewInternalSetup = bool(__fastcall*)(void* thisPtr, void* edx_unused, uint32_t b1, uint32_t b2);
    reinterpret_cast<PFN_ViewInternalSetup>(0x002A4870)(thisPtr, nullptr, 0, 1);
    return true;
}

static void FlushPendingFovRefresh() {
    if (g_fovRefreshPending && g_lastViewRenderObj && g_lastRequestedFovX > 0.01f) {
        g_fovRefreshPending = false;
        Detour_View_SetFOV(g_lastViewRenderObj, nullptr, g_lastRequestedFovX);
    }
}
// ============================================================================
static uint8_t* g_fakePushBuffer = nullptr;
static constexpr uint32_t kPushBufferBytes = 4 * 1024 * 1024;

struct XboxD3DSurface {
    uint32_t Common;
    uint32_t Data;
    uint32_t Lock;
    uint32_t Format;
    uint32_t Size;
    void*    Parent;
};

static XboxD3DSurface g_fakeBackBufferSurface{};
static XboxD3DSurface g_fakeDepthStencilSurface{};
static uint8_t*       g_fakeSurfacePixels = nullptr;

static void PumpWin32Messages() {
    MSG msg{};
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            LogPrintf("[GUI] Window closed by user. Exiting cleanly.\n");
            dsound_bridge::Shutdown();
            d3d9_bridge::Shutdown();
            ExitProcess(0);
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    if (!d3d9_bridge::IsReady() && g_hWnd && IsWindow(g_hWnd)) {
        InvalidateRect(g_hWnd, nullptr, FALSE);
        UpdateWindow(g_hWnd);
    }
}

static int32_t __stdcall Detour_Direct3D_CreateDevice(
    uint32_t Adapter,
    uint32_t DeviceType,
    void* /*hFocusWindow*/,
    uint32_t BehaviorFlags,
    uint32_t* pPresentationParameters,
    void** ppReturnedDeviceInterface
) {
    uint32_t width  = pPresentationParameters ? pPresentationParameters[0] : 640;
    uint32_t height = pPresentationParameters ? pPresentationParameters[1] : 480;
    uint32_t fmt    = pPresentationParameters ? pPresentationParameters[2] : 0x1E;
    LogPrintf("[D3D8] Direct3D_CreateDevice(Adapter=%u, Type=%u, Flags=0x%X, %ux%u, Fmt=0x%X)\n",
              Adapter, DeviceType, BehaviorFlags, width, height, fmt);

    if (!g_fakePushBuffer) {
        g_fakePushBuffer = static_cast<uint8_t*>(VirtualAlloc(nullptr, kPushBufferBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    }
    if (!g_fakeSurfacePixels) {
        g_fakeSurfacePixels = static_cast<uint8_t*>(VirtualAlloc(nullptr, 1024 * 768 * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    }

    uint8_t* dev = reinterpret_cast<uint8_t*>(0x0059FEA0); // D3D::g_Device
    std::memset(dev, 0, 0x2AF8);

    // Initialize software pushbuffer pointers in D3D::g_Device
    uint32_t pbBase = reinterpret_cast<uint32_t>(g_fakePushBuffer);
    *reinterpret_cast<uint32_t*>(dev + 0x00) = pbBase; // m_pPushCurrent
    *reinterpret_cast<uint32_t*>(dev + 0x04) = pbBase + (kPushBufferBytes - 0x10000); // m_pPushThreshold
    *reinterpret_cast<uint32_t*>(dev + 0x24) = pbBase; // m_pPushBase
    *reinterpret_cast<uint32_t*>(dev + 0x28) = pbBase + kPushBufferBytes; // m_pPushEnd

    // Initialize fake backbuffer & depth targets
    // Xbox NV2A D3DSurface.Size packing: bits 0..11 = width-1, bits 12..23 = height-1, bits 24..31 = (pitch/64)-1
    uint32_t pitch = width * 4;
    g_fakeBackBufferSurface.Common = 0x00030001u;
    g_fakeBackBufferSurface.Data   = reinterpret_cast<uint32_t>(g_fakeSurfacePixels);
    g_fakeBackBufferSurface.Format = 0x00011E29u; // 640x480 A8R8G8B8 linear
    g_fakeBackBufferSurface.Size   = (((pitch / 64) - 1) << 24) | ((height - 1) << 12) | (width - 1);

    g_fakeDepthStencilSurface = g_fakeBackBufferSurface;

    *reinterpret_cast<uint32_t*>(dev + 0x2070) = reinterpret_cast<uint32_t>(&g_fakeBackBufferSurface); // m_pRenderTarget
    *reinterpret_cast<uint32_t*>(dev + 0x2074) = reinterpret_cast<uint32_t>(&g_fakeDepthStencilSurface); // m_pZBuffer
    *reinterpret_cast<uint32_t*>(dev + 0x2078) = reinterpret_cast<uint32_t>(&g_fakeDepthStencilSurface); // m_pAutoDepthStencil
    *reinterpret_cast<uint32_t*>(dev + 0x207C) = reinterpret_cast<uint32_t>(&g_fakeBackBufferSurface); // m_pBackBuffer
    *reinterpret_cast<uint32_t*>(dev + 0x2080) = reinterpret_cast<uint32_t>(&g_fakeBackBufferSurface); // m_pFrontBuffer
    *reinterpret_cast<uint32_t*>(dev + 0x2088) = reinterpret_cast<uint32_t>(&g_fakeDepthStencilSurface);
    *reinterpret_cast<uint32_t*>(dev + 0x0380) = 0x0059FD78u; // g_FvfVertexShader

    *reinterpret_cast<uint32_t*>(0x005A29A0) = 0x0059FEA0u; // D3D::g_pDevice
    *reinterpret_cast<uint32_t*>(0x005A29A4) = 0x0059FEA0u; // _g_pPushBuffer
    *reinterpret_cast<uint32_t*>(0x005A02E0) = 1;

    // Initialize Xbox D3D8 power-on state defaults in _D3D__RenderState (0x0059F940) & _D3D__TextureState (0x0059F740)
    uint32_t* rs = reinterpret_cast<uint32_t*>(0x0059F940);
    std::memset(rs, 0, 150 * sizeof(uint32_t));
    rs[57]  = 0x203u;      // D3DRS_ZFUNC = LESSEQUAL
    rs[58]  = 0x207u;      // D3DRS_ALPHAFUNC = ALWAYS
    rs[59]  = 0;           // D3DRS_ALPHABLENDENABLE = FALSE
    rs[60]  = 0;           // D3DRS_ALPHATESTENABLE = FALSE
    rs[61]  = 0;           // D3DRS_ALPHAREF = 0
    rs[62]  = 1;           // D3DRS_SRCBLEND = ONE
    rs[63]  = 0;           // D3DRS_DESTBLEND = ZERO
    rs[64]  = 1;           // D3DRS_ZWRITEENABLE = TRUE
    rs[83]  = 0;           // D3DRS_FOGENABLE = FALSE
    rs[84]  = 3;           // D3DRS_FOGTABLEMODE = LINEAR
    rs[85]  = 0;           // D3DRS_FOGSTART = 0.0f
    rs[86]  = 0x3F800000u; // D3DRS_FOGEND = 1.0f
    rs[87]  = 0x3F800000u; // D3DRS_FOGDENSITY = 1.0f
    rs[125] = 1;           // D3DRS_ZENABLE = TRUE
    rs[129] = 0x901u;      // D3DRS_CULLMODE = CCW
    rs[130] = 0xFFFFFFFFu; // D3DRS_TEXTUREFACTOR = white

    uint32_t* ts = reinterpret_cast<uint32_t*>(0x0059F740);
    std::memset(ts, 0, 4 * 32 * sizeof(uint32_t));
    for (uint32_t s = 0; s < 4; ++s) {
        uint32_t* st = ts + s * 32;
        st[0]  = 1; // ADDRESSU = WRAP
        st[1]  = 1; // ADDRESSV = WRAP
        st[3]  = 2; // MAGFILTER = LINEAR
        st[4]  = 2; // MINFILTER = LINEAR
        st[12] = (s == 0) ? 4u : 1u; // COLOROP = MODULATE on stage 0, DISABLE on 1..3
        st[14] = 2; // COLORARG1 = TEXTURE
        st[15] = 0; // COLORARG2 = DIFFUSE
        st[16] = (s == 0) ? 4u : 1u; // ALPHAOP = MODULATE on stage 0, DISABLE on 1..3
        st[18] = 2; // ALPHAARG1 = TEXTURE
        st[19] = 0; // ALPHAARG2 = DIFFUSE
        st[28] = s; // TEXCOORDINDEX = stage index (0, 1, 2, 3)
    }

    if (ppReturnedDeviceInterface) {
        *ppReturnedDeviceInterface = dev;
    }

    // Initialize real hardware Direct3D 9 HD backend targeting the viewport!
    if (g_hWnd && IsWindow(g_hWnd)) {
        uint32_t resScale = 3; // Default 3x (1920x1440 native HD)
        const char* cl = GetCommandLineA();
        if (cl) {
            if (std::strstr(cl, "--scale=1")) resScale = 1;
            else if (std::strstr(cl, "--scale=2")) resScale = 2;
            else if (std::strstr(cl, "--scale=3")) resScale = 3;
            else if (std::strstr(cl, "--scale=4")) resScale = 4;
        }
        d3d9_bridge::Init(g_hWnd, width, height, resScale);
    }
    return 0; // D3D_OK
}

static volatile uint32_t* __stdcall Detour_D3D_MakeRequestedSpace(uint32_t /*Space*/, uint32_t /*SegmentSize*/) {
    uint8_t* dev = reinterpret_cast<uint8_t*>(0x0059FEA0);
    uint32_t pbBase = reinterpret_cast<uint32_t>(g_fakePushBuffer);
    *reinterpret_cast<uint32_t*>(dev + 0x00) = pbBase;
    *reinterpret_cast<uint32_t*>(dev + 0x04) = pbBase + (kPushBufferBytes - 0x10000);
    return reinterpret_cast<volatile uint32_t*>(pbBase);
}

static volatile uint32_t* __stdcall Detour_D3D_MakeSpace() {
    return Detour_D3D_MakeRequestedSpace(0, 0);
}

static void __fastcall Detour_D3D_KickOff(void* /*thisPtr*/, void* /*edx*/) {
    Detour_D3D_MakeRequestedSpace(0, 0);
}

static uint32_t __stdcall Detour_D3D_SetFence(uint32_t /*Flags*/) { return 1; }
static void __stdcall Detour_D3D_BlockOnTime(uint32_t /*Time*/, int /*b*/) {}
static void __stdcall Detour_D3D_KickOffAndWaitForIdle() { Detour_D3D_MakeRequestedSpace(0, 0); }
static void __stdcall Detour_D3D_BlockOnResource(void* /*pRes*/) {}

static void __stdcall Detour_D3DDevice_Clear(
    uint32_t Count,
    const void* pRects,
    uint32_t Flags,
    uint32_t Color,
    float Z,
    uint32_t Stencil
) {
    if (Flags & 0x000000F0u /*D3DCLEAR_TARGET*/) {
        g_clearColor = Color & 0x00FFFFFFu;
    }
    d3d9_bridge::Clear(Count, pRects, Flags, Color, Z, Stencil);
}

static void __stdcall Detour_D3DDevice_Swap(uint32_t Flags) {
    ++g_frameCount;
    g_lastSwapTick = GetTickCount();
    Detour_D3D_MakeRequestedSpace(0, 0);
    d3d9_bridge::Swap(Flags);

    // Keep Main Menu / frontend pillarboxed to centered 4:3 so 3D menu glows align behind 2D text;
    // automatically expand to full 16:9 / 21:9 Hor+ widescreen as soon as a gameplay map loads!
    // Defer View_SetFOV to TTimerManager::Update (outside Slate's non-recursive D3D CLocker 0x007F30DC!).
    bool inMenu = !IsInGameplayMap();
    d3d9_bridge::SetMenuPillarbox(inMenu);
    uint32_t curAsp = inMenu ? 0u : d3d9_bridge::GetAspectMode();
    if (g_lastViewRenderObj && g_lastRequestedFovX > 0.01f && curAsp != g_lastAppliedAspect) {
        g_fovRefreshPending = true;
    }

    // Auto-restore Buffy's exact saved position after a Quick Load / Auto-Resume level boot
    if (g_pendingQuickPosRestore) {
        void* player = GetLivePlayerObject();
        if (player) {
            if (++g_playerAliveFrames >= 20) {
                ApplyPlayerMatrix(player, g_pendingQuickState.playerMtx);
                LogPrintf("[QuickLoad] Restored Buffy to exact saved spot (%.2f, %.2f, %.2f)!\n",
                          g_pendingQuickState.playerMtx[9],
                          g_pendingQuickState.playerMtx[10],
                          g_pendingQuickState.playerMtx[11]);
                g_pendingQuickPosRestore = false;
            }
        } else {
            g_playerAliveFrames = 0;
        }
    }

    // Handle Quick Save (F5 / Slots 1..5) safely on the main game thread
    if (InterlockedExchange(&g_quickSaveReq, 0) != 0) {
        int slot = g_reqSaveSlot;
        void* player = GetLivePlayerObject();
        if (player || g_hasLastLaunchData) {
            QuickSaveState qs{};
            qs.magic   = 0x53514642u; // 'BFQS'
            qs.version = 1u;
            if (g_hasLastLaunchData) {
                qs.hasLaunchData = 1u;
                std::memcpy(qs.launchData, g_lastLaunchData, sizeof(qs.launchData));
                SaveQuickLaunchBlob(g_lastLaunchData);
            }
            if (player) {
                qs.hasPlayerMtx = 1u;
                std::memcpy(qs.playerMtx, static_cast<const uint8_t*>(player) + 0x21C, sizeof(qs.playerMtx));
            }
            if (WriteQuickSaveState(qs, slot)) {
                LogPrintf("[QuickSave] Saved Slot %d exact spot! (level=%u, pos=%u: %.2f, %.2f, %.2f)\n",
                          slot, qs.hasLaunchData, qs.hasPlayerMtx,
                          qs.playerMtx[9], qs.playerMtx[10], qs.playerMtx[11]);
                char notice[64]{};
                std::snprintf(notice, sizeof(notice), "Quick Saved (Slot %d)!", slot);
                SetQuickStatusNotice(notice);
            } else {
                LogPrintf("[QuickSave] Failed to write Slot %d!\n", slot);
            }
        } else {
            LogPrintf("[QuickSave] Not in a level yet — enter gameplay first, then press F5!\n");
        }
    }

    // Handle Quick Load (F6 / Slots 1..5) safely on the main game thread
    if (InterlockedExchange(&g_quickLoadReq, 0) != 0) {
        int slot = g_reqLoadSlot;
        QuickSaveState qs{};
        if (ReadQuickSaveState(qs, slot)) {
            void* player = GetLivePlayerObject();
            bool sameLevel = (!qs.hasLaunchData) ||
                             (g_hasLastLaunchData && std::memcmp(qs.launchData, g_lastLaunchData, sizeof(qs.launchData)) == 0);
            if (player && qs.hasPlayerMtx && sameLevel) {
                ApplyPlayerMatrix(player, qs.playerMtx);
                LogPrintf("[QuickLoad] Teleported Buffy instantly to Slot %d spot (%.2f, %.2f, %.2f)!\n",
                          slot, qs.playerMtx[9], qs.playerMtx[10], qs.playerMtx[11]);
                char notice[64]{};
                std::snprintf(notice, sizeof(notice), "Quick Loaded (Slot %d)!", slot);
                SetQuickStatusNotice(notice);
            } else if (qs.hasLaunchData) {
                char qlFlagPath[512]{};
                xboxkrnl::TranslateXboxPath("T:\\quickload_restore.flag", qlFlagPath, sizeof(qlFlagPath));
                FILE* ff = std::fopen(qlFlagPath, "wb");
                if (ff) { std::fputc('0' + slot, ff); std::fclose(ff); }
                LogPrintf("[QuickLoad] Reloading Slot %d saved level & exact spot...\n", slot);
                Detour_XLaunchNewImageA("d:\\default.xbe", qs.launchData);
            } else {
                LogPrintf("[QuickLoad] Saved state in Slot %d has no level launch data!\n", slot);
            }
        } else {
            LogPrintf("[QuickLoad] Slot %d is empty — press F5 in-game first!\n", slot);
            char notice[64]{};
            std::snprintf(notice, sizeof(notice), "Slot %d Empty!", slot);
            SetQuickStatusNotice(notice);
        }
    }

    // Handle live Buffy Costume switch: save CURRENT level & exact spot, then reload it in-place with the new outfit
    if (InterlockedExchange(&g_costumeReloadReq, 0) != 0) {
        void* player = GetLivePlayerObject();
        if (player && g_hasLastLaunchData) {
            QuickSaveState qs{};
            qs.magic         = 0x53514642u;
            qs.version       = 1u;
            qs.hasLaunchData = 1u;
            std::memcpy(qs.launchData, g_lastLaunchData, sizeof(qs.launchData));
            qs.hasPlayerMtx  = 1u;
            std::memcpy(qs.playerMtx, static_cast<const uint8_t*>(player) + 0x21C, sizeof(qs.playerMtx));
            WriteQuickSaveState(qs, g_activeSaveSlot);
            char qlFlagPath[512]{};
            xboxkrnl::TranslateXboxPath("T:\\quickload_restore.flag", qlFlagPath, sizeof(qlFlagPath));
            FILE* ff = std::fopen(qlFlagPath, "wb");
            if (ff) { std::fputc('0' + g_activeSaveSlot, ff); std::fclose(ff); }
            LogPrintf("[Costume] Reloading current level in-place at (%.2f, %.2f, %.2f) with costume=%u...\n",
                      qs.playerMtx[9], qs.playerMtx[10], qs.playerMtx[11], g_buffyCostume);
            Detour_XLaunchNewImageA("d:\\default.xbe", qs.launchData);
        }
    }

    // Handle Chapter / Arena Launcher (Game -> Jump to Level / Arena)
    if (InterlockedExchange(&g_jumpLevelReq, 0) != 0 && g_jumpMapName[0]) {
        uint8_t launchBuf[3072]{};
        std::snprintf(reinterpret_cast<char*>(launchBuf), sizeof(launchBuf),
                      "root:Z:\\ rpecopy:d:\\ controller:0 memoryunit:0 pack:2 session:Buffy sessionload:0 preservebackbuffer:1 preservelogfile:1 map:%s",
                      g_jumpMapName);
        LogPrintf("[Launcher] Jumping straight to map '%s'...\n", g_jumpMapName);
        Detour_XLaunchNewImageA("d:\\default.xbe", launchBuf);
    }

    if (g_frameCount == 1 || (g_frameCount % 60) == 0) {
        LogPrintf("[D3D8] Frame %u presented (DrawCalls=%u, ClearColor=0x%06X, FPS=%u)\n",
                  g_frameCount, g_drawCalls, g_clearColor, g_measuredFps);
    }
    PumpWin32Messages();

    // Microsecond-accurate QPC frame pacer (replaces jittery Sleep(8))
    EnsureQpcInit();
    uint32_t target = g_targetFps;
    if (target > 0) {
        int64_t targetTicks = g_qpcFreq.QuadPart / static_cast<int64_t>(target);
        int64_t deadline    = g_lastSwapQpc.QuadPart + targetTicks;
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        // Sleep(1) while more than 1.5ms remain, then spin-wait for exact frame pacing
        int64_t spinThreshold = (g_qpcFreq.QuadPart * 3) / 2000;
        while (now.QuadPart + spinThreshold < deadline) {
            Sleep(1);
            QueryPerformanceCounter(&now);
        }
        while (now.QuadPart < deadline) {
            YieldProcessor();
            QueryPerformanceCounter(&now);
        }
        // Avoid spiral-of-death if we overshot by more than 2 frames
        g_lastSwapQpc.QuadPart = (now.QuadPart - deadline > targetTicks * 2) ? now.QuadPart : deadline;
    } else {
        QueryPerformanceCounter(&g_lastSwapQpc);
    }

    // Update live measured FPS every 0.5 seconds
    ++g_fpsWindowFrames;
    LARGE_INTEGER fpsNow{};
    QueryPerformanceCounter(&fpsNow);
    double fpsSec = static_cast<double>(fpsNow.QuadPart - g_fpsWindowQpc.QuadPart) / static_cast<double>(g_qpcFreq.QuadPart);
    if (fpsSec >= 0.5) {
        g_measuredFps = static_cast<uint32_t>((static_cast<double>(g_fpsWindowFrames) / fpsSec) + 0.5);
        g_fpsWindowFrames = 0;
        g_fpsWindowQpc = fpsNow;
    }
}

static void __stdcall Detour_D3DDevice_DrawVerticesUP(
    uint32_t PrimitiveType,
    uint32_t VertexCount,
    const void* pVertexStreamZeroData,
    uint32_t VertexStreamZeroStride
) {
    ++g_drawCalls;
    d3d9_bridge::DrawVerticesUP(PrimitiveType, VertexCount, pVertexStreamZeroData, VertexStreamZeroStride);
}

static void __stdcall Detour_D3DDevice_DrawIndexedVerticesUP(
    uint32_t PrimitiveType,
    uint32_t VertexCount,
    const void* pIndexData,
    const void* pVertexStreamZeroData,
    uint32_t VertexStreamZeroStride
) {
    ++g_drawCalls;
    d3d9_bridge::DrawIndexedVerticesUP(PrimitiveType, VertexCount, pIndexData, pVertexStreamZeroData, VertexStreamZeroStride);
}

static void __stdcall Detour_D3DDevice_DrawIndexedVertices(
    uint32_t PrimitiveType,
    uint32_t VertexCount,
    const void* pIndexData
) {
    ++g_drawCalls;
    d3d9_bridge::DrawIndexedVertices(PrimitiveType, VertexCount, pIndexData);
}

static void __stdcall Detour_D3DDevice_SetViewport(const void* pViewport) {
    d3d9_bridge::SetViewport(pViewport);
}

static void __stdcall Detour_D3DDevice_SetTransform(uint32_t State, const void* pMatrix) {
    d3d9_bridge::SetTransform(State, pMatrix);
}

static void __stdcall Detour_D3DDevice_SetVertexShader(uint32_t Handle) {
    d3d9_bridge::SetVertexShader(Handle);
}

static uint32_t __fastcall Detour_Precache_Shader(void* thisPtr, void* /*edx*/, int32_t shaderId, const uint32_t* pDecl) {
    using LockFn   = void(__thiscall*)(void*);
    using GetFn    = const uint32_t*(__thiscall*)(void*, int32_t);
    using CreateFn = HRESULT(__stdcall*)(const uint32_t*, const uint32_t*, uint32_t*, uint32_t);
    auto* locker = reinterpret_cast<void*>(0x007F30DC);
    reinterpret_cast<LockFn>(0x002AA870)(locker);
    const uint32_t* pFunc = (shaderId == -1) ? nullptr : reinterpret_cast<GetFn>(0x0029D700)(thisPtr, shaderId);
    uint32_t handle = 0;
    reinterpret_cast<CreateFn>(0x00594720)(pDecl, pFunc, &handle, 0);
    reinterpret_cast<LockFn>(0x002AA950)(locker);
    if (handle != 0 && pDecl != nullptr) {
        d3d9_bridge::RegisterVertexShaderDecl(handle, pDecl, shaderId);
    }
    return handle;
}

static void __stdcall Detour_D3DDevice_SetStreamSource(uint32_t StreamNumber, const void* pStreamData, uint32_t Stride) {
    d3d9_bridge::SetStreamSource(StreamNumber, pStreamData, Stride);
}

static void __stdcall Detour_D3DDevice_SetIndices(const void* pIndexData, uint32_t BaseVertexIndex) {
    d3d9_bridge::SetIndices(pIndexData, BaseVertexIndex);
}

static void __stdcall Detour_D3DDevice_SetTexture(uint32_t Stage, const void* pTexture) {
    d3d9_bridge::SetTexture(Stage, pTexture);
}

static void __stdcall Detour_D3DDevice_SetPalette(uint32_t Stage, const void* pPalette) {
    d3d9_bridge::SetPalette(Stage, pPalette);
}

static void __stdcall Detour_D3DDevice_SetRenderStateNotInline(uint32_t State, uint32_t Value) {
    d3d9_bridge::SetRenderState(State, Value);
}

static void __stdcall Detour_D3DDevice_SetRenderState_CullMode(uint32_t Value) {
    d3d9_bridge::SetRenderState_CullMode(Value);
}

static void __stdcall Detour_D3DDevice_SetRenderState_ZEnable(uint32_t Value) {
    d3d9_bridge::SetRenderState_ZEnable(Value);
}

static void __stdcall Detour_D3DDevice_SetRenderState_TextureFactor(uint32_t Value) {
    d3d9_bridge::SetRenderState_TextureFactor(Value);
}

static void __stdcall Detour_D3DDevice_SetRenderState_FogColor(uint32_t Value) {
    d3d9_bridge::SetRenderState_FogColor(Value);
}

static void __stdcall Detour_D3DDevice_SetTextureStageStateNotInline(uint32_t Stage, uint32_t Type, uint32_t Value) {
    d3d9_bridge::SetTextureStageState(Stage, Type, Value);
}

// TIF_Render_Implement::RenderString (0x0029D2A0)
static bool __fastcall Detour_RenderString(
    void* /*thisPtr*/,
    void* /*edx*/,
    const void* /*pFont*/,
    const float* pos2D,
    const char* text,
    const uint32_t* /*pColor*/,
    float /*scaleX*/,
    float /*scaleY*/,
    long /*flags*/
) {
    if (text && text[0]) {
        g_lastHudStrings[g_hudStringIdx] = text;
        g_hudStringIdx = (g_hudStringIdx + 1) & 7;
        static std::string s_lastLogged;
        if (s_lastLogged != text) {
            s_lastLogged = text;
            LogPrintf("[RenderString @ (%.0f,%.0f)] %s\n",
                      pos2D ? pos2D[0] : 0.0f, pos2D ? pos2D[1] : 0.0f, text);
        }
    }
    return true;
}

// TIF_Render_Implement::RenderRectangle (0x0029D2B0)
struct XboxTRectangle {
    int32_t x0, y0, x1, y1;
};

static bool __fastcall Detour_RenderRectangle(
    void* /*thisPtr*/,
    void* /*edx*/,
    const XboxTRectangle* rect,
    bool bFilled,
    const uint8_t* rgba,
    bool /*bBlend*/,
    float /*z*/,
    bool /*bScreenSpace*/
) {
    if (g_framebuffer && rect) {
        int x0 = std::max(0, std::min(639, static_cast<int>(rect->x0)));
        int y0 = std::max(0, std::min(479, static_cast<int>(rect->y0)));
        int x1 = std::max(0, std::min(640, static_cast<int>(rect->x1)));
        int y1 = std::max(0, std::min(480, static_cast<int>(rect->y1)));
        uint32_t col = rgba ? ((static_cast<uint32_t>(rgba[2]) << 16) | (static_cast<uint32_t>(rgba[1]) << 8) | rgba[0]) : 0x00405070u;
        if (bFilled) {
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    g_framebuffer[y * 640 + x] = col;
                }
            }
        }
    }
    return true;
}

static bool ContainsCaseInsensitive(const char* hay, const char* needle) {
    if (!hay || !needle || !needle[0]) return false;
    size_t nlen = std::strlen(needle);
    for (const char* p = hay; *p; ++p) {
        if (_strnicmp(p, needle, nlen) == 0) return true;
    }
    return false;
}

static void PlayXboxMovieWmv(const char* xboxPath, bool bLoop, bool bSkippable) {
    if (!xboxPath || !xboxPath[0] || !g_hWnd || !IsWindow(g_hWnd)) return;

    // Skip looping background loading spinners (loading_in, summoning, channeling, etc.) & INSTALLING.wmv
    if (bLoop || ContainsCaseInsensitive(xboxPath, "INSTALLING")) return;
    if (g_skipIntroMovies && ContainsCaseInsensitive(xboxPath, "Logo_")) {
        LogPrintf("[Movie_Play] Skipping intro movie '%s' (Skip Intro Logos enabled)\n", xboxPath);
        return;
    }

    char hostPath[512]{};
    auto tryResolve = [&](const char* candidate) -> bool {
        xboxkrnl::TranslateXboxPath(candidate, hostPath, sizeof(hostPath));
        return GetFileAttributesA(hostPath) != INVALID_FILE_ATTRIBUTES;
    };

    bool found = false;
    // 1. Check optional HD FMV override folder: recomp\hd_movies\<stem>.(mp4|wmv|avi)
    {
        const char* base = xboxPath;
        for (const char* p = xboxPath; *p; ++p) {
            if (*p == '\\' || *p == '/') base = p + 1;
        }
        std::string stem = base;
        size_t dot = stem.find_last_of('.');
        if (dot != std::string::npos) stem.resize(dot);

        char cacheRoot[512]{};
        xboxkrnl::TranslateXboxPath("Z:\\", cacheRoot, sizeof(cacheRoot));
        std::string hdMoviesDir = cacheRoot;
        while (!hdMoviesDir.empty() && (hdMoviesDir.back() == '\\' || hdMoviesDir.back() == '/')) {
            hdMoviesDir.pop_back();
        }
        size_t slash = hdMoviesDir.find_last_of("\\/");
        if (slash != std::string::npos && !stem.empty()) {
            hdMoviesDir = hdMoviesDir.substr(0, slash) + "\\hd_movies";
            CreateDirectoryA(hdMoviesDir.c_str(), nullptr);
            static const char* kExts[3] = { ".mp4", ".wmv", ".avi" };
            for (const char* ext : kExts) {
                std::string cand = hdMoviesDir + "\\" + stem + ext;
                if (GetFileAttributesA(cand.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    std::snprintf(hostPath, sizeof(hostPath), "%s", cand.c_str());
                    LogPrintf("[Movie_Play] Using HD movie override: '%s'\n", hostPath);
                    found = true;
                    break;
                }
            }
        }
    }

    if (!found) {
        if (xboxPath[0] && xboxPath[1] == ':') {
            found = tryResolve(xboxPath);
        } else {
            // Slate Engine passes relative paths like "interfc\Logo_Fox.wmv" or "interfc\attract.wmv"
            std::string zPath = std::string("Z:\\") + xboxPath;
            std::string dPath = std::string("D:\\") + xboxPath;
            found = tryResolve(zPath.c_str()) || tryResolve(dPath.c_str()) || tryResolve(xboxPath);
        }
    }
    if (!found) {
        LogPrintf("[Movie_Play] File not found for '%s' (last tried: '%s')\n", xboxPath, hostPath);
        return;
    }

    int wlen = MultiByteToWideChar(CP_ACP, 0, hostPath, -1, nullptr, 0);
    if (wlen <= 1) return;
    std::wstring wpath(static_cast<size_t>(wlen - 1), L'\0');
    MultiByteToWideChar(CP_ACP, 0, hostPath, -1, &wpath[0], wlen);

    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IGraphBuilder* pGraph  = nullptr;
    IMediaControl* pCtrl   = nullptr;
    IMediaEventEx* pEvent  = nullptr;
    IVideoWindow*  pVidWin = nullptr;
    IBasicVideo*   pBasVid = nullptr;
    IBasicAudio*   pAudio  = nullptr;

    if (SUCCEEDED(CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_IGraphBuilder, reinterpret_cast<void**>(&pGraph))) && pGraph) {
        if (SUCCEEDED(pGraph->RenderFile(wpath.c_str(), nullptr))) {
            pGraph->QueryInterface(IID_IMediaControl, reinterpret_cast<void**>(&pCtrl));
            pGraph->QueryInterface(IID_IMediaEventEx, reinterpret_cast<void**>(&pEvent));
            pGraph->QueryInterface(IID_IVideoWindow,  reinterpret_cast<void**>(&pVidWin));
            pGraph->QueryInterface(IID_IBasicVideo,   reinterpret_cast<void**>(&pBasVid));
            pGraph->QueryInterface(IID_IBasicAudio,   reinterpret_cast<void**>(&pAudio));

            // Clear D3D9 backbuffer to pure black for clean 4:3 pillarboxing on 16:9 / 21:9
            if (d3d9_bridge::IsReady()) {
                d3d9_bridge::Clear(0, nullptr, 0x000000F0u, 0xFF000000u, 1.0f, 0);
                d3d9_bridge::Swap(0);
            }

            long vidW = 640, vidH = 480;
            if (pBasVid) {
                long vw = 0, vh = 0;
                if (SUCCEEDED(pBasVid->GetVideoSize(&vw, &vh)) && vw > 0 && vh > 0) {
                    vidW = vw; vidH = vh;
                }
            }

            RECT lastRc{};
            auto updateVideoPos = [&]() {
                if (!pVidWin || !IsWindow(g_hWnd)) return;
                RECT rc{};
                GetClientRect(g_hWnd, &rc);
                if (rc.right == lastRc.right && rc.bottom == lastRc.bottom && lastRc.right > 0) return;
                lastRc = rc;
                long cw = rc.right - rc.left;
                long ch = rc.bottom - rc.top;
                if (cw <= 0 || ch <= 0) return;
                // Fit video preserving 4:3 (or native video aspect) centered inside viewport
                long targetW = cw;
                long targetH = (cw * vidH) / vidW;
                if (targetH > ch) {
                    targetH = ch;
                    targetW = (ch * vidW) / vidH;
                }
                long vx = (cw - targetW) / 2;
                long vy = (ch - targetH) / 2;
                // Cover the entire viewport with the DirectShow video window (pure black border)
                // and place the 4:3 video destination centered inside it so pillarboxes are always jet black!
                pVidWin->put_BorderColor(RGB(0, 0, 0));
                pVidWin->SetWindowPosition(0, 0, cw, ch);
                if (pBasVid) {
                    pBasVid->SetDestinationPosition(vx, vy, targetW, targetH);
                } else {
                    pVidWin->SetWindowPosition(vx, vy, targetW, targetH);
                }
            };

            if (pVidWin) {
                pVidWin->put_Owner(reinterpret_cast<OAHWND>(g_hWnd));
                pVidWin->put_WindowStyle(WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN);
                pVidWin->put_MessageDrain(reinterpret_cast<OAHWND>(g_hWnd));
                updateVideoPos();
                pVidWin->put_Visible(OATRUE);
            }

            auto syncVolume = [&]() {
                if (!pAudio) return;
                bool muted = dsound_bridge::IsMuted() ||
                             (dsound_bridge::IsMuteInBackground() && !IsGameWindowFocused());
                int vol = muted ? 0 : dsound_bridge::GetMasterVolume();
                long db = (vol <= 0) ? -10000L : static_cast<long>(2000.0 * std::log10(static_cast<double>(vol) / 100.0));
                if (db < -10000L) db = -10000L;
                if (db > 0L)      db = 0L;
                pAudio->put_Volume(db);
            };
            syncVolume();

            if (pCtrl && SUCCEEDED(pCtrl->Run())) {
                LogPrintf("[Movie_Play] Native DirectShow playback started (%ldx%ld): '%s'\n", vidW, vidH, hostPath);
                DWORD startTick = GetTickCount();
                bool prevSkipDown = true; // require fresh press after grace period
                while (IsWindow(g_hWnd)) {
                    PumpWin32Messages();
                    updateVideoPos();
                    syncVolume();

                    if (pEvent) {
                        long evCode = 0;
                        LONG_PTR p1 = 0, p2 = 0;
                        bool ended = false;
                        while (SUCCEEDED(pEvent->GetEvent(&evCode, &p1, &p2, 0))) {
                            pEvent->FreeEventParams(evCode, p1, p2);
                            if (evCode == EC_COMPLETE || evCode == EC_USERABORT || evCode == EC_ERRORABORT) {
                                ended = true;
                            }
                        }
                        if (ended) break;
                    }

                    // Allow skipping any intro/FMV via Space, Enter, Esc, LMB, or Gamepad A/B/Start
                    bool skipDown = false;
                    if (IsGameWindowFocused()) {
                        if ((GetAsyncKeyState(VK_SPACE)  & 0x8000) ||
                            (GetAsyncKeyState(VK_RETURN) & 0x8000) ||
                            (GetAsyncKeyState(VK_ESCAPE) & 0x8000) ||
                            (GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
                            skipDown = true;
                        }
                        if (!skipDown && g_winXInputGetState) {
                            WIN_XINPUT_STATE ws{};
                            if (g_winXInputGetState(g_activeWinPadIndex, &ws) == ERROR_SUCCESS) {
                                if (ws.Gamepad.wButtons & (0x1000u | 0x2000u | 0x0010u)) { // A, B, Start
                                    skipDown = true;
                                }
                            }
                        }
                    }
                    if (GetTickCount() - startTick > 250) {
                        if (skipDown && !prevSkipDown && (bSkippable || !bLoop)) {
                            LogPrintf("[Movie_Play] Skipped by user.\n");
                            break;
                        }
                    }
                    prevSkipDown = skipDown;
                    Sleep(10);
                }
                pCtrl->Stop();
            }
            if (pVidWin) {
                pVidWin->put_Visible(OAFALSE);
                pVidWin->put_Owner(0);
            }
        } else {
            LogPrintf("[Movie_Play] DirectShow RenderFile failed for '%s'\n", hostPath);
        }
    }

    if (pAudio)  pAudio->Release();
    if (pBasVid) pBasVid->Release();
    if (pVidWin) pVidWin->Release();
    if (pEvent)  pEvent->Release();
    if (pCtrl)   pCtrl->Release();
    if (pGraph)  pGraph->Release();
    if (SUCCEEDED(hrCo)) CoUninitialize();
}

// TIF_Render_Implement::Movie_Play (0x00297A90)
static void* __fastcall Detour_Movie_Play(
    void* /*thisPtr*/,
    void* /*edx*/,
    const void* pFileName,
    bool bLoop,
    bool bSkippable,
    bool /*barg4*/,
    void* /*pDirectSound*/
) {
    // TFileName has file path string at +0x12 (or call TFileName::GetFilePath at 0x002B6730)
    using GetFilePathFn = const char* (__fastcall*)(const void* thisPtr, void* edx, int appendType);
    const char* path = reinterpret_cast<GetFilePathFn>(0x002B6730)(pFileName, nullptr, 2);
    LogPrintf("[Movie_Play] Playing '%s' (loop=%d, skippable=%d)\n",
              path ? path : "(unknown)", bLoop ? 1 : 0, bSkippable ? 1 : 0);
    if (path) {
        g_lastMovieName = path;
        PlayXboxMovieWmv(path, bLoop, bSkippable);
    }
    PumpWin32Messages();
    return nullptr;
}

static void* __fastcall Detour_Movie_Playing(void* /*thisPtr*/, void* /*edx*/) {
    return nullptr;
}

static bool __fastcall Detour_Movie_Kill(void* /*thisPtr*/, void* /*edx*/, void* /*hMovie*/, bool /*bWait*/) {
    return true;
}

static bool __fastcall Detour_Movie_KillCurrent(void* /*thisPtr*/, void* /*edx*/, bool /*bWait*/) {
    return true;
}

static void __cdecl Detour_XboxExit(int code) {
    LogPrintf("[XBOX-EXIT] _exit(%d) called!\n", code);
    ExitProcess(static_cast<UINT>(code));
}

static void __cdecl Detour_XboxAmsgExit(int rterrnum) {
    LogPrintf("[XBOX-EXIT] __amsg_exit(%d) called!\n", rterrnum);
    ExitProcess(255);
}

static void __cdecl Detour_G_ErrorToUser(uint32_t errType, bool bFatal, const void* pTString) {
    const char* msg = "(null)";
    if (pTString && !IsBadReadPtr(pTString, 8)) {
        const char* p = *reinterpret_cast<const char* const*>(pTString);
        if (p && !IsBadReadPtr(p, 1)) msg = p;
    }
    LogPrintf("[XBOX-ERROR] G_ErrorToUser(type=%u, fatal=%d, msg='%s')\n", errType, bFatal ? 1 : 0, msg);
}

// ============================================================================
// XBE Loader & Patcher
// ============================================================================
#pragma pack(push, 1)
struct XbeSectionHeader {
    uint32_t dwFlags;
    uint32_t dwVirtualAddr;
    uint32_t dwVirtualSize;
    uint32_t dwRawAddr;
    uint32_t dwRawSize;
    uint32_t pszSectionNameAddr;
    uint32_t dwSectionRefCount;
    uint32_t pHeadSharedPageRefCountAddr;
    uint32_t pTailSharedPageRefCountAddr;
    uint8_t  bzSectionDigest[20];
};
#pragma pack(pop)

bool LoadAndBridgeDefaultXbe(const char* xbePath) {
    FILE* f = std::fopen(xbePath, "rb");
    if (!f) {
        LogPrintf("[XBE-ERROR] Could not open '%s'\n", xbePath ? xbePath : "(null)");
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    long fileSize = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (fileSize < 0x1000) {
        std::fclose(f);
        return false;
    }

    std::vector<uint8_t> xbe(fileSize);
    std::fread(xbe.data(), 1, fileSize, f);
    std::fclose(f);

    if (std::memcmp(xbe.data(), "XBEH", 4) != 0) {
        LogPrintf("[XBE-ERROR] Invalid XBE magic in '%s'\n", xbePath);
        return false;
    }

    // Make 0x00011000 .. 0x00921000 readable, writable, and executable
    // (Leaving 0x00010000..0x00010FFF untouched so the Win32 PE header remains 100% intact!)
    DWORD oldProt = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(0x00011000), 0x00910000, PAGE_EXECUTE_READWRITE, &oldProt)) {
        LogPrintf("[XBE-ERROR] VirtualProtect(0x00011000, 0x00910000) failed (err=%lu). Are we running from the low-base launcher?\n",
                  GetLastError());
        return false;
    }

    // Store XBE header metadata in g_xbeHeaderCopy
    std::memcpy(g_xbeHeaderCopy, xbe.data(), 0x1000);

    // Zero-initialize the entire Xbox section region 0x00011000 .. 0x00921000
    std::memset(reinterpret_cast<void*>(0x00011000), 0, 0x00910000);

    uint32_t numSections = *reinterpret_cast<const uint32_t*>(xbe.data() + 0x011C);
    uint32_t secHdrVa    = *reinterpret_cast<const uint32_t*>(xbe.data() + 0x0120);
    uint32_t secHdrOff   = secHdrVa - 0x00010000;

    LogPrintf("[XBE] Mapping %u sections from '%s' (%ld bytes) into 0x00011000..0x00921000:\n",
              numSections, xbePath, fileSize);

    for (uint32_t i = 0; i < numSections; ++i) {
        const auto* sh = reinterpret_cast<const XbeSectionHeader*>(xbe.data() + secHdrOff + i * sizeof(XbeSectionHeader));
        const char* sname = (sh->pszSectionNameAddr >= 0x00010000 && (sh->pszSectionNameAddr - 0x00010000) < 0x1000)
                            ? reinterpret_cast<const char*>(xbe.data() + (sh->pszSectionNameAddr - 0x00010000))
                            : "?";
        uint32_t copyBytes = std::min(sh->dwRawSize, sh->dwVirtualSize);
        if (sh->dwRawAddr + copyBytes <= static_cast<uint32_t>(fileSize)) {
            std::memcpy(reinterpret_cast<void*>(sh->dwVirtualAddr), xbe.data() + sh->dwRawAddr, copyBytes);
        }
        LogPrintf("  [%2u] %-10s VA=0x%08X..0x%08X (VSize=0x%08X, RawSize=0x%08X)\n",
                  i, sname, sh->dwVirtualAddr, sh->dwVirtualAddr + sh->dwVirtualSize, sh->dwVirtualSize, sh->dwRawSize);
    }

    // 1. Install all 120 Xbox Kernel Thunks at 0x00616D80..0x00616F5C
    xboxkrnl::InstallKernelThunkTable(reinterpret_cast<uint32_t*>(0x00616D80));
    LogPrintf("[XBE] Populated 120 Xbox Kernel Thunks at 0x00616D80..0x00616F5C.\n");

    // 2. Initialize _XapiProcessHeap (0x00911BF4)
    *reinterpret_cast<HANDLE*>(0x00911BF4) = GetProcessHeap();

    // 3. Patch out the 3 non-.text FS segment accesses in D3D and DSOUND:
    //    0x005989BF: 64 A1 20 00 00 00 (mov eax, fs:[20h]) -> 33 C0 90 90 90 90 (xor eax, eax; nop x4)
    const uint8_t xorEaxNop6[6] = {0x33, 0xC0, 0x90, 0x90, 0x90, 0x90};
    std::memcpy(reinterpret_cast<void*>(0x005989BF), xorEaxNop6, 6);
    //    0x005A6C10 & 0x005A6C35: DirectSound::CFpState::Save / Restore -> C3 (ret)
    *reinterpret_cast<uint8_t*>(0x005A6C10) = 0xC3;
    *reinterpret_cast<uint8_t*>(0x005A6C35) = 0xC3;

    // 3b. Patch out Xbox 0x80000000 physical-to-virtual ORs in D3D resource.obj!
    //     On Xbox, D3DResource.Data stores physical RAM addresses and LockRect/Free ORs 0x80000000 to map into kernel VA.
    //     On Win32, MmGetPhysicalAddress returns the real Win32 VA directly, so ORing 0x80000000 faults at 0x8xxxxxxx!
    std::memset(reinterpret_cast<void*>(0x00594F17), 0x90, 5); // 0D 00 00 00 80 -> nop x5 (GetDataFromResource)
    std::memset(reinterpret_cast<void*>(0x00594F58), 0x90, 5); // 0D 00 00 00 80 -> nop x5 (Destroy -> MmFreeContiguousMemory)
    std::memset(reinterpret_cast<void*>(0x00594FA4), 0x90, 6); // 81 CA 00 00 00 80 -> nop x6 (Destroy -> MmFreeContiguousMemory)

    // 3c. Patch out Xbox 128MB physical address masks (`and reg, 0x07FFFFFF` -> `and reg, 0xFFFFFFFF`) in D3D section!
    //     On Win32, VirtualAlloc returns addresses >= 0x08000000; masking with 0x07FFFFFF strips bit 27+ and causes AVs!
    const uint32_t d3dMaskImmHighByteVAs[] = {
        0x0059388D, 0x00595312, 0x00596314, 0x005963E5, 0x00596809,
        0x00596851, 0x005968DC, 0x0059696A, 0x00596D1D, 0x00596D65, 0x0059A723
    };
    for (uint32_t va : d3dMaskImmHighByteVAs) {
        if (*reinterpret_cast<uint8_t*>(va) == 0x07) {
            *reinterpret_cast<uint8_t*>(va) = 0xFF;
        }
    }

    // 3d. Enable Slate Engine's built-in CPU software vertex shader path (`m_bUseSoftwareShaders = 1` at 0x0029B223)!
    //     This makes Buffy's own shader_SkinMeshOmni / shader_StaticMesh / shader_StaticOmni8 compute authentic
    //     character skinning, 8-light lighting, water, and wind into clean fixed-function FVF 0x242 vertices!
    const uint8_t forceSoftShaders[6] = {0xEB, 0xEE, 0x90, 0x90, 0x90, 0x90}; // jmp 0x0029B213; nop x4
    std::memcpy(reinterpret_cast<void*>(0x0029B223), forceSoftShaders, 6);
    InstallDetour(0x00582C0E, &Detour_mtinit);
    InstallDetour(0x00580DEE, &Detour_mtinitlocknum);
    InstallDetour(0x00582A77, &Detour_getptd);
    InstallDetour(0x00582AF9, &Detour_freeptd);

    // 5. Install Heap & Virtual Memory Detours
    InstallDetour(0x0057B058, &Detour_RtlCreateHeap);
    InstallDetour(0x0057B47C, &Detour_RtlAllocateHeap);
    InstallDetour(0x0057BBFE, &Detour_RtlFreeHeap);
    InstallDetour(0x0057BDCE, &Detour_RtlReAllocateHeap);
    InstallDetour(0x0057AADA, &Detour_RtlSizeHeap);
    InstallDetour(0x005772DB, &Detour_GlobalMemoryStatus);
    InstallDetour(0x00577271, &Detour_VirtualAlloc);
    InstallDetour(0x0057729F, &Detour_VirtualFree);

    // 6. Install Win32 / XAPI File, Thread, Sync & Error Detours
    InstallDetour(0x005775DD, &Detour_GetLastError);
    InstallDetour(0x00577605, &Detour_SetLastError);
    InstallDetour(0x00577B4B, &Detour_GetCurrentThreadId);
    InstallDetour(0x00577F57, &Detour_CreateFileA);
    InstallDetour(0x00577B76, &Detour_ReadFile);
    InstallDetour(0x00577C63, &Detour_WriteFile);
    InstallDetour(0x00577B58, &Detour_CloseHandle);
    InstallDetour(0x00577DBD, &Detour_SetFilePointer);
    InstallDetour(0x00577D39, &Detour_SetEndOfFile);
    InstallDetour(0x00577EB7, &Detour_FlushFileBuffers);
    InstallDetour(0x00577EDE, &Detour_GetFileSizeEx);
    InstallDetour(0x00577F1C, &Detour_GetFileSize);
    InstallDetour(0x005773D0, &Detour_GetFileAttributesA);
    InstallDetour(0x0057732A, &Detour_SetFileAttributesA);
    InstallDetour(0x0057741B, &Detour_DeleteFileA);
    InstallDetour(0x005783C4, &Detour_CopyFileA);
    InstallDetour(0x0057CFE9, &Detour_CreateDirectoryA);
    InstallDetour(0x0057D055, &Detour_RemoveDirectoryA);
    InstallDetour(0x0057D339, &Detour_FindFirstFileA);
    InstallDetour(0x0057D446, &Detour_FindNextFileA);
    InstallDetour(0x005783E3, &Detour_GetDiskFreeSpaceExA);
    InstallDetour(0x00577ADD, &Detour_CreateThread);
    InstallDetour(0x0057764A, &::GetOverlappedResult);
    InstallDetour(0x0057799E, &::GetExitCodeThread);
    InstallDetour(0x0057778B, &::CreateEventA);
    InstallDetour(0x005777EC, &::SetEvent);
    InstallDetour(0x0057780C, &::ResetEvent);
    InstallDetour(0x0057782A, &::WaitForSingleObjectEx);
    InstallDetour(0x005778C0, &::WaitForSingleObject);
    InstallDetour(0x005778D2, &Detour_Sleep);
    InstallDetour(0x0057786E, &Detour_SleepEx);
    InstallDetour(0x0057798B, &Detour_SwitchToThread);
    InstallDetour(0x00578AA9, &Detour_GetTickCount);
    InstallDetour(0x00587D06, &Detour_OutputDebugStringA);
    InstallDetour(0x005770DB, &::timeSetEvent);
    InstallDetour(0x00577202, &::timeKillEvent);

    // 7. Install XAPI Launch, Sections, Video, Input & SaveGame Detours
    InstallDetour(0x00579052, &Detour_XGetLaunchInfo);
    InstallDetour(0x005790FB, &Detour_XLaunchNewImageA);
    InstallDetour(0x005790B6, &xboxkrnl::Stub_Ret20);
    InstallDetour(0x00578F78, &xboxkrnl::Stub_Ret20);
    InstallDetour(0x005786DB, &Detour_XMountUtilityDrive);
    InstallDetour(0x005787E0, &Detour_XFormatUtilityDrive);
    InstallDetour(0x00579D58, &Detour_XGetVideoStandard);
    InstallDetour(0x00579D81, &Detour_XGetVideoFlags);
    InstallDetour(0x00579DCA, &Detour_XGetAudioFlags);
    InstallDetour(0x0057D5B4, &Detour_XGetSectionHandleA);
    InstallDetour(0x0057D61B, &Detour_XLoadSectionByHandle);
    InstallDetour(0x0057D63C, &Detour_XFreeSectionByHandle);
    InstallDetour(0x0057D65A, &Detour_XGetSectionSize);

    InstallDetour(0x006106B6, &Detour_XInitDevices);
    InstallDetour(0x006106BB, &Detour_XGetDevices);
    InstallDetour(0x006106DD, &Detour_XGetDeviceChanges);
    InstallDetour(0x0061058D, &Detour_XInputOpen);
    InstallDetour(0x006105E3, &Detour_XInputClose);
    InstallDetour(0x006105EF, &Detour_XInputGetState);
    InstallDetour(0x0061065E, &Detour_XInputSetState);

    InstallDetour(0x00610BFF, &Detour_XMountMUA);
    InstallDetour(0x00610D31, &Detour_XUnmountMU);
    InstallDetour(0x0057965D, &Detour_XCreateSaveGame);
    InstallDetour(0x005798C0, &Detour_XDeleteSaveGame);
    InstallDetour(0x0057998F, &Detour_XFindFirstSaveGame);
    InstallDetour(0x00579A96, &Detour_XFindNextSaveGame);
    InstallDetour(0x00579ADD, &Detour_XFindClose);
    InstallDetour(0x00576ED9, &Detour_XCalculateSignatureBegin);
    InstallDetour(0x00576DE7, &Detour_XCalculateSignatureBeginEx);
    InstallDetour(0x00576E62, &Detour_XCalculateSignatureUpdate);
    InstallDetour(0x00576E7C, &Detour_XCalculateSignatureEnd);

    // 8. Install Slate Engine Logging, Installer & Sound Detours
    // NOTE: 0x000215A0 is a 1-byte `ret` COMDAT-folded across many empty retail functions (including IMasterCamera::AddCameras),
    // so we must NOT detour 0x000215A0!
    InstallDetour(0x00063AE0, &Detour_ShellWarning);
    InstallDetour(0x00063920, &Detour_ShellSetTitle);
    InstallDetour(0x00064510, &Detour_ShellHandleBug);
    InstallDetour(0x00064550, &Detour_ShellAbort);
    InstallDetour(0x00038220, &Detour_RenderLog);
    InstallDetour(0x00038250, &Detour_RenderError);
    InstallDetour(0x000A06D0, &Detour_ScriptLog);
    InstallDetour(0x000A0700, &Detour_ScriptWarning);
    InstallDetour(0x000A0730, &Detour_ScriptError);
    InstallDetour(0x00063250, &Detour_RunInstaller);
    InstallDetour(0x002B7250, &Detour_TResourcePack_ReadIndex);
    // Live Windows waveOut 48 kHz Audio Bridge (lets real TSoundManager / CSoundBufferDS / CSoundBufferDS3D / CMusicBufferDS run!)
    dsound_bridge::InstallDetours();
    InstallDetour(0x002AA870, &Detour_TLocker_Lock);
    InstallDetour(0x002AA950, &Detour_TLocker_Unlock);
    // High-precision 1 MHz QPC clock replacement for Xbox 733 MHz RDTSC TTimerManager
    InstallDetour(0x002AC5B0, &Detour_TTimerManager_GetTimeStamp);
    InstallDetour(0x002AC510, &Detour_TTimerManager_ConvertClocksToMs);
    InstallDetour(0x002AC530, &Detour_TTimerManager_ConvertClocksToSec);
    InstallDetour(0x002AC550, &Detour_TTimerManager_ConvertDeltaClocksToMs);
    InstallDetour(0x002AC580, &Detour_TTimerManager_ConvertDeltaClocksToSec);
    InstallDetour(0x002AC740, &Detour_TTimerManager_GetClocksDeltaAndConvertMs);
    // Engine-native Hor+ widescreen / 21:9 frustum + projection hook
    InstallDetour(0x002A5110, &Detour_View_SetFOV);
    // Photo Mode / Auto-Pause TimeScale & Fun Cheats / Trainer Detours
    InstallDetour(0x0008DAC0, &Detour_QueryTimeScale);
    InstallDetour(0x00204F90, &Detour_ICharacterBtvS_Health_Change);
    InstallDetour(0x001F0110, &Detour_IAmmo_UseAmmo);
    {
        // Build 10-byte executable trampoline for IPickup::Event_TakeDamage (0x000BFC60)
        uint8_t* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (tramp) {
            std::memcpy(tramp, reinterpret_cast<const void*>(0x000BFC60), 10);
            tramp[10] = 0xE9;
            int32_t rel = static_cast<int32_t>(0x000BFC6Au - reinterpret_cast<uintptr_t>(tramp + 15));
            std::memcpy(tramp + 11, &rel, 4);
            g_origPickupTakeDamage = reinterpret_cast<FnPickupTakeDamage>(tramp);
            InstallDetour(0x000BFC60, &Detour_IPickup_TakeDamage);
        }
    }

    // 9. Install Xbox Direct3D8 & TIF_Render_Implement Detours
    InstallDetour(0x005966E0, &Detour_Direct3D_CreateDevice);
    InstallDetour(0x00596F50, &Detour_D3D_MakeRequestedSpace);
    InstallDetour(0x005970D0, &Detour_D3D_MakeSpace);
    InstallDetour(0x00596CC0, &Detour_D3D_KickOff);
    InstallDetour(0x00596DB0, &Detour_D3D_SetFence);
    InstallDetour(0x00596E60, &Detour_D3D_BlockOnTime);
    InstallDetour(0x00597130, &Detour_D3D_KickOffAndWaitForIdle);
    InstallDetour(0x00597150, &Detour_D3D_BlockOnResource);
    InstallDetour(0x005971D0, &Detour_D3D_BlockOnResource);
    InstallDetour(0x005969A0, &xboxkrnl::Stub_Ret0);
    InstallDetour(0x00594F00, &xboxkrnl::Stub_Ret4); // _D3DResource_BlockUntilNotBusy@4
    InstallDetour(0x005950D0, &xboxkrnl::Stub_Ret4); // _D3DResource_IsBusy@4 -> FALSE
    InstallDetour(0x005937E0, &xboxkrnl::Stub_Ret0); // _D3DDevice_IsBusy@0 -> FALSE
    InstallDetour(0x00592930, &xboxkrnl::Stub_Ret0); // _D3DDevice_BlockUntilVerticalBlank@0
    InstallDetour(0x00593A80, &xboxkrnl::Stub_Ret0); // _D3DDevice_PersistDisplay@0
    InstallDetour(0x005928F0, &xboxkrnl::Stub_Ret0); // _D3DDevice_Release@0
    InstallDetour(0x00592AF0, &xboxkrnl::Stub_Ret8); // _D3DDevice_SetRenderTarget@8
    InstallDetour(0x005926E0, &xboxkrnl::Stub_Ret8); // _D3DDevice_SetGammaRamp@8
    InstallDetour(0x00592960, &xboxkrnl::Stub_Ret4); // _D3DDevice_SetFlickerFilter@4
    InstallDetour(0x005929B0, &xboxkrnl::Stub_Ret4); // _D3DDevice_SetSoftDisplayFilter@4
    InstallDetour(0x00593110, &xboxkrnl::Stub_Ret20);// _D3DDevice_CopyRects@20

    InstallDetour(0x00595E70, &Detour_D3DDevice_Clear);
    InstallDetour(0x00595DB0, &Detour_D3DDevice_Swap);
    InstallDetour(0x00593D20, &Detour_D3DDevice_DrawVerticesUP);
    InstallDetour(0x00593E80, &Detour_D3DDevice_DrawIndexedVerticesUP);
    InstallDetour(0x00593FC0, &Detour_D3DDevice_DrawIndexedVertices);
    InstallDetour(0x005933E0, &Detour_D3DDevice_SetViewport);
    InstallDetour(0x005927E0, &Detour_D3DDevice_SetTransform);
    InstallDetour(0x00594C20, &Detour_D3DDevice_SetVertexShader);
    InstallDetour(0x0029CBB0, &Detour_Precache_Shader);
    InstallDetour(0x00594860, &Detour_D3DDevice_SetStreamSource);
    InstallDetour(0x00593750, &Detour_D3DDevice_SetIndices);
    InstallDetour(0x00593540, &Detour_D3DDevice_SetTexture);
    InstallDetour(0x005936C0, &Detour_D3DDevice_SetPalette);
    // Let Xbox D3D8 update _D3D__RenderState (0x0059F940) & _D3D__TextureState (0x0059F740) natively;
    // d3d9_bridge::SyncXboxStates() mirrors the live XDK 4432 state tables to D3D9 before every draw call!

    InstallDetour(0x0029D2A0, &Detour_RenderString);
    InstallDetour(0x0029D2B0, &Detour_RenderRectangle);
    InstallDetour(0x00297A90, &Detour_Movie_Play);
    InstallDetour(0x002970A0, &Detour_Movie_Playing);
    InstallDetour(0x00297020, &Detour_Movie_Kill);
    InstallDetour(0x00297060, &Detour_Movie_KillCurrent);
    InstallDetour(0x0057FAE9, &Detour_XboxExit);
    InstallDetour(0x005812A8, &Detour_XboxAmsgExit);
    InstallDetour(0x00062650, &Detour_G_ErrorToUser);

    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(0x00010000), 0x00921000);
    ULONG stackGuarantee = 32768;
    SetThreadStackGuarantee(&stackGuarantee);
    AddVectoredExceptionHandler(1, &XboxVectoredExceptionHandler);
    SetUnhandledExceptionFilter(&XboxVectoredExceptionHandler);
    LogPrintf("[XBE] Applied all Win32 detours and registered Vectored Exception Handler.\n");
    return true;
}

static DWORD WINAPI HangWatchdogThreadProc(LPVOID) {
    bool dumped = false;
    for (;;) {
        Sleep(500);
        uint32_t fc = g_frameCount;
        DWORD lastTick = g_lastSwapTick;
        if (fc >= 1 && lastTick != 0 && !dumped) {
            DWORD elapsed = GetTickCount() - lastTick;
            if (elapsed >= 3500 && g_hMainGameThread) {
                dumped = true;
                if (SuspendThread(g_hMainGameThread) != static_cast<DWORD>(-1)) {
                    CONTEXT ctx{};
                    ctx.ContextFlags = CONTEXT_FULL;
                    if (GetThreadContext(g_hMainGameThread, &ctx)) {
                        char hangPath[512]{};
                        GetModuleFileNameA(nullptr, hangPath, sizeof(hangPath));
                        if (char* sl = std::strrchr(hangPath, '\\')) {
                            *sl = '\0';
                            if (char* sl2 = std::strrchr(hangPath, '\\')) std::strcpy(sl2 + 1, "hang.log");
                            else std::strcpy(hangPath, "hang.log");
                        } else {
                            std::strcpy(hangPath, "hang.log");
                        }
                        FILE* hf = std::fopen(hangPath, "w");
                        auto hlog = [&](const char* fmt, ...) {
                            char buf[1024]{};
                            va_list ap;
                            va_start(ap, fmt);
                            std::vsnprintf(buf, sizeof(buf), fmt, ap);
                            va_end(ap);
                            if (hf) { std::fputs(buf, hf); std::fflush(hf); }
                        };
                        char eipSym[512]{};
                        FormatXboxSymbol(ctx.Eip, eipSym, sizeof(eipSym));
                        hlog("\n====================================================================\n");
                        hlog("[HANG-WATCHDOG] Main thread stalled for %lu ms after Frame %u!\n", elapsed, fc);
                        hlog("  EIP=%s\n", eipSym);
                        hlog("  EAX=0x%08lX EBX=0x%08lX ECX=0x%08lX EDX=0x%08lX\n", ctx.Eax, ctx.Ebx, ctx.Ecx, ctx.Edx);
                        hlog("  ESI=0x%08lX EDI=0x%08lX EBP=0x%08lX ESP=0x%08lX\n", ctx.Esi, ctx.Edi, ctx.Ebp, ctx.Esp);
                        hlog("  Stack Trace (Xbox + Runtime frames):\n");
                        uint32_t* sp = reinterpret_cast<uint32_t*>(ctx.Esp);
                        int printed = 0;
                        for (int i = 0; i < 128 && printed < 24; ++i) {
                            if (IsBadReadPtr(sp + i, 4)) break;
                            uint32_t val = sp[i];
                            if (val >= 0x00011000 && val < 0x00616D80) {
                                char sym[512]{};
                                FormatXboxSymbol(val, sym, sizeof(sym));
                                hlog("    [ESP+0x%03X] %s\n", i * 4, sym);
                                ++printed;
                            } else if (val >= 0x10000000 && val < 0x10400000) {
                                hlog("    [ESP+0x%03X] BuffyRecompRuntime.dll+0x%06X\n", i * 4, val - 0x10000000u);
                                ++printed;
                            }
                        }
                        hlog("====================================================================\n");
                        if (hf) std::fclose(hf);
                    }
                    ResumeThread(g_hMainGameThread);
                }
            }
        } else if (fc >= 1 && lastTick != 0 && (GetTickCount() - lastTick) < 1000) {
            dumped = false;
        }
    }
    return 0;
}

int ExecuteXboxMain() {
    // Ensure all 26 cached files from D:\install.dat (including gen_dialogue.rpe & level dialogue RPEs)
    // are unpacked into Z:\ (recomp\cache\) before _main runs (instant no-op if already present).
    RunInstaller(false);

    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                    GetCurrentProcess(), &g_hMainGameThread,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0);
    HANDLE hWd = CreateThread(nullptr, 0, &HangWatchdogThreadProc, nullptr, 0, nullptr);
    if (hWd) CloseHandle(hWd);
    LogPrintf("\n[EXEC] Step 1/3: Calling Xbox CRT __rtinit@0 (0x0057D4EF)...\n");
    using RtInitFn = int (__stdcall*)();
    int rtRes = reinterpret_cast<RtInitFn>(0x0057D4EF)();
    LogPrintf("[EXEC] __rtinit@0 returned %d.\n", rtRes);

    LogPrintf("[EXEC] Step 2/3: Calling Xbox CRT __cinit@0 (0x0057D497) -> Running 470 C++ global constructors...\n");
    using CInitFn = void (__stdcall*)();
    reinterpret_cast<CInitFn>(0x0057D497)();
    LogPrintf("[EXEC] __cinit@0 completed! All 470 C++ static objects constructed.\n");

    LogPrintf("[EXEC] Step 3/3: Jumping into Xbox _main (0x00064F90)...\n\n");
    using MainFn = int (__cdecl*)();
    int mainRes = reinterpret_cast<MainFn>(0x00064F90)();
    LogPrintf("\n[EXEC] Xbox _main (0x00064F90) returned %d.\n", mainRes);
    return mainRes;
}

void RequestQuickSave(int slot) {
    g_reqSaveSlot = (slot >= 1 && slot <= 5) ? slot : g_activeSaveSlot;
    InterlockedExchange(&g_quickSaveReq, 1);
}

void RequestQuickLoad(int slot) {
    g_reqLoadSlot = (slot >= 1 && slot <= 5) ? slot : g_activeSaveSlot;
    InterlockedExchange(&g_quickLoadReq, 1);
}

void SetActiveQuickSaveSlot(int slot) {
    g_activeSaveSlot = (slot >= 1 && slot <= 5) ? slot : 1;
}

int GetActiveQuickSaveSlot() {
    return g_activeSaveSlot;
}

bool HasQuickSaveInSlot(int slot) {
    QuickSaveState qs{};
    return ReadQuickSaveState(qs, slot);
}

void RequestJumpToLevel(const char* mapName) {
    if (!mapName || !mapName[0]) return;
    std::strncpy(g_jumpMapName, mapName, sizeof(g_jumpMapName) - 1);
    g_jumpMapName[sizeof(g_jumpMapName) - 1] = '\0';
    InterlockedExchange(&g_jumpLevelReq, 1);
}

void SetAutoResumeQuickSave(bool enabled) {
    g_autoResumeQuick = enabled;
}

void SetSubtitlesEnabled(bool enabled) {
    g_subtitlesEnabled = enabled;
}

bool IsSubtitlesEnabled() {
    return g_subtitlesEnabled;
}

void SetBuffyCostume(uint32_t costumeId) {
    g_buffyCostume = (costumeId <= 8) ? costumeId : 0;
}

uint32_t GetBuffyCostume() {
    return g_buffyCostume;
}

void RequestCostumeReloadInPlace() {
    InterlockedExchange(&g_costumeReloadReq, 1);
}

void SetTargetFps(uint32_t fps) {
    g_targetFps = fps;
    if (g_qpcInited) {
        QueryPerformanceCounter(&g_lastSwapQpc);
    }
    LogPrintf("[Timer] Target Frame Rate set to %s (exact 1.0x QPC game speed)\n",
              fps ? (std::to_string(fps) + " FPS").c_str() : "Uncapped");
}

uint32_t GetTargetFps() {
    return g_targetFps;
}

uint32_t GetMeasuredFps() {
    return g_measuredFps;
}

void SetFovOffsetDeg(int offsetDeg) {
    int clamped = std::max(-15, std::min(25, offsetDeg));
    if (clamped != g_fovOffsetDeg) {
        g_fovOffsetDeg = clamped;
        g_fovRefreshPending = true;
    }
}

int GetFovOffsetDeg() {
    return g_fovOffsetDeg;
}

void SetRumbleEnabled(bool enabled) {
    g_rumbleEnabled = enabled;
    if (!enabled && g_winXInputSetState) {
        WIN_XINPUT_VIBRATION zero{0, 0};
        g_winXInputSetState(g_activeWinPadIndex, &zero);
    }
}

bool IsRumbleEnabled() { return g_rumbleEnabled; }

void SetDeadzonePercent(int percent) {
    g_deadzonePercent = std::max(0, std::min(40, percent));
}

int GetDeadzonePercent() { return g_deadzonePercent; }

void SetCameraControlSettings(bool mouseLook, int sensitivityPercent, bool invertX, bool invertY) {
    g_mouseLookEnabled = mouseLook;
    g_mouseSensitivity = std::max(25, std::min(250, sensitivityPercent));
    g_invertCameraX    = invertX;
    g_invertCameraY    = invertY;
    if (!mouseLook) {
        InterlockedExchange(&g_mouseCaptured, 0);
    }
}

void SetMouseLookCaptured(bool captured) {
    InterlockedExchange(&g_mouseCaptured, captured ? 1 : 0);
    if (!captured) {
        InterlockedExchange(&g_mouseDeltaX, 0);
        InterlockedExchange(&g_mouseDeltaY, 0);
    }
}

void AddMouseLookDelta(int dx, int dy) {
    if (!g_mouseLookEnabled || !g_mouseCaptured) return;
    InterlockedExchangeAdd(&g_mouseDeltaX, static_cast<LONG>(dx));
    InterlockedExchangeAdd(&g_mouseDeltaY, static_cast<LONG>(dy));
}

void SetKeybinds(const int vkCodes[10]) {
    if (!vkCodes) return;
    for (int i = 0; i < 10; ++i) {
        if (vkCodes[i] > 0) g_keybinds[i] = vkCodes[i];
    }
}

bool IsGamepadConnected() { return g_winPadConnected; }

uint32_t GetActivePadSlot() { return static_cast<uint32_t>(g_activeWinPadIndex); }

const char* GetQuickStatusText() {
    if (g_quickStatusExpireTick != 0 && static_cast<int32_t>(g_quickStatusExpireTick - GetTickCount()) > 0) {
        return g_quickStatusBuf;
    }
    return "";
}

void SetAutoPauseUnfocused(bool enabled) {
    g_autoPauseUnfocused = enabled;
}

void SetSkipIntroMovies(bool enabled) {
    g_skipIntroMovies = enabled;
}

void SetCheats(bool infHealth, bool infPower, bool infAmmoDurability, bool oneHitDust) {
    g_cheatInfHealth  = infHealth;
    g_cheatInfPower   = infPower;
    g_cheatInfAmmo    = infAmmoDurability;
    g_cheatOneHitDust = oneHitDust;
    SyncSubtitlesFlagToEngine();
}

void ShowStatusNotice(const char* msg) {
    if (!msg || !msg[0]) return;
    std::snprintf(g_quickStatusBuf, sizeof(g_quickStatusBuf), "%s", msg);
    g_quickStatusExpireTick = GetTickCount() + 3000;
}

} // namespace xbe_loader
