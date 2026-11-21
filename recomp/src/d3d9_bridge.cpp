#include "d3d9_bridge.h"
#include <d3d9.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using std::min;
using std::max;
#include <objidl.h>
#include <gdiplus.h>

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")

namespace d3d9_bridge {

struct XboxD3DResource {
    uint32_t Common;
    uint32_t Data;
    uint32_t Lock;
};

struct XboxD3DBaseTexture {
    uint32_t Common;
    uint32_t Data;
    uint32_t Lock;
    uint32_t Format;
    uint32_t Size;
};

struct CachedTexture {
    IDirect3DBaseTexture9* tex      = nullptr;
    uint32_t               data     = 0;
    uint32_t               format   = 0;
    uint32_t               size     = 0;
    uint32_t               width    = 0;
    uint32_t               height   = 0;
    bool                   linear   = false;
    const uint32_t*        palette  = nullptr;
    uint64_t               hash     = 0;
    bool                   isVolume = false;
};

static IDirect3D9*       g_d3d9      = nullptr;
static IDirect3DDevice9* g_dev       = nullptr;
static bool              g_inScene   = false;
static uint32_t          g_fvf       = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;
static bool              g_customVS  = false;
static uint32_t          g_customVSHandle = 0;

static const uint8_t*    g_streams[4] = {};
static uint32_t          g_strides[4] = {};
static std::vector<float> g_volVertScratch;
static std::vector<uint8_t> g_customVsScratch;

struct ParsedVsDecl {
    int32_t  shaderId   = -1;
    int8_t   posStream  = -1; uint16_t posOff  = 0;
    int8_t   diffStream = -1; uint16_t diffOff = 0;
    int8_t   uv0Stream  = -1; uint16_t uv0Off  = 0; uint8_t uv0Floats = 0;
    int8_t   uv1Stream  = -1; uint16_t uv1Off  = 0; uint8_t uv1Floats = 0;
};
static std::unordered_map<uint32_t, ParsedVsDecl> g_vsDecls;
static const uint16_t*   g_indices   = nullptr;
static uint32_t          g_baseVert  = 0;
static const uint32_t*   g_palettes[4] = {};
static uint32_t          g_boundTexW[4] = {};
static uint32_t          g_boundTexH[4] = {};
static bool              g_boundTexLin[4] = {};
static bool              g_boundTexPresent[4] = {};
static IDirect3DBaseTexture9* g_boundTexPtr[4] = {};
static bool              g_boundTexVol[4] = {};
static uint64_t          g_boundTexHash[4] = {};
static uint32_t          g_boundTexFmt[4] = {};
static D3DMATRIX         g_gameTexMatrix[4] = {};
static bool              g_hasGameTexMatrix[4] = {};

static uint32_t          g_xboxW     = 640;
static uint32_t          g_xboxH     = 480;
static uint32_t          g_outW      = 1920;
static uint32_t          g_outH      = 1440;
static float             g_scaleX    = 3.0f;
static float             g_scaleY    = 3.0f;
static float             g_rhwScaleX = 3.0f;
static float             g_rhwOffX   = 0.0f;
static uint32_t          g_curScale  = 3;
static uint32_t          g_curMsaa   = 4;
static uint32_t          g_curAspect = 0; // 0=4:3, 1=16:9, 2=21:9
static DWORD             g_maxAniso  = 16;
static DWORD             g_hwMaxAniso = 16;
static D3DPRESENT_PARAMETERS g_presentParams{};
static D3DVIEWPORT9      g_lastXboxVp{0, 0, 640, 480, 0.0f, 1.0f};
static D3DMATRIX         g_lastProjMatrix{};
static bool              g_hasLastProj = false;
static D3DMATRIX         g_lastWorldMatrix = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
static std::atomic<bool> g_resetPending{false};
static std::atomic<uint32_t> g_reqScale{3};
static std::atomic<uint32_t> g_reqMsaa{4};
static std::atomic<uint32_t> g_reqAniso{16};
static std::atomic<uint32_t> g_reqAspect{0};
static std::atomic<bool>     g_fogEnabled{true};
static bool                  g_menuPillarbox = false;
static std::atomic<bool> g_traceRequested{false};
static bool              g_tracingThisFrame = false;
static uint32_t          g_traceDrawIdx     = 0;
static FILE*             g_traceFile        = nullptr;

void SetFogEnabled(bool enabled) {
    g_fogEnabled.store(enabled);
    LogPrintf("[D3D9] Atmospheric Fog %s\n", enabled ? "ENABLED" : "DISABLED");
}

bool IsFogEnabled() {
    return g_fogEnabled.load();
}

void SetMenuPillarbox(bool enabled) {
    if (g_menuPillarbox != enabled) {
        g_menuPillarbox = enabled;
        if (g_dev) {
            SetViewport(&g_lastXboxVp);
        }
    }
}

void RequestFrameTrace() {
    g_traceRequested.store(true);
    LogPrintf("[D3D9-TRACE] Next frame draw calls will be saved to recomp\\frame_trace.log\n");
}

static std::unordered_map<const void*, CachedTexture> g_texCache;
static std::vector<uint8_t>  g_quadVertScratch;
static std::vector<uint16_t> g_quadIdxScratch;
static std::vector<uint8_t>  g_rhwVertScratch;

// HD Texture Replacement Pack & Dumper state
static std::string                              g_hdTexDir;
static std::string                              g_dumpTexDir;
static std::unordered_map<uint64_t, std::string> g_hdTextureFiles;
static std::unordered_set<uint64_t>             g_dumpedHashes;
static std::atomic<bool>                        g_hdTexturesEnabled{true};
static std::atomic<bool>                        g_dumpTextures{false};
static std::atomic<bool>                        g_flushCacheNextFrame{false};
static ULONG_PTR                                g_gdiplusToken = 0;
static CLSID                                    g_pngClsid{};
static bool                                     g_hasPngClsid  = false;

static bool EnsureGdiPlusInit() {
    if (g_gdiplusToken != 0) return true;
    Gdiplus::GdiplusStartupInput si{};
    if (Gdiplus::GdiplusStartup(&g_gdiplusToken, &si, nullptr) != Gdiplus::Ok) {
        g_gdiplusToken = 0;
        return false;
    }
    UINT num = 0, size = 0;
    if (Gdiplus::GetImageEncodersSize(&num, &size) == Gdiplus::Ok && size > 0) {
        std::vector<uint8_t> buf(size);
        auto* enc = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
        if (Gdiplus::GetImageEncoders(num, size, enc) == Gdiplus::Ok) {
            for (UINT i = 0; i < num; ++i) {
                if (enc[i].MimeType && wcscmp(enc[i].MimeType, L"image/png") == 0) {
                    g_pngClsid = enc[i].Clsid;
                    g_hasPngClsid = true;
                    break;
                }
            }
        }
    }
    return true;
}

static bool ParseLeadingHex64(const char* name, uint64_t& outHash) {
    uint64_t h = 0;
    for (int i = 0; i < 16; ++i) {
        char c = name[i];
        uint64_t nib = 0;
        if (c >= '0' && c <= '9') nib = static_cast<uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f') nib = static_cast<uint64_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') nib = static_cast<uint64_t>(c - 'A' + 10);
        else return false;
        h = (h << 4) | nib;
    }
    // Must be followed by '.', '_', '-', or end of string
    char next = name[16];
    if (next != '.' && next != '_' && next != '-' && next != '\0') return false;
    outHash = h;
    return true;
}

static bool HasSupportedImageExt(const std::string& fn) {
    auto dot = fn.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = fn.substr(dot);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga");
}

static void ScanHDTextureDir() {
    g_hdTextureFiles.clear();
    if (g_hdTexDir.empty()) return;
    DWORD attr = GetFileAttributesA(g_hdTexDir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return;

    std::string pattern = g_hdTexDir + "\\*.*";
    WIN32_FIND_DATAA fd{};
    HANDLE hFind = FindFirstFileA(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string name = fd.cFileName;
        if (!HasSupportedImageExt(name)) continue;
        uint64_t hash = 0;
        if (ParseLeadingHex64(name.c_str(), hash)) {
            g_hdTextureFiles[hash] = g_hdTexDir + "\\" + name;
        }
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);

    if (!g_hdTextureFiles.empty()) {
        LogPrintf("[HDTex] Loaded index of %zu custom HD texture(s) from '%s'\n",
                  g_hdTextureFiles.size(), g_hdTexDir.c_str());
    }
}

void SetTexturePackDirs(const char* hdTexturesDir, const char* dumpTexturesDir) {
    g_hdTexDir   = hdTexturesDir   ? hdTexturesDir   : "";
    g_dumpTexDir = dumpTexturesDir ? dumpTexturesDir : "";
    if (!g_hdTexDir.empty()) {
        CreateDirectoryA(g_hdTexDir.c_str(), nullptr);
        ScanHDTextureDir();
    }
}

void SetHDTexturesEnabled(bool enabled) {
    bool prev = g_hdTexturesEnabled.exchange(enabled);
    if (prev != enabled) {
        if (enabled) ScanHDTextureDir();
        g_flushCacheNextFrame.store(true);
        LogPrintf("[HDTex] Custom HD textures %s\n", enabled ? "ENABLED" : "DISABLED");
    }
}

bool IsHDTexturesEnabled() {
    return g_hdTexturesEnabled.load();
}

void SetTextureDumpEnabled(bool enabled) {
    g_dumpTextures.store(enabled);
    if (enabled) {
        if (!g_dumpTexDir.empty()) CreateDirectoryA(g_dumpTexDir.c_str(), nullptr);
        g_dumpedHashes.clear();
        g_flushCacheNextFrame.store(true);
        LogPrintf("[HDTex] Texture dumping ENABLED -> '%s'\n", g_dumpTexDir.c_str());
    } else {
        LogPrintf("[HDTex] Texture dumping DISABLED\n");
    }
}

bool IsTextureDumpEnabled() {
    return g_dumpTextures.load();
}

bool ToggleTextureDump() {
    bool next = !g_dumpTextures.load();
    SetTextureDumpEnabled(next);
    return next;
}

void ReloadHDTextures() {
    ScanHDTextureDir();
    g_flushCacheNextFrame.store(true);
    if (g_hdTextureFiles.empty()) {
        LogPrintf("[HDTex] Reloaded: '%s' is empty (using original Xbox textures)\n", g_hdTexDir.c_str());
    }
}

static uint64_t HashXboxTexture(uint32_t width, uint32_t height, uint32_t fmtByte, const uint8_t* data, uint32_t len) {
    uint64_t h = 0xcbf29ce484222325ULL;
    auto mix = [&](uint8_t b) { h ^= b; h *= 0x100000001b3ULL; };
    mix(static_cast<uint8_t>(width & 0xFF));
    mix(static_cast<uint8_t>((width >> 8) & 0xFF));
    mix(static_cast<uint8_t>(height & 0xFF));
    mix(static_cast<uint8_t>((height >> 8) & 0xFF));
    mix(static_cast<uint8_t>(fmtByte & 0xFF));
    for (uint32_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

static bool LoadImageFileToARGB(const std::string& path, uint32_t& outW, uint32_t& outH, std::vector<uint32_t>& outARGB) {
    // Handle uncompressed 24/32-bit TGA natively
    if (path.size() >= 4) {
        std::string ext = path.substr(path.size() - 4);
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".tga") {
            FILE* f = std::fopen(path.c_str(), "rb");
            if (!f) return false;
            uint8_t hdr[18]{};
            if (std::fread(hdr, 1, 18, f) != 18) { std::fclose(f); return false; }
            uint8_t idLen = hdr[0], imgType = hdr[2], bpp = hdr[16], desc = hdr[17];
            uint32_t w = hdr[12] | (hdr[13] << 8);
            uint32_t h = hdr[14] | (hdr[15] << 8);
            if (imgType != 2 || (bpp != 24 && bpp != 32) || w == 0 || h == 0 || w > 4096 || h > 4096) {
                std::fclose(f);
                return false;
            }
            if (idLen) std::fseek(f, idLen, SEEK_CUR);
            uint32_t bytesPerPx = bpp / 8;
            std::vector<uint8_t> raw(w * h * bytesPerPx);
            if (std::fread(raw.data(), 1, raw.size(), f) != raw.size()) { std::fclose(f); return false; }
            std::fclose(f);
            bool topOrigin = (desc & 0x20) != 0;
            outW = w; outH = h;
            outARGB.resize(w * h);
            for (uint32_t y = 0; y < h; ++y) {
                uint32_t sy = topOrigin ? y : (h - 1 - y);
                const uint8_t* srcRow = raw.data() + sy * w * bytesPerPx;
                uint32_t* dstRow = outARGB.data() + y * w;
                for (uint32_t x = 0; x < w; ++x) {
                    uint8_t b = srcRow[x * bytesPerPx + 0];
                    uint8_t g = srcRow[x * bytesPerPx + 1];
                    uint8_t r = srcRow[x * bytesPerPx + 2];
                    uint8_t a = (bytesPerPx == 4) ? srcRow[x * bytesPerPx + 3] : 255;
                    dstRow[x] = (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) |
                                (static_cast<uint32_t>(g) << 8) | b;
                }
            }
            return true;
        }
    }

    // PNG / JPG / BMP via built-in Windows GDI+
    if (!EnsureGdiPlusInit()) return false;
    int wlen = MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return false;
    std::wstring wpath(wlen, L'\0');
    MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, &wpath[0], wlen);

    Gdiplus::Bitmap bmp(wpath.c_str());
    if (bmp.GetLastStatus() != Gdiplus::Ok) return false;
    uint32_t w = bmp.GetWidth();
    uint32_t h = bmp.GetHeight();
    if (w == 0 || h == 0 || w > 4096 || h > 4096) return false;

    Gdiplus::Rect rc(0, 0, static_cast<INT>(w), static_cast<INT>(h));
    Gdiplus::BitmapData bd{};
    if (bmp.LockBits(&rc, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) != Gdiplus::Ok) {
        return false;
    }
    outW = w;
    outH = h;
    outARGB.resize(w * h);
    const uint8_t* src = static_cast<const uint8_t*>(bd.Scan0);
    for (uint32_t y = 0; y < h; ++y) {
        std::memcpy(outARGB.data() + y * w, src + y * bd.Stride, w * 4);
    }
    bmp.UnlockBits(&bd);
    return true;
}

static IDirect3DTexture9* UploadARGBTextureWithMips(uint32_t w, uint32_t h, const std::vector<uint32_t>& lvl0) {
    bool isPow2 = (w > 0 && h > 0 && (w & (w - 1)) == 0 && (h & (h - 1)) == 0);
    uint32_t mipLevels = 1;
    if (isPow2) {
        for (uint32_t tw = w, th = h; tw > 1 || th > 1; tw = std::max<uint32_t>(1, tw >> 1), th = std::max<uint32_t>(1, th >> 1)) {
            ++mipLevels;
        }
    }
    IDirect3DTexture9* tex = nullptr;
    if (FAILED(g_dev->CreateTexture(w, h, mipLevels, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr)) || !tex) {
        mipLevels = 1;
        if (FAILED(g_dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr)) || !tex) {
            return nullptr;
        }
    }
    std::vector<uint32_t> cur = lvl0;
    uint32_t cw = w, ch = h;
    for (uint32_t m = 0; m < mipLevels; ++m) {
        D3DLOCKED_RECT lr{};
        if (SUCCEEDED(tex->LockRect(m, &lr, nullptr, 0))) {
            uint8_t* dst = static_cast<uint8_t*>(lr.pBits);
            for (uint32_t y = 0; y < ch; ++y) {
                std::memcpy(dst + y * lr.Pitch, cur.data() + y * cw, cw * 4);
            }
            tex->UnlockRect(m);
        }
        if (m + 1 < mipLevels) {
            uint32_t nw = std::max<uint32_t>(1u, cw >> 1);
            uint32_t nh = std::max<uint32_t>(1u, ch >> 1);
            std::vector<uint32_t> next(nw * nh);
            for (uint32_t y = 0; y < nh; ++y) {
                uint32_t sy0 = std::min<uint32_t>(ch - 1, y * 2);
                uint32_t sy1 = std::min<uint32_t>(ch - 1, y * 2 + 1);
                for (uint32_t x = 0; x < nw; ++x) {
                    uint32_t sx0 = std::min<uint32_t>(cw - 1, x * 2);
                    uint32_t sx1 = std::min<uint32_t>(cw - 1, x * 2 + 1);
                    uint32_t p00 = cur[sy0 * cw + sx0], p10 = cur[sy0 * cw + sx1];
                    uint32_t p01 = cur[sy1 * cw + sx0], p11 = cur[sy1 * cw + sx1];
                    uint32_t a = (((p00 >> 24) & 0xFF) + ((p10 >> 24) & 0xFF) + ((p01 >> 24) & 0xFF) + ((p11 >> 24) & 0xFF) + 2) >> 2;
                    uint32_t r = (((p00 >> 16) & 0xFF) + ((p10 >> 16) & 0xFF) + ((p01 >> 16) & 0xFF) + ((p11 >> 16) & 0xFF) + 2) >> 2;
                    uint32_t g = (((p00 >>  8) & 0xFF) + ((p10 >>  8) & 0xFF) + ((p01 >>  8) & 0xFF) + ((p11 >>  8) & 0xFF) + 2) >> 2;
                    uint32_t b = (((p00      ) & 0xFF) + ((p10      ) & 0xFF) + ((p01      ) & 0xFF) + ((p11      ) & 0xFF) + 2) >> 2;
                    next[y * nw + x] = (a << 24) | (r << 16) | (g << 8) | b;
                }
            }
            cur.swap(next);
            cw = nw; ch = nh;
        }
    }
    return tex;
}

