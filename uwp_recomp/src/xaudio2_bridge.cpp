#include "xaudio2_bridge.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <xaudio2.h>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

namespace uwp_recomp {
namespace xaudio2_bridge {

static IXAudio2 *g_xa2 = nullptr;
static IXAudio2MasteringVoice *g_master = nullptr;
static std::mutex g_xaMtx;

struct XaVoice {
  IXAudio2SourceVoice *voice = nullptr;
  uint16_t formatTag = 1; // 1=PCM, 0x0069=Xbox IMA-ADPCM
  uint16_t channels = 1;
  uint32_t sampleRate = 22050;
  uint16_t bitsPerSamp = 16;
  float volume = 1.0f;
  float freqRatio = 1.0f;
  bool is3D = false;
  float pos[3] = {0.0f, 0.0f, 0.0f};
  float minDist = 1.0f;
  float maxDist = 1000.0f;
  uint32_t mode3D = 0; // 0=NORMAL, 1=HEADRELATIVE, 2=DISABLE
  bool looping = false;
  bool submitted = false;
  std::vector<int16_t> pcm;
};

static float g_listPos[3] = {0.0f, 0.0f, 0.0f};
static float g_listFront[3] = {0.0f, 0.0f, 1.0f};
static float g_listTop[3] = {0.0f, 1.0f, 0.0f};
static float g_listRight[3] = {1.0f, 0.0f, 0.0f};
static float g_distFactor = 1.0f;
static float g_rolloffFactor = 1.0f;

static void UpdateListenerRight() {
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

static void ApplyVoice3D(XaVoice &v) {
  if (!v.voice || !g_master || !v.is3D)
    return;
  uint32_t ch = (v.channels >= 1 && v.channels <= 2) ? v.channels : 1;
  float gL = 1.0f, gR = 1.0f;
  if (v.mode3D != 2u) {
    float dx = (v.mode3D == 1u) ? v.pos[0] : (v.pos[0] - g_listPos[0]);
    float dy = (v.mode3D == 1u) ? v.pos[1] : (v.pos[1] - g_listPos[1]);
    float dz = (v.mode3D == 1u) ? v.pos[2] : (v.pos[2] - g_listPos[2]);
    float rawDist = std::sqrt(dx * dx + dy * dy + dz * dz);
    float dist = rawDist * g_distFactor;
    float minD = std::max(0.1f, v.minDist * g_distFactor);
    float maxD = std::max(minD + 0.1f, v.maxDist * g_distFactor);
    float atten = 1.0f;
    if (dist > maxD) {
      atten = 0.0f;
    } else if (dist > minD) {
      float roll = std::max(0.1f, g_rolloffFactor);
      atten = minD / (minD + roll * (dist - minD));
    }
    float pan = 0.0f;
    if (rawDist > 1e-4f) {
      float invD = 1.0f / rawDist;
      pan = (dx * g_listRight[0] + dy * g_listRight[1] + dz * g_listRight[2]) * invD;
      pan = std::max(-1.0f, std::min(1.0f, pan));
    }
    gL = atten * std::sqrt(0.5f * (1.0f - pan));
    gR = atten * std::sqrt(0.5f * (1.0f + pan));
  }
  if (ch == 1u) {
    float mat[2] = {gL, gR};
    v.voice->SetOutputMatrix(g_master, 1, 2, mat);
  } else {
    float mat[4] = {gL, 0.0f, 0.0f, gR};
    v.voice->SetOutputMatrix(g_master, 2, 2, mat);
  }
}

static std::unordered_map<uint32_t, XaVoice> g_voices;
static bool g_mfStarted = false;

struct WmaDecoder {
    IMFSourceReader*     reader = nullptr;
    std::vector<uint8_t> residual;
    size_t               resPos = 0;
    bool                 eof    = false;
};

struct StreamPacketCtx {
    std::vector<uint8_t>               pcm;
    uint8_t*                           guestMem       = nullptr;
    uint32_t                           pdwStatusVA    = 0;
    uint32_t                           pdwCompletedVA = 0;
    uint32_t                           origBytes      = 0;
    void*                              hEvent         = nullptr;
    std::shared_ptr<std::atomic<bool>> alive;
};

struct StreamVoiceCallback : public IXAudio2VoiceCallback {
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) override {}
    void STDMETHODCALLTYPE OnBufferEnd(void* pBufferContext) override {
        auto* pkt = static_cast<StreamPacketCtx*>(pBufferContext);
        if (!pkt) return;
        if (pkt->alive && pkt->alive->load(std::memory_order_acquire)) {
            if (pkt->guestMem && pkt->pdwCompletedVA) {
                std::memcpy(pkt->guestMem + pkt->pdwCompletedVA, &pkt->origBytes, 4);
            }
            if (pkt->guestMem && pkt->pdwStatusVA) {
                uint32_t okStatus = 0u; // XMEDIAPACKET_STATUS_SUCCESS
                std::memcpy(pkt->guestMem + pkt->pdwStatusVA, &okStatus, 4);
            }
            if (pkt->hEvent) {
                SetEvent(static_cast<HANDLE>(pkt->hEvent));
            }
        }
        delete pkt;
    }
};

struct XaStream {
    IXAudio2SourceVoice*                 voice      = nullptr;
    std::unique_ptr<StreamVoiceCallback> cb;
    std::shared_ptr<std::atomic<bool>>   alive;
    uint16_t                             channels   = 2;
    uint32_t                             sampleRate = 44100;
    float                                volume     = 1.0f;
    bool                                 paused     = false;
};

static std::unordered_map<uint32_t, WmaDecoder> g_wmaDecoders;
static std::unordered_map<uint32_t, XaStream>   g_streams;

// ============================================================================
// Xbox IMA-ADPCM Decoder (wFormatTag == 0x0069: 36 bytes/ch -> 65 samples/ch)
// ============================================================================
static const int16_t kImaStepTable[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,
    19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
    337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
    876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
    5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

static const int8_t kImaIndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8,
                                          -1, -1, -1, -1, 2, 4, 6, 8};

static inline int16_t DecodeImaNibble(uint8_t nibble, int32_t &pred,
                                      int32_t &stepIdx) {
  int32_t step = kImaStepTable[stepIdx];
  int32_t diff = step >> 3;
  if (nibble & 1)
    diff += step >> 2;
  if (nibble & 2)
    diff += step >> 1;
  if (nibble & 4)
    diff += step;
  if (nibble & 8)
    pred -= diff;
  else
    pred += diff;
  if (pred > 32767)
    pred = 32767;
  if (pred < -32768)
    pred = -32768;
  stepIdx += kImaIndexTable[nibble & 15];
  if (stepIdx < 0)
    stepIdx = 0;
  if (stepIdx > 88)
    stepIdx = 88;
  return static_cast<int16_t>(pred);
}

static void DecodeXboxAdpcm(const uint8_t *src, uint32_t srcBytes,
                            uint32_t channels, std::vector<int16_t> &outPcm) {
  if (channels < 1 || channels > 2)
    channels = 1;
  const uint32_t blockSize = 36u * channels;
  const uint32_t numBlocks = srcBytes / blockSize;
  outPcm.clear();
  outPcm.reserve(static_cast<size_t>(numBlocks) * 65u * channels);
  for (uint32_t b = 0; b < numBlocks; ++b) {
    const uint8_t *blk = src + b * blockSize;
    int32_t pred[2]{}, idx[2]{};
    for (uint32_t c = 0; c < channels; ++c) {
      const uint8_t *hdr = blk + c * 4u;
      pred[c] = static_cast<int16_t>(hdr[0] | (hdr[1] << 8));
      idx[c] = std::max(0, std::min(88, static_cast<int32_t>(hdr[2])));
    }
    for (uint32_t c = 0; c < channels; ++c)
      outPcm.push_back(static_cast<int16_t>(pred[c]));
    const uint8_t *data = blk + 4u * channels;
    for (uint32_t sub = 0; sub < 8u; ++sub) {
      int16_t tmp[2][8]{};
      for (uint32_t c = 0; c < channels; ++c) {
        const uint8_t *p = data + (sub * channels + c) * 4u;
        for (uint32_t i = 0; i < 4u; ++i) {
          tmp[c][i * 2 + 0] = DecodeImaNibble(p[i] & 0x0Fu, pred[c], idx[c]);
          tmp[c][i * 2 + 1] =
              DecodeImaNibble((p[i] >> 4) & 0x0Fu, pred[c], idx[c]);
        }
      }
      for (uint32_t s = 0; s < 8u; ++s) {
        for (uint32_t c = 0; c < channels; ++c)
          outPcm.push_back(tmp[c][s]);
      }
    }
  }
}

static bool EnsureSourceVoice(XaVoice &v) {
  if (v.voice)
    return true;
  if (!g_xa2)
    return false;
  WAVEFORMATEX wf{};
  wf.wFormatTag = WAVE_FORMAT_PCM;
  wf.nChannels = (v.channels >= 1 && v.channels <= 2) ? v.channels : 1;
  wf.nSamplesPerSec =
      (v.sampleRate >= 4000 && v.sampleRate <= 96000) ? v.sampleRate : 22050;
  wf.wBitsPerSample = 16;
  wf.nBlockAlign = wf.nChannels * 2;
  wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
  if (FAILED(g_xa2->CreateSourceVoice(&v.voice, &wf, 0, 4.0f)) || !v.voice) {
    v.voice = nullptr;
    return false;
  }
  v.voice->SetVolume(v.volume);
  v.voice->SetFrequencyRatio(v.freqRatio);
  return true;
}

bool Init() {
  std::lock_guard<std::mutex> lk(g_xaMtx);
  if (!g_mfStarted) {
    if (SUCCEEDED(MFStartup(MF_VERSION)))
      g_mfStarted = true;
  }
  if (g_xa2)
    return true;
  HRESULT hr = XAudio2Create(&g_xa2, 0, XAUDIO2_DEFAULT_PROCESSOR);
  if (FAILED(hr) || !g_xa2) {
    std::printf("[UWP-XAudio2] XAudio2Create failed (hr=0x%08lX)\n",
                static_cast<unsigned long>(hr));
    return false;
  }
  hr = g_xa2->CreateMasteringVoice(&g_master, 2, 48000);
  if (FAILED(hr) || !g_master) {
    std::printf("[UWP-XAudio2] CreateMasteringVoice failed (hr=0x%08lX)\n",
                static_cast<unsigned long>(hr));
    if (g_xa2) {
      g_xa2->Release();
      g_xa2 = nullptr;
    }
    return false;
  }
  std::printf("[UWP-XAudio2] Initialized 48 kHz Stereo Mastering Voice!\n");
  return true;
}

void Shutdown() {
  std::lock_guard<std::mutex> lk(g_xaMtx);
  for (auto &kv : g_voices) {
    if (kv.second.voice) {
      kv.second.voice->Stop(0);
      kv.second.voice->FlushSourceBuffers();
      kv.second.voice->DestroyVoice();
      kv.second.voice = nullptr;
    }
  }
  g_voices.clear();
  for (auto &kv : g_streams) {
    if (kv.second.alive)
      kv.second.alive->store(false, std::memory_order_release);
    if (kv.second.voice) {
      kv.second.voice->Stop(0);
      kv.second.voice->FlushSourceBuffers();
      kv.second.voice->DestroyVoice();
      kv.second.voice = nullptr;
    }
  }
  g_streams.clear();
  for (auto &kv : g_wmaDecoders) {
    if (kv.second.reader) {
      kv.second.reader->Release();
      kv.second.reader = nullptr;
    }
  }
  g_wmaDecoders.clear();
  if (g_master) {
    g_master->DestroyVoice();
    g_master = nullptr;
  }
  if (g_xa2) {
    g_xa2->Release();
    g_xa2 = nullptr;
  }
}

bool IsReady() { return g_xa2 != nullptr && g_master != nullptr; }

void SetMasterVolume(float volume01) {
  std::lock_guard<std::mutex> lk(g_xaMtx);
  if (g_master) {
    g_master->SetVolume(std::max(0.0f, std::min(1.0f, volume01)));
  }
}

void CreateBuffer(uint32_t handle, uint16_t formatTag, uint16_t channels,
                  uint32_t sampleRate, uint16_t bitsPerSample, bool is3D) {
  std::lock_guard<std::mutex> lk(g_xaMtx);
  auto it = g_voices.find(handle);
  if (it != g_voices.end() && it->second.voice) {
    it->second.voice->Stop(0);
    it->second.voice->FlushSourceBuffers();
    it->second.voice->DestroyVoice();
  }
  XaVoice v{};
  v.formatTag = formatTag ? formatTag : 0x0069u;
  v.channels = (channels >= 1 && channels <= 2) ? channels : 1;
  v.sampleRate =
      (sampleRate >= 4000 && sampleRate <= 96000) ? sampleRate : 22050;
  v.bitsPerSamp = bitsPerSample ? bitsPerSample : 16;
  v.is3D = is3D;
  EnsureSourceVoice(v);
  ApplyVoice3D(v);
  g_voices[handle] = std::move(v);
}

void ReleaseBuffer(uint32_t handle) {
  std::lock_guard<std::mutex> lk(g_xaMtx);
  auto it = g_voices.find(handle);
  if (it != g_voices.end()) {
    if (it->second.voice) {
      it->second.voice->Stop(0);
      it->second.voice->FlushSourceBuffers();
      it->second.voice->DestroyVoice();
    }
    g_voices.erase(it);
  }
}

void SetBufferData(uint32_t handle, const uint8_t *data, uint32_t bytes) {
  if (!data || bytes == 0)
    return;
  std::lock_guard<std::mutex> lk(g_xaMtx);
  auto &v = g_voices[handle];
  if (v.voice) {
    v.voice->Stop(0);
    v.voice->FlushSourceBuffers();
  }
  if (v.formatTag == 0x0069u) {
    DecodeXboxAdpcm(data, bytes, v.channels, v.pcm);
  } else if (v.bitsPerSamp == 8) {
    v.pcm.resize(bytes);
    for (uint32_t i = 0; i < bytes; ++i) {
      v.pcm[i] =
          static_cast<int16_t>((static_cast<int32_t>(data[i]) - 128) << 8);
    }
  } else {
    uint32_t nSamp = bytes / 2u;
    v.pcm.resize(nSamp);
    std::memcpy(v.pcm.data(), data, nSamp * 2u);
  }
  v.submitted = false;
}

void Play(uint32_t handle, bool looping) {
  std::lock_guard<std::mutex> lk(g_xaMtx);
  auto it = g_voices.find(handle);
  if (it == g_voices.end() || it->second.pcm.empty())
    return;
  auto &v = it->second;
  if (!EnsureSourceVoice(v))
    return;
  v.looping = looping;
  v.voice->Stop(0);
  v.voice->FlushSourceBuffers();
  XAUDIO2_BUFFER buf{};
  buf.Flags = XAUDIO2_END_OF_STREAM;
  buf.AudioBytes = static_cast<UINT32>(v.pcm.size() * sizeof(int16_t));
  buf.pAudioData = reinterpret_cast<const BYTE *>(v.pcm.data());
  buf.LoopCount = looping ? XAUDIO2_LOOP_INFINITE : 0;
  if (SUCCEEDED(v.voice->SubmitSourceBuffer(&buf))) {
    v.voice->SetVolume(v.volume);
    v.voice->SetFrequencyRatio(v.freqRatio);
    ApplyVoice3D(v);
    v.voice->Start(0);
    v.submitted = true;
  }
}

void Stop(uint32_t handle) {
  std::lock_guard<std::mutex> lk(g_xaMtx);
  auto it = g_voices.find(handle);
  if (it != g_voices.end() && it->second.voice) {
    it->second.voice->Stop(0);
    it->second.voice->FlushSourceBuffers();
    it->second.submitted = false;
  }
}

void SetVolume(uint32_t handle, int32_t dsVolHundredthsDb) {
  std::lock_guard<std::mutex> lk(g_xaMtx);
  auto it = g_voices.find(handle);
  if (it == g_voices.end())
    return;
  float gain = 1.0f;
  if (dsVolHundredthsDb <= -9999)
    gain = 0.0f;
  else if (dsVolHundredthsDb < 0)
    gain = std::pow(10.0f, static_cast<float>(dsVolHundredthsDb) / 2000.0f);
  it->second.volume = std::max(0.0f, std::min(1.0f, gain));
  if (it->second.voice)
    it->second.voice->SetVolume(it->second.volume);
}

void SetFrequency(uint32_t handle, uint32_t freqHz) {
  std::lock_guard<std::mutex> lk(g_xaMtx);
  auto it = g_voices.find(handle);
  if (it == g_voices.end() || freqHz == 0)
    return;
  float ratio =
      static_cast<float>(freqHz) /
      static_cast<float>(std::max<uint32_t>(4000u, it->second.sampleRate));
  it->second.freqRatio = std::max(0.05f, std::min(4.0f, ratio));
  if (it->second.voice)
    it->second.voice->SetFrequencyRatio(it->second.freqRatio);
}

void SetBuffer3DParams(uint32_t handle, float px, float py, float pz,
                       float minDist, float maxDist, uint32_t mode3D) {
  std::lock_guard<std::mutex> lk(g_xaMtx);
  auto it = g_voices.find(handle);
  if (it == g_voices.end())
    return;
  auto &v = it->second;
  v.is3D = true;
  v.pos[0] = px;
  v.pos[1] = py;
  v.pos[2] = pz;
  v.minDist = minDist;
  v.maxDist = maxDist;
  v.mode3D = mode3D;
  ApplyVoice3D(v);
}

void SetListenerPosition(float x, float y, float z) {
  std::lock_guard<std::mutex> lk(g_xaMtx);
  g_listPos[0] = x;
  g_listPos[1] = y;
  g_listPos[2] = z;
}

void SetListenerOrientation(float fx, float fy, float fz, float tx, float ty,
                            float tz) {
  std::lock_guard<std::mutex> lk(g_xaMtx);
  g_listFront[0] = fx;
  g_listFront[1] = fy;
  g_listFront[2] = fz;
  g_listTop[0] = tx;
  g_listTop[1] = ty;
  g_listTop[2] = tz;
  UpdateListenerRight();
}

void SetDistanceFactor(float factor) {
  std::lock_guard<std::mutex> lk(g_xaMtx);
  g_distFactor = (factor > 0.0f) ? factor : 1.0f;
}

void SetRolloffFactor(float factor) {
  std::lock_guard<std::mutex> lk(g_xaMtx);
  g_rolloffFactor = (factor >= 0.0f) ? factor : 1.0f;
}

void Commit3DSettings() {
  std::lock_guard<std::mutex> lk(g_xaMtx);
  for (auto &kv : g_voices) {
    if (kv.second.is3D)
      ApplyVoice3D(kv.second);
  }
}

uint32_t GetStatus(uint32_t handle) {
  std::lock_guard<std::mutex> lk(g_xaMtx);
  auto it = g_voices.find(handle);
  if (it == g_voices.end() || !it->second.voice || !it->second.submitted)
    return 0u;
  XAUDIO2_VOICE_STATE st{};
  it->second.voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
  if (st.BuffersQueued == 0) {
    it->second.submitted = false;
    return 0u;
  }
  return it->second.looping ? 5u
                            : 1u; // DSBSTATUS_PLAYING (| DSBSTATUS_LOOPING)
}

uint32_t GetPlayCursorBytes(uint32_t handle) {
  std::lock_guard<std::mutex> lk(g_xaMtx);
  auto it = g_voices.find(handle);
  if (it == g_voices.end() || !it->second.voice || it->second.pcm.empty())
    return 0u;
  XAUDIO2_VOICE_STATE st{};
  it->second.voice->GetState(&st);
  uint64_t totalFrames =
      it->second.pcm.size() / std::max<uint16_t>(1u, it->second.channels);
  if (totalFrames == 0)
    return 0u;
  uint64_t posFrame = st.SamplesPlayed % totalFrames;
  return static_cast<uint32_t>(posFrame * it->second.channels * 2u);
}

// ============================================================================
// Media Foundation WMA Decoder (IMFSourceReader -> 16-bit PCM)
// ============================================================================
bool CreateWmaDecoder(uint32_t handle, const char *hostPath,
                      uint16_t &outChannels, uint32_t &outSampleRate,
                      uint16_t &outBitsPerSample) {
  if (!hostPath || !hostPath[0])
    return false;
  std::lock_guard<std::mutex> lk(g_xaMtx);
  if (!g_mfStarted) {
    if (SUCCEEDED(MFStartup(MF_VERSION)))
      g_mfStarted = true;
  }
  auto oldIt = g_wmaDecoders.find(handle);
  if (oldIt != g_wmaDecoders.end()) {
    if (oldIt->second.reader)
      oldIt->second.reader->Release();
    g_wmaDecoders.erase(oldIt);
  }

  int wlen = MultiByteToWideChar(CP_ACP, 0, hostPath, -1, nullptr, 0);
  if (wlen <= 0)
    return false;
  std::wstring wpath(static_cast<size_t>(wlen), L'\0');
  MultiByteToWideChar(CP_ACP, 0, hostPath, -1, &wpath[0], wlen);

  IMFSourceReader *reader = nullptr;
  HRESULT hr = MFCreateSourceReaderFromURL(wpath.c_str(), nullptr, &reader);
  if (FAILED(hr) || !reader) {
    std::printf("[UWP-WMA] MFCreateSourceReaderFromURL failed for '%s' (hr=0x%08lX)\n",
                hostPath, static_cast<unsigned long>(hr));
    return false;
  }

  reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
  reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE);

