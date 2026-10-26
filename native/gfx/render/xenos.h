// Xenos hardware facts the renderer decodes: the bit fields of the state registers, the fetch constants, and their translation to Vulkan state. Facts about the hardware as the game's
// Direct3D library programs it (verified against the registers of the running game, see ../gpu/README.md), written as plain accessors. No SDK.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

namespace gh2render::xenos {

inline constexpr uint32_t Bits(uint32_t value, uint32_t lsb, uint32_t count) { return (value >> lsb) & ((count >= 32) ? 0xFFFFFFFFu : ((1u << count) - 1u)); }

// ---- register indices (dword) used by the draw path; the command processor owns the rest ----
namespace reg {
inline constexpr uint32_t RB_SURFACE_INFO = 0x2000, RB_COLOR_INFO = 0x2001, RB_DEPTH_INFO = 0x2002, RB_COLOR1_INFO = 0x2003, RB_COLOR2_INFO = 0x2004, RB_COLOR3_INFO = 0x2005;
inline constexpr uint32_t PA_SC_WINDOW_OFFSET = 0x2080, PA_SC_WINDOW_SCISSOR_TL = 0x2081, PA_SC_WINDOW_SCISSOR_BR = 0x2082, PA_SC_SCREEN_SCISSOR_TL = 0x200E, PA_SC_SCREEN_SCISSOR_BR = 0x200F;
inline constexpr uint32_t VGT_MAX_VTX_INDX = 0x2100, VGT_MIN_VTX_INDX = 0x2101, VGT_INDX_OFFSET = 0x2102, VGT_MULTI_PRIM_IB_RESET_INDX = 0x2103;
inline constexpr uint32_t RB_COLOR_MASK = 0x2104, RB_BLEND_RED = 0x2105, RB_STENCILREFMASK_BF = 0x210C, RB_STENCILREFMASK = 0x210D, RB_ALPHA_REF = 0x210E;
inline constexpr uint32_t PA_CL_VPORT_XSCALE = 0x210F, PA_CL_VPORT_XOFFSET = 0x2110, PA_CL_VPORT_YSCALE = 0x2111, PA_CL_VPORT_YOFFSET = 0x2112, PA_CL_VPORT_ZSCALE = 0x2113, PA_CL_VPORT_ZOFFSET = 0x2114;
inline constexpr uint32_t RB_DEPTHCONTROL = 0x2200, RB_BLENDCONTROL0 = 0x2201, RB_COLORCONTROL = 0x2202, PA_CL_CLIP_CNTL = 0x2204, PA_SU_SC_MODE_CNTL = 0x2205, PA_CL_VTE_CNTL = 0x2206, RB_MODECONTROL = 0x2208;
inline constexpr uint32_t RB_BLENDCONTROL1 = 0x2209, RB_BLENDCONTROL2 = 0x220A, RB_BLENDCONTROL3 = 0x220B;
inline constexpr uint32_t PA_SC_AA_CONFIG = 0x2301, PA_SU_VTX_CNTL = 0x2302;
inline constexpr uint32_t RB_COPY_CONTROL = 0x2318, RB_COPY_DEST_BASE = 0x2319, RB_COPY_DEST_PITCH = 0x231A, RB_COPY_DEST_INFO = 0x231B, RB_DEPTH_CLEAR = 0x231D, RB_COLOR_CLEAR = 0x231E, RB_COLOR_CLEAR_LO = 0x231F;
inline constexpr uint32_t PA_SU_POLY_OFFSET_FRONT_SCALE = 0x2380, PA_SU_POLY_OFFSET_FRONT_OFFSET = 0x2381, PA_SU_POLY_OFFSET_BACK_SCALE = 0x2382, PA_SU_POLY_OFFSET_BACK_OFFSET = 0x2383;
inline constexpr uint32_t SHADER_CONSTANT_VS = 0x4000, SHADER_CONSTANT_PS = 0x4400, SHADER_CONSTANT_FETCH = 0x4800, SHADER_CONSTANT_BOOL = 0x4900, SHADER_CONSTANT_LOOP = 0x4908;
}  // namespace reg

// ---- render target description ----
struct SurfaceInfo {
  uint32_t pitch;   // pixels (of one tile pass: a 1280-wide target in two halves has 640)
  uint32_t msaa;    // 0 = 1x, 1 = 2x, 2 = 4x
};
inline SurfaceInfo DecodeSurfaceInfo(uint32_t v) { return {Bits(v, 0, 14), Bits(v, 16, 2)}; }
// RB_COLOR_INFO: base in EDRAM tiles (12 bits incl. bit 11), format (ColorRenderTargetFormat) in bits 16..19
inline uint32_t ColorBase(uint32_t v) { return Bits(v, 0, 12); }
inline uint32_t ColorFormat(uint32_t v) { return Bits(v, 16, 4); }
inline uint32_t DepthBase(uint32_t v) { return Bits(v, 0, 12); }
inline uint32_t DepthFormat(uint32_t v) { return Bits(v, 16, 1); }

// ---- fetch constants (6 dwords) ----
struct TextureFetch {
  uint32_t type, pitch_blocks32, tiled, format, endian, base_address /* in 4 KB units */, width, height, depth, swizzle, mag_filter, min_filter, mip_filter, aniso, clamp_x, clamp_y, clamp_z, num_format;
  uint32_t mip_min, mip_max;
  bool border;
};
inline TextureFetch DecodeTextureFetch(const uint32_t* f) {
  TextureFetch t;
  t.type = Bits(f[0], 0, 2);
  t.clamp_x = Bits(f[0], 10, 3);
  t.clamp_y = Bits(f[0], 13, 3);
  t.clamp_z = Bits(f[0], 16, 3);
  t.pitch_blocks32 = Bits(f[0], 22, 9);
  t.tiled = Bits(f[0], 31, 1);
  t.format = Bits(f[1], 0, 6);
  t.endian = Bits(f[1], 6, 2);
  t.base_address = Bits(f[1], 12, 20);
  t.width = Bits(f[2], 0, 13) + 1;
  t.height = Bits(f[2], 13, 13) + 1;
  t.depth = Bits(f[2], 26, 6) + 1;
  t.num_format = Bits(f[3], 0, 1);
  t.swizzle = Bits(f[3], 1, 12);
  t.mag_filter = Bits(f[3], 19, 2);
  t.min_filter = Bits(f[3], 21, 2);
  t.mip_filter = Bits(f[3], 23, 2);
  t.aniso = Bits(f[3], 25, 3);
  t.border = Bits(f[3], 31, 1) != 0;
  t.mip_min = Bits(f[4], 2, 4);
  t.mip_max = Bits(f[4], 6, 4);
  return t;
}
struct VertexFetch {
  uint32_t type, address_dwords, endian, size_dwords;
};
inline VertexFetch DecodeVertexFetch(const uint32_t* f) { return {Bits(f[0], 0, 2), Bits(f[0], 2, 30), Bits(f[1], 0, 2), Bits(f[1], 2, 24)}; }

// ---- Xenos state -> Vulkan ----
inline VkCompareOp CompareOp(uint32_t f) {
  static const VkCompareOp map[8] = {VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS, VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_LESS_OR_EQUAL,
                                     VK_COMPARE_OP_GREATER, VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS};
  return map[f & 7];
}
inline VkStencilOp StencilOp(uint32_t o) {
  static const VkStencilOp map[8] = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_INCREMENT_AND_CLAMP, VK_STENCIL_OP_DECREMENT_AND_CLAMP,
                                     VK_STENCIL_OP_INVERT, VK_STENCIL_OP_INCREMENT_AND_WRAP, VK_STENCIL_OP_DECREMENT_AND_WRAP};
  return map[o & 7];
}
inline VkBlendFactor BlendFactor(uint32_t f) {
  switch (f) {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 4: return VK_BLEND_FACTOR_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 6: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 8: return VK_BLEND_FACTOR_DST_COLOR;
    case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_DST_ALPHA;
    case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return VK_BLEND_FACTOR_ONE;
  }
}
inline VkBlendOp BlendOp(uint32_t o) {
  switch (o) {
    case 0: return VK_BLEND_OP_ADD;
    case 1: return VK_BLEND_OP_SUBTRACT;
    case 2: return VK_BLEND_OP_MIN;
    case 3: return VK_BLEND_OP_MAX;
    case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default: return VK_BLEND_OP_ADD;
  }
}
inline VkSamplerAddressMode AddressMode(uint32_t clamp) {
  switch (clamp) {
    case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 2: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 3: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;     // clamp to halfway: nearest approximation
    case 5: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    case 6: case 7: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
  }
}

