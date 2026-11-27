#include "d3d11_bridge.h"
#include "uwp_config_ui.h"
#include "x86_cpu.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_5.h>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace uwp_recomp {
namespace d3d11_bridge {

struct UwpVert {
  float x, y, z;
  float r, g, b, a;
  float u, v, w0;
  float u1, v1, w1;
};

struct alignas(16) CbPerDraw {
  float mWorld[16];
  float mView[16];
  float mProj[16];
  float screenParams[4]; // x=640, y=480, z=isRhw, w=hasTex0
  float tfactor[4];      // (r, g, b, a) from Xbox D3DRS_TEXTUREFACTOR (rs[130])
  float tssMode[4]; // x=useTfRgb, y=useTfAlpha, z=alphaTestRef, w=stage0Mul (1,
                    // 2, 4, or -1 for SELECT_TEX)
  float stage1Mode[4]; // x=hasTex1, y=cOp1 (4=MOD, 5=MOD2X, 6=MOD4X, 7=ADD),
                       // z=charAmbientFloor, w=volMask (1=stage0 3D, 2=stage1 3D)
  float uvScale[4];  // x=u0Scale, y=v0Scale, z=u1Scale, w=v1Scale (1/W, 1/H for
                     // linear Xbox textures!)
  float fogColor[4]; // rgb=fogColor, a=fogEnabled (0 or 1)
  float fogParams[4]; // x=fogStart, y=fogEnd, z=waterMode(0/1/2), w=timeSec
  float mTex0[16];    // Stage 0 texture transform matrix (D3DTS_TEXTURE0)
  float mTex1[16];    // Stage 1 texture transform matrix (D3DTS_TEXTURE1)
  float alphaParams[4]; // x=alphaMode (0=MOD, 1=SEL_TEX, 2=SEL_DIFF, 3=ONE)
};

struct CachedD3D11Tex {
  ID3D11Resource *tex = nullptr;
  ID3D11ShaderResourceView *srv = nullptr;
  uint32_t dataVA = 0;
  uint32_t format = 0;
  uint32_t size = 0;
  uint32_t palVA = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  bool isVol = false;
};

static ID3D11Device *g_dev = nullptr;
static ID3D11DeviceContext *g_ctx = nullptr;
static IDXGISwapChain1 *g_swapChain = nullptr;
static bool g_allowTearing = false;
static ID3D11RenderTargetView *g_rtv = nullptr;
static ID3D11DepthStencilView *g_dsv = nullptr;
static ID3D11ShaderResourceView *g_depthSrv = nullptr;
static ID3D11Texture2D *g_sceneTex = nullptr;
static ID3D11RenderTargetView *g_sceneRtv = nullptr;
static ID3D11ShaderResourceView *g_sceneSrv = nullptr;
static ID3D11Texture2D *g_ssaoTex = nullptr;
static ID3D11RenderTargetView *g_ssaoRtv = nullptr;
static ID3D11ShaderResourceView *g_ssaoSrv = nullptr;
static ID3D11Texture2D *g_bloomATex = nullptr;
static ID3D11RenderTargetView *g_bloomARtv = nullptr;
static ID3D11ShaderResourceView *g_bloomASrv = nullptr;
static ID3D11Texture2D *g_bloomBTex = nullptr;
static ID3D11RenderTargetView *g_bloomBRtv = nullptr;
static ID3D11ShaderResourceView *g_bloomBSrv = nullptr;
static uint32_t g_ssaoW = 1, g_ssaoH = 1;
static uint32_t g_bloomW = 1, g_bloomH = 1;
static ID3D11VertexShader *g_vsPost = nullptr;
static ID3D11PixelShader *g_psSsao = nullptr;
static ID3D11PixelShader *g_psBloomExtract = nullptr;
static ID3D11PixelShader *g_psBloomBlur = nullptr;
static ID3D11PixelShader *g_psComposite = nullptr;
static ID3D11Buffer *g_cbPost = nullptr;
static ID3D11SamplerState *g_sampLinearClamp = nullptr;
static ID3D11SamplerState *g_sampPointClamp = nullptr;
static ID3D11SamplerState *g_sampVolBorder = nullptr;
static bool g_sceneHas3D = false;
static bool g_postResolved = false;
static bool g_boundToSceneRt = false;
static float g_last3DProj[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

struct alignas(16) CbPost {
  float invRes[4];     // x=1/w, y=1/h, z=1/ssaoW, w=1/ssaoH
  float projParams[4]; // x=m0, y=m5, z=m10, w=m14
  float projExtra[4];  // x=m11, y=m15, z=ssaoEnabled(0/1), w=bloomEnabled(0/1)
  float blurDir[4];    // x=dx, y=dy, z=threshold, w=bloomIntensity
};
static ID3D11VertexShader *g_vs = nullptr;
static ID3D11PixelShader *g_ps = nullptr;
static ID3D11InputLayout *g_layout = nullptr;
static ID3D11Buffer *g_cb = nullptr;
static ID3D11Buffer *g_vb = nullptr;
static ID3D11Buffer *g_ib = nullptr;
static uint32_t g_vbCap = 0;
static uint32_t g_ibCap = 0;
static uint32_t g_vbCursor = 0;
static uint32_t g_ibCursor = 0;
static uint32_t g_frameVerts = 0;
static uint32_t g_frameVbDiscards = 0;
static uint32_t g_accTexCalls = 0;
static uint32_t g_accTexUploads = 0;
static int64_t g_accDrawTicks = 0;
static int64_t g_accTexTicks = 0;
static int64_t g_accPresentTicks = 0;
static LARGE_INTEGER g_qpcFreq{};
struct ScopedTickAcc {
  int64_t &acc;
  LARGE_INTEGER t0{};
  explicit ScopedTickAcc(int64_t &a) : acc(a) { QueryPerformanceCounter(&t0); }
  ~ScopedTickAcc() { LARGE_INTEGER t1{}; QueryPerformanceCounter(&t1); acc += (t1.QuadPart - t0.QuadPart); }
};
static bool g_frameBound = false;
static D3D11_PRIMITIVE_TOPOLOGY g_lastTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
static ID3D11RasterizerState *g_lastRs = nullptr;
static ID3D11DepthStencilState *g_lastDs = nullptr;
static ID3D11BlendState *g_lastBs = nullptr;
static ID3D11SamplerState *g_lastSamps[2] = {};
static ID3D11ShaderResourceView *g_lastSrvs[4] = {};
static int g_lastVpMode = -1; // -1=unset, 0=3D, 1=RHW
static uint32_t g_lastBlendKey = 0xFFFFFFFFu;
static ID3D11BlendState *g_lastBlendVal = nullptr;

static void ResetPipelineCache() {
  g_frameBound = false;
  g_lastTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  g_lastRs = nullptr;
  g_lastDs = nullptr;
  g_lastBs = nullptr;
  g_lastSamps[0] = g_lastSamps[1] = nullptr;
  g_lastSrvs[0] = g_lastSrvs[1] = g_lastSrvs[2] = g_lastSrvs[3] = nullptr;
  g_lastVpMode = -1;
}
static ID3D11SamplerState *g_sampler = nullptr;
static ID3D11BlendState *g_blendOpaque = nullptr;
static ID3D11BlendState *g_blendAlpha = nullptr;
static ID3D11BlendState *g_blendAdd = nullptr;
static ID3D11BlendState *g_blendAddOne = nullptr;
static std::unordered_map<uint32_t, ID3D11BlendState *> g_blendCache;

static D3D11_BLEND MapXboxBlend11(uint32_t v) {
  switch (v) {
  case 0x0000:
    return D3D11_BLEND_ZERO;
  case 0x0001:
    return D3D11_BLEND_ONE;
  case 0x0300:
    return D3D11_BLEND_SRC_COLOR;
  case 0x0301:
    return D3D11_BLEND_INV_SRC_COLOR;
  case 0x0302:
    return D3D11_BLEND_SRC_ALPHA;
  case 0x0303:
    return D3D11_BLEND_INV_SRC_ALPHA;
  case 0x0304:
    return D3D11_BLEND_DEST_ALPHA;
  case 0x0305:
    return D3D11_BLEND_INV_DEST_ALPHA;
  case 0x0306:
    return D3D11_BLEND_DEST_COLOR;
  case 0x0307:
    return D3D11_BLEND_INV_DEST_COLOR;
  case 0x0308:
    return D3D11_BLEND_SRC_ALPHA_SAT;
  default:
    return D3D11_BLEND_ONE;
  }
}

static ID3D11BlendState *GetXboxBlendState(uint32_t srcBlend,
                                           uint32_t destBlend) {
  if (srcBlend == 1u && destBlend == 0u)
    return g_blendOpaque; // D3DBLEND_ONE + D3DBLEND_ZERO == solid opaque!
  uint32_t key = ((srcBlend & 0xFFFFu) << 16) | (destBlend & 0xFFFFu);
  auto it = g_blendCache.find(key);
  if (it != g_blendCache.end())
    return it->second;
  D3D11_BLEND_DESC bd{};
  bd.RenderTarget[0].BlendEnable = TRUE;
  bd.RenderTarget[0].SrcBlend = MapXboxBlend11(srcBlend);
  bd.RenderTarget[0].DestBlend = MapXboxBlend11(destBlend);
  bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
  bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  ID3D11BlendState *bs = nullptr;
  if (SUCCEEDED(g_dev->CreateBlendState(&bd, &bs)) && bs) {
    g_blendCache[key] = bs;
    return bs;
  }
  return g_blendAlpha;
}
static ID3D11DepthStencilState *g_dsOn = nullptr;
static ID3D11DepthStencilState *g_dsNoWrite = nullptr;
static ID3D11DepthStencilState *g_dsOff = nullptr;
static ID3D11RasterizerState *g_rsCullNone = nullptr;
static ID3D11RasterizerState *g_rsCullCW = nullptr;
static ID3D11RasterizerState *g_rsCullCCW = nullptr;
static std::unordered_map<uint32_t, ID3D11SamplerState *> g_samplerCache;

static D3D11_TEXTURE_ADDRESS_MODE MapXboxAddr11(uint32_t v) {
  switch (v) {
  case 1:
    return D3D11_TEXTURE_ADDRESS_WRAP;
  case 2:
    return D3D11_TEXTURE_ADDRESS_MIRROR;
  case 3:
    return D3D11_TEXTURE_ADDRESS_CLAMP;
  case 4:
    return D3D11_TEXTURE_ADDRESS_BORDER;
  case 5:
    return D3D11_TEXTURE_ADDRESS_CLAMP;
  default:
    return D3D11_TEXTURE_ADDRESS_WRAP;
  }
}

static ID3D11SamplerState *GetXboxSampler(uint32_t addrU, uint32_t addrV,
                                          uint32_t addrW = 3u, bool isVol = false) {
  uint32_t key = ((addrU & 0xFu) << 8) | ((addrV & 0xFu) << 4) | (addrW & 0xFu) | (isVol ? 0x1000u : 0u);
  auto it = g_samplerCache.find(key);
  if (it != g_samplerCache.end())
    return it->second;
  D3D11_SAMPLER_DESC sd{};
  sd.Filter = isVol ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_ANISOTROPIC;
  sd.AddressU = MapXboxAddr11(addrU);
  sd.AddressV = MapXboxAddr11(addrV);
  sd.AddressW = MapXboxAddr11(addrW);
  sd.MaxAnisotropy = isVol ? 1 : 16;
  sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
  sd.BorderColor[0] = sd.BorderColor[1] = sd.BorderColor[2] = sd.BorderColor[3] = 0.0f;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  ID3D11SamplerState *ss = nullptr;
  if (SUCCEEDED(g_dev->CreateSamplerState(&sd, &ss)) && ss) {
    g_samplerCache[key] = ss;
    return ss;
  }
  return g_sampler;
}

static uint32_t g_width = 1280;
static uint32_t g_height = 720;

static float g_mWorld[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
static float g_mView[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
static float g_mProj[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
static float g_mTex0[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
static float g_mTex1[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
static bool  g_hasTexMat[2] = {false, false};
static uint32_t g_fvf = 0x144u; // XYZRHW | DIFFUSE | TEX1

static const uint8_t *g_streams[4] = {};
static uint32_t g_strides[4] = {};
static const uint16_t *g_indices = nullptr;
static uint32_t g_baseVert = 0;
static ID3D11ShaderResourceView *g_boundSrv[4] = {};
static uint32_t g_boundPalVA[4] = {};
static uint32_t g_boundTexVA[4] = {};
static bool g_boundTexVol[4] = {};
static bool g_boundTexLin[4] = {};
static uint32_t g_boundTexW[4] = {};
static uint32_t g_boundTexH[4] = {};
static uint32_t g_boundTexFmt[4] = {};

static std::unordered_map<uint64_t, CachedD3D11Tex> g_texCache;
static inline uint64_t MakeTexCacheKey(uint32_t dataVA, uint32_t palVA, uint32_t format, uint32_t sizeField, bool isVol) {
  uint64_t h = (static_cast<uint64_t>(dataVA) << 32) | static_cast<uint64_t>(palVA);
  h ^= (static_cast<uint64_t>(format) * 0x9E3779B97F4A7C15ULL);
  h ^= (static_cast<uint64_t>(sizeField) * 0xBF58476D1CE4E5B9ULL);
  if (isVol) h ^= 0x55AA55AA55AA55AAULL;
  return h;
}
static std::vector<UwpVert> g_vertScratch;
static std::vector<uint16_t> g_idxScratch;

static void Unswizzle2D(const uint8_t *src, uint8_t *dst, uint32_t width,
                        uint32_t height, uint32_t bpp) {
  uint32_t maskU = 0, maskV = 0, j = 1, w = width, h = height;
  while (w > 1 || h > 1) {
    if (w > 1) {
      maskU |= j;
      j <<= 1;
      w >>= 1;
    }
    if (h > 1) {
      maskV |= j;
      j <<= 1;
      h >>= 1;
    }
  }
  uint32_t v = 0;
  for (uint32_t y = 0; y < height; ++y) {
    uint32_t u = 0;
    uint8_t *dstRow = dst + y * width * bpp;
    for (uint32_t x = 0; x < width; ++x) {
      std::memcpy(dstRow + x * bpp, src + (u | v) * bpp, bpp);
      u = (u - maskU) & maskU;
    }
    v = (v - maskV) & maskV;
  }
}

static bool InitShadersAndStates() {
  static const char *kUberHlsl =
      "cbuffer CbPerDraw : register(b0) {\n"
      "    row_major float4x4 mWorld;\n"
      "    row_major float4x4 mView;\n"
      "    row_major float4x4 mProj;\n"
      "    float4 screenParams;\n"
      "    float4 tfactor;\n"
      "    float4 tssMode;\n"
      "    float4 stage1Mode;\n"
      "    float4 uvScale;\n"
      "    float4 fogColor;\n"
      "    float4 fogParams;\n"
      "    row_major float4x4 mTex0;\n"
      "    row_major float4x4 mTex1;\n"
      "    float4 alphaParams;\n"
      "};\n"
      "struct VSIn {\n"
      "    float3 pos : POSITION;\n"
      "    float4 col : COLOR0;\n"
      "    float3 uv  : TEXCOORD0;\n"
      "    float3 uv1 : TEXCOORD1;\n"
      "};\n"
      "struct PSIn {\n"
      "    float4 pos     : SV_POSITION;\n"
      "    float4 col     : COLOR0;\n"
      "    float3 uv      : TEXCOORD0;\n"
      "    float3 uv1     : TEXCOORD1;\n"
      "    float3 viewPos : TEXCOORD2;\n"
      "};\n"
      "PSIn VSMain(VSIn i) {\n"
      "    PSIn o;\n"
      "    if (screenParams.z > 0.5f) {\n"
      "        float nx = (i.pos.x / screenParams.x) * 2.0f - 1.0f;\n"
      "        float ny = 1.0f - (i.pos.y / screenParams.y) * 2.0f;\n"
      "        o.pos = float4(nx, ny, saturate(i.pos.z), 1.0f);\n"
      "        o.viewPos = float3(0.0f, 0.0f, 0.0f);\n"
      "    } else {\n"
      "        float4 p = float4(i.pos, 1.0f);\n"
      "        p = mul(p, mWorld);\n"
      "        p = mul(p, mView);\n"
      "        o.viewPos = p.xyz;\n"
      "        p = mul(p, mProj);\n"
      "        o.pos = p;\n"
      "    }\n"
      "    o.col = i.col;\n"
      "    int tciFlags = (int)(alphaParams.w + 0.5f);\n"
      "    float3 u0s = ((tciFlags & 1) != 0) ? o.viewPos : float3(i.uv.xy  * uvScale.xy, i.uv.z);\n"
      "    float3 u1s = ((tciFlags & 2) != 0) ? o.viewPos : float3(i.uv1.xy * uvScale.zw, i.uv1.z);\n"
      "    o.uv  = mul(float4(u0s, 1.0f), mTex0).xyz;\n"
      "    o.uv1 = mul(float4(u1s, 1.0f), mTex1).xyz;\n"
      "    return o;\n"
      "}\n"
      "Texture2D    t0 : register(t0);\n"
      "Texture2D    t1 : register(t1);\n"
      "Texture3D    v0 : register(t2);\n"
      "Texture3D    v1 : register(t3);\n"
      "SamplerState s0 : register(s0);\n"
      "SamplerState s1 : register(s1);\n"
      "float4 PSMain(PSIn i) : SV_TARGET {\n"
      "    if (fogParams.z > 0.5f) {\n"
      "        float t = fogParams.w;\n"
      "        bool enh = (fogParams.z > 1.5f);\n"
      "        float gust = 0.5f + 0.5f * sin(t * 0.55f) * cos(t * 0.23f);\n"
      "        float shimmer = 0.90f + (enh ? 0.12f : 0.08f) * sin(t * 1.4f + gust * 1.2f);\n"
      "        float3 tint = (enh ? float3(0.149f, 0.220f, 0.259f) : float3(0.141f, 0.204f, 0.235f)) * shimmer;\n"
      "        float2 u0 = i.uv.xy;\n"
      "        float2 uvA = u0;\n"
      "        if (enh) {\n"
      "            float sc0 = 1.15f + 0.03f * sin(t * 0.9f);\n"
      "            float c0 = 0.9781f * sc0, s0a = 0.2079f * sc0;\n"
      "            uvA = float2(u0.x * c0 - u0.y * s0a, u0.x * s0a + u0.y * c0)\n"
      "                + float2(t * 0.022f + 0.008f * sin(t * 0.7f), t * 0.014f + 0.006f * cos(t * 0.55f));\n"
      "        }\n"
      "        float2 uvB;\n"
      "        if (enh) {\n"
      "            float sc1 = 1.72f + 0.05f * cos(t * 1.3f);\n"
      "            float c1 = 0.7880f * sc1, s1a = -0.6157f * sc1;\n"
      "            uvB = float2(u0.x * c1 - u0.y * s1a, u0.x * s1a + u0.y * c1)\n"
      "                + float2(-t * 0.038f + 0.011f * sin(t * 1.25f), t * 0.029f + 0.009f * cos(t * 0.95f));\n"
      "        } else {\n"
      "            uvB = u0 * 1.35f + float2(t * 0.025f, -t * 0.018f);\n"
      "        }\n"
      "        float3 w0 = t0.Sample(s0, uvA).rgb;\n"
      "        float3 w1 = t0.Sample(s0, uvB).rgb;\n"
      "        float3 col = i.col.rgb * w0 * w1 * 2.0f * tint;\n"
      "        float alpha = i.col.a * 0.33f;\n"
      "        if (enh) {\n"
      "            float rScale = 0.0025f + 0.00018f * sin(t * 1.9f);\n"
      "            float2 rUv = i.viewPos.xz * rScale + float2(0.5f + 0.018f * sin(t * 2.2f) + 0.010f * cos(t * 1.1f),\n"
      "                                                        0.5f + 0.016f * cos(t * 1.8f) + 0.010f * sin(t * 0.85f));\n"
      "            float3 refl = (stage1Mode.x > 0.5f) ? t1.Sample(s1, rUv).rgb : t0.Sample(s0, rUv).rgb;\n"
      "            col += alpha * refl;\n"
      "        }\n"
      "        if (fogColor.a > 0.5f) {\n"
      "            float denom = max(fogParams.y - fogParams.x, 1.0f);\n"
      "            float f = saturate((fogParams.y - i.viewPos.z) / denom);\n"
      "            col = lerp(fogColor.rgb, col, f);\n"
      "        }\n"
      "        return float4(col, alpha);\n"
      "    }\n"
      "    float4 c = i.col;\n"
      "    if (stage1Mode.z > 0.0f) {\n"
      "        c.rgb = max(c.rgb, float3(stage1Mode.z, stage1Mode.z, "
      "stage1Mode.z * 1.12f));\n"
      "    }\n"
      "    int volMask = (int)(stage1Mode.w + 0.5f);\n"
      "    if (screenParams.w > 0.5f) {\n"
      "        float4 tex0 = ((volMask & 1) != 0) ? v0.Sample(s0, i.uv) : t0.Sample(s0, i.uv.xy);\n"
      "        if (tssMode.w < -0.5f) {\n"
      "            c.rgb = tex0.rgb;\n"
      "        } else {\n"
      "            c.rgb *= tex0.rgb;\n"
      "            if (tssMode.w > 1.5f) c.rgb *= tssMode.w;\n"
      "        }\n"
      "        if (alphaParams.x > 2.5f)      c.a = 1.0f;\n"
      "        else if (alphaParams.x > 1.5f) c.a = i.col.a;\n"
      "        else if (alphaParams.x > 0.5f) c.a = tex0.a;\n"
      "        else                           c.a *= tex0.a;\n"
      "    } else if (alphaParams.x > 2.5f) {\n"
      "        c.a = 1.0f;\n"
      "    }\n"
      "    if (tssMode.x > 0.5f) c.rgb *= tfactor.rgb;\n"
      "    if (tssMode.y > 0.5f) c.a   *= tfactor.a;\n"
      "    if (stage1Mode.x > 0.5f) {\n"
      "        float4 tex1 = ((volMask & 2) != 0) ? v1.Sample(s1, i.uv1) : t1.Sample(s1, i.uv1.xy);\n"
      "        if (stage1Mode.y > 6.5f)      c.rgb += tex1.rgb;\n"
      "        else if (stage1Mode.y > 5.5f) c.rgb *= tex1.rgb * 4.0f;\n"
      "        else if (stage1Mode.y > 4.5f) c.rgb *= tex1.rgb * 2.0f;\n"
      "        else                          c.rgb *= tex1.rgb;\n"
      "    }\n"
      "    if (tssMode.z > 0.0f && c.a < tssMode.z) discard;\n"
      "    if ((alphaParams.y > 0.001f || alphaParams.z > 0.001f) && screenParams.z < 0.5f) {\n"
      "        float3 dpX = ddx(i.viewPos);\n"
      "        float3 dpY = ddy(i.viewPos);\n"
      "        float3 rawN = cross(dpX, dpY);\n"
      "        float nLen = length(rawN);\n"
      "        if (nLen > 1e-6f) {\n"
      "            float3 N = rawN / nLen;\n"
      "            float3 V = normalize(-i.viewPos);\n"
      "            if (dot(N, V) < 0.0f) N = -N;\n"
      "            float luma = dot(c.rgb, float3(0.299f, 0.587f, 0.114f));\n"
      "            if (alphaParams.y > 0.001f) {\n"
      "                float ndotv = saturate(dot(N, V));\n"
      "                float rim = pow(1.0f - ndotv, 3.0f);\n"
      "                float upMask = saturate(N.y * 0.5f + 0.65f);\n"
      "                float3 rimTint = float3(0.58f, 0.70f, 0.95f) * (0.35f + 0.65f * saturate(luma));\n"
      "                c.rgb += rim * upMask * alphaParams.y * rimTint;\n"
      "            }\n"
      "            if (alphaParams.z > 0.001f) {\n"
      "                float3 bx = normalize(dpX + 1e-6f);\n"
      "                float3 by = normalize(dpY + 1e-6f);\n"
      "                float3 Nb = normalize(N - (ddx(luma) * bx + ddy(luma) * by) * 1.4f);\n"
      "                float3 L = normalize(float3(0.32f, 0.72f, -0.61f));\n"
      "                float3 H = normalize(L + V);\n"
      "                float spec = pow(saturate(dot(Nb, H)), 26.0f);\n"
      "                float specMask = smoothstep(0.10f, 0.85f, luma);\n"
      "                c.rgb += spec * specMask * alphaParams.z * float3(1.0f, 0.96f, 0.90f);\n"
      "            }\n"
      "        }\n"
      "    }\n"
      "    if (fogColor.a > 0.5f) {\n"
      "        float denom = max(fogParams.y - fogParams.x, 1.0f);\n"
      "        float f = saturate((fogParams.y - i.viewPos.z) / denom);\n"
      "        c.rgb = lerp(fogColor.rgb, c.rgb, f);\n"
      "    }\n"
      "    return c;\n"
      "}\n";

  ID3DBlob *vsBlob = nullptr;
  ID3DBlob *psBlob = nullptr;
  ID3DBlob *errBlob = nullptr;
  if (FAILED(D3DCompile(kUberHlsl, std::strlen(kUberHlsl), "uwp_uber.hlsl",
                        nullptr, nullptr, "VSMain", "vs_4_0", 0, 0, &vsBlob,
                        &errBlob))) {
    if (errBlob) {
      std::printf("[UWP-D3D11] VS err: %s\n",
                  static_cast<const char *>(errBlob->GetBufferPointer()));
      errBlob->Release();
    }
    return false;
  }
  if (errBlob) {
    errBlob->Release();
    errBlob = nullptr;
  }
  if (FAILED(D3DCompile(kUberHlsl, std::strlen(kUberHlsl), "uwp_uber.hlsl",
                        nullptr, nullptr, "PSMain", "ps_4_0", 0, 0, &psBlob,
                        &errBlob))) {
    if (errBlob) {
      std::printf("[UWP-D3D11] PS err: %s\n",
                  static_cast<const char *>(errBlob->GetBufferPointer()));
      errBlob->Release();
    }
    vsBlob->Release();
    return false;
  }
  if (errBlob)
    errBlob->Release();

  g_dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                            nullptr, &g_vs);
  g_dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                           nullptr, &g_ps);

  D3D11_INPUT_ELEMENT_DESC ied[4] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
       D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12,
       D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 28,
       D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, 40,
       D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  g_dev->CreateInputLayout(ied, 4, vsBlob->GetBufferPointer(),
                           vsBlob->GetBufferSize(), &g_layout);
  vsBlob->Release();
  psBlob->Release();

  D3D11_BUFFER_DESC cbd{};
  cbd.ByteWidth = sizeof(CbPerDraw);
  cbd.Usage = D3D11_USAGE_DYNAMIC;
  cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  g_dev->CreateBuffer(&cbd, nullptr, &g_cb);

  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_ANISOTROPIC;
  sd.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
  sd.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
  sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
  sd.MaxAnisotropy = 16;
  sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  g_dev->CreateSamplerState(&sd, &g_sampler);

  D3D11_BLEND_DESC bd{};
  bd.RenderTarget[0].BlendEnable = FALSE;
  bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  g_dev->CreateBlendState(&bd, &g_blendOpaque);

  bd.RenderTarget[0].BlendEnable = TRUE;
  bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
  bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
  bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  g_dev->CreateBlendState(&bd, &g_blendAlpha);

  bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
  g_dev->CreateBlendState(&bd, &g_blendAdd);

  bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
  g_dev->CreateBlendState(&bd, &g_blendAddOne);

  D3D11_DEPTH_STENCIL_DESC dsd{};
  dsd.DepthEnable = TRUE;
  dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  dsd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
  g_dev->CreateDepthStencilState(&dsd, &g_dsOn);
  dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  g_dev->CreateDepthStencilState(&dsd, &g_dsNoWrite);
  dsd.DepthEnable = FALSE;
  g_dev->CreateDepthStencilState(&dsd, &g_dsOff);

  D3D11_RASTERIZER_DESC rsd{};
  rsd.FillMode = D3D11_FILL_SOLID;
  rsd.CullMode = D3D11_CULL_NONE;
  rsd.DepthClipEnable = TRUE;
  g_dev->CreateRasterizerState(&rsd, &g_rsCullNone);
  rsd.CullMode = D3D11_CULL_FRONT; // Xbox CULLMODE_CW (0x900)
  g_dev->CreateRasterizerState(&rsd, &g_rsCullCW);
  rsd.CullMode = D3D11_CULL_BACK; // Xbox CULLMODE_CCW (0x901)
  g_dev->CreateRasterizerState(&rsd, &g_rsCullCCW);

  // Post-processing shaders (Fullscreen VS, SSAO, HDR Bloom Extract/Blur, Composite + Tone Map)
  static const char *kPostHlsl =
      "cbuffer CbPost : register(b0) {\n"
      "    float4 invRes;\n"
      "    float4 projParams;\n"
      "    float4 projExtra;\n"
      "    float4 blurDir;\n"
      "};\n"
      "Texture2D t0 : register(t0);\n"
      "Texture2D t1 : register(t1);\n"
      "Texture2D t2 : register(t2);\n"
      "Texture2D t3 : register(t3);\n"
      "SamplerState sLin : register(s0);\n"
      "SamplerState sPnt : register(s1);\n"
      "struct PostVSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
      "PostVSOut VSPost(uint id : SV_VertexID) {\n"
      "    PostVSOut o;\n"
      "    float2 uv = float2((id << 1) & 2, id & 2);\n"
      "    o.uv = uv;\n"
      "    o.pos = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f, 1.0f);\n"
      "    return o;\n"
      "}\n"
      "float LinearViewZ(float d) {\n"
      "    float denom = d * projExtra.x - projParams.z;\n"
      "    if (abs(denom) < 1e-6f) denom = (denom >= 0.0f) ? 1e-6f : -1e-6f;\n"
      "    return abs((projParams.w - d * projExtra.y) / denom);\n"
      "}\n"
      "float3 ReconstructViewPos(float2 uv, float d) {\n"
      "    float z = LinearViewZ(d);\n"
      "    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);\n"
      "    float m0 = (abs(projParams.x) > 1e-4f) ? projParams.x : 1.0f;\n"
      "    float m5 = (abs(projParams.y) > 1e-4f) ? projParams.y : 1.0f;\n"
      "    return float3(ndc.x * z / m0, ndc.y * z / m5, z);\n"
      "}\n"
      "float4 PSSsao(PostVSOut i) : SV_TARGET {\n"
      "    float d = t0.SampleLevel(sPnt, i.uv, 0).r;\n"
      "    if (d >= 0.9998f || d <= 0.0001f) return float4(1, 1, 1, 1);\n"
      "    float3 P = ReconstructViewPos(i.uv, d);\n"
      "    float2 st = invRes.zw;\n"
      "    float dR = t0.SampleLevel(sPnt, i.uv + float2(st.x, 0.0f), 0).r;\n"
      "    float dL = t0.SampleLevel(sPnt, i.uv - float2(st.x, 0.0f), 0).r;\n"
      "    float dU = t0.SampleLevel(sPnt, i.uv - float2(0.0f, st.y), 0).r;\n"
      "    float dD = t0.SampleLevel(sPnt, i.uv + float2(0.0f, st.y), 0).r;\n"
      "    float3 Px = (abs(dR - d) < abs(dL - d)) ? (ReconstructViewPos(i.uv + float2(st.x, 0.0f), dR) - P)\n"
      "                                            : (P - ReconstructViewPos(i.uv - float2(st.x, 0.0f), dL));\n"
      "    float3 Py = (abs(dD - d) < abs(dU - d)) ? (ReconstructViewPos(i.uv + float2(0.0f, st.y), dD) - P)\n"
      "                                            : (P - ReconstructViewPos(i.uv - float2(0.0f, st.y), dU));\n"
      "    float3 rawN = cross(Px, Py);\n"
      "    float nLen = length(rawN);\n"
      "    if (nLen < 1e-6f) return float4(1, 1, 1, 1);\n"
      "    float3 N = rawN / nLen;\n"
      "    if (dot(N, -P) < 0.0f) N = -N;\n"
      "    float aspect = invRes.x / max(invRes.y, 1e-6f);\n"
      "    float radY = 0.016f;\n"
      "    float2 radUv = float2(radY * aspect, radY);\n"
      "    float worldRad = P.z * radY * 2.0f / max(abs(projParams.y), 0.1f);\n"
      "    static const float kBayer16[16] = {\n"
      "         0.0f,  8.0f,  2.0f, 10.0f,\n"
      "        12.0f,  4.0f, 14.0f,  6.0f,\n"
      "         3.0f, 11.0f,  1.0f,  9.0f,\n"
      "        15.0f,  7.0f, 13.0f,  5.0f\n"
      "    };\n"
      "    uint bx = ((uint)i.pos.x) & 3u;\n"
      "    uint by = ((uint)i.pos.y) & 3u;\n"
      "    float rot = kBayer16[by * 4u + bx] * (6.2831853f / 16.0f);\n"
      "    float occ = 0.0f;\n"
      "    [unroll]\n"
      "    for (int k = 0; k < 12; ++k) {\n"
      "        float ang = rot + (float)k * 2.3999632f;\n"
      "        float r = sqrt(((float)k + 0.5f) / 12.0f);\n"
      "        float2 suv = i.uv + float2(cos(ang), sin(ang)) * radUv * r;\n"
      "        float sd = t0.SampleLevel(sPnt, suv, 0).r;\n"
      "        if (sd >= 0.9998f || sd <= 0.0001f) continue;\n"
      "        float3 Q = ReconstructViewPos(suv, sd);\n"
      "        float3 v = Q - P;\n"
      "        float dist = length(v);\n"
      "        if (dist > 1e-5f) {\n"
      "            float ndotv = dot(N, v / dist);\n"
      "            float falloff = smoothstep(worldRad * 1.6f, worldRad * 0.15f, dist);\n"
      "            occ += saturate(ndotv - 0.25f) * falloff;\n"
      "        }\n"
      "    }\n"
      "    float ao = saturate(1.0f - (occ / 12.0f) * 1.55f);\n"
      "    ao = lerp(0.45f, 1.0f, ao * ao);\n"
      "    return float4(ao, ao, ao, 1.0f);\n"
      "}\n"
      "float4 PSBloomExtract(PostVSOut i) : SV_TARGET {\n"
      "    float2 o = invRes.xy;\n"
      "    float3 c = 0.25f * (\n"
      "        t0.SampleLevel(sLin, i.uv + float2(-o.x, -o.y), 0).rgb +\n"
      "        t0.SampleLevel(sLin, i.uv + float2( o.x, -o.y), 0).rgb +\n"
      "        t0.SampleLevel(sLin, i.uv + float2(-o.x,  o.y), 0).rgb +\n"
      "        t0.SampleLevel(sLin, i.uv + float2( o.x,  o.y), 0).rgb);\n"
      "    c = min(c, float3(8.0f, 8.0f, 8.0f));\n"
      "    float br = max(c.r, max(c.g, c.b));\n"
      "    float thresh = blurDir.z;\n"
      "    float knee = 0.35f;\n"
      "    float soft = clamp(br - thresh + knee, 0.0f, 2.0f * knee);\n"
      "    soft = (soft * soft) / (4.0f * knee + 1e-5f);\n"
      "    float contrib = max(soft, br - thresh) / max(br, 1e-5f);\n"
      "    return float4(c * saturate(contrib), 1.0f);\n"
      "}\n"
      "float4 PSBloomBlur(PostVSOut i) : SV_TARGET {\n"
      "    static const float w[5] = { 0.2270270f, 0.1945946f, 0.1216216f, 0.0540541f, 0.0162162f };\n"
      "    float3 acc = t0.SampleLevel(sLin, i.uv, 0).rgb * w[0];\n"
      "    float2 stepUv = blurDir.xy;\n"
      "    [unroll]\n"
      "    for (int k = 1; k < 5; ++k) {\n"
      "        acc += t0.SampleLevel(sLin, i.uv + stepUv * (float)k, 0).rgb * w[k];\n"
      "        acc += t0.SampleLevel(sLin, i.uv - stepUv * (float)k, 0).rgb * w[k];\n"
      "    }\n"
      "    return float4(acc, 1.0f);\n"
      "}\n"
      "float3 SoftShoulderToneMap(float3 x) {\n"
      "    float3 s = max(x - 0.85f, 0.0f);\n"
      "    return min(x, 0.85f) + 0.15f * (s / (s + 0.55f));\n"
      "}\n"
      "float4 PSComposite(PostVSOut i) : SV_TARGET {\n"
      "    float4 scene = t0.SampleLevel(sPnt, i.uv, 0);\n"
      "    float3 col = scene.rgb;\n"
      "    if (projExtra.z > 0.5f) {\n"
      "        float cd = t3.SampleLevel(sPnt, i.uv, 0).r;\n"
      "        if (cd < 0.9998f && cd > 0.0001f) {\n"
      "            float cz = LinearViewZ(cd);\n"
      "            float aoSum = 0.0f, wSum = 0.0f;\n"
      "            [unroll]\n"
      "            for (int dy = -1; dy <= 2; ++dy) {\n"
      "                [unroll]\n"
      "                for (int dx = -1; dx <= 2; ++dx) {\n"
      "                    float2 suv = i.uv + float2(((float)dx - 0.5f) * invRes.z, ((float)dy - 0.5f) * invRes.w);\n"
      "                    float sd = t3.SampleLevel(sPnt, suv, 0).r;\n"
      "                    float sz = LinearViewZ(sd);\n"
      "                    float dz = abs(sz - cz) / max(cz * 0.08f, 1e-4f);\n"
      "                    float bw = exp(-dz * dz * 2.0f);\n"
      "                    aoSum += t1.SampleLevel(sLin, suv, 0).r * bw;\n"
      "                    wSum += bw;\n"
      "                }\n"
      "            }\n"
      "            float ao = (wSum > 1e-4f) ? (aoSum / wSum) : 1.0f;\n"
      "            col *= ao;\n"
      "        }\n"
      "    }\n"
      "    if (projExtra.w > 0.5f) {\n"
      "        float3 bloom = t2.SampleLevel(sLin, i.uv, 0).rgb;\n"
      "        col += bloom * blurDir.w;\n"
      "        col = SoftShoulderToneMap(max(col, 0.0f));\n"
      "    }\n"
      "    return float4(saturate(col), scene.a);\n"
      "}\n";

  auto compilePost = [&](const char *entry, const char *target, ID3DBlob **outBlob) -> bool {
    ID3DBlob *eb = nullptr;
    HRESULT hr = D3DCompile(kPostHlsl, std::strlen(kPostHlsl), "uwp_post.hlsl",
                            nullptr, nullptr, entry, target, 0, 0, outBlob, &eb);
    if (FAILED(hr)) {
      if (eb) {
        std::printf("[UWP-D3D11] Post shader %s err: %s\n", entry,
                    static_cast<const char *>(eb->GetBufferPointer()));
        eb->Release();
      }
      return false;
    }
    if (eb) eb->Release();
    return true;
  };
  ID3DBlob *pBlob = nullptr;
  if (compilePost("VSPost", "vs_4_0", &pBlob) && pBlob) {
    g_dev->CreateVertexShader(pBlob->GetBufferPointer(), pBlob->GetBufferSize(), nullptr, &g_vsPost);
    pBlob->Release(); pBlob = nullptr;
  }
  if (compilePost("PSSsao", "ps_4_0", &pBlob) && pBlob) {
    g_dev->CreatePixelShader(pBlob->GetBufferPointer(), pBlob->GetBufferSize(), nullptr, &g_psSsao);
    pBlob->Release(); pBlob = nullptr;
  }
  if (compilePost("PSBloomExtract", "ps_4_0", &pBlob) && pBlob) {
    g_dev->CreatePixelShader(pBlob->GetBufferPointer(), pBlob->GetBufferSize(), nullptr, &g_psBloomExtract);
    pBlob->Release(); pBlob = nullptr;
  }
  if (compilePost("PSBloomBlur", "ps_4_0", &pBlob) && pBlob) {
    g_dev->CreatePixelShader(pBlob->GetBufferPointer(), pBlob->GetBufferSize(), nullptr, &g_psBloomBlur);
    pBlob->Release(); pBlob = nullptr;
  }
  if (compilePost("PSComposite", "ps_4_0", &pBlob) && pBlob) {
    g_dev->CreatePixelShader(pBlob->GetBufferPointer(), pBlob->GetBufferSize(), nullptr, &g_psComposite);
    pBlob->Release(); pBlob = nullptr;
  }
  {
    D3D11_BUFFER_DESC pcbd{};
    pcbd.ByteWidth = sizeof(CbPost);
    pcbd.Usage = D3D11_USAGE_DYNAMIC;
    pcbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    pcbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dev->CreateBuffer(&pcbd, nullptr, &g_cbPost);

    D3D11_SAMPLER_DESC psd{};
    psd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    psd.AddressU = psd.AddressV = psd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    psd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    psd.MaxLOD = D3D11_FLOAT32_MAX;
    g_dev->CreateSamplerState(&psd, &g_sampLinearClamp);
    psd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    g_dev->CreateSamplerState(&psd, &g_sampPointClamp);

    // 3D attenuation light volumes must sample black outside [0,1]^3 (never WRAP into a grid!)
    D3D11_SAMPLER_DESC vsd{};
    vsd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    vsd.AddressU = vsd.AddressV = vsd.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    vsd.BorderColor[0] = vsd.BorderColor[1] = vsd.BorderColor[2] = vsd.BorderColor[3] = 0.0f;
    vsd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    vsd.MaxLOD = D3D11_FLOAT32_MAX;
    g_dev->CreateSamplerState(&vsd, &g_sampVolBorder);
  }
  return g_vs && g_ps && g_layout && g_cb;
}

static void ReleaseRenderTargets() {
  if (g_ctx) {
    ID3D11ShaderResourceView *nullSrvs[4] = {};
    g_ctx->PSSetShaderResources(0, 4, nullSrvs);
    g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
  }
  auto rel = [](auto *&p) { if (p) { p->Release(); p = nullptr; } };
  rel(g_rtv);
  rel(g_dsv);
  rel(g_depthSrv);
  rel(g_sceneSrv); rel(g_sceneRtv); rel(g_sceneTex);
  rel(g_ssaoSrv);  rel(g_ssaoRtv);  rel(g_ssaoTex);
  rel(g_bloomASrv); rel(g_bloomARtv); rel(g_bloomATex);
  rel(g_bloomBSrv); rel(g_bloomBRtv); rel(g_bloomBTex);
  g_boundToSceneRt = false;
}

static bool CreateRenderTargets(uint32_t w, uint32_t h) {
  if (!g_dev || !g_swapChain)
    return false;
  ID3D11Texture2D *bb = nullptr;
  if (FAILED(g_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                    reinterpret_cast<void **>(&bb))) ||
      !bb) {
    return false;
  }
  HRESULT hr = g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
  bb->Release();
  if (FAILED(hr))
    return false;

  // Typeless 32-bit float depth buffer so we can bind both DSV (write) and SRV (high-precision SSAO depth read!)
  D3D11_TEXTURE2D_DESC dsDesc{};
  dsDesc.Width = w;
  dsDesc.Height = h;
  dsDesc.MipLevels = 1;
  dsDesc.ArraySize = 1;
  dsDesc.Format = DXGI_FORMAT_R32_TYPELESS;
  dsDesc.SampleDesc.Count = 1;
  dsDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
  ID3D11Texture2D *dsTex = nullptr;
  if (SUCCEEDED(g_dev->CreateTexture2D(&dsDesc, nullptr, &dsTex)) && dsTex) {
    D3D11_DEPTH_STENCIL_VIEW_DESC dsvd{};
    dsvd.Format = DXGI_FORMAT_D32_FLOAT;
    dsvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    g_dev->CreateDepthStencilView(dsTex, &dsvd, &g_dsv);

    D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format = DXGI_FORMAT_R32_FLOAT;
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels = 1;
    g_dev->CreateShaderResourceView(dsTex, &srvd, &g_depthSrv);
    dsTex->Release();
  }

  auto makeRt = [&](uint32_t tw, uint32_t th, DXGI_FORMAT fmt,
                    ID3D11Texture2D *&tex, ID3D11RenderTargetView *&rtv,
                    ID3D11ShaderResourceView *&srv) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = std::max<uint32_t>(1u, tw);
    td.Height = std::max<uint32_t>(1u, th);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &tex)) && tex) {
      g_dev->CreateRenderTargetView(tex, nullptr, &rtv);
      g_dev->CreateShaderResourceView(tex, nullptr, &srv);
    }
  };
  g_ssaoW  = std::max<uint32_t>(1u, w / 2u);
  g_ssaoH  = std::max<uint32_t>(1u, h / 2u);
  g_bloomW = std::max<uint32_t>(1u, w / 4u);
  g_bloomH = std::max<uint32_t>(1u, h / 4u);
  makeRt(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, g_sceneTex, g_sceneRtv, g_sceneSrv);
  makeRt(g_ssaoW, g_ssaoH, DXGI_FORMAT_R8_UNORM, g_ssaoTex, g_ssaoRtv, g_ssaoSrv);
  makeRt(g_bloomW, g_bloomH, DXGI_FORMAT_R16G16B16A16_FLOAT, g_bloomATex, g_bloomARtv, g_bloomASrv);
  makeRt(g_bloomW, g_bloomH, DXGI_FORMAT_R16G16B16A16_FLOAT, g_bloomBTex, g_bloomBRtv, g_bloomBSrv);

