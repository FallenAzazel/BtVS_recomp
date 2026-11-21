#include "xboxkrnl_win32.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace xboxkrnl {

static std::string g_extractedRoot = "extracted";
static std::string g_cacheRoot     = "recomp/cache";
static std::string g_savesRoot     = "recomp/saves";
static std::mutex  g_allocMtx;
static std::unordered_map<void*, uint32_t> g_allocSizes;

// ============================================================================
// Xbox Kernel Data Exports (Thunk Entries that are Pointers to Variables!)
// ============================================================================
static uint8_t            g_XboxHDKey[16]                  = {0};
static uint8_t            g_XboxSignatureKey[16]           = {0};
static uint8_t            g_XboxAlternateSignatureKeys[256]= {0};
static volatile uint32_t  g_KeTickCount                    = 100;
static uint32_t           g_ExEventObjectType              = 0x45564E54;
static uint32_t           g_PsThreadObjectType             = 0x54485244;
static uint32_t           g_HalDiskCachePartitionCount     = 3;
static void*              g_LaunchDataPage                 = nullptr;
static uint32_t           g_HalBootSMCVideoMode            = 1;
static XBOX_HARDWARE_INFO g_XboxHardwareInfo               = {0x00000020u, 0xA3, 0xD4, {0, 0}};
static void*              g_AvSavedDataAddress             = nullptr;

void InitVirtualDrives(const char* extractedRoot, const char* cacheRoot, const char* savesRoot) {
    if (extractedRoot) g_extractedRoot = extractedRoot;
    if (cacheRoot)     g_cacheRoot     = cacheRoot;
    if (savesRoot)     g_savesRoot     = savesRoot;
    CreateDirectoryA(g_cacheRoot.c_str(), nullptr);
    CreateDirectoryA(g_savesRoot.c_str(), nullptr);
}

bool TranslateXboxPath(const char* xboxPath, char* outWinPath, size_t maxLen) {
    if (!xboxPath || !outWinPath || maxLen == 0) return false;
    std::string p(xboxPath);

    // Strip leading "\??\" if present
    if (p.rfind("\\??\\", 0) == 0) {
        p = p.substr(4);
    }

    const char* cdromPrefix = "\\Device\\CdRom0\\";
    const char* hd1Prefix   = "\\Device\\Harddisk0\\Partition1\\";
    const char* hd2Prefix   = "\\Device\\Harddisk0\\Partition2\\";
    const char* hdZPrefix   = "\\Device\\Harddisk0\\partition3\\";
    if (_strnicmp(p.c_str(), cdromPrefix, strlen(cdromPrefix)) == 0) {
        p = g_extractedRoot + "\\" + p.substr(strlen(cdromPrefix));
    } else if (_strnicmp(p.c_str(), hd1Prefix, strlen(hd1Prefix)) == 0) {
        p = g_savesRoot + "\\" + p.substr(strlen(hd1Prefix));
    } else if (_strnicmp(p.c_str(), hd2Prefix, strlen(hd2Prefix)) == 0) {
        p = g_savesRoot + "\\" + p.substr(strlen(hd2Prefix));
    } else if (_strnicmp(p.c_str(), hdZPrefix, strlen(hdZPrefix)) == 0) {
        p = g_cacheRoot + "\\" + p.substr(strlen(hdZPrefix));
    } else if (_strnicmp(p.c_str(), "D:\\", 3) == 0) {
        p = g_extractedRoot + "\\" + p.substr(3);
    } else if (_strnicmp(p.c_str(), "D:", 2) == 0) {
        p = g_extractedRoot + "\\" + p.substr(2);
    } else if (_strnicmp(p.c_str(), "Z:\\", 3) == 0) {
        p = g_cacheRoot + "\\" + p.substr(3);
    } else if (_strnicmp(p.c_str(), "Z:", 2) == 0) {
        p = g_cacheRoot + "\\" + p.substr(2);
    } else if (_strnicmp(p.c_str(), "T:\\", 3) == 0 || _strnicmp(p.c_str(), "U:\\", 3) == 0) {
        p = g_savesRoot + "\\" + p.substr(3);
    } else if (_strnicmp(p.c_str(), "T:", 2) == 0 || _strnicmp(p.c_str(), "U:", 2) == 0) {
        p = g_savesRoot + "\\" + p.substr(2);
    }

    // Normalize slashes
    for (char& c : p) {
        if (c == '/') c = '\\';
    }

    if (p.size() + 1 > maxLen) return false;
    memcpy(outWinPath, p.c_str(), p.size() + 1);
    return true;
}

// ============================================================================
// Critical Sections (28-byte Xbox RTL_CRITICAL_SECTION compatible with static .data)
// ============================================================================
struct XBOX_RTL_CRITICAL_SECTION {
    uint32_t Unknown[4];     // +0x00..0x0F
    LONG     LockCount;      // +0x10
    LONG     RecursionCount; // +0x14
    DWORD    OwningThread;   // +0x18
};

void __stdcall Shim_RtlInitializeCriticalSection(XBOX_RTL_CRITICAL_SECTION* cs) {
    if (!cs) return;
    memset(cs, 0, sizeof(*cs));
    cs->LockCount = -1;
}

void __stdcall Shim_RtlEnterCriticalSection(XBOX_RTL_CRITICAL_SECTION* cs) {
    if (!cs) return;
    DWORD tid = GetCurrentThreadId();
    if (cs->OwningThread == tid) {
        cs->RecursionCount++;
        return;
    }
    while (InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&cs->OwningThread), static_cast<LONG>(tid), 0) != 0) {
        SwitchToThread();
    }
    cs->RecursionCount = 1;
}

void __stdcall Shim_RtlLeaveCriticalSection(XBOX_RTL_CRITICAL_SECTION* cs) {
    if (!cs) return;
    DWORD tid = GetCurrentThreadId();
    if (cs->OwningThread == tid) {
        if (--cs->RecursionCount <= 0) {
            cs->RecursionCount = 0;
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&cs->OwningThread), 0);
        }
    }
}

