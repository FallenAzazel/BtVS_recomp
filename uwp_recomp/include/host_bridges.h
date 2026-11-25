#pragma once
#include "x86_cpu.h"

namespace uwp_recomp {
namespace host_bridges {

// Configure virtual drive roots (D:\ -> extracted, Z:\ -> cache, T:\ & U:\ -> saves)
void SetDriveRoots(const char* dRoot, const char* zRoot, const char* tRoot);

// Register all portable x64 host bridges for Xbox Kernel, XAPI, CRT, D3D8->D3D11 & DSound->XAudio2
void RegisterAll();

// Clean up open guest handles, worker threads, and cached guest-heap pointers for in-process relaunch
void ResetForRelaunch();

} // namespace host_bridges
} // namespace uwp_recomp