  D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h),
                    0.0f, 1.0f};
  g_ctx->RSSetViewports(1, &vp);
  g_ctx->OMSetRenderTargets(1, &g_rtv, g_dsv);
  g_boundToSceneRt = false;
  g_width = w;
  g_height = h;
  return true;
}

bool Init(void *nativeWindow, uint32_t width, uint32_t height,
          bool isCoreWindow) {
  if (!nativeWindow || width == 0 || height == 0)
    return false;

  D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL chosen{};
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  HRESULT hr =
      D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                        levels, 3, D3D11_SDK_VERSION, &g_dev, &chosen, &g_ctx);
  if (FAILED(hr) || !g_dev || !g_ctx) {
    std::printf("[UWP-D3D11] D3D11CreateDevice failed (hr=0x%08lX)\n",
                static_cast<unsigned long>(hr));
    return false;
  }

  IDXGIDevice1 *dxgiDev = nullptr;
  IDXGIAdapter *adapter = nullptr;
  IDXGIFactory2 *factory = nullptr;
  if (SUCCEEDED(g_dev->QueryInterface(__uuidof(IDXGIDevice1),
                                      reinterpret_cast<void **>(&dxgiDev))) &&
      dxgiDev) {
    if (SUCCEEDED(dxgiDev->GetAdapter(&adapter)) && adapter) {
      adapter->GetParent(__uuidof(IDXGIFactory2),
                         reinterpret_cast<void **>(&factory));
      adapter->Release();
    }
    dxgiDev->Release();
  }
  if (!factory) {
    Shutdown();
    return false;
  }

  static bool g_allowTearingFlag = false;
  {
    BOOL tearing = FALSE;
    IDXGIFactory5 *factory5 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(
            __uuidof(IDXGIFactory5), reinterpret_cast<void **>(&factory5))) &&
        factory5) {
      if (SUCCEEDED(factory5->CheckFeatureSupport(
              DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing)))) {
        g_allowTearingFlag = (tearing == TRUE);
      }
      factory5->Release();
    }
  }
  g_allowTearing = g_allowTearingFlag;

  DXGI_SWAP_CHAIN_DESC1 scDesc{};
  scDesc.Width = width;
  scDesc.Height = height;
  scDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  scDesc.SampleDesc.Count = 1;
  scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  scDesc.BufferCount = 2;
  scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  scDesc.Flags = g_allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;

  if (isCoreWindow) {
    hr = factory->CreateSwapChainForCoreWindow(
        g_dev, reinterpret_cast<IUnknown *>(nativeWindow), &scDesc, nullptr,
        &g_swapChain);
  } else {
    hr = factory->CreateSwapChainForHwnd(
        g_dev, reinterpret_cast<HWND>(nativeWindow), &scDesc, nullptr, nullptr,
        &g_swapChain);
  }
  factory->Release();

  if (FAILED(hr) || !g_swapChain || !CreateRenderTargets(width, height) ||
      !InitShadersAndStates()) {
    Shutdown();
    return false;
  }
  float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  g_ctx->ClearRenderTargetView(g_rtv, black);
  g_swapChain->Present(0, 0);

  std::printf("[UWP-D3D11] Initialized Direct3D 11 (%ux%u, FL 0x%X, "
              "vs_4_0/ps_4_0 pipeline ready, %s)\n",
              width, height, static_cast<unsigned>(chosen),
              isCoreWindow ? "UWP CoreWindow" : "Win64 HWND");
  return true;
}

