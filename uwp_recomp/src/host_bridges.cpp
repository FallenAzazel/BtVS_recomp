#include "host_bridges.h"
#include "d3d11_bridge.h"
#include "uwp_config_ui.h"
#include "xaudio2_bridge.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <unordered_map>

namespace uwp_recomp {
extern void sub_00063250(X86Context& ctx);
extern uint32_t GuestHeapSize(uint32_t va);
extern bool IsGuestHeapAddr(uint32_t va);
extern uint32_t GuestHeapFreeBytes();
namespace host_bridges {

static std::string g_dRoot = "extracted";
static std::string g_zRoot = "recomp\\cache";
static std::string g_tRoot = "recomp\\saves";

static std::mutex g_vfsMtx;
static uint64_t g_cntReadFile = 0, g_cntFread = 0, g_cntVsnprintf = 0, g_cntLock = 0, g_cntSleep = 0;
static uint64_t g_cntSlpMain = 0, g_cntSlpDisk = 0;
static uint32_t g_lastSlpCaller = 0, g_lastSlpMs = 0;
static std::unordered_map<uint32_t, HANDLE> g_fileHandles;
static std::unordered_map<uint32_t, HANDLE> g_findHandles;
static std::unordered_map<uint32_t, FILE*>       g_cFiles;
static std::unordered_map<uint32_t, std::string> g_cFilePaths;
static uint32_t g_nextHandle = 0x1000u;
static uint32_t g_nextFind   = 0x3000u;
static uint32_t g_nextCFile  = 0x2000u;

// Portable 32-bit guest printf formatter (reads 32-bit guest varargs / va_list from flat 64 MB RAM)
static int FormatGuestPrintf(const X86Context& c, char* out, size_t cap, const char* fmt, uint32_t argVA) {
    if (!out || cap == 0) return 0;
    if (!fmt) { out[0] = '\0'; return 0; }
    size_t pos = 0;
    auto emit = [&](const char* s, size_t n) {
        for (size_t i = 0; i < n && pos + 1 < cap; ++i) out[pos++] = s[i];
    };
    const char* p = fmt;
    while (*p && pos + 1 < cap) {
        if (*p != '%') { out[pos++] = *p++; continue; }
        const char* specStart = p++;
        if (*p == '%') { out[pos++] = '%'; ++p; continue; }
        while (*p && (std::strchr("-+ #0", *p) != nullptr)) ++p;
        while (*p >= '0' && *p <= '9') ++p;
        if (*p == '.') { ++p; while (*p >= '0' && *p <= '9') ++p; }
        while (*p == 'l' || *p == 'h') ++p;
        char conv = *p ? *p++ : '\0';
        char spec[32]{};
        size_t slen = std::min<size_t>(sizeof(spec) - 1, static_cast<size_t>(p - specStart));
        std::memcpy(spec, specStart, slen);
        char tmp[256]{};
        int n = 0;
        if (conv == 's') {
            uint32_t sVA = c.r32(argVA); argVA += 4u;
            const char* s = sVA ? reinterpret_cast<const char*>(c.ptr(sVA)) : "(null)";
            n = std::snprintf(tmp, sizeof(tmp), spec, s);
        } else if (conv == 'f' || conv == 'g' || conv == 'e' || conv == 'F' || conv == 'G' || conv == 'E') {
            double dv = c.rf64(argVA); argVA += 8u;
            n = std::snprintf(tmp, sizeof(tmp), spec, dv);
        } else if (conv == 'c') {
            int cv = static_cast<int>(c.r32(argVA) & 0xFFu); argVA += 4u;
            n = std::snprintf(tmp, sizeof(tmp), spec, cv);
        } else if (conv == 'd' || conv == 'i') {
            int iv = static_cast<int>(c.r32(argVA)); argVA += 4u;
            n = std::snprintf(tmp, sizeof(tmp), spec, iv);
        } else if (conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o' || conv == 'p') {
            uint32_t uv = c.r32(argVA); argVA += 4u;
            if (conv == 'p') n = std::snprintf(tmp, sizeof(tmp), "0x%08X", uv);
            else             n = std::snprintf(tmp, sizeof(tmp), spec, uv);
        } else {
            emit(spec, slen);
            continue;
        }
        if (n > 0) emit(tmp, std::min<size_t>(static_cast<size_t>(n), sizeof(tmp) - 1));
    }
    out[pos] = '\0';
    return static_cast<int>(pos);
}

static std::vector<uint32_t> g_scriptPressFrames;
static std::string           g_overrideMap;
static uint8_t               g_inProcessLaunchData[3072] = {};
static bool                  g_hasInProcessLaunchData    = false;

static std::atomic<uint32_t> g_relaunchEpoch{0};
static thread_local bool     tl_isWorkerThread = false;
static thread_local uint32_t tl_workerEpoch    = 0;
static std::mutex            g_workerMtx;
static std::vector<HANDLE>   g_workerHandles;
static std::vector<uint32_t> g_freeStacks;

// Cached guest-heap VAs cleared cleanly on in-process relaunch
static uint32_t g_fakeD3D8     = 0;
static uint32_t g_fakeDS       = 0;
static uint32_t g_fakeBufVtbl  = 0;
static uint32_t g_wmaVtbl      = 0;
static uint32_t g_streamVtbl   = 0;
static uint32_t g_ptdVA        = 0;
static uint32_t g_pushBufVA    = 0;
static uint32_t g_fakeBbSurfVA = 0;

static void CheckWorkerAbort() {
    if (tl_isWorkerThread && g_relaunchEpoch.load(std::memory_order_acquire) != tl_workerEpoch) {
        throw GuestRelaunchException{};
    }
}

void ResetForRelaunch() {
    // 1. Stop and join all guest worker threads
    std::vector<HANDLE> handles;
    {
        std::lock_guard<std::mutex> lk(g_workerMtx);
        handles.swap(g_workerHandles);
        g_freeStacks.clear();
    }
    if (!handles.empty()) {
        WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), TRUE, 500u);
        for (HANDLE h : handles) CloseHandle(h);
    }
    // 2. Close any open guest file / find handles
    {
        std::lock_guard<std::mutex> lk(g_vfsMtx);
        for (auto& kv : g_fileHandles) {
            if (kv.second && kv.second != INVALID_HANDLE_VALUE) CloseHandle(kv.second);
        }
        g_fileHandles.clear();
        for (auto& kv : g_findHandles) {
            if (kv.second && kv.second != INVALID_HANDLE_VALUE) FindClose(kv.second);
        }
        g_findHandles.clear();
        for (auto& kv : g_cFiles) {
            if (kv.second) std::fclose(kv.second);
        }
        g_cFiles.clear();
        g_cFilePaths.clear();
    }
    // 3. Clear cached guest-heap pointers so they re-allocate in fresh guest RAM
    g_fakeD3D8     = 0;
    g_fakeDS       = 0;
    g_fakeBufVtbl  = 0;
    g_wmaVtbl      = 0;
    g_streamVtbl   = 0;
    g_ptdVA        = 0;
    g_pushBufVA    = 0;
    g_fakeBbSurfVA = 0;
}

void SetScriptedPressFrames(const uint32_t* frames, uint32_t count) {
    g_scriptPressFrames.assign(frames, frames + count);
}
void SetOverrideMap(const char* mapName) {
    g_overrideMap = mapName ? mapName : "";
}

void SetDriveRoots(const char* dRoot, const char* zRoot, const char* tRoot) {
    if (dRoot) g_dRoot = dRoot;
    if (zRoot) g_zRoot = zRoot;
    if (tRoot) g_tRoot = tRoot;
}

static std::string TranslateGuestPath(const char* xboxPath, bool forWrite = false) {
    if (!xboxPath || !xboxPath[0]) return "";
    std::string s(xboxPath);
    for (char& c : s) {
        if (c == '/') c = '\\';
    }
    if (s.size() >= 2 && s[1] == ':') {
        char drv = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
        std::string rest = (s.size() > 2 && s[2] == '\\') ? s.substr(3) : s.substr(2);
        if (drv == 'D') {
            std::string dp = g_dRoot + "\\" + rest;
            if (forWrite || GetFileAttributesA(dp.c_str()) != INVALID_FILE_ATTRIBUTES) return dp;
            std::string zp = g_zRoot + "\\" + rest;
            if (GetFileAttributesA(zp.c_str()) != INVALID_FILE_ATTRIBUTES) return zp;
            return dp;
        }
        if (drv == 'Z') {
            std::string zp = g_zRoot + "\\" + rest;
            if (forWrite || GetFileAttributesA(zp.c_str()) != INVALID_FILE_ATTRIBUTES) return zp;
            std::string dp = g_dRoot + "\\" + rest;
            if (GetFileAttributesA(dp.c_str()) != INVALID_FILE_ATTRIBUTES) return dp;
            return zp;
        }
        if (drv == 'T' || drv == 'U') return g_tRoot + "\\" + rest;
    }
    if (!forWrite) {
        std::string dp = g_dRoot + "\\" + s;
        if (GetFileAttributesA(dp.c_str()) != INVALID_FILE_ATTRIBUTES) return dp;
    }
    return g_zRoot + "\\" + s;
}

static inline uint32_t Arg(const X86Context& c, int index1Based) {
    return c.r32(c.esp + static_cast<uint32_t>(index1Based * 4));
}

// __cdecl helper: caller cleans args, callee only pops return address
static inline void RetCdecl(X86Context& c) { c.esp += 4u; }
// __stdcall helper: callee pops return address + argBytes
static inline void RetStdcall(X86Context& c, uint32_t argBytes) { c.esp += 4u + argBytes; }

static std::mutex g_lockerMapMtx;
static std::unordered_map<uint32_t, std::unique_ptr<std::recursive_mutex>> g_lockers;

static std::recursive_mutex& GetGuestLocker(uint32_t va) {
    thread_local uint32_t tlsVa[4] = {};
    thread_local std::recursive_mutex* tlsMtx[4] = {};
    for (int i = 0; i < 4; ++i) {
        if (tlsVa[i] == va && tlsMtx[i]) return *tlsMtx[i];
    }
    std::recursive_mutex* m = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_lockerMapMtx);
        auto& ptr = g_lockers[va];
        if (!ptr) ptr = std::make_unique<std::recursive_mutex>();
        m = ptr.get();
    }
    thread_local int tlsNext = 0;
    tlsVa[tlsNext]  = va;
    tlsMtx[tlsNext] = m;
    tlsNext = (tlsNext + 1) & 3;
    return *m;
}

