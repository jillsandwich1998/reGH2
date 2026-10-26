#include "audio_format.h"

#include <algorithm>
#include <cstring>

namespace gh2plat::audio {

void GuestFrameToInterleaved(const uint8_t* guest, float* out) noexcept {
  for (uint32_t c = 0; c < kChannels; ++c) {
    const uint8_t* plane = guest + static_cast<size_t>(c) * kFrameSamples * sizeof(float);
    for (uint32_t s = 0; s < kFrameSamples; ++s) {
      uint32_t v;
      std::memcpy(&v, plane + static_cast<size_t>(s) * sizeof(float), sizeof(v));
      v = __builtin_bswap32(v);
      std::memcpy(&out[static_cast<size_t>(s) * kChannels + c], &v, sizeof(v));
    }
  }
}

// Same operation order as the SDK's conversion (separate multiplies and adds, no fused multiply-add: this file is built with
// -ffp-contract=off), so the result is bit identical to what the PoC played.
void FoldToStereo(const float* in, float* out, const StereoFold& fold, float gain) noexcept {
  const float scale = fold.scale * gain;
  for (uint32_t s = 0; s < kFrameSamples; ++s) {
    const float* x = in + static_cast<size_t>(s) * kChannels;
    const float mid = x[2] * fold.center + x[3] * fold.lfe;
    out[s * 2] = std::clamp((x[0] + mid + x[4] * fold.surround) * scale, -1.0f, 1.0f);
    out[s * 2 + 1] = std::clamp((x[1] + mid + x[5] * fold.surround) * scale, -1.0f, 1.0f);
  }
}

void ScaleClamp6(const float* in, float* out, float gain) noexcept {
  for (uint32_t k = 0; k < kFrameFloats; ++k) out[k] = std::clamp(in[k] * gain, -1.0f, 1.0f);
}

}  // namespace gh2plat::audio