static std::atomic<uint32_t> g_pendingResizeW{0};
static std::atomic<uint32_t> g_pendingResizeH{0};
static std::atomic<bool> g_widescreenHorPlus{true};
static std::atomic<bool> g_enhancedLighting{true};
static std::atomic<float> g_charAmbientVal{0.48f};
static std::atomic<bool> g_vsyncEnabled{true};
static std::atomic<bool> g_ssaoEnabled{true};
static std::atomic<bool> g_hdrBloomEnabled{true};
static std::atomic<bool> g_charRimSpecEnabled{true};
static float g_lastFps = 60.0f;

void Resize(uint32_t width, uint32_t height) {
  if (!g_swapChain || width == 0 || height == 0)
    return;
  g_pendingResizeW.store(width, std::memory_order_relaxed);
  g_pendingResizeH.store(height, std::memory_order_relaxed);
}

static void ApplyPendingResizeIfNeeded() {
  uint32_t pw = g_pendingResizeW.exchange(0, std::memory_order_acq_rel);
  uint32_t ph = g_pendingResizeH.exchange(0, std::memory_order_acq_rel);
  if (pw > 0 && ph > 0 && (pw != g_width || ph != g_height) && g_swapChain) {
    g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ReleaseRenderTargets();
    UINT scFlags = g_allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;
    if (SUCCEEDED(g_swapChain->ResizeBuffers(2, pw, ph,
                                             DXGI_FORMAT_B8G8R8A8_UNORM, scFlags))) {
      CreateRenderTargets(pw, ph);
      ResetPipelineCache();
      std::printf("[UWP-D3D11] Resized backbuffer -> %ux%u\n", g_width, g_height);
    }
  }
}

uint32_t GetWidth() { return g_width; }
uint32_t GetHeight() { return g_height; }
void SetWidescreenHorPlus(bool enabled) { g_widescreenHorPlus.store(enabled, std::memory_order_relaxed); }
bool GetWidescreenHorPlus() { return g_widescreenHorPlus.load(std::memory_order_relaxed); }
void SetEnhancedLighting(bool enabled) { g_enhancedLighting.store(enabled, std::memory_order_relaxed); }
void SetCharAmbient(float val) { g_charAmbientVal.store(val, std::memory_order_relaxed); }
void SetVsyncEnabled(bool enabled) { g_vsyncEnabled.store(enabled, std::memory_order_relaxed); }
void SetSsaoEnabled(bool enabled) { g_ssaoEnabled.store(enabled, std::memory_order_relaxed); }
void SetHdrBloomEnabled(bool enabled) { g_hdrBloomEnabled.store(enabled, std::memory_order_relaxed); }
void SetCharRimSpecEnabled(bool enabled) { g_charRimSpecEnabled.store(enabled, std::memory_order_relaxed); }

static inline bool UsePostPipeline() {
  return (g_ssaoEnabled.load(std::memory_order_relaxed) ||
          g_hdrBloomEnabled.load(std::memory_order_relaxed)) &&
         g_sceneRtv && g_sceneSrv && g_vsPost && g_psComposite && g_cbPost;
}