void RegisterAll() {
    // ------------------------------------------------------------------------
    // 1. Heap & Memory Allocation (Xbox CRT malloc/calloc/realloc/free + RtlAllocateHeap)
    // ------------------------------------------------------------------------
    RegisterGuestFunc(0x0057F72Au, [](X86Context& c) { // _malloc(size)
        uint32_t sz = Arg(c, 1);
        c.eax = GuestHeapAlloc(sz ? sz : 4u, 16u);
        RetCdecl(c);
    });
    RegisterGuestFunc(0x00580A9Fu, [](X86Context& c) { // _calloc(n, sz)
        uint32_t total = Arg(c, 1) * Arg(c, 2);
        uint32_t va = GuestHeapAlloc(total ? total : 4u, 16u);
        if (va && total) std::memset(c.ptr(va), 0, total);
        c.eax = va;
        RetCdecl(c);
    });
    RegisterGuestFunc(0x00580C4Du, [](X86Context& c) { // _realloc(ptr, sz)
        uint32_t oldVA = Arg(c, 1), sz = Arg(c, 2);
        if (sz == 0u) {
            if (oldVA) GuestHeapFree(oldVA);
            c.eax = 0u;
            RetCdecl(c);
            return;
        }
        uint32_t newVA = GuestHeapAlloc(sz, 16u);
        if (newVA && oldVA) {
            uint32_t oldSz = GuestHeapSize(oldVA);
            uint32_t copySz = oldSz ? std::min<uint32_t>(oldSz, sz) : sz;
            if (copySz) std::memcpy(c.ptr(newVA), c.ptr(oldVA), copySz);
            GuestHeapFree(oldVA);
        }
        c.eax = newVA;
        RetCdecl(c);
    });
    RegisterGuestFunc(0x0057F566u, [](X86Context& c) { // _free(ptr)
        GuestHeapFree(Arg(c, 1));
        RetCdecl(c);
    });
    RegisterGuestFunc(0x00580200u, [](X86Context& c) { // _memmove(dst, src, n)
        uint32_t d = Arg(c, 1), s = Arg(c, 2), n = Arg(c, 3);
        if (d && s && n) std::memmove(c.ptr(d), c.ptr(s), n);
        c.eax = d;
        RetCdecl(c);
    });
    RegisterGuestFunc(0x0057B47Cu, [](X86Context& c) { // RtlAllocateHeap(heap, flags, size)
        uint32_t flags = Arg(c, 2);
        uint32_t size  = Arg(c, 3);
        uint32_t va    = GuestHeapAlloc(size ? size : 4u, 16u);
        if (va && (flags & 0x08u)) std::memset(c.ptr(va), 0, size);
        c.eax = va;
        RetStdcall(c, 12);
    });
    RegisterGuestFunc(0x0057BBFEu, [](X86Context& c) { // RtlFreeHeap(heap, flags, ptr)
        GuestHeapFree(Arg(c, 3));
        c.eax = 1u;
        RetStdcall(c, 12);
    });
    RegisterGuestFunc(0x00577271u, [](X86Context& c) { // VirtualAlloc(addr, size, type, prot)
        uint32_t size = Arg(c, 2);
        uint32_t va   = GuestHeapAlloc(size ? size : 4096u, 4096u);
        if (va) std::memset(c.ptr(va), 0, size);
        c.eax = va;
        RetStdcall(c, 16);
    });
    RegisterGuestFunc(0x0057729Fu, [](X86Context& c) { // VirtualFree(addr, size, freeType)
        uint32_t addr = Arg(c, 1), freeType = Arg(c, 3);
        if (addr && (freeType & 0x8000u)) {
            d3d11_bridge::InvalidateTexture(addr);
            GuestHeapFree(addr);
        }
        c.eax = 1u;
        RetStdcall(c, 12);
    });

    // ------------------------------------------------------------------------
    // 2. LIBCMT String, Printf, Stdio, Math & Logging Helpers
    // ------------------------------------------------------------------------
    // Lock-free ASCII _stricmp / _strnicmp (avoids UCRT locale lock on 25M+ Level 1 name lookups!)
    auto fnStricmp = [](X86Context& c) {
        const unsigned char* a = c.ptr(Arg(c, 1));
        const unsigned char* b = c.ptr(Arg(c, 2));
        int diff = 0;
        for (;;) {
            unsigned char ca = *a++, cb = *b++;
            if (ca != cb) {
                if (ca >= 'A' && ca <= 'Z') ca = static_cast<unsigned char>(ca + 32);
                if (cb >= 'A' && cb <= 'Z') cb = static_cast<unsigned char>(cb + 32);
                if (ca != cb) { diff = static_cast<int>(ca) - static_cast<int>(cb); break; }
            }
            if (ca == 0) break;
        }
        c.eax = static_cast<uint32_t>(diff);
        RetCdecl(c);
    };
    RegisterGuestFunc(0x002AABA0u, fnStricmp); // SlateEngine _stricmp
    RegisterGuestFunc(0x005808CAu, fnStricmp); // LIBCMT __stricmp
    RegisterGuestFunc(0x00587D3Eu, [](X86Context& c) { // __strnicmp
        const unsigned char* a = c.ptr(Arg(c, 1));
        const unsigned char* b = c.ptr(Arg(c, 2));
        uint32_t n = Arg(c, 3);
        int diff = 0;
        while (n--) {
            unsigned char ca = *a++, cb = *b++;
            if (ca != cb) {
                if (ca >= 'A' && ca <= 'Z') ca = static_cast<unsigned char>(ca + 32);
                if (cb >= 'A' && cb <= 'Z') cb = static_cast<unsigned char>(cb + 32);
                if (ca != cb) { diff = static_cast<int>(ca) - static_cast<int>(cb); break; }
            }
            if (ca == 0) break;
        }
        c.eax = static_cast<uint32_t>(diff);
        RetCdecl(c);
    });
    RegisterGuestFunc(0x0057F290u, [](X86Context& c) { // _strncmp
        c.eax = static_cast<uint32_t>(std::strncmp(reinterpret_cast<const char*>(c.ptr(Arg(c, 1))),
                                                   reinterpret_cast<const char*>(c.ptr(Arg(c, 2))), Arg(c, 3)));
        RetCdecl(c);
    });
    RegisterGuestFunc(0x0057EFC0u, [](X86Context& c) { // _strncpy
        uint32_t d = Arg(c, 1);
        std::strncpy(reinterpret_cast<char*>(c.ptr(d)), reinterpret_cast<const char*>(c.ptr(Arg(c, 2))), Arg(c, 3));
        c.eax = d;
        RetCdecl(c);
    });
    RegisterGuestFunc(0x0057E360u, [](X86Context& c) { // _strchr
        uint32_t sVA = Arg(c, 1);
        const char* base = reinterpret_cast<const char*>(c.ptr(sVA));
        const char* hit = std::strchr(base, static_cast<int>(Arg(c, 2) & 0xFFu));
        c.eax = hit ? (sVA + static_cast<uint32_t>(hit - base)) : 0u;
        RetCdecl(c);
    });
    RegisterGuestFunc(0x0057FA00u, [](X86Context& c) { // _strrchr
        uint32_t sVA = Arg(c, 1);
        const char* base = reinterpret_cast<const char*>(c.ptr(sVA));
        const char* hit = std::strrchr(base, static_cast<int>(Arg(c, 2) & 0xFFu));
        c.eax = hit ? (sVA + static_cast<uint32_t>(hit - base)) : 0u;
        RetCdecl(c);
    });
    RegisterGuestFunc(0x0057F4E0u, [](X86Context& c) { // _strstr
        uint32_t sVA = Arg(c, 1);
        const char* base = reinterpret_cast<const char*>(c.ptr(sVA));
        const char* hit = std::strstr(base, reinterpret_cast<const char*>(c.ptr(Arg(c, 2))));
        c.eax = hit ? (sVA + static_cast<uint32_t>(hit - base)) : 0u;
        RetCdecl(c);
    });
    RegisterGuestFunc(0x0057E41Eu, [](X86Context& c) { // _sprintf(dst, fmt, ...)
        char* dst = reinterpret_cast<char*>(c.ptr(Arg(c, 1)));
        const char* fmt = reinterpret_cast<const char*>(c.ptr(Arg(c, 2)));
        c.eax = static_cast<uint32_t>(FormatGuestPrintf(c, dst, 4096, fmt, c.esp + 12u));
        RetCdecl(c);
    });
    RegisterGuestFunc(0x0058053Du, [](X86Context& c) { // __snprintf(dst, n, fmt, ...)
        char* dst = reinterpret_cast<char*>(c.ptr(Arg(c, 1)));
        uint32_t n = Arg(c, 2);
        const char* fmt = reinterpret_cast<const char*>(c.ptr(Arg(c, 3)));
        c.eax = static_cast<uint32_t>(FormatGuestPrintf(c, dst, n, fmt, c.esp + 16u));
        RetCdecl(c);
    });
    RegisterGuestFunc(0x0057FB0Du, [](X86Context& c) { // __vsnprintf(dst, n, fmt, va_list)
        ++g_cntVsnprintf;
        char* dst = reinterpret_cast<char*>(c.ptr(Arg(c, 1)));
        uint32_t n = Arg(c, 2);
        const char* fmt = reinterpret_cast<const char*>(c.ptr(Arg(c, 3)));
        uint32_t vaList = Arg(c, 4);
        c.eax = static_cast<uint32_t>(FormatGuestPrintf(c, dst, n, fmt, vaList));
        RetCdecl(c);
    });
    RegisterGuestFunc(0x0057DC14u, [](X86Context& c) { c.eax = static_cast<uint32_t>(std::atoi(reinterpret_cast<const char*>(c.ptr(Arg(c, 1))))); RetCdecl(c); });
    RegisterGuestFunc(0x0057E2F0u, [](X86Context& c) { c.fpuPush(std::atof(reinterpret_cast<const char*>(c.ptr(Arg(c, 1))))); RetCdecl(c); });
    RegisterGuestFunc(0x0057EA62u, [](X86Context& c) { std::srand(Arg(c, 1)); RetCdecl(c); });
    RegisterGuestFunc(0x0057EA6Fu, [](X86Context& c) { c.eax = static_cast<uint32_t>(std::rand()); RetCdecl(c); });
    RegisterGuestFunc(0x0057F866u, [](X86Context& c) { c.fpuPush(std::floor(c.rf64(c.esp + 4u))); RetCdecl(c); });
    RegisterGuestFunc(0x0057EA94u, [](X86Context& c) { c.fpuPush(std::pow(c.rf64(c.esp + 4u), c.rf64(c.esp + 12u))); RetCdecl(c); });
    // MSVC x87 __CI* math intrinsics (operate directly on ST(0)/ST(1))
    RegisterGuestFunc(0x0057EA98u, [](X86Context& c) { double e = c.fpuPop(); double b = c.fpuPop(); c.fpuPush(std::pow(b, e)); RetCdecl(c); });  // __CIpow
    RegisterGuestFunc(0x0057EA9Au, [](X86Context& c) { double e = c.fpuPop(); double b = c.fpuPop(); c.fpuPush(std::pow(b, e)); RetCdecl(c); });  // __CIpow_default
    RegisterGuestFunc(0x0057F778u, [](X86Context& c) { double x = c.fpuPop(); c.fpuPush(std::acos(std::max(-1.0, std::min(1.0, x)))); RetCdecl(c); }); // __CIacos
    RegisterGuestFunc(0x0057F934u, [](X86Context& c) { double x = c.fpuPop(); c.fpuPush(std::asin(std::max(-1.0, std::min(1.0, x)))); RetCdecl(c); }); // __CIasin
    RegisterGuestFunc(0x0057F84Eu, [](X86Context& c) { double y = c.fpuPop(); double x = c.fpuPop(); c.fpuPush(y != 0.0 ? std::fmod(x, y) : 0.0); RetCdecl(c); }); // __CIfmod
    RegisterGuestFunc(0x005800C0u, [](X86Context& c) { double x = c.fpuPop(); c.fpuPush(x > 0.0 ? std::sqrt(x) : 0.0); RetCdecl(c); });           // __CIsqrt
    RegisterGuestFunc(0x0057E476u, [](X86Context& c) { c.eax = std::isspace(static_cast<unsigned char>(Arg(c, 1) & 0xFFu)) ? 1u : 0u; RetCdecl(c); }); // _isspace
    RegisterGuestFunc(0x0057E837u, [](X86Context& c) { // ___RTDynamicCast(inptr, vfdelta, srcType, targetType, isRef)
        uint32_t inptr      = Arg(c, 1);
        uint32_t srcType    = Arg(c, 3);
        uint32_t targetType = Arg(c, 4);
        uint32_t result     = 0u;
        if (inptr && targetType) {
            if (srcType == targetType) {
                result = inptr;
            } else {
                uint32_t vtbl = c.r32(inptr);
                if (vtbl >= 0x00010004u && vtbl < kXboxRamSize) {
                    uint32_t col = c.r32(vtbl - 4u);
                    if (col >= 0x00010000u && col + 20u < kXboxRamSize) {
                        uint32_t offset      = c.r32(col + 4u);
                        uint32_t completeObj = inptr - offset;
                        uint32_t chd         = c.r32(col + 16u);
                        if (chd >= 0x00010000u && chd + 16u < kXboxRamSize) {
                            uint32_t numBases  = c.r32(chd + 8u);
                            uint32_t baseArr   = c.r32(chd + 12u);
                            const char* tgtName = reinterpret_cast<const char*>(c.ptr(targetType + 8u));
                            if (numBases <= 128u && baseArr >= 0x00010000u && baseArr + numBases * 4u < kXboxRamSize) {
                                for (uint32_t i = 0; i < numBases; ++i) {
                                    uint32_t bcd = c.r32(baseArr + i * 4u);
                                    if (bcd < 0x00010000u || bcd + 24u >= kXboxRamSize) continue;
                                    uint32_t bcdType = c.r32(bcd + 0u);
                                    if (bcdType == targetType ||
                                        (bcdType >= 0x00010000u && bcdType + 8u < kXboxRamSize &&
                                         std::strcmp(reinterpret_cast<const char*>(c.ptr(bcdType + 8u)), tgtName) == 0)) {
                                        int32_t mdisp = static_cast<int32_t>(c.r32(bcd + 8u));
                                        result = static_cast<uint32_t>(static_cast<int32_t>(completeObj) + mdisp);
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        c.eax = result;
        RetCdecl(c);
    });
    RegisterGuestFunc(0x0058005Du, [](X86Context& c) { // eh vector constructor iterator(ptr, size, count, ctor, dtor)
        uint32_t ptr   = Arg(c, 1);
        uint32_t sz    = Arg(c, 2);
        int32_t  count = static_cast<int32_t>(Arg(c, 3));
        uint32_t ctor  = Arg(c, 4);
        RetStdcall(c, 20);
        if (ctor && ptr && count > 0) {
            for (int32_t i = 0; i < count; ++i) {
                c.ecx = ptr + static_cast<uint32_t>(i) * sz;
                c.push32(kReturnSentinelVA);
                CallGuestVA(c, ctor);
            }
        }
    });
    RegisterGuestFunc(0x005A29C0u, [](X86Context& c) { // _D3DXMatrixPerspectiveOffCenterLH@28(pOut, l, r, b, t, zn, zf)
        uint32_t pOut = Arg(c, 1);
        float l  = c.rf32(c.esp + 8u);
        float r  = c.rf32(c.esp + 12u);
        float b  = c.rf32(c.esp + 16u);
        float t  = c.rf32(c.esp + 20u);
        float zn = c.rf32(c.esp + 24u);
        float zf = c.rf32(c.esp + 28u);
        static uint32_t s_perspLogs = 0;
        if (++s_perspLogs <= 8u) {
            std::printf("[UWP-PERSP] #%u l=%.3f r=%.3f b=%.3f t=%.3f zn=%.3f zf=%.3f\n",
                        s_perspLogs, l, r, b, t, zn, zf);
        }
        if (pOut) {
            float m[16] = {};
            float rl = (r - l), tb = (t - b), zfn = (zf - zn);
            if (rl != 0.0f && tb != 0.0f && zfn != 0.0f) {
                m[0]  = (2.0f * zn) / rl;
                m[5]  = (2.0f * zn) / tb;
                m[8]  = (l + r) / (l - r);
                m[9]  = (t + b) / (b - t);
                m[10] = zf / zfn;
                m[11] = 1.0f;
                m[14] = (zn * zf) / (zn - zf);
            }
            std::memcpy(c.ptr(pOut), m, 64);
        }
        c.eax = pOut;
        RetStdcall(c, 28);
    });
    RegisterGuestFunc(0x0057DCA4u, [](X86Context& c) { // __ftol2: ST(0) -> int64 in EDX:EAX, pops ST(0)
        int64_t v = static_cast<int64_t>(c.fpuPop());
        c.eax = static_cast<uint32_t>(v & 0xFFFFFFFFLL);
        c.edx = static_cast<uint32_t>(static_cast<uint64_t>(v) >> 32);
        RetCdecl(c);
    });
    RegisterGuestFunc(0x0057ED89u, [](X86Context& c) { c.eax = 0u; RetCdecl(c); });        // _atexit
    RegisterGuestFunc(0x0057ED51u, [](X86Context& c) { c.eax = Arg(c, 1); RetCdecl(c); }); // __onexit
    RegisterGuestFunc(0x0057DD19u, [](X86Context& c) { std::printf("[UWP-CRT] __purecall!\n"); RetCdecl(c); });
    RegisterGuestFunc(0x0057FFE0u, [](X86Context& c) { // __EH_prolog (exact MSVC SEH stack frame setup!)
        uint32_t retAddr = c.r32(c.esp);
        uint32_t handler = c.eax;
        c.w32(c.esp, c.ebp);
        c.ebp = c.esp;
        c.push32(0xFFFFFFFFu); // try level = -1
        c.push32(handler);     // SEH handler VA
        c.push32(0u);          // prev fs:[0]
        c.eax = retAddr;
        // Return cleanly to caller without popping the 3 SEH frame dwords!
    });
    // SDK static constructor no-ops (d3d8, dsound, xapilib)
    RegisterGuestFunc(0x00593C60u, [](X86Context& c) { RetCdecl(c); }); // d3d8::_$E1
    RegisterGuestFunc(0x005A8FE3u, [](X86Context& c) { RetCdecl(c); }); // dsound::_$E1
    RegisterGuestFunc(0x005A8FEEu, [](X86Context& c) { RetCdecl(c); }); // dsound::_$E3
    RegisterGuestFunc(0x005AE054u, [](X86Context& c) { RetCdecl(c); }); // dsound::heap _$E1
    RegisterGuestFunc(0x005AE05Fu, [](X86Context& c) { RetCdecl(c); }); // dsound::heap _$E3
    RegisterGuestFunc(0x00610B65u, [](X86Context& c) { RetCdecl(c); }); // xapilib::mu _$E1
    RegisterGuestFunc(0x00596400u, [](X86Context& c) { // Direct3DCreate8(sdkVer) -> non-null fake IDirect3D8*
        if (!g_fakeD3D8) g_fakeD3D8 = GuestHeapAlloc(256, 16);
        c.eax = g_fakeD3D8;
        RetStdcall(c, 4);
    });

    // LIBCMT stdio FILE* (fopen / fclose / fread / fwrite / fseek / ftell)
    RegisterGuestFunc(0x0057DF7Eu, [](X86Context& c) { // _fopen
        const char* path = reinterpret_cast<const char*>(c.ptr(Arg(c, 1)));
        const char* mode = reinterpret_cast<const char*>(c.ptr(Arg(c, 2)));
        bool forWrite = mode && (std::strchr(mode, 'w') || std::strchr(mode, 'a') || std::strchr(mode, '+'));
        std::string host = TranslateGuestPath(path, forWrite);
        if (forWrite) {
            for (size_t i = 0; i < host.size(); ++i) {
                if (host[i] == '\\') CreateDirectoryA(host.substr(0, i).c_str(), nullptr);
            }
        }
        FILE* f = std::fopen(host.c_str(), mode ? mode : "rb");
        if (!f) {
            std::printf("[UWP-VFS] fopen FAILED: '%s' -> '%s' (mode=%s)\n",
                        path ? path : "(null)", host.c_str(), mode ? mode : "rb");
            c.eax = 0u;
        } else {
            SetLastError(0);
            std::lock_guard<std::mutex> lk(g_vfsMtx);
            uint32_t id = g_nextCFile++;
            g_cFiles[id] = f;
            g_cFilePaths[id] = host;
            c.eax = id;
        }
        RetCdecl(c);
    });
    RegisterGuestFunc(0x0057DD6Eu, [](X86Context& c) { // _fclose
        FILE* fp = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_vfsMtx);
            auto it = g_cFiles.find(Arg(c, 1));
            if (it != g_cFiles.end()) { fp = it->second; g_cFiles.erase(it); }
            g_cFilePaths.erase(Arg(c, 1));
        }
        c.eax = static_cast<uint32_t>(fp ? std::fclose(fp) : -1);
        RetCdecl(c);
    });
    RegisterGuestFunc(0x0057EF6Bu, [](X86Context& c) { // _fread
        ++g_cntFread;
        FILE* fp = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_vfsMtx);
            auto it = g_cFiles.find(Arg(c, 4));
            if (it != g_cFiles.end()) fp = it->second;
        }
        size_t got = fp ? std::fread(c.ptr(Arg(c, 1)), Arg(c, 2), Arg(c, 3), fp) : 0;
        c.eax = static_cast<uint32_t>(got);
        RetCdecl(c);
    });
    RegisterGuestFunc(0x0057DED6u, [](X86Context& c) { // _fwrite
        FILE* fp = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_vfsMtx);
            auto it = g_cFiles.find(Arg(c, 4));
            if (it != g_cFiles.end()) fp = it->second;
        }
        size_t put = fp ? std::fwrite(c.ptr(Arg(c, 1)), Arg(c, 2), Arg(c, 3), fp) : 0;
        c.eax = static_cast<uint32_t>(put);
        RetCdecl(c);
    });
    RegisterGuestFunc(0x0057EE2Au, [](X86Context& c) { // _fseek
        FILE* fp = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_vfsMtx);
            auto it = g_cFiles.find(Arg(c, 1));
            if (it != g_cFiles.end()) fp = it->second;
        }
        int r = fp ? std::fseek(fp, static_cast<long>(Arg(c, 2)), static_cast<int>(Arg(c, 3))) : -1;
        c.eax = static_cast<uint32_t>(r);
        RetCdecl(c);
    });
    RegisterGuestFunc(0x0057F246u, [](X86Context& c) { // _ftell
        FILE* fp = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_vfsMtx);
            auto it = g_cFiles.find(Arg(c, 1));
            if (it != g_cFiles.end()) fp = it->second;
        }
        long r = fp ? std::ftell(fp) : -1L;
        c.eax = static_cast<uint32_t>(r);
        RetCdecl(c);
    });
    RegisterGuestFunc(0x0057DF91u, [](X86Context& c) { // _fprintf(fp, fmt, ...)
        auto it = g_cFiles.find(Arg(c, 1));
        const char* fmt = reinterpret_cast<const char*>(c.ptr(Arg(c, 2)));
        char buf[1024]{};
        int n = FormatGuestPrintf(c, buf, sizeof(buf), fmt, c.esp + 12u);
        if (it != g_cFiles.end() && n > 0) std::fwrite(buf, 1, static_cast<size_t>(n), it->second);
        c.eax = static_cast<uint32_t>(n);
        RetCdecl(c);
    });
    RegisterGuestFunc(0x0057E913u, [](X86Context& c) { // __splitpath(path, drv, dir, fname, ext)
        const char* path = reinterpret_cast<const char*>(c.ptr(Arg(c, 1)));
        char drv[16]{}, dir[256]{}, fn[256]{}, ext[64]{};
        _splitpath(path ? path : "", drv, dir, fn, ext);
        if (uint32_t p = Arg(c, 2)) std::strcpy(reinterpret_cast<char*>(c.ptr(p)), drv);
        if (uint32_t p = Arg(c, 3)) std::strcpy(reinterpret_cast<char*>(c.ptr(p)), dir);
        if (uint32_t p = Arg(c, 4)) std::strcpy(reinterpret_cast<char*>(c.ptr(p)), fn);
        if (uint32_t p = Arg(c, 5)) std::strcpy(reinterpret_cast<char*>(c.ptr(p)), ext);
        RetCdecl(c);
    });
    RegisterGuestFunc(0x005783C4u, [](X86Context& c) { // _CopyFileA@12
        const char* rawSrc = reinterpret_cast<const char*>(c.ptr(Arg(c, 1)));
        const char* rawDst = reinterpret_cast<const char*>(c.ptr(Arg(c, 2)));
        std::string src = TranslateGuestPath(rawSrc, false);
        std::string dst = TranslateGuestPath(rawDst, true);
        BOOL ok = TRUE;
        if (GetFileAttributesA(dst.c_str()) == INVALID_FILE_ATTRIBUTES) {
            // Ensure parent directory exists
            for (size_t i = 0; i < dst.size(); ++i) {
                if (dst[i] == '\\') CreateDirectoryA(dst.substr(0, i).c_str(), nullptr);
            }
            ok = CopyFileA(src.c_str(), dst.c_str(), Arg(c, 3) ? TRUE : FALSE);
            if (!ok) {
                std::printf("[UWP-VFS] CopyFileA FAILED: '%s' -> '%s' (err=%lu)\n",
                            src.c_str(), dst.c_str(), static_cast<unsigned long>(GetLastError()));
            }
        }
        c.eax = ok ? 1u : 0u;
        RetStdcall(c, 12);
    });
    RegisterGuestFunc(0x00577ADDu, [](X86Context& c) { // _CreateThread@24
        uint32_t startVA = Arg(c, 3);
        uint32_t param   = Arg(c, 4);
        uint32_t tidOut  = Arg(c, 6);
        static uint32_t s_nextTid = 100u;
        uint32_t tid = s_nextTid++;
        if (tidOut) c.w32(tidOut, tid);
        // Reuse a small pool of 256 KB guest stacks so repeated audio-stream retries never exhaust guest RAM!
        uint32_t stackBase = 0;
        {
            std::lock_guard<std::mutex> lk(g_workerMtx);
            if (!g_freeStacks.empty()) {
                stackBase = g_freeStacks.back();
                g_freeStacks.pop_back();
            }
        }
        if (!stackBase) stackBase = GuestHeapAlloc(256u * 1024u, 16u);
        uint32_t epoch = g_relaunchEpoch.load(std::memory_order_acquire);
        struct ThreadArgs { uint32_t fnVA; uint32_t param; uint32_t stackBase; uint32_t epoch; };
        auto* ta = new ThreadArgs{ startVA, param, stackBase, epoch };
        if (tid < 105u) {
            std::printf("[UWP-THR] CreateThread startVA=0x%08X param=0x%08X tid=%u\n", startVA, param, tid);
        }
        HANDLE h = CreateThread(nullptr, 2u * 1024u * 1024u, [](LPVOID lp) -> DWORD {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            auto* a = static_cast<ThreadArgs*>(lp);
            tl_isWorkerThread = true;
            tl_workerEpoch    = a->epoch;
            X86Context tc{};
            tc.mem = GetGuestRamBase();
            tc.esp = a->stackBase ? (a->stackBase + 256u * 1024u - 16u) : (kGuestStackTop - 0x00200000u);
            tc.push32(a->param);
            tc.push32(kReturnSentinelVA);
            uint32_t fn = a->fnVA;
            uint32_t sb = a->stackBase;
            uint32_t ep = a->epoch;
            delete a;
            try {
                CallGuestVA(tc, fn);
            } catch (const GuestRelaunchException&) {
                CoUninitialize();
                return 0;
            }
            if (sb && g_relaunchEpoch.load(std::memory_order_acquire) == ep) {
                std::lock_guard<std::mutex> lk(g_workerMtx);
                g_freeStacks.push_back(sb);
            }
            CoUninitialize();
            return tc.eax;
        }, ta, 0, nullptr);
        uint32_t gh = 0xDEAD0000u | tid;
        if (h) {
            // Expose a duplicated thread HANDLE in g_fileHandles so WaitForSingleObject(hThread, INFINITE)
            // genuinely joins audio/music worker threads before the main thread frees CMusicBufferDS!
            HANDLE hGuest = nullptr;
            if (DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &hGuest, 0, FALSE, DUPLICATE_SAME_ACCESS) && hGuest) {
                std::lock_guard<std::mutex> lk(g_vfsMtx);
                g_fileHandles[gh] = hGuest;
            }
            std::lock_guard<std::mutex> lk(g_workerMtx);
            // Prune already-exited worker handles before pushing
            g_workerHandles.erase(
                std::remove_if(g_workerHandles.begin(), g_workerHandles.end(), [](HANDLE wh) {
                    if (WaitForSingleObject(wh, 0) == WAIT_OBJECT_0) { CloseHandle(wh); return true; }
                    return false;
                }),
                g_workerHandles.end());
            g_workerHandles.push_back(h);
        }
        c.eax = gh;
        RetStdcall(c, 24);
    });
    RegisterGuestFunc(0x005770DBu, [](X86Context& c) { c.eax = 1u; RetStdcall(c, 20); }); // _timeSetEvent@20
    RegisterGuestFunc(0x00577202u, [](X86Context& c) { c.eax = 0u; RetStdcall(c, 4); });  // _timeKillEvent@4

    // DirectSound8 initialization bridges (routing to XAudio2 in Phase 4+)
    RegisterGuestFunc(0x005A8D07u, [](X86Context& c) { // _DirectSoundCreate@12
        uint32_t ppOut = Arg(c, 2);
        if (ppOut) {
            if (!g_fakeDS) g_fakeDS = GuestHeapAlloc(256, 16);
            c.w32(ppOut, g_fakeDS);
        }
        c.eax = 0u; // DS_OK
        RetStdcall(c, 12);
    });
    RegisterGuestFunc(0x005A6D44u, [](X86Context& c) { RetStdcall(c, 0); });              // _DirectSoundUseFullHRTF@0
    RegisterGuestFunc(0x005A7738u, [](X86Context& c) { c.eax = 0u; RetStdcall(c, 20); }); // _IDirectSound_DownloadEffectsImage@20
    RegisterGuestFunc(0x005A7E50u, [](X86Context& c) { c.eax = 0u; RetStdcall(c, 8); });  // _IDirectSound_EnableHeadphones@8
    RegisterGuestFunc(0x005A7835u, [](X86Context& c) { RetStdcall(c, 0); });              // _DirectSoundDoWork@0
    RegisterGuestFunc(0x005A871Bu, [](X86Context& c) { c.eax = 0u; RetStdcall(c, 12); }); // _IDirectSound_SetDopplerFactor@12
    RegisterGuestFunc(0x005A87BEu, [](X86Context& c) { // _IDirectSound_SetRolloffFactor@12
        xaudio2_bridge::SetRolloffFactor(c.rf32(c.esp + 8u));
        c.eax = 0u;
        RetStdcall(c, 12);
    });
    RegisterGuestFunc(0x005A86F7u, [](X86Context& c) { // _IDirectSound_SetDistanceFactor@12
        xaudio2_bridge::SetDistanceFactor(c.rf32(c.esp + 8u));
        c.eax = 0u;
        RetStdcall(c, 12);
    });
    RegisterGuestFunc(0x005A8789u, [](X86Context& c) { // _IDirectSound_SetPosition@20
        xaudio2_bridge::SetListenerPosition(c.rf32(c.esp + 8u), c.rf32(c.esp + 12u), c.rf32(c.esp + 16u));
        c.eax = 0u;
        RetStdcall(c, 20);
    });
    RegisterGuestFunc(0x005A873Fu, [](X86Context& c) { // _IDirectSound_SetOrientation@32
        xaudio2_bridge::SetListenerOrientation(
            c.rf32(c.esp + 8u),  c.rf32(c.esp + 12u), c.rf32(c.esp + 16u),
            c.rf32(c.esp + 20u), c.rf32(c.esp + 24u), c.rf32(c.esp + 28u));
        c.eax = 0u;
        RetStdcall(c, 32);
    });
    RegisterGuestFunc(0x005A87E2u, [](X86Context& c) { c.eax = 0u; RetStdcall(c, 20); }); // _IDirectSound_SetVelocity@20
    RegisterGuestFunc(0x005A8817u, [](X86Context& c) { c.eax = 0u; RetStdcall(c, 12); }); // _IDirectSound_SetI3DL2Listener@12
    RegisterGuestFunc(0x005A7E6Cu, [](X86Context& c) { // _IDirectSound_CommitDeferredSettings@4
        xaudio2_bridge::Commit3DSettings();
        c.eax = 0u;
        RetStdcall(c, 4);
    });
    RegisterGuestFunc(0x005A8B4Du, [](X86Context& c) { // _IDirectSound_CreateSoundBuffer@16(pDS, pDesc, ppBuf, pUnk)
        if (!g_fakeBufVtbl) {
            g_fakeBufVtbl = GuestHeapAlloc(128u, 16u);
            for (uint32_t i = 0; i < 32u; ++i) c.w32(g_fakeBufVtbl + i * 4u, 0x005A77D7u); // safe stdcall-4 no-op
        }
        uint32_t pDesc = Arg(c, 2), ppBuf = Arg(c, 3);
        uint32_t bufVA = GuestHeapAlloc(32u, 16u);
        if (bufVA) {
            c.w32(bufVA, g_fakeBufVtbl);
            uint16_t fmtTag = 0x0069u, chans = 1u, bits = 16u;
            uint32_t rate = 22050u;
            bool is3D = false;
            if (pDesc) {
                is3D = (c.r32(pDesc + 4u) & 0x10u) != 0u; // XBOX_DSBCAPS_CTRL3D
                uint32_t pWfx = c.r32(pDesc + 12u); // XBOX_DSBUFFERDESC::lpwfxFormat
                if (pWfx) {
                    fmtTag = c.r16(pWfx + 0u);
                    chans  = c.r16(pWfx + 2u);
                    rate   = c.r32(pWfx + 4u);
                    bits   = c.r16(pWfx + 14u);
                }
            }
            xaudio2_bridge::CreateBuffer(bufVA, fmtTag, chans, rate, bits, is3D);
        }
        if (ppBuf) c.w32(ppBuf, bufVA);
        c.eax = 0u; // DS_OK
        RetStdcall(c, 16);
    });
    RegisterGuestFunc(0x005A6D2Eu, [](X86Context& c) { // _IDirectSoundBuffer_Release@4
        uint32_t pBuf = Arg(c, 1);
        xaudio2_bridge::ReleaseBuffer(pBuf);
        if (IsGuestHeapAddr(pBuf)) GuestHeapFree(pBuf);
        c.eax = 0u;
        RetStdcall(c, 4);
    });
    RegisterGuestFunc(0x005A777Bu, [](X86Context& c) { // _IDirectSoundBuffer_SetVolume@8
        xaudio2_bridge::SetVolume(Arg(c, 1), static_cast<int32_t>(Arg(c, 2)));
        c.eax = 0u;
        RetStdcall(c, 8);
    });
    RegisterGuestFunc(0x005A7797u, [](X86Context& c) { c.eax = 0u; RetStdcall(c, 8); });  // _IDirectSoundBuffer_SetHeadroom@8
    RegisterGuestFunc(0x005A77B3u, [](X86Context& c) { // _IDirectSoundBuffer_Play@16(pBuf, rsv1, rsv2, flags)
        xaudio2_bridge::Play(Arg(c, 1), (Arg(c, 4) & 1u) != 0u);
        c.eax = 0u;
        RetStdcall(c, 16);
    });
    RegisterGuestFunc(0x005A77D7u, [](X86Context& c) { // _IDirectSoundBuffer_Stop@4
        xaudio2_bridge::Stop(Arg(c, 1));
        c.eax = 0u;
        RetStdcall(c, 4);
    });
    RegisterGuestFunc(0x005A77EFu, [](X86Context& c) { // _IDirectSoundBuffer_GetStatus@8
        uint32_t pBuf = Arg(c, 1), pSt = Arg(c, 2);
        uint32_t st = xaudio2_bridge::GetStatus(pBuf);
        if (pSt) c.w32(pSt, st);
        c.eax = 0u;
        RetStdcall(c, 8);
    });
    RegisterGuestFunc(0x005A780Bu, [](X86Context& c) { // _IDirectSoundBuffer_GetCurrentPosition@12
        uint32_t pBuf = Arg(c, 1), pPlay = Arg(c, 2), pWrite = Arg(c, 3);
        uint32_t pos = xaudio2_bridge::GetPlayCursorBytes(pBuf);
        if (pPlay)  c.w32(pPlay, pos);
        if (pWrite) c.w32(pWrite, pos);
        c.eax = 0u;
        RetStdcall(c, 12);
    });
    RegisterGuestFunc(0x005A7E84u, [](X86Context& c) { // _IDirectSoundBuffer_SetFrequency@8
        xaudio2_bridge::SetFrequency(Arg(c, 1), Arg(c, 2));
        c.eax = 0u;
        RetStdcall(c, 8);
    });
    RegisterGuestFunc(0x005A7EA0u, [](X86Context& c) { // _IDirectSoundBuffer_SetAllParameters@12(pBuf, pParams, dwApply)
        uint32_t pBuf = Arg(c, 1), pParams = Arg(c, 2);
        if (pBuf && pParams) {
            xaudio2_bridge::SetBuffer3DParams(
                pBuf,
                c.rf32(pParams + 0x04u), c.rf32(pParams + 0x08u), c.rf32(pParams + 0x0Cu),
                c.rf32(pParams + 0x34u), c.rf32(pParams + 0x38u), c.r32(pParams + 0x3Cu));
        }
        c.eax = 0u;
        RetStdcall(c, 12);
    });
    RegisterGuestFunc(0x005A7EC0u, [](X86Context& c) { c.eax = 0u; RetStdcall(c, 12); }); // _IDirectSoundBuffer_SetI3DL2Source@12
    RegisterGuestFunc(0x005A8837u, [](X86Context& c) { // _IDirectSoundBuffer_SetBufferData@12(pBuf, pvData, dwBytes)
        uint32_t pBuf = Arg(c, 1), pvData = Arg(c, 2), dwBytes = Arg(c, 3);
        if (pBuf && pvData && dwBytes > 0) {
            xaudio2_bridge::SetBufferData(pBuf, c.ptr(pvData), dwBytes);
        }
        c.eax = 0u;
        RetStdcall(c, 12);
    });
    // Synthetic COM vtables for WMA In-Memory Decoder XMO & IDirectSoundStream
    RegisterGuestFunc(0xFFFF1000u, [](X86Context& c) { c.eax = 1u; RetStdcall(c, 4); }); // WMA::AddRef
    RegisterGuestFunc(0xFFFF1004u, [](X86Context& c) { // WMA::Release
        uint32_t p = Arg(c, 1);
        xaudio2_bridge::ReleaseWmaDecoder(p);
        if (IsGuestHeapAddr(p)) GuestHeapFree(p);
        c.eax = 0u;
        RetStdcall(c, 4);
    });
    RegisterGuestFunc(0xFFFF1008u, [](X86Context& c) { // WMA::GetInfo
        uint32_t pInfo = Arg(c, 2);
        if (pInfo) std::memset(c.ptr(pInfo), 0, 16);
        c.eax = 0u;
        RetStdcall(c, 8);
    });
    RegisterGuestFunc(0xFFFF100Cu, [](X86Context& c) { // WMA::GetStatus
        uint32_t pSt = Arg(c, 2);
        if (pSt) c.w32(pSt, 2u); // XMO_STATUSF_ACCEPT_OUTPUT_DATA
        c.eax = 0u;
        RetStdcall(c, 8);
    });
    RegisterGuestFunc(0xFFFF1010u, [](X86Context& c) { // WMA::Process(this, pIn, pOut)
        uint32_t pThis = Arg(c, 1), pOut = Arg(c, 3);
        if (pThis && pOut) {
            uint32_t pvBuf  = c.r32(pOut + 0u);
            uint32_t maxSz  = c.r32(pOut + 4u);
            uint32_t pComp  = c.r32(pOut + 8u);
            uint32_t pStat  = c.r32(pOut + 12u);
            uint32_t got    = (pvBuf && maxSz) ? xaudio2_bridge::ReadWmaPcm(pThis, c.ptr(pvBuf), maxSz) : 0u;
            if (pComp) c.w32(pComp, got);
            if (pStat) c.w32(pStat, 0u);
        }
        c.eax = 0u; // S_OK
        RetStdcall(c, 12);
    });
    RegisterGuestFunc(0xFFFF1014u, [](X86Context& c) { c.eax = 0u; RetStdcall(c, 4); }); // WMA::Discontinuity
    RegisterGuestFunc(0xFFFF1018u, [](X86Context& c) { // WMA::Flush (rewind to track start for seamless looping)
        xaudio2_bridge::FlushWmaDecoder(Arg(c, 1));
        c.eax = 0u;
        RetStdcall(c, 4);
    });

    RegisterGuestFunc(0xFFFF1100u, [](X86Context& c) { c.eax = 1u; RetStdcall(c, 4); }); // Stream::AddRef
    RegisterGuestFunc(0xFFFF1104u, [](X86Context& c) { // Stream::Release
        uint32_t p = Arg(c, 1);
        xaudio2_bridge::ReleaseStream(p);
        if (IsGuestHeapAddr(p)) GuestHeapFree(p);
        c.eax = 0u;
        RetStdcall(c, 4);
    });
    RegisterGuestFunc(0xFFFF1108u, [](X86Context& c) { // Stream::GetInfo
        uint32_t pInfo = Arg(c, 2);
        if (pInfo) std::memset(c.ptr(pInfo), 0, 16);
        c.eax = 0u;
        RetStdcall(c, 8);
    });
    RegisterGuestFunc(0xFFFF110Cu, [](X86Context& c) { // Stream::GetStatus
        uint32_t pThis = Arg(c, 1), pSt = Arg(c, 2);
        if (pSt) c.w32(pSt, (xaudio2_bridge::GetStreamQueuedCount(pThis) < 8u) ? 1u : 0u);
        c.eax = 0u;
        RetStdcall(c, 8);
    });
    RegisterGuestFunc(0xFFFF1110u, [](X86Context& c) { // Stream::Process(this, pIn, pOut)
        uint32_t pThis = Arg(c, 1), pIn = Arg(c, 2);
        if (!pThis || !pIn) { c.eax = 0x80070057u; RetStdcall(c, 12); return; }
        uint32_t pvBuf  = c.r32(pIn + 0u);
        uint32_t maxSz  = c.r32(pIn + 4u);
        uint32_t pComp  = c.r32(pIn + 8u);
        uint32_t pStat  = c.r32(pIn + 12u);
        uint32_t hEvId  = c.r32(pIn + 16u);
        HANDLE hEv = nullptr;
        if (hEvId) {
            std::lock_guard<std::mutex> lk(g_vfsMtx);
            auto it = g_fileHandles.find(hEvId);
            if (it != g_fileHandles.end()) hEv = it->second;
        }
        bool ok = (pvBuf && maxSz) ? xaudio2_bridge::SubmitStreamPacket(
                      pThis, c.ptr(pvBuf), maxSz, c.mem, pStat, pComp, hEv) : false;
        c.eax = ok ? 0u : 0x80004005u;
        RetStdcall(c, 12);
    });
    RegisterGuestFunc(0xFFFF1114u, [](X86Context& c) { c.eax = 0u; RetStdcall(c, 4); }); // Stream::Discontinuity
    RegisterGuestFunc(0xFFFF1118u, [](X86Context& c) { // Stream::Flush
        xaudio2_bridge::FlushStream(Arg(c, 1));
        c.eax = 0u;
        RetStdcall(c, 4);
    });

    RegisterGuestFunc(0x005C407Bu, [](X86Context& c) { // _WmaCreateInMemoryDecoder@20(cb, ctx, yield, pWfxOut, ppObj)
        uint32_t pvCtx   = Arg(c, 2);
        uint32_t pWfxOut = Arg(c, 4);
        uint32_t ppObj   = Arg(c, 5);
        std::string hostPath;
        if (pvCtx) {
            uint32_t fileId = c.r32(pvCtx + 0x7Cu); // CMusicBufferDS::m_pFile (from _fopen)
            std::lock_guard<std::mutex> lk(g_vfsMtx);
            auto it = g_cFilePaths.find(fileId);
            if (it != g_cFilePaths.end()) hostPath = it->second;
        }
        if (!g_wmaVtbl) {
            g_wmaVtbl = GuestHeapAlloc(32u, 16u);
            for (uint32_t i = 0; i < 7u; ++i) c.w32(g_wmaVtbl + i * 4u, 0xFFFF1000u + i * 4u);
        }
        uint32_t wmaVA = GuestHeapAlloc(16u, 16u);
        uint16_t ch = 2, bps = 16;
        uint32_t sr = 44100;
        if (wmaVA && !hostPath.empty() && xaudio2_bridge::CreateWmaDecoder(wmaVA, hostPath.c_str(), ch, sr, bps)) {
            c.w32(wmaVA, g_wmaVtbl);
            if (pWfxOut) {
                uint16_t align = static_cast<uint16_t>(ch * (bps / 8u));
                c.w16(pWfxOut + 0u,  1u);            // WAVE_FORMAT_PCM
                c.w16(pWfxOut + 2u,  ch);
                c.w32(pWfxOut + 4u,  sr);
                c.w32(pWfxOut + 8u,  sr * align);
                c.w16(pWfxOut + 12u, align);
                c.w16(pWfxOut + 14u, bps);
                c.w16(pWfxOut + 16u, 0u);
            }
            if (ppObj) c.w32(ppObj, wmaVA);
            c.eax = 0u; // S_OK
        } else {
            if (wmaVA) GuestHeapFree(wmaVA);
            if (ppObj) c.w32(ppObj, 0u);
            c.eax = 0x80004005u; // E_FAIL
        }
        RetStdcall(c, 20);
    });
    RegisterGuestFunc(0x005A8D4Eu, [](X86Context& c) { // _DirectSoundCreateStream@8(pDesc, ppStm)
        uint32_t pDesc = Arg(c, 1), ppStm = Arg(c, 2);
        uint16_t ch = 2;
        uint32_t sr = 44100;
        if (pDesc) {
            uint32_t pWfx = c.r32(pDesc + 8u); // XBOX_DSSTREAMDESC::lpwfxFormat
            if (pWfx) {
                ch = c.r16(pWfx + 2u);
                sr = c.r32(pWfx + 4u);
            }
        }
        if (!g_streamVtbl) {
            g_streamVtbl = GuestHeapAlloc(32u, 16u);
            for (uint32_t i = 0; i < 7u; ++i) c.w32(g_streamVtbl + i * 4u, 0xFFFF1100u + i * 4u);
        }
        uint32_t stmVA = GuestHeapAlloc(16u, 16u);
        if (stmVA && xaudio2_bridge::CreateStream(stmVA, ch, sr)) {
            c.w32(stmVA, g_streamVtbl);
            if (ppStm) c.w32(ppStm, stmVA);
            c.eax = 0u; // DS_OK
        } else {
            if (stmVA) GuestHeapFree(stmVA);
            if (ppStm) c.w32(ppStm, 0u);
            c.eax = 0x80004005u;
        }
        RetStdcall(c, 8);
    });
    RegisterGuestFunc(0x005A6D13u, [](X86Context& c) { c.eax = 0u; RetStdcall(c, 12); }); // _IDirectSound_SetCooperativeLevel@12
    RegisterGuestFunc(0x005A782Bu, [](X86Context& c) { // _IDirectSoundStream_SetVolume@8
        xaudio2_bridge::SetStreamVolume(Arg(c, 1), static_cast<int32_t>(Arg(c, 2)));
        c.eax = 0u;
        RetStdcall(c, 8);
    });
    RegisterGuestFunc(0x005A7830u, [](X86Context& c) { // _IDirectSoundStream_Pause@8
        xaudio2_bridge::PauseStream(Arg(c, 1), Arg(c, 2) != 0u);
        c.eax = 0u;
        RetStdcall(c, 8);
    });
    RegisterGuestFunc(0x0057741Bu, [](X86Context& c) { // _DeleteFileA@4
        const char* p = reinterpret_cast<const char*>(c.ptr(Arg(c, 1)));
        std::string host = TranslateGuestPath(p, true);
        // Protect Z:\ cache archives (e.g. dialogue\*.rpe) from Xbox 750 MB Z-drive LRU eviction when streaming WMA music!
        if (host.rfind(g_zRoot, 0) == 0 &&
            (host.find(".rpe") != std::string::npos || host.find(".RPE") != std::string::npos ||
             host.find(".pak") != std::string::npos || host.find(".wmv") != std::string::npos)) {
            c.eax = 1u;
        } else {
            c.eax = DeleteFileA(host.c_str()) ? 1u : 0u;
        }
        RetStdcall(c, 4);
    });
    RegisterGuestFunc(0x0057998Fu, [](X86Context& c) { c.eax = 0xFFFFFFFFu; RetStdcall(c, 8); }); // _XFindFirstSaveGame@8
    RegisterGuestFunc(0x00579A96u, [](X86Context& c) { c.eax = 0u; RetStdcall(c, 8); });          // _XFindNextSaveGame@8
    RegisterGuestFunc(0x00579ADDu, [](X86Context& c) { c.eax = 1u; RetStdcall(c, 4); });          // _XFindClose@4
    RegisterGuestFunc(0x00579C7Bu, [](X86Context& c) { // _MultiByteToWideChar@24
        UINT cp = Arg(c, 1); DWORD fl = Arg(c, 2);
        uint32_t pMb = Arg(c, 3); int cbMb = static_cast<int>(Arg(c, 4));
        uint32_t pWc = Arg(c, 5); int cchWc = static_cast<int>(Arg(c, 6));
        int res = MultiByteToWideChar(cp, fl, pMb ? reinterpret_cast<LPCSTR>(c.ptr(pMb)) : nullptr, cbMb,
                                      pWc ? reinterpret_cast<LPWSTR>(c.ptr(pWc)) : nullptr, cchWc);
        c.eax = static_cast<uint32_t>(res);
        RetStdcall(c, 24);
    });
    RegisterGuestFunc(0x0057965Du, [](X86Context& c) { // _XCreateSaveGame@24(root, wName, disp, flags, pOutPath, outSize)
        uint32_t pOut = Arg(c, 5), outSz = Arg(c, 6);
        CreateDirectoryA(g_tRoot.c_str(), nullptr);
        std::string saveDir = g_tRoot + "\\Save0";
        CreateDirectoryA(saveDir.c_str(), nullptr);
        if (pOut && outSz > 0) {
            std::strncpy(reinterpret_cast<char*>(c.ptr(pOut)), "U:\\Save0\\", outSz - 1);
            c.w8(pOut + outSz - 1, 0);
        }
        c.eax = 0u; // ERROR_SUCCESS
        RetStdcall(c, 24);
    });
    RegisterGuestFunc(0x005798C0u, [](X86Context& c) { c.eax = 0u; RetStdcall(c, 8); }); // _XDeleteSaveGame@8
    RegisterGuestFunc(0x0057E060u, [](X86Context& c) { // _qsort(base, num, width, cmpVA)
        uint32_t base  = Arg(c, 1);
        uint32_t num   = Arg(c, 2);
        uint32_t width = Arg(c, 3);
        uint32_t cmpVA = Arg(c, 4);
        if (base && num > 1u && width > 0u && width <= 4096u && cmpVA) {
            std::vector<uint32_t> order(num);
            for (uint32_t i = 0; i < num; ++i) order[i] = i;
            std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
                uint32_t savedEsp = c.esp;
                c.push32(base + b * width);
                c.push32(base + a * width);
                c.push32(kReturnSentinelVA);
                CallGuestVA(c, cmpVA);
                int32_t res = static_cast<int32_t>(c.eax);
                c.esp = savedEsp;
                return res < 0;
            });
            std::vector<uint8_t> tmp(static_cast<size_t>(num) * width);
            const uint8_t* src = c.ptr(base);
            for (uint32_t i = 0; i < num; ++i) {
                std::memcpy(tmp.data() + static_cast<size_t>(i) * width, src + static_cast<size_t>(order[i]) * width, width);
            }
            std::memcpy(c.ptr(base), tmp.data(), tmp.size());
        }
        RetCdecl(c);
    });

    // Direct3D8 adapter enumeration, texture & state bridges
    RegisterGuestFunc(0x00596410u, [](X86Context& c) { c.eax = 2u; RetStdcall(c, 4); });  // _Direct3D_GetAdapterModeCount@4
    RegisterGuestFunc(0x005964B0u, [](X86Context& c) { // _Direct3D_EnumAdapterModes@12(adapter, mode, pMode)
        uint32_t mode  = Arg(c, 2);
        uint32_t pMode = Arg(c, 3);
        if (pMode) {
            c.w32(pMode + 0,  640u);                           // Width
            c.w32(pMode + 4,  480u);                           // Height
            c.w32(pMode + 8,  60u);                            // RefreshRate
            c.w32(pMode + 12, 0u);                             // Flags (Xbox D3DDISPLAYMODE has Flags at +0x0C!)
            c.w32(pMode + 16, (mode == 0) ? 0x1Eu : 0x12u);    // Format: D3DFMT_LIN_X8R8G8B8 / D3DFMT_LIN_A8R8G8B8
        }
        c.eax = 0u;
        RetStdcall(c, 12);
    });
    RegisterGuestFunc(0x00596610u, [](X86Context& c) { // _Direct3D_GetDeviceCaps@12
        uint32_t pCaps = Arg(c, 3);
        if (pCaps) std::memset(c.ptr(pCaps), 0, 224);
        c.eax = 0u; // D3D_OK
        RetStdcall(c, 12);
    });
    RegisterGuestFunc(0x00596640u, [](X86Context& c) { c.eax = 0u; RetStdcall(c, 24); }); // _Direct3D_CheckDeviceFormat@24
    RegisterGuestFunc(0x005966C0u, [](X86Context& c) { RetStdcall(c, 8); });              // _Direct3D_SetPushBufferSize@8
    RegisterGuestFunc(0x005926C0u, [](X86Context& c) { // _D3DDevice_GetDeviceCaps@4
        uint32_t pCaps = Arg(c, 1);
        if (pCaps) std::memset(c.ptr(pCaps), 0, 224);
        c.eax = 0u;
        RetStdcall(c, 4);
    });
    RegisterGuestFunc(0x00591430u, [](X86Context& c) { RetStdcall(c, 0); }); // @D3DDevice_SetRenderState_Simple@8 (__fastcall)
    RegisterGuestFunc(0x00595080u, [](X86Context& c) { // _D3DResource_Release@4 (refcounted + reclaim pixel/VB/IB buffer!)
        uint32_t pRes = Arg(c, 1);
        uint32_t newRef = 0u;
        if (IsGuestHeapAddr(pRes)) {
            uint32_t common = c.r32(pRes + 0u);
            uint32_t ref    = common & 0xFFFFu;
            if (ref > 1u) {
                newRef = ref - 1u;
                c.w32(pRes + 0u, (common & 0xFFFF0000u) | newRef);
            } else if (ref == 1u) {
                c.w32(pRes + 0u, common & 0xFFFF0000u);
                uint32_t dataVA = c.r32(pRes + 4u);
                c.w32(pRes + 4u, 0u);
                if (IsGuestHeapAddr(dataVA)) {
                    d3d11_bridge::InvalidateTexture(dataVA);
                    GuestHeapFree(dataVA);
                }
                GuestHeapFree(pRes);
            }
        }
        c.eax = newRef;
        RetStdcall(c, 4);
    });
    RegisterGuestFunc(0x00596260u, [](X86Context& c) { // _D3DDevice_CreateIndexBuffer@20(len, usage, fmt, pool, ppIB)
        uint32_t len = Arg(c, 1), ppIB = Arg(c, 5);
        uint32_t hdrVA = GuestHeapAlloc(16u, 16u);
        uint32_t bufVA = GuestHeapAlloc(std::max<uint32_t>(len, 16u), 64u);
        if (hdrVA && bufVA) {
            std::memset(c.ptr(hdrVA), 0, 16);
            c.w32(hdrVA + 0, 0x00010001u); // D3DCOMMON_TYPE_INDEXBUFFER
            c.w32(hdrVA + 4, bufVA);       // Data pointer
        }
        if (ppIB) c.w32(ppIB, hdrVA);
        c.eax = 0u;
        RetStdcall(c, 20);
    });
    RegisterGuestFunc(0x005962D0u, [](X86Context& c) { // _D3DDevice_CreateVertexBuffer@20(len, usage, fvf, pool, ppVB)
        uint32_t len = Arg(c, 1), ppVB = Arg(c, 5);
        uint32_t hdrVA = GuestHeapAlloc(16u, 16u);
        uint32_t bufVA = GuestHeapAlloc(std::max<uint32_t>(len, 16u), 64u);
        if (hdrVA && bufVA) {
            std::memset(c.ptr(hdrVA), 0, 16);
            c.w32(hdrVA + 0, 0x00000001u); // D3DCOMMON_TYPE_VERTEXBUFFER
            c.w32(hdrVA + 4, bufVA);       // Data pointer
        }
        if (ppVB) c.w32(ppVB, hdrVA);
        c.eax = 0u;
        RetStdcall(c, 20);
    });
    RegisterGuestFunc(0x00596330u, [](X86Context& c) { // _D3DVertexBuffer_Lock@20(pVB, off, sz, ppbData, flags)
        uint32_t pVB = Arg(c, 1), off = Arg(c, 2), ppb = Arg(c, 4);
        if (pVB && ppb) c.w32(ppb, c.r32(pVB + 4u) + off);
        c.eax = 0u;
        RetStdcall(c, 20);
    });
    RegisterGuestFunc(0x00596390u, [](X86Context& c) { // _D3DDevice_CreatePalette@8(size, ppPal)
        uint32_t ppPal = Arg(c, 2);
        uint32_t hdrVA = GuestHeapAlloc(16u, 16u);
        uint32_t bufVA = GuestHeapAlloc(1024u, 64u);
        if (hdrVA && bufVA) {
            std::memset(c.ptr(hdrVA), 0, 16);
            std::memset(c.ptr(bufVA), 0, 1024);
            c.w32(hdrVA + 0, 0x00030001u);
            c.w32(hdrVA + 4, bufVA);
        }
        if (ppPal) c.w32(ppPal, hdrVA);
        c.eax = 0u;
        RetStdcall(c, 8);
    });
    RegisterGuestFunc(0x005962A0u, [](X86Context& c) { // _D3DPalette_Lock@12(pPal, ppData, flags)
        uint32_t pPal = Arg(c, 1), ppData = Arg(c, 2);
        if (pPal && ppData) {
            uint32_t palVA = c.r32(pPal + 4u);
            if (palVA) d3d11_bridge::InvalidateTexture(palVA);
            c.w32(ppData, palVA);
        }
        c.eax = 0u;
        RetStdcall(c, 12);
    });
    RegisterGuestFunc(0x005928F0u, [](X86Context& c) { c.eax = 0u; RetStdcall(c, 0); }); // _D3DDevice_Release@0
    RegisterGuestFunc(0x00592770u, [](X86Context& c) { // _D3DDevice_CreateVolumeTexture@32(w, h, d, lvls, usage, fmt, pool, ppVol)
        uint32_t w = std::max<uint32_t>(1u, Arg(c, 1));
        uint32_t h = std::max<uint32_t>(1u, Arg(c, 2));
        uint32_t d = std::max<uint32_t>(1u, Arg(c, 3));
        uint32_t fmt = Arg(c, 6), ppVol = Arg(c, 8);
        uint32_t bpp = (fmt == 0x0Bu || fmt == 0x00u || fmt == 0x19u) ? 1u :
                       ((fmt == 0x02u || fmt == 0x04u || fmt == 0x05u || fmt == 0x1Au) ? 2u : 4u);
        uint32_t pixBytes = std::max<uint32_t>(64u, w * h * d * bpp);
        uint32_t hdrVA = GuestHeapAlloc(32u, 16u);
        uint32_t pixVA = GuestHeapAlloc(pixBytes, 64u);
        if (hdrVA && pixVA) {
            std::memset(c.ptr(hdrVA), 0, 32);
            std::memset(c.ptr(pixVA), 0, pixBytes);
            uint32_t logW = 0, logH = 0, logD = 0;
            while ((1u << logW) < w && logW < 8u) ++logW;
            while ((1u << logH) < h && logH < 8u) ++logH;
            while ((1u << logD) < d && logD < 8u) ++logD;
            c.w32(hdrVA + 0,  0x00050001u); // D3DCOMMON_TYPE_VOLUMETEXTURE
            c.w32(hdrVA + 4,  pixVA);
            c.w32(hdrVA + 12, (fmt << 8) | (3u << 4) | (logW << 20) | (logH << 24) | (logD << 28));
            c.w32(hdrVA + 16, ((w - 1u) & 0xFFFu) | (((h - 1u) & 0xFFFu) << 12));
            c.w32(hdrVA + 20, d);
            c.w32(hdrVA + 24, bpp);
        }
        if (ppVol) c.w32(ppVol, hdrVA);
        c.eax = (hdrVA && pixVA) ? 0u : 0x8007000Eu;
        RetStdcall(c, 32);
    });
    RegisterGuestFunc(0x00595260u, [](X86Context& c) { // _D3DVolumeTexture_GetLevelDesc@12(pVol, lvl, pDesc)
        uint32_t pVol = Arg(c, 1), pDesc = Arg(c, 3);
        if (pVol && pDesc) {
            uint32_t fmt = (c.r32(pVol + 12u) >> 8) & 0xFFu;
            uint32_t sz  = c.r32(pVol + 16u);
            uint32_t w   = (sz & 0xFFFu) + 1u;
            uint32_t h   = ((sz >> 12) & 0xFFFu) + 1u;
            uint32_t d   = std::max<uint32_t>(1u, c.r32(pVol + 20u));
            std::memset(c.ptr(pDesc), 0, 32);
            c.w32(pDesc + 0, fmt);
            c.w32(pDesc + 16, w * h * d * 4u);
            c.w32(pDesc + 20, w);
            c.w32(pDesc + 24, h);
            c.w32(pDesc + 28, d);
        }
        c.eax = 0u;
        RetStdcall(c, 12);
    });
    RegisterGuestFunc(0x00595270u, [](X86Context& c) { // _D3DVolumeTexture_LockBox@20(pVol, lvl, pLockedBox, pBox, flags)
        uint32_t pVol = Arg(c, 1), pLB = Arg(c, 3);
        if (pVol && pLB) {
            uint32_t pixVA = c.r32(pVol + 4u);
            uint32_t sz    = c.r32(pVol + 16u);
            uint32_t w     = (sz & 0xFFFu) + 1u;
            uint32_t h     = ((sz >> 12) & 0xFFFu) + 1u;
            uint32_t bpp   = std::max<uint32_t>(1u, c.r32(pVol + 24u));
            c.w32(pLB + 0, w * bpp);     // RowPitch
            c.w32(pLB + 4, w * h * bpp); // SlicePitch
            c.w32(pLB + 8, pixVA);       // pBits
            if (pixVA) d3d11_bridge::InvalidateTexture(pixVA);
        }
        c.eax = 0u;
        RetStdcall(c, 20);
    });
    RegisterGuestFunc(0x005A697Bu, [](X86Context& c) { // _XGSwizzleBox@40(pSrc, rowPitch, slicePitch, pBox, pDst, w, h, d, pPt, bpp)
        uint32_t pSrcVA = Arg(c, 1);
        uint32_t rowPitch = Arg(c, 2);
        uint32_t slicePitch = Arg(c, 3);
        uint32_t pBoxVA = Arg(c, 4);
        uint32_t pDstVA = Arg(c, 5);
        uint32_t w = Arg(c, 6), h = Arg(c, 7), d = Arg(c, 8);
        uint32_t pPtVA = Arg(c, 9);
        uint32_t bpp = Arg(c, 10);
        if (pSrcVA && pDstVA && w > 0 && h > 0 && d > 0 && bpp > 0 && bpp <= 16 && w <= 256 && h <= 256 && d <= 256 &&
            (w & (w - 1u)) == 0u && (h & (h - 1u)) == 0u && (d & (d - 1u)) == 0u) {
            uint32_t bx0 = 0, by0 = 0, bz0 = 0, bx1 = w, by1 = h, bz1 = d;
            if (pBoxVA) {
                bx0 = c.r32(pBoxVA + 0);  by0 = c.r32(pBoxVA + 4);
                bx1 = c.r32(pBoxVA + 8);  by1 = c.r32(pBoxVA + 12);
                bz0 = c.r32(pBoxVA + 16); bz1 = c.r32(pBoxVA + 20);
            }
            uint32_t ox = 0, oy = 0, oz = 0;
            if (pPtVA) {
                ox = c.r32(pPtVA + 0); oy = c.r32(pPtVA + 4); oz = c.r32(pPtVA + 8);
            }
            uint32_t subW = (bx1 > bx0) ? (bx1 - bx0) : w;
            uint32_t subH = (by1 > by0) ? (by1 - by0) : h;
            uint32_t subD = (bz1 > bz0) ? (bz1 - bz0) : d;
            if (rowPitch == 0) rowPitch = subW * bpp;
            if (slicePitch == 0) slicePitch = rowPitch * subH;
            size_t maxDstBytes = static_cast<size_t>(w) * h * d * bpp;

            uint32_t maskU = 0, maskV = 0, maskW = 0, j = 1;
            uint32_t tw = w, th = h, td = d;
            while (tw > 1 || th > 1 || td > 1) {
                if (tw > 1) { maskU |= j; j <<= 1; tw >>= 1; }
                if (th > 1) { maskV |= j; j <<= 1; th >>= 1; }
                if (td > 1) { maskW |= j; j <<= 1; td >>= 1; }
            }
            auto expandBits = [](uint32_t val, uint32_t mask) -> uint32_t {
                uint32_t res = 0, bit = 1;
                for (uint32_t m = 1; m <= mask && m != 0; m <<= 1) {
                    if (mask & m) {
                        if (val & bit) res |= m;
                        bit <<= 1;
                    }
                }
                return res;
            };
            std::vector<uint8_t> srcCopy;
            const uint8_t* srcPtr = c.ptr(pSrcVA);
            uint8_t* dstPtr = c.ptr(pDstVA);
            if (pSrcVA == pDstVA) {
                size_t copyBytes = std::min<size_t>(maxDstBytes, static_cast<size_t>(slicePitch) * subD);
                srcCopy.assign(srcPtr, srcPtr + copyBytes);
                srcPtr = srcCopy.data();
            }
            for (uint32_t z = 0; z < subD && (oz + z) < d; ++z) {
                uint32_t sw = expandBits(oz + z, maskW);
                for (uint32_t y = 0; y < subH && (oy + y) < h; ++y) {
                    uint32_t sv = expandBits(oy + y, maskV);
                    const uint8_t* sRow = srcPtr + static_cast<size_t>(z) * slicePitch + static_cast<size_t>(y) * rowPitch;
                    for (uint32_t x = 0; x < subW && (ox + x) < w; ++x) {
                        uint32_t su = expandBits(ox + x, maskU);
                        size_t dstOff = static_cast<size_t>(su | sv | sw) * bpp;
                        if (dstOff + bpp <= maxDstBytes) {
                            std::memcpy(dstPtr + dstOff, sRow + static_cast<size_t>(x) * bpp, bpp);
                        }
                    }
                }
            }
            d3d11_bridge::InvalidateTexture(pDstVA);
        }
        RetStdcall(c, 40);
    });
    RegisterGuestFunc(0x005927A0u, [](X86Context& c) { // _D3DDevice_CreateCubeTexture@24(edge, lvls, usage, fmt, pool, ppCube)
        uint32_t edge = Arg(c, 1), fmt = Arg(c, 4), ppCube = Arg(c, 6);
        uint32_t hdrVA = GuestHeapAlloc(32u, 16u);
        uint32_t pixBytes = std::max<uint32_t>(256u, edge * edge * 4u);
        uint32_t pixVA = GuestHeapAlloc(pixBytes, 64u);
        if (hdrVA && pixVA) {
            std::memset(c.ptr(hdrVA), 0, 32);
            std::memset(c.ptr(pixVA), 0, pixBytes);
            c.w32(hdrVA + 0,  0x00040001u);
            c.w32(hdrVA + 4,  pixVA);
            c.w32(hdrVA + 12, (fmt << 8) | (2u << 4));
            c.w32(hdrVA + 16, ((edge - 1u) & 0xFFFu) | (((edge - 1u) & 0xFFFu) << 12));
        }
        if (ppCube) c.w32(ppCube, hdrVA);
        c.eax = 0u;
        RetStdcall(c, 24);
    });
    RegisterGuestFunc(0x00595200u, [](X86Context& c) { // _D3DCubeTexture_GetCubeMapSurface@16(pCube, face, lvl, ppSurf)
        uint32_t pCube = Arg(c, 1), ppSurf = Arg(c, 4);
        if (IsGuestHeapAddr(pCube)) {
            uint32_t common = c.r32(pCube + 0u);
            c.w32(pCube + 0u, (common & 0xFFFF0000u) | (((common & 0xFFFFu) + 1u) & 0xFFFFu));
        }
        if (ppSurf) c.w32(ppSurf, pCube);
        c.eax = 0u;
        RetStdcall(c, 16);
    });
    RegisterGuestFunc(0x005967C0u, [](X86Context& c) { // _D3DSurface_LockRect@16(pSurf, pLockedRect, pRect, flags)
        uint32_t pSurf = Arg(c, 1), pLR = Arg(c, 2);
        if (pSurf && pLR) {
            uint32_t pixVA = c.r32(pSurf + 4u);
            uint32_t sz    = c.r32(pSurf + 16u);
            uint32_t w     = (sz & 0xFFFu) + 1u;
            c.w32(pLR + 0, w * 4u);
            c.w32(pLR + 4, pixVA);
            if (pixVA) d3d11_bridge::InvalidateTexture(pixVA);
        }
        c.eax = 0u;
        RetStdcall(c, 16);
    });
    RegisterGuestFunc(0x00591EA0u, [](X86Context& c) { // _D3DDevice_SetTextureStageStateNotInline@12(stage, type, val)
        uint32_t stage = Arg(c, 1), type = Arg(c, 2), val = Arg(c, 3);
        if (stage < 4u && type < 32u) c.w32(0x0059F740u + (stage * 32u + type) * 4u, val);
        RetStdcall(c, 12);
    });
    RegisterGuestFunc(0x00591C90u, [](X86Context& c) { // _D3DDevice_SetTextureState_TexCoordIndex@8(stage, val)
        uint32_t stage = Arg(c, 1), val = Arg(c, 2);
        if (stage < 4u) c.w32(0x0059F740u + (stage * 32u + 28u) * 4u, val);
        RetStdcall(c, 8);
    });
    RegisterGuestFunc(0x00591DB0u, [](X86Context& c) { RetStdcall(c, 12); }); // _D3DDevice_SetTextureState_BumpEnv@12
    RegisterGuestFunc(0x00591E20u, [](X86Context& c) { RetStdcall(c, 8); });  // _D3DDevice_SetTextureState_BorderColor@8
    RegisterGuestFunc(0x00591E60u, [](X86Context& c) { RetStdcall(c, 8); });  // _D3DDevice_SetTextureState_ColorKeyColor@8
    RegisterGuestFunc(0x0059C660u, [](X86Context& c) { RetStdcall(c, 4); });  // _D3DDevice_SetPixelShader@4
    RegisterGuestFunc(0x005783E3u, [](X86Context& c) { // _GetDiskFreeSpaceExA@16
        // Report 512 MB (0x20000000) so signed 32-bit block math never overflows negative!
        uint64_t freeBytes = 512ULL * 1024ULL * 1024ULL;
        for (int a = 2; a <= 4; ++a) {
            uint32_t p = Arg(c, a);
            if (p) std::memcpy(c.ptr(p), &freeBytes, 8);
        }
        c.eax = 1u;
        RetStdcall(c, 16);
    });
    RegisterGuestFunc(0x00610BFFu, [](X86Context& c) { c.eax = 1167u; RetStdcall(c, 12); }); // _XMountMUA@12 (not connected)
    RegisterGuestFunc(0x0057799Eu, [](X86Context& c) { // _GetExitCodeThread@8
        uint32_t pCode = Arg(c, 2);
        if (pCode) c.w32(pCode, 0u);
        c.eax = 1u;
        RetStdcall(c, 8);
    });
    RegisterGuestFunc(0x006106DDu, [](X86Context& c) { // _XGetDeviceChanges@12(devType, pIns, pRem)
        static bool s_first = true;
        uint32_t pIns = Arg(c, 2), pRem = Arg(c, 3);
        if (pIns) c.w32(pIns, s_first ? 1u : 0u); // Report Port 0 controller insertion on first poll!
        if (pRem) c.w32(pRem, 0u);
        c.eax = s_first ? 1u : 0u;
        s_first = false;
        RetStdcall(c, 12);
    });
    // All _D3DDevice_SetRenderState_*@4 functions
    for (uint32_t rsVA : {
        0x005913A0u, 0x005913D0u, 0x00591400u, 0x00591730u, 0x00591770u,
        0x00591870u, 0x005918B0u, 0x00591950u, 0x005919C0u, 0x00591A40u,
        0x00591AC0u, 0x00591B20u, 0x00591B70u, 0x00591BD0u, 0x00591C40u,
        0x00592390u, 0x00592420u, 0x00592490u, 0x005924C0u, 0x00592530u, 0x005925A0u,
        0x005925C0u, 0x005925E0u, 0x00592600u, 0x00592660u
    }) {
        RegisterGuestFunc(rsVA, [](X86Context& c) { RetStdcall(c, 4); });
    }
    RegisterGuestFunc(0x00591460u, [](X86Context& c) { // _D3DDevice_SetRenderStateNotInline@8(state, val)
        uint32_t st = Arg(c, 1), val = Arg(c, 2);
        if (st < 150u) c.w32(0x0059F940u + st * 4u, val);
        RetStdcall(c, 8);
    });
    RegisterGuestFunc(0x005917B0u, [](X86Context& c) { c.w32(0x0059F940u + 120u * 4u, Arg(c, 1)); RetStdcall(c, 4); }); // SetRenderState_FogColor
    RegisterGuestFunc(0x00591800u, [](X86Context& c) { c.w32(0x0059F940u + 129u * 4u, Arg(c, 1)); RetStdcall(c, 4); }); // SetRenderState_CullMode
    RegisterGuestFunc(0x00592300u, [](X86Context& c) { c.w32(0x0059F940u + 125u * 4u, Arg(c, 1)); RetStdcall(c, 4); }); // SetRenderState_ZEnable
    RegisterGuestFunc(0x00592740u, [](X86Context& c) { // _D3DDevice_CreateTexture@28(w, h, lvls, usage, fmt, pool, ppTex)
        uint32_t w = Arg(c, 1), h = Arg(c, 2), fmt = Arg(c, 5), ppTex = Arg(c, 7);
        uint32_t hdrVA = GuestHeapAlloc(32u, 16u);
        uint32_t pixBytes = 64u;
        if (fmt == 0x0Cu) pixBytes = std::max<uint32_t>(64u, ((w + 3u) / 4u) * ((h + 3u) / 4u) * 8u);
        else if (fmt == 0x0Eu || fmt == 0x0Fu) pixBytes = std::max<uint32_t>(64u, ((w + 3u) / 4u) * ((h + 3u) / 4u) * 16u);
        else if (fmt == 0x0Bu || fmt == 0x00u || fmt == 0x19u || fmt == 0x13u || fmt == 0x1Fu) pixBytes = std::max<uint32_t>(64u, w * h);
        else if (fmt == 0x02u || fmt == 0x04u || fmt == 0x05u || fmt == 0x1Au || fmt == 0x11u || fmt == 0x1Cu || fmt == 0x1Du || fmt == 0x20u) pixBytes = std::max<uint32_t>(64u, w * h * 2u);
        else pixBytes = std::max<uint32_t>(64u, w * h * 4u);
        uint32_t lvls = Arg(c, 3);
        uint32_t allocBytes = (lvls != 1u) ? (pixBytes + std::max<uint32_t>(64u, pixBytes / 2u)) : pixBytes;
        uint32_t pixVA = GuestHeapAlloc(allocBytes, 64u);
        if (hdrVA && pixVA) {
            std::memset(c.ptr(hdrVA), 0, 32);
            std::memset(c.ptr(pixVA), 0, allocBytes);
            uint32_t logW = 0, logH = 0;
            while ((1u << logW) < w && logW < 11u) ++logW;
            while ((1u << logH) < h && logH < 11u) ++logH;
            c.w32(hdrVA + 0,  0x00040001u); // Common
            c.w32(hdrVA + 4,  pixVA);       // Data (mip 0)
            c.w32(hdrVA + 12, (fmt << 8) | (2u << 4) | (logW << 20) | (logH << 24));
            c.w32(hdrVA + 16, ((w - 1u) & 0xFFFu) | (((h - 1u) & 0xFFFu) << 12));
            c.w32(hdrVA + 24, pixBytes);    // Store mip-0 byte size so LockRect(lvl>0) never overwrites mip 0!
        } else {
            if (hdrVA) { GuestHeapFree(hdrVA); hdrVA = 0; }
            if (pixVA) { GuestHeapFree(pixVA); pixVA = 0; }
        }
        if (ppTex) c.w32(ppTex, hdrVA);
        c.eax = (hdrVA && pixVA) ? 0u : 0x8007000Eu; // D3D_OK or E_OUTOFMEMORY
        RetStdcall(c, 28);
    });
    RegisterGuestFunc(0x005951D0u, [](X86Context& c) { // _D3DTexture_LockRect@20(pTex, lvl, pLockedRect, pRect, flags)
        uint32_t pTex = Arg(c, 1), lvl = Arg(c, 2), pLR = Arg(c, 3);
        if (pTex && pLR) {
            uint32_t pixVA = c.r32(pTex + 4u);
            uint32_t fmt   = (c.r32(pTex + 12u) >> 8) & 0xFFu;
            uint32_t sz    = c.r32(pTex + 16u);
            uint32_t mip0Sz = c.r32(pTex + 24u);
            uint32_t w     = std::max<uint32_t>(1u, ((sz & 0xFFFu) + 1u) >> lvl);
            uint32_t bpp   = (fmt == 0x0Bu || fmt == 0x00u || fmt == 0x19u) ? 1u :
                             ((fmt == 0x02u || fmt == 0x04u || fmt == 0x05u || fmt == 0x1Au) ? 2u : 4u);
            uint32_t pitch = (fmt == 0x0Cu) ? std::max<uint32_t>(8u, ((w + 3u) / 4u) * 8u) :
                             ((fmt == 0x0Eu || fmt == 0x0Fu) ? std::max<uint32_t>(16u, ((w + 3u) / 4u) * 16u) : (w * bpp));
            uint32_t lockVA = (lvl == 0u) ? pixVA : (pixVA + mip0Sz);
            if (lvl == 0u && pixVA) d3d11_bridge::InvalidateTexture(pixVA);
            c.w32(pLR + 0, pitch);   // Pitch
            c.w32(pLR + 4, lockVA);  // pBits
        }
        c.eax = 0u;
        RetStdcall(c, 20);
    });
    RegisterGuestFunc(0x005936C0u, [](X86Context& c) { // _D3DDevice_SetPalette@8(stage, pPal)
        d3d11_bridge::SetPalette(Arg(c, 1), c.mem, Arg(c, 2));
        RetStdcall(c, 8);
    });
    RegisterGuestFunc(0x005918F0u, [](X86Context& c) { c.w32(0x0059F940u + 130u * 4u, Arg(c, 1)); RetStdcall(c, 4); }); // SetRenderState_TextureFactor
    RegisterGuestFunc(0x00594D80u, [](X86Context& c) { RetStdcall(c, 12); }); // _D3DDevice_SetVertexShaderConstant@12

    // XInput bridges (Real xinput1_4.dll Gamepad + Keyboard fallback -> Original Xbox XINPUT_STATE)
    RegisterGuestFunc(0x006106B6u, [](X86Context& c) { RetStdcall(c, 8); });                      // XInitDevices
    RegisterGuestFunc(0x006106BBu, [](X86Context& c) { c.eax = 1u; RetStdcall(c, 4); });          // XGetDevices (Port 0)
    RegisterGuestFunc(0x0061058Du, [](X86Context& c) { // XInputOpen(devType, dwPort, dwSlot, pPoll)
        uint32_t port = Arg(c, 2);
        c.eax = (port == 0u) ? 0xCAFE0001u : 0u;
        RetStdcall(c, 16);
    });
    RegisterGuestFunc(0x006105EFu, [](X86Context& c) {                                            // XInputGetState
        uint32_t pSt = Arg(c, 2);
        if (pSt) {
            struct HostXInputGamepad {
                uint16_t wButtons;
                uint8_t  bLeftTrigger, bRightTrigger;
                int16_t  sThumbLX, sThumbLY, sThumbRX, sThumbRY;
            };
            struct HostXInputState {
                uint32_t dwPacketNumber;
                HostXInputGamepad Gamepad;
            };
            using PfnXInputGetState = uint32_t (WINAPI*)(uint32_t, HostXInputState*);
            static PfnXInputGetState s_pfn = nullptr;
            static bool s_loaded = false;
            if (!s_loaded) {
                s_loaded = true;
                HMODULE hm = LoadLibraryA("xinput1_4.dll");
                if (!hm) hm = LoadLibraryA("xinput9_1_0.dll");
                if (hm) s_pfn = reinterpret_cast<PfnXInputGetState>(GetProcAddress(hm, "XInputGetState"));
            }

            static uint32_t s_pkt = 1u;
            uint16_t btns = 0;
            uint8_t analog[8] = {};
            int16_t lx = 0, ly = 0, rx = 0, ry = 0;

            HostXInputState hs{};
            bool gotPad = false;
            static bool s_loggedPad = false;
            if (s_pfn) {
                for (uint32_t slot = 0; slot < 4u; ++slot) {
                    if (s_pfn(slot, &hs) == 0u) {
                        gotPad = true;
                        if (!s_loggedPad) {
                            s_loggedPad = true;
                            std::printf("[UWP-PAD] XInput controller connected on slot %u!\n", slot);
                        }
                        break;
                    }
                }
            }
            {
                uint32_t curF = d3d11_bridge::GetFrameCount();
                for (uint32_t pf : g_scriptPressFrames) {
                    if (curF >= pf && curF < pf + 8u) {
                        btns |= 0x0010u;   // START
                        analog[0] = 255u;  // A
                        break;
                    }
                }
            }
            if (gotPad) {
                btns |= (hs.Gamepad.wButtons & 0x00FFu); // D-pad + Start/Back/L3/R3 match Original Xbox!
                if (hs.Gamepad.wButtons & 0x1000u) analog[0] = 255; // A
                if (hs.Gamepad.wButtons & 0x2000u) analog[1] = 255; // B
                if (hs.Gamepad.wButtons & 0x4000u) analog[2] = 255; // X
                if (hs.Gamepad.wButtons & 0x8000u) analog[3] = 255; // Y
                if (hs.Gamepad.wButtons & 0x0200u) analog[4] = 255; // RB -> Black
                if (hs.Gamepad.wButtons & 0x0100u) analog[5] = 255; // LB -> White
                analog[6] = hs.Gamepad.bLeftTrigger;
                analog[7] = hs.Gamepad.bRightTrigger;
                lx = hs.Gamepad.sThumbLX; ly = hs.Gamepad.sThumbLY;
                rx = hs.Gamepad.sThumbRX; ry = hs.Gamepad.sThumbRY;
            }

            auto kd = [](int vk) -> bool { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
            uint16_t rawPadBtns = gotPad ? hs.Gamepad.wButtons : 0u;
            int16_t  rawPadLY   = gotPad ? hs.Gamepad.sThumbLY : 0;
            bool menuConsumed = config_ui::UpdateInput(
                rawPadBtns, rawPadLY,
                kd(VK_F1) || kd(VK_INSERT),
                kd(VK_UP), kd(VK_DOWN), kd(VK_LEFT), kd(VK_RIGHT),
                kd(VK_RETURN), kd(VK_BACK));
            if (menuConsumed) {
                btns = 0; std::memset(analog, 0, sizeof(analog));
                lx = ly = rx = ry = 0;
            } else {
                if (kd(VK_UP))    btns |= 0x0001u;
                if (kd(VK_DOWN))  btns |= 0x0002u;
                if (kd(VK_LEFT))  btns |= 0x0004u;
                if (kd(VK_RIGHT)) btns |= 0x0008u;
                if (kd(VK_RETURN)) btns |= 0x0010u; // START
                if (kd(VK_TAB))    btns |= 0x0020u; // BACK
                if (kd(VK_SPACE) || kd('Z') || kd('J')) analog[0] = 255; // A
                if (kd(VK_BACK)  || kd('X') || kd('K')) analog[1] = 255; // B
                if (kd('C') || kd('U')) analog[2] = 255; // X
            }
            if (kd('V') || kd('I')) analog[3] = 255; // Y
            if (kd('W')) ly =  32767;
            if (kd('S')) ly = -32767;
            if (kd('A')) lx = -32767;
            if (kd('D')) lx =  32767;

            static bool s_loggedBtn = false;
            if (!s_loggedBtn && (btns || analog[0] || analog[1])) {
                s_loggedBtn = true;
                std::printf("[UWP-INPUT] First button press delivered to game! btns=0x%04X A=%u B=%u\n",
                            btns, analog[0], analog[1]);
            }
            uint8_t* dst = c.ptr(pSt);
            std::memset(dst, 0, 24);
            uint32_t pkt = s_pkt++;
            std::memcpy(dst + 0x00, &pkt, 4);
            std::memcpy(dst + 0x04, &btns, 2);
            std::memcpy(dst + 0x06, analog, 8);
            std::memcpy(dst + 0x0E, &lx, 2);
            std::memcpy(dst + 0x10, &ly, 2);
            std::memcpy(dst + 0x12, &rx, 2);
            std::memcpy(dst + 0x14, &ry, 2);
        }
        c.eax = 0u; // ERROR_SUCCESS
        RetStdcall(c, 8);
    });
    RegisterGuestFunc(0x0061065Eu, [](X86Context& c) { c.eax = 0u; RetStdcall(c, 8); });          // XInputSetState
    RegisterGuestFunc(0x00063920u, [](X86Context& c) { // ShellSetTitle(str)
        const char* t = reinterpret_cast<const char*>(c.ptr(Arg(c, 1)));
        std::printf("[UWP-ShellSetTitle] %s\n", t ? t : "(null)");
        RetCdecl(c);
    });
    RegisterGuestFunc(0x00587D06u, [](X86Context& c) { // OutputDebugStringA
        const char* s = reinterpret_cast<const char*>(c.ptr(Arg(c, 1)));
        if (s) std::printf("[UWP-DBG] %s", s);
        RetStdcall(c, 4);
    });

    // ------------------------------------------------------------------------
    // 3. Virtual File System (CreateFileA / ReadFile / WriteFile / CloseHandle / GetFileSize)
    // ------------------------------------------------------------------------
    RegisterGuestFunc(0x00577F57u, [](X86Context& c) { // CreateFileA
        const char* path = reinterpret_cast<const char*>(c.ptr(Arg(c, 1)));
        DWORD access     = Arg(c, 2);
        DWORD share      = Arg(c, 3);
        DWORD disp       = Arg(c, 5);
        bool forWrite = (access & GENERIC_WRITE) != 0 || disp == CREATE_ALWAYS || disp == CREATE_NEW;
        std::string host = TranslateGuestPath(path, forWrite);
        DWORD actualDisp = disp;
        std::string openTarget = host;
        if (forWrite) {
            for (size_t i = 0; i < host.size(); ++i) {
                if (host[i] == '\\') CreateDirectoryA(host.substr(0, i).c_str(), nullptr);
            }
            if (disp == CREATE_NEW) {
                if (GetFileAttributesA(host.c_str()) != INVALID_FILE_ATTRIBUTES &&
                    host.rfind(g_zRoot, 0) == 0) {
                    // Cache file already unpacked in recomp\cache — sink writes to NUL so installer finishes fast!
                    openTarget = "NUL";
                    actualDisp = OPEN_EXISTING;
                } else {
                    actualDisp = CREATE_ALWAYS;
                }
            }
        }
        HANDLE h = CreateFileA(openTarget.c_str(), access, share ? share : (FILE_SHARE_READ | FILE_SHARE_WRITE),
                               nullptr, actualDisp, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            std::printf("[UWP-VFS] CreateFileA FAILED: '%s' -> '%s' (disp=%lu)\n",
                        path ? path : "(null)", host.c_str(), static_cast<unsigned long>(disp));
            c.eax = 0xFFFFFFFFu;
        } else {
            SetLastError(0);
            uint32_t gh = 0;
            {
                std::lock_guard<std::mutex> lk(g_vfsMtx);
                gh = g_nextHandle++;
                g_fileHandles[gh] = h;
            }
            if (path && (std::strstr(path, ".rpe") || std::strstr(path, ".tgr") || std::strstr(path, ".bfl") || std::strstr(path, ".RPE") || std::strstr(path, ".TGR"))) {
                static uint32_t s_openLogCount = 0;
                if (++s_openLogCount <= 30u) {
                    std::printf("[UWP-VFS] Open #%u: '%s' -> '%s'\n", s_openLogCount, path, openTarget.c_str());
                }
            }
            c.eax = gh;
        }
        RetStdcall(c, 28);
    });
    RegisterGuestFunc(0x00577B76u, [](X86Context& c) { // ReadFile
        ++g_cntReadFile;
        uint32_t gh    = Arg(c, 1);
        uint32_t bufVA = Arg(c, 2);
        DWORD    nReq  = Arg(c, 3);
        uint32_t pRead = Arg(c, 4);
        HANDLE h = INVALID_HANDLE_VALUE;
        {
            std::lock_guard<std::mutex> lk(g_vfsMtx);
            auto it = g_fileHandles.find(gh);
            if (it != g_fileHandles.end()) h = it->second;
        }
        DWORD got = 0;
        BOOL ok = (h != INVALID_HANDLE_VALUE) ? ReadFile(h, c.ptr(bufVA), nReq, &got, nullptr) : FALSE;
        if (ok) SetLastError(0);
        if (pRead) c.w32(pRead, got);
        c.eax = ok ? 1u : 0u;
        RetStdcall(c, 20);
    });
    RegisterGuestFunc(0x00577C63u, [](X86Context& c) { // WriteFile
        uint32_t gh    = Arg(c, 1);
        uint32_t bufVA = Arg(c, 2);
        DWORD    nReq  = Arg(c, 3);
        uint32_t pWrt  = Arg(c, 4);
        HANDLE h = INVALID_HANDLE_VALUE;
        {
            std::lock_guard<std::mutex> lk(g_vfsMtx);
            auto it = g_fileHandles.find(gh);
            if (it != g_fileHandles.end()) h = it->second;
        }
        DWORD wrt = 0;
        BOOL ok = (h != INVALID_HANDLE_VALUE) ? WriteFile(h, c.ptr(bufVA), nReq, &wrt, nullptr) : FALSE;
        if (ok) SetLastError(0);
        if (pWrt) c.w32(pWrt, wrt);
        c.eax = ok ? 1u : 0u;
        RetStdcall(c, 20);
    });
    RegisterGuestFunc(0x00577B58u, [](X86Context& c) { // CloseHandle
        uint32_t gh = Arg(c, 1);
        HANDLE h = INVALID_HANDLE_VALUE;
        {
            std::lock_guard<std::mutex> lk(g_vfsMtx);
            auto it = g_fileHandles.find(gh);
            if (it != g_fileHandles.end()) { h = it->second; g_fileHandles.erase(it); }
        }
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        c.eax = 1u;
        RetStdcall(c, 4);
    });
    RegisterGuestFunc(0x00577F1Cu, [](X86Context& c) { // GetFileSize
        uint32_t gh = Arg(c, 1);
        HANDLE h = INVALID_HANDLE_VALUE;
        {
            std::lock_guard<std::mutex> lk(g_vfsMtx);
            auto it = g_fileHandles.find(gh);
            if (it != g_fileHandles.end()) h = it->second;
        }
        DWORD hi = 0;
        DWORD lo = (h != INVALID_HANDLE_VALUE) ? GetFileSize(h, &hi) : INVALID_FILE_SIZE;
        uint32_t pHi = Arg(c, 2);
        if (pHi) c.w32(pHi, hi);
        c.eax = lo;
        RetStdcall(c, 8);
    });

    // ------------------------------------------------------------------------
    // 4. Timing, Threading, CRT Locks, Logging & XAPI Stubs
    // ------------------------------------------------------------------------
    RegisterGuestFunc(0x00578AA9u, [](X86Context& c) { // GetTickCount
        c.eax = static_cast<uint32_t>(GetTickCount64());
        RetStdcall(c, 0);
    });
    RegisterGuestFunc(0x00577B4Bu, [](X86Context& c) { c.eax = GetCurrentThreadId(); RetStdcall(c, 0); });
    RegisterGuestFunc(0x005775DDu, [](X86Context& c) { c.eax = GetLastError(); RetStdcall(c, 0); });
    RegisterGuestFunc(0x00577605u, [](X86Context& c) { SetLastError(Arg(c, 1)); RetStdcall(c, 4); });
    RegisterGuestFunc(0x005778D2u, [](X86Context& c) { // Sleep
        CheckWorkerAbort();
        ++g_cntSleep;
        uint32_t ms = Arg(c, 1);
        uint32_t caller = c.r32(c.esp);
        static std::unordered_map<uint32_t, uint32_t> s_sleepCallers;
        if (++s_sleepCallers[caller] == 1u) {
            std::printf("[UWP-SLEEP] First Sleep(%u) from caller=0x%08X (TID=%lu)\n",
                        ms, caller, static_cast<unsigned long>(GetCurrentThreadId()));
        }
        Sleep(ms);
        CheckWorkerAbort();
        RetStdcall(c, 4);
    });
    RegisterGuestFunc(0x0057786Eu, [](X86Context& c) { CheckWorkerAbort(); Sleep(Arg(c, 1)); CheckWorkerAbort(); c.eax = 0; RetStdcall(c, 8); });
    RegisterGuestFunc(0x0057798Bu, [](X86Context& c) { CheckWorkerAbort(); c.eax = SwitchToThread() ? 1u : 0u; RetStdcall(c, 0); });
    RegisterGuestFunc(0x005772DBu, [](X86Context& c) { // GlobalMemoryStatus (report real free guest heap!)
        uint32_t p = Arg(c, 1);
        if (p) {
            uint32_t avail = GuestHeapFreeBytes();
            uint32_t load  = (avail < kXboxRamSize) ? ((kXboxRamSize - avail) * 100u / kXboxRamSize) : 0u;
            c.w32(p + 0, 32u);
            c.w32(p + 4, load);
            c.w32(p + 8, kXboxRamSize);
            c.w32(p + 12, avail);
            c.w32(p + 16, kXboxRamSize);
            c.w32(p + 20, avail);
            c.w32(p + 24, kXboxRamSize);
            c.w32(p + 28, avail);
        }
        RetStdcall(c, 4);
    });
    RegisterGuestFunc(0x00577DBDu, [](X86Context& c) { // SetFilePointer
        HANDLE h = INVALID_HANDLE_VALUE;
        {
            std::lock_guard<std::mutex> lk(g_vfsMtx);
            auto it = g_fileHandles.find(Arg(c, 1));
            if (it != g_fileHandles.end()) h = it->second;
        }
        DWORD res = (h != INVALID_HANDLE_VALUE) ? SetFilePointer(h, static_cast<LONG>(Arg(c, 2)), nullptr, Arg(c, 4)) : INVALID_SET_FILE_POINTER;
        c.eax = res;
        RetStdcall(c, 16);
    });
    RegisterGuestFunc(0x005773D0u, [](X86Context& c) { // GetFileAttributesA
        const char* path = reinterpret_cast<const char*>(c.ptr(Arg(c, 1)));
        c.eax = GetFileAttributesA(TranslateGuestPath(path).c_str());
        RetStdcall(c, 4);
    });
    RegisterGuestFunc(0x0057CFE9u, [](X86Context& c) { // CreateDirectoryA
        const char* path = reinterpret_cast<const char*>(c.ptr(Arg(c, 1)));
        c.eax = CreateDirectoryA(TranslateGuestPath(path).c_str(), nullptr) ? 1u : 0u;
        RetStdcall(c, 8);
    });
    RegisterGuestFunc(0x0057D055u, [](X86Context& c) { // RemoveDirectoryA
        const char* path = reinterpret_cast<const char*>(c.ptr(Arg(c, 1)));
        c.eax = RemoveDirectoryA(TranslateGuestPath(path, true).c_str()) ? 1u : 0u;
        RetStdcall(c, 4);
    });
    RegisterGuestFunc(0x0057732Au, [](X86Context& c) { c.eax = 1u; RetStdcall(c, 8); }); // SetFileAttributesA
    RegisterGuestFunc(0x00577D39u, [](X86Context& c) { // SetEndOfFile
        HANDLE h = INVALID_HANDLE_VALUE;
        { std::lock_guard<std::mutex> lk(g_vfsMtx); auto it = g_fileHandles.find(Arg(c, 1)); if (it != g_fileHandles.end()) h = it->second; }
        c.eax = (h != INVALID_HANDLE_VALUE && SetEndOfFile(h)) ? 1u : 0u;
        RetStdcall(c, 4);
    });
    RegisterGuestFunc(0x00577EB7u, [](X86Context& c) { // FlushFileBuffers
        HANDLE h = INVALID_HANDLE_VALUE;
        { std::lock_guard<std::mutex> lk(g_vfsMtx); auto it = g_fileHandles.find(Arg(c, 1)); if (it != g_fileHandles.end()) h = it->second; }
        c.eax = (h != INVALID_HANDLE_VALUE && FlushFileBuffers(h)) ? 1u : 0u;
        RetStdcall(c, 4);
    });
    RegisterGuestFunc(0x00577EDEu, [](X86Context& c) { // GetFileSizeEx
        HANDLE h = INVALID_HANDLE_VALUE;
        { std::lock_guard<std::mutex> lk(g_vfsMtx); auto it = g_fileHandles.find(Arg(c, 1)); if (it != g_fileHandles.end()) h = it->second; }
        LARGE_INTEGER li{};
        BOOL ok = (h != INVALID_HANDLE_VALUE) ? GetFileSizeEx(h, &li) : FALSE;
        if (ok && Arg(c, 2)) std::memcpy(c.ptr(Arg(c, 2)), &li.QuadPart, 8);
        c.eax = ok ? 1u : 0u;
        RetStdcall(c, 8);
    });
    RegisterGuestFunc(0x0057778Bu, [](X86Context& c) { // CreateEventA(sa, manualReset, initialState, name)
        HANDLE h = CreateEventA(nullptr, Arg(c, 2) ? TRUE : FALSE, Arg(c, 3) ? TRUE : FALSE, nullptr);
        uint32_t gh = 0;
        if (h) { std::lock_guard<std::mutex> lk(g_vfsMtx); gh = g_nextHandle++; g_fileHandles[gh] = h; }
        c.eax = gh;
        RetStdcall(c, 16);
    });
    RegisterGuestFunc(0x005777ECu, [](X86Context& c) { // SetEvent
        HANDLE h = INVALID_HANDLE_VALUE;
        { std::lock_guard<std::mutex> lk(g_vfsMtx); auto it = g_fileHandles.find(Arg(c, 1)); if (it != g_fileHandles.end()) h = it->second; }
        c.eax = (h != INVALID_HANDLE_VALUE && SetEvent(h)) ? 1u : 0u;
        RetStdcall(c, 4);
    });
    RegisterGuestFunc(0x0057780Cu, [](X86Context& c) { // ResetEvent
        HANDLE h = INVALID_HANDLE_VALUE;
        { std::lock_guard<std::mutex> lk(g_vfsMtx); auto it = g_fileHandles.find(Arg(c, 1)); if (it != g_fileHandles.end()) h = it->second; }
        c.eax = (h != INVALID_HANDLE_VALUE && ResetEvent(h)) ? 1u : 0u;
        RetStdcall(c, 4);
    });
    RegisterGuestFunc(0x005778C0u, [](X86Context& c) { // WaitForSingleObject(h, ms)
        HANDLE h = INVALID_HANDLE_VALUE;
        { std::lock_guard<std::mutex> lk(g_vfsMtx); auto it = g_fileHandles.find(Arg(c, 1)); if (it != g_fileHandles.end()) h = it->second; }
        c.eax = (h != INVALID_HANDLE_VALUE) ? WaitForSingleObject(h, Arg(c, 2)) : WAIT_OBJECT_0;
        RetStdcall(c, 8);
    });
    RegisterGuestFunc(0x0057782Au, [](X86Context& c) { // WaitForSingleObjectEx(h, ms, alertable)
        HANDLE h = INVALID_HANDLE_VALUE;
        { std::lock_guard<std::mutex> lk(g_vfsMtx); auto it = g_fileHandles.find(Arg(c, 1)); if (it != g_fileHandles.end()) h = it->second; }
        c.eax = (h != INVALID_HANDLE_VALUE) ? WaitForSingleObjectEx(h, Arg(c, 2), Arg(c, 3) ? TRUE : FALSE) : WAIT_OBJECT_0;
        RetStdcall(c, 12);
    });
    RegisterGuestFunc(0x0057764Au, [](X86Context& c) { c.eax = 1u; RetStdcall(c, 16); }); // GetOverlappedResult
    RegisterGuestFunc(0x005790B6u, [](X86Context& c) { c.eax = 0u; RetStdcall(c, 20); }); // XAPI launch helper
    RegisterGuestFunc(0x00578F78u, [](X86Context& c) { c.eax = 0u; RetStdcall(c, 20); }); // XAPI launch helper
    RegisterGuestFunc(0x00610D31u, [](X86Context& c) { c.eax = 0u; RetStdcall(c, 8); });  // XUnmountMU
    RegisterGuestFunc(0x00576DE7u, [](X86Context& c) { c.eax = 0x51600001u; RetStdcall(c, 8); }); // XCalculateSignatureBeginEx
    RegisterGuestFunc(0x0057D5B4u, [](X86Context& c) { c.eax = 0xFFFFFFFFu; RetStdcall(c, 4); }); // XGetSectionHandleA
    RegisterGuestFunc(0x0057D61Bu, [](X86Context& c) { c.eax = 0u; RetStdcall(c, 4); });          // XLoadSectionByHandle
    RegisterGuestFunc(0x0057D63Cu, [](X86Context& c) { c.eax = 1u; RetStdcall(c, 4); });          // XFreeSectionByHandle
    RegisterGuestFunc(0x0057D65Au, [](X86Context& c) { c.eax = 0u; RetStdcall(c, 4); });          // XGetSectionSize
    RegisterGuestFunc(0x00593A80u, [](X86Context& c) { RetStdcall(c, 0); });                      // _D3DDevice_PersistDisplay@0
    RegisterGuestFunc(0x005969A0u, [](X86Context& c) { RetStdcall(c, 0); });                      // D3D8 internal stub
    RegisterGuestFunc(0x0057FAE9u, [](X86Context& c) { std::printf("[UWP-EXIT] Guest _exit(%u)\n", Arg(c, 1)); RetCdecl(c); });
    RegisterGuestFunc(0x005812A8u, [](X86Context& c) { std::printf("[UWP-EXIT] Guest __amsg_exit(%u)\n", Arg(c, 1)); RetCdecl(c); });
    // CRT threading stubs + Slate Engine TLocker no-ops (single-threaded main loop)
    RegisterGuestFunc(0x00582C0Eu, [](X86Context& c) { c.eax = 1u; RetCdecl(c); }); // _mtinit
    RegisterGuestFunc(0x00580DEEu, [](X86Context& c) { c.eax = 1u; RetCdecl(c); }); // _mtinitlocknum
    RegisterGuestFunc(0x00582A77u, [](X86Context& c) { if (!g_ptdVA) g_ptdVA = GuestHeapAlloc(512, 16); c.eax = g_ptdVA; RetCdecl(c); }); // _getptd
    RegisterGuestFunc(0x00582AF9u, [](X86Context& c) { RetCdecl(c); }); // _freeptd
    // Thread-safe host recursive_mutex per guest TLocker object (prevents multi-core x64 spinlock deadlock!)
    RegisterGuestFunc(0x002AA870u, [](X86Context& c) { ++g_cntLock; GetGuestLocker(c.ecx).lock(); RetCdecl(c); });   // TLocker::Lock
    RegisterGuestFunc(0x002AA950u, [](X86Context& c) { GetGuestLocker(c.ecx).unlock(); RetCdecl(c); }); // TLocker::Unlock
    RegisterGuestFunc(0x00063250u, [](X86Context& c) { // RunInstaller: skip if dialogue cache exists, else unpack install.dat!
        std::string m00 = g_zRoot + "\\dialogue\\m00_dialogue.rpe";
        std::string gen = g_zRoot + "\\dialogue\\gen_dialogue.rpe";
        if (GetFileAttributesA(m00.c_str()) != INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesA(gen.c_str()) != INVALID_FILE_ATTRIBUTES) {
            RetCdecl(c);
            return;
        }
        std::printf("[UWP-INSTALL] Unpacking extracted\\install.dat into %s (one-time)...\n", g_zRoot.c_str());
        sub_00063250(c);
    });
    RegisterGuestFunc(0x00063AE0u, [](X86Context& c) { RetCdecl(c); }); // ShellWarning
    RegisterGuestFunc(0x00064550u, [](X86Context& c) { // ShellAbort(const char* msg)
        const char* msg = reinterpret_cast<const char*>(c.ptr(Arg(c, 1)));
        std::printf("[UWP-ABORT] ShellAbort('%s') caller=0x%08X\n", msg ? msg : "(null)", c.r32(c.esp));
        DumpRecentGuestCalls();
        c.eax = 0u;
        RetCdecl(c);
    });
    RegisterGuestFunc(0x00064510u, [](X86Context& c) { // ShellHandleBug(file, line, fatal)
        const char* f = reinterpret_cast<const char*>(c.ptr(Arg(c, 1)));
        std::printf("[UWP-BUG] ShellHandleBug('%s', line=%u, fatal=%u)\n", f ? f : "(null)", Arg(c, 2), Arg(c, 3));
        RetCdecl(c);
    });
    RegisterGuestFunc(0x00064610u, [](X86Context& c) { // ShellQuit()
        std::printf("[UWP-QUIT] ShellQuit() called! caller=0x%08X\n", c.r32(c.esp));
        RetCdecl(c);
    });
    RegisterGuestFunc(0x00062650u, [](X86Context& c) { // G_ErrorToUser(errType, flag, msg)
        std::printf("[UWP-FATAL] G_ErrorToUser(type=%u, flag=%u, caller=0x%08X)\n",
                    Arg(c, 1), Arg(c, 2), c.r32(c.esp));
        DumpRecentGuestCalls();
        RetCdecl(c);
    });
    RegisterGuestFunc(0x00038220u, [](X86Context& c) { RetCdecl(c); }); // RenderLog
    RegisterGuestFunc(0x00038250u, [](X86Context& c) { RetCdecl(c); }); // RenderError
    RegisterGuestFunc(0x000A06D0u, [](X86Context& c) { RetCdecl(c); }); // ScriptLog
    RegisterGuestFunc(0x00297A90u, [](X86Context& c) { c.eax = 0u; RetStdcall(c, 20); }); // Movie_Play
    RegisterGuestFunc(0x002970A0u, [](X86Context& c) { c.eax = 0u; RetStdcall(c, 0); });  // Movie_Playing
    RegisterGuestFunc(0x00297020u, [](X86Context& c) { c.eax = 1u; RetStdcall(c, 8); });  // Movie_Kill
    RegisterGuestFunc(0x00297060u, [](X86Context& c) { c.eax = 1u; RetStdcall(c, 4); });  // Movie_KillCurrent

    // TIF_Math virtual SSE1 & x87 trig bridges (StdMathLink.obj — all virtual calls go via CallGuestVA!)
    RegisterGuestFunc(0x002A9090u, [](X86Context& c) { // TIF_Math::Normalize(T_Vector<float>*)
        uint32_t vVA = Arg(c, 1);
        float len = 0.0f;
        if (vVA) {
            float x = c.rf32(vVA), y = c.rf32(vVA + 4u), z = c.rf32(vVA + 8u);
            len = std::sqrt(x * x + y * y + z * z);
            if (len > 1e-12f) {
                float inv = 1.0f / len;
                c.wf32(vVA, x * inv); c.wf32(vVA + 4u, y * inv); c.wf32(vVA + 8u, z * inv);
            }
        }
        c.fpuPush(static_cast<double>(len));
        RetStdcall(c, 4);
    });
    RegisterGuestFunc(0x002A9130u, [](X86Context& c) { // TIF_Math::Length(T_Vector<float> const*)
        uint32_t vVA = Arg(c, 1);
        float len = 0.0f;
        if (vVA) {
            float x = c.rf32(vVA), y = c.rf32(vVA + 4u), z = c.rf32(vVA + 8u);
            len = std::sqrt(x * x + y * y + z * z);
        }
        c.fpuPush(static_cast<double>(len));
        RetStdcall(c, 4);
    });
    RegisterGuestFunc(0x002A9230u, [](X86Context& c) { // TIF_Math::Matrix4Mul(dst, a, b) (4x3 affine matrices)
        uint32_t dstVA = Arg(c, 1), aVA = Arg(c, 2), bVA = Arg(c, 3);
        if (dstVA && aVA && bVA) {
            float a[12], b[12], out[12];
            std::memcpy(a, c.ptr(aVA), 48);
            std::memcpy(b, c.ptr(bVA), 48);
            for (int r = 0; r < 3; ++r) {
                for (int col = 0; col < 3; ++col) {
                    out[r * 3 + col] = a[r * 3 + 0] * b[0 + col] + a[r * 3 + 1] * b[3 + col] + a[r * 3 + 2] * b[6 + col];
                }
            }
            for (int col = 0; col < 3; ++col) {
                out[9 + col] = a[9] * b[0 + col] + a[10] * b[3 + col] + a[11] * b[6 + col] + b[9 + col];
            }
            std::memcpy(c.ptr(dstVA), out, 48);
        }
        RetStdcall(c, 12);
    });
    RegisterGuestFunc(0x002A9430u, [](X86Context& c) { // TIF_Math::Matrix3Transform(dst, mat, vec)
        uint32_t dstVA = Arg(c, 1), mVA = Arg(c, 2), vVA = Arg(c, 3);
        if (dstVA && mVA && vVA) {
            float m[9], v[3], out[3];
            std::memcpy(m, c.ptr(mVA), 36);
            std::memcpy(v, c.ptr(vVA), 12);
            for (int col = 0; col < 3; ++col) {
                out[col] = v[0] * m[0 + col] + v[1] * m[3 + col] + v[2] * m[6 + col];
            }
            std::memcpy(c.ptr(dstVA), out, 12);
        }
        RetStdcall(c, 12);
    });
    RegisterGuestFunc(0x002A94C0u, [](X86Context& c) { // TIF_Math::Matrix4Transform(dst, mat, vec)
        uint32_t dstVA = Arg(c, 1), mVA = Arg(c, 2), vVA = Arg(c, 3);
        if (dstVA && mVA && vVA) {
            float m[12], v[3], out[3];
            std::memcpy(m, c.ptr(mVA), 48);
            std::memcpy(v, c.ptr(vVA), 12);
            for (int col = 0; col < 3; ++col) {
                out[col] = v[0] * m[0 + col] + v[1] * m[3 + col] + v[2] * m[6 + col] + m[9 + col];
            }
            std::memcpy(c.ptr(dstVA), out, 12);
        }
        RetStdcall(c, 12);
    });
    RegisterGuestFunc(0x002A9190u, [](X86Context& c) { // TIF_Math::Matrix3Mul(dst, a, b)
        uint32_t dstVA = Arg(c, 1), aVA = Arg(c, 2), bVA = Arg(c, 3);
        if (dstVA && aVA && bVA) {
            float a[9], b[9], out[9];
            std::memcpy(a, c.ptr(aVA), 36);
            std::memcpy(b, c.ptr(bVA), 36);
            for (int r = 0; r < 3; ++r)
                for (int col = 0; col < 3; ++col)
                    out[r * 3 + col] = a[r * 3 + 0] * b[0 + col] + a[r * 3 + 1] * b[3 + col] + a[r * 3 + 2] * b[6 + col];
            std::memcpy(c.ptr(dstVA), out, 36);
        }
        RetStdcall(c, 12);
    });
    RegisterGuestFunc(0x002A9370u, [](X86Context& c) { // TIF_Math::Matrix4x4Mul(dst, a, b)
        uint32_t dstVA = Arg(c, 1), aVA = Arg(c, 2), bVA = Arg(c, 3);
        if (dstVA && aVA && bVA) {
            float a[16], b[16], out[16];
            std::memcpy(a, c.ptr(aVA), 64);
            std::memcpy(b, c.ptr(bVA), 64);
            for (int r = 0; r < 4; ++r)
                for (int col = 0; col < 4; ++col)
                    out[r * 4 + col] = a[r * 4 + 0] * b[0 + col] + a[r * 4 + 1] * b[4 + col] +
                                       a[r * 4 + 2] * b[8 + col] + a[r * 4 + 3] * b[12 + col];
            std::memcpy(c.ptr(dstVA), out, 64);
        }
        RetStdcall(c, 12);
    });
    RegisterGuestFunc(0x002A9580u, [](X86Context& c) { // TIF_Math::Matrix3Inverse(dst, src) (orthonormal transpose)
        uint32_t dstVA = Arg(c, 1), srcVA = Arg(c, 2);
        if (dstVA && srcVA) {
            float m[9], out[9];
            std::memcpy(m, c.ptr(srcVA), 36);
            for (int r = 0; r < 3; ++r)
                for (int col = 0; col < 3; ++col)
                    out[r * 3 + col] = m[col * 3 + r];
            std::memcpy(c.ptr(dstVA), out, 36);
        }
        RetStdcall(c, 8);
    });
    RegisterGuestFunc(0x002A95C0u, [](X86Context& c) { // TIF_Math::Matrix4Inverse(dst, src) (rigid 4x3 inverse -> Camera View!)
        uint32_t dstVA = Arg(c, 1), srcVA = Arg(c, 2);
        if (dstVA && srcVA) {
            float m[12], out[12];
            std::memcpy(m, c.ptr(srcVA), 48);
            for (int r = 0; r < 3; ++r)
                for (int col = 0; col < 3; ++col)
                    out[r * 3 + col] = m[col * 3 + r];
            float tx = m[9], ty = m[10], tz = m[11];
            out[9]  = -(tx * out[0] + ty * out[3] + tz * out[6]);
            out[10] = -(tx * out[1] + ty * out[4] + tz * out[7]);
            out[11] = -(tx * out[2] + ty * out[5] + tz * out[8]);
            std::memcpy(c.ptr(dstVA), out, 48);
        }
        RetStdcall(c, 8);
    });
    RegisterGuestFunc(0x002A9650u, [](X86Context& c) { // TIF_Math::Matrix3Copy(dst, src)
        uint32_t d = Arg(c, 1), s = Arg(c, 2);
        if (d && s) std::memcpy(c.ptr(d), c.ptr(s), 36);
        RetStdcall(c, 8);
    });
    RegisterGuestFunc(0x002A9670u, [](X86Context& c) { // TIF_Math::Matrix4Copy(dst, src)
        uint32_t d = Arg(c, 1), s = Arg(c, 2);
        if (d && s) std::memcpy(c.ptr(d), c.ptr(s), 48);
        RetStdcall(c, 8);
    });
    RegisterGuestFunc(0x002A9740u, [](X86Context& c) { // TIF_Math::MASin
        double x = static_cast<double>(c.rf32(c.esp + 4u));
        c.fpuPush(std::asin(std::max(-1.0, std::min(1.0, x))));
        RetStdcall(c, 4);
    });
    RegisterGuestFunc(0x002A9790u, [](X86Context& c) { // TIF_Math::MACos
        double x = static_cast<double>(c.rf32(c.esp + 4u));
        c.fpuPush(std::acos(std::max(-1.0, std::min(1.0, x))));
        RetStdcall(c, 4);
    });
    RegisterGuestFunc(0x002A9800u, [](X86Context& c) { // TIF_Math::MPow(b, e)
        double b = static_cast<double>(c.rf32(c.esp + 4u));
        double e = static_cast<double>(c.rf32(c.esp + 8u));
        c.fpuPush(std::pow(b, e));
        RetStdcall(c, 8);
    });
    RegisterGuestFunc(0x002A98F0u, [](X86Context& c) { // TIF_Math::Matrix3TransformList(dst, mat, src, count, stride)
        uint32_t dstVA = Arg(c, 1), mVA = Arg(c, 2), srcVA = Arg(c, 3);
        int32_t count = static_cast<int32_t>(Arg(c, 4));
        uint32_t stride = Arg(c, 5);
        if (dstVA && mVA && srcVA && count > 0) {
            if (stride == 0) stride = 12u;
            float m[9];
            std::memcpy(m, c.ptr(mVA), 36);
            for (int32_t i = 0; i < count; ++i) {
                uint32_t off = static_cast<uint32_t>(i) * stride;
                float v[3], out[3];
                std::memcpy(v, c.ptr(srcVA + off), 12);
                for (int col = 0; col < 3; ++col)
                    out[col] = v[0] * m[0 + col] + v[1] * m[3 + col] + v[2] * m[6 + col];
                std::memcpy(c.ptr(dstVA + off), out, 12);
            }
        }
        RetStdcall(c, 20);
    });
    RegisterGuestFunc(0x002A9980u, [](X86Context& c) { // TIF_Math::Matrix4TransformList(dst, mat, src, count, stride)
        uint32_t dstVA = Arg(c, 1), mVA = Arg(c, 2), srcVA = Arg(c, 3);
        int32_t count = static_cast<int32_t>(Arg(c, 4));
        uint32_t stride = Arg(c, 5);
        if (dstVA && mVA && srcVA && count > 0) {
            if (stride == 0) stride = 12u;
            float m[12];
            std::memcpy(m, c.ptr(mVA), 48);
            for (int32_t i = 0; i < count; ++i) {
                uint32_t off = static_cast<uint32_t>(i) * stride;
                float v[3], out[3];
                std::memcpy(v, c.ptr(srcVA + off), 12);
                for (int col = 0; col < 3; ++col)
                    out[col] = v[0] * m[0 + col] + v[1] * m[3 + col] + v[2] * m[6 + col] + m[9 + col];
                std::memcpy(c.ptr(dstVA + off), out, 12);
            }
        }
        RetStdcall(c, 20);
    });
    RegisterGuestFunc(0x002A96E0u, [](X86Context& c) { float x = c.rf32(c.esp + 4u); c.fpuPush(x > 0.0f ? std::sqrt(static_cast<double>(x)) : 0.0); RetStdcall(c, 4); });
    RegisterGuestFunc(0x002A96F0u, [](X86Context& c) { c.fpuPush(std::sin(static_cast<double>(c.rf32(c.esp + 4u)))); RetStdcall(c, 4); });
    RegisterGuestFunc(0x002A9700u, [](X86Context& c) { c.fpuPush(std::cos(static_cast<double>(c.rf32(c.esp + 4u)))); RetStdcall(c, 4); });
    RegisterGuestFunc(0x002A9710u, [](X86Context& c) { c.fpuPush(std::tan(static_cast<double>(c.rf32(c.esp + 4u)))); RetStdcall(c, 4); });
    RegisterGuestFunc(0x002A9720u, [](X86Context& c) { // TIF_Math::MSinCos(angle, pSin, pCos)
        double a = static_cast<double>(c.rf32(c.esp + 4u));
        uint32_t pS = Arg(c, 2), pC = Arg(c, 3);
        if (pS) c.wf32(pS, static_cast<float>(std::sin(a)));
        if (pC) c.wf32(pC, static_cast<float>(std::cos(a)));
        RetStdcall(c, 12);
    });
    RegisterGuestFunc(0x002A97E0u, [](X86Context& c) { // TIF_Math::MATan(x, y) -> atan2(y, x) wrapped to [0, 2*PI)
        double x = static_cast<double>(c.rf32(c.esp + 4u));
        double y = static_cast<double>(c.rf32(c.esp + 8u));
        double r = std::atan2(y, x);
        if (r < 0.0) r += 6.283185307179586;
        c.fpuPush(r);
        RetStdcall(c, 8);
    });
    RegisterGuestFunc(0x002AA780u, [](X86Context& c) { // cvtColorSource(TColor* dst, TRGBA* src, long count) (SSE/MMX!)
        uint32_t dstVA = Arg(c, 1), srcVA = Arg(c, 2);
        int32_t count  = static_cast<int32_t>(Arg(c, 3));
        if (dstVA && srcVA && count > 0) {
            float sc[4], lo[4];
            std::memcpy(sc, c.ptr(0x00771EA4u), 16);
            std::memcpy(lo, c.ptr(0x00771EB4u), 16);
            for (int32_t i = 0; i < count; ++i) {
                float f[4];
                std::memcpy(f, c.ptr(srcVA + static_cast<uint32_t>(i) * 16u), 16);
                // shufps 0x1B reverses (f0,f1,f2,f3) -> (f3,f2,f1,f0), then *sc, clamp [lo, sc], pack u8
                uint8_t b[4];
                for (int k = 0; k < 4; ++k) {
                    float v = f[3 - k] * sc[k];
                    if (v > sc[k]) v = sc[k];
                    if (v < lo[k]) v = lo[k];
                    int iv = static_cast<int>(v + 0.5f);
                    b[k] = static_cast<uint8_t>(iv < 0 ? 0 : (iv > 255 ? 255 : iv));
                }
                uint32_t packed = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
                                  (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
                c.w32(dstVA + static_cast<uint32_t>(i) * 4u, packed);
            }
        }
        RetCdecl(c);
    });
    RegisterGuestFunc(0x00034FB0u, [](X86Context& c) { // Float2SINTABLE(long* dst, float* src, long count) (SSE/MMX!)
        uint32_t dstVA = Arg(c, 1), srcVA = Arg(c, 2);
        int32_t count  = static_cast<int32_t>(Arg(c, 3));
        if (dstVA && srcVA && count > 0) {
            float sc = c.rf32(0x006192F8u);
            for (int32_t i = 0; i < count; ++i) {
                float v = c.rf32(srcVA + static_cast<uint32_t>(i) * 4u) * sc;
                c.w32(dstVA + static_cast<uint32_t>(i) * 4u, static_cast<uint32_t>(static_cast<int32_t>(std::lrint(v))));
            }
        }
        RetCdecl(c);
    });

    // TTimerManager (0x002AC510..0x002AC740) now uses exact 1 MHz steady_clock directly in recomp_chunk_013.cpp!

    // XAPI Video / Audio / LaunchInfo (how Buffy transitions between Main Menu & Levels!)
    RegisterGuestFunc(0x00579052u, [](X86Context& c) { // _XGetLaunchInfo@8(pdwLaunchDataType, pLaunchData)
        uint32_t pType = Arg(c, 1), pData = Arg(c, 2);
        if (!g_overrideMap.empty()) {
            uint8_t buf[3072]{};
            std::snprintf(reinterpret_cast<char*>(buf), sizeof(buf), "map:%s", g_overrideMap.c_str());
            g_overrideMap.clear();
            if (pType) c.w32(pType, 0u); // LDT_TITLE
            if (pData) std::memcpy(c.ptr(pData), buf, sizeof(buf));
            std::printf("[UWP-XAPI] XGetLaunchInfo -> Loaded CLI --map ('%.128s')\n", reinterpret_cast<const char*>(buf));
            c.eax = 0u;
            RetStdcall(c, 8);
            return;
        }
        if (g_hasInProcessLaunchData) {
            g_hasInProcessLaunchData = false;
            if (pType) c.w32(pType, 0u); // LDT_TITLE
            if (pData) std::memcpy(c.ptr(pData), g_inProcessLaunchData, sizeof(g_inProcessLaunchData));
            std::printf("[UWP-XAPI] XGetLaunchInfo -> Loaded in-process LAUNCH_DATA ('%.128s')\n",
                        reinterpret_cast<const char*>(g_inProcessLaunchData));
            c.eax = 0u;
            RetStdcall(c, 8);
            return;
        }
        std::string relaunchPath = g_tRoot + "\\relaunch_data.bin";
        FILE* f = std::fopen(relaunchPath.c_str(), "rb");
        if (f) {
            uint8_t buf[3072]{};
            std::fread(buf, 1, sizeof(buf), f);
            std::fclose(f);
            DeleteFileA(relaunchPath.c_str());
            if (pType) c.w32(pType, 0u); // LDT_TITLE (launched by Default.xbe)
            if (pData) std::memcpy(c.ptr(pData), buf, sizeof(buf));
            std::printf("[UWP-XAPI] XGetLaunchInfo -> Loaded relaunch LAUNCH_DATA ('%.128s')\n",
                        reinterpret_cast<const char*>(buf));
            c.eax = 0u; // ERROR_SUCCESS
        } else {
            if (pType) c.w32(pType, 3u); // LDT_NONE / from Dashboard
            if (pData) std::memset(c.ptr(pData), 0, 3072);
            c.eax = ERROR_NOT_FOUND;
        }
        RetStdcall(c, 8);
    });
    RegisterGuestFunc(0x005790FBu, [](X86Context& c) { // _XLaunchNewImageA@8(lpTitlePath, pLaunchData)
        const char* title = Arg(c, 1) ? reinterpret_cast<const char*>(c.ptr(Arg(c, 1))) : "(null)";
        uint32_t pData = Arg(c, 2);
        std::printf("[UWP-XAPI] XLaunchNewImageA('%s', pLaunchData=0x%08X) -> In-Process Relaunch!\n", title, pData);
        if (pData) {
            std::memcpy(g_inProcessLaunchData, c.ptr(pData), sizeof(g_inProcessLaunchData));
            g_hasInProcessLaunchData = true;
            std::printf("[UWP-XAPI] LAUNCH_DATA payload: '%.128s'\n", reinterpret_cast<const char*>(g_inProcessLaunchData));
        } else {
            std::memset(g_inProcessLaunchData, 0, sizeof(g_inProcessLaunchData));
            g_hasInProcessLaunchData = false;
        }
        config_ui::Save();
        g_relaunchEpoch.fetch_add(1u, std::memory_order_acq_rel);
        std::fflush(stdout);
        throw GuestRelaunchException{};
    });
    RegisterGuestFunc(0x005786DBu, [](X86Context& c) { c.eax = 1u; RetStdcall(c, 4); });              // XMountUtilityDrive
    RegisterGuestFunc(0x005787E0u, [](X86Context& c) { c.eax = 1u; RetStdcall(c, 0); });              // XFormatUtilityDrive
    RegisterGuestFunc(0x0057FB63u, [](X86Context& c) { // __mkdir
        const char* path = reinterpret_cast<const char*>(c.ptr(Arg(c, 1)));
        CreateDirectoryA(TranslateGuestPath(path).c_str(), nullptr);
        c.eax = 0u;
        RetCdecl(c);
    });
    RegisterGuestFunc(0x0057F680u, [](X86Context& c) { // __chkstk / _alloca_probe (EAX = stack bytes)
        uint32_t allocBytes = c.eax;
        c.esp += 4u;         // pop return address
        c.esp -= allocBytes; // reserve stack frame on guest stack
    });
    RegisterGuestFunc(0x0057F32Bu, [](X86Context& c) { // __findfirst(filespec, _finddata_t*)
        const char* spec = reinterpret_cast<const char*>(c.ptr(Arg(c, 1)));
        uint32_t pData   = Arg(c, 2);
        std::string host = TranslateGuestPath(spec, false);
        WIN32_FIND_DATAA fd{};
        HANDLE hf = FindFirstFileA(host.c_str(), &fd);
        if (hf == INVALID_HANDLE_VALUE) {
            c.eax = 0xFFFFFFFFu;
        } else {
            auto toUnix = [](const FILETIME& ft) -> uint32_t {
                uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
                return (t > 116444736000000000ULL) ? static_cast<uint32_t>((t - 116444736000000000ULL) / 10000000ULL) : 0u;
            };
            if (pData) {
                uint8_t* dst = c.ptr(pData);
                std::memset(dst, 0, 280);
                uint32_t attr = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0x10u : 0x00u;
                c.w32(pData + 0x00, attr);
                c.w32(pData + 0x04, toUnix(fd.ftCreationTime));
                c.w32(pData + 0x08, toUnix(fd.ftLastAccessTime));
                c.w32(pData + 0x0C, toUnix(fd.ftLastWriteTime));
                c.w32(pData + 0x10, fd.nFileSizeLow);
                std::strncpy(reinterpret_cast<char*>(dst + 0x14), fd.cFileName, 259);
            }
            uint32_t id = g_nextFind++;
            g_findHandles[id] = hf;
            c.eax = id;
        }
        RetCdecl(c);
    });
    RegisterGuestFunc(0x0057F404u, [](X86Context& c) { // __findnext(handle, _finddata_t*)
        uint32_t id    = Arg(c, 1);
        uint32_t pData = Arg(c, 2);
        auto it = g_findHandles.find(id);
        WIN32_FIND_DATAA fd{};
        if (it == g_findHandles.end() || !FindNextFileA(it->second, &fd)) {
            c.eax = 0xFFFFFFFFu;
        } else {
            auto toUnix = [](const FILETIME& ft) -> uint32_t {
                uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
                return (t > 116444736000000000ULL) ? static_cast<uint32_t>((t - 116444736000000000ULL) / 10000000ULL) : 0u;
            };
            if (pData) {
                uint8_t* dst = c.ptr(pData);
                std::memset(dst, 0, 280);
                uint32_t attr = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0x10u : 0x00u;
                c.w32(pData + 0x00, attr);
                c.w32(pData + 0x04, toUnix(fd.ftCreationTime));
                c.w32(pData + 0x08, toUnix(fd.ftLastAccessTime));
                c.w32(pData + 0x0C, toUnix(fd.ftLastWriteTime));
                c.w32(pData + 0x10, fd.nFileSizeLow);
                std::strncpy(reinterpret_cast<char*>(dst + 0x14), fd.cFileName, 259);
            }
            c.eax = 0u;
        }
        RetCdecl(c);
    });
    RegisterGuestFunc(0x005A6D18u, [](X86Context& c) { c.eax = 0u; RetStdcall(c, 4); }); // _IDirectSound_Release@4
    RegisterGuestFunc(0x006105E3u, [](X86Context& c) { RetStdcall(c, 4); });              // _XInputClose@4
    RegisterGuestFunc(0x00576ED9u, [](X86Context& c) { c.eax = 0x51600001u; RetStdcall(c, 4); }); // _XCalculateSignatureBegin@4
    RegisterGuestFunc(0x00576E62u, [](X86Context& c) { c.eax = 0u; RetStdcall(c, 12); });         // _XCalculateSignatureUpdate@12 (ERROR_SUCCESS)
    RegisterGuestFunc(0x00576E7Cu, [](X86Context& c) {                                            // _XCalculateSignatureEnd@8
        uint32_t pSig = Arg(c, 2);
        if (pSig) std::memset(c.ptr(pSig), 0x42, 20); // 20-byte XCALCSIG_SIGNATURE
        c.eax = 0u; // ERROR_SUCCESS
        RetStdcall(c, 8);
    });
    RegisterGuestFunc(0x005A5B13u, [](X86Context& c) { // _XGSwizzleRect@32(pSrc, pitch, pRect, pDst, w, h, pPt, bpp)
        uint32_t pSrcVA  = Arg(c, 1);
        uint32_t pitch   = Arg(c, 2);
        uint32_t pRectVA = Arg(c, 3);
        uint32_t pDstVA  = Arg(c, 4);
        uint32_t w       = Arg(c, 5);
        uint32_t h       = Arg(c, 6);
        uint32_t pPtVA   = Arg(c, 7);
        uint32_t bpp     = Arg(c, 8);
        if (pSrcVA >= 0x00010000u && pDstVA >= 0x00010000u && pDstVA < kXboxRamSize &&
            w > 0 && h > 0 && w <= 2048 && h <= 2048 && (bpp == 1 || bpp == 2 || bpp == 4)) {
            int32_t srcL = 0, srcT = 0, srcR = static_cast<int32_t>(w), srcB = static_cast<int32_t>(h);
            if (pRectVA >= 0x00010000u && pRectVA + 16u <= kXboxRamSize) {
                srcL = static_cast<int32_t>(c.r32(pRectVA + 0u));
                srcT = static_cast<int32_t>(c.r32(pRectVA + 4u));
                srcR = static_cast<int32_t>(c.r32(pRectVA + 8u));
                srcB = static_cast<int32_t>(c.r32(pRectVA + 12u));
            }
            int32_t dstX0 = 0, dstY0 = 0;
            if (pPtVA >= 0x00010000u && pPtVA + 8u <= kXboxRamSize) {
                dstX0 = static_cast<int32_t>(c.r32(pPtVA + 0u));
                dstY0 = static_cast<int32_t>(c.r32(pPtVA + 4u));
            }
            int32_t copyW = srcR - srcL;
            int32_t copyH = srcB - srcT;
            if (copyW > 0 && copyH > 0) {
                const uint8_t* src = c.ptr(pSrcVA);
                uint8_t* dst = c.ptr(pDstVA);
                uint32_t srcPitch = pitch ? pitch : (static_cast<uint32_t>(copyW) * bpp);
                uint32_t maskU = 0, maskV = 0;
                uint32_t i = 1, j = 1;
                while (i < w || i < h) {
                    if (i < w) { maskU |= j; j <<= 1; }
                    if (i < h) { maskV |= j; j <<= 1; }
                    i <<= 1;
                }
                auto spreadBits = [](uint32_t coord, uint32_t mask) -> uint32_t {
                    uint32_t res = 0, bit = 1;
                    for (uint32_t m = mask; m; m &= (m - 1u)) {
                        uint32_t lsb = m & (0u - m);
                        if (coord & bit) res |= lsb;
                        bit <<= 1;
                    }
                    return res;
                };
                uint32_t uStart = spreadBits(static_cast<uint32_t>(std::max(0, dstX0)), maskU);
                for (int32_t ry = 0; ry < copyH; ++ry) {
                    int32_t dy = dstY0 + ry;
                    if (dy < 0 || static_cast<uint32_t>(dy) >= h) continue;
                    uint32_t v = spreadBits(static_cast<uint32_t>(dy), maskV);
                    uint32_t u = uStart;
                    const uint8_t* row = src + static_cast<size_t>(srcT + ry) * srcPitch + static_cast<size_t>(srcL) * bpp;
                    for (int32_t rx = 0; rx < copyW; ++rx) {
                        int32_t dx = dstX0 + rx;
                        if (dx >= 0 && static_cast<uint32_t>(dx) < w) {
                            std::memcpy(dst + (u | v) * bpp, row + static_cast<size_t>(rx) * bpp, bpp);
                        }
                        u = (u - maskU) & maskU;
                    }
                }
                d3d11_bridge::InvalidateTexture(pDstVA);
            }
        }
        RetStdcall(c, 32);
    });
    RegisterGuestFunc(0x00594720u, [](X86Context& c) { // _D3DDevice_CreateVertexShader@16(pDecl, pFunc, pHandle, usage)
        static uint32_t s_nextVs = 0x1001u;
        uint32_t pDecl   = Arg(c, 1);
        uint32_t pFunc   = Arg(c, 2);
        uint32_t pHandle = Arg(c, 3);
        uint32_t h       = s_nextVs;
        s_nextVs += 2u; // keep LSB=1 (Xbox programmable/custom VS handle convention)
        if (pDecl) {
            d3d11_bridge::RegisterVertexShaderDecl(h, reinterpret_cast<const uint32_t*>(c.ptr(pDecl)), pFunc != 0);
        }
        if (pHandle) c.w32(pHandle, h);
        c.eax = 0u;
        RetStdcall(c, 16);
    });
    RegisterGuestFunc(0x00579D58u, [](X86Context& c) { c.eax = 1u; RetStdcall(c, 0); });              // XGetVideoStandard (NTSC-M / 60Hz)
    RegisterGuestFunc(0x00579D81u, [](X86Context& c) { c.eax = 0x00010001u; RetStdcall(c, 0); });     // XGetVideoFlags (Widescreen + 480p)
    RegisterGuestFunc(0x00579DCAu, [](X86Context& c) { c.eax = 0u; RetStdcall(c, 0); });              // XGetAudioFlags (Stereo)

    // Native Slate Engine Hor+ Widescreen FOV & Frustum Culling hook (TIF_Render_Implement::View_SetFOV @ 0x002A5110)
    // Matches recomp/src/xbe_loader.cpp so frustum culling, portals, skybox & parallax match 16:9 / 21:9!
    RegisterGuestFunc(0x002A5110u, [](X86Context& c) {
        uint32_t thisVA = c.ecx;
        float fovX = c.rf32(c.esp + 4u);
        uint32_t vpW = c.r32(0x007F425Cu);
        if (vpW == 0u) {
            c.wf32(0x007F4248u, fovX * 0.5f);
            c.eax = 0u;
            RetStdcall(c, 4);
            return;
        }
        float targetAspect = 4.0f / 3.0f;
        if (d3d11_bridge::GetWidescreenHorPlus() && vpW > 160u) {
            uint32_t w = d3d11_bridge::GetWidth(), h = d3d11_bridge::GetHeight();
            if (w > 0 && h > 0) {
                float a = static_cast<float>(w) / static_cast<float>(h);
                if (a > targetAspect) targetAspect = a;
            }
        }
        float halfFov43 = fovX * 0.5f;
        float tanHalfY  = std::tan(halfFov43) * 0.75f;
        float halfFovY  = std::atan(tanHalfY);
        float halfFovX  = std::atan(tanHalfY * targetAspect);
        if (halfFovX > 1.48f) halfFovX = 1.48f;
        c.wf32(0x007F4248u, halfFovX);
        c.wf32(0x007F424Cu, halfFovY);
        // Call TIF_Render_Implement::View_InternalSetup(0, 1) @ 0x002A4870 (thiscall, 2 args)
        c.ecx = thisVA;
        c.push32(1u);
        c.push32(0u);
        c.push32(kReturnSentinelVA);
        CallGuestVA(c, 0x002A4870u);
        c.eax = 1u;
        RetStdcall(c, 4);
    });

    // D3D8 CreateDevice & Synchronization Stubs (with real D3D::g_Device pushbuffer & power-on defaults!)
    RegisterGuestFunc(0x005966E0u, [](X86Context& c) { // Direct3D_CreateDevice
        uint32_t ppOut = Arg(c, 6);
        constexpr uint32_t kPushBytes = 256u * 1024u;
        if (!g_pushBufVA) g_pushBufVA = GuestHeapAlloc(kPushBytes, 64u);
        constexpr uint32_t kDevVA = 0x0059FEA0u; // D3D::g_Device
        std::memset(c.ptr(kDevVA), 0, 0x2AF8u);
        c.w32(kDevVA + 0x00u, g_pushBufVA);
        c.w32(kDevVA + 0x04u, g_pushBufVA + kPushBytes - 4096u);
        c.w32(kDevVA + 0x24u, g_pushBufVA);
        c.w32(kDevVA + 0x28u, g_pushBufVA + kPushBytes);
        c.w32(kDevVA + 0x0380u, 0x0059FD78u); // g_FvfVertexShader
        if (!g_fakeBbSurfVA) {
            g_fakeBbSurfVA = GuestHeapAlloc(32u, 16u);
            if (g_fakeBbSurfVA) {
                std::memset(c.ptr(g_fakeBbSurfVA), 0, 32);
                c.w32(g_fakeBbSurfVA + 0u,  0x00030001u);
                c.w32(g_fakeBbSurfVA + 4u,  g_pushBufVA);
                c.w32(g_fakeBbSurfVA + 12u, 0x00011E29u);
                c.w32(g_fakeBbSurfVA + 16u, (((640u * 4u / 64u) - 1u) << 24) | ((480u - 1u) << 12) | (640u - 1u));
            }
        }
        c.w32(kDevVA + 0x2070u, g_fakeBbSurfVA); // m_pRenderTarget
        c.w32(kDevVA + 0x2074u, g_fakeBbSurfVA); // m_pZBuffer
        c.w32(kDevVA + 0x2078u, g_fakeBbSurfVA); // m_pAutoDepthStencil
        c.w32(kDevVA + 0x207Cu, g_fakeBbSurfVA); // m_pBackBuffer
        c.w32(kDevVA + 0x2080u, g_fakeBbSurfVA); // m_pFrontBuffer
        c.w32(kDevVA + 0x2088u, g_fakeBbSurfVA);
        c.w32(0x005A29A0u, kDevVA);           // D3D::g_pDevice
        c.w32(0x005A29A4u, kDevVA);           // _g_pPushBuffer
        c.w32(0x005A02E0u, 1u);

        // Power-on Xbox D3D8 render & texture state defaults
        uint32_t* rs = reinterpret_cast<uint32_t*>(c.ptr(0x0059F940u));
        std::memset(rs, 0, 150 * sizeof(uint32_t));
        rs[57]  = 0x203u;      // ZFUNC = LESSEQUAL
        rs[58]  = 0x207u;      // ALPHAFUNC = ALWAYS
        rs[59]  = 0u;          // ALPHABLENDENABLE = FALSE
        rs[60]  = 0u;          // ALPHATESTENABLE = FALSE
        rs[62]  = 1u;          // SRCBLEND = ONE
        rs[63]  = 0u;          // DESTBLEND = ZERO
        rs[64]  = 1u;          // ZWRITEENABLE = TRUE
        rs[125] = 1u;          // ZENABLE = TRUE
        rs[129] = 0x901u;      // CULLMODE = CCW
        rs[130] = 0xFFFFFFFFu; // TEXTUREFACTOR = white
        uint32_t* ts = reinterpret_cast<uint32_t*>(c.ptr(0x0059F740u));
        std::memset(ts, 0, 4 * 32 * sizeof(uint32_t));
        for (uint32_t s = 0; s < 4u; ++s) {
            uint32_t* st = ts + s * 32u;
            st[0] = 1u; st[1] = 1u; st[3] = 2u; st[4] = 2u;
            st[12] = (s == 0u) ? 4u : 1u; // COLOROP = MODULATE on stage 0, DISABLE on 1..3
            st[14] = 2u; st[15] = 0u;     // COLORARG1 = TEXTURE, COLORARG2 = DIFFUSE
            st[16] = (s == 0u) ? 4u : 1u; // ALPHAOP
            st[18] = 2u; st[19] = 0u;     // ALPHAARG1 = TEXTURE, ALPHAARG2 = DIFFUSE
            st[28] = s;
        }
        if (ppOut) c.w32(ppOut, kDevVA);
        c.eax = 0u; // D3D_OK
        RetStdcall(c, 24);
    });
    RegisterGuestFunc(0x00596F50u, [](X86Context& c) { // MakeRequestedSpace -> reset pushbuffer & return valid VA!
        if (g_pushBufVA) {
            c.w32(0x0059FEA0u + 0x00u, g_pushBufVA);
            c.w32(0x0059FEA0u + 0x04u, g_pushBufVA + (256u * 1024u - 4096u));
        }
        c.eax = g_pushBufVA;
        RetStdcall(c, 8);
    });
    RegisterGuestFunc(0x005970D0u, [](X86Context& c) { // MakeSpace -> reset pushbuffer & return valid VA!
        if (g_pushBufVA) {
            c.w32(0x0059FEA0u + 0x00u, g_pushBufVA);
            c.w32(0x0059FEA0u + 0x04u, g_pushBufVA + (256u * 1024u - 4096u));
        }
        c.eax = g_pushBufVA;
        RetStdcall(c, 0);
    });
    RegisterGuestFunc(0x00596CC0u, [](X86Context& c) { RetStdcall(c, 0); });              // KickOff
    RegisterGuestFunc(0x00596DB0u, [](X86Context& c) { c.eax = 1u; RetStdcall(c, 4); });  // SetFence
    RegisterGuestFunc(0x00596E60u, [](X86Context& c) { RetStdcall(c, 8); });              // BlockOnTime
    RegisterGuestFunc(0x00597130u, [](X86Context& c) { RetStdcall(c, 0); });              // KickOffAndWaitForIdle
    RegisterGuestFunc(0x00597150u, [](X86Context& c) { RetStdcall(c, 4); });              // BlockOnResource
    RegisterGuestFunc(0x005971D0u, [](X86Context& c) { RetStdcall(c, 4); });              // BlockOnResource
    RegisterGuestFunc(0x00594F00u, [](X86Context& c) { RetStdcall(c, 4); });              // D3DResource_BlockUntilNotBusy
    RegisterGuestFunc(0x005950D0u, [](X86Context& c) { c.eax = 0u; RetStdcall(c, 4); });  // D3DResource_IsBusy
    RegisterGuestFunc(0x005937E0u, [](X86Context& c) { c.eax = 0u; RetStdcall(c, 0); });  // D3DDevice_IsBusy
    RegisterGuestFunc(0x00592930u, [](X86Context& c) { RetStdcall(c, 0); });              // BlockUntilVerticalBlank
    RegisterGuestFunc(0x00592AF0u, [](X86Context& c) { c.eax = 0u; RetStdcall(c, 8); });  // SetRenderTarget
    RegisterGuestFunc(0x005926E0u, [](X86Context& c) { RetStdcall(c, 8); });              // SetGammaRamp
    RegisterGuestFunc(0x00592960u, [](X86Context& c) { RetStdcall(c, 4); });              // SetFlickerFilter
    RegisterGuestFunc(0x005929B0u, [](X86Context& c) { RetStdcall(c, 4); });              // SetSoftDisplayFilter
    RegisterGuestFunc(0x00593110u, [](X86Context& c) { c.eax = 0u; RetStdcall(c, 20); }); // CopyRects

    // ------------------------------------------------------------------------
    // 5. D3D8 -> D3D11 Full Draw, Texture, Transform & Present Bridges
    // ------------------------------------------------------------------------
    RegisterGuestFunc(0x00595E70u, [](X86Context& c) { // D3DDevice_Clear(count, pRects, flags, color, z, stencil)
        uint32_t flags = Arg(c, 3);
        uint32_t color = Arg(c, 4);
        float z        = c.rf32(c.esp + 20u);
        d3d11_bridge::Clear(flags, color, z);
        RetStdcall(c, 24);
    });
    RegisterGuestFunc(0x00595DB0u, [](X86Context& c) { // D3DDevice_Swap
        if (g_pushBufVA) c.w32(0x0059FEA0u + 0x00u, g_pushBufVA);
        d3d11_bridge::Present(true);
        RetStdcall(c, 4);
    });
    RegisterGuestFunc(0x005933E0u, [](X86Context& c) { // D3DDevice_SetViewport
        uint32_t vpVA = Arg(c, 1);
        if (vpVA) {
            d3d11_bridge::SetViewport(c.r32(vpVA), c.r32(vpVA + 4), c.r32(vpVA + 8), c.r32(vpVA + 12),
                                      c.rf32(vpVA + 16), c.rf32(vpVA + 20));
        }
        RetStdcall(c, 4);
    });
    RegisterGuestFunc(0x005927E0u, [](X86Context& c) { // D3DDevice_SetTransform
        uint32_t st = Arg(c, 1);
        uint32_t mVA = Arg(c, 2);
        if (mVA) d3d11_bridge::SetTransform(st, reinterpret_cast<const float*>(c.ptr(mVA)));
        RetStdcall(c, 8);
    });
    RegisterGuestFunc(0x00594C20u, [](X86Context& c) { // D3DDevice_SetVertexShader
        d3d11_bridge::SetVertexShader(Arg(c, 1));
        RetStdcall(c, 4);
    });
    RegisterGuestFunc(0x00594860u, [](X86Context& c) { // D3DDevice_SetStreamSource
        d3d11_bridge::SetStreamSource(Arg(c, 1), c.mem, Arg(c, 2), Arg(c, 3));
        RetStdcall(c, 12);
    });
    RegisterGuestFunc(0x00593750u, [](X86Context& c) { // D3DDevice_SetIndices + mirror _D3D__IndexData (0x0059F734)!
        uint32_t ibVA = Arg(c, 1);
        uint32_t dataVA = (ibVA >= 0x00010000u && ibVA < kXboxRamSize) ? c.r32(ibVA + 4u) : 0u;
        c.w32(0x0059F734u, dataVA); // Inline DrawIndexedPrimitive reads _D3D__IndexData directly!
        d3d11_bridge::SetIndices(c.mem, ibVA, Arg(c, 2));
        RetStdcall(c, 8);
    });
    RegisterGuestFunc(0x00593540u, [](X86Context& c) { // D3DDevice_SetTexture
        d3d11_bridge::SetTexture(Arg(c, 1), c.mem, Arg(c, 2));
        RetStdcall(c, 8);
    });
    RegisterGuestFunc(0x00593D20u, [](X86Context& c) { // D3DDevice_DrawVerticesUP
        uint32_t pt = Arg(c, 1), cnt = Arg(c, 2), vVA = Arg(c, 3), str = Arg(c, 4);
        if (vVA) d3d11_bridge::DrawVerticesUP(pt, cnt, c.ptr(vVA), str);
        RetStdcall(c, 16);
    });
    RegisterGuestFunc(0x00593E80u, [](X86Context& c) { // D3DDevice_DrawIndexedVerticesUP
        uint32_t pt = Arg(c, 1), cnt = Arg(c, 2), iVA = Arg(c, 3), vVA = Arg(c, 4), str = Arg(c, 5);
        if (iVA && vVA) {
            d3d11_bridge::DrawIndexedVerticesUP(pt, cnt, reinterpret_cast<const uint16_t*>(c.ptr(iVA)), c.ptr(vVA), str);
        }
        RetStdcall(c, 20);
    });
    RegisterGuestFunc(0x00593FC0u, [](X86Context& c) { // D3DDevice_DrawIndexedVertices
        d3d11_bridge::DrawIndexedVertices(Arg(c, 1), Arg(c, 2), c.mem, Arg(c, 3));
        RetStdcall(c, 12);
    });

    std::printf("[UWP-Bridges] Registered core portable x64 host bridges (Heap, VFS, XAPI, D3D11).\n");
}

} // namespace host_bridges
} // namespace uwp_recomp