// ============================================================================
// Memory Management
// ============================================================================
void* __stdcall Shim_MmAllocateContiguousMemory(uint32_t NumberOfBytes) {
    size_t sz = (NumberOfBytes > 0) ? NumberOfBytes : 4096;
    void* ptr = VirtualAlloc(nullptr, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (ptr) {
        std::lock_guard<std::mutex> lk(g_allocMtx);
        g_allocSizes[ptr] = static_cast<uint32_t>(sz);
    }
    return ptr;
}

void* __stdcall Shim_MmAllocateContiguousMemoryEx(
    uint32_t NumberOfBytes,
    uint32_t /*LowestAcceptableAddress*/,
    uint32_t /*HighestAcceptableAddress*/,
    uint32_t /*Alignment*/,
    uint32_t /*ProtectionType*/
) {
    return Shim_MmAllocateContiguousMemory(NumberOfBytes);
}

void __stdcall Shim_MmFreeContiguousMemory(void* BaseAddress) {
    if (BaseAddress) {
        {
            std::lock_guard<std::mutex> lk(g_allocMtx);
            g_allocSizes.erase(BaseAddress);
        }
        VirtualFree(BaseAddress, 0, MEM_RELEASE);
    }
}

uint32_t __stdcall Shim_MmQueryAllocationSize(void* BaseAddress) {
    std::lock_guard<std::mutex> lk(g_allocMtx);
    auto it = g_allocSizes.find(BaseAddress);
    return (it != g_allocSizes.end()) ? it->second : 4096;
}

int32_t __stdcall Shim_MmQueryStatistics(MM_STATISTICS* Stats) {
    if (!Stats) return -1;
    Stats->TotalPhysicalPages = 16384;
    Stats->AvailablePages     = 12288;
    Stats->VirtualMemoryBytesCommitted = 16 * 1024 * 1024;
    Stats->VirtualMemoryBytesReserved  = 16 * 1024 * 1024;
    Stats->CachePagesCommitted = 512;
    Stats->PoolPagesCommitted  = 256;
    Stats->StackPagesCommitted = 64;
    Stats->ImagePagesCommitted = 2320;
    return 0;
}

void* __stdcall Shim_MmAllocateSystemMemory(uint32_t NumberOfBytes, uint32_t /*Protect*/) {
    return Shim_MmAllocateContiguousMemory(NumberOfBytes);
}

uint32_t __stdcall Shim_MmFreeSystemMemory(void* BaseAddress, uint32_t /*NumberOfBytes*/) {
    Shim_MmFreeContiguousMemory(BaseAddress);
    return 0;
}

void __stdcall Shim_MmSetAddressProtect(void* BaseAddress, uint32_t NumberOfBytes, uint32_t NewProtect) {
    if (!BaseAddress || NumberOfBytes == 0) return;
    DWORD oldProt = 0;
    VirtualProtect(BaseAddress, NumberOfBytes, NewProtect ? NewProtect : PAGE_EXECUTE_READWRITE, &oldProt);
}

uint32_t __stdcall Shim_MmQueryAddressProtect(void* /*BaseAddress*/) {
    return PAGE_EXECUTE_READWRITE;
}

uint32_t __stdcall Shim_MmGetPhysicalAddress(void* BaseAddress) {
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(BaseAddress));
}

void __stdcall Shim_MmLockUnlockBufferPages(void* /*BaseAddress*/, uint32_t /*NumberOfBytes*/, uint32_t /*UnlockPages*/) {}
void __stdcall Shim_MmLockUnlockPhysicalPage(uint32_t /*PhysicalAddress*/, uint32_t /*UnlockPage*/) {}
void __stdcall Shim_MmPersistContiguousMemory(void* /*BaseAddress*/, uint32_t /*NumberOfBytes*/, uint32_t /*Persist*/) {}

void* __stdcall Shim_MmClaimGpuInstanceMemory(uint32_t /*NumberOfBytes*/, uint32_t* NumberOfPaddingBytes) {
    if (NumberOfPaddingBytes) *NumberOfPaddingBytes = 0;
    static uint8_t s_dummyGpuInst[65536]{};
    return s_dummyGpuInst;
}

int32_t __stdcall Shim_NtAllocateVirtualMemory(
    void** BaseAddress,
    uint32_t /*ZeroBits*/,
    uint32_t* AllocationSize,
    uint32_t AllocationType,
    uint32_t Protect
) {
    if (!BaseAddress || !AllocationSize) return -1;
    uint32_t sz = (*AllocationSize > 0) ? *AllocationSize : 4096;
    DWORD winType = AllocationType & (MEM_COMMIT | MEM_RESERVE);
    if (winType == 0) winType = MEM_COMMIT | MEM_RESERVE;
    DWORD winProt = Protect ? Protect : PAGE_READWRITE;
    void* p = VirtualAlloc(*BaseAddress, sz, winType, winProt);
    if (!p && *BaseAddress != nullptr) {
        p = VirtualAlloc(nullptr, sz, MEM_COMMIT | MEM_RESERVE, winProt);
    }
    if (!p) return -1;
    *BaseAddress = p;
    g_allocSizes[p] = sz;
    return 0;
}

int32_t __stdcall Shim_NtFreeVirtualMemory(void** BaseAddress, uint32_t* FreeSize, uint32_t FreeType) {
    if (!BaseAddress || !*BaseAddress) return -1;
    if (FreeType & MEM_RELEASE) {
        g_allocSizes.erase(*BaseAddress);
        VirtualFree(*BaseAddress, 0, MEM_RELEASE);
    } else if (FreeSize && *FreeSize > 0) {
        VirtualFree(*BaseAddress, *FreeSize, MEM_DECOMMIT);
    }
    return 0;
}

int32_t __stdcall Shim_NtQueryVirtualMemory(void* BaseAddress, MEMORY_BASIC_INFORMATION* MemoryInformation) {
    if (!MemoryInformation) return -1;
    VirtualQuery(BaseAddress, MemoryInformation, sizeof(*MemoryInformation));
    return 0;
}

void* __stdcall Shim_ExAllocatePool(uint32_t NumberOfBytes) {
    void* p = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, NumberOfBytes ? NumberOfBytes : 16);
    if (p) {
        std::lock_guard<std::mutex> lk(g_allocMtx);
        g_allocSizes[p] = NumberOfBytes;
    }
    return p;
}

void* __stdcall Shim_ExAllocatePoolWithTag(uint32_t NumberOfBytes, uint32_t /*Tag*/) {
    return Shim_ExAllocatePool(NumberOfBytes);
}

void __stdcall Shim_ExFreePool(void* P) {
    if (P) {
        bool owned = false;
        {
            std::lock_guard<std::mutex> lk(g_allocMtx);
            owned = (g_allocSizes.erase(P) > 0);
        }
        if (owned) {
            HeapFree(GetProcessHeap(), 0, P);
        }
    }
}

uint32_t __stdcall Shim_ExQueryPoolBlockSize(void* PoolBlock) {
    {
        std::lock_guard<std::mutex> lk(g_allocMtx);
        auto it = g_allocSizes.find(PoolBlock);
        if (it != g_allocSizes.end()) return it->second;
    }
    return static_cast<uint32_t>(HeapSize(GetProcessHeap(), 0, PoolBlock));
}

int32_t __stdcall Shim_FscSetCacheSize(uint32_t /*NumberOfCachePages*/) {
    return 0;
}

// ============================================================================
// Non-Volatile Settings, Strings, Time, Crypto, Display
// ============================================================================
int32_t __stdcall Shim_ExQueryNonVolatileSetting(
    uint32_t ValueIndex,
    uint32_t* Type,
    void* Value,
    uint32_t ValueLength,
    uint32_t* ResultLength
) {
    if (Type) *Type = 4; // REG_DWORD
    if (Value && ValueLength >= 4) {
        uint32_t val = 0;
        switch (ValueIndex) {
            case 7:  val = 1; break;          // XC_LANGUAGE = English (1)
            case 8:  val = 0x00400100; break; // XC_VIDEO_STANDARD = NTSC_M
            case 9:  val = 0; break;          // XC_AUDIO_FLAGS
            default: val = 0; break;
        }
        *reinterpret_cast<uint32_t*>(Value) = val;
    }
    if (ResultLength) *ResultLength = 4;
    return 0;
}

