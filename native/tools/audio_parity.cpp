// Is gh2plat's audio conversion bit-identical to the SDK's (the sound the PoC always played)? Random guest frames (big-endian, planar, values past
// full scale so the clamp is exercised) through both: the 6-channel path with gains 1 / 0.5 / 0.8 and the stereo fold. Exit status 0 = 0 differences.
//
//   SDK=work/sdk-built/linux-amd64; SRC=work/gh2-recomp/native/platform/audio_sdl3
//   clang++ -std=gnu++23 -O2 -msse4.1 -ffp-contract=off -I$SDK/include -I$SRC native/tools/audio_parity.cpp $SRC/audio_format.cpp \
//           -o /tmp/audio_parity -L$SDK/lib -lrexruntime -Wl,-rpath,$SDK/lib && /tmp/audio_parity
// (This file includes SDK headers on purpose: it is a check against the SDK, not part of native/platform.)
#include <rex/audio/conversion.h>
#include <rex/audio/downmix.h>

#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "audio_format.h"

int main() {
  std::mt19937 rng(1234);
  std::uniform_real_distribution<float> dist(-1.6f, 1.6f);
  const uint32_t N = 256;
  uint64_t frames = 0, bad6 = 0, bad2 = 0, clamp_hits = 0;
  std::vector<float> in_host(6 * N), out_sdk(6 * N), out_mine(6 * N), sdk2(2 * N), mine2(2 * N);
  std::vector<uint8_t> guest(6 * N * 4);
  for (int iter = 0; iter < 20000; ++iter) {
    for (uint32_t i = 0; i < 6 * N; ++i) {
      float v = dist(rng);
      if ((rng() & 63) == 0) v = (rng() & 1) ? 1.0f : -1.0f;      // exact edges
      if ((rng() & 127) == 0) v = 0.0f;
      in_host[i] = v;
      uint32_t bits;
      std::memcpy(&bits, &v, 4);
      bits = __builtin_bswap32(bits);
      std::memcpy(&guest[i * 4], &bits, 4);
    }
    // the SDK takes the guest buffer as float* (big-endian bit patterns), channel-sequential
    const float* sdk_in = reinterpret_cast<const float*>(guest.data());
    const rex::audio::SurroundMix mix;       // defaults 1,1,1
    const rex::audio::StereoFold fold;       // defaults
    const float gains[] = {1.0f, 1.0f, 0.5f, 0.8f};
    const float gain = gains[iter & 3];
    // 6 channels, gain 1.0 only (the gain pass of the SDK multiplies; ours copies when gain == 1 and multiplies otherwise)
    rex::audio::conversion::sequential_6_BE_to_interleaved_6_LE(out_sdk.data(), sdk_in, N, mix, gain);
    gh2plat::audio::GuestFrameToInterleaved(guest.data(), out_mine.data());
    std::vector<float> mine6(6 * N);
    gh2plat::audio::ScaleClamp6(out_mine.data(), mine6.data(), gain);
    if (std::memcmp(out_sdk.data(), mine6.data(), out_sdk.size() * 4) != 0) ++bad6;
    // stereo fold, several gains
    rex::audio::conversion::sequential_6_BE_to_interleaved_2_LE(sdk2.data(), sdk_in, N, fold, gain);
    gh2plat::audio::StereoFold myfold;
    gh2plat::audio::FoldToStereo(out_mine.data(), mine2.data(), myfold, gain);
    if (std::memcmp(sdk2.data(), mine2.data(), sdk2.size() * 4) != 0) ++bad2;
    for (float x : sdk2) clamp_hits += (x == 1.0f || x == -1.0f);
    ++frames;
  }
  std::printf("frames compared: %llu\n6-channel path (gains 1, 0.5, 0.8, clamp): %llu differ (bit-exact compare)\nstereo fold (gains 1, 0.5, 0.8): %llu differ; clamped samples seen: %llu\n",
              (unsigned long long)frames, (unsigned long long)bad6, (unsigned long long)bad2, (unsigned long long)clamp_hits);
  return (bad6 || bad2) ? 1 : 0;
}