static void DecodeDxtBlockToARGB(const uint8_t* blk, uint32_t fmtByte, uint32_t out4x4[16]) {
    uint8_t alphas[16];
    const uint8_t* cblk = blk;
    if (fmtByte == 0x0E) { // DXT3 explicit 4-bit alpha
        for (int i = 0; i < 8; ++i) {
            uint8_t b = blk[i];
            alphas[i * 2 + 0] = static_cast<uint8_t>((b & 0x0Fu) * 17u);
            alphas[i * 2 + 1] = static_cast<uint8_t>((b >> 4) * 17u);
        }
        cblk = blk + 8;
    } else if (fmtByte == 0x0F) { // DXT5 interpolated alpha
        uint8_t a[8];
        a[0] = blk[0]; a[1] = blk[1];
        if (a[0] > a[1]) {
            for (int i = 1; i <= 6; ++i) a[i + 1] = static_cast<uint8_t>(((7 - i) * a[0] + i * a[1]) / 7);
        } else {
            for (int i = 1; i <= 4; ++i) a[i + 1] = static_cast<uint8_t>(((5 - i) * a[0] + i * a[1]) / 5);
            a[6] = 0; a[7] = 255;
        }
        uint64_t bits = 0;
        for (int i = 0; i < 6; ++i) bits |= static_cast<uint64_t>(blk[2 + i]) << (8 * i);
        for (int i = 0; i < 16; ++i) alphas[i] = a[(bits >> (3 * i)) & 7u];
        cblk = blk + 8;
    } else {
        std::memset(alphas, 255, sizeof(alphas));
    }

    uint16_t c0 = static_cast<uint16_t>(cblk[0] | (cblk[1] << 8));
    uint16_t c1 = static_cast<uint16_t>(cblk[2] | (cblk[3] << 8));
    auto unpack565 = [](uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b) {
        r = static_cast<uint8_t>(((c >> 11) & 31) * 255 / 31);
        g = static_cast<uint8_t>(((c >>  5) & 63) * 255 / 63);
        b = static_cast<uint8_t>(( c        & 31) * 255 / 31);
    };
    uint8_t cr[4], cg[4], cb[4], ca[4] = {255, 255, 255, 255};
    unpack565(c0, cr[0], cg[0], cb[0]);
    unpack565(c1, cr[1], cg[1], cb[1]);
    if (c0 > c1 || fmtByte != 0x0C) {
        cr[2] = static_cast<uint8_t>((2 * cr[0] + cr[1]) / 3);
        cg[2] = static_cast<uint8_t>((2 * cg[0] + cg[1]) / 3);
        cb[2] = static_cast<uint8_t>((2 * cb[0] + cb[1]) / 3);
        cr[3] = static_cast<uint8_t>((cr[0] + 2 * cr[1]) / 3);
        cg[3] = static_cast<uint8_t>((cg[0] + 2 * cg[1]) / 3);
        cb[3] = static_cast<uint8_t>((cb[0] + 2 * cb[1]) / 3);
    } else {
        cr[2] = static_cast<uint8_t>((cr[0] + cr[1]) / 2);
        cg[2] = static_cast<uint8_t>((cg[0] + cg[1]) / 2);
        cb[2] = static_cast<uint8_t>((cb[0] + cb[1]) / 2);
        cr[3] = cg[3] = cb[3] = 0; ca[3] = 0;
    }
    uint32_t idx = cblk[4] | (cblk[5] << 8) | (cblk[6] << 16) | (cblk[7] << 24);
    for (int i = 0; i < 16; ++i) {
        uint32_t k = (idx >> (2 * i)) & 3u;
        uint8_t a = (fmtByte == 0x0C) ? ca[k] : alphas[i];
        out4x4[i] = (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(cr[k]) << 16) |
                    (static_cast<uint32_t>(cg[k]) << 8) | cb[k];
    }
}

static void SaveARGBToPNG(const std::string& path, uint32_t w, uint32_t h, const std::vector<uint32_t>& argb) {
    if (!EnsureGdiPlusInit() || !g_hasPngClsid || w == 0 || h == 0) return;
    int wlen = MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return;
    std::wstring wpath(wlen, L'\0');
    MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, &wpath[0], wlen);
    Gdiplus::Bitmap bmp(static_cast<INT>(w), static_cast<INT>(h), static_cast<INT>(w * 4),
                        PixelFormat32bppARGB, reinterpret_cast<BYTE*>(const_cast<uint32_t*>(argb.data())));
    bmp.Save(wpath.c_str(), &g_pngClsid, nullptr);
}

static void DumpXboxTextureLevel0(uint64_t hash, uint32_t width, uint32_t height, uint32_t fmtByte,
                                  D3DFORMAT d3dFmt, bool isDxt, IDirect3DTexture9* tex, const uint8_t* rawLvl0) {
    if (g_dumpTexDir.empty() || !g_dumpedHashes.insert(hash).second) return;
    std::vector<uint32_t> argb(width * height, 0xFF000000u);
    if (isDxt) {
        uint32_t bw = std::max<uint32_t>(1u, (width + 3) / 4);
        uint32_t bh = std::max<uint32_t>(1u, (height + 3) / 4);
        uint32_t blkSize = (fmtByte == 0x0C) ? 8u : 16u;
        uint32_t blockPx[16];
        for (uint32_t by = 0; by < bh; ++by) {
            for (uint32_t bx = 0; bx < bw; ++bx) {
                DecodeDxtBlockToARGB(rawLvl0 + (by * bw + bx) * blkSize, fmtByte, blockPx);
                for (uint32_t py = 0; py < 4; ++py) {
                    uint32_t dy = by * 4 + py;
                    if (dy >= height) continue;
                    for (uint32_t px = 0; px < 4; ++px) {
                        uint32_t dx = bx * 4 + px;
                        if (dx >= width) continue;
                        argb[dy * width + dx] = blockPx[py * 4 + px];
                    }
                }
            }
        }
    } else if (tex) {
        D3DLOCKED_RECT lr{};
        if (SUCCEEDED(tex->LockRect(0, &lr, nullptr, D3DLOCK_READONLY))) {
            const uint8_t* src = static_cast<const uint8_t*>(lr.pBits);
            for (uint32_t y = 0; y < height; ++y) {
                const uint8_t* row = src + y * lr.Pitch;
                uint32_t* out = argb.data() + y * width;
                if (d3dFmt == D3DFMT_A8R8G8B8) {
                    std::memcpy(out, row, width * 4);
                } else if (d3dFmt == D3DFMT_X8R8G8B8) {
                    const uint32_t* p = reinterpret_cast<const uint32_t*>(row);
                    for (uint32_t x = 0; x < width; ++x) out[x] = p[x] | 0xFF000000u;
                } else if (d3dFmt == D3DFMT_R5G6B5) {
                    const uint16_t* p = reinterpret_cast<const uint16_t*>(row);
                    for (uint32_t x = 0; x < width; ++x) {
                        uint16_t c = p[x];
                        uint32_t r = ((c >> 11) & 31) * 255 / 31;
                        uint32_t g = ((c >>  5) & 63) * 255 / 63;
                        uint32_t b = ( c        & 31) * 255 / 31;
                        out[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
                    }
                } else if (d3dFmt == D3DFMT_A4R4G4B4) {
                    const uint16_t* p = reinterpret_cast<const uint16_t*>(row);
                    for (uint32_t x = 0; x < width; ++x) {
                        uint16_t c = p[x];
                        uint32_t a = ((c >> 12) & 15) * 17;
                        uint32_t r = ((c >>  8) & 15) * 17;
                        uint32_t g = ((c >>  4) & 15) * 17;
                        uint32_t b = ( c        & 15) * 17;
                        out[x] = (a << 24) | (r << 16) | (g << 8) | b;
                    }
                } else if (d3dFmt == D3DFMT_A1R5G5B5 || d3dFmt == D3DFMT_X1R5G5B5) {
                    const uint16_t* p = reinterpret_cast<const uint16_t*>(row);
                    for (uint32_t x = 0; x < width; ++x) {
                        uint16_t c = p[x];
                        uint32_t a = (d3dFmt == D3DFMT_X1R5G5B5 || (c & 0x8000u)) ? 255u : 0u;
                        uint32_t r = ((c >> 10) & 31) * 255 / 31;
                        uint32_t g = ((c >>  5) & 31) * 255 / 31;
                        uint32_t b = ( c        & 31) * 255 / 31;
                        out[x] = (a << 24) | (r << 16) | (g << 8) | b;
                    }
                }
            }
            tex->UnlockRect(0);
        }
    }
    char fname[128]{};
    std::snprintf(fname, sizeof(fname), "\\%016llx_%ux%u.png",
                  static_cast<unsigned long long>(hash), width, height);
    SaveARGBToPNG(g_dumpTexDir + fname, width, height, argb);
}

static void EnsureSceneBegun() {
    if (g_dev && !g_inScene) {
        if (SUCCEEDED(g_dev->BeginScene())) {
            g_inScene = true;
        }
    }
}

static D3DMULTISAMPLE_TYPE ProbeMsaaType(uint32_t desiredSamples) {
    if (!g_d3d9 || desiredSamples < 2) return D3DMULTISAMPLE_NONE;
    const D3DMULTISAMPLE_TYPE candidates[] = { D3DMULTISAMPLE_4_SAMPLES, D3DMULTISAMPLE_2_SAMPLES };
    for (D3DMULTISAMPLE_TYPE mt : candidates) {
        if (static_cast<uint32_t>(mt) > desiredSamples) continue;
        DWORD qColor = 0, qDepth = 0;
        if (SUCCEEDED(g_d3d9->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, TRUE, mt, &qColor)) &&
            SUCCEEDED(g_d3d9->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_D24S8, TRUE, mt, &qDepth))) {
            return mt;
        }
    }
    return D3DMULTISAMPLE_NONE;
}

static void ApplyInitialFixedState(D3DMULTISAMPLE_TYPE msaaType) {
    if (!g_dev) return;
    g_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    g_dev->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, (msaaType != D3DMULTISAMPLE_NONE) ? TRUE : FALSE);

    g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
}

static void ComputeOutputResolution(uint32_t scale, uint32_t aspectMode, uint32_t& outW, uint32_t& outH) {
    if (aspectMode == 2) { // 21:9 Ultrawide (exact 3440x1440 Alienware 34" QD-OLED ratio)
        switch (scale) {
            case 1:  outW = 1148; outH = 480;  break;
            case 2:  outW = 1720; outH = 720;  break;
            case 3:  outW = 2580; outH = 1080; break;
            default: outW = 3440; outH = 1440; break;
        }
    } else if (aspectMode == 1) { // 16:9 Widescreen
        switch (scale) {
            case 1:  outW = 854;  outH = 480;  break;
            case 2:  outW = 1280; outH = 720;  break;
            case 3:  outW = 1920; outH = 1080; break;
            default: outW = 2560; outH = 1440; break;
        }
    } else { // 4:3 Classic
        outW = g_xboxW * scale;
        outH = g_xboxH * scale;
    }
}

static void UpdateScaleMetrics(uint32_t outW, uint32_t outH, uint32_t aspectMode) {
    g_outW   = outW;
    g_outH   = outH;
    g_scaleX = static_cast<float>(outW) / static_cast<float>(g_xboxW);
    g_scaleY = static_cast<float>(outH) / static_cast<float>(g_xboxH);
    if (aspectMode != 0) {
        // Keep 2D HUD/menus unstretched (4:3 aspect centered inside 16:9 or 21:9)
        g_rhwScaleX = g_scaleY;
        g_rhwOffX   = (static_cast<float>(outW) - static_cast<float>(g_xboxW) * g_scaleY) * 0.5f;
    } else {
        g_rhwScaleX = g_scaleX;
        g_rhwOffX   = 0.0f;
    }
}

void RequestGraphicsSettings(uint32_t resScale, uint32_t msaaSamples, uint32_t maxAniso, uint32_t aspectMode) {
    uint32_t s = std::max<uint32_t>(1, std::min<uint32_t>(4, resScale));
    uint32_t m = (msaaSamples >= 4) ? 4 : ((msaaSamples >= 2) ? 2 : 0);
    uint32_t a = std::max<uint32_t>(1, std::min<uint32_t>(16, maxAniso));
    uint32_t asp = (aspectMode <= 2) ? aspectMode : 0;
    g_reqScale.store(s);
    g_reqMsaa.store(m);
    g_reqAniso.store(a);
    g_reqAspect.store(asp);
    if (g_dev) {
        g_resetPending.store(true);
    } else {
        g_curScale  = s;
        g_curMsaa   = m;
        g_maxAniso  = a;
        g_curAspect = asp;
    }
}

uint32_t GetResolutionScale() { return g_reqScale.load(); }
uint32_t GetMsaaSamples()     { return g_reqMsaa.load(); }
uint32_t GetAnisoLevel()      { return g_reqAniso.load(); }
uint32_t GetAspectMode()      { return g_reqAspect.load(); }
uint32_t GetOutputWidth()     { return g_outW; }
uint32_t GetOutputHeight()    { return g_outH; }

bool Init(HWND hRenderWnd, uint32_t xboxWidth, uint32_t xboxHeight, uint32_t resScale) {
    if (!hRenderWnd) return false;
    if (g_dev) return true;

    g_d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_d3d9) {
        LogPrintf("[D3D9-ERROR] Direct3DCreate9 failed!\n");
        return false;
    }

    g_xboxW  = xboxWidth ? xboxWidth : 640;
    g_xboxH  = xboxHeight ? xboxHeight : 480;
    uint32_t scale = std::max<uint32_t>(1, std::min<uint32_t>(4, g_reqScale.load() ? g_reqScale.load() : resScale));
    uint32_t asp = g_reqAspect.load();
    g_curScale  = scale;
    g_curAspect = asp;
    g_reqScale.store(scale);
    uint32_t ow = 0, oh = 0;
    ComputeOutputResolution(scale, asp, ow, oh);
    UpdateScaleMetrics(ow, oh, asp);

    D3DMULTISAMPLE_TYPE msaaType = ProbeMsaaType(g_reqMsaa.load());
    DWORD msaaQuality = 0;

    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed               = TRUE;
    pp.SwapEffect             = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow          = hRenderWnd;
    pp.BackBufferFormat       = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth        = g_outW;
    pp.BackBufferHeight       = g_outH;
    pp.BackBufferCount        = 1;
    pp.MultiSampleType        = msaaType;
    pp.MultiSampleQuality     = msaaQuality;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.PresentationInterval   = D3DPRESENT_INTERVAL_IMMEDIATE;

    HRESULT hr = g_d3d9->CreateDevice(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hRenderWnd,
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
        &pp, &g_dev
    );
    if (FAILED(hr)) {
        pp.MultiSampleType = D3DMULTISAMPLE_NONE;
        pp.MultiSampleQuality = 0;
        msaaType = D3DMULTISAMPLE_NONE;
        hr = g_d3d9->CreateDevice(
            D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hRenderWnd,
            D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
            &pp, &g_dev
        );
    }
    if (FAILED(hr)) {
        hr = g_d3d9->CreateDevice(
            D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hRenderWnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
            &pp, &g_dev
        );
    }
    if (FAILED(hr) || !g_dev) {
        LogPrintf("[D3D9-ERROR] IDirect3D9::CreateDevice failed (hr=0x%08lX)\n", static_cast<unsigned long>(hr));
        g_d3d9->Release();
        g_d3d9 = nullptr;
        return false;
    }

    g_presentParams = pp;
    g_curMsaa = static_cast<uint32_t>(msaaType);
    g_reqMsaa.store(g_curMsaa);

    D3DCAPS9 caps{};
    if (SUCCEEDED(g_dev->GetDeviceCaps(&caps))) {
        g_hwMaxAniso = std::max<DWORD>(1, std::min<DWORD>(16, caps.MaxAnisotropy));
    }
    g_maxAniso = std::min<DWORD>(g_hwMaxAniso, g_reqAniso.load());

    ApplyInitialFixedState(msaaType);

    LogPrintf("[D3D9] Hardware Direct3D 9 HD Renderer initialized: %ux%u (%ux Native Scale), %ux MSAA, %lux AF!\n",
              g_outW, g_outH, scale, static_cast<unsigned>(msaaType), static_cast<unsigned long>(g_maxAniso));
    return true;
}