void __stdcall Shim_RtlInitAnsiString(ANSI_STRING* DestinationString, const char* SourceString) {
    if (!DestinationString) return;
    DestinationString->Buffer = const_cast<char*>(SourceString);
    if (SourceString) {
        uint16_t len = static_cast<uint16_t>(strlen(SourceString));
        DestinationString->Length        = len;
        DestinationString->MaximumLength = len + 1;
    } else {
        DestinationString->Length        = 0;
        DestinationString->MaximumLength = 0;
    }
}

int32_t __stdcall Shim_RtlAnsiStringToUnicodeString(
    UNICODE_STRING* DestinationString,
    const ANSI_STRING* SourceString,
    uint8_t AllocateDestinationString
) {
    if (!DestinationString || !SourceString) return -1;
    uint16_t len = SourceString->Length;
    if (AllocateDestinationString) {
        DestinationString->MaximumLength = (len + 1) * sizeof(wchar_t);
        DestinationString->Buffer = static_cast<wchar_t*>(Shim_ExAllocatePool(DestinationString->MaximumLength));
    }
    if (!DestinationString->Buffer) return -1;
    for (uint16_t i = 0; i < len; ++i) {
        DestinationString->Buffer[i] = static_cast<uint8_t>(SourceString->Buffer[i]);
    }
    DestinationString->Buffer[len] = L'\0';
    DestinationString->Length = len * sizeof(wchar_t);
    return 0;
}

uint32_t __stdcall Shim_RtlNtStatusToDosError(int32_t Status) {
    if (Status == 0) return ERROR_SUCCESS;
    if (Status == static_cast<int32_t>(0xC0000034) || Status == static_cast<int32_t>(0xC000000F)) return ERROR_FILE_NOT_FOUND;
    if (Status == static_cast<int32_t>(0xC000003A)) return ERROR_PATH_NOT_FOUND;
    if (Status == static_cast<int32_t>(0xC0000035)) return ERROR_ALREADY_EXISTS;
    return ERROR_GEN_FAILURE;
}

uint32_t __stdcall Shim_RtlCompareMemoryUlong(void* Source, uint32_t Length, uint32_t Pattern) {
    if (!Source) return 0;
    uint32_t* p = static_cast<uint32_t*>(Source);
    uint32_t count = Length / 4;
    uint32_t matched = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (p[i] != Pattern) break;
        matched += 4;
    }
    return matched;
}

uint8_t __stdcall Shim_RtlEqualString(const ANSI_STRING* String1, const ANSI_STRING* String2, uint8_t CaseInSensitive) {
    if (!String1 || !String2) return 0;
    if (String1->Length != String2->Length) return 0;
    if (CaseInSensitive) {
        return (_strnicmp(String1->Buffer, String2->Buffer, String1->Length) == 0) ? 1 : 0;
    }
    return (memcmp(String1->Buffer, String2->Buffer, String1->Length) == 0) ? 1 : 0;
}

