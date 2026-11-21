#pragma once
#include <windows.h>
#include <cstdint>

namespace dsound_bridge {

// Install all Xbox DirectSound8 / Buffer / Stream detours in Default.xbe (0x005A6D13..0x005A8D4E)
void InstallDetours();

// Shutdown the Windows waveOut audio mixer cleanly on exit / relaunch
void Shutdown();

// Master volume (0..100%), mute, and mute-in-background controls
void SetMuted(bool muted);
bool IsMuted();
void SetMasterVolume(int percent);
int  GetMasterVolume();
void SetMuteInBackground(bool enabled);
bool IsMuteInBackground();

} // namespace dsound_bridge