static void ResolvePostProcess() {
  if (!g_ctx || !g_rtv || !g_sceneSrv || !g_vsPost || !g_psComposite || !g_cbPost) {
    g_postResolved = true;
    return;
  }
  bool doSsao  = g_ssaoEnabled.load(std::memory_order_relaxed) && g_ssaoRtv && g_ssaoSrv && g_depthSrv && g_psSsao;
  bool doBloom = g_hdrBloomEnabled.load(std::memory_order_relaxed) && g_bloomARtv && g_bloomASrv && g_bloomBRtv && g_bloomBSrv && g_psBloomExtract && g_psBloomBlur;

  ID3D11ShaderResourceView *nullSrvs[4] = {};
  g_ctx->PSSetShaderResources(0, 4, nullSrvs);
  g_ctx->OMSetRenderTargets(0, nullptr, nullptr);

  g_ctx->IASetInputLayout(nullptr);
  g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  g_ctx->VSSetShader(g_vsPost, nullptr, 0);
  g_ctx->PSSetConstantBuffers(0, 1, &g_cbPost);
  ID3D11SamplerState *pSamps[2] = { g_sampLinearClamp, g_sampPointClamp };
  g_ctx->PSSetSamplers(0, 2, pSamps);
  g_ctx->RSSetState(g_rsCullNone);
  g_ctx->OMSetDepthStencilState(g_dsOff, 0);
  float bf[4] = {};
  g_ctx->OMSetBlendState(g_blendOpaque, bf, 0xFFFFFFFFu);

  auto writePostCb = [&](float dx, float dy, float thresh, float bloomInt) {
    D3D11_MAPPED_SUBRESOURCE ms{};
    if (SUCCEEDED(g_ctx->Map(g_cbPost, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
      auto *cb = static_cast<CbPost *>(ms.pData);
      cb->invRes[0] = 1.0f / static_cast<float>(std::max<uint32_t>(1u, g_width));
      cb->invRes[1] = 1.0f / static_cast<float>(std::max<uint32_t>(1u, g_height));
      cb->invRes[2] = 1.0f / static_cast<float>(std::max<uint32_t>(1u, g_ssaoW));
      cb->invRes[3] = 1.0f / static_cast<float>(std::max<uint32_t>(1u, g_ssaoH));
      cb->projParams[0] = g_last3DProj[0];
      cb->projParams[1] = g_last3DProj[5];
      cb->projParams[2] = g_last3DProj[10];
      cb->projParams[3] = g_last3DProj[14];
      cb->projExtra[0]  = g_last3DProj[11];
      cb->projExtra[1]  = g_last3DProj[15];
      cb->projExtra[2]  = doSsao ? 1.0f : 0.0f;
      cb->projExtra[3]  = doBloom ? 1.0f : 0.0f;
      cb->blurDir[0]    = dx;
      cb->blurDir[1]    = dy;
      cb->blurDir[2]    = thresh;
      cb->blurDir[3]    = bloomInt;
      g_ctx->Unmap(g_cbPost, 0);
    }
  };

  // 1. Half-res SSAO pass from readable hardware depth buffer
  if (doSsao) {
    D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(g_ssaoW), static_cast<float>(g_ssaoH), 0.0f, 1.0f};
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->OMSetRenderTargets(1, &g_ssaoRtv, nullptr);
    writePostCb(0.0f, 0.0f, 0.0f, 0.0f);
    g_ctx->PSSetShader(g_psSsao, nullptr, 0);
    g_ctx->PSSetShaderResources(0, 1, &g_depthSrv);
    g_ctx->Draw(3, 0);
    g_ctx->PSSetShaderResources(0, 4, nullSrvs);
  }

  // 2. Quarter-res HDR Bloom extract + 2-pass separable Gaussian blur
  if (doBloom) {
    D3D11_VIEWPORT bvp{0.0f, 0.0f, static_cast<float>(g_bloomW), static_cast<float>(g_bloomH), 0.0f, 1.0f};
    g_ctx->RSSetViewports(1, &bvp);
    float invBw = 1.0f / static_cast<float>(std::max<uint32_t>(1u, g_bloomW));
    float invBh = 1.0f / static_cast<float>(std::max<uint32_t>(1u, g_bloomH));

    // Bright extract -> BloomA
    g_ctx->OMSetRenderTargets(1, &g_bloomARtv, nullptr);
    writePostCb(0.0f, 0.0f, 0.78f, 0.36f);
    g_ctx->PSSetShader(g_psBloomExtract, nullptr, 0);
    g_ctx->PSSetShaderResources(0, 1, &g_sceneSrv);
    g_ctx->Draw(3, 0);
    g_ctx->PSSetShaderResources(0, 4, nullSrvs);

    // Horizontal blur (BloomA -> BloomB)
    g_ctx->OMSetRenderTargets(1, &g_bloomBRtv, nullptr);
    writePostCb(invBw * 1.35f, 0.0f, 0.78f, 0.36f);
    g_ctx->PSSetShader(g_psBloomBlur, nullptr, 0);
    g_ctx->PSSetShaderResources(0, 1, &g_bloomASrv);
    g_ctx->Draw(3, 0);
    g_ctx->PSSetShaderResources(0, 4, nullSrvs);

    // Vertical blur (BloomB -> BloomA)
    g_ctx->OMSetRenderTargets(1, &g_bloomARtv, nullptr);
    writePostCb(0.0f, invBh * 1.35f, 0.78f, 0.36f);
    g_ctx->PSSetShaderResources(0, 1, &g_bloomBSrv);
    g_ctx->Draw(3, 0);
    g_ctx->PSSetShaderResources(0, 4, nullSrvs);
  }

  // 3. Final composite (Scene * bilateral-blurred SSAO + HDR Bloom + Soft Shoulder Tone Map -> Backbuffer)
  D3D11_VIEWPORT fullVp{0.0f, 0.0f, static_cast<float>(g_width), static_cast<float>(g_height), 0.0f, 1.0f};
  g_ctx->RSSetViewports(1, &fullVp);
  g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
  writePostCb(0.0f, 0.0f, 0.78f, 0.36f);
  g_ctx->PSSetShader(g_psComposite, nullptr, 0);
  ID3D11ShaderResourceView *compSrvs[4] = {
      g_sceneSrv,
      doSsao ? g_ssaoSrv : nullptr,
      doBloom ? g_bloomASrv : nullptr,
      doSsao ? g_depthSrv : nullptr};
  g_ctx->PSSetShaderResources(0, 4, compSrvs);
  g_ctx->Draw(3, 0);
  g_ctx->PSSetShaderResources(0, 4, nullSrvs);

  g_ctx->OMSetRenderTargets(1, &g_rtv, g_dsv);
  g_boundToSceneRt = false;
  g_postResolved = true;
  ResetPipelineCache();
}

void Clear(uint32_t xboxFlags, uint32_t argbColor, float depth) {
  if (!g_ctx || !g_rtv)
    return;
  bool clearColor = (xboxFlags & 0x000000F0u) != 0u; // Xbox D3DCLEAR_TARGET (RGBA mask 0xF0)
  if (clearColor) {
    bool usePost = UsePostPipeline();
    g_sceneHas3D = false;
    g_postResolved = false;
    ID3D11RenderTargetView *targetRtv = usePost ? g_sceneRtv : g_rtv;
    g_ctx->OMSetRenderTargets(1, &targetRtv, g_dsv);
    g_boundToSceneRt = usePost;
    float rgba[4] = {static_cast<float>((argbColor >> 16) & 0xFFu) / 255.0f,
                     static_cast<float>((argbColor >> 8) & 0xFFu) / 255.0f,
                     static_cast<float>(argbColor & 0xFFu) / 255.0f, 1.0f};
    g_ctx->ClearRenderTargetView(g_rtv, rgba);
    if (usePost && g_sceneRtv)
      g_ctx->ClearRenderTargetView(g_sceneRtv, rgba);
  }
  if (g_dsv &&
      (xboxFlags &
       0x00000003u)) { // Xbox D3DCLEAR_ZBUFFER (1) | D3DCLEAR_STENCIL (2)
    g_ctx->ClearDepthStencilView(g_dsv, D3D11_CLEAR_DEPTH, depth, 0);
  }
}

static uint32_t g_frameNum = 0;
static uint32_t g_drawNum = 0;

static std::string g_capDir;
static std::vector<uint32_t> g_capFrames;
static uint32_t g_exitAfterFrame = 0;
static uint32_t g_traceFrame = 0;
static FILE *g_traceFp = nullptr;

void ConfigureCapture(const char *outDir, const uint32_t *frames,
                      uint32_t count, uint32_t exitAfterFrame,
                      uint32_t traceFrame) {
  g_capDir = outDir ? outDir : "";
  g_capFrames.assign(frames, frames + count);
  g_exitAfterFrame = exitAfterFrame;
  g_traceFrame = traceFrame;
}

void RequestFrameTrace() {
  if (g_capDir.empty()) g_capDir = "uwp_recomp\\build";
  g_traceFrame = g_frameNum + 2u;
  std::printf("[UWP-F7] Armed live frame trace for frame %u -> %s\\frame_trace.log\n",
              g_traceFrame, g_capDir.c_str());
  std::fflush(stdout);
}

static void WritePngChunkVec(std::vector<uint8_t>& out, const char type[4], const uint8_t* data, uint32_t len) {
    auto pushBE32 = [&](uint32_t v) {
        out.push_back(uint8_t(v >> 24)); out.push_back(uint8_t(v >> 16));
        out.push_back(uint8_t(v >> 8));  out.push_back(uint8_t(v));
    };
    pushBE32(len);
    out.insert(out.end(), type, type + 4);
    if (len && data) out.insert(out.end(), data, data + len);
    uint32_t crc = 0xFFFFFFFFu;
    auto crcUpdate = [&](const uint8_t* p, uint32_t n) {
        for (uint32_t i = 0; i < n; ++i) {
            crc ^= p[i];
            for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    };
    crcUpdate(reinterpret_cast<const uint8_t*>(type), 4);
    if (len && data) crcUpdate(data, len);
    pushBE32(~crc);
}

static void SaveBackbufferPng(const std::string& path) {
    if (!g_dev || !g_ctx || !g_swapChain) return;
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(g_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) || !bb) return;
    D3D11_TEXTURE2D_DESC desc{};
    bb->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC sd = desc;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ID3D11Texture2D* stg = nullptr;
    if (SUCCEEDED(g_dev->CreateTexture2D(&sd, nullptr, &stg)) && stg) {
        g_ctx->CopyResource(stg, bb);
        D3D11_MAPPED_SUBRESOURCE ms{};
        if (SUCCEEDED(g_ctx->Map(stg, 0, D3D11_MAP_READ, 0, &ms))) {
            uint32_t w = desc.Width, h = desc.Height;
            std::vector<uint8_t> png;
            png.reserve(static_cast<size_t>(w) * h * 3u + 4096u);
            static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
            png.insert(png.end(), sig, sig + 8);
            uint8_t ihdr[13] = {
                uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w),
                uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h),
                8, 2, 0, 0, 0
            };
            WritePngChunkVec(png, "IHDR", ihdr, 13);

            uint32_t rowLen = 1u + w * 3u;
            std::vector<uint8_t> raw(static_cast<size_t>(rowLen) * h);
            uint32_t a1 = 1u, a2 = 0u;
            for (uint32_t y = 0; y < h; ++y) {
                const uint8_t* src = static_cast<const uint8_t*>(ms.pData) + static_cast<size_t>(y) * ms.RowPitch;
                uint8_t* dst = raw.data() + static_cast<size_t>(y) * rowLen;
                dst[0] = 0;
                for (uint32_t x = 0; x < w; ++x) {
                    dst[1 + x * 3 + 0] = src[x * 4 + 2];
                    dst[1 + x * 3 + 1] = src[x * 4 + 1];
                    dst[1 + x * 3 + 2] = src[x * 4 + 0];
                }
                for (uint32_t i = 0; i < rowLen; ++i) {
                    a1 = (a1 + dst[i]) % 65521u;
                    a2 = (a2 + a1) % 65521u;
                }
            }
            std::vector<uint8_t> zbuf;
            zbuf.reserve(raw.size() + (raw.size() / 65535u + 1u) * 5u + 6u);
            zbuf.push_back(0x78); zbuf.push_back(0x01);
            size_t pos = 0;
            while (pos < raw.size()) {
                size_t chunk = std::min<size_t>(65535u, raw.size() - pos);
                bool last = (pos + chunk == raw.size());
                zbuf.push_back(last ? 0x01 : 0x00);
                uint16_t l = static_cast<uint16_t>(chunk), nl = static_cast<uint16_t>(~l);
                zbuf.push_back(uint8_t(l & 0xFFu)); zbuf.push_back(uint8_t(l >> 8));
                zbuf.push_back(uint8_t(nl & 0xFFu)); zbuf.push_back(uint8_t(nl >> 8));
                zbuf.insert(zbuf.end(), raw.begin() + pos, raw.begin() + pos + chunk);
                pos += chunk;
            }
            uint32_t adler = (a2 << 16) | a1;
            zbuf.push_back(uint8_t(adler >> 24)); zbuf.push_back(uint8_t(adler >> 16));
            zbuf.push_back(uint8_t(adler >> 8));  zbuf.push_back(uint8_t(adler));
            WritePngChunkVec(png, "IDAT", zbuf.data(), static_cast<uint32_t>(zbuf.size()));
            WritePngChunkVec(png, "IEND", nullptr, 0);

            char fullPath[512]{};
            GetFullPathNameA(path.c_str(), sizeof(fullPath), fullPath, nullptr);
            HANDLE hf = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hf != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(hf, png.data(), static_cast<DWORD>(png.size()), &written, nullptr);
                FlushFileBuffers(hf);
                CloseHandle(hf);
                std::printf("[UWP-CAP] Saved frame %u -> %s (%ux%u, %lu bytes, draws=%u)\n",
                            g_frameNum + 1u, fullPath, w, h, static_cast<unsigned long>(written), g_drawNum);
            } else {
                std::printf("[UWP-CAP] FAILED CreateFileA(%s) err=%lu\n", fullPath, static_cast<unsigned long>(GetLastError()));
            }
            g_ctx->Unmap(stg, 0);
        }
        stg->Release();
    }
    bb->Release();
}

void Present(bool vsync) {
  if (g_swapChain) {
    if (g_traceFp) {
      std::fclose(g_traceFp);
      g_traceFp = nullptr;
      std::printf("[UWP-TRACE] Closed frame %u draw trace (%u draws)\n",
                  g_frameNum, g_drawNum);
    }
    // Resolve 3D HDR + SSAO + Bloom to backbuffer if no 2D HUD draw triggered it yet
    if (UsePostPipeline() && g_sceneHas3D && !g_postResolved) {
      ResolvePostProcess();
    } else if (g_boundToSceneRt && g_ctx && g_rtv) {
      g_ctx->OMSetRenderTargets(1, &g_rtv, g_dsv);
      g_boundToSceneRt = false;
    }
    // Draw in-game UWP / Xbox Dev Mode config menu & optional FPS badge before capture & present
    config_ui::RenderOverlay(g_lastFps, g_drawNum);

    uint32_t nextFrame = g_frameNum + 1u;
    if (!g_capDir.empty() && !g_capFrames.empty()) {
      for (uint32_t cf : g_capFrames) {
        if (cf == nextFrame) {
          CreateDirectoryA(g_capDir.c_str(), nullptr);
          char fname[512]{};
          std::snprintf(fname, sizeof(fname), "%s\\frame_%04u.png",
                        g_capDir.c_str(), nextFrame);
          SaveBackbufferPng(fname);
          break;
        }
      }
    }
    static LARGE_INTEGER s_last{}, s_freq{}, s_perfStart{};
    if (s_freq.QuadPart == 0) {
      QueryPerformanceFrequency(&s_freq);
      QueryPerformanceCounter(&s_perfStart);
    }
    bool isCapture = (g_exitAfterFrame > 0);
    bool useVsync = vsync && g_vsyncEnabled.load(std::memory_order_relaxed) && !isCapture;
    {
      ScopedTickAcc _pt(g_accPresentTicks);
      UINT presentFlags = (!useVsync && g_allowTearing) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
      g_swapChain->Present(useVsync ? 1u : 0u, presentFlags);
    }
    ApplyPendingResizeIfNeeded();
    g_sceneHas3D = false;
    g_postResolved = false;
    g_boundToSceneRt = false;
    ResetPipelineCache();
    // Only sleep during headless capture (where Present(0,0) doesn't VSync);
    // during interactive play, Present(1,0) handles VSync without double-stutter!
    if (vsync && isCapture && s_freq.QuadPart > 0) {
      LARGE_INTEGER now{};
      QueryPerformanceCounter(&now);
      if (s_last.QuadPart > 0) {
        double ms = static_cast<double>(now.QuadPart - s_last.QuadPart) *
                    1000.0 / static_cast<double>(s_freq.QuadPart);
        if (ms < 14.0)
          Sleep(static_cast<DWORD>(15.0 - ms));
      }
      QueryPerformanceCounter(&s_last);
    }
    ++g_frameNum;
    if (g_frameNum <= 3u || (g_frameNum % 60u) == 0u) {
      LARGE_INTEGER now{};
      QueryPerformanceCounter(&now);
      double sec = static_cast<double>(now.QuadPart - s_perfStart.QuadPart) /
                   static_cast<double>(s_freq.QuadPart);
      if (g_frameNum <= 3u) {
        std::printf("[UWP-D3D11] Frame %u presented (DrawCalls=%u)\n", g_frameNum,
                    g_drawNum);
      } else if (sec > 0.001) {
        g_lastFps = static_cast<float>(60.0 / sec);
        double totMs = (sec * 1000.0) / 60.0;
        double drwMs = (static_cast<double>(g_accDrawTicks) * 1000.0 /
                        static_cast<double>(s_freq.QuadPart)) / 60.0;
        double texMs = (static_cast<double>(g_accTexTicks) * 1000.0 /
                        static_cast<double>(s_freq.QuadPart)) / 60.0;
        double prsMs = (static_cast<double>(g_accPresentTicks) * 1000.0 /
                        static_cast<double>(s_freq.QuadPart)) / 60.0;
        double cpuMs = totMs - drwMs - texMs - prsMs;
        std::printf("[UWP-PERF] Frame %u: %.1f FPS (tot=%.1fms | draw=%.1fms | tex=%.1fms(%u/%u) | prs=%.1fms | cpu=%.1fms | draws=%u)\n",
                    g_frameNum, 60.0 / sec, totMs, drwMs, texMs,
                    g_accTexCalls / 60u, g_accTexUploads / 60u, prsMs, cpuMs, g_drawNum);
      }
      g_accDrawTicks = 0;
      g_accTexTicks = 0;
      g_accTexCalls = 0;
      g_accTexUploads = 0;
      g_accPresentTicks = 0;
      s_perfStart = now;
    }
    g_drawNum = 0;
    g_frameVerts = 0;
    g_frameVbDiscards = 0;
    if (g_exitAfterFrame > 0 && g_frameNum >= g_exitAfterFrame) {
      std::printf("[UWP-CAP] Reached exit-after frame %u, exiting cleanly.\n",
                  g_frameNum);
      std::fflush(stdout);
      ExitProcess(0);
    }
  }
}

static D3D11_VIEWPORT g_cur3DVp{0.0f, 0.0f, 1280.0f, 720.0f, 0.0f, 1.0f};
static bool g_has3DVp = false;

void SetViewport(uint32_t x, uint32_t y, uint32_t w, uint32_t h, float minZ,
                 float maxZ) {
  if (!g_ctx || w == 0 || h == 0)
    return;
  float sx = static_cast<float>(g_width) / 640.0f;
  float sy = static_cast<float>(g_height) / 480.0f;
  // Bottom-row 3D HUD weapon icon sub-viewports (active weapon + D-Pad weapon carousel)
  // must use the exact same centered 4:3 coordinate space as the 2D Weapon Ring & frame.
  bool isWeaponIconVp = (y >= 320u && w <= 160u && h <= 160u);
  if (isWeaponIconVp && g_widescreenHorPlus.load(std::memory_order_relaxed) &&
      g_height > 0u && g_width * 3u > g_height * 4u) {
    float vpOffX = (static_cast<float>(g_width) - 640.0f * sy) * 0.5f;
    g_cur3DVp = D3D11_VIEWPORT{vpOffX + x * sy, y * sy, w * sy, h * sy, minZ, maxZ};
  } else {
    g_cur3DVp = D3D11_VIEWPORT{x * sx, y * sy, w * sx, h * sy, minZ, maxZ};
  }
  g_has3DVp = true;
  g_lastVpMode = 0;
  g_ctx->RSSetViewports(1, &g_cur3DVp);
}

void SetTransform(uint32_t xboxState, const float *mat16) {
  if (!mat16)
    return;
  if (xboxState == 0)
    std::memcpy(g_mView, mat16, 64);
  else if (xboxState == 1)
    std::memcpy(g_mProj, mat16, 64);
  else if (xboxState == 2) {
    std::memcpy(g_mTex0, mat16, 64);
    g_hasTexMat[0] = true;
  } else if (xboxState == 3) {
    std::memcpy(g_mTex1, mat16, 64);
    g_hasTexMat[1] = true;
  } else if (xboxState == 6)
    std::memcpy(g_mWorld, mat16, 64);
}

struct ParsedVsDecl {
  int8_t posStream = -1;
  uint16_t posOff = 0;
  int8_t diffStream = -1;
  uint16_t diffOff = 0;
  int8_t uv0Stream = -1;
  uint16_t uv0Off = 0;
  int8_t uv1Stream = -1;
  uint16_t uv1Off = 0;
  bool isProg = false;
};

static std::unordered_map<uint32_t, ParsedVsDecl> g_vsDecls;
static uint32_t g_customVsHandle = 0;

void RegisterVertexShaderDecl(uint32_t handle, const uint32_t *pDecl,
                              bool isProgrammable) {
  if (!handle || !pDecl)
    return;
  ParsedVsDecl d{};
  d.isProg = isProgrammable;
  int8_t curStream = 0;
  uint16_t curOff = 0;
  for (int i = 0; i < 64; ++i) {
    uint32_t tok = pDecl[i];
    if (tok == 0xFFFFFFFFu)
      break;
    uint32_t tt = (tok >> 29) & 7u;
    if (tt == 1u) {
      curStream = static_cast<int8_t>(tok & 0xFu);
      curOff = 0;
    } else if (tt == 2u) {
      if (tok & 0x10000000u) {
        curOff = static_cast<uint16_t>(curOff + ((tok >> 16) & 0xFu) * 4u);
      } else {
        uint32_t reg = tok & 0xFu;
        uint32_t dt = (tok >> 16) & 0xFFu;
        uint32_t cnt = (dt >> 4) & 0xFu;
        uint32_t et = dt & 0xFu;
        uint32_t bytes = 4u;
        if (et == 2u)
          bytes = cnt * 4u;
        else if (et == 5u)
          bytes = cnt * 2u;
        else if (et == 0u || et == 1u || et == 4u)
          bytes = cnt ? cnt : 4u;
        else if (et == 6u)
          bytes = 4u;
        if (reg == 0u) {
          d.posStream = curStream;
          d.posOff = curOff;
        } else if (reg == 3u || (reg == 11u && dt == 0x40u) ||
                   (reg != 1u && dt == 0x40u && d.diffStream < 0)) {
          d.diffStream = curStream;
          d.diffOff = curOff;
        } else if (reg == 9u || (reg == 7u && d.uv0Stream < 0 && et == 2u) ||
                   (dt == 0x22u && d.uv0Stream < 0)) {
          d.uv0Stream = curStream;
          d.uv0Off = curOff;
        } else if (reg == 10u || (reg == 8u && d.uv1Stream < 0 && et == 2u) ||
                   (dt == 0x22u && d.uv1Stream < 0)) {
          d.uv1Stream = curStream;
          d.uv1Off = curOff;
        }
        curOff = static_cast<uint16_t>(curOff + bytes);
      }
    }
  }
  g_vsDecls[handle] = d;
}

void SetVertexShader(uint32_t handle) {
  if ((handle & 1u) != 0) {
    g_customVsHandle = handle;
  } else if (handle != 0) {
    g_customVsHandle = 0;
    g_fvf = handle;
  }
}

void SetStreamSource(uint32_t streamNumber, const uint8_t *guestRam,
                     uint32_t vbVA, uint32_t stride) {
  if (streamNumber >= 4 || !guestRam)
    return;
  if (!vbVA) {
    g_streams[streamNumber] = nullptr;
    g_strides[streamNumber] = 0;
    return;
  }
  uint32_t dataVA = 0;
  std::memcpy(&dataVA, guestRam + ((vbVA + 4u) & (kXboxRamSize - 1u)), 4);
  g_streams[streamNumber] =
      dataVA ? (guestRam + (dataVA & (kXboxRamSize - 1u))) : nullptr;
  g_strides[streamNumber] = stride;
}

void SetIndices(const uint8_t *guestRam, uint32_t ibVA,
                uint32_t baseVertexIndex) {
  g_baseVert = baseVertexIndex;
  if (!guestRam || !ibVA) {
    g_indices = nullptr;
    return;
  }
  uint32_t dataVA = 0;
  std::memcpy(&dataVA, guestRam + ((ibVA + 4u) & (kXboxRamSize - 1u)), 4);
  g_indices = dataVA ? reinterpret_cast<const uint16_t *>(
                           guestRam + (dataVA & (kXboxRamSize - 1u)))
                     : nullptr;
}

void SetPalette(uint32_t stage, const uint8_t *guestRam, uint32_t xpalVA) {
  if (stage >= 4)
    return;
  uint32_t dataVA = 0;
  if (guestRam && xpalVA) {
    std::memcpy(&dataVA, guestRam + ((xpalVA + 4u) & (kXboxRamSize - 1u)), 4);
  }
  if (g_boundPalVA[stage] != dataVA) {
    g_boundPalVA[stage] = dataVA;
    if (g_boundTexVA[stage] && g_boundTexFmt[stage] == 0x0Bu && guestRam) {
      SetTexture(stage, guestRam, g_boundTexVA[stage]);
    }
  }
}

void InvalidateTexture(uint32_t dataVA) {
  if (!dataVA) return;
  for (auto it = g_texCache.begin(); it != g_texCache.end(); ) {
    if (it->second.dataVA == dataVA || it->second.palVA == dataVA) {
      for (int s = 0; s < 4; ++s) {
        if (g_boundSrv[s] == it->second.srv) {
          g_boundSrv[s] = nullptr;
          g_boundTexVA[s] = 0u;
        }
      }
      if (it->second.srv)
        it->second.srv->Release();
      if (it->second.tex)
        it->second.tex->Release();
      it = g_texCache.erase(it);
    } else {
      ++it;
    }
  }
}