void __stdcall Shim_KeQuerySystemTime(uint64_t* CurrentTime) {
    if (!CurrentTime) return;
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    *CurrentTime = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

uint64_t __stdcall Shim_KeQueryInterruptTime() {
    g_KeTickCount = GetTickCount();
    return static_cast<uint64_t>(g_KeTickCount) * 10000ULL;
}

void __stdcall Shim_RtlTimeToTimeFields(const uint64_t* Time, TIME_FIELDS* TimeFields) {
    if (!Time || !TimeFields) return;
    FILETIME ft{};
    ft.dwLowDateTime  = static_cast<DWORD>(*Time & 0xFFFFFFFFULL);
    ft.dwHighDateTime = static_cast<DWORD>(*Time >> 32);
    SYSTEMTIME st{};
    FileTimeToSystemTime(&ft, &st);
    TimeFields->Year         = st.wYear;
    TimeFields->Month        = st.wMonth;
    TimeFields->Day          = st.wDay;
    TimeFields->Hour         = st.wHour;
    TimeFields->Minute       = st.wMinute;
    TimeFields->Second       = st.wSecond;
    TimeFields->Milliseconds = st.wMilliseconds;
    TimeFields->Weekday      = st.wDayOfWeek;
}

uint8_t __stdcall Shim_RtlTimeFieldsToTime(const TIME_FIELDS* TimeFields, uint64_t* Time) {
    if (!TimeFields || !Time) return 0;
    SYSTEMTIME st{};
    st.wYear         = TimeFields->Year;
    st.wMonth        = TimeFields->Month;
    st.wDay          = TimeFields->Day;
    st.wHour         = TimeFields->Hour;
    st.wMinute       = TimeFields->Minute;
    st.wSecond       = TimeFields->Second;
    st.wMilliseconds = TimeFields->Milliseconds;
    FILETIME ft{};
    if (!SystemTimeToFileTime(&st, &ft)) return 0;
    *Time = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return 1;
}

int32_t __stdcall Shim_XeLoadSection(void* /*SectionHeader*/) { return 0; }
int32_t __stdcall Shim_XeUnloadSection(void* /*SectionHeader*/) { return 0; }

void __stdcall Shim_AvSetDisplayMode(
    void* /*RegisterBase*/,
    uint32_t /*Step*/,
    uint32_t Mode,
    uint32_t Format,
    uint32_t Pitch,
    uint32_t FrameBuffer
) {
    LogPrintf("[xboxkrnl] AvSetDisplayMode(mode=0x%X, fmt=0x%X, pitch=%u, fb=0x%08X)\n",
              Mode, Format, Pitch, FrameBuffer);
}

void __stdcall Shim_AvSendTVEncoderOption(
    void* /*RegisterBase*/,
    uint32_t Option,
    uint32_t /*Param*/,
    uint32_t* pResult
) {
    if (pResult) {
        // Option 6 = AV_QUERY_AV_CAPABILITIES -> return 0x00400105 (Standard NTSC/VGA 640x480 mode pack)
        *pResult = (Option == 6) ? 0x00400105u : 0u;
    }
}

void* __stdcall Shim_AvGetSavedDataAddress() {
    return g_AvSavedDataAddress;
}

void __stdcall Shim_AvSetSavedDataAddress(void* Address) {
    g_AvSavedDataAddress = Address;
}

void __stdcall Shim_XcSHAInit(uint8_t* pbSHAContext) {
    if (pbSHAContext) memset(pbSHAContext, 0, 116);
}

void __stdcall Shim_XcSHAUpdate(uint8_t* pbSHAContext, const uint8_t* pbInput, uint32_t dwInputLength) {
    if (!pbSHAContext || !pbInput) return;
    uint32_t* state = reinterpret_cast<uint32_t*>(pbSHAContext);
    for (uint32_t i = 0; i < dwInputLength; ++i) {
        state[i % 5] = (state[i % 5] * 16777619u) ^ pbInput[i];
    }
}

void __stdcall Shim_XcSHAFinal(uint8_t* pbSHAContext, uint8_t* pbDigest) {
    if (pbSHAContext && pbDigest) {
        memcpy(pbDigest, pbSHAContext, 20);
    }
}

uint32_t __cdecl Shim_DbgPrint(const char* Format, ...) {
    if (!Format) return 0;
    char buf[1024]{};
    va_list args;
    va_start(args, Format);
    vsnprintf(buf, sizeof(buf), Format, args);
    va_end(args);
    LogPrintf("[DbgPrint] %s", buf);
    return 0;
}

// ============================================================================
// Events, Synchronization, Timers, Threads, IRQL
// ============================================================================
int32_t __stdcall Shim_NtCreateEvent(
    HANDLE* EventHandle,
    OBJECT_ATTRIBUTES* /*ObjectAttributes*/,
    uint32_t EventType,
    uint8_t InitialState
) {
    if (!EventHandle) return -1;
    // EventType: 0 = NotificationEvent (ManualReset=TRUE), 1 = SynchronizationEvent (ManualReset=FALSE)
    *EventHandle = CreateEventA(nullptr, (EventType == 0) ? TRUE : FALSE, InitialState ? TRUE : FALSE, nullptr);
    return (*EventHandle != nullptr) ? 0 : -1;
}

int32_t __stdcall Shim_NtSetEvent(HANDLE EventHandle, int32_t* PreviousState) {
    if (PreviousState) *PreviousState = 0;
    SetEvent(EventHandle);
    return 0;
}

int32_t __stdcall Shim_NtPulseEvent(HANDLE EventHandle, int32_t* PreviousState) {
    if (PreviousState) *PreviousState = 0;
    PulseEvent(EventHandle);
    return 0;
}

int32_t __stdcall Shim_NtClearEvent(HANDLE EventHandle) {
    ResetEvent(EventHandle);
    return 0;
}

int32_t __stdcall Shim_NtWaitForSingleObject(HANDLE Handle, uint8_t /*Alertable*/, int64_t* Timeout) {
    DWORD ms = INFINITE;
    if (Timeout) {
        int64_t t = *Timeout;
        if (t <= 0) {
            ms = static_cast<DWORD>((-t) / 10000LL);
        } else {
            ms = 0;
        }
    }
    DWORD res = WaitForSingleObject(Handle, ms);
    return (res == WAIT_TIMEOUT) ? 0x00000102 : 0;
}

int32_t __stdcall Shim_NtWaitForSingleObjectEx(HANDLE Handle, uint8_t /*WaitMode*/, uint8_t Alertable, int64_t* Timeout) {
    return Shim_NtWaitForSingleObject(Handle, Alertable, Timeout);
}

int32_t __stdcall Shim_KeDelayExecutionThread(uint8_t /*WaitMode*/, uint8_t /*Alertable*/, int64_t* Interval) {
    DWORD ms = 1;
    if (Interval) {
        int64_t t = *Interval;
        ms = (t < 0) ? static_cast<DWORD>((-t) / 10000LL) : 0;
    }
    Sleep(ms);
    return 0;
}

int32_t __stdcall Shim_NtYieldExecution() {
    SwitchToThread();
    return 0;
}

int32_t __stdcall Shim_KeSetEvent(void* /*Event*/, int32_t /*Increment*/, uint8_t /*Wait*/) { return 0; }
int32_t __stdcall Shim_KeWaitForSingleObject(void* /*Object*/, uint32_t /*WaitReason*/, uint8_t /*WaitMode*/, uint8_t /*Alertable*/, int64_t* Timeout) {
    if (Timeout && *Timeout < 0) Sleep(static_cast<DWORD>((-*Timeout) / 10000LL));
    return 0;
}
int32_t __stdcall Shim_KeWaitForMultipleObjects(uint32_t /*Count*/, void** /*Objects*/, uint32_t /*WaitType*/, uint32_t /*WaitReason*/, uint8_t /*WaitMode*/, uint8_t /*Alertable*/, int64_t* /*Timeout*/, void* /*WaitBlockArray*/) {
    return 0;
}

void __stdcall Shim_KeInitializeTimerEx(void* /*Timer*/, uint32_t /*Type*/) {}
uint8_t __stdcall Shim_KeSetTimer(void* /*Timer*/, int64_t /*DueTime*/, void* /*Dpc*/) { return 1; }
uint8_t __stdcall Shim_KeSetTimerEx(void* /*Timer*/, int64_t /*DueTime*/, int32_t /*Period*/, void* /*Dpc*/) { return 1; }
uint8_t __stdcall Shim_KeCancelTimer(void* /*Timer*/) { return 1; }
void __stdcall Shim_KeInitializeDpc(void* /*Dpc*/, void* /*DeferredRoutine*/, void* /*DeferredContext*/) {}
uint8_t __stdcall Shim_KeInsertQueueDpc(void* /*Dpc*/, void* /*SystemArgument1*/, void* /*SystemArgument2*/) { return 1; }
uint8_t __stdcall Shim_KeRemoveQueueDpc(void* /*Dpc*/) { return 1; }

uint8_t __fastcall Shim_KfRaiseIrql(uint8_t NewIrql) { return NewIrql; }
void    __fastcall Shim_KfLowerIrql(uint8_t /*NewIrql*/) {}
uint8_t __stdcall  Shim_KeRaiseIrqlToDpcLevel() { return 2; }
void    __fastcall Shim_ObfDereferenceObject(void* /*Object*/) {}
int32_t __stdcall  Shim_ObReferenceObjectByHandle(HANDLE Handle, void* /*ObjectType*/, void** Object) {
    if (Object) *Object = Handle;
    return 0;
}
void    __fastcall Shim_IofCompleteRequest(void* /*Irp*/, uint8_t /*PriorityBoost*/) {}

// ============================================================================
// Threads (PsCreateSystemThreadEx / PsTerminateSystemThread)
// ============================================================================
struct XboxSystemThreadStartCtx {
    void* StartRoutine; // void (__stdcall *)(void* StartContext1, void* StartContext2)
    void* StartContext1;
    void* StartContext2;
};

static DWORD WINAPI XboxSystemThreadBridgeProc(LPVOID lpParam) {
    ULONG sg = 32768;
    SetThreadStackGuarantee(&sg);
    XboxSystemThreadStartCtx ctx = *static_cast<XboxSystemThreadStartCtx*>(lpParam);
    HeapFree(GetProcessHeap(), 0, lpParam);
    using FnType = void (__stdcall*)(void*, void*);
    reinterpret_cast<FnType>(ctx.StartRoutine)(ctx.StartContext1, ctx.StartContext2);
    return 0;
}

int32_t __stdcall Shim_PsCreateSystemThreadEx(
    HANDLE* ThreadHandle,
    uint32_t /*ThreadExtraSize*/,
    uint32_t KernelStackSize,
    uint32_t /*TlsDataSize*/,
    DWORD* ThreadId,
    void* StartContext1,
    void* StartContext2,
    uint8_t CreateSuspended,
    uint8_t /*DebugStack*/,
    void* StartRoutine
) {
    if (!ThreadHandle || !StartRoutine) return -1;
    auto* ctx = static_cast<XboxSystemThreadStartCtx*>(HeapAlloc(GetProcessHeap(), 0, sizeof(XboxSystemThreadStartCtx)));
    ctx->StartRoutine  = StartRoutine;
    ctx->StartContext1 = StartContext1;
    ctx->StartContext2 = StartContext2;
    DWORD tid = 0;
    SIZE_T winStack = (KernelStackSize > 1024u * 1024u) ? static_cast<SIZE_T>(KernelStackSize) : static_cast<SIZE_T>(1024u * 1024u);
    HANDLE h = CreateThread(nullptr, winStack, &XboxSystemThreadBridgeProc, ctx, CreateSuspended ? CREATE_SUSPENDED : 0, &tid);
    if (!h) {
        HeapFree(GetProcessHeap(), 0, ctx);
        return -1;
    }
    *ThreadHandle = h;
    if (ThreadId) *ThreadId = tid;
    return 0;
}

void __stdcall Shim_PsTerminateSystemThread(int32_t ExitStatus) {
    ExitThread(static_cast<DWORD>(ExitStatus));
}

// ============================================================================
// NT File & Directory I/O
// ============================================================================
int32_t __stdcall Shim_NtClose(HANDLE Handle) {
    if (Handle && Handle != INVALID_HANDLE_VALUE) {
        CloseHandle(Handle);
    }
    return 0;
}

int32_t __stdcall Shim_NtCreateFile(
    HANDLE* FileHandle,
    uint32_t DesiredAccess,
    OBJECT_ATTRIBUTES* ObjectAttributes,
    IO_STATUS_BLOCK* IoStatusBlock,
    int64_t* /*AllocationSize*/,
    uint32_t FileAttributes,
    uint32_t ShareAccess,
    uint32_t CreateDisposition,
    uint32_t /*CreateOptions*/
) {
    if (!FileHandle || !ObjectAttributes || !ObjectAttributes->ObjectName || !ObjectAttributes->ObjectName->Buffer) {
        return static_cast<int32_t>(0xC000000D);
    }
    char winPath[512]{};
    TranslateXboxPath(ObjectAttributes->ObjectName->Buffer, winPath, sizeof(winPath));

    DWORD access = 0;
    if (DesiredAccess & (GENERIC_READ | FILE_READ_DATA | SYNCHRONIZE)) access |= GENERIC_READ;
    if (DesiredAccess & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)) access |= GENERIC_WRITE;
    if (access == 0) access = GENERIC_READ;

    DWORD disp = OPEN_EXISTING;
    switch (CreateDisposition) {
        case 0: disp = CREATE_ALWAYS; break; // FILE_SUPERSEDE
        case 1: disp = OPEN_EXISTING; break; // FILE_OPEN
        case 2: disp = CREATE_NEW;    break; // FILE_CREATE
        case 3: disp = OPEN_ALWAYS;   break; // FILE_OPEN_IF
        case 4: disp = TRUNCATE_EXISTING; break; // FILE_OVERWRITE
        case 5: disp = CREATE_ALWAYS; break; // FILE_OVERWRITE_IF
    }

    HANDLE h = CreateFileA(winPath, access, ShareAccess ? ShareAccess : (FILE_SHARE_READ | FILE_SHARE_WRITE),
                           nullptr, disp, FileAttributes ? FileAttributes : FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *FileHandle = INVALID_HANDLE_VALUE;
        if (IoStatusBlock) { IoStatusBlock->Status = static_cast<int32_t>(0xC0000034); IoStatusBlock->Information = 0; }
        return static_cast<int32_t>(0xC0000034); // STATUS_OBJECT_NAME_NOT_FOUND
    }
    *FileHandle = h;
    if (IoStatusBlock) { IoStatusBlock->Status = 0; IoStatusBlock->Information = 1; }
    return 0;
}