  IMFMediaType *partialType = nullptr;
  if (SUCCEEDED(MFCreateMediaType(&partialType)) && partialType) {
    partialType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    partialType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    partialType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    hr = reader->SetCurrentMediaType(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, partialType);
    partialType->Release();
  } else {
    hr = E_FAIL;
  }
  if (FAILED(hr)) {
    std::printf("[UWP-WMA] SetCurrentMediaType(PCM16) failed for '%s' (hr=0x%08lX)\n",
                hostPath, static_cast<unsigned long>(hr));
    reader->Release();
    return false;
  }

  UINT32 ch = 2, sr = 44100, bps = 16;
  IMFMediaType *actualType = nullptr;
  if (SUCCEEDED(reader->GetCurrentMediaType(
          static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &actualType)) &&
      actualType) {
    actualType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
    actualType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
    actualType->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bps);
    actualType->Release();
  }
  outChannels = static_cast<uint16_t>((ch >= 1 && ch <= 2) ? ch : 2);
  outSampleRate = (sr >= 8000 && sr <= 96000) ? sr : 44100;
  outBitsPerSample = static_cast<uint16_t>(bps ? bps : 16);

  WmaDecoder dec{};
  dec.reader = reader;
  g_wmaDecoders[handle] = std::move(dec);
  std::printf("[UWP-WMA] Opened '%s' (%u ch, %u Hz, %u-bit PCM)\n",
              hostPath, outChannels, outSampleRate, outBitsPerSample);
  return true;
}

