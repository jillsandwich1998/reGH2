// gh2plat audio format: what the game hands to XAudioSubmitRenderDriverFrame and what the host sink plays.
//
// Verified in the SDK's own conversion (rex/audio/conversion.h, "sequential_6_BE_to_interleaved_6_LE") and by the call counts of
// the golden Quick Play trace (9011 submits in ~48 s = 187.6/s = 48000 / 256):
//   one frame = 6 channels x 256 samples of float32, big-endian, channel-sequential (plane 0 = 256 samples of channel 0, then channel 1, ...);
//   channel order FL FR FC LFE BL BR (the same order SDL uses for 6 channels), 6144 bytes per frame, 5.33 ms.
// The host side plays interleaved little-endian float32.
#pragma once

#include <cstdint>

namespace gh2plat::audio {

constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kFrameSamples = 256;  // samples per channel in one submitted frame
constexpr uint32_t kChannels = 6;
constexpr uint32_t kFrameFloats = kFrameSamples * kChannels;
constexpr uint32_t kFrameBytes = kFrameFloats * sizeof(float);
constexpr double kFrameMs = 1000.0 * kFrameSamples / kSampleRate;  // 5.333...

// The stereo fold of a 6-channel frame: L = clamp((FL + FC*center + LFE*lfe + BL*surround) * scale * gain), R likewise with FR / BR.
// The defaults are the ones the SDK's SDL driver has always used (so the sound does not change when the output moves here).
struct StereoFold {
  float center = 0.70710678f;
  float surround = 0.70710678f;
  float lfe = 0.0f;
  float scale = 0.58578644f;  // 1 / (1 + 0.707): the loudest case cannot clip
};

// in: one interleaved 6-channel frame (GuestFrameToInterleaved's output); out: kFrameSamples interleaved stereo samples.
void FoldToStereo(const float* in, float* out, const StereoFold& fold, float gain) noexcept;

// The 6-channel path: out = clamp(in * gain, -1, 1) (the SDK clamps there too, so a hot mix clips at full scale instead of leaving SDL a value above 1).
void ScaleClamp6(const float* in, float* out, float gain) noexcept;

// guest: 6 planes of 256 big-endian floats (kFrameBytes, no alignment required); out: kFrameFloats interleaved host floats.
void GuestFrameToInterleaved(const uint8_t* guest, float* out) noexcept;

}  // namespace gh2plat::audio