int32_t __stdcall Shim_NtOpenFile(
    HANDLE* FileHandle,
    uint32_t DesiredAccess,
    OBJECT_ATTRIBUTES* ObjectAttributes,
    IO_STATUS_BLOCK* IoStatusBlock,
    uint32_t ShareAccess,
    uint32_t OpenOptions
) {
    return Shim_NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, nullptr,
                             FILE_ATTRIBUTE_NORMAL, ShareAccess, 1 /*FILE_OPEN*/, OpenOptions);
}

int32_t __stdcall Shim_NtReadFile(
    HANDLE FileHandle,
    HANDLE Event,
    void* /*ApcRoutine*/,
    void* /*ApcContext*/,
    IO_STATUS_BLOCK* IoStatusBlock,
    void* Buffer,
    uint32_t Length,
    int64_t* ByteOffset
) {
    if (ByteOffset && *ByteOffset >= 0) {
        LARGE_INTEGER li{};
        li.QuadPart = *ByteOffset;
        SetFilePointerEx(FileHandle, li, nullptr, FILE_BEGIN);
    }
    DWORD bytesRead = 0;
    BOOL ok = ReadFile(FileHandle, Buffer, Length, &bytesRead, nullptr);
    if (Event) SetEvent(Event);
    if (IoStatusBlock) {
        IoStatusBlock->Status      = ok ? 0 : -1;
        IoStatusBlock->Information = bytesRead;
    }
    return ok ? 0 : -1;
}

int32_t __stdcall Shim_NtWriteFile(
    HANDLE FileHandle,
    HANDLE Event,
    void* /*ApcRoutine*/,
    void* /*ApcContext*/,
    IO_STATUS_BLOCK* IoStatusBlock,
    const void* Buffer,
    uint32_t Length,
    int64_t* ByteOffset
) {
    if (ByteOffset && *ByteOffset >= 0) {
        LARGE_INTEGER li{};
        li.QuadPart = *ByteOffset;
        SetFilePointerEx(FileHandle, li, nullptr, FILE_BEGIN);
    }
    DWORD bytesWritten = 0;
    BOOL ok = WriteFile(FileHandle, Buffer, Length, &bytesWritten, nullptr);
    if (Event) SetEvent(Event);
    if (IoStatusBlock) {
        IoStatusBlock->Status      = ok ? 0 : -1;
        IoStatusBlock->Information = bytesWritten;
    }
    return ok ? 0 : -1;
}

int32_t __stdcall Shim_NtQueryInformationFile(
    HANDLE FileHandle,
    IO_STATUS_BLOCK* IoStatusBlock,
    void* FileInformation,
    uint32_t Length,
    uint32_t FileInformationClass
) {
    if (!FileInformation) return -1;
    memset(FileInformation, 0, Length);
    LARGE_INTEGER sz{};
    GetFileSizeEx(FileHandle, &sz);
    // FileStandardInformation = 5 (AllocationSize at +0, EndOfFile at +8)
    if (FileInformationClass == 5 && Length >= 16) {
        *reinterpret_cast<int64_t*>(static_cast<uint8_t*>(FileInformation) + 0) = sz.QuadPart;
        *reinterpret_cast<int64_t*>(static_cast<uint8_t*>(FileInformation) + 8) = sz.QuadPart;
    } else if (FileInformationClass == 14 && Length >= 8) { // FilePositionInformation
        LARGE_INTEGER zero{}, pos{};
        SetFilePointerEx(FileHandle, zero, &pos, FILE_CURRENT);
        *reinterpret_cast<int64_t*>(FileInformation) = pos.QuadPart;
    }
    if (IoStatusBlock) { IoStatusBlock->Status = 0; IoStatusBlock->Information = Length; }
    return 0;
}

