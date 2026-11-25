#pragma once
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cmath>

namespace uwp_recomp {

inline uint32_t UwpTimerMicros32() {
    static const auto t0 = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    return static_cast<uint32_t>(us);
}

// Original Xbox has 64 MB unified RAM (0x00000000 .. 0x03FFFFFF).
// Static recomp maps all 32-bit guest pointers as offsets into this flat 64 MB buffer,
// making the entire runtime 100% portable to x64 UWP (Xbox One / Series X|S Dev Mode) & ARM64!
static constexpr uint32_t kXboxRamSize      = 64u * 1024u * 1024u; // 64 MB
static constexpr uint32_t kGuestStackTop    = 0x03F00000u;         // 8 MB stack below top of RAM
static constexpr uint32_t kGuestHeapBase    = 0x01000000u;         // Dynamic guest heap (16 MB..56 MB)
static constexpr uint32_t kGuestHeapLimit   = 0x03700000u;
static constexpr uint32_t kReturnSentinelVA = 0xFFFFFFF0u;

struct X86Context {
    // General-purpose 32-bit registers
    uint32_t eax = 0, ecx = 0, edx = 0, ebx = 0;
    uint32_t esp = kGuestStackTop, ebp = 0, esi = 0, edi = 0;

    // CPU flags (unpacked for fast portable C++ evaluation)
    bool cf = false; // Carry
    bool zf = false; // Zero
    bool sf = false; // Sign
    bool of = false; // Overflow
    bool pf = false; // Parity
    bool df = false; // Direction (string ops)

    // x87 FPU 8-register stack (Original Xbox Slate Engine uses x87 extensively)
    double   st[8] = {};
    int      fpuTop = 0;
    uint16_t fpuSw  = 0;

    // Flat 64 MB guest RAM pointer
    uint8_t* mem = nullptr;

#ifdef _MSC_VER
#define UWP_FORCEINLINE __forceinline
#else
#define UWP_FORCEINLINE inline __attribute__((always_inline))
#endif

    // ------------------------------------------------------------------------
    // Flat Guest Memory Accessors (32-bit guest VA -> host pointer)
    // ------------------------------------------------------------------------
    UWP_FORCEINLINE uint8_t* ptr(uint32_t va) const {
        return mem + (va & (kXboxRamSize - 1u));
    }

    UWP_FORCEINLINE uint8_t  r8(uint32_t va)  const { return *ptr(va); }
    UWP_FORCEINLINE uint16_t r16(uint32_t va) const { uint16_t v; std::memcpy(&v, ptr(va), 2); return v; }
    UWP_FORCEINLINE uint32_t r32(uint32_t va) const { uint32_t v; std::memcpy(&v, ptr(va), 4); return v; }
    UWP_FORCEINLINE float    rf32(uint32_t va) const { float v;   std::memcpy(&v, ptr(va), 4); return v; }
    UWP_FORCEINLINE double   rf64(uint32_t va) const { double v;  std::memcpy(&v, ptr(va), 8); return v; }

    UWP_FORCEINLINE void w8(uint32_t va, uint8_t v)   { *ptr(va) = v; }
    UWP_FORCEINLINE void w16(uint32_t va, uint16_t v) { std::memcpy(ptr(va), &v, 2); }
    UWP_FORCEINLINE void w32(uint32_t va, uint32_t v) { std::memcpy(ptr(va), &v, 4); }
    UWP_FORCEINLINE void wf32(uint32_t va, float v)   { std::memcpy(ptr(va), &v, 4); }
    UWP_FORCEINLINE void wf64(uint32_t va, double v)  { std::memcpy(ptr(va), &v, 8); }

    // Stack helpers
    UWP_FORCEINLINE void push32(uint32_t v) { esp -= 4u; w32(esp, v); }
    UWP_FORCEINLINE uint32_t pop32()        { uint32_t v = r32(esp); esp += 4u; return v; }

    // x87 FPU stack helpers
    UWP_FORCEINLINE double& fpuReg(int idx) { return st[(fpuTop + idx) & 7]; }
    UWP_FORCEINLINE void    fpuPush(double v) { fpuTop = (fpuTop - 1) & 7; st[fpuTop] = v; }
    UWP_FORCEINLINE double  fpuPop()          { double v = st[fpuTop]; fpuTop = (fpuTop + 1) & 7; return v; }
    UWP_FORCEINLINE void    fpuCompare(double a, double b) {
        fpuSw &= ~0x4500u; // Clear C3(0x4000), C2(0x0400), C0(0x0100)
        if (std::isnan(a) || std::isnan(b)) { fpuSw |= 0x4500u; }
        else if (a < b)                     { fpuSw |= 0x0100u; }
        else if (a == b)                    { fpuSw |= 0x4000u; }
    }
    UWP_FORCEINLINE void sahf() {
        uint8_t ah = static_cast<uint8_t>((eax >> 8) & 0xFFu);
        cf = (ah & 0x01u) != 0;
        pf = (ah & 0x04u) != 0;
        zf = (ah & 0x40u) != 0;
        sf = (ah & 0x80u) != 0;
    }