// ============================================================================
// Live GPU Post-Process: Soft Bloom + CRT Gamma Lift, Brightness & Contrast
// (Works identically in Windowed, Borderless & Fullscreen modes!)
// ============================================================================
static std::atomic<bool>      g_bloomEnabled{false};
static std::atomic<int>       g_gammaPercent{100};
static std::atomic<int>       g_brightnessPercent{100};
static std::atomic<int>       g_contrastPercent{100};
static IDirect3DTexture9*     g_postTex   = nullptr;
static IDirect3DTexture9*     g_bloomTexA = nullptr;
static IDirect3DTexture9*     g_bloomTexB = nullptr;
static uint32_t               g_bloomW    = 0;
static uint32_t               g_bloomH    = 0;
static IDirect3DPixelShader9* g_postPS        = nullptr;
static IDirect3DPixelShader9* g_bloomBrightPS = nullptr;
static IDirect3DPixelShader9* g_bloomBlurPS   = nullptr;
static bool                   g_postTriedCompile = false;

void SetBloomEnabled(bool enabled) { g_bloomEnabled.store(enabled); }
bool IsBloomEnabled() { return g_bloomEnabled.load(); }

void SetColorCorrection(int gammaPercent, int brightnessPercent, int contrastPercent) {
    g_gammaPercent.store(std::max(50, std::min(200, gammaPercent)));
    g_brightnessPercent.store(std::max(50, std::min(150, brightnessPercent)));
    g_contrastPercent.store(std::max(50, std::min(150, contrastPercent)));
}

static void ReleasePostResources(bool releaseShader) {
    if (g_postTex)   { g_postTex->Release();   g_postTex   = nullptr; }
    if (g_bloomTexA) { g_bloomTexA->Release(); g_bloomTexA = nullptr; }
    if (g_bloomTexB) { g_bloomTexB->Release(); g_bloomTexB = nullptr; }
    g_bloomW = g_bloomH = 0;
    if (releaseShader) {
        if (g_postPS)        { g_postPS->Release();        g_postPS        = nullptr; }
        if (g_bloomBrightPS) { g_bloomBrightPS->Release(); g_bloomBrightPS = nullptr; }
        if (g_bloomBlurPS)   { g_bloomBlurPS->Release();   g_bloomBlurPS   = nullptr; }
        g_postTriedCompile = false;
    }
}

static bool EnsurePostResources(bool needBloom) {
    if (!g_dev) return false;
    if (!g_postPS && !g_postTriedCompile) {
        g_postTriedCompile = true;
        HMODULE hComp = LoadLibraryA("d3dcompiler_47.dll");
        if (!hComp) hComp = LoadLibraryA("d3dcompiler_43.dll");
        if (hComp) {
            struct D3DBlobIface {
                virtual HRESULT __stdcall QueryInterface(REFIID, void**) = 0;
                virtual ULONG   __stdcall AddRef() = 0;
                virtual ULONG   __stdcall Release() = 0;
                virtual LPVOID  __stdcall GetBufferPointer() = 0;
                virtual SIZE_T  __stdcall GetBufferSize() = 0;
            };
            using D3DCompileFn = HRESULT(__stdcall*)(
                LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR,
                UINT, UINT, D3DBlobIface**, D3DBlobIface**);
            auto fnCompile = reinterpret_cast<D3DCompileFn>(GetProcAddress(hComp, "D3DCompile"));
            if (fnCompile) {
                auto compilePS = [&](const char* src, const char* name, IDirect3DPixelShader9** outPS) {
                    D3DBlobIface* code = nullptr;
                    D3DBlobIface* err  = nullptr;
                    if (SUCCEEDED(fnCompile(src, std::strlen(src), name,
                                            nullptr, nullptr, "main", "ps_2_0", 0, 0, &code, &err)) && code) {
                        g_dev->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), outPS);
                        code->Release();
                    }
                    if (err) err->Release();
                };
                static const char* kPostHlsl =
                    "sampler2D s0 : register(s0);\n"
                    "sampler2D s1 : register(s1);\n"
                    "float4 cParams : register(c0);\n"
                    "float4 main(float2 uv : TEXCOORD0) : COLOR0 {\n"
                    "    float3 rgb = tex2D(s0, uv).rgb;\n"
                    "    if (cParams.w > 0.001f) {\n"
                    "        rgb += tex2D(s1, uv).rgb * cParams.w;\n"
                    "    }\n"
                    "    rgb = pow(max(rgb, 0.0001f), cParams.x);\n"
                    "    rgb = (rgb - 0.5f) * cParams.z + 0.5f + cParams.y;\n"
                    "    return float4(saturate(rgb), 1.0f);\n"
                    "}\n";
                static const char* kBrightHlsl =
                    "sampler2D s0 : register(s0);\n"
                    "float4 main(float2 uv : TEXCOORD0) : COLOR0 {\n"
                    "    float3 c = tex2D(s0, uv).rgb;\n"
                    "    float l = dot(c, float3(0.299f, 0.587f, 0.114f));\n"
                    "    float w = saturate((l - 0.68f) / 0.32f);\n"
                    "    return float4(c * (w * w), 1.0f);\n"
                    "}\n";
                static const char* kBlurHlsl =
                    "sampler2D s0 : register(s0);\n"
                    "float4 cStep : register(c0);\n"
                    "float4 main(float2 uv : TEXCOORD0) : COLOR0 {\n"
                    "    float3 acc = tex2D(s0, uv).rgb * 0.227027f;\n"
                    "    acc += (tex2D(s0, uv + cStep.xy * 1.384615f).rgb + tex2D(s0, uv - cStep.xy * 1.384615f).rgb) * 0.316216f;\n"
                    "    acc += (tex2D(s0, uv + cStep.xy * 3.230769f).rgb + tex2D(s0, uv - cStep.xy * 3.230769f).rgb) * 0.070270f;\n"
                    "    return float4(acc, 1.0f);\n"
                    "}\n";
                compilePS(kPostHlsl,   "buffy_post.hlsl",   &g_postPS);
                compilePS(kBrightHlsl, "buffy_bright.hlsl", &g_bloomBrightPS);
                compilePS(kBlurHlsl,   "buffy_blur.hlsl",   &g_bloomBlurPS);
            }
        }
    }
    if (!g_postPS) return false;
    if (!g_postTex && g_outW > 0 && g_outH > 0) {
        g_dev->CreateTexture(g_outW, g_outH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &g_postTex, nullptr);
    }
    if (needBloom && g_bloomBrightPS && g_bloomBlurPS && (!g_bloomTexA || !g_bloomTexB) && g_outW > 0 && g_outH > 0) {
        g_bloomW = std::max<uint32_t>(64u, g_outW / 4u);
        g_bloomH = std::max<uint32_t>(64u, g_outH / 4u);
        if (!g_bloomTexA) g_dev->CreateTexture(g_bloomW, g_bloomH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &g_bloomTexA, nullptr);
        if (!g_bloomTexB) g_dev->CreateTexture(g_bloomW, g_bloomH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &g_bloomTexB, nullptr);
    }
    return g_postTex != nullptr;
}

static void DrawFullScreenPostQuad(uint32_t w, uint32_t h) {
    struct PostVert { float x, y, z, rhw, u, v; };
    float fw = static_cast<float>(w);
    float fh = static_cast<float>(h);
    PostVert quad[4] = {
        { -0.5f,     -0.5f,     0.0f, 1.0f, 0.0f, 0.0f },
        { fw - 0.5f, -0.5f,     0.0f, 1.0f, 1.0f, 0.0f },
        { -0.5f,     fh - 0.5f, 0.0f, 1.0f, 0.0f, 1.0f },
        { fw - 0.5f, fh - 0.5f, 0.0f, 1.0f, 1.0f, 1.0f }
    };
    g_dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    g_dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(PostVert));
}

