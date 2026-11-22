#pragma once
#include "xboxkrnl_win32.h"
#include <cstdint>

namespace xbe_loader {

// Register the Win32 window and framebuffer callback so D3DDevice_Swap/Clear/Draw can present live frames
void SetPresentationWindow(HWND hWnd, uint32_t* framebuffer640x480);

// Load decompile/functions.tsv for symbol-resolved exception & callstack logging
void LoadSymbolTable(const char* functionsTsvPath);

// Resolve an Xbox VA (0x00010000..0x00921000) to "Module!Function+0xOffset"
void FormatXboxSymbol(uint32_t va, char* outBuf, size_t maxLen);

// Map Default.xbe into 0x00010000..0x00921000, install 120 kernel thunks, and apply Win32 detours
bool LoadAndBridgeDefaultXbe(const char* xbePath);

// Execute Xbox CRT initialization (__rtinit, __cinit for 470 C++ constructors) and jump into _main (0x00064F90)
int ExecuteXboxMain();

// Quick Save / Quick Load (5 slots: 1..5) / Auto-Resume into exact saved gameplay spot
void RequestQuickSave(int slot = 0); // 0 = active slot
void RequestQuickLoad(int slot = 0); // 0 = active slot
void SetActiveQuickSaveSlot(int slot);
int  GetActiveQuickSaveSlot();
bool HasQuickSaveInSlot(int slot);
void SetAutoResumeQuickSave(bool enabled);
void SetSubtitlesEnabled(bool enabled);
bool IsSubtitlesEnabled();
void SetAutoPauseUnfocused(bool enabled);
void SetSkipIntroMovies(bool enabled);
void SetCheats(bool infHealth, bool infPower, bool infAmmoDurability, bool oneHitDust);
void ShowStatusNotice(const char* msg);

// Chapter / Arena Launcher (relaunch straight into any mission, hub, or arena via LAUNCH_DATA)
void RequestJumpToLevel(const char* mapName);

// Buffy Costume Override (0=Level Default, 1=Slayer, 2=Cheerleader, 3=The Bronze, 4=Workout)
void     SetBuffyCostume(uint32_t costumeId);
uint32_t GetBuffyCostume();
void     RequestCostumeReloadInPlace();

// High-precision frame pacer (0 = uncapped, 30 / 60 / 120 / 165 FPS) with exact 1.0x QPC real-time speed
void     SetTargetFps(uint32_t fps);
uint32_t GetTargetFps();
uint32_t GetMeasuredFps();

// Live Camera FOV Offset in degrees (-15..+25, applied on top of 4:3 / 16:9 / 21:9 Hor+ FOV during gameplay)
void     SetFovOffsetDeg(int offsetDeg);
int      GetFovOffsetDeg();

// Controller, Mouse-Look & Custom Keybind settings
void     SetRumbleEnabled(bool enabled);
bool     IsRumbleEnabled();
void     SetDeadzonePercent(int percent);
int      GetDeadzonePercent();
void     SetCameraControlSettings(bool mouseLook, int sensitivityPercent, bool invertX, bool invertY);
void     SetMouseLookCaptured(bool captured);
void     AddMouseLookDelta(int dx, int dy);
void     SetKeybinds(const int vkCodes[10]);
bool     IsGamepadConnected();
uint32_t GetActivePadSlot();
const char* GetQuickStatusText();

} // namespace xbe_loader
