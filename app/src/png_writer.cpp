#include "png_writer.h"

#include <cstdio>
#include <vector>

namespace {

uint32_t Crc32(const uint8_t* data, size_t n, uint32_t crc = 0) {
  static uint32_t table[256];
  static bool ready = false;
  if (!ready) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    ready = true;
  }
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

void Put32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(x >> 24);
  v.push_back(x >> 16);
  v.push_back(x >> 8);
  v.push_back(x);
}

bool Chunk(FILE* f, const char type[4], const std::vector<uint8_t>& data) {
  std::vector<uint8_t> head;
  Put32(head, static_cast<uint32_t>(data.size()));
  std::vector<uint8_t> body(type, type + 4);
  body.insert(body.end(), data.begin(), data.end());
  std::vector<uint8_t> tail;
  Put32(tail, Crc32(body.data(), body.size()));
  return fwrite(head.data(), 1, head.size(), f) == head.size() && fwrite(body.data(), 1, body.size(), f) == body.size() &&
         fwrite(tail.data(), 1, tail.size(), f) == tail.size();
}

}  // namespace

bool Gh2WritePngRgb(const std::string& path, uint32_t width, uint32_t height, const uint8_t* rgb) {
  if (!width || !height || !rgb) return false;
  // Scanlines, each with filter type 0 in front.
  std::vector<uint8_t> raw;
  raw.reserve((static_cast<size_t>(width) * 3 + 1) * height);
  for (uint32_t y = 0; y < height; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgb + static_cast<size_t>(y) * width * 3, rgb + static_cast<size_t>(y + 1) * width * 3);
  }
  // zlib stream made of stored (uncompressed) deflate blocks.
  std::vector<uint8_t> z = {0x78, 0x01};
  for (size_t pos = 0; pos < raw.size();) {
    const size_t n = std::min<size_t>(65535, raw.size() - pos);
    const bool last = pos + n == raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(n & 0xFF);
    z.push_back(n >> 8);
    z.push_back(~n & 0xFF);
    z.push_back((~n >> 8) & 0xFF);
    z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
    pos += n;
  }
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) {
    a = (a + c) % 65521;
    b = (b + a) % 65521;
  }
  Put32(z, (b << 16) | a);

  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return false;
  static const uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<uint8_t> ihdr;
  Put32(ihdr, width);
  Put32(ihdr, height);
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8 bits, RGB, deflate, no filter method, no interlace
  const bool ok = fwrite(kSignature, 1, 8, f) == 8 && Chunk(f, "IHDR", ihdr) && Chunk(f, "IDAT", z) && Chunk(f, "IEND", {});
  fclose(f);
  return ok;
}