uint32_t ReadWmaPcm(uint32_t handle, uint8_t *dst, uint32_t maxBytes) {
  if (!dst || maxBytes == 0)
    return 0u;
  std::lock_guard<std::mutex> lk(g_xaMtx);
  auto it = g_wmaDecoders.find(handle);
  if (it == g_wmaDecoders.end() || !it->second.reader)
    return 0u;
  auto &dec = it->second;

  uint32_t written = 0;
  if (dec.resPos < dec.residual.size()) {
    size_t avail = dec.residual.size() - dec.resPos;
    size_t take = std::min<size_t>(avail, maxBytes);
    std::memcpy(dst, dec.residual.data() + dec.resPos, take);
    dec.resPos += take;
    written += static_cast<uint32_t>(take);
    if (dec.resPos >= dec.residual.size()) {
      dec.residual.clear();
      dec.resPos = 0;
    }
  }

  while (written < maxBytes && !dec.eof) {
    DWORD dwFlags = 0;
    IMFSample *sample = nullptr;
    HRESULT hr = dec.reader->ReadSample(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr,
        &dwFlags, nullptr, &sample);
    if (FAILED(hr) || (dwFlags & MF_SOURCE_READERF_ENDOFSTREAM)) {
      dec.eof = true;
      if (sample)
        sample->Release();
      break;
    }
    if (!sample)
      continue;
    IMFMediaBuffer *buf = nullptr;
    if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf)) && buf) {
      BYTE *pData = nullptr;
      DWORD cbLen = 0;
      if (SUCCEEDED(buf->Lock(&pData, nullptr, &cbLen)) && pData && cbLen > 0) {
        uint32_t need = maxBytes - written;
        uint32_t take = std::min<uint32_t>(need, cbLen);
        std::memcpy(dst + written, pData, take);
        written += take;
        if (cbLen > take) {
          dec.residual.assign(pData + take, pData + cbLen);
          dec.resPos = 0;
        }
        buf->Unlock();
      }
      buf->Release();
    }
    sample->Release();
  }
  return written;
}

