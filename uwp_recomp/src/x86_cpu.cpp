#include "x86_cpu.h"
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace uwp_recomp {

static uint8_t* g_guestRam = nullptr;
static std::mutex g_heapMtx;
// True O(log F) best-fit allocator + O(1) free-byte tracking (zero linear scans on alloc OR free!)
static std::map<uint32_t, uint32_t> g_freeByAddr;                // freeAddr -> freeSize
static std::set<std::pair<uint32_t, uint32_t>> g_freeBySize;     // (freeSize, freeAddr)
static std::unordered_map<uint32_t, uint32_t> g_allocMap;        // allocAddr -> allocSize
static uint32_t g_totalFreeBytes = 0;
static uint64_t g_totalAllocCalls = 0;
static uint64_t g_totalFreeCalls  = 0;
static std::unordered_map<uint32_t, GuestFunc> g_funcTable;
static constexpr uint32_t kFastFuncLimit = 0x00620000u;
static GuestFunc* g_fastFuncTable = nullptr;
static std::unordered_set<uint32_t> g_loggedMissingVAs;
static uint32_t g_recentCalls[16] = {};
static uint32_t g_recentCallIdx   = 0;

// Full Xbox-equivalent 51.5 MB dynamic heap (Default.xbe ends at 0x00920D40; main stack uses 0x03D00000..0x03F00000)
static constexpr uint32_t kDynHeapBase  = 0x00980000u;
static constexpr uint32_t kDynHeapLimit = 0x03D00000u;

static void FreeInsert(uint32_t addr, uint32_t sz) {
    g_freeByAddr[addr] = sz;
    g_freeBySize.emplace(sz, addr);
}

static void FreeErase(uint32_t addr, uint32_t sz) {
    g_freeByAddr.erase(addr);
    g_freeBySize.erase(std::make_pair(sz, addr));
}

bool InitGuestMemory() {
    if (g_guestRam) return true;
    g_guestRam = static_cast<uint8_t*>(std::calloc(1, kXboxRamSize));
    std::lock_guard<std::mutex> lk(g_heapMtx);
    g_freeByAddr.clear();
    g_freeBySize.clear();
    g_allocMap.clear();
    g_totalFreeBytes = kDynHeapLimit - kDynHeapBase;
    FreeInsert(kDynHeapBase, g_totalFreeBytes);
    return g_guestRam != nullptr;
}

void ShutdownGuestMemory() {
    if (g_guestRam) {
        std::free(g_guestRam);
        g_guestRam = nullptr;
    }
    if (g_fastFuncTable) {
        std::free(g_fastFuncTable);
        g_fastFuncTable = nullptr;
    }
    g_funcTable.clear();
    g_freeByAddr.clear();
    g_freeBySize.clear();
    g_allocMap.clear();
    g_totalFreeBytes = 0;
}

uint8_t* GetGuestRamBase() {
    return g_guestRam;
}

uint32_t GuestHeapAlloc(uint32_t size, uint32_t align) {
    if (size == 0) size = 16u;
    if (align < 16u) align = 16u;
    if (size > (kDynHeapLimit - kDynHeapBase)) return 0;
    size = (size + 15u) & ~15u;
    std::lock_guard<std::mutex> lk(g_heapMtx);

    // True O(log F) best-fit lookup: jump straight to holes >= size (almost always hits on 1st or 2nd entry!)
    auto sit = g_freeBySize.lower_bound(std::make_pair(size, 0u));
    while (sit != g_freeBySize.end()) {
        uint32_t bSize = sit->first;
        uint32_t bAddr = sit->second;
        uint32_t aligned = (bAddr + (align - 1u)) & ~(align - 1u);
        uint32_t pad = aligned - bAddr;
        if (bSize >= pad + size) {
            uint32_t rem = bSize - (pad + size);
            g_freeBySize.erase(sit);
            g_freeByAddr.erase(bAddr);
            if (pad > 0) FreeInsert(bAddr, pad);
            if (rem > 0) FreeInsert(aligned + size, rem);
            g_allocMap[aligned] = size;
            g_totalFreeBytes -= size;
            ++g_totalAllocCalls;
            return aligned;
        }
        ++sit;
    }
    uint32_t largestFree = g_freeBySize.empty() ? 0u : g_freeBySize.rbegin()->first;
    static uint32_t s_oomCount = 0;
    if (++s_oomCount <= 10u) {
        std::printf("[UWP-HEAP] OOM! req=%u align=%u totalFree=%u largestFree=%u allocs=%zu holes=%zu\n",
                    size, align, g_totalFreeBytes, largestFree, g_allocMap.size(), g_freeByAddr.size());
    }
    return 0;
}

void GuestHeapFree(uint32_t va) {
    if (va < kDynHeapBase || va >= kDynHeapLimit) return;
    std::lock_guard<std::mutex> lk(g_heapMtx);
    auto ait = g_allocMap.find(va);
    if (ait == g_allocMap.end()) return;
    uint32_t addr = ait->first;
    uint32_t size = ait->second;
    g_allocMap.erase(ait);
    g_totalFreeBytes += size;
    ++g_totalFreeCalls;

    // Coalesce with next free hole
    auto nextIt = g_freeByAddr.lower_bound(addr);
    if (nextIt != g_freeByAddr.end() && addr + size == nextIt->first) {
        uint32_t nAddr = nextIt->first, nSz = nextIt->second;
        FreeErase(nAddr, nSz);
        size += nSz;
    }
    // Coalesce with previous free hole
    nextIt = g_freeByAddr.lower_bound(addr);
    if (nextIt != g_freeByAddr.begin()) {
        auto prevIt = std::prev(nextIt);
        if (prevIt->first + prevIt->second == addr) {
            uint32_t pAddr = prevIt->first, pSz = prevIt->second;
            FreeErase(pAddr, pSz);
            addr = pAddr;
            size += pSz;
        }
    }
    FreeInsert(addr, size);
}

uint32_t GuestHeapSize(uint32_t va) {
    std::lock_guard<std::mutex> lk(g_heapMtx);
    auto it = g_allocMap.find(va);
    return (it != g_allocMap.end()) ? it->second : 0u;
}

bool IsGuestHeapAddr(uint32_t va) {
    return va >= kDynHeapBase && va < kDynHeapLimit;
}

uint32_t GuestHeapFreeBytes() {
    std::lock_guard<std::mutex> lk(g_heapMtx);
    return g_totalFreeBytes;
}

static std::vector<uint8_t> g_cachedXbeRaw;

static bool MapCachedXbeIntoGuestRam(uint32_t& outEntryVA) {
    if (!g_guestRam || g_cachedXbeRaw.size() < 0x180) return false;
    const auto& raw = g_cachedXbeRaw;
    if (std::memcmp(raw.data(), "XBEH", 4) != 0) return false;
    auto rd32 = [&](size_t off) -> uint32_t {
        uint32_t v = 0;
        std::memcpy(&v, raw.data() + off, 4);
        return v;
    };
    uint32_t baseAddr     = rd32(0x104);
    uint32_t sizeOfHdrs   = rd32(0x108);
    uint32_t sizeOfImage  = rd32(0x10C);
    uint32_t entryEnc     = rd32(0x128);
    uint32_t numSections  = rd32(0x11C);
    uint32_t secHdrsVA    = rd32(0x120);

    outEntryVA = entryEnc ^ 0xA8FC57ABu;
    if (outEntryVA < baseAddr || outEntryVA >= baseAddr + sizeOfImage) {
        outEntryVA = entryEnc ^ 0x94859D4Bu;
    }
    if (baseAddr + sizeOfHdrs < kXboxRamSize && sizeOfHdrs <= raw.size()) {
        std::memcpy(g_guestRam + baseAddr, raw.data(), sizeOfHdrs);
    }
    uint32_t secHdrsOff = secHdrsVA - baseAddr;
    for (uint32_t i = 0; i < numSections; ++i) {
        size_t sOff = static_cast<size_t>(secHdrsOff) + i * 56u;
        if (sOff + 56u > raw.size()) break;
        uint32_t vAddr = rd32(sOff + 4);
        uint32_t vSize = rd32(sOff + 8);
        uint32_t rAddr = rd32(sOff + 12);
        uint32_t rSize = rd32(sOff + 16);
        if (vAddr + vSize < kXboxRamSize) {
            std::memset(g_guestRam + vAddr, 0, vSize);
            if (rSize > 0 && static_cast<size_t>(rAddr) + rSize <= raw.size()) {
                uint32_t copyLen = (rSize < vSize) ? rSize : vSize;
                std::memcpy(g_guestRam + vAddr, raw.data() + rAddr, copyLen);
            }
        }
    }
    return true;
}

bool LoadXbeIntoGuestRam(const char* xbePath, uint32_t& outEntryVA) {
    outEntryVA = 0;
    if (!g_guestRam && !InitGuestMemory()) return false;

    FILE* f = std::fopen(xbePath, "rb");
    if (!f) {
        std::printf("[UWP-XBE] Failed to open '%s'\n", xbePath);
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    long fileSize = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (fileSize < 0x180) {
        std::fclose(f);
        return false;
    }

    g_cachedXbeRaw.resize(static_cast<size_t>(fileSize));
    std::fread(g_cachedXbeRaw.data(), 1, g_cachedXbeRaw.size(), f);
    std::fclose(f);

    if (!MapCachedXbeIntoGuestRam(outEntryVA)) {
        std::printf("[UWP-XBE] Invalid XBE magic in '%s'\n", xbePath);
        return false;
    }
    uint32_t baseAddr = 0, sizeOfImage = 0, numSections = 0;
    std::memcpy(&baseAddr,    g_cachedXbeRaw.data() + 0x104, 4);
    std::memcpy(&sizeOfImage, g_cachedXbeRaw.data() + 0x10C, 4);
    std::memcpy(&numSections, g_cachedXbeRaw.data() + 0x11C, 4);
    std::printf("[UWP-XBE] Loaded '%s' into portable 64 MB guest RAM (base=0x%08X, image=0x%X, sections=%u, entry=0x%08X)\n",
                xbePath, baseAddr, sizeOfImage, numSections, outEntryVA);
    return true;
}

bool ResetGuestMemoryAndReloadXbe() {
    if (!g_guestRam || g_cachedXbeRaw.empty()) return false;
    std::memset(g_guestRam, 0, kXboxRamSize);
    {
        std::lock_guard<std::mutex> lk(g_heapMtx);
        g_freeByAddr.clear();
        g_freeBySize.clear();
        g_allocMap.clear();
        g_totalFreeBytes = kDynHeapLimit - kDynHeapBase;
        FreeInsert(kDynHeapBase, g_totalFreeBytes);
    }
    uint32_t dummyEntry = 0;
    return MapCachedXbeIntoGuestRam(dummyEntry);
}

void RegisterGuestFunc(uint32_t va, GuestFunc fn) {
    if (va < kFastFuncLimit) {
        if (!g_fastFuncTable) {
            g_fastFuncTable = static_cast<GuestFunc*>(std::calloc(kFastFuncLimit, sizeof(GuestFunc)));
        }
        if (g_fastFuncTable) {
            g_fastFuncTable[va] = fn;
            return;
        }
    }
    g_funcTable[va] = fn;
}

void CallGuestVA(X86Context& ctx, uint32_t targetVA) {
    g_recentCalls[g_recentCallIdx++ & 15u] = targetVA;
    if (targetVA < kFastFuncLimit && g_fastFuncTable) {
        GuestFunc fn = g_fastFuncTable[targetVA];
        if (fn) {
            fn(ctx);
            return;
        }
    }
    auto it = g_funcTable.find(targetVA);
    if (it != g_funcTable.end() && it->second) {
        it->second(ctx);
        return;
    }
    static std::mutex s_logMtx;
    bool shouldLog = false;
    {
        std::lock_guard<std::mutex> lk(s_logMtx);
        shouldLog = g_loggedMissingVAs.insert(targetVA).second;
    }
    if (shouldLog) {
        uint32_t caller = ctx.r32(ctx.esp);
        std::printf("[UWP-CPU] Unimplemented guest call target 0x%08X (ESP=0x%08X, caller=0x%08X)\n", targetVA, ctx.esp, caller);
        if (targetVA == 0u || targetVA >= 0x01000000u) {
            DumpRecentGuestCalls();
        }
    }
    // Pop return address so execution can continue tracing missing bridges
    ctx.esp += 4u;
    ctx.eax = 0u;
}

void DumpRecentGuestCalls() {
    {
        std::lock_guard<std::mutex> lk(g_heapMtx);
        std::printf("[UWP-HEAP-STAT] live=%zu free=%.2fMB holes=%zu totalAllocs=%llu totalFrees=%llu\n",
                    g_allocMap.size(), static_cast<double>(g_totalFreeBytes) / (1024.0 * 1024.0),
                    g_freeByAddr.size(), static_cast<unsigned long long>(g_totalAllocCalls),
                    static_cast<unsigned long long>(g_totalFreeCalls));
    }
    std::printf("[UWP-TRACE] Last 16 guest calls (oldest -> newest):\n");
    for (uint32_t i = 0; i < 16u; ++i) {
        uint32_t va = g_recentCalls[(g_recentCallIdx + i) & 15u];
        if (va) std::printf("  #%02u: 0x%08X\n", i + 1u, va);
    }
}

} // namespace uwp_recomp