    // 8-bit & 16-bit sub-register helpers
    static UWP_FORCEINLINE uint8_t  lo8(uint32_t r)  { return static_cast<uint8_t>(r & 0xFFu); }
    static UWP_FORCEINLINE uint8_t  hi8(uint32_t r)  { return static_cast<uint8_t>((r >> 8) & 0xFFu); }
    static UWP_FORCEINLINE uint16_t lo16(uint32_t r) { return static_cast<uint16_t>(r & 0xFFFFu); }
    static UWP_FORCEINLINE void setLo8(uint32_t& r, uint8_t v)   { r = (r & 0xFFFFFF00u) | v; }
    static UWP_FORCEINLINE void setHi8(uint32_t& r, uint8_t v)   { r = (r & 0xFFFF00FFu) | (static_cast<uint32_t>(v) << 8); }
    static UWP_FORCEINLINE void setLo16(uint32_t& r, uint16_t v) { r = (r & 0xFFFF0000u) | v; }

    static UWP_FORCEINLINE bool parity8(uint8_t v) {
        v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
        return (v & 1u) == 0;
    }

    // Common ALU flag helpers
    UWP_FORCEINLINE void setFlagsSub(uint32_t a, uint32_t b, uint32_t res, int bits) {
        uint32_t mask = (bits == 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
        uint32_t sign = 1u << (bits - 1);
        a &= mask; b &= mask; res &= mask;
        cf = (a < b);
        zf = (res == 0);
        sf = (res & sign) != 0;
        of = (((a ^ b) & (a ^ res)) & sign) != 0;
        pf = parity8(static_cast<uint8_t>(res & 0xFFu));
    }
    UWP_FORCEINLINE void setFlagsAdd(uint32_t a, uint32_t b, uint32_t res, int bits) {
        uint32_t mask = (bits == 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
        uint32_t sign = 1u << (bits - 1);
        a &= mask; b &= mask; res &= mask;
        cf = (res < a);
        zf = (res == 0);
        sf = (res & sign) != 0;
        of = ((~(a ^ b) & (a ^ res)) & sign) != 0;
        pf = parity8(static_cast<uint8_t>(res & 0xFFu));
    }
    UWP_FORCEINLINE void setFlagsLogic(uint32_t res, int bits) {
        uint32_t mask = (bits == 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
        uint32_t sign = 1u << (bits - 1);
        res &= mask;
        cf = false;
        of = false;
        zf = (res == 0);
        sf = (res & sign) != 0;
        pf = parity8(static_cast<uint8_t>(res & 0xFFu));
    }
    inline void setFlagsSub32(uint32_t a, uint32_t b, uint32_t res) { setFlagsSub(a, b, res, 32); }
    inline void setFlagsAdd32(uint32_t a, uint32_t b, uint32_t res) { setFlagsAdd(a, b, res, 32); }
    inline void setFlagsLogic32(uint32_t res)                       { setFlagsLogic(res, 32); }
};

using GuestFunc = void (*)(X86Context& ctx);

// Thrown by XLaunchNewImageA / worker threads to unwind cleanly for seamless in-process stage transitions!
struct GuestRelaunchException {};

// Initialize flat 64 MB guest RAM and map Default.xbe sections into it (no OS fixed-base needed!)
bool InitGuestMemory();
void ShutdownGuestMemory();
uint8_t* GetGuestRamBase();
bool LoadXbeIntoGuestRam(const char* xbePath, uint32_t& outEntryVA);
bool ResetGuestMemoryAndReloadXbe();

// Guest heap allocator (for RtlAllocateHeap / malloc inside 64 MB guest RAM)
uint32_t GuestHeapAlloc(uint32_t size, uint32_t align = 16);
void     GuestHeapFree(uint32_t va);

// Function registration & indirect call dispatch (handles vtables, function pointers & host bridges)
void RegisterGuestFunc(uint32_t va, GuestFunc fn);
void CallGuestVA(X86Context& ctx, uint32_t targetVA);
void DumpRecentGuestCalls();

} // namespace uwp_recomp