void FlushWmaDecoder(uint32_t handle) {
  std::lock_guard<std::mutex> lk(g_xaMtx);
  auto it = g_wmaDecoders.find(handle);
  if (it == g_wmaDecoders.end() || !it->second.reader)
    return;
  auto &dec = it->second;
  dec.residual.clear();
  dec.resPos = 0;
  dec.eof = false;
  PROPVARIANT var{};
  PropVariantInit(&var);
  var.vt = VT_I8;
  var.hVal.QuadPart = 0;
  dec.reader->SetCurrentPosition(GUID_NULL, var);
  PropVariantClear(&var);
}

void ReleaseWmaDecoder(uint32_t handle) {
  std::lock_guard<std::mutex> lk(g_xaMtx);
  auto it = g_wmaDecoders.find(handle);
  if (it != g_wmaDecoders.end()) {
    if (it->second.reader)
      it->second.reader->Release();
    g_wmaDecoders.erase(it);
  }
}

// ============================================================================
// IDirectSoundStream -> XAudio2 Streaming Voice
// ============================================================================
bool CreateStream(uint32_t handle, uint16_t channels, uint32_t sampleRate) {
  std::lock_guard<std::mutex> lk(g_xaMtx);
  if (!g_xa2)
    return false;
  auto oldIt = g_streams.find(handle);
  if (oldIt != g_streams.end()) {
    if (oldIt->second.alive)
      oldIt->second.alive->store(false, std::memory_order_release);
    if (oldIt->second.voice) {
      oldIt->second.voice->Stop(0);
      oldIt->second.voice->FlushSourceBuffers();
      oldIt->second.voice->DestroyVoice();
    }
    g_streams.erase(oldIt);
  }

  XaStream st{};
  st.channels = (channels >= 1 && channels <= 2) ? channels : 2;
  st.sampleRate =
      (sampleRate >= 8000 && sampleRate <= 96000) ? sampleRate : 44100;
  st.cb = std::make_unique<StreamVoiceCallback>();
  st.alive = std::make_shared<std::atomic<bool>>(true);

  WAVEFORMATEX wf{};
  wf.wFormatTag = WAVE_FORMAT_PCM;
  wf.nChannels = st.channels;
  wf.nSamplesPerSec = st.sampleRate;
  wf.wBitsPerSample = 16;
  wf.nBlockAlign = wf.nChannels * 2;
  wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;

  if (FAILED(g_xa2->CreateSourceVoice(&st.voice, &wf, 0, 2.0f, st.cb.get())) ||
      !st.voice) {
    return false;
  }
  st.voice->SetVolume(st.volume);
  st.voice->Start(0);
  g_streams[handle] = std::move(st);
  return true;
}