int32_t __stdcall Shim_NtSetInformationFile(
    HANDLE FileHandle,
    IO_STATUS_BLOCK* IoStatusBlock,
    void* FileInformation,
    uint32_t /*Length*/,
    uint32_t FileInformationClass
) {
    if (FileInformationClass == 14 && FileInformation) { // FilePositionInformation
        LARGE_INTEGER pos{};
        pos.QuadPart = *reinterpret_cast<int64_t*>(FileInformation);
        SetFilePointerEx(FileHandle, pos, nullptr, FILE_BEGIN);
    } else if (FileInformationClass == 20 && FileInformation) { // FileEndOfFileInformation
        LARGE_INTEGER pos{};
        pos.QuadPart = *reinterpret_cast<int64_t*>(FileInformation);
        SetFilePointerEx(FileHandle, pos, nullptr, FILE_BEGIN);
        SetEndOfFile(FileHandle);
    }
    if (IoStatusBlock) { IoStatusBlock->Status = 0; IoStatusBlock->Information = 0; }
    return 0;
}

int32_t __stdcall Shim_NtQueryFullAttributesFile(OBJECT_ATTRIBUTES* ObjectAttributes, void* FileInformation) {
    if (!ObjectAttributes || !ObjectAttributes->ObjectName || !ObjectAttributes->ObjectName->Buffer || !FileInformation) {
        return static_cast<int32_t>(0xC0000034);
    }
    char winPath[512]{};
    TranslateXboxPath(ObjectAttributes->ObjectName->Buffer, winPath, sizeof(winPath));
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExA(winPath, GetFileExInfoStandard, &fad)) {
        return static_cast<int32_t>(0xC0000034);
    }
    // FILE_NETWORK_OPEN_INFORMATION (56 bytes)
    uint8_t* out = static_cast<uint8_t*>(FileInformation);
    memset(out, 0, 56);
    memcpy(out + 0,  &fad.ftCreationTime,   8);
    memcpy(out + 8,  &fad.ftLastAccessTime, 8);
    memcpy(out + 16, &fad.ftLastWriteTime,  8);
    memcpy(out + 24, &fad.ftLastWriteTime,  8);
    uint64_t fsz = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    *reinterpret_cast<uint64_t*>(out + 32) = (fsz + 4095ULL) & ~4095ULL;
    *reinterpret_cast<uint64_t*>(out + 40) = fsz;
    *reinterpret_cast<uint32_t*>(out + 48) = fad.dwFileAttributes;
    return 0;
}

int32_t __stdcall Shim_NtQueryVolumeInformationFile(
    HANDLE /*FileHandle*/,
    IO_STATUS_BLOCK* IoStatusBlock,
    void* FsInformation,
    uint32_t Length,
    uint32_t FsInformationClass
) {
    if (FsInformation && Length > 0) {
        memset(FsInformation, 0, Length);
        if (FsInformationClass == 3 && Length >= 24) { // FileFsSizeInformation
            uint8_t* p = static_cast<uint8_t*>(FsInformation);
            *reinterpret_cast<uint64_t*>(p + 0)  = 500000; // TotalAllocationUnits
            *reinterpret_cast<uint64_t*>(p + 8)  = 400000; // AvailableAllocationUnits
            *reinterpret_cast<uint32_t*>(p + 16) = 32;     // SectorsPerAllocationUnit
            *reinterpret_cast<uint32_t*>(p + 20) = 512;    // BytesPerSector
        }
    }
    if (IoStatusBlock) { IoStatusBlock->Status = 0; IoStatusBlock->Information = Length; }
    return 0;
}

int32_t __stdcall Shim_NtFlushBuffersFile(HANDLE FileHandle, IO_STATUS_BLOCK* IoStatusBlock) {
    FlushFileBuffers(FileHandle);
    if (IoStatusBlock) { IoStatusBlock->Status = 0; IoStatusBlock->Information = 0; }
    return 0;
}

int32_t __stdcall Shim_IoCreateSymbolicLink(ANSI_STRING* /*SymbolicLinkName*/, ANSI_STRING* /*DeviceName*/) { return 0; }
int32_t __stdcall Shim_IoDeleteSymbolicLink(ANSI_STRING* /*SymbolicLinkName*/) { return 0; }
int32_t __stdcall Shim_NtOpenSymbolicLinkObject(HANDLE* LinkHandle, OBJECT_ATTRIBUTES* /*ObjectAttributes*/) {
    if (LinkHandle) *LinkHandle = reinterpret_cast<HANDLE>(0x12345678);
    return 0;
}
int32_t __stdcall Shim_NtQuerySymbolicLinkObject(HANDLE /*LinkHandle*/, ANSI_STRING* /*LinkTarget*/, uint32_t* /*ReturnedLength*/) {
    return 0;
}

