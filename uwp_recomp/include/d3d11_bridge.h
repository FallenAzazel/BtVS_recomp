#pragma once
#include <cstdint>

namespace uwp_recomp {
namespace d3d11_bridge {

// UWP & Win64 Direct3D 11 Hardware Renderer (replaces d3d9_bridge for Xbox Dev Mode!)
// - On Win64 desktop: pass HWND as nativeWindow
// - On Xbox UWP: pass IUnknown* (CoreWindow) as nativeWindow with isCoreWindow=true
bool Init(void* nativeWindow, uint32_t width, uint32_t height, bool isCoreWindow = false);
void Shutdown();
void ResetForRelaunch();
void Resize(uint32_t width, uint32_t height);
bool IsReady();
uint32_t GetWidth();
uint32_t GetHeight();
void SetWidescreenHorPlus(bool enabled);
bool GetWidescreenHorPlus();
void SetEnhancedLighting(bool enabled);
void SetCharAmbient(float val);
void SetVsyncEnabled(bool enabled);
void SetSsaoEnabled(bool enabled);
void SetHdrBloomEnabled(bool enabled);
void SetCharRimSpecEnabled(bool enabled);

// Lightweight UWP-safe 2D overlay drawing (640x480 virtual coordinates, auto-scaled to any resolution)
void DrawOverlayRect(float x, float y, float w, float h, uint32_t argb);
void DrawOverlayText(float x, float y, float scale, uint32_t argb, const char* text);

void Clear(uint32_t xboxFlags, uint32_t argbColor, float depth = 1.0f);
void Present(bool vsync = true);
uint32_t GetFrameCount();
void ConfigureCapture(const char* outDir, const uint32_t* frames, uint32_t count, uint32_t exitAfterFrame, uint32_t traceFrame);
void RequestFrameTrace();

// Xbox D3D8 -> D3D11 bridged draw & state operations (reads directly from flat 64 MB guest RAM)
void SetViewport(uint32_t x, uint32_t y, uint32_t w, uint32_t h, float minZ, float maxZ);
void SetTransform(uint32_t xboxState, const float* mat16);
void RegisterVertexShaderDecl(uint32_t handle, const uint32_t* pDecl, bool isProgrammable);
void SetVertexShader(uint32_t handle);
void SetStreamSource(uint32_t streamNumber, const uint8_t* guestRam, uint32_t vbVA, uint32_t stride);
void SetIndices(const uint8_t* guestRam, uint32_t ibVA, uint32_t baseVertexIndex);
void SetTexture(uint32_t stage, const uint8_t* guestRam, uint32_t xtexVA);
void SetPalette(uint32_t stage, const uint8_t* guestRam, uint32_t xpalVA);
void InvalidateTexture(uint32_t dataVA);

void DrawVerticesUP(uint32_t xboxPrimType, uint32_t vertexCount, const uint8_t* vdata, uint32_t stride);
void DrawIndexedVerticesUP(uint32_t xboxPrimType, uint32_t vertexCount, const uint16_t* idx, const uint8_t* vdata, uint32_t stride);
void DrawIndexedVertices(uint32_t xboxPrimType, uint32_t vertexCount, const uint8_t* guestRam, uint32_t idxParam);

} // namespace d3d11_bridge
} // namespace uwp_recomp