static void ApplyColorCorrectionPostPass() {
    bool bloomOn = g_bloomEnabled.load();
    int gam = g_gammaPercent.load();
    int bri = g_brightnessPercent.load();
    int con = g_contrastPercent.load();
    if (!bloomOn && gam == 100 && bri == 100 && con == 100) return;
    if (!EnsurePostResources(bloomOn)) return;

    IDirect3DSurface9* bb = nullptr;
    IDirect3DSurface9* dstSurf = nullptr;
    if (FAILED(g_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    if (FAILED(g_postTex->GetSurfaceLevel(0, &dstSurf)) || !dstSurf) {
        bb->Release();
        return;
    }

    // Resolve MSAA backbuffer into g_postTex
    HRESULT hr = g_dev->StretchRect(bb, nullptr, dstSurf, nullptr, D3DTEXF_NONE);
    dstSurf->Release();
    if (FAILED(hr)) { bb->Release(); return; }

    D3DVIEWPORT9 prevVp{};
    bool hadVp = SUCCEEDED(g_dev->GetViewport(&prevVp));

    g_dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    g_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

    // Optional quarter-res Soft Bloom (bright-pass + separable 5-tap Gaussian H/V blur)
    bool bloomReady = false;
    if (bloomOn && g_bloomTexA && g_bloomTexB && g_bloomBrightPS && g_bloomBlurPS) {
        IDirect3DSurface9* sA = nullptr;
        IDirect3DSurface9* sB = nullptr;
        if (SUCCEEDED(g_bloomTexA->GetSurfaceLevel(0, &sA)) && SUCCEEDED(g_bloomTexB->GetSurfaceLevel(0, &sB))) {
            D3DVIEWPORT9 bVp{0, 0, g_bloomW, g_bloomH, 0.0f, 1.0f};
            g_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            g_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            g_dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            g_dev->SetSamplerState(0, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP);
            g_dev->SetSamplerState(0, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP);

            // Pass 1: Bright extract -> g_bloomTexA
            g_dev->SetRenderTarget(0, sA);
            g_dev->SetViewport(&bVp);
            g_dev->SetPixelShader(g_bloomBrightPS);
            g_dev->SetTexture(0, g_postTex);
            DrawFullScreenPostQuad(g_bloomW, g_bloomH);

            // Pass 2: Horizontal Gaussian blur -> g_bloomTexB
            float stepH[4] = { 1.0f / static_cast<float>(g_bloomW), 0.0f, 0.0f, 0.0f };
            g_dev->SetRenderTarget(0, sB);
            g_dev->SetViewport(&bVp);
            g_dev->SetPixelShader(g_bloomBlurPS);
            g_dev->SetPixelShaderConstantF(0, stepH, 1);
            g_dev->SetTexture(0, g_bloomTexA);
            DrawFullScreenPostQuad(g_bloomW, g_bloomH);

            // Pass 3: Vertical Gaussian blur -> g_bloomTexA
            float stepV[4] = { 0.0f, 1.0f / static_cast<float>(g_bloomH), 0.0f, 0.0f };
            g_dev->SetRenderTarget(0, sA);
            g_dev->SetViewport(&bVp);
            g_dev->SetPixelShaderConstantF(0, stepV, 1);
            g_dev->SetTexture(0, g_bloomTexB);
            DrawFullScreenPostQuad(g_bloomW, g_bloomH);

            // Restore backbuffer render target
            g_dev->SetRenderTarget(0, bb);
            bloomReady = true;
        }
        if (sA) sA->Release();
        if (sB) sB->Release();
    }
    bb->Release();

    // Final composite pass: Scene + Subtle Bloom + CRT Gamma / Brightness / Contrast
    D3DVIEWPORT9 fullVp{0, 0, g_outW, g_outH, 0.0f, 1.0f};
    g_dev->SetViewport(&fullVp);

    float gammaVal = std::max(0.5f, static_cast<float>(gam) / 100.0f);
    float params[4] = {
        1.0f / gammaVal,                                // c0.x = inverse gamma (>100% lifts dark shadows)
        static_cast<float>(bri - 100) * 0.005f,         // c0.y = brightness offset (-0.25 .. +0.25)
        static_cast<float>(con) / 100.0f,               // c0.z = contrast scale (0.50 .. 1.50)
        bloomReady ? 0.34f : 0.0f                       // c0.w = subtle soft bloom strength
    };
    g_dev->SetPixelShader(g_postPS);
    g_dev->SetPixelShaderConstantF(0, params, 1);
    g_dev->SetTexture(0, g_postTex);
    g_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    g_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    g_dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    g_dev->SetSamplerState(0, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP);
    g_dev->SetSamplerState(0, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP);
    if (bloomReady) {
        g_dev->SetTexture(1, g_bloomTexA);
        g_dev->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        g_dev->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        g_dev->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        g_dev->SetSamplerState(1, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP);
        g_dev->SetSamplerState(1, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP);
    }
    DrawFullScreenPostQuad(g_outW, g_outH);

    g_dev->SetPixelShader(nullptr);
    g_dev->SetTexture(0, nullptr);
    g_dev->SetTexture(1, nullptr);
    if (!g_customVS && g_fvf) g_dev->SetFVF(g_fvf);
    if (hadVp) g_dev->SetViewport(&prevVp);
}

void Shutdown() {
    ReleasePostResources(true);
    for (auto& kv : g_texCache) {
        if (kv.second.tex) kv.second.tex->Release();
    }
    g_texCache.clear();
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
    if (g_d3d9) { g_d3d9->Release(); g_d3d9 = nullptr; }
    if (g_gdiplusToken) {
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }
    g_inScene = false;
}

bool IsReady() {
    return g_dev != nullptr;
}

// ============================================================================
// Morton Swizzle -> Linear Unswizzle Helper (Original Xbox NV2A)
// ============================================================================
static void Unswizzle2D(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t height, uint32_t dstPitch, uint32_t bpp) {
    uint32_t maskU = 0, maskV = 0;
    uint32_t i = 1, j = 1;
    uint32_t w = width, h = height;
    while (w > 1 || h > 1) {
        if (w > 1) { maskU |= j; j <<= 1; w >>= 1; }
        if (h > 1) { maskV |= j; j <<= 1; h >>= 1; }
        ++i;
    }
    (void)i;

    uint32_t v = 0;
    for (uint32_t y = 0; y < height; ++y) {
        uint32_t u = 0;
        uint8_t* dstRow = dst + y * dstPitch;
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t swizIdx = u | v;
            std::memcpy(dstRow + x * bpp, src + swizIdx * bpp, bpp);
            u = (u - maskU) & maskU;
        }
        v = (v - maskV) & maskV;
    }
}

static void Unswizzle3D(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t height, uint32_t depth,
                        uint32_t rowPitch, uint32_t slicePitch, uint32_t bpp) {
    uint32_t maskU = 0, maskV = 0, maskW = 0;
    uint32_t j = 1;
    uint32_t w = width, h = height, d = depth;
    while (w > 1 || h > 1 || d > 1) {
        if (w > 1) { maskU |= j; j <<= 1; w >>= 1; }
        if (h > 1) { maskV |= j; j <<= 1; h >>= 1; }
        if (d > 1) { maskW |= j; j <<= 1; d >>= 1; }
    }

    uint32_t sw = 0;
    for (uint32_t z = 0; z < depth; ++z) {
        uint32_t sv = 0;
        uint8_t* dstSlice = dst + z * slicePitch;
        for (uint32_t y = 0; y < height; ++y) {
            uint32_t su = 0;
            uint8_t* dstRow = dstSlice + y * rowPitch;
            for (uint32_t x = 0; x < width; ++x) {
                uint32_t swizIdx = su | sv | sw;
                std::memcpy(dstRow + x * bpp, src + swizIdx * bpp, bpp);
                su = (su - maskU) & maskU;
            }
            sv = (sv - maskV) & maskV;
        }
        sw = (sw - maskW) & maskW;
    }
}

static IDirect3DBaseTexture9* GetOrUploadTexture(uint32_t stage, const XboxD3DBaseTexture* xtex) {
    if (!g_dev || !xtex || !xtex->Data) return nullptr;

    const uint32_t* curPal = (stage < 4) ? g_palettes[stage] : nullptr;
    auto it = g_texCache.find(xtex);
    if (it != g_texCache.end() &&
        it->second.data == xtex->Data &&
        it->second.format == xtex->Format &&
        it->second.size == xtex->Size &&
        (((xtex->Format >> 8) & 0xFFu) != 0x0Bu || it->second.palette == curPal)) {
        return it->second.tex;
    }

    if (it != g_texCache.end() && it->second.tex) {
        it->second.tex->Release();
        g_texCache.erase(it);
    }

    uint32_t fmtByte = (xtex->Format >> 8) & 0xFFu;
    uint32_t dim     = (xtex->Format >> 4) & 0xFu;
    uint32_t width = 0, height = 0, depth = 1, srcPitch = 0;
    if (xtex->Size != 0) {
        width    = (xtex->Size & 0xFFFu) + 1;
        height   = ((xtex->Size >> 12) & 0xFFFu) + 1;
        srcPitch = (((xtex->Size >> 24) & 0xFFu) + 1) * 64;
    } else {
        uint32_t logW = (xtex->Format >> 20) & 0xFu;
        uint32_t logH = (xtex->Format >> 24) & 0xFu;
        uint32_t logD = (xtex->Format >> 28) & 0xFu;
        width  = 1u << logW;
        height = 1u << logH;
        if (dim == 3) depth = 1u << logD;
    }
    if (width == 0 || height == 0 || width > 2048 || height > 2048) return nullptr;

    // Handle Xbox 3D Volume Textures (e.g. 32x32x32 spherical point-light / torch falloff maps!)
    if (dim == 3 && depth > 1 && depth <= 256) {
        uint32_t vbpp = 1;
        bool vIsA8 = false, vIsL8 = false, vIsA8L8 = false, vIs565 = false, vIs4444 = false;
        switch (fmtByte) {
            case 0x00: case 0x13: vbpp = 1; vIsL8 = true; break;
            case 0x01: case 0x19: case 0x1F: vbpp = 1; vIsA8 = true; break;
            case 0x1A: vbpp = 2; vIsA8L8 = true; break;
            case 0x05: case 0x11: vbpp = 2; vIs565 = true; break;
            case 0x04: case 0x1D: vbpp = 2; vIs4444 = true; break;
            case 0x06: case 0x07: case 0x12: case 0x1E: vbpp = 4; break;
            default: vbpp = 1; vIsL8 = true; break;
        }
        uint32_t volBytes = width * height * depth * vbpp;
        if (!IsBadReadPtr(reinterpret_cast<const void*>(xtex->Data), volBytes)) {
            IDirect3DVolumeTexture9* vtex = nullptr;
            if (SUCCEEDED(g_dev->CreateVolumeTexture(width, height, depth, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &vtex, nullptr)) && vtex) {
                std::vector<uint8_t> lin(volBytes);
                Unswizzle3D(reinterpret_cast<const uint8_t*>(xtex->Data), lin.data(),
                            width, height, depth, width * vbpp, width * height * vbpp, vbpp);
                D3DLOCKED_BOX lb{};
                if (SUCCEEDED(vtex->LockBox(0, &lb, nullptr, 0))) {
                    uint8_t* dstBase = static_cast<uint8_t*>(lb.pBits);
                    for (uint32_t z = 0; z < depth; ++z) {
                        for (uint32_t y = 0; y < height; ++y) {
                            uint32_t* outRow = reinterpret_cast<uint32_t*>(dstBase + z * lb.SlicePitch + y * lb.RowPitch);
                            const uint8_t* inRow = lin.data() + (z * height + y) * width * vbpp;
                            for (uint32_t x = 0; x < width; ++x) {
                                if (vIsA8) {
                                    uint8_t v = inRow[x];
                                    outRow[x] = (static_cast<uint32_t>(v) << 24) | (static_cast<uint32_t>(v) << 16) |
                                                (static_cast<uint32_t>(v) << 8) | v;
                                } else if (vIsL8) {
                                    uint8_t v = inRow[x];
                                    outRow[x] = (static_cast<uint32_t>(v) << 24) | (static_cast<uint32_t>(v) << 16) |
                                                (static_cast<uint32_t>(v) << 8) | v;
                                } else if (vIsA8L8) {
                                    uint8_t lo = inRow[x * 2 + 0], hi = inRow[x * 2 + 1];
                                    outRow[x] = (static_cast<uint32_t>(hi) << 24) | (static_cast<uint32_t>(lo) << 16) |
                                                (static_cast<uint32_t>(lo) << 8) | lo;
                                } else if (vIs565) {
                                    uint16_t c = reinterpret_cast<const uint16_t*>(inRow)[x];
                                    uint32_t r = ((c >> 11) & 31) * 255 / 31;
                                    uint32_t g = ((c >>  5) & 63) * 255 / 63;
                                    uint32_t b = ( c        & 31) * 255 / 31;
                                    outRow[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
                                } else if (vIs4444) {
                                    uint16_t c = reinterpret_cast<const uint16_t*>(inRow)[x];
                                    uint32_t a = ((c >> 12) & 15) * 17;
                                    uint32_t r = ((c >>  8) & 15) * 17;
                                    uint32_t g = ((c >>  4) & 15) * 17;
                                    uint32_t b = ( c        & 15) * 17;
                                    outRow[x] = (a << 24) | (r << 16) | (g << 8) | b;
                                } else {
                                    outRow[x] = reinterpret_cast<const uint32_t*>(inRow)[x];
                                }
                            }
                        }
                    }
                    vtex->UnlockBox(0);
                }
                CachedTexture ct{vtex, xtex->Data, xtex->Format, xtex->Size, width, height, false, curPal, 0, true};
                g_texCache[xtex] = ct;
                LogPrintf("[D3D9] Uploaded 3D Volume Texture (%ux%ux%u, fmt=0x%02X) for dynamic point/torch lighting!\n",
                          width, height, depth, fmtByte);
                return vtex;
            }
        }
    }

    D3DFORMAT d3dFmt = D3DFMT_A8R8G8B8;
    uint32_t  bpp    = 4;
    bool      swiz   = false;
    bool      isDxt  = false;
    bool      isL8   = false;
    bool      isA8   = false;
    bool      isP8   = false;
    bool      isA8L8 = false;
    bool      isV8U8 = false;

    switch (fmtByte) {
        case 0x0C: d3dFmt = D3DFMT_DXT1;     isDxt = true; bpp = 8;  break;
        case 0x0E: d3dFmt = D3DFMT_DXT3;     isDxt = true; bpp = 16; break;
        case 0x0F: d3dFmt = D3DFMT_DXT5;     isDxt = true; bpp = 16; break;
        case 0x06: d3dFmt = D3DFMT_A8R8G8B8; swiz = true;  bpp = 4;  break;
        case 0x07: d3dFmt = D3DFMT_X8R8G8B8; swiz = true;  bpp = 4;  break;
        case 0x12: d3dFmt = D3DFMT_A8R8G8B8; swiz = false; bpp = 4;  break;
        case 0x1E: d3dFmt = D3DFMT_X8R8G8B8; swiz = false; bpp = 4;  break;
        case 0x05: d3dFmt = D3DFMT_R5G6B5;   swiz = true;  bpp = 2;  break;
        case 0x11: d3dFmt = D3DFMT_R5G6B5;   swiz = false; bpp = 2;  break;
        case 0x04: d3dFmt = D3DFMT_A4R4G4B4; swiz = true;  bpp = 2;  break;
        case 0x1D: d3dFmt = D3DFMT_A4R4G4B4; swiz = false; bpp = 2;  break;
        case 0x02: d3dFmt = D3DFMT_A1R5G5B5; swiz = true;  bpp = 2;  break;
        case 0x03: d3dFmt = D3DFMT_X1R5G5B5; swiz = true;  bpp = 2;  break;
        case 0x10: d3dFmt = D3DFMT_A1R5G5B5; swiz = false; bpp = 2;  break;
        case 0x1C: d3dFmt = D3DFMT_X1R5G5B5; swiz = false; bpp = 2;  break;
        case 0x00: d3dFmt = D3DFMT_A8R8G8B8; swiz = true;  bpp = 1; isL8 = true; break;
        case 0x13: d3dFmt = D3DFMT_A8R8G8B8; swiz = false; bpp = 1; isL8 = true; break;
        case 0x01:
        case 0x19: d3dFmt = D3DFMT_A8R8G8B8; swiz = true;  bpp = 1; isA8 = true; break;
        case 0x1F: d3dFmt = D3DFMT_A8R8G8B8; swiz = false; bpp = 1; isA8 = true; break;
        case 0x0B: d3dFmt = D3DFMT_A8R8G8B8; swiz = true;  bpp = 1; isP8 = true; break;
        case 0x1A: d3dFmt = D3DFMT_A8R8G8B8; swiz = true;  bpp = 2; isA8L8 = true; break;
        case 0x28: d3dFmt = D3DFMT_V8U8;     swiz = true;  bpp = 2; break; // Native hardware V8U8 bump map!
        case 0x27: d3dFmt = D3DFMT_V8U8;     swiz = true;  bpp = 2; isV8U8 = true; break; // L6V5U5 -> V8U8 bump map
        case 0x3A: d3dFmt = D3DFMT_A8R8G8B8; swiz = true;  bpp = 4; break;
        default: {
            static uint32_t s_loggedMask[8]{};
            if (!(s_loggedMask[fmtByte >> 5] & (1u << (fmtByte & 31)))) {
                s_loggedMask[fmtByte >> 5] |= (1u << (fmtByte & 31));
                LogPrintf("[D3D9] Unhandled Xbox texture format 0x%02X (%ux%u)\n", fmtByte, width, height);
            }
            return nullptr;
        }
    }

    // Read full mipmap count from Xbox D3DBaseTexture.Format (bits 16..19)
    uint32_t mipLevels = (xtex->Size != 0) ? 1u : ((xtex->Format >> 16) & 0xFu);
    if (mipLevels == 0) mipLevels = 1;
    // Clamp mipLevels so we never exceed valid dimensions
    uint32_t maxPossibleMips = 1;
    for (uint32_t tw = width, th = height; tw > 1 || th > 1; tw = std::max<uint32_t>(1, tw >> 1), th = std::max<uint32_t>(1, th >> 1)) {
        ++maxPossibleMips;
    }
    mipLevels = std::min(mipLevels, maxPossibleMips);

    // Compute total bytes across all mip levels to verify readability
    uint32_t totalBytes = 0;
    for (uint32_t m = 0; m < mipLevels; ++m) {
        uint32_t mw = std::max<uint32_t>(1u, width >> m);
        uint32_t mh = std::max<uint32_t>(1u, height >> m);
        if (isDxt) {
            totalBytes += std::max<uint32_t>(1u, (mw + 3) / 4) * std::max<uint32_t>(1u, (mh + 3) / 4) * bpp;
        } else if (m == 0 && srcPitch) {
            totalBytes += srcPitch * mh;
        } else {
            totalBytes += mw * mh * bpp;
        }
    }
    uint32_t lvl0Bytes = isDxt
        ? (std::max<uint32_t>(1u, (width + 3) / 4) * std::max<uint32_t>(1u, (height + 3) / 4) * bpp)
        : (srcPitch ? (srcPitch * height) : (width * height * bpp));

    if (IsBadReadPtr(reinterpret_cast<const void*>(xtex->Data), totalBytes)) {
        // Fallback to single mip level if full chain isn't committed
        mipLevels = 1;
        if (IsBadReadPtr(reinterpret_cast<const void*>(xtex->Data), lvl0Bytes)) {
            return nullptr;
        }
    }

    // Only hash if custom HD textures exist (and are enabled), dumping, or tracing
    bool wantHD   = g_hdTexturesEnabled.load() && !g_hdTextureFiles.empty();
    bool wantDump = g_dumpTextures.load();
    uint64_t texHash = 0;
    if (wantHD || wantDump || g_tracingThisFrame) {
        texHash = HashXboxTexture(width, height, fmtByte, reinterpret_cast<const uint8_t*>(xtex->Data), lvl0Bytes);
        if (wantHD) {
            auto hdIt = g_hdTextureFiles.find(texHash);
            if (hdIt != g_hdTextureFiles.end()) {
                uint32_t rw = 0, rh = 0;
                std::vector<uint32_t> argb;
                if (LoadImageFileToARGB(hdIt->second, rw, rh, argb)) {
                    IDirect3DTexture9* hdTex = UploadARGBTextureWithMips(rw, rh, argb);
                    if (hdTex) {
                        LogPrintf("[HDTex] Replaced %016llx (%ux%u -> %ux%u HD)\n",
                                  static_cast<unsigned long long>(texHash), width, height, rw, rh);
                        CachedTexture ct{hdTex, xtex->Data, xtex->Format, xtex->Size, width, height, (xtex->Size != 0) && !isDxt};
                        g_texCache[xtex] = ct;
                        return hdTex;
                    }
                }
            }
        }
    }

    IDirect3DTexture9* tex = nullptr;
    if (FAILED(g_dev->CreateTexture(width, height, mipLevels, 0, d3dFmt, D3DPOOL_MANAGED, &tex, nullptr)) || !tex) {
        return nullptr;
    }

    const uint8_t* srcCursor = reinterpret_cast<const uint8_t*>(xtex->Data);
    for (uint32_t m = 0; m < mipLevels; ++m) {
        uint32_t mw = std::max<uint32_t>(1u, width >> m);
        uint32_t mh = std::max<uint32_t>(1u, height >> m);
        uint32_t levelBytes = 0;

        D3DLOCKED_RECT lr{};
        if (SUCCEEDED(tex->LockRect(m, &lr, nullptr, 0))) {
            uint8_t* dst = static_cast<uint8_t*>(lr.pBits);

            if (isDxt) {
                uint32_t bw = std::max<uint32_t>(1u, (mw + 3) / 4);
                uint32_t bh = std::max<uint32_t>(1u, (mh + 3) / 4);
                uint32_t rowBytes = bw * bpp;
                for (uint32_t y = 0; y < bh; ++y) {
                    std::memcpy(dst + y * lr.Pitch, srcCursor + y * rowBytes, rowBytes);
                }
                levelBytes = bh * rowBytes;
            } else if (isL8 || isA8 || isP8) {
                std::vector<uint8_t> lin(mw * mh);
                if (swiz) {
                    Unswizzle2D(srcCursor, lin.data(), mw, mh, mw, 1);
                    levelBytes = mw * mh;
                } else {
                    uint32_t sp = (m == 0 && srcPitch) ? srcPitch : mw;
                    for (uint32_t y = 0; y < mh; ++y) {
                        std::memcpy(lin.data() + y * mw, srcCursor + y * sp, mw);
                    }
                    levelBytes = sp * mh;
                }
                const uint32_t* pal = (isP8 && stage < 4) ? g_palettes[stage] : nullptr;
                for (uint32_t y = 0; y < mh; ++y) {
                    uint32_t* outRow = reinterpret_cast<uint32_t*>(dst + y * lr.Pitch);
                    for (uint32_t x = 0; x < mw; ++x) {
                        uint8_t v = lin[y * mw + x];
                        if (isP8 && pal) {
                            outRow[x] = pal[v];
                        } else if (isA8) {
                            outRow[x] = (static_cast<uint32_t>(v) << 24) | 0x00FFFFFFu;
                        } else {
                            outRow[x] = 0xFF000000u | (static_cast<uint32_t>(v) << 16) | (static_cast<uint32_t>(v) << 8) | v;
                        }
                    }
                }
            } else if (isA8L8) {
                std::vector<uint16_t> lin(mw * mh);
                Unswizzle2D(srcCursor, reinterpret_cast<uint8_t*>(lin.data()), mw, mh, mw * 2, 2);
                levelBytes = mw * mh * 2;
                for (uint32_t y = 0; y < mh; ++y) {
                    uint32_t* outRow = reinterpret_cast<uint32_t*>(dst + y * lr.Pitch);
                    for (uint32_t x = 0; x < mw; ++x) {
                        uint16_t p = lin[y * mw + x];
                        uint8_t lo = static_cast<uint8_t>(p & 0xFFu);
                        uint8_t hi = static_cast<uint8_t>(p >> 8);
                        outRow[x] = (static_cast<uint32_t>(hi) << 24) | (static_cast<uint32_t>(lo) << 16) |
                                    (static_cast<uint32_t>(lo) << 8) | lo;
                    }
                }
            } else if (isV8U8) {
                // Convert Xbox L6V5U5 (0x27) into native signed V8U8 bump map
                std::vector<uint16_t> lin(mw * mh);
                Unswizzle2D(srcCursor, reinterpret_cast<uint8_t*>(lin.data()), mw, mh, mw * 2, 2);
                levelBytes = mw * mh * 2;
                for (uint32_t y = 0; y < mh; ++y) {
                    uint8_t* outRow = dst + y * lr.Pitch;
                    for (uint32_t x = 0; x < mw; ++x) {
                        uint16_t p = lin[y * mw + x];
                        int8_t u = static_cast<int8_t>((p & 0x1Fu) << 3);
                        int8_t v = static_cast<int8_t>(((p >> 5) & 0x1Fu) << 3);
                        outRow[x * 2 + 0] = static_cast<uint8_t>(u);
                        outRow[x * 2 + 1] = static_cast<uint8_t>(v);
                    }
                }
            } else if (swiz) {
                Unswizzle2D(srcCursor, dst, mw, mh, static_cast<uint32_t>(lr.Pitch), bpp);
                levelBytes = mw * mh * bpp;
            } else {
                uint32_t sp = (m == 0 && srcPitch) ? srcPitch : (mw * bpp);
                uint32_t copyBytes = mw * bpp;
                for (uint32_t y = 0; y < mh; ++y) {
                    std::memcpy(dst + y * lr.Pitch, srcCursor + y * sp, copyBytes);
                }
                levelBytes = sp * mh;
            }
            tex->UnlockRect(m);
        }
        srcCursor += levelBytes;
    }

    if (wantDump) {
        DumpXboxTextureLevel0(texHash, width, height, fmtByte, d3dFmt, isDxt, tex,
                              reinterpret_cast<const uint8_t*>(xtex->Data));
    }

    CachedTexture ct{tex, xtex->Data, xtex->Format, xtex->Size, width, height, (xtex->Size != 0) && !isDxt, curPal, texHash};
    g_texCache[xtex] = ct;
    return tex;
}

// ============================================================================
// Bridged D3D8 -> D3D9 Device Calls
// ============================================================================
void Clear(uint32_t count, const void* pRects, uint32_t flags, uint32_t color, float z, uint32_t stencil) {
    if (!g_dev) return;
    EnsureSceneBegun();
    DWORD d3d9Flags = 0;
    if (flags & 0x000000F0u) d3d9Flags |= D3DCLEAR_TARGET;
    if (flags & 0x00000001u) d3d9Flags |= D3DCLEAR_ZBUFFER;
    if (flags & 0x00000002u) d3d9Flags |= D3DCLEAR_STENCIL;
    if (d3d9Flags == 0) d3d9Flags = D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER;
    if (count > 0 && pRects && (g_scaleX != 1.0f || g_scaleY != 1.0f)) {
        const D3DRECT* src = reinterpret_cast<const D3DRECT*>(pRects);
        std::vector<D3DRECT> scaled(count);
        for (uint32_t i = 0; i < count; ++i) {
            scaled[i].x1 = static_cast<LONG>(src[i].x1 * g_scaleX);
            scaled[i].y1 = static_cast<LONG>(src[i].y1 * g_scaleY);
            scaled[i].x2 = static_cast<LONG>(src[i].x2 * g_scaleX);
            scaled[i].y2 = static_cast<LONG>(src[i].y2 * g_scaleY);
        }
        g_dev->Clear(count, scaled.data(), d3d9Flags, color, z, stencil);
        return;
    }
    if (count == 0 && g_menuPillarbox && g_curAspect != 0) {
        // Clear full backbuffer (including left/right 4:3 pillarboxes) to clean black
        D3DVIEWPORT9 prevVp{};
        bool hadVp = SUCCEEDED(g_dev->GetViewport(&prevVp));
        D3DVIEWPORT9 fullVp{0, 0, g_outW, g_outH, 0.0f, 1.0f};
        g_dev->SetViewport(&fullVp);
        g_dev->Clear(0, nullptr, d3d9Flags, color, z, stencil);
        if (hadVp) g_dev->SetViewport(&prevVp);
        return;
    }
    g_dev->Clear(count, reinterpret_cast<const D3DRECT*>(pRects), d3d9Flags, color, z, stencil);
}

void Swap(uint32_t /*flags*/) {
    if (!g_dev) return;
    if (g_tracingThisFrame) {
        if (g_traceFile) {
            std::fprintf(g_traceFile, "[D3D9-TRACE] End of traced frame (%u draw calls)\n", g_traceDrawIdx);
            std::fclose(g_traceFile);
            g_traceFile = nullptr;
        }
        LogPrintf("[D3D9-TRACE] Saved %u draw calls to recomp\\frame_trace.log\n", g_traceDrawIdx);
        g_tracingThisFrame = false;
    }
    if (g_inScene) {
        ApplyColorCorrectionPostPass();
        g_dev->EndScene();
        g_inScene = false;
    }
    g_dev->Present(nullptr, nullptr, nullptr, nullptr);
    if (g_traceRequested.exchange(false)) {
        g_tracingThisFrame = true;
        g_traceDrawIdx = 0;
        std::string tracePath = g_hdTexDir.empty() ? "frame_trace.log" : (g_hdTexDir + "\\..\\frame_trace.log");
        g_traceFile = std::fopen(tracePath.c_str(), "w");
        if (g_traceFile) {
            std::fprintf(g_traceFile, "[D3D9-TRACE] Begin frame trace (%ux%u, scale=%.1fx%.1f, msaa=%u, af=%lu)\n",
                         g_outW, g_outH, g_scaleX, g_scaleY, g_curMsaa, static_cast<unsigned long>(g_maxAniso));
        }
    }

    // Safe per-frame texture cache flush (for F8 dump / F9 HD reload / toggle)
    if (g_flushCacheNextFrame.exchange(false)) {
        for (uint32_t s = 0; s < 4; ++s) {
            g_dev->SetTexture(s, nullptr);
            g_boundTexPtr[s] = nullptr;
        }
        for (auto& kv : g_texCache) {
            if (kv.second.tex) kv.second.tex->Release();
        }
        g_texCache.clear();
    }

    // Safe per-frame D3D9 Reset for live resolution / aspect / MSAA / Anisotropic changes
    if (g_resetPending.exchange(false)) {
        uint32_t newScale  = g_reqScale.load();
        uint32_t newMsaa   = g_reqMsaa.load();
        uint32_t newAniso  = g_reqAniso.load();
        uint32_t newAspect = g_reqAspect.load();
        g_maxAniso = std::min<DWORD>(g_hwMaxAniso, newAniso);

        D3DMULTISAMPLE_TYPE mt = ProbeMsaaType(newMsaa);
        bool needReset = (newScale != g_curScale) || (static_cast<uint32_t>(mt) != g_curMsaa) || (newAspect != g_curAspect);
        if (needReset) {
            uint32_t ow = 0, oh = 0;
            ComputeOutputResolution(newScale, newAspect, ow, oh);
            D3DPRESENT_PARAMETERS pp = g_presentParams;
            pp.BackBufferWidth  = ow;
            pp.BackBufferHeight = oh;
            pp.MultiSampleType  = mt;
            pp.MultiSampleQuality = 0;
            ReleasePostResources(false);
            if (SUCCEEDED(g_dev->Reset(&pp))) {
                g_presentParams = pp;
                g_curScale  = newScale;
                g_curMsaa   = static_cast<uint32_t>(mt);
                g_curAspect = newAspect;
                UpdateScaleMetrics(ow, oh, newAspect);
                ApplyInitialFixedState(mt);
                SetViewport(&g_lastXboxVp);
                if (g_hasLastProj) {
                    SetTransform(1, &g_lastProjMatrix);
                }
                const char* aspLabel = (g_curAspect == 2) ? "21:9" : ((g_curAspect == 1) ? "16:9" : "4:3");
                LogPrintf("[D3D9] Live graphics updated: %ux%u (%s %ux), %ux MSAA, %lux AF\n",
                          g_outW, g_outH, aspLabel, g_curScale, g_curMsaa, static_cast<unsigned long>(g_maxAniso));
            }
        } else {
            LogPrintf("[D3D9] Live Anisotropic Filtering updated: %lux AF\n", static_cast<unsigned long>(g_maxAniso));
        }
    }
}

void SetViewport(const void* pXboxViewport) {
    if (!g_dev || !pXboxViewport) return;
    const D3DVIEWPORT9* src = reinterpret_cast<const D3DVIEWPORT9*>(pXboxViewport);
    if (src->Width > 0 && src->Height > 0) {
        g_lastXboxVp = *src;
        D3DVIEWPORT9 vp{};
        // All bottom-row 3D HUD weapon icon sub-viewports (active weapon + D-Pad weapon selector carousel)
        // use the exact same centered 4:3 coordinate space (g_rhwScaleX + g_rhwOffX) as the 2D Weapon Ring & highlight!
        bool isWeaponIconVp = (g_curAspect != 0 &&
                               src->Y >= 320 &&
                               src->Width <= 160 && src->Height <= 160);
        if (g_menuPillarbox && g_curAspect != 0) {
            // Frontend Main Menu: pillarbox 3D scene (Buffy, moon, selected-item glow particles) to centered 4:3
            // so 3D menu glows align pixel-for-pixel behind the 2D menu text and no background leaks past the border.
            vp.X      = std::min<DWORD>(g_outW > 0 ? g_outW - 1 : 0, static_cast<DWORD>(src->X * g_rhwScaleX + g_rhwOffX));
            vp.Y      = std::min<DWORD>(g_outH > 0 ? g_outH - 1 : 0, static_cast<DWORD>(src->Y * g_scaleY));
            vp.Width  = std::min<DWORD>(g_outW - vp.X, static_cast<DWORD>(src->Width * g_rhwScaleX));
            vp.Height = std::min<DWORD>(g_outH - vp.Y, static_cast<DWORD>(src->Height * g_scaleY));
        } else if (isWeaponIconVp) {
            vp.X      = std::min<DWORD>(g_outW > 0 ? g_outW - 1 : 0, static_cast<DWORD>(src->X * g_rhwScaleX + g_rhwOffX));
            vp.Y      = std::min<DWORD>(g_outH > 0 ? g_outH - 1 : 0, static_cast<DWORD>(src->Y * g_scaleY));
            vp.Width  = std::min<DWORD>(g_outW - vp.X, static_cast<DWORD>(src->Width * g_rhwScaleX));
            vp.Height = std::min<DWORD>(g_outH - vp.Y, static_cast<DWORD>(src->Height * g_scaleY));
        } else {
            vp.X      = std::min<DWORD>(g_outW > 0 ? g_outW - 1 : 0, static_cast<DWORD>(src->X * g_scaleX));
            vp.Y      = std::min<DWORD>(g_outH > 0 ? g_outH - 1 : 0, static_cast<DWORD>(src->Y * g_scaleY));
            vp.Width  = std::min<DWORD>(g_outW - vp.X, static_cast<DWORD>(src->Width * g_scaleX));
            vp.Height = std::min<DWORD>(g_outH - vp.Y, static_cast<DWORD>(src->Height * g_scaleY));
        }
        vp.MinZ   = src->MinZ;
        vp.MaxZ   = src->MaxZ;
        if (vp.Width > 0 && vp.Height > 0) {
            g_dev->SetViewport(&vp);
        }
    }
}

void SetTransform(uint32_t xboxState, const void* pMatrix) {
    if (!g_dev || !pMatrix) return;
    D3DTRANSFORMSTATETYPE st = D3DTS_WORLD;
    switch (xboxState) {
        case 0: st = D3DTS_VIEW;        break; // Xbox D3DTS_VIEW
        case 1: st = D3DTS_PROJECTION;  break; // Xbox D3DTS_PROJECTION
        case 2: st = D3DTS_TEXTURE0;    break;
        case 3: st = D3DTS_TEXTURE1;    break;
        case 4: st = D3DTS_TEXTURE2;    break;
        case 5: st = D3DTS_TEXTURE3;    break;
        case 6: st = D3DTS_WORLD;       break; // Xbox D3DTS_WORLD
        case 7: st = D3DTS_WORLD1;      break;
        default: return;
    }
    const D3DMATRIX* mat = reinterpret_cast<const D3DMATRIX*>(pMatrix);
    if (xboxState == 6) {
        g_lastWorldMatrix = *mat;
    }
    if (xboxState == 1) {
        g_lastProjMatrix = *mat;
        g_hasLastProj = true;
    }
    if (xboxState >= 2 && xboxState <= 5) {
        g_gameTexMatrix[xboxState - 2] = *mat;
        g_hasGameTexMatrix[xboxState - 2] = true;
    }
    g_dev->SetTransform(st, mat);
}

void SetVertexShader(uint32_t handle) {
    if (!g_dev) return;
    if ((handle & 1u) == 0 && handle != 0) {
        g_fvf            = handle;
        g_customVS       = false;
        g_customVSHandle = 0;
        g_dev->SetFVF(handle);
    } else {
        g_customVS       = true;
        g_customVSHandle = handle;
    }
}

void RegisterVertexShaderDecl(uint32_t handle, const uint32_t* pDecl, int32_t shaderId) {
    if (!handle || !pDecl) return;
    ParsedVsDecl d{};
    d.shaderId = shaderId;
    int8_t curStream = 0;
    uint16_t curOff = 0;
    char rawToks[256]{};
    size_t rawPos = 0;
    for (int i = 0; i < 64; ++i) {
        uint32_t tok = pDecl[i];
        if (rawPos + 12 < sizeof(rawToks)) {
            rawPos += static_cast<size_t>(std::snprintf(rawToks + rawPos, sizeof(rawToks) - rawPos, "%08X ", tok));
        }
        if (tok == 0xFFFFFFFFu) break;
        uint32_t tt = (tok >> 29) & 7u;
        if (tt == 1u) {
            curStream = static_cast<int8_t>(tok & 0xFu);
            curOff = 0;
        } else if (tt == 2u) {
            if (tok & 0x10000000u) {
                uint32_t skipDwords = (tok >> 16) & 0xFu;
                curOff = static_cast<uint16_t>(curOff + skipDwords * 4u);
            } else {
                uint32_t reg = tok & 0xFu;
                uint32_t dt  = (tok >> 16) & 0xFFu;
                uint32_t cnt = (dt >> 4) & 0xFu;
                uint32_t et  = dt & 0xFu;
                uint32_t bytes = 4u;
                if (et == 2u) bytes = cnt * 4u;      // FLOAT1..4
                else if (et == 5u) bytes = cnt * 2u; // SHORT1..4
                else if (et == 0u || et == 1u || et == 4u) bytes = cnt ? cnt : 4u; // D3DCOLOR / BYTE
                else if (et == 6u) bytes = 4u;       // NORMPACKED3

                if (reg == 0u) {
                    d.posStream = curStream; d.posOff = curOff;
                } else if (reg == 3u) {
                    d.diffStream = curStream; d.diffOff = curOff;
                } else if (reg == 11u && dt == 0x40u) {
                    // Slate Engine dynamic/baked per-vertex diffuse lighting stream (v11 overrides packed normal v1!)
                    d.diffStream = curStream; d.diffOff = curOff;
                } else if (reg == 9u || (reg == 7u && d.uv0Stream < 0 && et == 2u)) {
                    d.uv0Stream = curStream; d.uv0Off = curOff; d.uv0Floats = static_cast<uint8_t>(cnt ? cnt : 2u);
                } else if (reg == 10u || (reg == 8u && d.uv1Stream < 0 && et == 2u)) {
                    d.uv1Stream = curStream; d.uv1Off = curOff; d.uv1Floats = static_cast<uint8_t>(cnt ? cnt : 2u);
                } else if (dt == 0x40u && d.diffStream < 0) {
                    // Fallback programmable VS diffuse color (e.g. StaticWind v2)
                    d.diffStream = curStream; d.diffOff = curOff;
                } else if (dt == 0x22u && d.uv0Stream < 0) {
                    // Programmable VS TEXCOORD0 (e.g. StaticWind v1 or StaticOmni0 v2)
                    d.uv0Stream = curStream; d.uv0Off = curOff; d.uv0Floats = 2u;
                } else if (dt == 0x22u && d.uv1Stream < 0) {
                    // Programmable VS TEXCOORD1
                    d.uv1Stream = curStream; d.uv1Off = curOff; d.uv1Floats = 2u;
                }
                curOff = static_cast<uint16_t>(curOff + bytes);
            }
        }
    }
    g_vsDecls[handle] = d;
    LogPrintf("[D3D9] VSDecl 0x%08X (id=%d): pos(s%d+%u) diff(s%d+%u) uv0(s%d+%u,%uF) uv1(s%d+%u,%uF) | %s\n",
              handle, shaderId, d.posStream, d.posOff, d.diffStream, d.diffOff,
              d.uv0Stream, d.uv0Off, d.uv0Floats, d.uv1Stream, d.uv1Off, d.uv1Floats, rawToks);
}

void SetStreamSource(uint32_t streamNumber, const void* pXboxVB, uint32_t stride) {
    if (streamNumber >= 4) return;
    const auto* vb = reinterpret_cast<const XboxD3DResource*>(pXboxVB);
    g_streams[streamNumber] = (vb && vb->Data) ? reinterpret_cast<const uint8_t*>(vb->Data) : nullptr;
    g_strides[streamNumber] = stride;
}

void SetIndices(const void* pXboxIB, uint32_t baseVertexIndex) {
    const auto* ib = reinterpret_cast<const XboxD3DResource*>(pXboxIB);
    g_indices  = (ib && ib->Data) ? reinterpret_cast<const uint16_t*>(ib->Data) : nullptr;
    g_baseVert = baseVertexIndex;
    // Mirror to Xbox D3D global _D3D__IndexData (0x0059F734) read directly by inline DrawIndexedPrimitive!
    *reinterpret_cast<uint32_t*>(0x0059F734) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_indices));
}