static bool IsLinearXboxFmt(uint32_t fmtByte) {
  return fmtByte == 0x12u || fmtByte == 0x1Eu || fmtByte == 0x11u ||
         fmtByte == 0x1Cu || fmtByte == 0x1Du || fmtByte == 0x13u ||
         fmtByte == 0x1Fu || fmtByte == 0x20u;
}

void SetTexture(uint32_t stage, const uint8_t *guestRam, uint32_t xtexVA) {
  ScopedTickAcc _tt(g_accTexTicks);
  ++g_accTexCalls;
  if (!g_dev || stage >= 4)
    return;
  if (xtexVA != 0u && xtexVA == g_boundTexVA[stage] &&
      g_boundSrv[stage] != nullptr && g_boundTexFmt[stage] != 0x0Bu) {
    return;
  }
  g_boundTexVA[stage] = xtexVA;
  g_boundTexVol[stage] = false;
  g_boundTexFmt[stage] = 0;
  g_boundTexLin[stage] = false;
  g_boundTexW[stage] = 1u;
  g_boundTexH[stage] = 1u;
  if (!guestRam || !xtexVA) {
    g_boundSrv[stage] = nullptr;
    return;
  }

  uint32_t baseOff = xtexVA & (kXboxRamSize - 1u);
  uint32_t common = 0, dataVA = 0, format = 0, sizeField = 0;
  std::memcpy(&common, guestRam + baseOff + 0u, 4);
  std::memcpy(&dataVA, guestRam + baseOff + 4u, 4);
  std::memcpy(&format, guestRam + baseOff + 12u, 4);
  std::memcpy(&sizeField, guestRam + baseOff + 16u, 4);
  if (!dataVA) {
    g_boundSrv[stage] = nullptr;
    return;
  }
  if (((common >> 16) & 7u) == 5u) { // D3DCOMMON_TYPE_VOLUMETEXTURE -> 3D light volume!
    uint32_t vw = 1u << ((format >> 20) & 0xFu);
    uint32_t vh = 1u << ((format >> 24) & 0xFu);
    uint32_t vd = 1u << ((format >> 28) & 0xFu);
    uint32_t fmtByteV = (format >> 8) & 0xFFu;
    g_boundTexFmt[stage] = fmtByteV;
    g_boundTexVol[stage] = true;
    if (vw == 0 || vh == 0 || vd == 0 || vw > 256 || vh > 256 || vd > 256) {
      g_boundSrv[stage] = nullptr;
      return;
    }
    uint64_t vKey = MakeTexCacheKey(dataVA, 0u, format, sizeField, true);
    auto vit = g_texCache.find(vKey);
    if (vit != g_texCache.end() && vit->second.dataVA == dataVA &&
        vit->second.format == format && vit->second.isVol) {
      g_boundSrv[stage] = vit->second.srv;
      return;
    }
    uint32_t vbpp = 1u;
    bool vIsA8 = false, vIsL8 = false, vIsA8L8 = false, vIs565 = false, vIs4444 = false;
    switch (fmtByteV) {
      case 0x00: case 0x13: vbpp = 1; vIsL8 = true; break;
      case 0x01: case 0x19: case 0x1F: vbpp = 1; vIsA8 = true; break;
      case 0x1A: vbpp = 2; vIsA8L8 = true; break;
      case 0x05: case 0x11: vbpp = 2; vIs565 = true; break;
      case 0x04: case 0x1D: vbpp = 2; vIs4444 = true; break;
      case 0x06: case 0x07: case 0x12: case 0x1E: vbpp = 4; break;
      default: vbpp = 1; vIsL8 = true; break;
    }
    // 3D Morton unswizzle into BGRA8888 ID3D11Texture3D
    uint32_t maskU = 0, maskV = 0, maskW = 0, j = 1;
    uint32_t tw = vw, th = vh, td3 = vd;
    while (tw > 1 || th > 1 || td3 > 1) {
      if (tw > 1)  { maskU |= j; j <<= 1; tw  >>= 1; }
      if (th > 1)  { maskV |= j; j <<= 1; th  >>= 1; }
      if (td3 > 1) { maskW |= j; j <<= 1; td3 >>= 1; }
    }
    const uint8_t *vsrc = guestRam + (dataVA & (kXboxRamSize - 1u));
    std::vector<uint32_t> rgba(static_cast<size_t>(vw) * vh * vd);
    uint32_t sw = 0;
    for (uint32_t z = 0; z < vd; ++z) {
      uint32_t sv = 0;
      for (uint32_t y = 0; y < vh; ++y) {
        uint32_t su = 0;
        for (uint32_t x = 0; x < vw; ++x) {
          const uint8_t *p = vsrc + (su | sv | sw) * vbpp;
          uint32_t outPix = 0xFFFFFFFFu;
          if (vIsA8 || vIsL8) {
            uint8_t v = p[0];
            outPix = (uint32_t(v) << 24) | (uint32_t(v) << 16) | (uint32_t(v) << 8) | v;
          } else if (vIsA8L8) {
            uint8_t lo = p[0], hi = p[1];
            outPix = (uint32_t(hi) << 24) | (uint32_t(lo) << 16) | (uint32_t(lo) << 8) | lo;
          } else if (vIs565) {
            uint16_t c = static_cast<uint16_t>(p[0] | (p[1] << 8));
            uint32_t r = ((c >> 11) & 31u) * 255u / 31u;
            uint32_t g = ((c >>  5) & 63u) * 255u / 63u;
            uint32_t b = ( c        & 31u) * 255u / 31u;
            outPix = 0xFF000000u | (r << 16) | (g << 8) | b;
          } else if (vIs4444) {
            uint16_t c = static_cast<uint16_t>(p[0] | (p[1] << 8));
            uint32_t a = ((c >> 12) & 15u) * 17u;
            uint32_t r = ((c >>  8) & 15u) * 17u;
            uint32_t g = ((c >>  4) & 15u) * 17u;
            uint32_t b = ( c        & 15u) * 17u;
            outPix = (a << 24) | (r << 16) | (g << 8) | b;
          } else {
            std::memcpy(&outPix, p, 4);
          }
          rgba[(z * vh + y) * vw + x] = outPix;
          su = (su - maskU) & maskU;
        }
        sv = (sv - maskV) & maskV;
      }
      sw = (sw - maskW) & maskW;
    }
    D3D11_TEXTURE3D_DESC tdDesc{};
    tdDesc.Width = vw;
    tdDesc.Height = vh;
    tdDesc.Depth = vd;
    tdDesc.MipLevels = 1;
    tdDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    tdDesc.Usage = D3D11_USAGE_IMMUTABLE;
    tdDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA vsrd{rgba.data(), vw * 4u, vw * vh * 4u};
    ID3D11Texture3D *vtex = nullptr;
    ID3D11ShaderResourceView *vsrv = nullptr;
    if (SUCCEEDED(g_dev->CreateTexture3D(&tdDesc, &vsrd, &vtex)) && vtex) {
      if (SUCCEEDED(g_dev->CreateShaderResourceView(vtex, nullptr, &vsrv)) && vsrv) {
        if (vit != g_texCache.end()) {
          if (vit->second.srv) vit->second.srv->Release();
          if (vit->second.tex) vit->second.tex->Release();
        }
        g_texCache[vKey] = CachedD3D11Tex{vtex, vsrv, dataVA, format, sizeField, 0u, vw, vh, true};
        g_boundSrv[stage] = vsrv;
        return;
      }
      vtex->Release();
    }
    g_boundSrv[stage] = nullptr;
    return;
  }

  uint32_t fmtByte = (format >> 8) & 0xFFu;
  g_boundTexFmt[stage] = fmtByte;
  uint32_t curPalVA = (fmtByte == 0x0Bu) ? g_boundPalVA[stage] : 0u;
  uint64_t tKey = MakeTexCacheKey(dataVA, curPalVA, format, sizeField, false);
  auto it = g_texCache.find(tKey);
  if (it != g_texCache.end() && it->second.dataVA == dataVA &&
      it->second.format == format && it->second.size == sizeField &&
      it->second.palVA == curPalVA && !it->second.isVol) {
    g_boundSrv[stage] = it->second.srv;
    g_boundTexLin[stage] = IsLinearXboxFmt(fmtByte);
    g_boundTexW[stage] = it->second.width;
    g_boundTexH[stage] = it->second.height;
    return;
  }
  uint32_t w = 0, h = 0;
  if (sizeField != 0) {
    w = (sizeField & 0xFFFu) + 1u;
    h = ((sizeField >> 12) & 0xFFFu) + 1u;
  } else {
    w = 1u << ((format >> 20) & 0xFu);
    h = 1u << ((format >> 24) & 0xFu);
  }
  if (w == 0 || h == 0 || w > 2048 || h > 2048) {
    g_boundSrv[stage] = nullptr;
    return;
  }

  const uint8_t *src = guestRam + (dataVA & (kXboxRamSize - 1u));
  DXGI_FORMAT dxFmt = DXGI_FORMAT_B8G8R8A8_UNORM;
  std::vector<uint8_t> conv;
  std::vector<uint8_t> unswz;
  const void *initPtr = src;
  UINT pitch = w * 4u;

  static uint32_t s_texLogCount = 0;
  if (s_texLogCount < 10u) {
    std::printf("[UWP-TEX] #%u dataVA=0x%08X fmt=0x%02X %ux%u (format=0x%08X "
                "size=0x%08X)\n",
                s_texLogCount, dataVA, fmtByte, w, h, format, sizeField);
    ++s_texLogCount;
  }

  auto expand16 = [&](const uint8_t *p16, bool is4444, bool is1555) {
    conv.resize(static_cast<size_t>(w) * h * 4u);
    for (uint32_t i = 0; i < w * h; ++i) {
      uint16_t v = static_cast<uint16_t>(p16[i * 2] | (p16[i * 2 + 1] << 8));
      uint8_t a = 255, r = 0, g = 0, b = 0;
      if (is4444) {
        a = static_cast<uint8_t>(((v >> 12) & 0xFu) * 17u);
        r = static_cast<uint8_t>(((v >> 8) & 0xFu) * 17u);
        g = static_cast<uint8_t>(((v >> 4) & 0xFu) * 17u);
        b = static_cast<uint8_t>((v & 0xFu) * 17u);
      } else if (is1555) {
        a = (v & 0x8000u) ? 255u : 0u;
        r = static_cast<uint8_t>((((v >> 10) & 0x1Fu) * 255u) / 31u);
        g = static_cast<uint8_t>((((v >> 5) & 0x1Fu) * 255u) / 31u);
        b = static_cast<uint8_t>(((v & 0x1Fu) * 255u) / 31u);
      } else { // R5G6B5
        r = static_cast<uint8_t>((((v >> 11) & 0x1Fu) * 255u) / 31u);
        g = static_cast<uint8_t>((((v >> 5) & 0x3Fu) * 255u) / 63u);
        b = static_cast<uint8_t>(((v & 0x1Fu) * 255u) / 31u);
      }
      conv[i * 4 + 0] = b;
      conv[i * 4 + 1] = g;
      conv[i * 4 + 2] = r;
      conv[i * 4 + 3] = a;
    }
    initPtr = conv.data();
  };

  if (fmtByte == 0x0C) { // DXT1 -> BC1
    dxFmt = DXGI_FORMAT_BC1_UNORM;
    pitch = std::max<uint32_t>(1u, (w + 3u) / 4u) * 8u;
  } else if (fmtByte == 0x0E) { // DXT3 -> BC2
    dxFmt = DXGI_FORMAT_BC2_UNORM;
    pitch = std::max<uint32_t>(1u, (w + 3u) / 4u) * 16u;
  } else if (fmtByte == 0x0F) { // DXT5 -> BC3
    dxFmt = DXGI_FORMAT_BC3_UNORM;
    pitch = std::max<uint32_t>(1u, (w + 3u) / 4u) * 16u;
  } else if (fmtByte == 0x06 || fmtByte == 0x07 ||
             fmtByte == 0x3A) { // Swizzled A8R8G8B8 / X8R8G8B8
    conv.resize(static_cast<size_t>(w) * h * 4u);
    Unswizzle2D(src, conv.data(), w, h, 4u);
    if (fmtByte == 0x07) {
      for (uint32_t i = 0; i < w * h; ++i)
        conv[i * 4 + 3] = 255u;
    }
    initPtr = conv.data();
  } else if (fmtByte == 0x12 || fmtByte == 0x1E) { // Linear A8R8G8B8 / X8R8G8B8
    conv.assign(src, src + static_cast<size_t>(w) * h * 4u);
    if (fmtByte == 0x1E) {
      for (uint32_t i = 0; i < w * h; ++i)
        conv[i * 4 + 3] = 255u;
    }
    initPtr = conv.data();
  } else if (fmtByte == 0x04 || fmtByte == 0x02 || fmtByte == 0x03 ||
             fmtByte == 0x05) { // Swizzled 16-bit A4R4G4B4 / A1R5G5B5 / X1R5G5B5 / R5G6B5
    unswz.resize(static_cast<size_t>(w) * h * 2u);
    Unswizzle2D(src, unswz.data(), w, h, 2u);
    expand16(unswz.data(), fmtByte == 0x04, fmtByte == 0x02 || fmtByte == 0x03);
    if (fmtByte == 0x03) {
      for (uint32_t i = 0; i < w * h; ++i)
        conv[i * 4 + 3] = 255u;
    }
  } else if (fmtByte == 0x1D || fmtByte == 0x10 || fmtByte == 0x1C ||
             fmtByte == 0x11) { // Linear 16-bit
    expand16(src, fmtByte == 0x1D, fmtByte == 0x10 || fmtByte == 0x1C);
    if (fmtByte == 0x1C) {
      for (uint32_t i = 0; i < w * h; ++i)
        conv[i * 4 + 3] = 255u;
    }
  } else if (fmtByte == 0x0B) { // Swizzled P8 (256-color paletted)
    unswz.resize(static_cast<size_t>(w) * h);
    Unswizzle2D(src, unswz.data(), w, h, 1u);
    conv.resize(static_cast<size_t>(w) * h * 4u);
    uint32_t palVA = g_boundPalVA[stage];
    const uint8_t *pal =
        palVA ? (guestRam + (palVA & (kXboxRamSize - 1u))) : nullptr;
    for (uint32_t i = 0; i < w * h; ++i) {
      uint8_t idx = unswz[i];
      if (pal) {
        conv[i * 4 + 0] = pal[idx * 4 + 0];
        conv[i * 4 + 1] = pal[idx * 4 + 1];
        conv[i * 4 + 2] = pal[idx * 4 + 2];
        conv[i * 4 + 3] = pal[idx * 4 + 3];
      } else {
        conv[i * 4 + 0] = conv[i * 4 + 1] = conv[i * 4 + 2] = idx;
        conv[i * 4 + 3] = 255u;
      }
    }
    initPtr = conv.data();
  } else if (fmtByte == 0x00 || fmtByte == 0x01 || fmtByte == 0x19 ||
             fmtByte == 0x13 || fmtByte == 0x1F) { // L8 / A8 (swizzled or linear)
    bool isSwz = (fmtByte == 0x00 || fmtByte == 0x01 || fmtByte == 0x19);
    bool isAlphaOnly = (fmtByte == 0x01 || fmtByte == 0x19 || fmtByte == 0x1F);
    const uint8_t *p8 = src;
    if (isSwz) {
      unswz.resize(static_cast<size_t>(w) * h);
      Unswizzle2D(src, unswz.data(), w, h, 1u);
      p8 = unswz.data();
    }
    conv.resize(static_cast<size_t>(w) * h * 4u);
    for (uint32_t i = 0; i < w * h; ++i) {
      uint8_t v = p8[i];
      conv[i * 4 + 0] = isAlphaOnly ? 255u : v;
      conv[i * 4 + 1] = isAlphaOnly ? 255u : v;
      conv[i * 4 + 2] = isAlphaOnly ? 255u : v;
      conv[i * 4 + 3] = isAlphaOnly ? v : 255u;
    }
    initPtr = conv.data();
  } else if (fmtByte == 0x1A || fmtByte == 0x20) { // A8L8
    const uint8_t *p16 = src;
    if (fmtByte == 0x1A) {
      unswz.resize(static_cast<size_t>(w) * h * 2u);
      Unswizzle2D(src, unswz.data(), w, h, 2u);
      p16 = unswz.data();
    }
    conv.resize(static_cast<size_t>(w) * h * 4u);
    for (uint32_t i = 0; i < w * h; ++i) {
      uint8_t l = p16[i * 2 + 0], a = p16[i * 2 + 1];
      conv[i * 4 + 0] = l;
      conv[i * 4 + 1] = l;
      conv[i * 4 + 2] = l;
      conv[i * 4 + 3] = a;
    }
    initPtr = conv.data();
  } else if (fmtByte == 0x28 || fmtByte == 0x27) { // V8U8 / L6V5U5 bump map
    unswz.resize(static_cast<size_t>(w) * h * 2u);
    Unswizzle2D(src, unswz.data(), w, h, 2u);
    conv.resize(static_cast<size_t>(w) * h * 4u);
    for (uint32_t i = 0; i < w * h; ++i) {
      uint8_t u = 128u, v = 128u, l = 255u;
      if (fmtByte == 0x28) {
        u = static_cast<uint8_t>(static_cast<int8_t>(unswz[i * 2 + 0]) + 128);
        v = static_cast<uint8_t>(static_cast<int8_t>(unswz[i * 2 + 1]) + 128);
      } else {
        uint16_t p = static_cast<uint16_t>(unswz[i * 2] | (unswz[i * 2 + 1] << 8));
        int su = static_cast<int>(p & 0x1Fu); if (su >= 16) su -= 32;
        int sv = static_cast<int>((p >> 5) & 0x1Fu); if (sv >= 16) sv -= 32;
        u = static_cast<uint8_t>(std::max(0, std::min(255, su * 8 + 128)));
        v = static_cast<uint8_t>(std::max(0, std::min(255, sv * 8 + 128)));
        l = static_cast<uint8_t>(((p >> 10) & 0x3Fu) * 255u / 63u);
      }
      conv[i * 4 + 0] = l;
      conv[i * 4 + 1] = v;
      conv[i * 4 + 2] = u;
      conv[i * 4 + 3] = 255u;
    }
    initPtr = conv.data();
  } else {
    initPtr = src;
  }

  D3D11_TEXTURE2D_DESC td{};
  td.Width = w;
  td.Height = h;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = dxFmt;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_IMMUTABLE;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA srd{initPtr, pitch, 0};

  ID3D11Texture2D *tex = nullptr;
  ID3D11ShaderResourceView *srv = nullptr;
  if (SUCCEEDED(g_dev->CreateTexture2D(&td, &srd, &tex)) && tex) {
    ++g_accTexUploads;
    if (SUCCEEDED(g_dev->CreateShaderResourceView(tex, nullptr, &srv)) && srv) {
      if (it != g_texCache.end()) {
        if (it->second.srv)
          it->second.srv->Release();
        if (it->second.tex)
          it->second.tex->Release();
      }
      g_texCache[tKey] =
          CachedD3D11Tex{tex, srv, dataVA, format, sizeField, curPalVA, w, h, false};
      g_boundSrv[stage] = srv;
      g_boundTexLin[stage] = IsLinearXboxFmt(fmtByte);
      g_boundTexW[stage] = w;
      g_boundTexH[stage] = h;
      return;
    }
    tex->Release();
  }
  g_boundSrv[stage] = nullptr;
}

