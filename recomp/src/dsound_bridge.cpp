#include "dsound_bridge.h"
#include "xboxkrnl_win32.h"
#include <mmsystem.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "winmm.lib")

namespace dsound_bridge {

// ============================================================================
// Xbox DirectSound8 / XMediaObject Structs (XDK 4432)
// ============================================================================
struct XBOX_DSBUFFERDESC {
    DWORD          dwSize;
    DWORD          dwFlags;
    DWORD          dwBufferBytes;
    WAVEFORMATEX*  lpwfxFormat;
    void*          lpMixBins;
    DWORD          dwInputMixBin;
};

struct XBOX_DSSTREAMDESC {
    DWORD          dwFlags;
    DWORD          dwMaxAttachedPackets;
    WAVEFORMATEX*  lpwfxFormat;
    void*          lpfnCallback;
    void*          lpvContext;
    void*          lpMixBins;
};

struct XBOX_XMEDIAPACKET {
    void*    pvBuffer;
    DWORD    dwMaxSize;
    DWORD*   pdwCompletedSize;
    DWORD*   pdwStatus;
    void*    hCompletionEvent;
    int64_t* prtTimestamp;
};

struct XBOX_DS3DBUFFER {
    DWORD dwSize;
    float vPosition[3];
    float vVelocity[3];
    DWORD dwInsideConeAngle;
    DWORD dwOutsideConeAngle;
    float vConeOrientation[3];
    LONG  lConeOutsideVolume;
    float flMinDistance;
    float flMaxDistance;
    DWORD dwMode; // 0=NORMAL, 1=HEADRELATIVE, 2=DISABLE
};

static constexpr DWORD XMEDIAPACKET_STATUS_SUCCESS = 0x00000000u;
static constexpr DWORD XMEDIAPACKET_STATUS_PENDING = 0x8000000Au;
static constexpr DWORD XBOX_DSBPLAY_LOOPING        = 0x00000001u;
static constexpr DWORD XBOX_DSBSTATUS_PLAYING      = 0x00000001u;
static constexpr DWORD XBOX_DSBSTATUS_LOOPING      = 0x00000004u;
static constexpr DWORD XBOX_DSBCAPS_CTRL3D         = 0x00000010u;
static constexpr uint32_t kOutRate                 = 48000;
static constexpr uint32_t kMixFrames               = 480; // 10ms @ 48 kHz
static constexpr uint32_t kNumWaveBufs             = 4;

// ============================================================================
// Xbox IMA-ADPCM Decoder (wFormatTag == 0x0069: 36 bytes/ch -> 64 samples/ch)
// ============================================================================
static const int16_t kImaStepTable[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static const int8_t kImaIndexTable[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

static inline int16_t DecodeImaNibble(uint8_t nibble, int32_t& pred, int32_t& stepIdx) {
    int32_t step = kImaStepTable[stepIdx];
    int32_t diff = step >> 3;
    if (nibble & 1) diff += step >> 2;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 4) diff += step;
    if (nibble & 8) pred -= diff;
    else            pred += diff;
    if (pred > 32767)  pred = 32767;
    if (pred < -32768) pred = -32768;
    stepIdx += kImaIndexTable[nibble & 15];
    if (stepIdx < 0)  stepIdx = 0;
    if (stepIdx > 88) stepIdx = 88;
    return static_cast<int16_t>(pred);
}

static void DecodeXboxAdpcm(const uint8_t* src, uint32_t srcBytes, uint32_t channels, std::vector<int16_t>& outPcm) {
    if (channels < 1 || channels > 2) channels = 1;
    const uint32_t blockSize = 36u * channels;
    const uint32_t numBlocks = srcBytes / blockSize;
    outPcm.clear();
    outPcm.reserve(numBlocks * 65u * channels);

    for (uint32_t b = 0; b < numBlocks; ++b) {
        const uint8_t* blk = src + b * blockSize;
        int32_t pred[2]{};
        int32_t idx[2]{};
        for (uint32_t c = 0; c < channels; ++c) {
            const uint8_t* hdr = blk + c * 4;
            pred[c] = static_cast<int16_t>(hdr[0] | (hdr[1] << 8));
            idx[c]  = static_cast<int32_t>(hdr[2]);
            if (idx[c] < 0) idx[c] = 0;
            if (idx[c] > 88) idx[c] = 88;
        }
        // First sample is the block header predictor
        for (uint32_t c = 0; c < channels; ++c) {
            outPcm.push_back(static_cast<int16_t>(pred[c]));
        }
        // 8 sub-blocks of 4 bytes per channel (8 nibbles = 8 samples per channel)
        const uint8_t* data = blk + 4u * channels;
        for (uint32_t sub = 0; sub < 8; ++sub) {
            int16_t tmp[2][8]{};
            for (uint32_t c = 0; c < channels; ++c) {
                const uint8_t* p = data + (sub * channels + c) * 4;
                for (uint32_t i = 0; i < 4; ++i) {
                    tmp[c][i * 2 + 0] = DecodeImaNibble(p[i] & 0x0Fu, pred[c], idx[c]);
                    tmp[c][i * 2 + 1] = DecodeImaNibble((p[i] >> 4) & 0x0Fu, pred[c], idx[c]);
                }
            }
            for (uint32_t s = 0; s < 8; ++s) {
                for (uint32_t c = 0; c < channels; ++c) {
                    outPcm.push_back(tmp[c][s]);
                }
            }
        }
    }
}

static float MillibelsToGain(LONG mb) {
    if (mb <= -9999) return 0.0f;
    if (mb >= 0)     return 1.0f;
    return std::pow(10.0f, static_cast<float>(mb) / 2000.0f);
}

// ============================================================================
// Virtual DirectSound Objects
// ============================================================================
struct VirtualSoundBuffer {
    uint32_t             magic      = 0x42534458u; // 'XDSB'
    LONG                 refCount   = 1;
    uint32_t             channels   = 1;
    uint32_t             sampleRate = 22050;
    uint32_t             origRate   = 22050;
    uint16_t             formatTag  = 1;
    uint16_t             bitsPerSamp= 16;
    uint32_t             origBytes  = 0;
    bool                 is3D       = false;
    bool                 playing    = false;
    bool                 looping    = false;
    double               playFrame  = 0.0; // Current position in decoded PCM frames
    float                gain       = 1.0f;
    // 3D parameters
    float                pos[3]     = {0.0f, 0.0f, 0.0f};
    float                minDist    = 1.0f;
    float                maxDist    = 1000.0f;
    DWORD                mode3D     = 0; // 0=NORMAL, 1=HEADRELATIVE, 2=DISABLE
    std::vector<int16_t> pcm;            // Decoded interleaved 16-bit PCM
};

struct QueuedStreamPacket {
    std::vector<int16_t> pcm;
    uint32_t             framePos = 0;
    DWORD*               pdwStatus = nullptr;
    DWORD*               pdwCompletedSize = nullptr;
    DWORD                origBytes = 0;
    HANDLE               hEvent = nullptr;
};

struct VirtualSoundStream {
    void**                        vtable     = nullptr; // Xbox XMediaObject COM vtable!
    uint32_t                      magic      = 0x53534458u; // 'XDSS'
    LONG                          refCount   = 1;
    uint32_t                      channels   = 2;
    uint32_t                      sampleRate = 44100;
    bool                          paused     = false;
    float                         gain       = 1.0f;
    double                        fracFrame  = 0.0;
    std::deque<QueuedStreamPacket> queue;
};

// ============================================================================
// Global Audio Mixer State
// ============================================================================
static std::mutex                          g_audioMtx;
static std::unordered_set<VirtualSoundBuffer*> g_buffers;
static std::unordered_set<VirtualSoundStream*> g_streams;
static uint32_t                            g_fakeDSObject = 0x44533858u; // 'X8SD'

// 3D Listener state
static float g_listPos[3]     = {0.0f, 0.0f, 0.0f};
static float g_listFront[3]   = {0.0f, 0.0f, 1.0f};
static float g_listTop[3]     = {0.0f, 1.0f, 0.0f};
static float g_listRight[3]   = {1.0f, 0.0f, 0.0f};
static float g_distFactor     = 1.0f;
static float g_rolloffFactor  = 1.0f;

static HWAVEOUT          g_hWaveOut = nullptr;
static HANDLE            g_hMixThread = nullptr;
static HANDLE            g_hWaveEvent = nullptr;
static std::atomic<bool> g_mixRunning{false};
static std::atomic<bool> g_muted{false};
static std::atomic<int>  g_masterVolPct{100};
static std::atomic<bool> g_muteInBg{false};

static bool IsAudioAppFocused() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static void UpdateListenerRight() {
    // Left-handed cross product: right = top x front
    float rx = g_listTop[1] * g_listFront[2] - g_listTop[2] * g_listFront[1];
    float ry = g_listTop[2] * g_listFront[0] - g_listTop[0] * g_listFront[2];
    float rz = g_listTop[0] * g_listFront[1] - g_listTop[1] * g_listFront[0];
    float len = std::sqrt(rx * rx + ry * ry + rz * rz);
    if (len > 1e-5f) {
        g_listRight[0] = rx / len;
        g_listRight[1] = ry / len;
        g_listRight[2] = rz / len;
    }
}

static void MixAudioBlock(int16_t* outStereo, uint32_t numFrames) {
    float mixL[kMixFrames]{};
    float mixR[kMixFrames]{};

    bool audible = !g_muted.load() && (!g_muteInBg.load() || IsAudioAppFocused());
    float masterGain = std::max(0.0f, std::min(1.0f, static_cast<float>(g_masterVolPct.load()) * 0.01f));
    if (audible && masterGain > 0.0001f) {
        std::lock_guard<std::mutex> lk(g_audioMtx);

        // 1. Mix all playing 2D & 3D sound buffers (SFX + Voice Dialogue)
        for (VirtualSoundBuffer* b : g_buffers) {
            if (!b || !b->playing || b->pcm.empty() || b->gain <= 0.0001f) continue;
            uint32_t totalFrames = static_cast<uint32_t>(b->pcm.size() / b->channels);
            if (totalFrames == 0) continue;

            float gL = b->gain;
            float gR = b->gain;
            if (b->is3D && b->mode3D != 2) {
                float dx = (b->mode3D == 1) ? b->pos[0] : (b->pos[0] - g_listPos[0]);
                float dy = (b->mode3D == 1) ? b->pos[1] : (b->pos[1] - g_listPos[1]);
                float dz = (b->mode3D == 1) ? b->pos[2] : (b->pos[2] - g_listPos[2]);
                float dist = std::sqrt(dx * dx + dy * dy + dz * dz) * g_distFactor;
                float minD = std::max(0.1f, b->minDist * g_distFactor);
                float maxD = std::max(minD + 0.1f, b->maxDist * g_distFactor);
                float atten = 1.0f;
                if (dist > maxD) {
                    atten = 0.0f;
                } else if (dist > minD) {
                    float roll = std::max(0.1f, g_rolloffFactor);
                    atten = minD / (minD + roll * (dist - minD));
                }
                float pan = 0.0f;
                if (dist > 1e-4f) {
                    float invD = 1.0f / std::sqrt(dx * dx + dy * dy + dz * dz);
                    pan = (dx * g_listRight[0] + dy * g_listRight[1] + dz * g_listRight[2]) * invD;
                    pan = std::max(-1.0f, std::min(1.0f, pan));
                }
                gL *= atten * std::sqrt(0.5f * (1.0f - pan));
                gR *= atten * std::sqrt(0.5f * (1.0f + pan));
            }
            if (gL <= 0.0001f && gR <= 0.0001f) {
                // Still advance play cursor even when attenuated out of range
                double step = static_cast<double>(b->sampleRate) / static_cast<double>(kOutRate);
                b->playFrame += step * numFrames;
                if (b->playFrame >= totalFrames) {
                    if (b->looping) b->playFrame = std::fmod(b->playFrame, static_cast<double>(totalFrames));
                    else { b->playFrame = 0.0; b->playing = false; }
                }
                continue;
            }

            double step = static_cast<double>(b->sampleRate) / static_cast<double>(kOutRate);
            for (uint32_t i = 0; i < numFrames; ++i) {
                if (b->playFrame >= totalFrames) {
                    if (b->looping) {
                        b->playFrame = std::fmod(b->playFrame, static_cast<double>(totalFrames));
                    } else {
                        b->playFrame = 0.0;
                        b->playing = false;
                        break;
                    }
                }
                uint32_t idx0 = static_cast<uint32_t>(b->playFrame);
                uint32_t idx1 = (idx0 + 1 < totalFrames) ? (idx0 + 1) : (b->looping ? 0 : idx0);
                float frac = static_cast<float>(b->playFrame - static_cast<double>(idx0));
                if (b->channels == 1) {
                    float s0 = static_cast<float>(b->pcm[idx0]) * (1.0f / 32768.0f);
                    float s1 = static_cast<float>(b->pcm[idx1]) * (1.0f / 32768.0f);
                    float s  = s0 + (s1 - s0) * frac;
                    mixL[i] += s * gL;
                    mixR[i] += s * gR;
                } else {
                    float l0 = static_cast<float>(b->pcm[idx0 * 2 + 0]) * (1.0f / 32768.0f);
                    float l1 = static_cast<float>(b->pcm[idx1 * 2 + 0]) * (1.0f / 32768.0f);
                    float r0 = static_cast<float>(b->pcm[idx0 * 2 + 1]) * (1.0f / 32768.0f);
                    float r1 = static_cast<float>(b->pcm[idx1 * 2 + 1]) * (1.0f / 32768.0f);
                    mixL[i] += (l0 + (l1 - l0) * frac) * gL;
                    mixR[i] += (r0 + (r1 - r0) * frac) * gR;
                }
                b->playFrame += step;
            }
        }

        // 2. Mix all active streaming buffers (WMA background music!)
        for (VirtualSoundStream* st : g_streams) {
            if (!st || st->paused || st->queue.empty()) continue;
            double step = static_cast<double>(st->sampleRate) / static_cast<double>(kOutRate);
            float g = st->gain;
            for (uint32_t i = 0; i < numFrames; ++i) {
                while (!st->queue.empty()) {
                    auto& pkt = st->queue.front();
                    uint32_t pktFrames = static_cast<uint32_t>(pkt.pcm.size() / st->channels);
                    if (pkt.framePos >= pktFrames) {
                        if (pkt.pdwCompletedSize) *pkt.pdwCompletedSize = pkt.origBytes;
                        if (pkt.pdwStatus)        *pkt.pdwStatus        = XMEDIAPACKET_STATUS_SUCCESS;
                        if (pkt.hEvent)           SetEvent(pkt.hEvent);
                        st->queue.pop_front();
                    } else {
                        break;
                    }
                }
                if (st->queue.empty()) break;
                auto& pkt = st->queue.front();
                uint32_t idx = pkt.framePos;
                if (st->channels == 1) {
                    float s = static_cast<float>(pkt.pcm[idx]) * (1.0f / 32768.0f) * g;
                    mixL[i] += s;
                    mixR[i] += s;
                } else {
                    mixL[i] += static_cast<float>(pkt.pcm[idx * 2 + 0]) * (1.0f / 32768.0f) * g;
                    mixR[i] += static_cast<float>(pkt.pcm[idx * 2 + 1]) * (1.0f / 32768.0f) * g;
                }
                st->fracFrame += step;
                uint32_t adv = static_cast<uint32_t>(st->fracFrame);
                if (adv > 0) {
                    pkt.framePos += adv;
                    st->fracFrame -= static_cast<double>(adv);
                }
            }
        }
    }

    // Soft-clip master mix to 16-bit stereo PCM
    for (uint32_t i = 0; i < numFrames; ++i) {
        float l = std::tanh(mixL[i] * masterGain);
        float r = std::tanh(mixR[i] * masterGain);
        outStereo[i * 2 + 0] = static_cast<int16_t>(std::max(-32767.0f, std::min(32767.0f, l * 32767.0f)));
        outStereo[i * 2 + 1] = static_cast<int16_t>(std::max(-32767.0f, std::min(32767.0f, r * 32767.0f)));
    }
}

static DWORD WINAPI MixerThreadProc(LPVOID) {
    WAVEHDR hdrs[kNumWaveBufs]{};
    int16_t bufs[kNumWaveBufs][kMixFrames * 2]{};

    for (uint32_t i = 0; i < kNumWaveBufs; ++i) {
        hdrs[i].lpData         = reinterpret_cast<LPSTR>(bufs[i]);
        hdrs[i].dwBufferLength = sizeof(bufs[i]);
        waveOutPrepareHeader(g_hWaveOut, &hdrs[i], sizeof(WAVEHDR));
        MixAudioBlock(bufs[i], kMixFrames);
        waveOutWrite(g_hWaveOut, &hdrs[i], sizeof(WAVEHDR));
    }

    while (g_mixRunning.load()) {
        WaitForSingleObject(g_hWaveEvent, 25);
        if (!g_mixRunning.load()) break;
        for (uint32_t i = 0; i < kNumWaveBufs; ++i) {
            if (hdrs[i].dwFlags & WHDR_DONE) {
                MixAudioBlock(bufs[i], kMixFrames);
                waveOutWrite(g_hWaveOut, &hdrs[i], sizeof(WAVEHDR));
            }
        }
    }

    waveOutReset(g_hWaveOut);
    for (uint32_t i = 0; i < kNumWaveBufs; ++i) {
        waveOutUnprepareHeader(g_hWaveOut, &hdrs[i], sizeof(WAVEHDR));
    }
    return 0;
}

static bool EnsureWaveOutStarted() {
    if (g_hWaveOut) return true;
    g_hWaveEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    WAVEFORMATEX wfx{};
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = kOutRate;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = 4;
    wfx.nAvgBytesPerSec = kOutRate * 4;
    MMRESULT mr = waveOutOpen(&g_hWaveOut, WAVE_MAPPER, &wfx,
                              reinterpret_cast<DWORD_PTR>(g_hWaveEvent), 0, CALLBACK_EVENT);
    if (mr != MMSYSERR_NOERROR || !g_hWaveOut) {
        LogPrintf("[Audio] waveOutOpen failed (err=%u) — running silently.\n", static_cast<unsigned>(mr));
        g_hWaveOut = nullptr;
        return false;
    }
    g_mixRunning.store(true);
    g_hMixThread = CreateThread(nullptr, 0, MixerThreadProc, nullptr, 0, nullptr);
    if (g_hMixThread) SetThreadPriority(g_hMixThread, THREAD_PRIORITY_HIGHEST);
    LogPrintf("[Audio] Windows waveOut 48 kHz Stereo Software Mixer initialized (PCM + Xbox ADPCM + WMA Music)!\n");
    return true;
}

// ============================================================================
// IDirectSoundStream COM Vtable Implementation (Xbox XMediaObject)
// ============================================================================
static ULONG __stdcall Stream_AddRef(VirtualSoundStream* st) {
    return st ? static_cast<ULONG>(InterlockedIncrement(&st->refCount)) : 0;
}

static ULONG __stdcall Stream_Release(VirtualSoundStream* st) {
    if (!st) return 0;
    LONG rc = InterlockedDecrement(&st->refCount);
    if (rc <= 0) {
        std::lock_guard<std::mutex> lk(g_audioMtx);
        for (auto& pkt : st->queue) {
            if (pkt.pdwStatus) *pkt.pdwStatus = XMEDIAPACKET_STATUS_SUCCESS;
            if (pkt.hEvent) SetEvent(pkt.hEvent);
        }
        g_streams.erase(st);
        delete st;
        return 0;
    }
    return static_cast<ULONG>(rc);
}

static HRESULT __stdcall Stream_GetInfo(VirtualSoundStream* /*st*/, void* pInfo) {
    if (pInfo) std::memset(pInfo, 0, 16);
    return S_OK;
}

static HRESULT __stdcall Stream_GetStatus(VirtualSoundStream* st, DWORD* pdwStatus) {
    if (pdwStatus) *pdwStatus = (st && st->queue.size() < 8) ? 1u : 0u; // XMO_STATUSF_ACCEPT_INPUT_DATA
    return S_OK;
}

static HRESULT __stdcall Stream_Process(VirtualSoundStream* st, const XBOX_XMEDIAPACKET* pIn, const XBOX_XMEDIAPACKET* /*pOut*/) {
    if (!st || !pIn || !pIn->pvBuffer || pIn->dwMaxSize == 0) return E_INVALIDARG;
    QueuedStreamPacket pkt{};
    pkt.origBytes        = pIn->dwMaxSize;
    pkt.pdwStatus        = pIn->pdwStatus;
    pkt.pdwCompletedSize = pIn->pdwCompletedSize;
    pkt.hEvent           = pIn->hCompletionEvent;
    uint32_t numSamples  = pIn->dwMaxSize / 2;
    pkt.pcm.resize(numSamples);
    std::memcpy(pkt.pcm.data(), pIn->pvBuffer, numSamples * 2);
    if (pkt.pdwStatus) *pkt.pdwStatus = XMEDIAPACKET_STATUS_PENDING;
    {
        std::lock_guard<std::mutex> lk(g_audioMtx);
        st->queue.push_back(std::move(pkt));
    }
    return S_OK;
}

static HRESULT __stdcall Stream_Discontinuity(VirtualSoundStream* /*st*/) { return S_OK; }

static HRESULT __stdcall Stream_Flush(VirtualSoundStream* st) {
    if (!st) return S_OK;
    std::lock_guard<std::mutex> lk(g_audioMtx);
    for (auto& pkt : st->queue) {
        if (pkt.pdwStatus) *pkt.pdwStatus = XMEDIAPACKET_STATUS_SUCCESS;
        if (pkt.hEvent) SetEvent(pkt.hEvent);
    }
    st->queue.clear();
    return S_OK;
}

static void* g_streamVtable[8] = {
    reinterpret_cast<void*>(&Stream_AddRef),        // +0x00
    reinterpret_cast<void*>(&Stream_Release),       // +0x04
    reinterpret_cast<void*>(&Stream_GetInfo),       // +0x08
    reinterpret_cast<void*>(&Stream_GetStatus),     // +0x0C
    reinterpret_cast<void*>(&Stream_Process),       // +0x10
    reinterpret_cast<void*>(&Stream_Discontinuity), // +0x14
    reinterpret_cast<void*>(&Stream_Flush),         // +0x18
    nullptr
};

// ============================================================================
// Detoured Xbox DirectSound8 Flat C APIs (0x005A6D13..0x005A8D4E)
// ============================================================================
static HRESULT __stdcall Detour_DirectSoundCreate(void* /*pGuid*/, void** ppDS, void* /*pUnk*/) {
    EnsureWaveOutStarted();
    if (ppDS) *ppDS = &g_fakeDSObject;
    return S_OK;
}

static void __stdcall Detour_DirectSoundUseFullHRTF() {}
static void __stdcall Detour_DirectSoundDoWork() {}
static ULONG __stdcall Detour_IDirectSound_Release(void* /*pDS*/) { return 0; }
static HRESULT __stdcall Detour_IDirectSound_SetCooperativeLevel(void*, HWND, DWORD) { return S_OK; }
static HRESULT __stdcall Detour_IDirectSound_DownloadEffectsImage(void*, const void*, DWORD, void*, void**) { return S_OK; }
static HRESULT __stdcall Detour_IDirectSound_EnableHeadphones(void*, BOOL) { return S_OK; }
static HRESULT __stdcall Detour_IDirectSound_CommitDeferredSettings(void*) { return S_OK; }
static HRESULT __stdcall Detour_IDirectSound_SetI3DL2Listener(void*, const void*, DWORD) { return S_OK; }

static HRESULT __stdcall Detour_IDirectSound_SetDistanceFactor(void*, float f, DWORD) {
    std::lock_guard<std::mutex> lk(g_audioMtx);
    g_distFactor = (f > 0.0f) ? f : 1.0f;
    return S_OK;
}
static HRESULT __stdcall Detour_IDirectSound_SetDopplerFactor(void*, float, DWORD) { return S_OK; }
static HRESULT __stdcall Detour_IDirectSound_SetRolloffFactor(void*, float f, DWORD) {
    std::lock_guard<std::mutex> lk(g_audioMtx);
    g_rolloffFactor = (f >= 0.0f) ? f : 1.0f;
    return S_OK;
}
static HRESULT __stdcall Detour_IDirectSound_SetVelocity(void*, float, float, float, DWORD) { return S_OK; }

static HRESULT __stdcall Detour_IDirectSound_SetPosition(void*, float x, float y, float z, DWORD) {
    std::lock_guard<std::mutex> lk(g_audioMtx);
    g_listPos[0] = x; g_listPos[1] = y; g_listPos[2] = z;
    return S_OK;
}

static HRESULT __stdcall Detour_IDirectSound_SetOrientation(void*, float fx, float fy, float fz, float tx, float ty, float tz, DWORD) {
    std::lock_guard<std::mutex> lk(g_audioMtx);
    g_listFront[0] = fx; g_listFront[1] = fy; g_listFront[2] = fz;
    g_listTop[0]   = tx; g_listTop[1]   = ty; g_listTop[2]   = tz;
    UpdateListenerRight();
    return S_OK;
}

static HRESULT __stdcall Detour_IDirectSound_CreateSoundBuffer(void* /*pDS*/, const XBOX_DSBUFFERDESC* pDesc, void** ppBuf, void* /*pUnk*/) {
    if (!ppBuf) return E_POINTER;
    auto* b = new VirtualSoundBuffer();
    if (pDesc) {
        b->is3D = (pDesc->dwFlags & XBOX_DSBCAPS_CTRL3D) != 0;
        if (pDesc->lpwfxFormat) {
            b->formatTag   = pDesc->lpwfxFormat->wFormatTag;
            b->channels    = std::max<uint32_t>(1, std::min<uint32_t>(2, pDesc->lpwfxFormat->nChannels));
            b->sampleRate  = pDesc->lpwfxFormat->nSamplesPerSec ? pDesc->lpwfxFormat->nSamplesPerSec : 22050;
            b->origRate    = b->sampleRate;
            b->bitsPerSamp = pDesc->lpwfxFormat->wBitsPerSample ? pDesc->lpwfxFormat->wBitsPerSample : 16;
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_audioMtx);
        g_buffers.insert(b);
    }
    *ppBuf = b;
    return S_OK;
}

static ULONG __stdcall Detour_IDirectSoundBuffer_Release(void* pBuf) {
    if (!pBuf) return 0;
    auto* b = reinterpret_cast<VirtualSoundBuffer*>(pBuf);
    if (b->magic != 0x42534458u) return 0;
    LONG rc = InterlockedDecrement(&b->refCount);
    if (rc <= 0) {
        std::lock_guard<std::mutex> lk(g_audioMtx);
        g_buffers.erase(b);
        delete b;
        return 0;
    }
    return static_cast<ULONG>(rc);
}

static HRESULT __stdcall Detour_IDirectSoundBuffer_SetBufferData(void* pBuf, const void* pvData, DWORD dwBytes) {
    if (!pBuf) return E_INVALIDARG;
    auto* b = reinterpret_cast<VirtualSoundBuffer*>(pBuf);
    if (b->magic != 0x42534458u) return E_INVALIDARG;

    std::vector<int16_t> decoded;
    if (pvData && dwBytes > 0 && !IsBadReadPtr(pvData, dwBytes)) {
        const uint8_t* src = static_cast<const uint8_t*>(pvData);
        if (b->formatTag == 0x0069) {
            DecodeXboxAdpcm(src, dwBytes, b->channels, decoded);
        } else if (b->bitsPerSamp == 8) {
            decoded.resize(dwBytes);
            for (DWORD i = 0; i < dwBytes; ++i) {
                decoded[i] = static_cast<int16_t>((static_cast<int32_t>(src[i]) - 128) << 8);
            }
        } else {
            uint32_t n = dwBytes / 2;
            decoded.resize(n);
            std::memcpy(decoded.data(), src, n * 2);
        }
    }
    std::lock_guard<std::mutex> lk(g_audioMtx);
    b->origBytes = dwBytes;
    b->pcm       = std::move(decoded);
    b->playFrame = 0.0;
    return S_OK;
}

static HRESULT __stdcall Detour_IDirectSoundBuffer_Play(void* pBuf, DWORD /*dwRes1*/, DWORD /*dwRes2*/, DWORD dwFlags) {
    if (!pBuf) return E_INVALIDARG;
    auto* b = reinterpret_cast<VirtualSoundBuffer*>(pBuf);
    if (b->magic != 0x42534458u) return E_INVALIDARG;
    std::lock_guard<std::mutex> lk(g_audioMtx);
    b->looping = (dwFlags & XBOX_DSBPLAY_LOOPING) != 0;
    b->playing = !b->pcm.empty();
    return S_OK;
}

static HRESULT __stdcall Detour_IDirectSoundBuffer_Stop(void* pBuf) {
    if (!pBuf) return E_INVALIDARG;
    auto* b = reinterpret_cast<VirtualSoundBuffer*>(pBuf);
    if (b->magic != 0x42534458u) return E_INVALIDARG;
    std::lock_guard<std::mutex> lk(g_audioMtx);
    b->playing   = false;
    b->playFrame = 0.0;
    return S_OK;
}

static HRESULT __stdcall Detour_IDirectSoundBuffer_GetStatus(void* pBuf, DWORD* pdwStatus) {
    if (!pdwStatus) return E_POINTER;
    *pdwStatus = 0;
    if (!pBuf) return S_OK;
    auto* b = reinterpret_cast<VirtualSoundBuffer*>(pBuf);
    if (b->magic != 0x42534458u) return S_OK;
    std::lock_guard<std::mutex> lk(g_audioMtx);
    if (b->playing) {
        *pdwStatus = XBOX_DSBSTATUS_PLAYING | (b->looping ? XBOX_DSBSTATUS_LOOPING : 0u);
    }
    return S_OK;
}

static HRESULT __stdcall Detour_IDirectSoundBuffer_GetCurrentPosition(void* pBuf, DWORD* pdwPlayCursor, DWORD* pdwWriteCursor) {
    DWORD cur = 0;
    if (pBuf) {
        auto* b = reinterpret_cast<VirtualSoundBuffer*>(pBuf);
        if (b->magic == 0x42534458u) {
            std::lock_guard<std::mutex> lk(g_audioMtx);
            uint32_t totalFrames = b->channels ? static_cast<uint32_t>(b->pcm.size() / b->channels) : 0;
            if (totalFrames > 0 && b->origBytes > 0) {
                double frac = b->playFrame / static_cast<double>(totalFrames);
                cur = static_cast<DWORD>(frac * static_cast<double>(b->origBytes));
            }
        }
    }
    if (pdwPlayCursor)  *pdwPlayCursor  = cur;
    if (pdwWriteCursor) *pdwWriteCursor = cur;
    return S_OK;
}

static HRESULT __stdcall Detour_IDirectSoundBuffer_SetVolume(void* pBuf, LONG lVolume) {
    if (!pBuf) return S_OK;
    auto* b = reinterpret_cast<VirtualSoundBuffer*>(pBuf);
    if (b->magic != 0x42534458u) return S_OK;
    std::lock_guard<std::mutex> lk(g_audioMtx);
    b->gain = MillibelsToGain(lVolume);
    return S_OK;
}

static HRESULT __stdcall Detour_IDirectSoundBuffer_SetHeadroom(void*, DWORD) { return S_OK; }

static HRESULT __stdcall Detour_IDirectSoundBuffer_SetFrequency(void* pBuf, DWORD dwFreq) {
    if (!pBuf) return S_OK;
    auto* b = reinterpret_cast<VirtualSoundBuffer*>(pBuf);
    if (b->magic != 0x42534458u) return S_OK;
    std::lock_guard<std::mutex> lk(g_audioMtx);
    b->sampleRate = (dwFreq >= 1000 && dwFreq <= 96000) ? dwFreq : b->origRate;
    return S_OK;
}

static HRESULT __stdcall Detour_IDirectSoundBuffer_SetAllParameters(void* pBuf, const XBOX_DS3DBUFFER* pParams, DWORD) {
    if (!pBuf || !pParams) return S_OK;
    auto* b = reinterpret_cast<VirtualSoundBuffer*>(pBuf);
    if (b->magic != 0x42534458u) return S_OK;
    std::lock_guard<std::mutex> lk(g_audioMtx);
    b->pos[0]  = pParams->vPosition[0];
    b->pos[1]  = pParams->vPosition[1];
    b->pos[2]  = pParams->vPosition[2];
    b->minDist = pParams->flMinDistance;
    b->maxDist = pParams->flMaxDistance;
    b->mode3D  = pParams->dwMode;
    return S_OK;
}

static HRESULT __stdcall Detour_IDirectSoundBuffer_SetI3DL2Source(void*, const void*, DWORD) { return S_OK; }

static HRESULT __stdcall Detour_DirectSoundCreateStream(const XBOX_DSSTREAMDESC* pDesc, void** ppStream) {
    if (!ppStream) return E_POINTER;
    EnsureWaveOutStarted();
    auto* st = new VirtualSoundStream();
    st->vtable = g_streamVtable;
    if (pDesc && pDesc->lpwfxFormat) {
        st->channels   = std::max<uint32_t>(1, std::min<uint32_t>(2, pDesc->lpwfxFormat->nChannels));
        st->sampleRate = pDesc->lpwfxFormat->nSamplesPerSec ? pDesc->lpwfxFormat->nSamplesPerSec : 44100;
    }
    {
        std::lock_guard<std::mutex> lk(g_audioMtx);
        g_streams.insert(st);
    }
    *ppStream = st;
    return S_OK;
}

static HRESULT __stdcall Detour_IDirectSoundStream_SetVolume(void* pStream, LONG lVolume) {
    if (!pStream) return S_OK;
    auto* st = reinterpret_cast<VirtualSoundStream*>(pStream);
    if (st->magic != 0x53534458u) return S_OK;
    std::lock_guard<std::mutex> lk(g_audioMtx);
    st->gain = MillibelsToGain(lVolume);
    return S_OK;
}

static HRESULT __stdcall Detour_IDirectSoundStream_Pause(void* pStream, DWORD dwPause) {
    if (!pStream) return S_OK;
    auto* st = reinterpret_cast<VirtualSoundStream*>(pStream);
    if (st->magic != 0x53534458u) return S_OK;
    std::lock_guard<std::mutex> lk(g_audioMtx);
    st->paused = (dwPause != 0);
    return S_OK;
}

static void PatchJump(uint32_t xboxVa, void* targetFn) {
    uint8_t* p = reinterpret_cast<uint8_t*>(xboxVa);
    int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(targetFn) - (xboxVa + 5));
    p[0] = 0xE9;
    *reinterpret_cast<int32_t*>(p + 1) = rel;
}

static LONG __cdecl Detour_DownloadEffectsImage(const char* /*pszScratchImage*/) {
    return 0; // S_OK (bypass Xbox MCPX DSP firmware download so TSoundManager::Init succeeds cleanly)
}

void InstallDetours() {
    PatchJump(0x00067320, &Detour_DownloadEffectsImage);
    PatchJump(0x005A8D07, &Detour_DirectSoundCreate);
    PatchJump(0x005A6D44, &Detour_DirectSoundUseFullHRTF);
    PatchJump(0x005A7835, &Detour_DirectSoundDoWork);
    PatchJump(0x005A6D13, &Detour_IDirectSound_SetCooperativeLevel);
    PatchJump(0x005A6D18, &Detour_IDirectSound_Release);
    PatchJump(0x005A7738, &Detour_IDirectSound_DownloadEffectsImage);
    PatchJump(0x005A7E50, &Detour_IDirectSound_EnableHeadphones);
    PatchJump(0x005A7E6C, &Detour_IDirectSound_CommitDeferredSettings);
    PatchJump(0x005A86F7, &Detour_IDirectSound_SetDistanceFactor);
    PatchJump(0x005A871B, &Detour_IDirectSound_SetDopplerFactor);
    PatchJump(0x005A873F, &Detour_IDirectSound_SetOrientation);
    PatchJump(0x005A8789, &Detour_IDirectSound_SetPosition);
    PatchJump(0x005A87BE, &Detour_IDirectSound_SetRolloffFactor);
    PatchJump(0x005A87E2, &Detour_IDirectSound_SetVelocity);
    PatchJump(0x005A8817, &Detour_IDirectSound_SetI3DL2Listener);
    PatchJump(0x005A8B4D, &Detour_IDirectSound_CreateSoundBuffer);

    PatchJump(0x005A6D2E, &Detour_IDirectSoundBuffer_Release);
    PatchJump(0x005A8837, &Detour_IDirectSoundBuffer_SetBufferData);
    PatchJump(0x005A77B3, &Detour_IDirectSoundBuffer_Play);
    PatchJump(0x005A77D7, &Detour_IDirectSoundBuffer_Stop);
    PatchJump(0x005A77EF, &Detour_IDirectSoundBuffer_GetStatus);
    PatchJump(0x005A780B, &Detour_IDirectSoundBuffer_GetCurrentPosition);
    PatchJump(0x005A777B, &Detour_IDirectSoundBuffer_SetVolume);
    PatchJump(0x005A7797, &Detour_IDirectSoundBuffer_SetHeadroom);
    PatchJump(0x005A7E84, &Detour_IDirectSoundBuffer_SetFrequency);
    PatchJump(0x005A7EA0, &Detour_IDirectSoundBuffer_SetAllParameters);
    PatchJump(0x005A7EC0, &Detour_IDirectSoundBuffer_SetI3DL2Source);

    PatchJump(0x005A8D4E, &Detour_DirectSoundCreateStream);
    PatchJump(0x005A782B, &Detour_IDirectSoundStream_SetVolume);
    PatchJump(0x005A7830, &Detour_IDirectSoundStream_Pause);
    LogPrintf("[Audio] Installed 30 Xbox DirectSound8 -> Windows waveOut mixer detours.\n");
}

void Shutdown() {
    if (g_mixRunning.exchange(false)) {
        if (g_hWaveEvent) SetEvent(g_hWaveEvent);
        if (g_hMixThread) {
            WaitForSingleObject(g_hMixThread, 500);
            CloseHandle(g_hMixThread);
            g_hMixThread = nullptr;
        }
    }
    if (g_hWaveOut) {
        waveOutClose(g_hWaveOut);
        g_hWaveOut = nullptr;
    }
    if (g_hWaveEvent) {
        CloseHandle(g_hWaveEvent);
        g_hWaveEvent = nullptr;
    }
}

void SetMuted(bool muted)               { g_muted.store(muted); }
bool IsMuted()                          { return g_muted.load(); }
void SetMasterVolume(int percent)       { g_masterVolPct.store(std::max(0, std::min(100, percent))); }
int  GetMasterVolume()                  { return g_masterVolPct.load(); }
void SetMuteInBackground(bool enabled)  { g_muteInBg.store(enabled); }
bool IsMuteInBackground()               { return g_muteInBg.load(); }

} // namespace dsound_bridge