void SetTexture(uint32_t stage, const void* pXboxTexture) {
    if (!g_dev || stage >= 4) return;
    if (!pXboxTexture) {
        g_dev->SetTexture(stage, nullptr);
        g_boundTexPtr[stage]     = nullptr;
        g_boundTexPresent[stage] = false;
        g_boundTexLin[stage]     = false;
        g_boundTexVol[stage]     = false;
        return;
    }
    const auto* xtex = reinterpret_cast<const XboxD3DBaseTexture*>(pXboxTexture);
    IDirect3DBaseTexture9* tex = GetOrUploadTexture(stage, xtex);
    g_dev->SetTexture(stage, tex);
    g_boundTexPtr[stage] = tex;
    auto it = g_texCache.find(xtex);
    if (it != g_texCache.end() && tex) {
        g_boundTexPresent[stage] = true;
        g_boundTexW[stage]       = it->second.width;
        g_boundTexH[stage]       = it->second.height;
        g_boundTexLin[stage]     = it->second.linear;
        g_boundTexVol[stage]     = it->second.isVolume;
        g_boundTexFmt[stage]     = (it->second.format >> 8) & 0xFFu;
        if (g_tracingThisFrame && it->second.hash == 0 && xtex->Data) {
            uint32_t fmtB = g_boundTexFmt[stage];
            bool isD = (fmtB == 0x0C || fmtB == 0x0E || fmtB == 0x0F);
            uint32_t bp = isD ? ((fmtB == 0x0C) ? 8u : 16u) : ((fmtB == 0x06 || fmtB == 0x07 || fmtB == 0x12 || fmtB == 0x1E || fmtB == 0x3A) ? 4u : ((fmtB == 0x00 || fmtB == 0x13 || fmtB == 0x01 || fmtB == 0x19 || fmtB == 0x1F || fmtB == 0x0B) ? 1u : 2u));
            uint32_t l0 = isD ? (std::max<uint32_t>(1u, (it->second.width + 3) / 4) * std::max<uint32_t>(1u, (it->second.height + 3) / 4) * bp) : (it->second.width * it->second.height * bp);
            if (!IsBadReadPtr(reinterpret_cast<const void*>(xtex->Data), l0)) {
                it->second.hash = HashXboxTexture(it->second.width, it->second.height, fmtB, reinterpret_cast<const uint8_t*>(xtex->Data), l0);
            }
        }
        g_boundTexHash[stage]    = it->second.hash;
    } else {
        g_boundTexPresent[stage] = false;
        g_boundTexLin[stage]     = false;
        g_boundTexVol[stage]     = false;
        g_boundTexHash[stage]    = 0;
        g_boundTexFmt[stage]     = 0;
    }
}