static void EnsureDynamicBuffers(uint32_t nVerts, uint32_t nIdx) {
  if (nVerts > g_vbCap) {
    if (g_vb) {
      g_vb->Release();
      g_vb = nullptr;
    }
    g_vbCap = std::max<uint32_t>(nVerts, 262144u); // 13.6 MB ring VB (NO_OVERWRITE!)
    g_vbCursor = 0;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = g_vbCap * sizeof(UwpVert);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dev->CreateBuffer(&bd, nullptr, &g_vb);
    g_frameBound = false;
  }
  if (nIdx > g_ibCap) {
    if (g_ib) {
      g_ib->Release();
      g_ib = nullptr;
    }
    g_ibCap = std::max<uint32_t>(nIdx, 524288u); // 1 MB ring IB (NO_OVERWRITE!)
    g_ibCursor = 0;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = g_ibCap * sizeof(uint16_t);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dev->CreateBuffer(&bd, nullptr, &g_ib);
    g_frameBound = false;
  }
}

static void UploadRingGeom(uint32_t nVerts, const uint16_t *indices,
                           uint32_t nIdx, uint32_t &outVbStart,
                           uint32_t &outIbStart) {
  EnsureDynamicBuffers(nVerts, nIdx);
  g_frameVerts += nVerts;
  D3D11_MAP vbMap = D3D11_MAP_WRITE_NO_OVERWRITE;
  if (g_vbCursor + nVerts > g_vbCap) {
    g_vbCursor = 0;
    vbMap = D3D11_MAP_WRITE_DISCARD;
    ++g_frameVbDiscards;
  }
  outVbStart = g_vbCursor;
  D3D11_MAPPED_SUBRESOURCE ms{};
  if (g_vb && SUCCEEDED(g_ctx->Map(g_vb, 0, vbMap, 0, &ms))) {
    std::memcpy(static_cast<UwpVert *>(ms.pData) + outVbStart,
                g_vertScratch.data(), nVerts * sizeof(UwpVert));
    g_ctx->Unmap(g_vb, 0);
    g_vbCursor += nVerts;
  }
  outIbStart = 0;
  if (indices && nIdx > 0 && g_ib) {
    D3D11_MAP ibMap = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (g_ibCursor + nIdx > g_ibCap) {
      g_ibCursor = 0;
      ibMap = D3D11_MAP_WRITE_DISCARD;
    }
    outIbStart = g_ibCursor;
    if (SUCCEEDED(g_ctx->Map(g_ib, 0, ibMap, 0, &ms))) {
      std::memcpy(static_cast<uint16_t *>(ms.pData) + outIbStart, indices,
                  nIdx * sizeof(uint16_t));
      g_ctx->Unmap(g_ib, 0);
      g_ibCursor += nIdx;
    }
  }
}

static void UnpackFvfVerts(const uint8_t *vdata, uint32_t count,
                           uint32_t stride, bool &outIsRhw) {
  outIsRhw = ((g_fvf & 0x0Eu) == 0x04u); // D3DFVF_XYZRHW
  bool hasNormal = (g_fvf & 0x10u) != 0;
  bool hasDiffuse = (g_fvf & 0x40u) != 0;
  bool hasSpec = (g_fvf & 0x80u) != 0;
  uint32_t nTex = (g_fvf >> 8) & 0xFu;

  uint32_t off = outIsRhw ? 16u : 12u;
  if (hasNormal)
    off += 12u;
  uint32_t diffOff = off;
  if (hasDiffuse)
    off += 4u;
  if (hasSpec)
    off += 4u;
  uint32_t uvOff = off;

  g_vertScratch.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t *p = vdata + i * stride;
    UwpVert &o = g_vertScratch[i];
    std::memcpy(&o.x, p, 12);
    if (hasDiffuse && diffOff + 4u <= stride) {
      uint32_t c = 0;
      std::memcpy(&c, p + diffOff, 4);
      const uint8_t *ram = GetGuestRamBase();
      bool alphaBlendOn = ram ? (reinterpret_cast<const uint32_t *>(
                                     ram + 0x0059F940u)[59] != 0u)
                              : true;
      if (!outIsRhw && !alphaBlendOn && c == 0u) {
        o.r = o.g = o.b = o.a = 1.0f;
      } else {
        o.a = static_cast<float>((c >> 24) & 0xFFu) / 255.0f;
        o.r = static_cast<float>((c >> 16) & 0xFFu) / 255.0f;
        o.g = static_cast<float>((c >> 8) & 0xFFu) / 255.0f;
        o.b = static_cast<float>(c & 0xFFu) / 255.0f;
        if (!outIsRhw && !alphaBlendOn && o.a == 0.0f)
          o.a = 1.0f;
      }
    } else {
      o.r = o.g = o.b = o.a = 1.0f;
    }
    const uint8_t *ram = GetGuestRamBase();
    uint32_t tci0 =
        ram ? (reinterpret_cast<const uint32_t *>(ram + 0x0059F740u)[28] & 0x7u)
            : 0u;
    if (nTex > 0 && uvOff + 8u <= stride) {
      uint32_t uv0Byte = (tci0 == 1u && nTex >= 2 && uvOff + 16u <= stride)
                             ? (uvOff + 8u)
                             : uvOff;
      std::memcpy(&o.u, p + uv0Byte, 8);
      if (nTex >= 2 && uvOff + 16u <= stride) {
        std::memcpy(&o.u1, p + uvOff + 8u, 8);
      } else {
        o.u1 = o.u;
        o.v1 = o.v;
      }
    } else {
      o.u = o.v = o.u1 = o.v1 = 0.0f;
    }
  }

  // Widescreen Hor+ 4:3 aspect-preserving & edge-anchored 2D HUD scaling (matches recomp/src/d3d9_bridge.cpp):
  //   - Full-screen fades/letterbox/backgrounds -> stretch full width
  //   - Top-left (Buffy health/slayer bar) -> anchor to left edge at true 4:3 aspect
  //   - Top-right (enemy health bar & cutscene location text) -> anchor to right edge at true 4:3 aspect
  //   - Center reticle, TIP banners, subtitles, weapon ring & menus -> centered 4:3 safe zone (no stretching!)
  if (outIsRhw && count >= 3u &&
      g_widescreenHorPlus.load(std::memory_order_relaxed) &&
      g_height > 0u && g_width * 3u > g_height * 4u) {
    float rhwScaleX = (static_cast<float>(g_height) * (4.0f / 3.0f)) /
                      static_cast<float>(g_width);
    float rhwOffX = (640.0f - 640.0f * rhwScaleX) * 0.5f;
    float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
    for (uint32_t i = 0; i < count; ++i) {
      float vx = g_vertScratch[i].x, vy = g_vertScratch[i].y;
      if (vx < minX) minX = vx;
      if (vx > maxX) maxX = vx;
      if (vy < minY) minY = vy;
      if (vy > maxY) maxY = vy;
    }
    float quadW = maxX - minX;
    bool stretchFullWidth = (minX <= 2.0f && maxX >= 638.0f);
    if (!stretchFullWidth) {
      float edgeOffX = rhwOffX;
      if (maxX < 285.0f && maxY < 130.0f && quadW >= 20.0f) {
        edgeOffX = 0.0f; // Top-left Buffy Health / Slayer Power bar
      } else if (minX > 310.0f && maxX > 460.0f && maxY < 150.0f &&
                 quadW >= 20.0f) {
        edgeOffX = rhwOffX * 2.0f; // Top-right Enemy Health bar & cutscene text
      }
      for (uint32_t i = 0; i < count; ++i) {
        g_vertScratch[i].x = g_vertScratch[i].x * rhwScaleX + edgeOffX;
      }
    }
  }
}