// ============================================================================
// Safe stdcall no-op stubs for unused driver/HAL thunks (exact stack cleanup!)
// ============================================================================
uint32_t __stdcall Stub_Ret0()  { return 0; }
uint32_t __stdcall Stub_Ret4(uint32_t) { return 0; }
uint32_t __stdcall Stub_Ret8(uint32_t, uint32_t) { return 0; }
uint32_t __stdcall Stub_Ret12(uint32_t, uint32_t, uint32_t) { return 0; }
uint32_t __stdcall Stub_Ret16(uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
uint32_t __stdcall Stub_Ret20(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
uint32_t __stdcall Stub_Ret24(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
uint32_t __stdcall Stub_Ret28(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
uint32_t __stdcall Stub_Ret32(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
uint32_t __stdcall Stub_Ret36(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
uint32_t __stdcall Stub_Ret40(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }

void __stdcall Shim_RtlRaiseException(EXCEPTION_RECORD* ExceptionRecord) {
    if (ExceptionRecord) {
        RaiseException(ExceptionRecord->ExceptionCode, ExceptionRecord->ExceptionFlags,
                       ExceptionRecord->NumberParameters, ExceptionRecord->ExceptionInformation);
    }
}

// ============================================================================
// Complete 120-Entry Xbox Kernel Thunk Table (VA 0x00616D80 .. 0x00616F5C)
// Exact 1-to-1 match with decompile/kernel_imports.json
// ============================================================================
void InstallKernelThunkTable(uint32_t* t) {
    if (!t) return;
    auto fn = [](auto p) -> uint32_t { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)); };

    t[0]   = fn(&Shim_XcSHAUpdate);                  // 0x00616D80: XcSHAUpdate@12
    t[1]   = fn(&Shim_XcSHAInit);                    // 0x00616D84: XcSHAInit@4
    t[2]   = fn(&Shim_XcSHAFinal);                   // 0x00616D88: XcSHAFinal@8
    t[3]   = fn(g_XboxHDKey);                        // 0x00616D8C: DATA XboxHDKey
    t[4]   = fn(g_XboxSignatureKey);                 // 0x00616D90: DATA XboxSignatureKey
    t[5]   = fn(g_XboxAlternateSignatureKeys);       // 0x00616D94: DATA XboxAlternateSignatureKeys
    t[6]   = fn(&Shim_KeSetTimerEx);                 // 0x00616D98: KeSetTimerEx@20
    t[7]   = fn(&Shim_KeQueryInterruptTime);         // 0x00616D9C: KeQueryInterruptTime@0
    t[8]   = fn(&Shim_RtlLeaveCriticalSection);      // 0x00616DA0: RtlLeaveCriticalSection@4
    t[9]   = fn(&Shim_NtPulseEvent);                 // 0x00616DA4: NtPulseEvent@8
    t[10]  = fn(&Shim_NtSetEvent);                   // 0x00616DA8: NtSetEvent@8
    t[11]  = fn(&Shim_RtlEnterCriticalSection);      // 0x00616DAC: RtlEnterCriticalSection@4
    t[12]  = fn(&Shim_KeWaitForMultipleObjects);     // 0x00616DB0: KeWaitForMultipleObjects@32
    t[13]  = fn(&Shim_KeSetEvent);                   // 0x00616DB4: KeSetEvent@12
    t[14]  = fn(&Shim_KeInitializeTimerEx);          // 0x00616DB8: KeInitializeTimerEx@8
    t[15]  = fn(&Shim_KeWaitForSingleObject);        // 0x00616DBC: KeWaitForSingleObject@20
    t[16]  = fn(&Shim_KeCancelTimer);                // 0x00616DC0: KeCancelTimer@4
    t[17]  = fn(&g_KeTickCount);                     // 0x00616DC4: DATA KeTickCount
    t[18]  = fn(&Shim_KfLowerIrql);                  // 0x00616DC8: @KfLowerIrql@4
    t[19]  = fn(&Shim_KeRaiseIrqlToDpcLevel);        // 0x00616DCC: KeRaiseIrqlToDpcLevel@0
    t[20]  = fn(&Stub_Ret24);                        // 0x00616DD0: IoCreateDevice@24
    t[21]  = fn(&Shim_RtlInitAnsiString);            // 0x00616DD4: RtlInitAnsiString@8
    t[22]  = fn(&Shim_ExAllocatePool);               // 0x00616DD8: ExAllocatePool@4
    t[23]  = fn(&Shim_KeInitializeDpc);              // 0x00616DDC: KeInitializeDpc@12
    t[24]  = fn(&Stub_Ret8);                         // 0x00616DE0: IoInvalidDeviceRequest@8
    t[25]  = fn(&Shim_NtAllocateVirtualMemory);      // 0x00616DE4: NtAllocateVirtualMemory@20
    t[26]  = fn(&Shim_NtFreeVirtualMemory);          // 0x00616DE8: NtFreeVirtualMemory@12
    t[27]  = fn(&Shim_NtQueryVirtualMemory);         // 0x00616DEC: NtQueryVirtualMemory@8
    t[28]  = fn(&Shim_MmQueryStatistics);            // 0x00616DF0: MmQueryStatistics@4
    t[29]  = fn(&Shim_NtClose);                      // 0x00616DF4: NtClose@4
    t[30]  = fn(&Shim_NtSetInformationFile);         // 0x00616DF8: NtSetInformationFile@20
    t[31]  = fn(&Shim_NtOpenFile);                   // 0x00616DFC: NtOpenFile@24
    t[32]  = fn(&Shim_NtQueryFullAttributesFile);    // 0x00616E00: NtQueryFullAttributesFile@8
    t[33]  = fn(&Shim_FscSetCacheSize);              // 0x00616E04: FscSetCacheSize@4
    t[34]  = fn(&Stub_Ret40);                        // 0x00616E08: NtQueryDirectoryFile@40
    t[35]  = fn(&Shim_RtlNtStatusToDosError);        // 0x00616E0C: RtlNtStatusToDosError@4
    t[36]  = fn(&Shim_NtCreateEvent);                // 0x00616E10: NtCreateEvent@16
    t[37]  = fn(&g_ExEventObjectType);               // 0x00616E14: DATA ExEventObjectType
    t[38]  = fn(&Shim_NtClearEvent);                 // 0x00616E18: NtClearEvent@4
    t[39]  = fn(&Shim_NtWaitForSingleObjectEx);      // 0x00616E1C: NtWaitForSingleObjectEx@16
    t[40]  = fn(&Shim_KeDelayExecutionThread);       // 0x00616E20: KeDelayExecutionThread@12
    t[41]  = fn(&Shim_ObfDereferenceObject);         // 0x00616E24: @ObfDereferenceObject@4
    t[42]  = fn(&Shim_ObReferenceObjectByHandle);    // 0x00616E28: ObReferenceObjectByHandle@12
    t[43]  = fn(&g_PsThreadObjectType);              // 0x00616E2C: DATA PsThreadObjectType
    t[44]  = fn(&Shim_RtlRaiseException);            // 0x00616E30: RtlRaiseException@4
    t[45]  = fn(&Shim_NtYieldExecution);             // 0x00616E34: NtYieldExecution@0
    t[46]  = fn(&Shim_PsTerminateSystemThread);      // 0x00616E38: PsTerminateSystemThread@4
    t[47]  = fn(&Shim_PsCreateSystemThreadEx);       // 0x00616E3C: PsCreateSystemThreadEx@40
    t[48]  = fn(&Shim_NtWaitForSingleObject);        // 0x00616E40: NtWaitForSingleObject@12
    t[49]  = fn(&Shim_NtReadFile);                   // 0x00616E44: NtReadFile@32
    t[50]  = fn(&Shim_NtWriteFile);                  // 0x00616E48: NtWriteFile@32
    t[51]  = fn(&Shim_NtQueryInformationFile);       // 0x00616E4C: NtQueryInformationFile@20
    t[52]  = fn(&Shim_NtQueryVolumeInformationFile); // 0x00616E50: NtQueryVolumeInformationFile@20
    t[53]  = fn(&Shim_NtFlushBuffersFile);           // 0x00616E54: NtFlushBuffersFile@8
    t[54]  = fn(&Stub_Ret40);                        // 0x00616E58: NtFsControlFile@40
    t[55]  = fn(&Stub_Ret40);                        // 0x00616E5C: NtDeviceIoControlFile@40
    t[56]  = fn(&Shim_NtCreateFile);                 // 0x00616E60: NtCreateFile@36
    t[57]  = fn(&Shim_MmFreeSystemMemory);           // 0x00616E64: MmFreeSystemMemory@8
    t[58]  = fn(&Shim_MmAllocateSystemMemory);       // 0x00616E68: MmAllocateSystemMemory@8
    t[59]  = fn(&g_HalDiskCachePartitionCount);      // 0x00616E6C: DATA HalDiskCachePartitionCount
    t[60]  = fn(&Shim_IoCreateSymbolicLink);         // 0x00616E70: IoCreateSymbolicLink@8
    t[61]  = fn(&Shim_NtQuerySymbolicLinkObject);    // 0x00616E74: NtQuerySymbolicLinkObject@12
    t[62]  = fn(&Shim_NtOpenSymbolicLinkObject);     // 0x00616E78: NtOpenSymbolicLinkObject@8
    t[63]  = fn(&Shim_IoDeleteSymbolicLink);         // 0x00616E7C: IoDeleteSymbolicLink@4
    t[64]  = fn(&Shim_RtlTimeToTimeFields);          // 0x00616E80: RtlTimeToTimeFields@8
    t[65]  = fn(&Shim_KeQuerySystemTime);            // 0x00616E84: KeQuerySystemTime@4
    t[66]  = fn(&Shim_RtlTimeFieldsToTime);          // 0x00616E88: RtlTimeFieldsToTime@8
    t[67]  = fn(&Shim_MmPersistContiguousMemory);    // 0x00616E8C: MmPersistContiguousMemory@12
    t[68]  = fn(&Shim_MmAllocateContiguousMemory);   // 0x00616E90: MmAllocateContiguousMemory@4
    t[69]  = fn(&g_LaunchDataPage);                  // 0x00616E94: DATA LaunchDataPage
    t[70]  = fn(&Shim_MmFreeContiguousMemory);       // 0x00616E98: MmFreeContiguousMemory@4
    t[71]  = fn(&Stub_Ret4);                         // 0x00616E9C: HalReturnToFirmware@4
    t[72]  = fn(&Stub_Ret4);                         // 0x00616EA0: IoDismountVolumeByName@4
    t[73]  = fn(&Shim_RtlAnsiStringToUnicodeString); // 0x00616EA4: RtlAnsiStringToUnicodeString@12
    t[74]  = fn(&Shim_ExQueryNonVolatileSetting);    // 0x00616EA8: ExQueryNonVolatileSetting@20
    t[75]  = fn(&g_HalBootSMCVideoMode);             // 0x00616EAC: DATA HalBootSMCVideoMode
    t[76]  = fn(&Shim_KeSetTimer);                   // 0x00616EB0: KeSetTimer@16
    t[77]  = fn(&g_XboxHardwareInfo);                // 0x00616EB4: DATA XboxHardwareInfo
    t[78]  = fn(&Shim_ExFreePool);                   // 0x00616EB8: ExFreePool@4
    t[79]  = fn(&Stub_Ret12);                        // 0x00616EBC: IoStartPacket@12
    t[80]  = fn(&Shim_IofCompleteRequest);           // 0x00616EC0: @IofCompleteRequest@8
    t[81]  = fn(&Stub_Ret4);                         // 0x00616EC4: IoStartNextPacket@4
    t[82]  = fn(&Shim_MmSetAddressProtect);          // 0x00616EC8: MmSetAddressProtect@12
    t[83]  = fn(&Shim_MmQueryAddressProtect);        // 0x00616ECC: MmQueryAddressProtect@4
    t[84]  = fn(&Stub_Ret4);                         // 0x00616ED0: IoMarkIrpMustComplete@4
    t[85]  = fn(&Stub_Ret0);                         // 0x00616ED4: HalIsResetOrShutdownPending@0
    t[86]  = fn(&Shim_RtlCompareMemoryUlong);        // 0x00616ED8: RtlCompareMemoryUlong@12
    t[87]  = fn(&Shim_RtlInitializeCriticalSection); // 0x00616EDC: RtlInitializeCriticalSection@4
    t[88]  = fn(&Stub_Ret0);                         // 0x00616EE0: HalInitiateShutdown@0
    t[89]  = fn(&Stub_Ret8);                         // 0x00616EE4: HalGetInterruptVector@8
    t[90]  = fn(&Shim_KfRaiseIrql);                  // 0x00616EE8: @KfRaiseIrql@4
    t[91]  = fn(&Stub_Ret8);                         // 0x00616EEC: HalRegisterShutdownNotification@8
    t[92]  = fn(&Stub_Ret4);                         // 0x00616EF0: KeConnectInterrupt@4
    t[93]  = fn(&Stub_Ret28);                        // 0x00616EF4: KeInitializeInterrupt@28
    t[94]  = fn(&Stub_Ret4);                         // 0x00616EF8: KeStallExecutionProcessor@4
    t[95]  = fn(&Shim_RtlEqualString);               // 0x00616EFC: RtlEqualString@12
    t[96]  = fn(&Shim_XeLoadSection);                // 0x00616F00: XeLoadSection@4
    t[97]  = fn(&Shim_XeUnloadSection);              // 0x00616F04: XeUnloadSection@4
    t[98]  = fn(&Shim_MmGetPhysicalAddress);         // 0x00616F08: MmGetPhysicalAddress@4
    t[99]  = fn(&Shim_MmLockUnlockBufferPages);      // 0x00616F0C: MmLockUnlockBufferPages@12
    t[100] = fn(&Shim_KeInsertQueueDpc);             // 0x00616F10: KeInsertQueueDpc@12
    t[101] = fn(&Shim_MmLockUnlockPhysicalPage);     // 0x00616F14: MmLockUnlockPhysicalPage@8
    t[102] = fn(&Shim_MmAllocateContiguousMemoryEx); // 0x00616F18: MmAllocateContiguousMemoryEx@20
    t[103] = fn(&Shim_AvGetSavedDataAddress);        // 0x00616F1C: AvGetSavedDataAddress@0
    t[104] = fn(&Shim_AvSendTVEncoderOption);        // 0x00616F20: AvSendTVEncoderOption@16
    t[105] = fn(&Shim_MmQueryAllocationSize);        // 0x00616F24: MmQueryAllocationSize@4
    t[106] = fn(&Shim_AvSetDisplayMode);             // 0x00616F28: AvSetDisplayMode@24
    t[107] = fn(&Shim_AvSetSavedDataAddress);        // 0x00616F2C: AvSetSavedDataAddress@4
    t[108] = fn(&Shim_DbgPrint);                     // 0x00616F30: DbgPrint (cdecl varargs)
    t[109] = fn(&Stub_Ret24);                        // 0x00616F34: HalReadWritePCISpace@24
    t[110] = fn(&Stub_Ret4);                         // 0x00616F38: KeDisconnectInterrupt@4
    t[111] = fn(&Shim_MmClaimGpuInstanceMemory);     // 0x00616F3C: MmClaimGpuInstanceMemory@8
    t[112] = fn(&Stub_Ret4);                         // 0x00616F40: KeSaveFloatingPointState@4
    t[113] = fn(&Stub_Ret4);                         // 0x00616F44: KeRestoreFloatingPointState@4
    t[114] = fn(&Shim_ExQueryPoolBlockSize);         // 0x00616F48: ExQueryPoolBlockSize@4
    t[115] = fn(&Shim_ExAllocatePoolWithTag);        // 0x00616F4C: ExAllocatePoolWithTag@8
    t[116] = fn(&Shim_KeRemoveQueueDpc);             // 0x00616F50: KeRemoveQueueDpc@4
    t[117] = fn(&Stub_Ret12);                        // 0x00616F54: KeSynchronizeExecution@12
    t[118] = fn(&Stub_Ret4);                         // 0x00616F58: KeBugCheck@4
    t[119] = fn(&Stub_Ret16);                        // 0x00616F5C: RtlUnwind@16
}

} // namespace xboxkrnl
