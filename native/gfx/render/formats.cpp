#include <cstring>
#include <utility>

#include "xenos.h"

namespace gh2render::xenos {

namespace {

TextureFormatInfo Fmt(VkFormat vk, uint32_t bw, uint32_t bh, uint32_t bytes, bool compressed, const char* name, bool depth = false) {
  TextureFormatInfo i;
  i.vk = vk;
  i.block_width = bw;
  i.block_height = bh;
  i.bytes_per_block = bytes;
  uint32_t log2 = 0;
  while ((1u << log2) < bytes) ++log2;
  i.bytes_per_block_log2 = log2;
  i.compressed = compressed;
  i.depth = depth;
  i.name = name;
  return i;
}

const TextureFormatInfo* BuildTable() {
  static TextureFormatInfo table[64];
  for (auto& t : table) t = TextureFormatInfo{};
  table[2] = Fmt(VK_FORMAT_R8_UNORM, 1, 1, 1, false, "8");
  table[3] = Fmt(VK_FORMAT_A1R5G5B5_UNORM_PACK16, 1, 1, 2, false, "1_5_5_5");
  table[4] = Fmt(VK_FORMAT_R5G6B5_UNORM_PACK16, 1, 1, 2, false, "5_6_5");
  table[6] = Fmt(VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 4, false, "8_8_8_8");
  table[7] = Fmt(VK_FORMAT_A2B10G10R10_UNORM_PACK32, 1, 1, 4, false, "2_10_10_10");
  table[8] = Fmt(VK_FORMAT_R8_UNORM, 1, 1, 1, false, "8_A");
  table[9] = Fmt(VK_FORMAT_R8_UNORM, 1, 1, 1, false, "8_B");
  table[10] = Fmt(VK_FORMAT_R8G8_UNORM, 1, 1, 2, false, "8_8");
  table[14] = Fmt(VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 4, false, "8_8_8_8_A");
  table[15] = Fmt(VK_FORMAT_R4G4B4A4_UNORM_PACK16, 1, 1, 2, false, "4_4_4_4");
  table[18] = Fmt(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 4, 4, 8, true, "DXT1");
  table[19] = Fmt(VK_FORMAT_BC2_UNORM_BLOCK, 4, 4, 16, true, "DXT2_3");
  table[20] = Fmt(VK_FORMAT_BC3_UNORM_BLOCK, 4, 4, 16, true, "DXT4_5");
  table[22] = Fmt(VK_FORMAT_D32_SFLOAT_S8_UINT, 1, 1, 4, false, "24_8", true);
  table[23] = Fmt(VK_FORMAT_D32_SFLOAT_S8_UINT, 1, 1, 4, false, "24_8_FLOAT", true);
  table[24] = Fmt(VK_FORMAT_R16_UNORM, 1, 1, 2, false, "16");
  table[25] = Fmt(VK_FORMAT_R16G16_UNORM, 1, 1, 4, false, "16_16");
  table[26] = Fmt(VK_FORMAT_R16G16B16A16_UNORM, 1, 1, 8, false, "16_16_16_16");
  table[30] = Fmt(VK_FORMAT_R16_SFLOAT, 1, 1, 2, false, "16_FLOAT");
  table[31] = Fmt(VK_FORMAT_R16G16_SFLOAT, 1, 1, 4, false, "16_16_FLOAT");
  table[32] = Fmt(VK_FORMAT_R16G16B16A16_SFLOAT, 1, 1, 8, false, "16_16_16_16_FLOAT");
  table[36] = Fmt(VK_FORMAT_R32_SFLOAT, 1, 1, 4, false, "32_FLOAT");
  table[37] = Fmt(VK_FORMAT_R32G32_SFLOAT, 1, 1, 8, false, "32_32_FLOAT");
  table[38] = Fmt(VK_FORMAT_R32G32B32A32_SFLOAT, 1, 1, 16, false, "32_32_32_32_FLOAT");
  table[49] = Fmt(VK_FORMAT_BC5_UNORM_BLOCK, 4, 4, 16, true, "DXN");
  table[50] = Fmt(VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 4, false, "8_8_8_8_AS_16_16_16_16");
  table[51] = Fmt(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 4, 4, 8, true, "DXT1_AS_16_16_16_16");
  table[52] = Fmt(VK_FORMAT_BC2_UNORM_BLOCK, 4, 4, 16, true, "DXT2_3_AS_16_16_16_16");
  table[53] = Fmt(VK_FORMAT_BC3_UNORM_BLOCK, 4, 4, 16, true, "DXT4_5_AS_16_16_16_16");
  table[54] = Fmt(VK_FORMAT_A2B10G10R10_UNORM_PACK32, 1, 1, 4, false, "2_10_10_10_AS_16_16_16_16");
  return table;
}

}  // namespace

const TextureFormatInfo& GetTextureFormat(uint32_t xenos_format) {
  static const TextureFormatInfo* table = BuildTable();
  return table[xenos_format & 63];
}

VkFormat VertexVkFormat(uint32_t f, bool is_signed, bool integer, bool as_uint, uint32_t* bytes) {
  VkFormat vk = VK_FORMAT_UNDEFINED;
  uint32_t size = 0;
  switch (f) {
    case 6:   // 8_8_8_8
      size = 4;
      vk = as_uint ? VK_FORMAT_R32_UINT : integer ? (is_signed ? VK_FORMAT_R8G8B8A8_SSCALED : VK_FORMAT_R8G8B8A8_USCALED) : (is_signed ? VK_FORMAT_R8G8B8A8_SNORM : VK_FORMAT_R8G8B8A8_UNORM);
      break;
    case 7:   // 2_10_10_10
      size = 4;
      vk = as_uint ? VK_FORMAT_R32_UINT : (is_signed ? VK_FORMAT_A2B10G10R10_SNORM_PACK32 : VK_FORMAT_A2B10G10R10_UNORM_PACK32);
      break;
    case 16:  // 10_11_11
    case 17:  // 11_11_10
      size = 4;
      vk = VK_FORMAT_R32_UINT;   // the game packs normals this way; the translated shader unpacks the raw word
      break;
    case 25:  // 16_16
      size = 4;
      vk = as_uint ? VK_FORMAT_R32_UINT : integer ? (is_signed ? VK_FORMAT_R16G16_SSCALED : VK_FORMAT_R16G16_USCALED) : (is_signed ? VK_FORMAT_R16G16_SNORM : VK_FORMAT_R16G16_UNORM);
      break;
    case 26:  // 16_16_16_16
      size = 8;
      vk = as_uint ? VK_FORMAT_R32G32_UINT : integer ? (is_signed ? VK_FORMAT_R16G16B16A16_SSCALED : VK_FORMAT_R16G16B16A16_USCALED) : (is_signed ? VK_FORMAT_R16G16B16A16_SNORM : VK_FORMAT_R16G16B16A16_UNORM);
      break;
    case 31: size = 4; vk = as_uint ? VK_FORMAT_R32_UINT : VK_FORMAT_R16G16_SFLOAT; break;
    case 32: size = 8; vk = as_uint ? VK_FORMAT_R32G32_UINT : VK_FORMAT_R16G16B16A16_SFLOAT; break;
    case 33: size = 4; vk = is_signed ? VK_FORMAT_R32_SINT : VK_FORMAT_R32_UINT; break;
    case 34: size = 8; vk = is_signed ? VK_FORMAT_R32G32_SINT : VK_FORMAT_R32G32_UINT; break;
    case 35: size = 16; vk = is_signed ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT; break;
    case 36: size = 4; vk = as_uint ? VK_FORMAT_R32_UINT : VK_FORMAT_R32_SFLOAT; break;
    case 37: size = 8; vk = as_uint ? VK_FORMAT_R32G32_UINT : VK_FORMAT_R32G32_SFLOAT; break;
    case 38: size = 16; vk = as_uint ? VK_FORMAT_R32G32B32A32_UINT : VK_FORMAT_R32G32B32A32_SFLOAT; break;
    case 57: size = 12; vk = as_uint ? VK_FORMAT_R32G32B32_UINT : VK_FORMAT_R32G32B32_SFLOAT; break;
    default: break;
  }
  if (bytes) *bytes = size;
  return vk;
}

int32_t TiledOffset2D(int32_t x, int32_t y, uint32_t pitch_blocks, uint32_t bpb_log2) {
  pitch_blocks = (pitch_blocks + 31u) & ~31u;
  // which 32x32-block tile, then the position inside it (the hardware interleaves the low bits of x and y)
  int32_t tile = ((x >> 5) + (y >> 5) * static_cast<int32_t>(pitch_blocks >> 5)) << (bpb_log2 + 7);
  int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << bpb_log2;
  int32_t offset = tile + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

void SwapEndian(uint8_t* data, size_t bytes, uint32_t endian) {
  // whole words at a time (the compiler turns these loops into byte shuffles); a trailing partial unit stays as it is, as before
  switch (endian) {
    case 1:   // 8in16: swap the two bytes of every 16-bit unit
      for (size_t i = 0; i + 1 < bytes; i += 2) {
        uint16_t w;
        std::memcpy(&w, data + i, 2);
        w = static_cast<uint16_t>((w << 8) | (w >> 8));
        std::memcpy(data + i, &w, 2);
      }
      break;
    case 2:   // 8in32: reverse the bytes of every 32-bit unit
      for (size_t i = 0; i + 3 < bytes; i += 4) {
        uint32_t w;
        std::memcpy(&w, data + i, 4);
        w = __builtin_bswap32(w);
        std::memcpy(data + i, &w, 4);
      }
      break;
    case 3:   // 16in32: swap the two halves of every 32-bit unit, each half keeps its bytes
      for (size_t i = 0; i + 3 < bytes; i += 4) {
        uint32_t w;
        std::memcpy(&w, data + i, 4);
        w = (w << 16) | (w >> 16);
        std::memcpy(data + i, &w, 4);
      }
      break;
    default: break;
  }
}

void SwapEndianCopy(uint8_t* dst, const uint8_t* src, size_t bytes, uint32_t endian) {
  // the same result as a copy followed by SwapEndian, in one pass (the destination may be write-combined memory: it is only written, in order)
  size_t i = 0;
  switch (endian) {
    case 1:
      for (; i + 1 < bytes; i += 2) {
        uint16_t w;
        std::memcpy(&w, src + i, 2);
        w = static_cast<uint16_t>((w << 8) | (w >> 8));
        std::memcpy(dst + i, &w, 2);
      }
      break;
    case 2:
      for (; i + 3 < bytes; i += 4) {
        uint32_t w;
        std::memcpy(&w, src + i, 4);
        w = __builtin_bswap32(w);
        std::memcpy(dst + i, &w, 4);
      }
      break;
    case 3:
      for (; i + 3 < bytes; i += 4) {
        uint32_t w;
        std::memcpy(&w, src + i, 4);
        w = (w << 16) | (w >> 16);
        std::memcpy(dst + i, &w, 4);
      }
      break;
    default: break;
  }
  if (i < bytes) std::memcpy(dst + i, src + i, bytes - i);   // a partial unit at the end is left as it is
}

size_t DecodeLevel2D(const uint8_t* src, uint32_t width_blocks, uint32_t height_blocks, uint32_t pitch_blocks, bool tiled, uint32_t endian, const TextureFormatInfo& info, uint8_t* dst) {
  const uint32_t bpb = info.bytes_per_block;
  const size_t row_bytes = static_cast<size_t>(width_blocks) * bpb;
  if (!tiled) {
    const size_t src_pitch = static_cast<size_t>(pitch_blocks ? pitch_blocks : width_blocks) * bpb;
    for (uint32_t y = 0; y < height_blocks; ++y) std::memcpy(dst + y * row_bytes, src + y * src_pitch, row_bytes);
  } else {
    for (uint32_t y = 0; y < height_blocks; ++y)
      for (uint32_t x = 0; x < width_blocks; ++x)
        std::memcpy(dst + y * row_bytes + static_cast<size_t>(x) * bpb, src + TiledOffset2D(static_cast<int32_t>(x), static_cast<int32_t>(y), pitch_blocks ? pitch_blocks : width_blocks, info.bytes_per_block_log2), bpb);
  }
  const size_t total = row_bytes * height_blocks;
  SwapEndian(dst, total, endian);
  return total;
}

}  // namespace gh2render::xenos