void SetPalette(uint32_t stage, const void* pXboxPalette) {
    if (stage >= 4) return;
    const auto* pal = reinterpret_cast<const XboxD3DResource*>(pXboxPalette);
    g_palettes[stage] = (pal && pal->Data) ? reinterpret_cast<const uint32_t*>(pal->Data) : nullptr;
}

void SetRenderState_CullMode(uint32_t value) {
    reinterpret_cast<uint32_t*>(0x0059F940)[129] = value;
}

void SetRenderState_ZEnable(uint32_t value) {
    reinterpret_cast<uint32_t*>(0x0059F940)[125] = value;
}

void SetRenderState_TextureFactor(uint32_t value) {
    reinterpret_cast<uint32_t*>(0x0059F940)[130] = value;
}

void SetRenderState_FogColor(uint32_t value) {
    reinterpret_cast<uint32_t*>(0x0059F940)[120] = value;
}

static DWORD MapXboxBlend(uint32_t v) {
    switch (v) {
        case 0x0000: return D3DBLEND_ZERO;
        case 0x0001: return D3DBLEND_ONE;
        case 0x0300: return D3DBLEND_SRCCOLOR;
        case 0x0301: return D3DBLEND_INVSRCCOLOR;
        case 0x0302: return D3DBLEND_SRCALPHA;
        case 0x0303: return D3DBLEND_INVSRCALPHA;
        case 0x0304: return D3DBLEND_DESTALPHA;
        case 0x0305: return D3DBLEND_INVDESTALPHA;
        case 0x0306: return D3DBLEND_DESTCOLOR;
        case 0x0307: return D3DBLEND_INVDESTCOLOR;
        case 0x0308: return D3DBLEND_SRCALPHASAT;
        default:     return D3DBLEND_ONE;
    }
}

static DWORD MapXboxCmpFunc(uint32_t v) {
    // Xbox NV2A GL-style compare funcs: 0x200..0x207 -> D3DCMP_NEVER(1)..D3DCMP_ALWAYS(8)
    if (v >= 0x200 && v <= 0x207) return (v - 0x200) + 1;
    return D3DCMP_LESSEQUAL;
}

static DWORD MapXboxTextureOp(uint32_t op) {
    // Xbox XDK 4432 D3DTEXTUREOP -> PC D3D9 D3DTEXTUREOP
    switch (op) {
        case 1:  return D3DTOP_DISABLE;
        case 2:  return D3DTOP_SELECTARG1;
        case 3:  return D3DTOP_SELECTARG2;
        case 4:  return D3DTOP_MODULATE;
        case 5:  return D3DTOP_MODULATE2X;
        case 6:  return D3DTOP_MODULATE4X;
        case 7:  return D3DTOP_ADD;
        case 8:  return D3DTOP_ADDSIGNED;
        case 9:  return D3DTOP_ADDSIGNED2X;
        case 10: return D3DTOP_SUBTRACT;
        case 11: return D3DTOP_ADDSMOOTH;
        case 12: return D3DTOP_BLENDDIFFUSEALPHA;
        case 13: return D3DTOP_BLENDCURRENTALPHA;
        case 14: return D3DTOP_BLENDTEXTUREALPHA;
        case 15: return D3DTOP_BLENDFACTORALPHA;
        case 16: return D3DTOP_BLENDTEXTUREALPHAPM;
        case 17: return D3DTOP_PREMODULATE;
        case 18: return D3DTOP_MODULATEALPHA_ADDCOLOR;
        case 19: return D3DTOP_MODULATECOLOR_ADDALPHA;
        case 20: return D3DTOP_MODULATEINVALPHA_ADDCOLOR;
        case 21: return D3DTOP_MODULATEINVCOLOR_ADDALPHA;
        case 22: return D3DTOP_DOTPRODUCT3;
        case 23: return D3DTOP_MULTIPLYADD;
        case 24: return D3DTOP_LERP;
        case 25: return D3DTOP_BUMPENVMAP;
        case 26: return D3DTOP_BUMPENVMAPLUMINANCE;
        default: return D3DTOP_MODULATE;
    }
}

static DWORD MapXboxAddress(uint32_t addr) {
    switch (addr) {
        case 1: return D3DTADDRESS_WRAP;
        case 2: return D3DTADDRESS_MIRROR;
        case 3: return D3DTADDRESS_CLAMP;
        case 4: return D3DTADDRESS_BORDER;
        case 5: return D3DTADDRESS_CLAMP; // Xbox D3DTADDRESS_CLAMPTOEDGE
        default: return D3DTADDRESS_WRAP;
    }
}

// Sync live Xbox D3D8 inline/non-inline state tables directly from Xbox memory:
//   _D3D__RenderState  @ 0x0059F940 (XDK 4432 exact indices verified from Default.xbe)
//   _D3D__TextureState @ 0x0059F740 (4 stages x 32 DWORDs)
static void SyncXboxStates() {
    if (!g_dev) return;
    const uint32_t* rs = reinterpret_cast<const uint32_t*>(0x0059F940);
    const uint32_t* ts = reinterpret_cast<const uint32_t*>(0x0059F740);

    // Core XDK 4432 RenderStates (indices 57..67 & 120..130)
    g_dev->SetRenderState(D3DRS_ZFUNC,            MapXboxCmpFunc(rs[57]));
    g_dev->SetRenderState(D3DRS_ALPHAFUNC,        MapXboxCmpFunc(rs[58]));
    g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, rs[59] ? TRUE : FALSE);
    g_dev->SetRenderState(D3DRS_ALPHATESTENABLE,  rs[60] ? TRUE : FALSE);
    g_dev->SetRenderState(D3DRS_ALPHAREF,         rs[61] & 0xFFu);
    g_dev->SetRenderState(D3DRS_SRCBLEND,         MapXboxBlend(rs[62]));
    g_dev->SetRenderState(D3DRS_DESTBLEND,        MapXboxBlend(rs[63]));
    g_dev->SetRenderState(D3DRS_ZWRITEENABLE,     rs[64] ? TRUE : FALSE);
    g_dev->SetRenderState(D3DRS_FOGCOLOR,         rs[120]);
    g_dev->SetRenderState(D3DRS_ZENABLE,          rs[125] ? D3DZB_TRUE : D3DZB_FALSE);
    g_dev->SetRenderState(D3DRS_TEXTUREFACTOR,    rs[130]);
    g_dev->SetRenderState(D3DRS_SPECULARENABLE,   rs[94] ? TRUE : FALSE);
    g_dev->SetRenderState(D3DRS_DITHERENABLE,     TRUE);

    // Authentic Xbox Atmospheric Distance Fog (XDK 4432: rs[83..88], disabled on 2D HUD/menus and additive glows)
    // Xbox NV2A FOGTABLEMODE uses camera-space W/Z (world units, e.g. 200..2500), whereas modern PC D3D9 drivers
    // evaluate FOGTABLEMODE in normalized [0..1] Z (making world-unit fog invisible!).
    // Using D3DRS_FOGVERTEXMODE + D3DRS_RANGEFOGENABLE evaluates FOGSTART/FOGEND in true camera-space world units!
    bool is2DGlobal = (!g_customVS && ((g_fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW));
    bool isAdditive = (rs[59] != 0 && rs[63] == 1u); // DESTBLEND == D3DBLEND_ONE (item glows, fire, magic, dynamic lights)
    bool fogOn = (!is2DGlobal && !isAdditive && g_fogEnabled.load() && rs[83] != 0);
    g_dev->SetRenderState(D3DRS_FOGENABLE, fogOn ? TRUE : FALSE);
    if (fogOn) {
        DWORD fogMode = (rs[84] >= 1u && rs[84] <= 3u) ? rs[84] : D3DFOG_LINEAR;
        g_dev->SetRenderState(D3DRS_FOGTABLEMODE,   D3DFOG_NONE);
        g_dev->SetRenderState(D3DRS_FOGVERTEXMODE,  fogMode);
        g_dev->SetRenderState(D3DRS_FOGSTART,       rs[85]);
        g_dev->SetRenderState(D3DRS_FOGEND,         rs[86]);
        g_dev->SetRenderState(D3DRS_FOGDENSITY,     rs[87]);
        g_dev->SetRenderState(D3DRS_RANGEFOGENABLE, TRUE);
    }

    // Cull mode: Xbox 0 = NONE, 0x900 = CW, 0x901 = CCW
    D3DCULL cull = D3DCULL_NONE;
    if (rs[129] == 0x900) cull = D3DCULL_CW;
    else if (rs[129] == 0x901) cull = D3DCULL_CCW;
    g_dev->SetRenderState(D3DRS_CULLMODE, cull);

    // Texture Stage States for stages 0..3
    bool isWaterSurface = (!g_customVS && g_fvf == 0x242 && ts[12] == 4 && ts[32 + 12] == 4 && rs[64] == 0);
    for (uint32_t s = 0; s < 4; ++s) {
        const uint32_t* st = ts + s * 32;
        uint32_t colorOp = st[12];
        uint32_t alphaOp = st[16];
        if (isWaterSurface && s == 2) continue; // Configured by HD Water pass on Stage 1
        // Disable stages above 0 if no texture is bound and stage uses D3DTA_TEXTURE
        if (s > 0 && !g_boundTexPresent[s] && (colorOp == 0 || colorOp == 1)) {
            g_dev->SetTextureStageState(s, D3DTSS_COLOROP, D3DTOP_DISABLE);
            g_dev->SetTextureStageState(s, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
            continue;
        }
        // HD Water Polish (fvf=0x242, zWrite=0, Stage 0=animated water, Stage 1=broken 8x8 reflection atlas):
        // Replace Stage 1's tiled atlas with a counter-scrolling dual-layer copy of Stage 0's own water texture
        // for authentic interference wave ripples, plus a gentle breathing moonlight/murky teal tint on Stage 2!
        if (s == 1 && isWaterSurface) {
            float tSec = static_cast<float>(GetTickCount() & 0xFFFFFu) * 0.001f;
            float shimmer = 0.92f + 0.08f * std::sin(tSec * 1.4f);
            uint32_t r = static_cast<uint32_t>(0x3A * shimmer);
            uint32_t g = static_cast<uint32_t>(0x50 * shimmer);
            uint32_t b = static_cast<uint32_t>(0x4A * shimmer);
            uint32_t waterTint = (0x72u << 24) | ((r & 0xFFu) << 16) | ((g & 0xFFu) << 8) | (b & 0xFFu);

            g_dev->SetRenderState(D3DRS_TEXTUREFACTOR, waterTint);
            g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
            g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);

            if (g_boundTexPtr[0]) {
                // Stage 1: counter-scrolling 1.35x ripple interference layer using Stage 0's water texture
                g_dev->SetTexture(1, g_boundTexPtr[0]);
                g_dev->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
                g_dev->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
                g_dev->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                g_dev->SetSamplerState(1, D3DSAMP_MINFILTER, (g_maxAniso > 1) ? D3DTEXF_ANISOTROPIC : D3DTEXF_LINEAR);
                g_dev->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
                D3DMATRIX wMat{};
                wMat._11 = 1.35f; wMat._22 = 1.35f; wMat._33 = 1.0f; wMat._44 = 1.0f;
                wMat._31 = std::fmod(tSec * 0.032f, 1.0f);
                wMat._32 = std::fmod(-tSec * 0.024f, 1.0f);
                g_dev->SetTransform(D3DTS_TEXTURE1, &wMat);
                g_dev->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 0);
                g_dev->SetTextureStageState(1, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
                g_dev->SetTextureStageState(1, D3DTSS_COLOROP,   D3DTOP_ADDSMOOTH);
                g_dev->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_CURRENT);
                g_dev->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_TEXTURE);
                g_dev->SetTextureStageState(1, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
                g_dev->SetTextureStageState(1, D3DTSS_ALPHAARG1, D3DTA_CURRENT);

                // Stage 2: murky green-teal atmospheric tint & specular shimmer
                g_dev->SetTextureStageState(2, D3DTSS_COLOROP,   D3DTOP_MODULATE);
                g_dev->SetTextureStageState(2, D3DTSS_COLORARG1, D3DTA_CURRENT);
                g_dev->SetTextureStageState(2, D3DTSS_COLORARG2, D3DTA_TFACTOR);
                g_dev->SetTextureStageState(2, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
                g_dev->SetTextureStageState(2, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
            } else {
                g_dev->SetTextureStageState(1, D3DTSS_COLOROP,   D3DTOP_MODULATE);
                g_dev->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_CURRENT);
                g_dev->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_TFACTOR);
                g_dev->SetTextureStageState(1, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
                g_dev->SetTextureStageState(1, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
            }
            continue;
        }
        g_dev->SetSamplerState(s, D3DSAMP_ADDRESSU,    MapXboxAddress(st[0]));
        g_dev->SetSamplerState(s, D3DSAMP_ADDRESSV,    MapXboxAddress(st[1]));
        g_dev->SetSamplerState(s, D3DSAMP_ADDRESSW,    MapXboxAddress(st[2]));
        g_dev->SetSamplerState(s, D3DSAMP_BORDERCOLOR, st[11]);
        bool is2D = (!g_customVS && ((g_fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW));
        if (g_boundTexVol[s]) {
            // D3D9 does NOT support ANISOTROPIC filtering on 3D Volume Textures; use pure trilinear/linear!
            g_dev->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            g_dev->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            g_dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        } else if (!is2D && g_maxAniso > 1) {
            // Force crisp 16x Anisotropic + Trilinear Mipmapping on all 3D world/ground textures
            g_dev->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            g_dev->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC);
            g_dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
            g_dev->SetSamplerState(s, D3DSAMP_MAXANISOTROPY, g_maxAniso);
        } else {
            g_dev->SetSamplerState(s, D3DSAMP_MAGFILTER, (st[3] == 2) ? D3DTEXF_LINEAR : D3DTEXF_POINT);
            g_dev->SetSamplerState(s, D3DSAMP_MINFILTER, (st[4] == 2) ? D3DTEXF_LINEAR : D3DTEXF_POINT);
            g_dev->SetSamplerState(s, D3DSAMP_MIPFILTER, (st[5] == 2) ? D3DTEXF_LINEAR : D3DTEXF_NONE);
        }

        // Character/prop multi-texture path: Xbox sets Stage 0 colorOp=25 on DXT/RGB character diffuse textures
        // (Buffy's face/hair/shirt, Giles, vampires) with Stage 1 = 64x64 specular env map.
        // D3D9 rejects D3DTOP_BUMPENVMAP on non-V8U8 textures (dropping the diffuse texture completely!).
        // Route non-bump colorOp=25 on Stage 0 to D3DTOP_MODULATE (TEXTURE * DIFFUSE) and disable Stage 1 dark env multiply.
        bool isCharNonBumpOp25 = (!is2D && ts[12] == 25 && g_boundTexPresent[0] &&
                                  g_boundTexFmt[0] != 0x28 && g_boundTexFmt[0] != 0x27);
        if (isCharNonBumpOp25 && s == 0) {
            g_dev->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_MODULATE);
            g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            g_dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_MODULATE);
            g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
            g_dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
            g_dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
            continue;
        }
        if (isCharNonBumpOp25 && s == 1) {
            g_dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
            g_dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
            continue;
        }

        // Boost 3D additive particle/sparkle glows (fvf=0x142, DESTBLEND=ONE, MODULATE -> MODULATE2X)
        // to restore the vivid Xbox CRT/gamma-lifted luminescence on item halos, slayer sparkles & flames.
        DWORD resolvedColorOp = colorOp ? MapXboxTextureOp(colorOp) : (s == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE);
        if (s == 0 && !is2D && g_fvf == 0x142 && isAdditive && resolvedColorOp == D3DTOP_MODULATE) {
            resolvedColorOp = D3DTOP_MODULATE2X;
        }
        g_dev->SetTextureStageState(s, D3DTSS_COLOROP,   resolvedColorOp);
        g_dev->SetTextureStageState(s, D3DTSS_COLORARG0, st[13]);
        g_dev->SetTextureStageState(s, D3DTSS_COLORARG1, st[14]);
        g_dev->SetTextureStageState(s, D3DTSS_COLORARG2, st[15]);
        g_dev->SetTextureStageState(s, D3DTSS_ALPHAOP,   alphaOp ? MapXboxTextureOp(alphaOp) : (s == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE));
        g_dev->SetTextureStageState(s, D3DTSS_ALPHAARG0, st[17]);
        g_dev->SetTextureStageState(s, D3DTSS_ALPHAARG1, st[18]);
        g_dev->SetTextureStageState(s, D3DTSS_ALPHAARG2, st[19]);

        // Hardware Bump-Env-Map matrices for water/reflective ripples (XDK 4432: 25=BUMPENVMAP, 26=BUMPENVMAPLUMINANCE)
        if (colorOp == 25 || colorOp == 26) {
            g_dev->SetTextureStageState(s, D3DTSS_BUMPENVMAT00,    st[22]);
            g_dev->SetTextureStageState(s, D3DTSS_BUMPENVMAT01,    st[23]);
            g_dev->SetTextureStageState(s, D3DTSS_BUMPENVMAT10,    st[24]);
            g_dev->SetTextureStageState(s, D3DTSS_BUMPENVMAT11,    st[25]);
            g_dev->SetTextureStageState(s, D3DTSS_BUMPENVLSCALE,   st[26]);
            g_dev->SetTextureStageState(s, D3DTSS_BUMPENVLOFFSET,  st[27]);
        }

        // Sync Xbox D3DTSS_TEXCOORDINDEX (st[28]) so Stage 0 lightmap passes (TEXCOORD1) work!
        g_dev->SetTextureStageState(s, D3DTSS_TEXCOORDINDEX, is2D ? (st[28] & 0x7u) : st[28]);

        // Linear Xbox textures use unnormalized pixel-space UVs (0..W, 0..H) -> normalize via texture matrix
        if (g_boundTexPresent[s] && g_boundTexLin[s] && g_boundTexW[s] > 0 && g_boundTexH[s] > 0) {
            D3DMATRIX m{};
            m._11 = 1.0f / static_cast<float>(g_boundTexW[s]);
            m._22 = 1.0f / static_cast<float>(g_boundTexH[s]);
            m._33 = 1.0f;
            m._44 = 1.0f;
            g_dev->SetTransform(static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_TEXTURE0 + s), &m);
            g_dev->SetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
        } else if (!is2D && g_hasGameTexMatrix[s] && (st[21] & ~256u) <= 4u && st[21] != 0) {
            // Apply Xbox 3D texture transform matrix (st[21] = D3DTSS_TEXTURETRANSFORMFLAGS) for water/reflections
            g_dev->SetTransform(static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_TEXTURE0 + s), &g_gameTexMatrix[s]);
            g_dev->SetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, st[21]);
        } else {
            g_dev->SetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        }
    }
}