inline float AsFloat(uint32_t v) {
  float f;
  __builtin_memcpy(&f, &v, 4);
  return f;
}

// ---- formats and decoding (formats.cpp) ----
struct TextureFormatInfo {
  VkFormat vk = VK_FORMAT_UNDEFINED;
  uint32_t block_width = 1, block_height = 1;
  uint32_t bytes_per_block = 0;
  uint32_t bytes_per_block_log2 = 0;
  bool compressed = false;
  bool depth = false;
  const char* name = "?";
};
// Xenos TextureFormat -> what the renderer stores it as (VK_FORMAT_UNDEFINED: unsupported, rendered as an opaque grey texture)
const TextureFormatInfo& GetTextureFormat(uint32_t xenos_format);

// The Vulkan format a vertex element is fed to the translated shader as. `as_uint`: the shader declares the input as uint4 (normals, tangents, binormals and blend indices the game
// packs itself), so the raw words are delivered. Returns VK_FORMAT_UNDEFINED for formats the game does not use; `bytes` gets the element size.
VkFormat VertexVkFormat(uint32_t xenos_format, bool is_signed, bool integer, bool as_uint, uint32_t* bytes);

// Byte offset of block (x, y) of a 2D tiled texture (Xenos 32x32-block tiles with the micro-tile swizzle); `pitch_blocks` is the row pitch in blocks
// (rounded up to a multiple of 32 by the callers' contract), `bytes_per_block_log2` the block size.
int32_t TiledOffset2D(int32_t x, int32_t y, uint32_t pitch_blocks, uint32_t bytes_per_block_log2);

// Copies one mip level 2D from guest memory (big endian, optionally tiled) to a tightly packed little-endian buffer ready for vkCmdCopyBufferToImage.
// `src` points at the level's data. Returns the bytes written (width_blocks * height_blocks * bytes_per_block).
size_t DecodeLevel2D(const uint8_t* src, uint32_t width_blocks, uint32_t height_blocks, uint32_t pitch_blocks, bool tiled, uint32_t endian, const TextureFormatInfo& info, uint8_t* dst);

// Endian conversions applied to a run of memory: swaps within 16- or 32-bit units as the Xenos endian modes say (0 none, 1 8in16, 2 8in32, 3 16in32).
void SwapEndian(uint8_t* data, size_t bytes, uint32_t endian);
void SwapEndianCopy(uint8_t* dst, const uint8_t* src, size_t bytes, uint32_t endian);   // dst = SwapEndian(copy of src), one pass

}  // namespace gh2render::xenos
