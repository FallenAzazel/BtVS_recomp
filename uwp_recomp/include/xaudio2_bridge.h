#pragma once
#include <cstdint>

namespace uwp_recomp {
namespace xaudio2_bridge {

// UWP & Win64 XAudio2 48 kHz Stereo Audio Engine (replaces waveOut dsound_bridge for Xbox Dev Mode!)
bool Init();
void Shutdown();
bool IsReady();
void SetMasterVolume(float volume01);

// DirectSound8 -> XAudio2 Voice Management (supports Xbox IMA-ADPCM 0x0069, PCM16, PCM8, and 3D Positional Audio)
void CreateBuffer(uint32_t handle, uint16_t formatTag, uint16_t channels, uint32_t sampleRate, uint16_t bitsPerSample, bool is3D = false);
void ReleaseBuffer(uint32_t handle);
void SetBufferData(uint32_t handle, const uint8_t* data, uint32_t bytes);
void Play(uint32_t handle, bool looping);
void Stop(uint32_t handle);
void SetVolume(uint32_t handle, int32_t dsVolHundredthsDb);
void SetFrequency(uint32_t handle, uint32_t freqHz);
void SetBuffer3DParams(uint32_t handle, float px, float py, float pz, float minDist, float maxDist, uint32_t mode3D);
void SetListenerPosition(float x, float y, float z);
void SetListenerOrientation(float fx, float fy, float fz, float tx, float ty, float tz);
void SetDistanceFactor(float factor);
void SetRolloffFactor(float factor);
void Commit3DSettings();
uint32_t GetStatus(uint32_t handle);
uint32_t GetPlayCursorBytes(uint32_t handle);

// WMA In-Memory Decoder (Media Foundation IMFSourceReader — 100% UWP / Xbox Dev Mode compatible)
bool CreateWmaDecoder(uint32_t handle, const char* hostPath,
                      uint16_t& outChannels, uint32_t& outSampleRate, uint16_t& outBitsPerSample);
uint32_t ReadWmaPcm(uint32_t handle, uint8_t* dst, uint32_t maxBytes);
void FlushWmaDecoder(uint32_t handle);
void ReleaseWmaDecoder(uint32_t handle);

// IDirectSoundStream -> XAudio2 Streaming Voice (hardware-clocked via OnBufferEnd callback)
bool CreateStream(uint32_t handle, uint16_t channels, uint32_t sampleRate);
void ReleaseStream(uint32_t handle);
bool SubmitStreamPacket(uint32_t handle, const uint8_t* pcmData, uint32_t bytes,
                        uint8_t* guestMem, uint32_t pdwStatusVA, uint32_t pdwCompletedVA, void* hEvent);
void FlushStream(uint32_t handle);
void SetStreamVolume(uint32_t handle, int32_t dsVolHundredthsDb);
void PauseStream(uint32_t handle, bool paused);
uint32_t GetStreamQueuedCount(uint32_t handle);

} // namespace xaudio2_bridge
} // namespace uwp_recomp