void SetRenderState(uint32_t xboxState, uint32_t value) {
    if (xboxState < 150) {
        reinterpret_cast<uint32_t*>(0x0059F940)[xboxState] = value;
    }
}

void SetTextureStageState(uint32_t stage, uint32_t xboxType, uint32_t value) {
    if (stage < 4 && xboxType < 32) {
        reinterpret_cast<uint32_t*>(0x0059F740)[stage * 32 + xboxType] = value;
    }
}

static bool MapXboxPrimitive(uint32_t xboxPrimType, uint32_t vertCount, D3DPRIMITIVETYPE& outType, uint32_t& outPrimCount) {
    switch (xboxPrimType) {
        case 1: outType = D3DPT_POINTLIST;     outPrimCount = vertCount;             return vertCount >= 1;
        case 2: outType = D3DPT_LINELIST;      outPrimCount = vertCount / 2;         return vertCount >= 2;
        case 3:
        case 4: outType = D3DPT_LINESTRIP;     outPrimCount = vertCount - 1;         return vertCount >= 2;
        case 5: outType = D3DPT_TRIANGLELIST;  outPrimCount = vertCount / 3;         return vertCount >= 3;
        case 6: outType = D3DPT_TRIANGLESTRIP; outPrimCount = vertCount - 2;         return vertCount >= 3;
        case 7: outType = D3DPT_TRIANGLEFAN;   outPrimCount = vertCount - 2;         return vertCount >= 3;
        case 9: outType = D3DPT_TRIANGLESTRIP; outPrimCount = vertCount - 2;         return vertCount >= 3;
        default: return false;
    }
}

static void EnsureValidFvfForStride(uint32_t stride) {
    if (!g_customVS) return;
    // Fallback FVF for custom Xbox vertex shaders until full VS translation is added
    uint32_t fvf = D3DFVF_XYZ | D3DFVF_TEX1;
    if (stride == 12)      fvf = D3DFVF_XYZ;
    else if (stride == 20) fvf = D3DFVF_XYZ | D3DFVF_TEX1;
    else if (stride == 24) fvf = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1;
    else if (stride == 28) fvf = D3DFVF_XYZ | D3DFVF_TEX2;
    else if (stride == 32) fvf = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX2;
    else if (stride == 36) fvf = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1;
    g_dev->SetFVF(fvf);
}

static const void* ScaleRhwVerticesIfNeeded(const void* pVertexData, uint32_t vertexCount, uint32_t stride) {
    bool isRhw = (!g_customVS && ((g_fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW));
    if (!isRhw || (g_curAspect == 0 && g_scaleX == 1.0f && g_scaleY == 1.0f) || stride < 16 || vertexCount == 0) {
        return pVertexData;
    }
    size_t totalBytes = static_cast<size_t>(vertexCount) * stride;
    g_rhwVertScratch.resize(totalBytes);
    std::memcpy(g_rhwVertScratch.data(), pVertexData, totalBytes);
    uint8_t* p = g_rhwVertScratch.data();

    // Detect full-screen 2D quads & edge-anchored HUD elements in 16:9 / 21:9 widescreen:
    //   - Full-screen fades/backgrounds -> stretch full width
    //   - Top-left (Buffy health/slayer bar) & bottom-left (weapon ring) -> anchor to left edge
    //   - Top-right (enemy health bar) & bottom-right HUD -> anchor to right edge
    //   - Centered TIP banners, subtitles, Main Menu & Pause Menu -> stay centered
    bool  stretchFullWidth = (g_curAspect == 0);
    float edgeOffX         = g_rhwOffX;
    if (g_curAspect != 0 && !g_menuPillarbox && vertexCount >= 3) {
        float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
        for (uint32_t i = 0; i < vertexCount; ++i) {
            const float* xy = reinterpret_cast<const float*>(p + i * stride);
            if (xy[0] < minX) minX = xy[0];
            if (xy[0] > maxX) maxX = xy[0];
            if (xy[1] < minY) minY = xy[1];
            if (xy[1] > maxY) maxY = xy[1];
        }
        float quadW = maxX - minX;
        float quadH = maxY - minY;
        if (minX <= 2.0f && maxX >= static_cast<float>(g_xboxW) - 2.0f) {
            stretchFullWidth = true;
        } else if (maxX < 285.0f && maxY < 130.0f && quadW >= 20.0f) {
            // Top-left Buffy Health / Slayer Power bar -> anchor to left edge
            edgeOffX = 0.0f;
        } else if (minX > 310.0f && maxX > 460.0f && maxY < 150.0f && quadW >= 20.0f) {
            // Top-right Enemy Health bar & right-aligned cutscene location/timestamp text -> anchor to right edge
            edgeOffX = g_rhwOffX * 2.0f;
        }
        // Bottom HUD (Weapon Ring, expanding D-Pad weapon selector carousel, highlight, TIP banners & subtitles)
        // stays in the unified centered 4:3 safe zone so the D-Pad weapon highlight always aligns pixel-for-pixel!
    }

    for (uint32_t i = 0; i < vertexCount; ++i) {
        float* xy = reinterpret_cast<float*>(p + i * stride);
        if (stretchFullWidth) {
            xy[0] *= g_scaleX;
        } else {
            xy[0] = xy[0] * g_rhwScaleX + edgeOffX;
        }
        xy[1] *= g_scaleY;
    }
    return g_rhwVertScratch.data();
}

static void TraceDrawCallIfEnabled(const char* tag, uint32_t prim, uint32_t vcount, uint32_t stride, const void* vdata) {
    if (!g_tracingThisFrame || !g_traceFile || g_traceDrawIdx >= 2000) return;
    ++g_traceDrawIdx;
    const float* f = reinterpret_cast<const float*>(vdata);
    const uint32_t* rs = reinterpret_cast<const uint32_t*>(0x0059F940);
    const uint32_t* ts = reinterpret_cast<const uint32_t*>(0x0059F740);
    float u0 = (stride >= 20) ? f[3] : 0.0f;
    float v0t = (stride >= 20) ? f[4] : 0.0f;
    float u1 = (stride >= 28) ? f[5] : 0.0f;
    float v1t = (stride >= 28) ? f[6] : 0.0f;
    std::fprintf(g_traceFile, "[#%04u] %s p=%u n=%u st=%u fvf=0x%X pos=(%.0f,%.0f,%.0f) uv0=(%.2f,%.2f) uv1=(%.2f,%.2f) t0=%016llx(%ux%u,f=%02X) t1=%016llx(%ux%u,v=%u,f=%02X) cop=%u/%u aop=%u/%u tci=%X/%X ttf=%X/%X bl=%u(%X,%X) z=%u/%u",
                 g_traceDrawIdx, tag, prim, vcount, stride, g_fvf,
                 f[0], f[1], (stride >= 12 ? f[2] : 0.0f),
                 u0, v0t, u1, v1t,
                 static_cast<unsigned long long>(g_boundTexHash[0]),
                 g_boundTexPresent[0] ? g_boundTexW[0] : 0, g_boundTexPresent[0] ? g_boundTexH[0] : 0, g_boundTexFmt[0],
                 static_cast<unsigned long long>(g_boundTexHash[1]),
                 g_boundTexPresent[1] ? g_boundTexW[1] : 0, g_boundTexPresent[1] ? g_boundTexH[1] : 0, g_boundTexVol[1] ? 1u : 0u, g_boundTexFmt[1],
                 ts[12], ts[32 + 12], ts[16], ts[32 + 16], ts[28], ts[32 + 28], ts[21], ts[32 + 21],
                 rs[59], rs[62], rs[63], rs[125], rs[64]);
    if (g_boundTexVol[1] || g_fvf == 0x40202) {
        std::fprintf(g_traceFile, " tf=0x%08X ca=(%X,%X)/(%X,%X) aa=(%X,%X)/(%X,%X) a1=%u,%u,%u m1=(%.4f,%.4f,%.4f;%.2f,%.2f,%.2f)",
                     rs[130], ts[14], ts[15], ts[32 + 14], ts[32 + 15],
                     ts[18], ts[19], ts[32 + 18], ts[32 + 19],
                     ts[32 + 0], ts[32 + 1], ts[32 + 2],
                     g_gameTexMatrix[1]._11, g_gameTexMatrix[1]._22, g_gameTexMatrix[1]._33,
                     g_gameTexMatrix[1]._41, g_gameTexMatrix[1]._42, g_gameTexMatrix[1]._43);
    }
    std::fprintf(g_traceFile, "\n");
}

// Expand 2D RHW D3DPT_LINELIST (e.g. TTextureImplementation::Draw2DWavy unselected menu text)
// into resolution-scaled 2-triangle quads so 1px Xbox scanlines stay solid at 2x/3x/4x/widescreen.
static bool TryDrawScaledRhwLineList(uint32_t xboxPrimType, uint32_t vertexCount, const uint16_t* idx, const void* vdata, uint32_t stride) {
    if (xboxPrimType != 2 || vertexCount < 2 || stride < 16) return false;
    bool isRhw = (!g_customVS && ((g_fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW));
    if (!isRhw || (g_scaleX <= 1.0f && g_scaleY <= 1.0f)) return false;

    uint32_t numLines = vertexCount / 2;
    if (numLines == 0) return false;

    const uint8_t* base = static_cast<const uint8_t*>(vdata);
    auto getVert = [&](uint32_t i) -> const uint8_t* {
        uint32_t vi = idx ? static_cast<uint32_t>(idx[i]) : i;
        return base + vi * stride;
    };

    float thickY = g_scaleY;
    if (numLines >= 2) {
        float y0 = reinterpret_cast<const float*>(getVert(0))[1];
        float y1 = reinterpret_cast<const float*>(getVert(2))[1];
        float stepY = std::fabs(y1 - y0);
        if (stepY >= g_scaleY * 0.5f && stepY <= g_scaleY * 2.5f) {
            thickY = stepY;
        }
    }
    float thickX = g_rhwScaleX;

    g_quadVertScratch.resize(static_cast<size_t>(numLines) * 6 * stride);
    uint8_t* dst = g_quadVertScratch.data();

    for (uint32_t l = 0; l < numLines; ++l) {
        const uint8_t* va = getVert(l * 2 + 0);
        const uint8_t* vb = getVert(l * 2 + 1);
        const float* fa = reinterpret_cast<const float*>(va);
        const float* fb = reinterpret_cast<const float*>(vb);
        bool horiz = (std::fabs(fb[0] - fa[0]) >= std::fabs(fb[1] - fa[1]));

        uint8_t* out = dst + static_cast<size_t>(l) * 6 * stride;
        // Triangle 1: (va, vb, vb+thick)
        std::memcpy(out + 0 * stride, va, stride);
        std::memcpy(out + 1 * stride, vb, stride);
        std::memcpy(out + 2 * stride, vb, stride);
        // Triangle 2: (va, vb+thick, va+thick)
        std::memcpy(out + 3 * stride, va, stride);
        std::memcpy(out + 4 * stride, vb, stride);
        std::memcpy(out + 5 * stride, va, stride);

        if (horiz) {
            reinterpret_cast<float*>(out + 2 * stride)[1] += thickY;
            reinterpret_cast<float*>(out + 4 * stride)[1] += thickY;
            reinterpret_cast<float*>(out + 5 * stride)[1] += thickY;
        } else {
            reinterpret_cast<float*>(out + 2 * stride)[0] += thickX;
            reinterpret_cast<float*>(out + 4 * stride)[0] += thickX;
            reinterpret_cast<float*>(out + 5 * stride)[0] += thickX;
        }
    }

    g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, numLines * 2, g_quadVertScratch.data(), stride);
    return true;
}

void DrawVerticesUP(uint32_t xboxPrimType, uint32_t vertexCount, const void* pVertexData, uint32_t stride) {
    if (!g_dev || !pVertexData || vertexCount == 0 || stride == 0 || g_customVS) return;
    EnsureSceneBegun();
    SyncXboxStates();
    const void* vdata = ScaleRhwVerticesIfNeeded(pVertexData, vertexCount, stride);
    TraceDrawCallIfEnabled("UP", xboxPrimType, vertexCount, stride, vdata);

    if (TryDrawScaledRhwLineList(xboxPrimType, vertexCount, nullptr, vdata, stride)) {
        return;
    }

    // Handle Xbox D3DPT_QUADLIST (8): expand each 4 vertices into 2 triangles (6 vertices)
    if (xboxPrimType == 8) {
        uint32_t numQuads = vertexCount / 4;
        if (numQuads == 0) return;
        g_quadVertScratch.resize(numQuads * 6 * stride);
        const uint8_t* src = static_cast<const uint8_t*>(vdata);
        uint8_t* dst = g_quadVertScratch.data();
        for (uint32_t q = 0; q < numQuads; ++q) {
            const uint8_t* v0 = src + (q * 4 + 0) * stride;
            const uint8_t* v1 = src + (q * 4 + 1) * stride;
            const uint8_t* v2 = src + (q * 4 + 2) * stride;
            const uint8_t* v3 = src + (q * 4 + 3) * stride;
            std::memcpy(dst + (q * 6 + 0) * stride, v0, stride);
            std::memcpy(dst + (q * 6 + 1) * stride, v1, stride);
            std::memcpy(dst + (q * 6 + 2) * stride, v2, stride);
            std::memcpy(dst + (q * 6 + 3) * stride, v0, stride);
            std::memcpy(dst + (q * 6 + 4) * stride, v2, stride);
            std::memcpy(dst + (q * 6 + 5) * stride, v3, stride);
        }
        g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, numQuads * 2, g_quadVertScratch.data(), stride);
        return;
    }

    D3DPRIMITIVETYPE pt = D3DPT_TRIANGLELIST;
    uint32_t primCount = 0;
    if (!MapXboxPrimitive(xboxPrimType, vertexCount, pt, primCount) || primCount == 0) return;
    g_dev->DrawPrimitiveUP(pt, primCount, vdata, stride);
}