void ReleaseStream(uint32_t handle) {
  std::lock_guard<std::mutex> lk(g_xaMtx);
  auto it = g_streams.find(handle);
  if (it != g_streams.end()) {
    if (it->second.alive)
      it->second.alive->store(false, std::memory_order_release);
    if (it->second.voice) {
      it->second.voice->Stop(0);
      it->second.voice->FlushSourceBuffers();
      it->second.voice->DestroyVoice();
    }
    g_streams.erase(it);
  }
}

bool SubmitStreamPacket(uint32_t handle, const uint8_t *pcmData, uint32_t bytes,
                        uint8_t *guestMem, uint32_t pdwStatusVA,
                        uint32_t pdwCompletedVA, void *hEvent) {
  if (!pcmData || bytes == 0)
    return false;
  std::lock_guard<std::mutex> lk(g_xaMtx);
  auto it = g_streams.find(handle);
  if (it == g_streams.end() || !it->second.voice)
    return false;

  if (guestMem && pdwStatusVA) {
    uint32_t pend = 0x8000000Au; // XMEDIAPACKET_STATUS_PENDING
    std::memcpy(guestMem + pdwStatusVA, &pend, 4);
  }

  auto *pkt = new StreamPacketCtx();
  pkt->pcm.assign(pcmData, pcmData + bytes);
  pkt->guestMem = guestMem;
  pkt->pdwStatusVA = pdwStatusVA;
  pkt->pdwCompletedVA = pdwCompletedVA;
  pkt->origBytes = bytes;
  pkt->hEvent = hEvent;
  pkt->alive = it->second.alive;

  XAUDIO2_BUFFER buf{};
  buf.AudioBytes = bytes;
  buf.pAudioData = pkt->pcm.data();
  buf.pContext = pkt;

  if (FAILED(it->second.voice->SubmitSourceBuffer(&buf))) {
    delete pkt;
    if (guestMem && pdwStatusVA) {
      uint32_t ok = 0u;
      std::memcpy(guestMem + pdwStatusVA, &ok, 4);
    }
    return false;
  }
  return true;
}

