#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>

void LogPrintf(const char* fmt, ...);

namespace xboxkrnl {

struct ANSI_STRING {
    uint16_t Length;
    uint16_t MaximumLength;
    char*    Buffer;
};

struct UNICODE_STRING {
    uint16_t Length;
    uint16_t MaximumLength;
    wchar_t* Buffer;
};

struct OBJECT_ATTRIBUTES {
    HANDLE       RootDirectory;
    ANSI_STRING* ObjectName;
    uint32_t     Attributes;
};

struct IO_STATUS_BLOCK {
    int32_t   Status;
    uintptr_t Information;
};

struct MM_STATISTICS {
    uint32_t Length;
    uint32_t TotalPhysicalPages;
    uint32_t AvailablePages;
    uint32_t VirtualMemoryBytesCommitted;
    uint32_t VirtualMemoryBytesReserved;
    uint32_t CachePagesCommitted;
    uint32_t PoolPagesCommitted;
    uint32_t StackPagesCommitted;
    uint32_t ImagePagesCommitted;
};

struct XBOX_HARDWARE_INFO {
    uint32_t Flags;
    uint8_t  GpuRevision;
    uint8_t  McpRevision;
    uint8_t  Reserved[2];
};

struct TIME_FIELDS {
    int16_t Year;
    int16_t Month;
    int16_t Day;
    int16_t Hour;
    int16_t Minute;
    int16_t Second;
    int16_t Milliseconds;
    int16_t Weekday;
};

// Initialize Xbox virtual drive mappings (D:\ -> extracted/, Z:\ -> cache/, T:\ & U:\ -> saves/)
void InitVirtualDrives(const char* extractedRoot, const char* cacheRoot, const char* savesRoot);

// Translate an Xbox NT/DOS path (e.g. "\Device\CdRom0\Default.xbe", "D:\pak\res_hub01.pak", "Z:\...") to a host Win32 path
bool TranslateXboxPath(const char* xboxPath, char* outWinPath, size_t maxLen);

// Populate the complete 120-entry Xbox Kernel Thunk Table at VA 0x00616D80..0x00616F5C
void InstallKernelThunkTable(uint32_t* thunkTableBase);

// Core Xbox Kernel Shim Functions (stdcall / fastcall matching XDK 4432 ABI)
void*    __stdcall Shim_MmAllocateContiguousMemory(uint32_t NumberOfBytes);
void*    __stdcall Shim_MmAllocateContiguousMemoryEx(uint32_t NumberOfBytes, uint32_t LowestAcceptableAddress, uint32_t HighestAcceptableAddress, uint32_t Alignment, uint32_t ProtectionType);
void     __stdcall Shim_MmFreeContiguousMemory(void* BaseAddress);
uint32_t __stdcall Shim_MmQueryAllocationSize(void* BaseAddress);
int32_t  __stdcall Shim_MmQueryStatistics(MM_STATISTICS* Stats);
void*    __stdcall Shim_ExAllocatePool(uint32_t NumberOfBytes);
void*    __stdcall Shim_ExAllocatePoolWithTag(uint32_t NumberOfBytes, uint32_t Tag);
void     __stdcall Shim_ExFreePool(void* P);
int32_t  __stdcall Shim_ExQueryNonVolatileSetting(uint32_t ValueIndex, uint32_t* Type, void* Value, uint32_t ValueLength, uint32_t* ResultLength);
void     __stdcall Shim_RtlInitAnsiString(ANSI_STRING* DestinationString, const char* SourceString);
int32_t  __stdcall Shim_XeLoadSection(void* SectionHeader);
int32_t  __stdcall Shim_XeUnloadSection(void* SectionHeader);
void     __stdcall Shim_AvSetDisplayMode(void* RegisterBase, uint32_t Step, uint32_t Mode, uint32_t Format, uint32_t Pitch, uint32_t FrameBuffer);
void     __stdcall Shim_XcSHAInit(uint8_t* pbSHAContext);
void     __stdcall Shim_XcSHAUpdate(uint8_t* pbSHAContext, const uint8_t* pbInput, uint32_t dwInputLength);
void     __stdcall Shim_XcSHAFinal(uint8_t* pbSHAContext, uint8_t* pbDigest);

// Safe stdcall no-op stubs with exact stack cleanup
uint32_t __stdcall Stub_Ret0();
uint32_t __stdcall Stub_Ret4(uint32_t);
uint32_t __stdcall Stub_Ret8(uint32_t, uint32_t);
uint32_t __stdcall Stub_Ret12(uint32_t, uint32_t, uint32_t);
uint32_t __stdcall Stub_Ret16(uint32_t, uint32_t, uint32_t, uint32_t);
uint32_t __stdcall Stub_Ret20(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
uint32_t __stdcall Stub_Ret24(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
uint32_t __stdcall Stub_Ret28(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
uint32_t __stdcall Stub_Ret32(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
uint32_t __stdcall Stub_Ret36(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
uint32_t __stdcall Stub_Ret40(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

} // namespace xboxkrnl