void DrawIndexedVerticesUP(uint32_t xboxPrimType, uint32_t vertexCount, const void* pIndexData, const void* pVertexData, uint32_t stride) {
    if (!g_dev || !pIndexData || !pVertexData || vertexCount == 0 || stride == 0 || g_customVS) return;
    EnsureSceneBegun();
    SyncXboxStates();

    const uint16_t* idx = static_cast<const uint16_t*>(pIndexData);
    uint16_t maxIdx = 0;
    for (uint32_t i = 0; i < vertexCount; ++i) {
        if (idx[i] > maxIdx) maxIdx = idx[i];
    }
    const void* vdata = ScaleRhwVerticesIfNeeded(pVertexData, static_cast<uint32_t>(maxIdx) + 1, stride);
    TraceDrawCallIfEnabled("IUP", xboxPrimType, vertexCount, stride, vdata);

    if (TryDrawScaledRhwLineList(xboxPrimType, vertexCount, idx, vdata, stride)) {
        return;
    }

    if (xboxPrimType == 8) {
        uint32_t numQuads = vertexCount / 4;
        if (numQuads == 0) return;
        g_quadIdxScratch.resize(numQuads * 6);
        for (uint32_t q = 0; q < numQuads; ++q) {
            g_quadIdxScratch[q * 6 + 0] = idx[q * 4 + 0];
            g_quadIdxScratch[q * 6 + 1] = idx[q * 4 + 1];
            g_quadIdxScratch[q * 6 + 2] = idx[q * 4 + 2];
            g_quadIdxScratch[q * 6 + 3] = idx[q * 4 + 0];
            g_quadIdxScratch[q * 6 + 4] = idx[q * 4 + 2];
            g_quadIdxScratch[q * 6 + 5] = idx[q * 4 + 3];
        }
        g_dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, maxIdx + 1, numQuads * 2,
                                      g_quadIdxScratch.data(), D3DFMT_INDEX16, vdata, stride);
        return;
    }

    D3DPRIMITIVETYPE pt = D3DPT_TRIANGLELIST;
    uint32_t primCount = 0;
    if (!MapXboxPrimitive(xboxPrimType, vertexCount, pt, primCount) || primCount == 0) return;
    g_dev->DrawIndexedPrimitiveUP(pt, 0, maxIdx + 1, primCount, idx, D3DFMT_INDEX16, vdata, stride);
}

void DrawIndexedVertices(uint32_t xboxPrimType, uint32_t vertexCount, const void* pIndexData) {
    if (!g_dev || !g_streams[0] || g_strides[0] == 0 || vertexCount == 0) return;
    uintptr_t rawIdx = reinterpret_cast<uintptr_t>(pIndexData);
    const uint16_t* idx = nullptr;
    if (rawIdx >= 0x00010000u) {
        idx = reinterpret_cast<const uint16_t*>(rawIdx);
    } else if (g_indices) {
        idx = reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(g_indices) + rawIdx);
    }
    if (!idx) return;

    // Multi-stream fixed-function 3D Volume Light passes (TBufferGroup::IntermediateRender: torch & dynamic point lights!)
    // Slate Engine creates a multi-stream FVF declaration via Precache_Shader(-1, decl):
    //   Stream 0: float3 Position, Stream 1: float3 TexCoord0, [Stream 2: float3 TexCoord1]
    if (g_customVS && (g_boundTexVol[0] || g_boundTexVol[1])) {
        EnsureSceneBegun();
        SyncXboxStates();

        uint16_t maxIdx = 0;
        for (uint32_t i = 0; i < vertexCount; ++i) {
            if (idx[i] > maxIdx) maxIdx = idx[i];
        }
        uint32_t numVerts = static_cast<uint32_t>(maxIdx) + 1;
        const uint32_t* ts = reinterpret_cast<const uint32_t*>(0x0059F740);
        bool hasS1 = (g_streams[1] != nullptr && g_strides[1] >= 8);
        bool hasS2 = (g_streams[2] != nullptr && g_strides[2] >= 8 && g_boundTexPresent[1] && ts[32 + 12] > 1);

        uint32_t floatsPerVert = hasS2 ? 9u : 6u;
        DWORD packedFvf = hasS2
            ? (D3DFVF_XYZ | D3DFVF_TEX2 | D3DFVF_TEXCOORDSIZE3(0) | D3DFVF_TEXCOORDSIZE3(1))
            : (D3DFVF_XYZ | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE3(0));

        g_volVertScratch.resize(numVerts * floatsPerVert);
        const uint8_t* s0Base = g_streams[0] + g_baseVert * g_strides[0];
        const uint8_t* s1Base = hasS1 ? (g_streams[1] + g_baseVert * g_strides[1]) : s0Base;
        uint32_t       s1Str  = hasS1 ? g_strides[1] : g_strides[0];
        const uint8_t* s2Base = hasS2 ? (g_streams[2] + g_baseVert * g_strides[2]) : s0Base;
        uint32_t       s2Str  = hasS2 ? g_strides[2] : g_strides[0];

        float* out = g_volVertScratch.data();
        for (uint32_t i = 0; i < numVerts; ++i) {
            const float* p0 = reinterpret_cast<const float*>(s0Base + i * g_strides[0]);
            const float* p1 = reinterpret_cast<const float*>(s1Base + i * s1Str);
            out[0] = p0[0]; out[1] = p0[1]; out[2] = p0[2];
            out[3] = p1[0]; out[4] = p1[1]; out[5] = (s1Str >= 12) ? p1[2] : 0.0f;
            if (hasS2) {
                const float* p2 = reinterpret_cast<const float*>(s2Base + i * s2Str);
                out[6] = p2[0]; out[7] = p2[1]; out[8] = (s2Str >= 12) ? p2[2] : 0.0f;
            }
            out += floatsPerVert;
        }

        uint32_t packedStride = floatsPerVert * sizeof(float);
        TraceDrawCallIfEnabled("VOL_VS", xboxPrimType, vertexCount, packedStride, g_volVertScratch.data());

        D3DPRIMITIVETYPE pt = D3DPT_TRIANGLELIST;
        uint32_t primCount = 0;
        if (MapXboxPrimitive(xboxPrimType, vertexCount, pt, primCount) && primCount > 0) {
            g_dev->SetFVF(packedFvf);
            g_dev->DrawIndexedPrimitiveUP(pt, 0, numVerts, primCount, idx, D3DFMT_INDEX16, g_volVertScratch.data(), packedStride);
        }
        return;
    }

    // Multi-stream fixed-function declarations registered via Precache_Shader (e.g. trees, foliage, props with separate color/UV streams):
    // Read Position, Diffuse, TexCoord0 & TexCoord1 from their exact declared streams & byte offsets!
    if (g_customVS) {
        auto it = g_vsDecls.find(g_customVSHandle);
        if (it == g_vsDecls.end() || it->second.posStream < 0) return;
        const ParsedVsDecl& d = it->second;
        if (!g_streams[d.posStream] || g_strides[d.posStream] < d.posOff + 12u) return;

        bool hasDiff = (d.diffStream >= 0 && g_streams[d.diffStream] && g_strides[d.diffStream] >= d.diffOff + 4u);
        bool hasUv0  = (d.uv0Stream  >= 0 && g_streams[d.uv0Stream]  && g_strides[d.uv0Stream]  >= d.uv0Off + d.uv0Floats * 4u);
        bool hasUv1  = (d.uv1Stream  >= 0 && g_streams[d.uv1Stream]  && g_strides[d.uv1Stream]  >= d.uv1Off + d.uv1Floats * 4u && hasUv0);

        DWORD packedFvf = D3DFVF_XYZ | (hasDiff ? D3DFVF_DIFFUSE : 0u);
        uint32_t outStride = 12u + (hasDiff ? 4u : 0u);
        uint32_t uv0F = hasUv0 ? (d.uv0Floats == 3u ? 3u : 2u) : 0u;
        uint32_t uv1F = hasUv1 ? (d.uv1Floats == 3u ? 3u : 2u) : 0u;
        if (hasUv1) {
            packedFvf |= D3DFVF_TEX2 | (uv0F == 3u ? D3DFVF_TEXCOORDSIZE3(0) : D3DFVF_TEXCOORDSIZE2(0))
                                     | (uv1F == 3u ? D3DFVF_TEXCOORDSIZE3(1) : D3DFVF_TEXCOORDSIZE2(1));
            outStride += (uv0F + uv1F) * 4u;
        } else if (hasUv0) {
            packedFvf |= D3DFVF_TEX1 | (uv0F == 3u ? D3DFVF_TEXCOORDSIZE3(0) : D3DFVF_TEXCOORDSIZE2(0));
            outStride += uv0F * 4u;
        }

        EnsureSceneBegun();
        SyncXboxStates();

        uint16_t maxIdx = 0;
        for (uint32_t i = 0; i < vertexCount; ++i) {
            if (idx[i] > maxIdx) maxIdx = idx[i];
        }
        uint32_t numVerts = static_cast<uint32_t>(maxIdx) + 1u;
        g_customVsScratch.resize(numVerts * outStride);
        uint8_t* dst = g_customVsScratch.data();

        const uint8_t* posBase  = g_streams[d.posStream]  + g_baseVert * g_strides[d.posStream]  + d.posOff;
        const uint8_t* diffBase = hasDiff ? (g_streams[d.diffStream] + g_baseVert * g_strides[d.diffStream] + d.diffOff) : nullptr;
        const uint8_t* uv0Base  = hasUv0  ? (g_streams[d.uv0Stream]  + g_baseVert * g_strides[d.uv0Stream]  + d.uv0Off)  : nullptr;
        const uint8_t* uv1Base  = hasUv1  ? (g_streams[d.uv1Stream]  + g_baseVert * g_strides[d.uv1Stream]  + d.uv1Off)  : nullptr;

        for (uint32_t i = 0; i < numVerts; ++i) {
            uint8_t* out = dst + i * outStride;
            std::memcpy(out, posBase + i * g_strides[d.posStream], 12);
            out += 12;
            if (hasDiff) {
                std::memcpy(out, diffBase + i * g_strides[d.diffStream], 4);
                out += 4;
            }
            if (hasUv0) {
                std::memcpy(out, uv0Base + i * g_strides[d.uv0Stream], uv0F * 4u);
                out += uv0F * 4u;
            }
            if (hasUv1) {
                std::memcpy(out, uv1Base + i * g_strides[d.uv1Stream], uv1F * 4u);
            }
        }

        D3DPRIMITIVETYPE pt = D3DPT_TRIANGLELIST;
        uint32_t primCount = 0;
        if (MapXboxPrimitive(xboxPrimType, vertexCount, pt, primCount) && primCount > 0) {
            // For programmable Xbox vertex shaders (shaderId >= 0), Slate Engine uploads the
            // object's 3x4 World matrix into CD3DMatrixState::m_dRenderRegisters[6..8] (0x00834BC0)
            // instead of calling SetTransform(D3DTS_WORLD). Apply it cleanly for this draw!
            bool appliedConstWorld = false;
            if (d.shaderId >= 0) {
                const float* c6 = reinterpret_cast<const float*>(0x00834BC0);
                const float* c7 = reinterpret_cast<const float*>(0x00834BD0);
                const float* c8 = reinterpret_cast<const float*>(0x00834BE0);
                if (c6[0] != 0.0f || c6[1] != 0.0f || c6[2] != 0.0f ||
                    c7[0] != 0.0f || c7[1] != 0.0f || c7[2] != 0.0f) {
                    D3DMATRIX w{};
                    w._11 = c6[0]; w._12 = c7[0]; w._13 = c8[0]; w._14 = 0.0f;
                    w._21 = c6[1]; w._22 = c7[1]; w._23 = c8[1]; w._24 = 0.0f;
                    w._31 = c6[2]; w._32 = c7[2]; w._33 = c8[2]; w._34 = 0.0f;
                    w._41 = c6[3]; w._42 = c7[3]; w._43 = c8[3]; w._44 = 1.0f;
                    g_dev->SetTransform(D3DTS_WORLD, &w);
                    appliedConstWorld = true;
                }
            }

            g_dev->SetFVF(packedFvf);
            g_dev->DrawIndexedPrimitiveUP(pt, 0, numVerts, primCount, idx, D3DFMT_INDEX16, g_customVsScratch.data(), outStride);

            if (appliedConstWorld) {
                g_dev->SetTransform(D3DTS_WORLD, &g_lastWorldMatrix);
            }
        }
        return;
    }

    const uint8_t* vbase = g_streams[0] + g_baseVert * g_strides[0];
    DrawIndexedVerticesUP(xboxPrimType, vertexCount, idx, vbase, g_strides[0]);
}

} // namespace d3d9_bridge