void FlushStream(uint32_t handle) {
  std::lock_guard<std::mutex> lk(g_xaMtx);
  auto it = g_streams.find(handle);
  if (it != g_streams.end() && it->second.voice) {
    it->second.voice->Stop(0);
    it->second.voice->FlushSourceBuffers();
    if (!it->second.paused)
      it->second.voice->Start(0);
  }
}

void SetStreamVolume(uint32_t handle, int32_t dsVolHundredthsDb) {
  std::lock_guard<std::mutex> lk(g_xaMtx);
  auto it = g_streams.find(handle);
  if (it == g_streams.end())
    return;
  float gain = 1.0f;
  if (dsVolHundredthsDb <= -9999)
    gain = 0.0f;
  else if (dsVolHundredthsDb < 0)
    gain = std::pow(10.0f, static_cast<float>(dsVolHundredthsDb) / 2000.0f);
  it->second.volume = std::max(0.0f, std::min(1.0f, gain));
  if (it->second.voice)
    it->second.voice->SetVolume(it->second.volume);
}

void PauseStream(uint32_t handle, bool paused) {
  std::lock_guard<std::mutex> lk(g_xaMtx);
  auto it = g_streams.find(handle);
  if (it == g_streams.end() || !it->second.voice)
    return;
  it->second.paused = paused;
  if (paused)
    it->second.voice->Stop(0);
  else
    it->second.voice->Start(0);
}

uint32_t GetStreamQueuedCount(uint32_t handle) {
  std::lock_guard<std::mutex> lk(g_xaMtx);
  auto it = g_streams.find(handle);
  if (it == g_streams.end() || !it->second.voice)
    return 0u;
  XAUDIO2_VOICE_STATE st{};
  it->second.voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
  return st.BuffersQueued;
}

} // namespace xaudio2_bridge
} // namespace uwp_recomp