static void SubmitDraw(D3D11_PRIMITIVE_TOPOLOGY topo, bool isRhw, bool indexed,
                       uint32_t drawCount, INT baseVert = 0,
                       uint32_t ibStart = 0) {
  if (!g_ctx || !g_vs || !g_ps || !g_vb || !g_rtv || drawCount == 0)
    return;

  // Route 3D draws to offscreen HDR scene target and resolve SSAO + HDR Bloom cleanly before 2D HUD!
  // Note: untextured fvf=0x044 rhw quads (e.g. mid-frame shadow-volume tint quads) draw before characters,
  // so only trigger post-process resolve on textured 2D HUD/menu draws ((g_fvf & 0xF00u) != 0).
  bool usePost = UsePostPipeline();
  bool isTexturedHud = isRhw && ((g_fvf & 0xF00u) != 0u);
  if (!isTexturedHud) {
    if (usePost && !g_postResolved) {
      if (!g_boundToSceneRt) {
        g_ctx->OMSetRenderTargets(1, &g_sceneRtv, g_dsv);
        g_boundToSceneRt = true;
      }
      if (!isRhw) {
        g_sceneHas3D = true;
        if (g_mProj[0] != 0.0f && g_mProj[5] != 0.0f) {
          std::memcpy(g_last3DProj, g_mProj, 64);
        }
      }
    } else if (g_boundToSceneRt) {
      g_ctx->OMSetRenderTargets(1, &g_rtv, g_dsv);
      g_boundToSceneRt = false;
    }
  } else {
    if (usePost && g_sceneHas3D && !g_postResolved) {
      ResolvePostProcess();
    } else if (g_boundToSceneRt) {
      g_ctx->OMSetRenderTargets(1, &g_rtv, g_dsv);
      g_boundToSceneRt = false;
    }
  }

  // Bind static pipeline state ONCE per frame (reset in Present) instead of 1,700x/frame!
  if (!g_frameBound) {
    UINT stride = sizeof(UwpVert), offset = 0;
    ID3D11RenderTargetView *activeRtv = g_boundToSceneRt ? g_sceneRtv : g_rtv;
    g_ctx->OMSetRenderTargets(1, &activeRtv, g_dsv);
    g_ctx->IASetInputLayout(g_layout);
    g_ctx->IASetVertexBuffers(0, 1, &g_vb, &stride, &offset);
    if (g_ib)
      g_ctx->IASetIndexBuffer(g_ib, DXGI_FORMAT_R16_UINT, 0);
    g_ctx->VSSetShader(g_vs, nullptr, 0);
    g_ctx->VSSetConstantBuffers(0, 1, &g_cb);
    g_ctx->PSSetShader(g_ps, nullptr, 0);
    g_ctx->PSSetConstantBuffers(0, 1, &g_cb);
    g_frameBound = true;
  }
  int wantVpMode = isRhw ? 1 : (g_has3DVp ? 0 : 1);
  if (wantVpMode != g_lastVpMode) {
    if (wantVpMode == 1) {
      D3D11_VIEWPORT fullVp{
          0.0f, 0.0f, static_cast<float>(g_width), static_cast<float>(g_height),
          0.0f, 1.0f};
      g_ctx->RSSetViewports(1, &fullVp);
    } else {
      g_ctx->RSSetViewports(1, &g_cur3DVp);
    }
    g_lastVpMode = wantVpMode;
  }

  // Read live Xbox D3D__RenderState (0x0059F940) & D3D__TextureState
  // (0x0059F740) from flat 64 MB guest RAM!
  const uint8_t *ram = GetGuestRamBase();
  uint32_t alphaBlendEn = 1u, alphaTestEn = 0u, alphaRef = 0u,
           srcBlend = 0x0302u, destBlend = 0x0303u, zWriteEn = 1u, zEnable = 1u,
           tf = 0xFFFFFFFFu;
  uint32_t cullMode = 0u, fogEnable = 0u, fogColorRaw = 0u;
  uint32_t addrU0 = 1u, addrV0 = 1u, addrW0 = 3u, addrU1 = 1u, addrV1 = 1u, addrW1 = 3u;
  uint32_t tci0 = 0u, tci1 = 0u;
  float fogStart = 0.0f, fogEnd = 1.0f;
  bool useTfRgb = false, useTfAlpha = false, texStage0Enabled = true,
       texStage1Enabled = false;
  float stage0Mul = 1.0f, stage1Op = 4.0f, charAmbient = 0.0f;
  float rimStrength = 0.0f, specStrength = 0.0f;
  bool isWaterSurface = false;
  float alphaMode = 0.0f;
  uint32_t ttf0 = 0u, ttf1 = 0u;
  if (ram) {
    const uint32_t *rs = reinterpret_cast<const uint32_t *>(ram + 0x0059F940u);
    const uint32_t *ts = reinterpret_cast<const uint32_t *>(ram + 0x0059F740u);
    alphaBlendEn = rs[59];
    alphaTestEn = (rs[60] != 0u && rs[58] != 0x207u)
                      ? 1u
                      : 0u; // Ignore D3DCMP_ALWAYS (0x207)
    alphaRef = rs[61] & 0xFFu;
    srcBlend = rs[62];
    destBlend = rs[63];
    zWriteEn = rs[64];
    zEnable = rs[125];
    cullMode = rs[129];
    tf = rs[130];
    fogEnable = rs[83];
    std::memcpy(&fogStart, &rs[85], 4);
    std::memcpy(&fogEnd, &rs[86], 4);
    fogColorRaw = rs[120];
    addrU0 = ts[0];
    addrV0 = ts[1];
    addrW0 = ts[2];
    addrU1 = ts[32 + 0];
    addrV1 = ts[32 + 1];
    addrW1 = ts[32 + 2];
    tci0 = ts[28];
    tci1 = ts[32 + 28];
    ttf0 = ts[21];
    ttf1 = ts[32 + 21];
    uint32_t cOp = ts[12];
    uint32_t aOp = ts[16];
    uint32_t cArg1 = ts[14] & 0xFu,
             cArg2 = ts[15] &
                     0xFu; // XDK 4432 exact indices: 14=COLORARG1, 15=COLORARG2
    uint32_t aArg1 = ts[18] & 0xFu,
             aArg2 = ts[19] &
                     0xFu; // XDK 4432 exact indices: 18=ALPHAARG1, 19=ALPHAARG2
    if (aOp == 1u && !alphaTestEn) alphaMode = 3.0f;      // D3DTOP_DISABLE -> opaque 1.0 (never override alpha-test!)
    else if ((aOp == 2u && aArg1 == 2u) || (aOp == 3u && aArg2 == 2u)) alphaMode = 1.0f;  // TEXTURE alpha
    else if ((aOp == 2u && aArg1 == 0u) || (aOp == 3u && aArg2 == 0u)) alphaMode = 2.0f;  // DIFFUSE alpha
    else if ((aOp == 2u && aArg1 == 3u) || (aOp == 3u && aArg2 == 3u)) alphaMode = 3.0f;  // TFACTOR alpha (multiplied below)
    isWaterSurface = (!isRhw && g_customVsHandle == 0u && g_fvf == 0x242u &&
                      cOp == 4u && ts[32 + 12] == 4u && zWriteEn == 0u &&
                      g_boundSrv[0] != nullptr);
    if (isWaterSurface) {
      addrU0 = addrV0 = addrU1 = addrV1 = 1u; // WRAP for seamless wind ripples
      alphaBlendEn = 1u;
      srcBlend = 0x0302u;  // SRC_ALPHA
      destBlend = 0x0303u; // INV_SRC_ALPHA
    }
    bool isRealBumpPass =
        (!isRhw && (cOp == 25u || cOp == 26u) &&
         (g_boundTexFmt[0] == 0x28u || g_boundTexFmt[0] == 0x27u));
    bool isCharNonBumpOp25 =
        (!isRhw && cOp == 25u && g_boundSrv[0] != nullptr &&
         g_boundTexFmt[0] != 0x28u && g_boundTexFmt[0] != 0x27u);
    bool isOpaqueLitProp =
        (!isRhw && g_customVsHandle == 0u && g_fvf == 0x242u &&
         cOp == 4u && zWriteEn != 0u && g_boundSrv[0] != nullptr);
    if (isCharNonBumpOp25 && zWriteEn != 0u) {
      alphaBlendEn = 0u; // Solid opaque characters/vampires while preserving alpha-test discard on foliage!
      alphaMode = 1.0f;  // Use TEXTURE alpha only for alpha-test so unlit vertices (DIFFUSE.a=0) never punch holes!
    } else if (isOpaqueLitProp && alphaRef == 0u &&
               g_boundTexFmt[0] != 0x0Eu && g_boundTexFmt[0] != 0x0Fu) {
      // Dynamic doors/crates/ladders (fvf=0x242, zWrite=1, opaque DXT1) store 0 in vertex DIFFUSE.a;
      // disable SRC_ALPHA blend so they never turn invisible when unlit or at distance!
      alphaBlendEn = 0u;
    }
    texStage0Enabled = (cOp != 1u) && !isRealBumpPass; // D3DTOP_DISABLE == 1; don't multiply color by V8U8 bump map!
    useTfRgb = (!isCharNonBumpOp25) && (cArg1 == 3u || cArg2 == 3u);
    useTfAlpha = (!isCharNonBumpOp25) && (aArg1 == 3u || aArg2 == 3u);
    if ((cOp == 2u && cArg1 == 2u) || (cOp == 3u && cArg2 == 2u))
      stage0Mul = -1.0f; // D3DTOP_SELECTARG1/2 (TEXTURE only)
    else if (cOp == 5u)
      stage0Mul = 2.0f; // D3DTOP_MODULATE2X
    else if (cOp == 6u)
      stage0Mul = 4.0f; // D3DTOP_MODULATE4X
    bool isVolPass = (!isRhw && (g_boundTexVol[0] || g_boundTexVol[1]));
    if (!isVolPass && (isCharNonBumpOp25 || isOpaqueLitProp || (!isRhw && g_customVsHandle != 0u))) {
      charAmbient = g_charAmbientVal.load(std::memory_order_relaxed);
    }
    if (!isRhw && !isVolPass && !isWaterSurface && zWriteEn != 0u &&
        g_charRimSpecEnabled.load(std::memory_order_relaxed)) {
      if (isCharNonBumpOp25 || g_customVsHandle != 0u) {
        rimStrength = 0.30f;
        specStrength = 0.22f;
      } else if (isRealBumpPass) {
        rimStrength = 0.14f;
        specStrength = 0.28f;
      } else if (isOpaqueLitProp) {
        rimStrength = 0.08f;
        specStrength = 0.14f;
      }
    }
    uint32_t cOp1 = ts[32 + 12];
    if (!isRhw && !isCharNonBumpOp25 && g_boundSrv[1] != nullptr &&
        ((cOp1 >= 4u && cOp1 <= 7u) || (isRealBumpPass && (cOp1 == 2u || cOp1 == 3u)))) {
      texStage1Enabled = true;
      stage1Op = (cOp1 >= 4u && cOp1 <= 7u) ? static_cast<float>(cOp1) : 4.0f;
    }
  }

  bool isVolPassFinal = (!isRhw && (g_boundTexVol[0] || g_boundTexVol[1]));
  bool isAdditive = (alphaBlendEn != 0u && destBlend == 1u);
  bool applyFog =
      (!isRhw && !isVolPassFinal && fogEnable != 0u && !isAdditive && fogEnd > fogStart + 0.01f);

  D3D11_MAPPED_SUBRESOURCE ms{};
  if (SUCCEEDED(g_ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
    auto *cb = static_cast<CbPerDraw *>(ms.pData);
    std::memcpy(cb->mWorld, g_mWorld, 64);
    std::memcpy(cb->mView, g_mView, 64);
    std::memcpy(cb->mProj, g_mProj, 64);
    cb->screenParams[0] = 640.0f;
    cb->screenParams[1] = 480.0f;
    cb->screenParams[2] = isRhw ? 1.0f : 0.0f;
    cb->screenParams[3] = (g_boundSrv[0] && texStage0Enabled) ? 1.0f : 0.0f;
    cb->tfactor[0] = static_cast<float>((tf >> 16) & 0xFFu) / 255.0f;
    cb->tfactor[1] = static_cast<float>((tf >> 8) & 0xFFu) / 255.0f;
    cb->tfactor[2] = static_cast<float>(tf & 0xFFu) / 255.0f;
    cb->tfactor[3] = static_cast<float>((tf >> 24) & 0xFFu) / 255.0f;
    cb->tssMode[0] = useTfRgb ? 1.0f : 0.0f;
    cb->tssMode[1] = useTfAlpha ? 1.0f : 0.0f;
    cb->tssMode[2] =
        alphaTestEn ? (static_cast<float>(alphaRef) / 255.0f) : 0.0f;
    cb->tssMode[3] = stage0Mul;
    cb->stage1Mode[0] = texStage1Enabled ? 1.0f : 0.0f;
    cb->stage1Mode[1] = stage1Op;
    cb->stage1Mode[2] = charAmbient;
    cb->stage1Mode[3] = static_cast<float>((g_boundTexVol[0] ? 1 : 0) | (g_boundTexVol[1] ? 2 : 0));
    cb->uvScale[0] = g_boundTexLin[0]
                         ? (1.0f / static_cast<float>(
                                       std::max<uint32_t>(1u, g_boundTexW[0])))
                         : 1.0f;
    cb->uvScale[1] = g_boundTexLin[0]
                         ? (1.0f / static_cast<float>(
                                       std::max<uint32_t>(1u, g_boundTexH[0])))
                         : 1.0f;
    cb->uvScale[2] = g_boundTexLin[1]
                         ? (1.0f / static_cast<float>(
                                       std::max<uint32_t>(1u, g_boundTexW[1])))
                         : 1.0f;
    cb->uvScale[3] = g_boundTexLin[1]
                         ? (1.0f / static_cast<float>(
                                       std::max<uint32_t>(1u, g_boundTexH[1])))
                         : 1.0f;
    cb->fogColor[0] = static_cast<float>((fogColorRaw >> 16) & 0xFFu) / 255.0f;
    cb->fogColor[1] = static_cast<float>((fogColorRaw >> 8) & 0xFFu) / 255.0f;
    cb->fogColor[2] = static_cast<float>(fogColorRaw & 0xFFu) / 255.0f;
    cb->fogColor[3] = applyFog ? 1.0f : 0.0f;
    cb->fogParams[0] = fogStart;
    cb->fogParams[1] = fogEnd;
    cb->fogParams[2] = isWaterSurface
                           ? (g_enhancedLighting.load(std::memory_order_relaxed) ? 2.0f : 1.0f)
                           : 0.0f;
    cb->fogParams[3] = isWaterSurface
                           ? (static_cast<float>(GetTickCount64() & 0xFFFFFu) * 0.001f)
                           : 0.0f;
    static const float kIdent16[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    auto fillTexMat = [&](float dst[16], const float src[16], bool hasMat, uint32_t ttf, bool isLin) {
      if (!isRhw && !isLin && hasMat && ttf != 0u && (ttf & ~256u) <= 4u) {
        std::memcpy(dst, src, 64);
        // D3D8/9 2D texture transform (COUNT2) stores UV translation in row 3 (_31, _32);
        // promote to row 4 (_41, _42) so mul(float4(u, v, w, 1), mTex) works for both 2D & 3D!
        if ((ttf & 7u) == 2u && dst[12] == 0.0f && dst[13] == 0.0f) {
          dst[12] = dst[8]; dst[13] = dst[9];
          dst[8] = 0.0f; dst[9] = 0.0f;
        }
      } else {
        std::memcpy(dst, kIdent16, 64);
      }
    };
    fillTexMat(cb->mTex0, g_mTex0, g_hasTexMat[0], ttf0, g_boundTexLin[0]);
    fillTexMat(cb->mTex1, g_mTex1, g_hasTexMat[1], ttf1, g_boundTexLin[1]);
    cb->alphaParams[0] = alphaMode;
    cb->alphaParams[1] = rimStrength;
    cb->alphaParams[2] = specStrength;
    uint32_t tciFlags = 0u;
    if (!isRhw && (tci0 & 0xFFFF0000u) == 0x00020000u) tciFlags |= 1u;
    if (!isRhw && (tci1 & 0xFFFF0000u) == 0x00020000u) tciFlags |= 2u;
    cb->alphaParams[3] = static_cast<float>(tciFlags);
    g_ctx->Unmap(g_cb, 0);
  }

  if (topo != g_lastTopo) {
    g_ctx->IASetPrimitiveTopology(topo);
    g_lastTopo = topo;
  }
  ID3D11SamplerState *samps[2] = {
      GetXboxSampler(addrU0, addrV0, addrW0, g_boundTexVol[0]),
      GetXboxSampler(addrU1, addrV1, addrW1, g_boundTexVol[1])};
  if (samps[0] != g_lastSamps[0] || samps[1] != g_lastSamps[1]) {
    g_ctx->PSSetSamplers(0, 2, samps);
    g_lastSamps[0] = samps[0];
    g_lastSamps[1] = samps[1];
  }
  ID3D11ShaderResourceView *srvs[4] = {
      g_boundTexVol[0] ? nullptr : g_boundSrv[0],
      (texStage1Enabled && !g_boundTexVol[1]) ? g_boundSrv[1] : nullptr,
      g_boundTexVol[0] ? g_boundSrv[0] : nullptr,
      (texStage1Enabled && g_boundTexVol[1]) ? g_boundSrv[1] : nullptr};
  if (srvs[0] != g_lastSrvs[0] || srvs[1] != g_lastSrvs[1] ||
      srvs[2] != g_lastSrvs[2] || srvs[3] != g_lastSrvs[3]) {
    g_ctx->PSSetShaderResources(0, 4, srvs);
    g_lastSrvs[0] = srvs[0];
    g_lastSrvs[1] = srvs[1];
    g_lastSrvs[2] = srvs[2];
    g_lastSrvs[3] = srvs[3];
  }
  ID3D11RasterizerState *rsState = g_rsCullNone;
  if (!isRhw) {
    if (cullMode == 0x900u && g_rsCullCW)
      rsState = g_rsCullCW;
    else if (cullMode == 0x901u && g_rsCullCCW)
      rsState = g_rsCullCCW;
  }
  if (rsState != g_lastRs) {
    g_ctx->RSSetState(rsState);
    g_lastRs = rsState;
  }
  ID3D11DepthStencilState *ds =
      (isRhw || !zEnable) ? g_dsOff : (zWriteEn ? g_dsOn : g_dsNoWrite);
  if (ds != g_lastDs) {
    g_ctx->OMSetDepthStencilState(ds, 0);
    g_lastDs = ds;
  }
  ID3D11BlendState *bs = g_blendOpaque;
  if (alphaBlendEn) {
    uint32_t bKey = (srcBlend & 0xFFFFu) | ((destBlend & 0xFFFFu) << 16);
    if (bKey == g_lastBlendKey && g_lastBlendVal) {
      bs = g_lastBlendVal;
    } else {
      bs = GetXboxBlendState(srcBlend, destBlend);
      g_lastBlendKey = bKey;
      g_lastBlendVal = bs;
    }
  }
  if (bs != g_lastBs) {
    float bf[4] = {0, 0, 0, 0};
    g_ctx->OMSetBlendState(bs, bf, 0xFFFFFFFFu);
    g_lastBs = bs;
  }

  if (g_traceFrame > 0 && (g_frameNum + 1u) == g_traceFrame) {
    if (!g_traceFp && !g_capDir.empty()) {
      CreateDirectoryA(g_capDir.c_str(), nullptr);
      char tf[512]{};
      std::snprintf(tf, sizeof(tf), "%s\\frame_trace.log", g_capDir.c_str());
      g_traceFp = std::fopen(tf, "w");
    }
    if (g_traceFp) {
      const uint8_t *r = GetGuestRamBase();
      const uint32_t *rs =
          r ? reinterpret_cast<const uint32_t *>(r + 0x0059F940u) : nullptr;
      const uint32_t *ts =
          r ? reinterpret_cast<const uint32_t *>(r + 0x0059F740u) : nullptr;
      const UwpVert v0 = g_vertScratch.empty() ? UwpVert{} : g_vertScratch[0];
      uint32_t t0Data = (g_boundTexVA[0] && r) ? *reinterpret_cast<const uint32_t *>(r + ((g_boundTexVA[0] + 4u) & (kXboxRamSize - 1u))) : 0u;
      std::fprintf(
          g_traceFp,
          "d=%03u vs=0x%04X fvf=0x%03X rhw=%d cnt=%4u nv=%4zu t0=%d(0x%02X,d=0x%08X,p=0x%08X,%ux%u) "
          "t1=%d ab=%u(0x%X,0x%X) at=%u(%u) z=%u/%u cull=0x%X cOp=%u(%u,%u) aOp=%u(%u,%u) "
          "cOp1=%u tci0=%u ttf0=%u fog=%u tf=0x%08X mw=(%.1f,%.1f,%.1f) p0=(%.1f,%.1f,%.1f) "
          "uv0=(%.2f,%.2f) col=(%.2f,%.2f,%.2f,%.2f)\n",
          g_drawNum, g_customVsHandle, g_fvf, isRhw ? 1 : 0, drawCount,
          g_vertScratch.size(), g_boundSrv[0] ? 1 : 0, g_boundTexFmt[0],
          t0Data, g_boundPalVA[0], g_boundTexW[0], g_boundTexH[0],
          g_boundSrv[1] ? 1 : 0, rs ? rs[59] : 0u, rs ? rs[62] : 0u,
          rs ? rs[63] : 0u, rs ? rs[60] : 0u, rs ? (rs[61] & 0xFFu) : 0u,
          rs ? rs[125] : 0u, rs ? rs[64] : 0u, rs ? rs[129] : 0u,
          ts ? ts[12] : 0u, ts ? (ts[14] & 0xFu) : 0u, ts ? (ts[15] & 0xFu) : 0u,
          ts ? ts[16] : 0u, ts ? (ts[18] & 0xFu) : 0u, ts ? (ts[19] & 0xFu) : 0u,
          ts ? ts[32 + 12] : 0u, ts ? (ts[28] & 0x7u) : 0u, ttf0,
          applyFog ? 1u : 0u, rs ? rs[130] : 0u,
          g_mWorld[12], g_mWorld[13], g_mWorld[14],
          v0.x, v0.y, v0.z, v0.u, v0.v, v0.r, v0.g, v0.b, v0.a);
    }
  }

  if (indexed && g_ib) {
    g_ctx->DrawIndexed(drawCount, ibStart, baseVert);
  } else {
    g_ctx->Draw(drawCount, static_cast<UINT>(baseVert));
  }
  ++g_drawNum;
}

static bool ExpandXboxIndices(uint32_t xboxPrimType, const uint16_t *srcIdx,
                              uint32_t count,
                              D3D11_PRIMITIVE_TOPOLOGY &outTopo) {
  g_idxScratch.clear();
  auto idxAt = [&](uint32_t i) -> uint16_t {
    return srcIdx ? srcIdx[i] : static_cast<uint16_t>(i);
  };
  if (xboxPrimType == 8) { // D3DPT_QUADLIST -> TRIANGLELIST (2 tris per quad)
    outTopo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    uint32_t nQuads = count / 4u;
    g_idxScratch.reserve(nQuads * 6u);
    for (uint32_t q = 0; q < nQuads; ++q) {
      uint16_t a = idxAt(q * 4 + 0), b = idxAt(q * 4 + 1), c = idxAt(q * 4 + 2),
               d = idxAt(q * 4 + 3);
      g_idxScratch.push_back(a);
      g_idxScratch.push_back(b);
      g_idxScratch.push_back(c);
      g_idxScratch.push_back(a);
      g_idxScratch.push_back(c);
      g_idxScratch.push_back(d);
    }
    return !g_idxScratch.empty();
  }
  if (xboxPrimType == 7) { // D3DPT_TRIANGLEFAN -> TRIANGLELIST
    outTopo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    if (count < 3u)
      return false;
    g_idxScratch.reserve((count - 2u) * 3u);
    uint16_t v0 = idxAt(0);
    for (uint32_t i = 1; i + 1 < count; ++i) {
      g_idxScratch.push_back(v0);
      g_idxScratch.push_back(idxAt(i));
      g_idxScratch.push_back(idxAt(i + 1));
    }
    return true;
  }
  if (xboxPrimType == 9) { // D3DPT_QUADSTRIP -> TRIANGLELIST
    outTopo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    if (count < 4u)
      return false;
    uint32_t nQuads = (count - 2u) / 2u;
    g_idxScratch.reserve(nQuads * 6u);
    for (uint32_t q = 0; q < nQuads; ++q) {
      uint16_t a = idxAt(q * 2 + 0), b = idxAt(q * 2 + 1), c = idxAt(q * 2 + 2),
               d = idxAt(q * 2 + 3);
      g_idxScratch.push_back(a);
      g_idxScratch.push_back(b);
      g_idxScratch.push_back(c);
      g_idxScratch.push_back(b);
      g_idxScratch.push_back(d);
      g_idxScratch.push_back(c);
    }
    return !g_idxScratch.empty();
  }
  return false;
}

void DrawVerticesUP(uint32_t xboxPrimType, uint32_t vertexCount,
                    const uint8_t *vdata, uint32_t stride) {
  if (!g_dev || !vdata || vertexCount == 0 || stride == 0)
    return;
  bool isRhw = false;
  UnpackFvfVerts(vdata, vertexCount, stride, isRhw);

  D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  bool expanded = ExpandXboxIndices(xboxPrimType, nullptr, vertexCount, topo);
  if (!expanded) {
    if (xboxPrimType == 6)
      topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    else if (xboxPrimType == 2)
      topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
  }

  uint32_t vbStart = 0, ibStart = 0;
  if (expanded) {
    uint32_t ic = static_cast<uint32_t>(g_idxScratch.size());
    UploadRingGeom(vertexCount, g_idxScratch.data(), ic, vbStart, ibStart);
    SubmitDraw(topo, isRhw, true, ic, vbStart, ibStart);
  } else {
    UploadRingGeom(vertexCount, nullptr, 0u, vbStart, ibStart);
    SubmitDraw(topo, isRhw, false, vertexCount, vbStart, 0u);
  }
}

void DrawIndexedVerticesUP(uint32_t xboxPrimType, uint32_t vertexCount,
                           const uint16_t *idx, const uint8_t *vdata,
                           uint32_t stride) {
  if (!g_dev || !idx || !vdata || vertexCount == 0 || stride == 0)
    return;
  uint16_t minIdx = 0xFFFFu, maxIdx = 0;
  for (uint32_t i = 0; i < vertexCount; ++i) {
    uint16_t v = idx[i];
    if (v < minIdx) minIdx = v;
    if (v > maxIdx) maxIdx = v;
  }
  uint32_t numVerts = static_cast<uint32_t>(maxIdx - minIdx) + 1u;

  bool isRhw = false;
  UnpackFvfVerts(vdata + static_cast<size_t>(minIdx) * stride, numVerts, stride, isRhw);

  D3D11_PRIMITIVE_TOPOLOGY topo = (xboxPrimType == 6)
                                      ? D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP
                                      : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  bool expanded = ExpandXboxIndices(xboxPrimType, idx, vertexCount, topo);
  const uint16_t *finalIdx = expanded ? g_idxScratch.data() : idx;
  uint32_t finalCount =
      expanded ? static_cast<uint32_t>(g_idxScratch.size()) : vertexCount;

  uint32_t vbStart = 0, ibStart = 0;
  UploadRingGeom(numVerts, finalIdx, finalCount, vbStart, ibStart);
  SubmitDraw(topo, isRhw, true, finalCount,
             static_cast<INT>(vbStart) - static_cast<INT>(minIdx), ibStart);
}

void DrawIndexedVertices(uint32_t xboxPrimType, uint32_t vertexCount,
                         const uint8_t *guestRam, uint32_t idxParam) {
  ScopedTickAcc _dt(g_accDrawTicks);
  if (!g_dev || vertexCount == 0)
    return;
  const uint16_t *idx = nullptr;
  if (idxParam >= 0x00010000u && guestRam) {
    idx = reinterpret_cast<const uint16_t *>(guestRam +
                                             (idxParam & (kXboxRamSize - 1u)));
  } else if (g_indices) {
    idx = reinterpret_cast<const uint16_t *>(
        reinterpret_cast<const uint8_t *>(g_indices) + idxParam);
  }
  if (!idx)
    return;

  // Multi-stream 3D Volume Light passes (TBufferGroup::IntermediateRender: lanterns, fire, dynamic point lights!)
  if (g_customVsHandle != 0 && (g_boundTexVol[0] || g_boundTexVol[1])) {
    if (!g_enhancedLighting.load(std::memory_order_relaxed)) return;
    if (!g_streams[0] || g_strides[0] < 12u) return;
    uint16_t minIdx = 0xFFFFu, maxIdx = 0;
    for (uint32_t i = 0; i < vertexCount; ++i) {
      uint16_t v = idx[i];
      if (v < minIdx) minIdx = v;
      if (v > maxIdx) maxIdx = v;
    }
    uint32_t numVerts = static_cast<uint32_t>(maxIdx - minIdx) + 1u;
    bool hasS1 = (g_streams[1] != nullptr && g_strides[1] >= 8u);
    bool hasS2 = (g_streams[2] != nullptr && g_strides[2] >= 8u && g_boundSrv[1] != nullptr);
    uint32_t vOff = g_baseVert + minIdx;
    const uint8_t *s0Base = g_streams[0] + vOff * g_strides[0];
    const uint8_t *s1Base = hasS1 ? (g_streams[1] + vOff * g_strides[1]) : s0Base;
    uint32_t s1Str = hasS1 ? g_strides[1] : g_strides[0];
    const uint8_t *s2Base = hasS2 ? (g_streams[2] + vOff * g_strides[2]) : s0Base;
    uint32_t s2Str = hasS2 ? g_strides[2] : g_strides[0];

    g_vertScratch.resize(numVerts);
    for (uint32_t i = 0; i < numVerts; ++i) {
      const float *p0 = reinterpret_cast<const float *>(s0Base + i * g_strides[0]);
      const float *p1 = reinterpret_cast<const float *>(s1Base + i * s1Str);
      UwpVert &o = g_vertScratch[i];
      o.x = p0[0]; o.y = p0[1]; o.z = p0[2];
      o.r = o.g = o.b = o.a = 1.0f;
      o.u = p1[0]; o.v = p1[1]; o.w0 = (s1Str >= 12u) ? p1[2] : 0.0f;
      if (hasS2) {
        const float *p2 = reinterpret_cast<const float *>(s2Base + i * s2Str);
        o.u1 = p2[0]; o.v1 = p2[1]; o.w1 = (s2Str >= 12u) ? p2[2] : 0.0f;
      } else {
        o.u1 = o.u; o.v1 = o.v; o.w1 = o.w0;
      }
    }
    D3D11_PRIMITIVE_TOPOLOGY topo = (xboxPrimType == 6) ? D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    bool expanded = ExpandXboxIndices(xboxPrimType, idx, vertexCount, topo);
    const uint16_t *finalIdx = expanded ? g_idxScratch.data() : idx;
    uint32_t finalCount = expanded ? static_cast<uint32_t>(g_idxScratch.size()) : vertexCount;
    uint32_t vbStart = 0, ibStart = 0;
    UploadRingGeom(numVerts, finalIdx, finalCount, vbStart, ibStart);
    SubmitDraw(topo, false, true, finalCount,
               static_cast<INT>(vbStart) - static_cast<INT>(minIdx), ibStart);
    return;
  }

  // Handle multi-stream & programmable Xbox Vertex Shaders (3D characters,
  // props, menu, levels!)
  if (g_customVsHandle != 0) {
    auto it = g_vsDecls.find(g_customVsHandle);
    if (it != g_vsDecls.end() && it->second.posStream >= 0 &&
        it->second.posStream < 4) {
      const ParsedVsDecl &d = it->second;
      if (!g_streams[d.posStream] || g_strides[d.posStream] < d.posOff + 12u)
        return;

      uint16_t minIdx = 0xFFFFu, maxIdx = 0;
      for (uint32_t i = 0; i < vertexCount; ++i) {
        uint16_t v = idx[i];
        if (v < minIdx) minIdx = v;
        if (v > maxIdx) maxIdx = v;
      }
      uint32_t numVerts = static_cast<uint32_t>(maxIdx - minIdx) + 1u;

      bool hasDiff =
          (d.diffStream >= 0 && d.diffStream < 4 && g_streams[d.diffStream] &&
           g_strides[d.diffStream] >= d.diffOff + 4u);
      bool hasUv0 =
          (d.uv0Stream >= 0 && d.uv0Stream < 4 && g_streams[d.uv0Stream] &&
           g_strides[d.uv0Stream] >= d.uv0Off + 8u);
      bool hasUv1 =
          (d.uv1Stream >= 0 && d.uv1Stream < 4 && g_streams[d.uv1Stream] &&
           g_strides[d.uv1Stream] >= d.uv1Off + 8u);

      uint32_t vOff = g_baseVert + minIdx;
      const uint8_t *posBase = g_streams[d.posStream] +
                               vOff * g_strides[d.posStream] + d.posOff;
      const uint8_t *diffBase =
          hasDiff ? (g_streams[d.diffStream] +
                     vOff * g_strides[d.diffStream] + d.diffOff)
                  : nullptr;
      const uint8_t *uv0Base =
          hasUv0 ? (g_streams[d.uv0Stream] +
                    vOff * g_strides[d.uv0Stream] + d.uv0Off)
                 : nullptr;
      const uint8_t *uv1Base =
          hasUv1 ? (g_streams[d.uv1Stream] +
                    vOff * g_strides[d.uv1Stream] + d.uv1Off)
                 : nullptr;

      const uint8_t *ram = GetGuestRamBase();
      bool alphaBlendOn = ram ? (reinterpret_cast<const uint32_t *>(
                                     ram + 0x0059F940u)[59] != 0u)
                              : true;
      g_vertScratch.resize(numVerts);
      for (uint32_t i = 0; i < numVerts; ++i) {
        UwpVert &o = g_vertScratch[i];
        std::memcpy(&o.x, posBase + i * g_strides[d.posStream], 12);
        if (hasDiff) {
          uint32_t c = 0;
          std::memcpy(&c, diffBase + i * g_strides[d.diffStream], 4);
          if (!alphaBlendOn && c == 0u) {
            o.r = o.g = o.b = o.a = 1.0f;
          } else {
            o.a = static_cast<float>((c >> 24) & 0xFFu) / 255.0f;
            o.r = static_cast<float>((c >> 16) & 0xFFu) / 255.0f;
            o.g = static_cast<float>((c >> 8) & 0xFFu) / 255.0f;
            o.b = static_cast<float>(c & 0xFFu) / 255.0f;
            if (!alphaBlendOn && o.a == 0.0f)
              o.a = 1.0f;
          }
        } else {
          o.r = o.g = o.b = o.a = 1.0f;
        }
        if (hasUv0)
          std::memcpy(&o.u, uv0Base + i * g_strides[d.uv0Stream], 8);
        else {
          o.u = o.v = 0.0f;
        }
        if (hasUv1)
          std::memcpy(&o.u1, uv1Base + i * g_strides[d.uv1Stream], 8);
        else {
          o.u1 = o.u;
          o.v1 = o.v;
        }
      }

      // Apply programmable VS 3x4 World matrix from
      // CD3DMatrixState::m_dRenderRegisters[6..8] (0x00834BC0)
      float savedWorld[16];
      bool appliedConstWorld = false;
      if (d.isProg && guestRam) {
        const float *c6 =
            reinterpret_cast<const float *>(guestRam + 0x00834BC0u);
        const float *c7 =
            reinterpret_cast<const float *>(guestRam + 0x00834BD0u);
        const float *c8 =
            reinterpret_cast<const float *>(guestRam + 0x00834BE0u);
        if (c6[0] != 0.0f || c6[1] != 0.0f || c6[2] != 0.0f || c7[0] != 0.0f ||
            c7[1] != 0.0f || c7[2] != 0.0f) {
          std::memcpy(savedWorld, g_mWorld, 64);
          g_mWorld[0] = c6[0];
          g_mWorld[1] = c7[0];
          g_mWorld[2] = c8[0];
          g_mWorld[3] = 0.0f;
          g_mWorld[4] = c6[1];
          g_mWorld[5] = c7[1];
          g_mWorld[6] = c8[1];
          g_mWorld[7] = 0.0f;
          g_mWorld[8] = c6[2];
          g_mWorld[9] = c7[2];
          g_mWorld[10] = c8[2];
          g_mWorld[11] = 0.0f;
          g_mWorld[12] = c6[3];
          g_mWorld[13] = c7[3];
          g_mWorld[14] = c8[3];
          g_mWorld[15] = 1.0f;
          appliedConstWorld = true;
        }
      }

      D3D11_PRIMITIVE_TOPOLOGY topo =
          (xboxPrimType == 6) ? D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP
                              : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      bool expanded = ExpandXboxIndices(xboxPrimType, idx, vertexCount, topo);
      const uint16_t *finalIdx = expanded ? g_idxScratch.data() : idx;
      uint32_t finalCount =
          expanded ? static_cast<uint32_t>(g_idxScratch.size()) : vertexCount;

      uint32_t vbStart = 0, ibStart = 0;
      UploadRingGeom(numVerts, finalIdx, finalCount, vbStart, ibStart);
      SubmitDraw(topo, false, true, finalCount,
                 static_cast<INT>(vbStart) - static_cast<INT>(minIdx), ibStart);
      if (appliedConstWorld)
        std::memcpy(g_mWorld, savedWorld, 64);
      return;
    }
  }

  if (!g_streams[0] || g_strides[0] == 0)
    return;
  const uint8_t *vbase = g_streams[0] + g_baseVert * g_strides[0];
  DrawIndexedVerticesUP(xboxPrimType, vertexCount, idx, vbase, g_strides[0]);
}

void ResetForRelaunch() {
  for (auto &kv : g_texCache) {
    if (kv.second.srv) kv.second.srv->Release();
    if (kv.second.tex) kv.second.tex->Release();
  }
  g_texCache.clear();
  for (int i = 0; i < 4; ++i) {
    g_streams[i] = nullptr;
    g_strides[i] = 0;
    g_boundSrv[i] = nullptr;
    g_boundPalVA[i] = 0;
    g_boundTexVA[i] = 0;
    g_boundTexVol[i] = false;
    g_boundTexLin[i] = false;
    g_boundTexW[i] = 0;
    g_boundTexH[i] = 0;
    g_boundTexFmt[i] = 0;
  }
  g_indices = nullptr;
  g_baseVert = 0;
  g_customVsHandle = 0;
  g_vsDecls.clear();
  g_has3DVp = false;
  g_fvf = 0x144u;
  static const float kIdent[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  std::memcpy(g_mWorld, kIdent, 64);
  std::memcpy(g_mView,  kIdent, 64);
  std::memcpy(g_mProj,  kIdent, 64);
  std::memcpy(g_mTex0,  kIdent, 64);
  std::memcpy(g_mTex1,  kIdent, 64);
  g_hasTexMat[0] = g_hasTexMat[1] = false;
  g_sceneHas3D = false;
  g_postResolved = false;
  g_boundToSceneRt = false;
  std::memcpy(g_last3DProj, kIdent, 64);
  if (g_ctx) {
    ID3D11ShaderResourceView *nullSrvs[4] = {};
    g_ctx->PSSetShaderResources(0, 4, nullSrvs);
    if (g_rtv) g_ctx->OMSetRenderTargets(1, &g_rtv, g_dsv);
  }
  ResetPipelineCache();
}

void Shutdown() {
  for (auto &kv : g_texCache) {
    if (kv.second.srv)
      kv.second.srv->Release();
    if (kv.second.tex)
      kv.second.tex->Release();
  }
  g_texCache.clear();
  if (g_rsCullNone) {
    g_rsCullNone->Release();
    g_rsCullNone = nullptr;
  }
  if (g_rsCullCW) {
    g_rsCullCW->Release();
    g_rsCullCW = nullptr;
  }
  if (g_rsCullCCW) {
    g_rsCullCCW->Release();
    g_rsCullCCW = nullptr;
  }
  for (auto &kv : g_samplerCache) {
    if (kv.second)
      kv.second->Release();
  }
  g_samplerCache.clear();
  if (g_dsOn) {
    g_dsOn->Release();
    g_dsOn = nullptr;
  }
  if (g_dsNoWrite) {
    g_dsNoWrite->Release();
    g_dsNoWrite = nullptr;
  }
  if (g_dsOff) {
    g_dsOff->Release();
    g_dsOff = nullptr;
  }
  if (g_blendOpaque) {
    g_blendOpaque->Release();
    g_blendOpaque = nullptr;
  }
  if (g_blendAlpha) {
    g_blendAlpha->Release();
    g_blendAlpha = nullptr;
  }
  for (auto &kv : g_blendCache) {
    if (kv.second)
      kv.second->Release();
  }
  g_blendCache.clear();
  if (g_blendAdd) {
    g_blendAdd->Release();
    g_blendAdd = nullptr;
  }
  if (g_blendAddOne) {
    g_blendAddOne->Release();
    g_blendAddOne = nullptr;
  }
  if (g_sampler) {
    g_sampler->Release();
    g_sampler = nullptr;
  }
  if (g_ib) {
    g_ib->Release();
    g_ib = nullptr;
  }
  if (g_vb) {
    g_vb->Release();
    g_vb = nullptr;
  }
  if (g_cb) {
    g_cb->Release();
    g_cb = nullptr;
  }
  if (g_layout) {
    g_layout->Release();
    g_layout = nullptr;
  }
  if (g_ps) {
    g_ps->Release();
    g_ps = nullptr;
  }
  if (g_vs) {
    g_vs->Release();
    g_vs = nullptr;
  }
  auto relPost = [](auto *&p) { if (p) { p->Release(); p = nullptr; } };
  relPost(g_vsPost);
  relPost(g_psSsao);
  relPost(g_psBloomExtract);
  relPost(g_psBloomBlur);
  relPost(g_psComposite);
  relPost(g_cbPost);
  relPost(g_sampLinearClamp);
  relPost(g_sampPointClamp);
  relPost(g_sampVolBorder);
  ReleaseRenderTargets();
  if (g_swapChain) {
    g_swapChain->Release();
    g_swapChain = nullptr;
  }
  if (g_ctx) {
    g_ctx->Release();
    g_ctx = nullptr;
  }
  if (g_dev) {
    g_dev->Release();
    g_dev = nullptr;
  }
}

static void FlushOverlayVerts(const std::vector<UwpVert> &verts) {
  if (!g_ctx || !g_vs || !g_ps || !g_rtv || verts.empty()) return;
  uint32_t n = static_cast<uint32_t>(verts.size());
  g_vertScratch = verts;
  uint32_t vbStart = 0, ibStart = 0;
  UploadRingGeom(n, nullptr, 0u, vbStart, ibStart);

  if (!g_frameBound) {
    UINT stride = sizeof(UwpVert), offset = 0;
    g_ctx->OMSetRenderTargets(1, &g_rtv, g_dsv);
    g_ctx->IASetInputLayout(g_layout);
    g_ctx->IASetVertexBuffers(0, 1, &g_vb, &stride, &offset);
    if (g_ib) g_ctx->IASetIndexBuffer(g_ib, DXGI_FORMAT_R16_UINT, 0);
    g_ctx->VSSetShader(g_vs, nullptr, 0);
    g_ctx->VSSetConstantBuffers(0, 1, &g_cb);
    g_ctx->PSSetShader(g_ps, nullptr, 0);
    g_ctx->PSSetConstantBuffers(0, 1, &g_cb);
    g_frameBound = true;
  }
  D3D11_VIEWPORT fullVp{0.0f, 0.0f, static_cast<float>(g_width), static_cast<float>(g_height), 0.0f, 1.0f};
  g_ctx->RSSetViewports(1, &fullVp);
  g_lastVpMode = 1;

  D3D11_MAPPED_SUBRESOURCE ms{};
  if (SUCCEEDED(g_ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
    auto *cb = static_cast<CbPerDraw *>(ms.pData);
    std::memset(cb, 0, sizeof(CbPerDraw));
    cb->screenParams[0] = 640.0f;
    cb->screenParams[1] = 480.0f;
    cb->screenParams[2] = 1.0f; // isRhw = true
    cb->screenParams[3] = 0.0f; // no texture (pure vertex color)
    cb->uvScale[0] = cb->uvScale[1] = cb->uvScale[2] = cb->uvScale[3] = 1.0f;
    cb->mTex0[0] = cb->mTex0[5] = cb->mTex0[10] = cb->mTex0[15] = 1.0f;
    cb->mTex1[0] = cb->mTex1[5] = cb->mTex1[10] = cb->mTex1[15] = 1.0f;
    g_ctx->Unmap(g_cb, 0);
  }
  if (g_lastTopo != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_lastTopo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  }
  if (g_lastRs != g_rsCullNone) { g_ctx->RSSetState(g_rsCullNone); g_lastRs = g_rsCullNone; }
  if (g_lastDs != g_dsOff)      { g_ctx->OMSetDepthStencilState(g_dsOff, 0); g_lastDs = g_dsOff; }
  if (g_lastBs != g_blendAlpha) { float bf[4] = {}; g_ctx->OMSetBlendState(g_blendAlpha, bf, 0xFFFFFFFFu); g_lastBs = g_blendAlpha; }
  g_ctx->Draw(n, vbStart);
}

static void PushOverlayQuad(std::vector<UwpVert> &v, float x, float y, float w, float h, float r, float g, float b, float a) {
  UwpVert v0{x, y, 0.0f, r, g, b, a, 0, 0, 0, 0, 0, 0};
  UwpVert v1{x + w, y, 0.0f, r, g, b, a, 0, 0, 0, 0, 0, 0};
  UwpVert v2{x + w, y + h, 0.0f, r, g, b, a, 0, 0, 0, 0, 0, 0};
  UwpVert v3{x, y + h, 0.0f, r, g, b, a, 0, 0, 0, 0, 0, 0};
  v.push_back(v0); v.push_back(v1); v.push_back(v2);
  v.push_back(v0); v.push_back(v2); v.push_back(v3);
}

void DrawOverlayRect(float x, float y, float w, float h, uint32_t argb) {
  float a = static_cast<float>((argb >> 24) & 0xFFu) / 255.0f;
  float r = static_cast<float>((argb >> 16) & 0xFFu) / 255.0f;
  float g = static_cast<float>((argb >>  8) & 0xFFu) / 255.0f;
  float b = static_cast<float>( argb        & 0xFFu) / 255.0f;
  std::vector<UwpVert> verts;
  verts.reserve(6);
  PushOverlayQuad(verts, x, y, w, h, r, g, b, a);
  FlushOverlayVerts(verts);
}

// Crisp 5x7 uppercase/ASCII bitmap font (covers 0x20..0x5F; lowercase maps to uppercase for compact readability)
static const uint8_t* GetGlyph5x7(char ch) {
  if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
  static const uint8_t kBlank[7] = {0,0,0,0,0,0,0};
  static const uint8_t kFont[][7] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ' '
    {0x04,0x04,0x04,0x04,0x04,0x00,0x04}, // '!'
    {0x0A,0x0A,0x00,0x00,0x00,0x00,0x00}, // '"'
    {0x0A,0x1F,0x0A,0x0A,0x1F,0x0A,0x00}, // '#'
    {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04}, // '$'
    {0x18,0x19,0x02,0x04,0x08,0x13,0x03}, // '%'
    {0x0C,0x12,0x14,0x08,0x15,0x12,0x0D}, // '&'
    {0x04,0x04,0x00,0x00,0x00,0x00,0x00}, // '''
    {0x02,0x04,0x08,0x08,0x08,0x04,0x02}, // '('
    {0x08,0x04,0x02,0x02,0x02,0x04,0x08}, // ')'
    {0x00,0x0A,0x04,0x1F,0x04,0x0A,0x00}, // '*'
    {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}, // '+'
    {0x00,0x00,0x00,0x00,0x00,0x04,0x08}, // ','
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, // '-'
    {0x00,0x00,0x00,0x00,0x00,0x06,0x06}, // '.'
    {0x01,0x01,0x02,0x04,0x08,0x10,0x10}, // '/'
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, // '0'
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, // '1'
    {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F}, // '2'
    {0x1F,0x02,0x04,0x06,0x01,0x11,0x0E}, // '3'
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, // '4'
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, // '5'
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, // '6'
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, // '7'
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, // '8'
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, // '9'
    {0x00,0x06,0x06,0x00,0x06,0x06,0x00}, // ':'
    {0x00,0x06,0x06,0x00,0x06,0x04,0x08}, // ';'
    {0x02,0x04,0x08,0x10,0x08,0x04,0x02}, // '<'
    {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}, // '='
    {0x08,0x04,0x02,0x01,0x02,0x04,0x08}, // '>'
    {0x0E,0x11,0x01,0x02,0x04,0x00,0x04}, // '?'
    {0x0E,0x11,0x17,0x15,0x17,0x10,0x0E}, // '@'
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, // 'A'
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, // 'B'
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, // 'C'
    {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}, // 'D'
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, // 'E'
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, // 'F'
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, // 'G'
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, // 'H'
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, // 'I'
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, // 'J'
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, // 'K'
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, // 'L'
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, // 'M'
    {0x11,0x19,0x15,0x13,0x11,0x11,0x11}, // 'N'
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, // 'O'
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, // 'P'
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, // 'Q'
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, // 'R'
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, // 'S'
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, // 'T'
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, // 'U'
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, // 'V'
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}, // 'W'
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, // 'X'
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, // 'Y'
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, // 'Z'
    {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}, // '['
    {0x10,0x10,0x08,0x04,0x02,0x01,0x01}, // '\'
    {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}, // ']'
    {0x04,0x0A,0x11,0x00,0x00,0x00,0x00}, // '^'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x1F}, // '_'
  };
  if (ch == '|') { static const uint8_t kPipe[7] = {0x04,0x04,0x04,0x04,0x04,0x04,0x04}; return kPipe; }
  if (ch >= 0x20 && ch <= 0x5F) return kFont[ch - 0x20];
  return kBlank;
}

void DrawOverlayText(float x, float y, float scale, uint32_t argb, const char *text) {
  if (!text || !*text) return;
  float a = static_cast<float>((argb >> 24) & 0xFFu) / 255.0f;
  float r = static_cast<float>((argb >> 16) & 0xFFu) / 255.0f;
  float g = static_cast<float>((argb >>  8) & 0xFFu) / 255.0f;
  float b = static_cast<float>( argb        & 0xFFu) / 255.0f;
  float px = 1.0f * scale;
  float py = 1.25f * scale;
  std::vector<UwpVert> verts;
  verts.reserve(std::strlen(text) * 90u);
  float cx = x;
  for (const char *p = text; *p; ++p) {
    const uint8_t *rows = GetGlyph5x7(*p);
    for (int ry = 0; ry < 7; ++ry) {
      uint8_t bits = rows[ry];
      if (!bits) continue;
      for (int rx = 0; rx < 5; ++rx) {
        if (bits & (0x10u >> rx)) {
          PushOverlayQuad(verts, cx + rx * px, y + ry * py, px, py, r, g, b, a);
        }
      }
    }
    cx += 6.0f * px;
  }
  FlushOverlayVerts(verts);
}

bool IsReady() { return g_dev != nullptr && g_swapChain != nullptr; }
uint32_t GetFrameCount() { return g_frameNum; }

} // namespace d3d11_bridge
} // namespace uwp_recomp
