#pragma once
#include "xboxkrnl_win32.h"
#include <cstdint>

namespace d3d9_bridge {

// Initialize hardware Direct3D 9 device with internal HD resolution scaling, MSAA, and 16x AF
bool Init(HWND hRenderWnd, uint32_t xboxWidth, uint32_t xboxHeight, uint32_t resScale = 3);
void Shutdown();
bool IsReady();

// Optional HD Texture Replacement Pack & Texture Dumper
void SetTexturePackDirs(const char* hdTexturesDir, const char* dumpTexturesDir);
void SetHDTexturesEnabled(bool enabled);
bool IsHDTexturesEnabled();
void SetTextureDumpEnabled(bool enabled);
bool IsTextureDumpEnabled();
bool ToggleTextureDump();
void ReloadHDTextures();

// Live graphics settings (applied cleanly on next Present)
// aspectMode: 0 = 4:3 Classic, 1 = 16:9 Widescreen, 2 = 21:9 Ultrawide (3440x1440)
void RequestGraphicsSettings(uint32_t resScale, uint32_t msaaSamples, uint32_t maxAniso, uint32_t aspectMode = 0);
uint32_t GetResolutionScale();
uint32_t GetMsaaSamples();
uint32_t GetAnisoLevel();
uint32_t GetAspectMode();
uint32_t GetOutputWidth();
uint32_t GetOutputHeight();
void SetFogEnabled(bool enabled);
bool IsFogEnabled();
void SetMenuPillarbox(bool enabled);
void RequestFrameTrace();

// Core Xbox D3D8 -> Win32 D3D9 bridged operations
void Clear(uint32_t count, const void* pRects, uint32_t flags, uint32_t color, float z, uint32_t stencil);
void Swap(uint32_t flags);
void SetViewport(const void* pXboxViewport);
void SetTransform(uint32_t xboxState, const void* pMatrix);
void SetVertexShader(uint32_t handle);
void RegisterVertexShaderDecl(uint32_t handle, const uint32_t* pDecl, int32_t shaderId = -1);
void SetStreamSource(uint32_t streamNumber, const void* pXboxVB, uint32_t stride);
void SetIndices(const void* pXboxIB, uint32_t baseVertexIndex);
void SetTexture(uint32_t stage, const void* pXboxTexture);
void SetPalette(uint32_t stage, const void* pXboxPalette);
void SetRenderState(uint32_t xboxState, uint32_t value);
void SetRenderState_CullMode(uint32_t value);
void SetRenderState_ZEnable(uint32_t value);
void SetRenderState_TextureFactor(uint32_t value);
void SetRenderState_FogColor(uint32_t value);
void SetTextureStageState(uint32_t stage, uint32_t xboxType, uint32_t value);

void DrawVerticesUP(uint32_t xboxPrimType, uint32_t vertexCount, const void* pVertexData, uint32_t stride);
void DrawIndexedVerticesUP(uint32_t xboxPrimType, uint32_t vertexCount, const void* pIndexData, const void* pVertexData, uint32_t stride);
void DrawIndexedVertices(uint32_t xboxPrimType, uint32_t vertexCount, const void* pIndexData);

} // namespace d3d9_bridge
