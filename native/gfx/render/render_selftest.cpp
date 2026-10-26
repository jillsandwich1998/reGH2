// Self-test of the renderer's pure helpers (no Vulkan): the Xenos tiling is a bijection onto the padded texture, the endian modes are what they say.
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

#include "xenos.h"

using namespace gh2render::xenos;

static int failures = 0;
#define CHECK(cond, ...)             \
  do {                               \
    if (cond) {                      \
      std::printf("ok:   " __VA_ARGS__); \
      std::printf("\n");             \
    } else {                         \
      std::printf("FAIL: " __VA_ARGS__); \
      std::printf("\n");             \
      ++failures;                    \
    }                                \
  } while (0)

int main() {
  // tiling: every block of a 96 x 64 texture lands on its own slot (inside the padded area for blocks of 2 bytes and more)
  for (uint32_t bpb_log2 = 0; bpb_log2 <= 4; ++bpb_log2) {
    const uint32_t w = 96, h = 64, bpb = 1u << bpb_log2;
    std::set<int32_t> seen;
    bool in_range = true, aligned = true;
    for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x) {
        const int32_t o = TiledOffset2D(x, y, w, bpb_log2);
        seen.insert(o);
        // one-byte blocks (8-bit textures) spread over a wider area than they fill: only uniqueness is demanded of them
        if (o < 0 || (bpb > 1 && static_cast<uint64_t>(o) + bpb > static_cast<uint64_t>(w) * h * bpb)) in_range = false;
        if (o % bpb) aligned = false;
      }
    CHECK(seen.size() == static_cast<size_t>(w) * h && in_range && aligned, "tiling of %ux%u blocks of %u bytes is a bijection inside the texture", w, h, bpb);
  }
  // a pitch that is not a multiple of 32 is rounded up
  CHECK(TiledOffset2D(33, 0, 40, 2) == TiledOffset2D(33, 0, 64, 2), "pitch 40 behaves as 64");

  // endian modes on one dword 00 11 22 33
  const uint8_t src[4] = {0x00, 0x11, 0x22, 0x33};
  uint8_t d[4];
  std::memcpy(d, src, 4);
  SwapEndian(d, 4, 0);
  CHECK(!std::memcmp(d, src, 4), "endian 0 leaves the data alone");
  std::memcpy(d, src, 4);
  SwapEndian(d, 4, 1);
  CHECK(d[0] == 0x11 && d[1] == 0x00 && d[2] == 0x33 && d[3] == 0x22, "endian 1 (8in16) swaps the bytes of each halfword");
  std::memcpy(d, src, 4);
  SwapEndian(d, 4, 2);
  CHECK(d[0] == 0x33 && d[1] == 0x22 && d[2] == 0x11 && d[3] == 0x00, "endian 2 (8in32) reverses the dword");
  std::memcpy(d, src, 4);
  SwapEndian(d, 4, 3);
  CHECK(d[0] == 0x22 && d[1] == 0x33 && d[2] == 0x00 && d[3] == 0x11, "endian 3 (16in32) swaps the halfwords");

  {   // the word-at-a-time swap equals the byte-by-byte definition on odd lengths and offsets
    std::vector<uint8_t> a(1031), b;
    for (size_t i = 0; i < a.size(); ++i) a[i] = static_cast<uint8_t>(i * 131 + 7);
    bool same = true;
    for (uint32_t mode = 1; mode <= 3 && same; ++mode)
      for (size_t len : {size_t(0), size_t(1), size_t(3), size_t(4), size_t(5), size_t(1031)}) {
        b = a;
        SwapEndian(b.data(), len, mode);
        std::vector<uint8_t> ref = a;
        if (mode == 1) for (size_t i = 0; i + 1 < len; i += 2) std::swap(ref[i], ref[i + 1]);
        if (mode == 2) for (size_t i = 0; i + 3 < len; i += 4) { std::swap(ref[i], ref[i + 3]); std::swap(ref[i + 1], ref[i + 2]); }
        if (mode == 3) for (size_t i = 0; i + 3 < len; i += 4) { std::swap(ref[i], ref[i + 2]); std::swap(ref[i + 1], ref[i + 3]); }
        if (b != ref) same = false;
      }
    CHECK(same, "SwapEndian equals the byte-wise definition (modes 1-3, lengths 0..1031)");
    bool copy_same = true;
    for (uint32_t mode = 0; mode <= 3; ++mode)
      for (size_t len : {size_t(0), size_t(1), size_t(3), size_t(4), size_t(5), size_t(1031)}) {
        std::vector<uint8_t> viaswap = a, viacopy(a.size(), 0xEE);
        SwapEndian(viaswap.data(), len, mode);
        SwapEndianCopy(viacopy.data(), a.data(), len, mode);
        if (!std::equal(viaswap.begin(), viaswap.begin() + len, viacopy.begin())) copy_same = false;
      }
    CHECK(copy_same, "SwapEndianCopy equals a copy followed by SwapEndian");
  }

  // decoding an untiled level honours the pitch: 3 blocks of 4 bytes per row, rows 5 blocks apart
  std::vector<uint8_t> in(5 * 4 * 2), out(3 * 4 * 2);
  for (size_t i = 0; i < in.size(); ++i) in[i] = static_cast<uint8_t>(i);
  const TextureFormatInfo& info = GetTextureFormat(6);   // 8_8_8_8: 4 bytes per block
  DecodeLevel2D(in.data(), 3, 2, 5, false, 0, info, out.data());
  CHECK(out[0] == 0 && out[11] == 11 && out[12] == 20 && out[23] == 31, "an untiled level is repacked row by row (pitch 5 -> width 3)");
  // decoding a tiled level then finding each block where the formula says
  std::vector<uint8_t> tiled(64 * 32 * 4), flat(40 * 32 * 4);
  for (uint32_t y = 0; y < 32; ++y)
    for (uint32_t x = 0; x < 40; ++x) {
      const uint32_t v = y * 1000 + x;
      std::memcpy(&tiled[TiledOffset2D(x, y, 64, 2)], &v, 4);
    }
  DecodeLevel2D(tiled.data(), 40, 32, 64, true, 0, info, flat.data());
  bool all = true;
  for (uint32_t y = 0; y < 32 && all; ++y)
    for (uint32_t x = 0; x < 40; ++x) {
      uint32_t v;
      std::memcpy(&v, &flat[(y * 40 + x) * 4], 4);
      if (v != y * 1000 + x) {
        all = false;
        break;
      }
    }
  CHECK(all, "a tiled 40x32 texture (pitch 64) decodes back to row-major");
  std::printf(failures ? "FAILED (%d)\n" : "PASSED\n", failures);
  return failures ? 1 : 0;
}
