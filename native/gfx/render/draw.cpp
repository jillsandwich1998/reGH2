// A guest draw -> Vulkan commands: shader modules from the cache, vertex input from the microcode's fetches, indices and vertices uploaded from guest memory, constants in the upload ring
// (the translated shaders read them through device addresses), textures through the bindless heaps, state from the register file.
#include <emmintrin.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <cstring>
#include <unordered_map>

#include "impl.h"

namespace gh2render {

namespace {

constexpr uint32_t kGuestMemoryLimit = 0x20000000;

// D3D declaration usages (xenos shader containers use these) and the Vulkan locations the translated vertex shaders give them (XenosRecomp USAGE_LOCATIONS)
enum Usage : uint32_t { kPosition = 0, kBlendWeight = 1, kBlendIndices = 2, kNormal = 3, kPointSize = 4, kTexCoord = 5, kTangent = 6, kBinormal = 7, kColor = 10 };

int UsageLocation(uint32_t usage, uint32_t index) {
  switch (usage) {
    case kPosition: return index == 0 ? 0 : (index == 1 ? 15 : -1);   // Position1: the bone / instance index of skinned meshes
    case kNormal: return index == 0 ? 1 : -1;
    case kTangent: return index == 0 ? 2 : -1;
    case kBinormal: return index == 0 ? 3 : -1;
    case kTexCoord: return index < 4 ? 4 + static_cast<int>(index) : (index < 8 ? 8 + static_cast<int>(index) : -1);
    case kColor: return index == 0 ? 8 : (index == 1 ? 11 : -1);
    case kBlendIndices: return index == 0 ? 9 : -1;
    case kBlendWeight: return index == 0 ? 10 : -1;
    default: return -1;
  }
}

bool UsageIsUint(uint32_t usage) { return usage == kBlendIndices || usage == kNormal || usage == kTangent || usage == kBinormal; }

float HalfToFloat(uint16_t h) {
  const uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
  uint32_t bits;
  if (exponent == 0) {
    if (mantissa == 0) bits = sign << 31;
    else {   // denormal
      uint32_t e = 127 - 15 + 1, m = mantissa;
      while (!(m & 0x400)) {
        m <<= 1;
        --e;
      }
      bits = (sign << 31) | (e << 23) | ((m & 0x3FF) << 13);
    }
  } else if (exponent == 31) {
    bits = (sign << 31) | 0x7F800000u | (mantissa << 13);
  } else {
    bits = (sign << 31) | ((exponent + 127 - 15) << 23) | (mantissa << 13);
  }
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

uint16_t FloatToHalf(float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  const uint32_t sign = (bits >> 16) & 0x8000;
  int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xFF) - 127 + 15;
  const uint32_t mantissa = bits & 0x7FFFFF;
  if (exponent <= 0) return static_cast<uint16_t>(sign);          // flushed to zero
  if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7C00);   // infinity
  return static_cast<uint16_t>(sign | (exponent << 10) | (mantissa >> 13));
}

// RECTLIST: the fourth vertex of a rectangle is v1 + v2 - v0, element by element. Floats and halves are computed exactly; normalised bytes are clamped; any other element keeps v2's value.
void SynthesizeRectVertex(uint8_t* out, const uint8_t* v0, const uint8_t* v1, const uint8_t* v2, uint32_t stride, const std::vector<VkVertexInputAttributeDescription>& attributes, uint32_t binding) {
  std::memcpy(out, v2, stride);
  for (const VkVertexInputAttributeDescription& a : attributes) {
    if (a.binding != binding) continue;
    uint32_t floats = 0, halves = 0, bytes = 0;
    switch (a.format) {
      case VK_FORMAT_R32_SFLOAT: floats = 1; break;
      case VK_FORMAT_R32G32_SFLOAT: floats = 2; break;
      case VK_FORMAT_R32G32B32_SFLOAT: floats = 3; break;
      case VK_FORMAT_R32G32B32A32_SFLOAT: floats = 4; break;
      case VK_FORMAT_R16G16_SFLOAT: halves = 2; break;
      case VK_FORMAT_R16G16B16A16_SFLOAT: halves = 4; break;
      case VK_FORMAT_R8G8B8A8_UNORM: bytes = 4; break;
      default: break;
    }
    for (uint32_t i = 0; i < floats; ++i) {
      float f0, f1, f2;
      std::memcpy(&f0, v0 + a.offset + 4 * i, 4);
      std::memcpy(&f1, v1 + a.offset + 4 * i, 4);
      std::memcpy(&f2, v2 + a.offset + 4 * i, 4);
      const float f3 = f1 + f2 - f0;
      std::memcpy(out + a.offset + 4 * i, &f3, 4);
    }
    for (uint32_t i = 0; i < halves; ++i) {
      uint16_t h0, h1, h2;
      std::memcpy(&h0, v0 + a.offset + 2 * i, 2);
      std::memcpy(&h1, v1 + a.offset + 2 * i, 2);
      std::memcpy(&h2, v2 + a.offset + 2 * i, 2);
      const uint16_t h3 = FloatToHalf(HalfToFloat(h1) + HalfToFloat(h2) - HalfToFloat(h0));
      std::memcpy(out + a.offset + 2 * i, &h3, 2);
    }
    for (uint32_t i = 0; i < bytes; ++i) {
      const int v = static_cast<int>(v1[a.offset + i]) + v2[a.offset + i] - v0[a.offset + i];
      out[a.offset + i] = static_cast<uint8_t>(std::min(255, std::max(0, v)));
    }
  }
}

// The destination swizzle of a vertex fetch: the library rewrites it per vertex declaration (a D3DCOLOR colour gets 0x60A = z y x w, the fetch the declaration does not have gets constants). The
// translated shader was made with the container's swizzle, so the data is rearranged on the host until the shader's own mapping gives what the live swizzle would: component c_i of the
// attribute (c_i = the container's selection for destination component i) takes the value the live selection l_i names. Selections: 0-3 a component of the fetched vector, 4 zero, 5 one.
struct SwizzleFix {
  bool used = false;
  uint32_t slot = 0, offset = 0, format = 0, live = 0, container = 0;   // offset in bytes
};

bool VertexComponents(uint32_t format, uint32_t* count, uint32_t* bytes, uint32_t* one) {
  switch (format) {
    case 6: *count = 4; *bytes = 1; *one = 0xFF; return true;
    case 25: *count = 2; *bytes = 2; *one = 0xFFFF; return true;
    case 26: *count = 4; *bytes = 2; *one = 0xFFFF; return true;
    case 31: *count = 2; *bytes = 2; *one = 0x3C00; return true;
    case 32: *count = 4; *bytes = 2; *one = 0x3C00; return true;
    case 33: *count = 1; *bytes = 4; *one = 1; return true;
    case 34: *count = 2; *bytes = 4; *one = 1; return true;
    case 35: *count = 4; *bytes = 4; *one = 1; return true;
    case 36: *count = 1; *bytes = 4; *one = 0x3F800000; return true;
    case 37: *count = 2; *bytes = 4; *one = 0x3F800000; return true;
    case 38: *count = 4; *bytes = 4; *one = 0x3F800000; return true;
    case 57: *count = 3; *bytes = 4; *one = 0x3F800000; return true;
    default: return false;
  }
}

// A copy into the upload ring. The ring is write-combined memory: whole 64-byte lines written in order go out as one burst, which glibc's memcpy (rep movsb above 2 KB, partial-line
// vector stores at the edges) does not always manage; this is the plain SSE2 loop with aligned stores (the ring hands out 16-byte aligned blocks).
inline void RingCopy(void* dst, const void* src, size_t bytes) {
  uint8_t* d = static_cast<uint8_t*>(dst);
  const uint8_t* s = static_cast<const uint8_t*>(src);
  if ((reinterpret_cast<uintptr_t>(d) & 15) == 0) {
    for (; bytes >= 64; bytes -= 64, d += 64, s += 64) {
      const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s)), b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 16));
      const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 32)), e = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 48));
      _mm_store_si128(reinterpret_cast<__m128i*>(d), a);
      _mm_store_si128(reinterpret_cast<__m128i*>(d + 16), b);
      _mm_store_si128(reinterpret_cast<__m128i*>(d + 32), c);
      _mm_store_si128(reinterpret_cast<__m128i*>(d + 48), e);
    }
    for (; bytes >= 16; bytes -= 16, d += 16, s += 16) _mm_store_si128(reinterpret_cast<__m128i*>(d), _mm_loadu_si128(reinterpret_cast<const __m128i*>(s)));
  }
  if (bytes) std::memcpy(d, s, bytes);
}

// the common case, 32-bit components: a fixed size per vertex lets the compiler move whole vectors
template <uint32_t N>
void SwizzleWords(uint8_t* data, size_t vertices, uint32_t stride, uint32_t offset, const uint32_t* source, uint32_t one) {
  for (size_t v = 0; v < vertices; ++v) {
    uint8_t* at = data + v * stride + offset;
    uint32_t orig[6] = {0, 0, 0, 0, one, 0}, out[N];
    std::memcpy(orig, at, 4 * N);
    for (uint32_t i = 0; i < N; ++i) out[i] = orig[source[i]];
    std::memcpy(at, out, 4 * N);
  }
}

void ApplySwizzle(uint8_t* data, size_t vertices, uint32_t stride, const SwizzleFix& fix) {
  uint32_t count = 0, bytes = 0, one = 0;
  if (!VertexComponents(fix.format, &count, &bytes, &one)) return;
  // what every output component takes, decided once: a component of the original, the constant one, zero, or (unlisted) its own value
  constexpr uint32_t kOne = 4, kZero = 5;
  uint32_t source[4] = {0, 1, 2, 3};
  bool changes = false;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t c = (fix.container >> (3 * i)) & 7, l = (fix.live >> (3 * i)) & 7;
    if (c >= count || l > 5) continue;   // a live code of 6 or 7 leaves the component as it is
    source[c] = l < count ? l : (l == 5 || l == 3) ? kOne : kZero;
    changes |= source[c] != c;
  }
  if (!changes) return;
  if (bytes == 4 && count == 3) return SwizzleWords<3>(data, vertices, stride, fix.offset, source, one);
  if (bytes == 4 && count == 4) return SwizzleWords<4>(data, vertices, stride, fix.offset, source, one);
  for (size_t v = 0; v < vertices; ++v) {
    uint8_t* at = data + v * stride + fix.offset;
    uint32_t orig[6] = {0, 0, 0, 0, one, 0}, out[4];
    if (bytes == 4) {
      std::memcpy(orig, at, 4 * count);
    } else {
      for (uint32_t i = 0; i < count; ++i) std::memcpy(&orig[i], at + i * bytes, bytes);   // little endian host: the low bytes
    }
    for (uint32_t i = 0; i < count; ++i) out[i] = orig[source[i]];
    if (bytes == 4) {
      std::memcpy(at, out, 4 * count);
    } else {
      for (uint32_t i = 0; i < count; ++i) std::memcpy(at + i * bytes, &out[i], bytes);
    }
  }
}

}  // namespace

bool Renderer::Impl::RefreshShader(ActiveShader& shader, bool vertex) {
  (void)vertex;
  if (!shader.info) return false;
  if (shader.immediate) {   // the microcode came in the command stream: it is exactly what the GPU runs
    if (!shader.scanned) {
      shader.usage = gh2gfx::d3d::ScanFetches(shader.microcode.data(), shader.microcode.size());
      shader.scanned = true;
    }
    return shader.usage.valid;
  }
  if (!shader.size_dwords || shader.size_dwords > 4096 || shader.address + shader.size_dwords * 4ull > kGuestMemoryLimit) return false;
  const uint8_t* source = config.memory.Physical(shader.address);
  thread_local std::vector<uint32_t> words;
  words.resize(shader.size_dwords);
  for (uint32_t i = 0; i < shader.size_dwords; ++i) {
    uint32_t v;
    std::memcpy(&v, source + 4 * i, 4);
    words[i] = __builtin_bswap32(v);
  }
  if (!shader.at_load.empty()) {
    ++microcode_checked;
    if (shader.at_load != words) ++microcode_changed;
  }
  if (!shader.scanned || shader.microcode != words) {
    shader.microcode = words;
    shader.usage = gh2gfx::d3d::ScanFetches(words.data(), words.size());
    shader.scanned = true;
  }
  return shader.usage.valid;
}

VkPipeline Renderer::Impl::BuildPipeline(const PipelineDesc& d) {
  const VkSpecializationMapEntry entry = {0, 0, 4};
  VkSpecializationInfo spec_vs = {1, &entry, 4, &d.vs_spec}, spec_ps = {1, &entry, 4, &d.ps_spec};
  VkPipelineShaderStageCreateInfo stages[2] = {};
  uint32_t n = 0;
  stages[n].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[n].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[n].module = d.vs;
  stages[n].pName = "main";
  stages[n].pSpecializationInfo = &spec_vs;
  ++n;
  if (d.ps) {
    stages[n].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[n].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[n].module = d.ps;
    stages[n].pName = "main";
    stages[n].pSpecializationInfo = &spec_ps;
    ++n;
  }

  VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vi.vertexBindingDescriptionCount = static_cast<uint32_t>(d.bindings.size());
  vi.pVertexBindingDescriptions = d.bindings.data();
  vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(d.attributes.size());
  vi.pVertexAttributeDescriptions = d.attributes.data();
  VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = d.topology;
  ia.primitiveRestartEnable = d.primitive_restart ? VK_TRUE : VK_FALSE;
  VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1;
  vp.scissorCount = 1;

  const bool triangles = d.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST || d.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || d.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
  const bool cull_front = d.mode_control & 1, cull_back = (d.mode_control >> 1) & 1;
  VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.lineWidth = 1.f;
  rs.cullMode = (d.rect || !triangles) ? VK_CULL_MODE_NONE : (cull_front && cull_back ? VK_CULL_MODE_FRONT_AND_BACK : cull_front ? VK_CULL_MODE_FRONT_BIT : cull_back ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE);
  rs.frontFace = ((d.mode_control >> 2) & 1) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rs.depthBiasEnable = (((d.mode_control >> 11) & 1) || ((d.mode_control >> 12) & 1)) ? VK_TRUE : VK_FALSE;
  VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = d.samples == 4 ? VK_SAMPLE_COUNT_4_BIT : d.samples == 2 ? VK_SAMPLE_COUNT_2_BIT : VK_SAMPLE_COUNT_1_BIT;

  const uint32_t dc = d.depth_control;
  const bool stencil_enable = dc & 1, z_enable = (dc >> 1) & 1, z_write = (dc >> 2) & 1, backface = (dc >> 7) & 1;
  VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable = z_enable ? VK_TRUE : VK_FALSE;
  ds.depthWriteEnable = (z_enable && z_write) ? VK_TRUE : VK_FALSE;
  ds.depthCompareOp = xenos::CompareOp((dc >> 4) & 7);
  ds.stencilTestEnable = stencil_enable ? VK_TRUE : VK_FALSE;
  ds.front.compareOp = xenos::CompareOp((dc >> 8) & 7);
  ds.front.failOp = xenos::StencilOp((dc >> 11) & 7);
  ds.front.passOp = xenos::StencilOp((dc >> 14) & 7);
  ds.front.depthFailOp = xenos::StencilOp((dc >> 17) & 7);
  if (backface) {
    ds.back.compareOp = xenos::CompareOp((dc >> 20) & 7);
    ds.back.failOp = xenos::StencilOp((dc >> 23) & 7);
    ds.back.passOp = xenos::StencilOp((dc >> 26) & 7);
    ds.back.depthFailOp = xenos::StencilOp((dc >> 29) & 7);
  } else {
    ds.back = ds.front;
  }

  VkPipelineColorBlendAttachmentState att = {};
  att.colorWriteMask = d.color_mask;
  if (d.blend) {
    att.blendEnable = VK_TRUE;
    att.srcColorBlendFactor = xenos::BlendFactor(d.blend & 31);
    att.colorBlendOp = xenos::BlendOp((d.blend >> 5) & 7);
    att.dstColorBlendFactor = xenos::BlendFactor((d.blend >> 8) & 31);
    att.srcAlphaBlendFactor = xenos::BlendFactor((d.blend >> 16) & 31);
    att.alphaBlendOp = xenos::BlendOp((d.blend >> 21) & 7);
    att.dstAlphaBlendFactor = xenos::BlendFactor((d.blend >> 24) & 31);
  }
  VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = d.color_format != VK_FORMAT_UNDEFINED ? 1 : 0;
  cb.pAttachments = &att;

  const VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT,       VK_DYNAMIC_STATE_SCISSOR,           VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                                    VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_DEPTH_BIAS};
  VkPipelineDynamicStateCreateInfo dyn = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dyn.dynamicStateCount = sizeof dynamic / sizeof dynamic[0];
  dyn.pDynamicStates = dynamic;

  const VkFormat depth_format = VK_FORMAT_D32_SFLOAT_S8_UINT;
  VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  ri.colorAttachmentCount = d.color_format != VK_FORMAT_UNDEFINED ? 1 : 0;
  ri.pColorAttachmentFormats = &d.color_format;
  ri.depthAttachmentFormat = depth_format;
  ri.stencilAttachmentFormat = depth_format;

  VkGraphicsPipelineCreateInfo pci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pci.pNext = &ri;
  pci.stageCount = n;
  pci.pStages = stages;
  pci.pVertexInputState = &vi;
  pci.pInputAssemblyState = &ia;
  pci.pViewportState = &vp;
  pci.pRasterizationState = &rs;
  pci.pMultisampleState = &ms;
  pci.pDepthStencilState = &ds;
  pci.pColorBlendState = &cb;
  pci.pDynamicState = &dyn;
  pci.layout = pipeline_layout;
  VkPipeline pipeline = VK_NULL_HANDLE;
  if (vkCreateGraphicsPipelines(vk.device(), pipeline_cache, 1, &pci, nullptr, &pipeline) != VK_SUCCESS) {
    std::fprintf(stderr, "RENDER pipeline creation failed\n");
    pipeline = VK_NULL_HANDLE;
  }
  ++stats.pipelines_created;
  return pipeline;
}

void Renderer::Impl::Draw(const uint32_t* regs, const gh2gpu::DrawParams& d) {
  namespace r = xenos::reg;
  using Clk = std::chrono::steady_clock;
  // the per-section timers cost a clock read each (about 3% of this thread): one draw in 16 is timed and counted 16 times
  static thread_local uint32_t timing_tick = 0;
  const bool timed = (++timing_tick & 15) == 0;
  Clk::time_point mark = timed ? Clk::now() : Clk::time_point();
  auto lap = [&](int section) {
    if (!timed) return;
    const Clk::time_point now = Clk::now();
    section_ns[section] += 16 * std::chrono::duration_cast<std::chrono::nanoseconds>(now - mark).count();
    mark = now;
  };
  const uint32_t mode = regs[r::RB_MODECONTROL] & 7;
  const uint32_t depth_control = regs[r::RB_DEPTHCONTROL];
  const bool z_write = ((depth_control >> 1) & 1) && ((depth_control >> 2) & 1);
  const bool stencil = depth_control & 1;

  int reason = 0;
  auto skip = [&]() {
    ++stats.draws_skipped;
    ++skip_reasons[reason];
  };
  static const uint64_t stats_frame = std::getenv("GH2_RENDER_LOG_FRAME") ? std::strtoull(std::getenv("GH2_RENDER_LOG_FRAME"), nullptr, 10) : ~0ull;
  const bool want_vertex_stats = frame >= stats_frame && frame < stats_frame + 3;
  std::string vertex_stats;   // min/max of the position and normal attributes of the draw (debugging, GH2_RENDER_LOG_FRAME)

  uint32_t color_mask = mode == 5 ? 0 : (regs[r::RB_COLOR_MASK] & 0xF);
  reason = 1;
  if (!active_vertex.info) {
    static std::unordered_map<uint64_t, uint64_t> by_kind;   // draws skipped because their vertex shader is not one the cache knows: counted by (primitive, mode, pixel shader known, indices)
    const uint64_t kind = (static_cast<uint64_t>(d.prim_type) << 40) | (static_cast<uint64_t>(mode) << 32) | (static_cast<uint64_t>(active_pixel.info ? 1 : 0) << 24) | std::min<uint32_t>(d.num_indices, 0xFFFFFF);
    if (by_kind[kind]++ == 0)
      std::fprintf(stderr, "RENDER draw skipped, unidentified vertex shader (immediate %d, %u dwords): prim %u, mode %u, %u indices, pixel shader %s, color mask %X, depth control %08X, rt %08X/%08X\n", active_vertex.immediate, active_vertex.size_dwords,
                   d.prim_type, mode, d.num_indices, active_pixel.info ? "known" : "unknown", regs[r::RB_COLOR_MASK] & 0xF, depth_control, regs[r::RB_COLOR_INFO], regs[r::RB_DEPTH_INFO]);
    return skip();
  }
  reason = 2;
  if (!RefreshShader(active_vertex, true)) return skip();
  const bool have_ps = active_pixel.info && RefreshShader(active_pixel, false);
  if (!have_ps) color_mask = 0;
  reason = 0;
  if (!color_mask && !z_write && !stencil) return skip();
  reason = 3;   // the library's no-op draws (a point without shaders, colour masked off)

  lap(0);
  // ---- primitive ----
  VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  bool rect = false, quad = false;
  switch (d.prim_type) {
    case 1: return skip();   // point lists: the translated vertex shaders write no point size (the game uses them only for no-op draws)
    case 2: topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    case 3: topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
    case 4: break;
    case 5: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
    case 6: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
    case 8: rect = true; break;
    case 13: quad = true; break;
    default: return skip();
  }
  const uint32_t count = d.num_indices;
  if (!count) return skip();
  const uint32_t rects = rect ? count / 3 : 0;
  if (rect && (d.source_select == 0 || count % 3)) return skip();   // only auto-indexed rectangle lists are expanded
  const uint32_t mode_control = regs[r::PA_SU_SC_MODE_CNTL];
  const bool restart = ((mode_control >> 21) & 1) && (topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN || topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP);

  // ---- indices ----
  reason = 4;
  const bool indexed = d.source_select == 0;
  thread_local std::vector<uint32_t> idx, out;
  thread_local std::vector<uint8_t> raw;
  uint32_t min_v = 0, max_v = count - 1;
  if (indexed) {
    const uint32_t isz = d.index32 ? 4 : 2;
    if (!d.dma_base || d.dma_base + static_cast<uint64_t>(count) * isz > kGuestMemoryLimit) return skip();
    raw.resize(static_cast<size_t>(count) * isz);
    std::memcpy(raw.data(), config.memory.Physical(d.dma_base), raw.size());
    xenos::SwapEndian(raw.data(), raw.size(), static_cast<uint32_t>(d.dma_endian));
    idx.resize(count);
    const uint32_t reset = restart ? (regs[r::VGT_MULTI_PRIM_IB_RESET_INDX] & (d.index32 ? 0xFFFFFFFFu : 0xFFFFu)) : 0xFFFFFFFFu;
    min_v = 0xFFFFFFFFu;
    max_v = 0;
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t v;
      if (d.index32) std::memcpy(&v, raw.data() + 4 * i, 4);
      else {
        uint16_t v16;
        std::memcpy(&v16, raw.data() + 2 * i, 2);
        v = v16;
      }
      if (restart && v == reset) {
        idx[i] = 0xFFFFFFFFu;
        continue;
      }
      idx[i] = v;
      min_v = std::min(min_v, v);
      max_v = std::max(max_v, v);
    }
    if (min_v > max_v) return skip();
  }
  const int64_t index_bias = static_cast<int32_t>(regs[r::VGT_INDX_OFFSET]);
  const int64_t first_vertex = static_cast<int64_t>(min_v) + index_bias;
  if (first_vertex < 0) return skip();
  const uint32_t vertex_count = max_v - min_v + 1;   // vertices read from guest memory
  const bool wide = (rect ? rects * 4 : vertex_count) >= 0xFFFF;
  const bool need_indices = indexed || quad || rect;
  uint32_t index_count = count;
  if (need_indices) {
    out.clear();
    auto at = [&](uint32_t i) -> uint32_t {
      if (!indexed) return i;
      return idx[i] == 0xFFFFFFFFu ? idx[i] : idx[i] - min_v;
    };
    if (rect) {
      for (uint32_t k = 0; k < rects; ++k) {
        const uint32_t b = 4 * k;
        out.insert(out.end(), {b, b + 1, b + 2, b + 1, b + 2, b + 3});
      }
    } else if (quad) {
      for (uint32_t q = 0; q + 3 < count; q += 4) {
        const uint32_t a = at(q), b = at(q + 1), c = at(q + 2), e = at(q + 3);
        out.insert(out.end(), {a, b, c, a, c, e});
      }
    } else {
      out.resize(count);
      for (uint32_t i = 0; i < count; ++i) out[i] = at(i);
    }
    index_count = static_cast<uint32_t>(out.size());
    if (!index_count) return skip();
  }

  lap(1);
  // ---- vertex input: the vertex shader's fetches matched with the container's element table ----
  PipelineDesc desc;
  struct Stream {
    uint32_t slot, stride;
    VkDeviceSize offset;
  };
  Stream streams[8];
  uint32_t stream_count = 0;
  bool packed_normals = false;
  struct Conversion {
    uint32_t slot, offset, components;
    bool is_signed;
  } conversions[8];
  uint32_t conversion_count = 0;
  // vertex elements the shader reads through its own index (patch 0004: the stream is described in the shared constants, no input is bound): indexed by the element's place in the table
  struct DependentStream {
    bool used = false, is_signed = false, integer = false;
    uint32_t slot = 0, stride = 0, offset = 0, format = 0;   // bytes
    SwizzleFix fix;
  } dependents[8];
  const uint32_t dependent_mask = DependentMask(active_vertex.info->hash);
  SwizzleFix swizzle_fixes[8];
  uint32_t swizzle_fix_count = 0;
  struct ConstantAttribute {
    uint32_t location;
    bool as_uint;
    float values[4];
  } constant_attrs[8];
  uint32_t constant_count = 0;
  uint32_t slot_stride[96] = {};   // dwords; a mini fetch has none of its own and reads with the stride of the full fetch before it
  for (const gh2gfx::d3d::VertexFetch& vf_in : active_vertex.usage.vertex_fetches) {
    gh2gfx::d3d::VertexFetch vf = vf_in;
    if (vf.slot < 96) {
      if (vf.stride) slot_stride[vf.slot] = vf.stride;
      else vf.stride = static_cast<uint8_t>(slot_stride[vf.slot]);
    }
    // which element this fetch reads: the library rewrites the fetches of a vertex shader to the vertex declaration and may reorder them, but a fetch keeps its destination register and swizzle
    // (the library also rewrites the destination swizzle to the component count of the declared type, so the register decides and the swizzle only tells apart two elements sharing one)
    const VertexElementInfo* element = nullptr;
    for (const VertexElementInfo& e : active_vertex.info->elements) {
      if (e.dst != vf.dst) continue;
      if (e.dst_swizzle == vf.dst_swizzle) {
        element = &e;
        break;
      }
      if (!element) element = &e;
    }
    if (!element)
      for (const VertexElementInfo& e : active_vertex.info->elements)
        if (e.instruction == vf.instruction) {
          element = &e;
          break;
        }
    auto dropped = [&](const char* why) {
      static std::unordered_map<uint64_t, bool> reported;
      const uint64_t key = active_vertex.info->hash ^ (static_cast<uint64_t>(vf.instruction) << 56);
      if (!reported[key]) {
        reported[key] = true;
        std::fprintf(stderr, "RENDER vertex attribute dropped (%s): vs=%016llX instruction %u fmt %u stride %u offset %d usage %d/%d\n", why, (unsigned long long)active_vertex.info->hash, vf.instruction,
                     vf.format, vf.stride, vf.offset, element ? element->usage : -1, element ? element->usage_index : -1);
      }
    };
    if (!element) {
      dropped("not in the container's element table");
      continue;
    }
    if (!vf.stride || vf.offset < 0) {
      dropped("no stride or negative offset");
      continue;
    }
    const uint32_t element_index = static_cast<uint32_t>(element - active_vertex.info->elements.data());
    if (element_index < 8 && ((dependent_mask >> element_index) & 1)) {
      DependentStream& ds = dependents[element_index];
      if (!ds.used) {
        ds = {true, vf.is_signed != 0, vf.integer != 0, vf.slot, vf.stride * 4u, static_cast<uint32_t>(vf.offset) * 4u, vf.format, {}};
        if (vf.dst_swizzle != element->dst_swizzle) ds.fix = {true, vf.slot, static_cast<uint32_t>(vf.offset) * 4u, vf.format, vf.dst_swizzle, element->dst_swizzle};
      }
      continue;
    }
    const int location = UsageLocation(element->usage, element->usage_index);
    if (location < 0) {
      dropped("usage has no input location");
      continue;
    }
    {   // a fetch whose destination swizzle selects only constants (0 or 1) reads nothing: the library switches off the fetches a vertex declaration does not have this way (its tangent, say)
      // and the shader gets (0, 0, 0, 1) here, not whatever bytes sit at the fetch's offset
      bool constants_only = true;
      float values[4];
      for (int c = 0; c < 4; ++c) {
        const uint32_t sel = (vf.dst_swizzle >> (3 * c)) & 7;
        constants_only = constants_only && (sel == 4 || sel == 5);
        values[c] = sel == 5 ? 1.f : 0.f;
      }
      if (constants_only) {
        if (constant_count < 8) {
          constant_attrs[constant_count++] = {static_cast<uint32_t>(location), UsageIsUint(element->usage), {values[0], values[1], values[2], values[3]}};
        }
        continue;
      }
    }
    if (vf.dst_swizzle != element->dst_swizzle) {   // the library gave this fetch another destination swizzle than the container's: reported once per shader and element
      static std::unordered_map<uint64_t, bool> seen;
      const uint64_t key = active_vertex.info->hash ^ (static_cast<uint64_t>(element->usage) << 60) ^ (static_cast<uint64_t>(vf.dst_swizzle) << 40);
      if (!seen[key]) {
        seen[key] = true;
        std::fprintf(stderr, "RENDER fetch swizzle differs: vs=%016llX usage %u/%u fmt %u live %03X container %03X\n", (unsigned long long)active_vertex.info->hash, element->usage, element->usage_index, vf.format, vf.dst_swizzle,
                     element->dst_swizzle);
      }
    }
    if (vf.dst_swizzle != element->dst_swizzle && swizzle_fix_count < 8) swizzle_fixes[swizzle_fix_count++] = {true, vf.slot, static_cast<uint32_t>(vf.offset) * 4u, vf.format, vf.dst_swizzle, element->dst_swizzle};
    uint32_t bytes = 0;
    // 32-bit integer elements feeding a float input (the hardware converts integers to floats on fetch; Vulkan has no such vertex format): converted on the host while uploading
    const bool int_to_float = (vf.format >= 33 && vf.format <= 35) && !UsageIsUint(element->usage);
    VkFormat format = int_to_float ? (vf.format == 33 ? VK_FORMAT_R32_SFLOAT : vf.format == 34 ? VK_FORMAT_R32G32_SFLOAT : VK_FORMAT_R32G32B32A32_SFLOAT)
                                   : xenos::VertexVkFormat(vf.format, vf.is_signed, vf.integer, UsageIsUint(element->usage), &bytes);
    if (int_to_float) bytes = vf.format == 33 ? 4 : vf.format == 34 ? 8 : 16;
    if (format == VK_FORMAT_UNDEFINED) {
      dropped("vertex format not supported");
      continue;
    }
    if ((vf.offset * 4u + bytes) > vf.stride * 4u) {
      dropped("element larger than the stride");
      continue;
    }
    if (int_to_float && conversion_count < 8) conversions[conversion_count++] = {vf.slot, vf.offset * 4u, vf.format == 33 ? 1u : vf.format == 34 ? 2u : 4u, vf.is_signed != 0};
    if (UsageIsUint(element->usage) && (vf.format == 16 || vf.format == 17)) packed_normals = true;
    uint32_t binding = stream_count;
    for (uint32_t i = 0; i < stream_count; ++i)
      if (streams[i].slot == vf.slot) binding = i;
    if (binding == stream_count) {
      if (stream_count == 8) continue;
      streams[stream_count++] = {vf.slot, vf.stride * 4u, 0};
      desc.bindings.push_back({binding, vf.stride * 4u, VK_VERTEX_INPUT_RATE_VERTEX});
    }
    bool duplicate = false;   // two fetches that resolve to the same input (the shader reads it once): the first one is used
    for (const auto& a : desc.attributes) duplicate = duplicate || a.location == static_cast<uint32_t>(location);
    if (duplicate) {
      dropped("another fetch feeds the same input");
      continue;
    }
    desc.attributes.push_back({static_cast<uint32_t>(location), binding, format, vf.offset * 4u});
  }

  uint32_t constant_binding = 0;
  for (uint32_t k = 0; k < constant_count; ++k) {   // constants-only fetches become attributes of one stride-0 binding (unless a real fetch feeds the same input)
    bool taken = false;
    for (const auto& a : desc.attributes) taken = taken || a.location == constant_attrs[k].location;
    for (uint32_t j = 0; j < k; ++j) taken = taken || constant_attrs[j].location == constant_attrs[k].location;
    if (taken) {
      constant_attrs[k].location = ~0u;
      continue;
    }
    if (!constant_binding) {
      constant_binding = stream_count + 1;
      desc.bindings.push_back({stream_count, 0, VK_VERTEX_INPUT_RATE_VERTEX});
    }
    desc.attributes.push_back({constant_attrs[k].location, stream_count, constant_attrs[k].as_uint ? VK_FORMAT_R32G32B32A32_UINT : VK_FORMAT_R32G32B32A32_SFLOAT, 16u * k});
  }
  bool any_dependent = false;
  for (const DependentStream& ds : dependents) any_dependent = any_dependent || ds.used;
  if (desc.attributes.empty() && !any_dependent && !active_vertex.info->elements.empty()) {   // the shader reads inputs but none of its fetches could be matched: the microcode was not patched yet
    static std::unordered_map<uint64_t, bool> reported;
    if (!reported[active_vertex.info->hash]) {
      reported[active_vertex.info->hash] = true;
      std::fprintf(stderr, "RENDER vertex shader %016llX has no usable vertex input: microcode at %08X, %u dwords, %zu fetches, %zu container elements, first dwords %08X %08X %08X\n",
                   (unsigned long long)active_vertex.info->hash, active_vertex.address, active_vertex.size_dwords, active_vertex.usage.vertex_fetches.size(), active_vertex.info->elements.size(),
                   active_vertex.microcode.size() > 0 ? active_vertex.microcode[0] : 0, active_vertex.microcode.size() > 1 ? active_vertex.microcode[1] : 0,
                   active_vertex.microcode.size() > 2 ? active_vertex.microcode[2] : 0);
    }
    reason = 9;
    return skip();
  }
  static const bool log_layouts = std::getenv("GH2_RENDER_LOG_LAYOUTS") != nullptr;
  if (log_layouts) {   // every distinct vertex layout once: what the microcode fetches and what the container says each fetch feeds
    static std::unordered_map<uint64_t, bool> seen;
    if (!seen[active_vertex.info->hash]) {
      seen[active_vertex.info->hash] = true;
      std::fprintf(stderr, "LAYOUT vs=%016llX prim=%u attrs=%zu:", (unsigned long long)active_vertex.info->hash, d.prim_type, desc.attributes.size());
      for (const gh2gfx::d3d::VertexFetch& vf : active_vertex.usage.vertex_fetches) {
        const VertexElementInfo* el = nullptr;
        for (const VertexElementInfo& e : active_vertex.info->elements)
          if (e.instruction == vf.instruction) el = &e;
        std::fprintf(stderr, " [i%u slot%u stride%u off%d fmt%u s%d n%d%s usage=%d/%d]", vf.instruction, vf.slot, vf.stride, vf.offset, vf.format, vf.is_signed, vf.integer, vf.mini ? " mini" : "",
                     el ? el->usage : -1, el ? el->usage_index : -1);
      }
      std::fprintf(stderr, "\n");
    }
  }
  if (active_vertex.usage.memexport_instructions) {   // the shader writes data to memory (Xenos memexport): not emulated, report once and count
    static std::unordered_map<uint64_t, bool> reported;
    ++memexport_draws;
    if (!reported[active_vertex.info->hash]) {
      reported[active_vertex.info->hash] = true;
      std::fprintf(stderr, "RENDER vertex shader %016llX exports to memory (%u instructions): prim %u, %u indices, vertex buffer stream 0 %08X, color mask %X\n", (unsigned long long)active_vertex.info->hash,
                   active_vertex.usage.memexport_instructions, d.prim_type, count, stream_count ? xenos::DecodeVertexFetch(&regs[r::SHADER_CONSTANT_FETCH + 2 * streams[0].slot]).address_dwords * 4 : 0u, color_mask);
    }
  }
  // ---- shaders ----
  reason = 5;
  uint32_t vs_mask = 0, ps_mask = 0;
  desc.vs_hash = active_vertex.info->hash;
  desc.vs = GetShaderModule(desc.vs_hash, gh2gfx::ShaderStage::kVertex, &vs_mask);
  if (!desc.vs) return skip();
  if (have_ps) {
    desc.ps_hash = active_pixel.info->hash;
    desc.ps = GetShaderModule(desc.ps_hash, gh2gfx::ShaderStage::kPixel, &ps_mask);
    if (!desc.ps) return skip();
  }
  desc.vs_spec = (vs_mask & 1) && packed_normals ? 1u : 0u;
  const uint32_t color_control = regs[r::RB_COLORCONTROL];
  if (color_control & 0x10) ++alpha_to_mask_draws;   // RB_COLORCONTROL bit 4: the game asks for alpha-to-coverage (not emulated yet; counted)
  const uint32_t alpha_func = color_control & 7;
  desc.ps_spec = ((ps_mask & 2) && ((color_control >> 3) & 1) && (alpha_func == 4 || alpha_func == 6)) ? 2u : 0u;

  // ---- targets ----
  reason = 6;
  const uint32_t color_info = regs[r::RB_COLOR_INFO], depth_info = regs[r::RB_DEPTH_INFO];
  const uint32_t samples = TargetSamples(regs);
  TargetImage* color = mode == 5 ? nullptr : GetColorTarget(xenos::ColorBase(color_info), xenos::ColorFormat(color_info), samples);
  TargetImage* depth = GetDepthTarget(xenos::DepthBase(depth_info), xenos::DepthFormat(depth_info), samples);
  if (!depth || (mode != 5 && !color)) return skip();
  desc.samples = depth->samples;

  lap(2);
  // ---- textures: uploads may need the command buffer outside a render pass ----
  uint32_t shared[112] = {};   // 80: the fixed part; 80 + 4 * element: the dependent vertex streams (address low/high, stride, meta)
  const uint32_t blend_reg = regs[r::RB_BLENDCONTROL0];   // translucent: anything but "source one, destination zero" for colour and alpha
  const bool blended_draw = !((blend_reg & 31) == 1 && ((blend_reg >> 8) & 31) == 0 && ((blend_reg >> 5) & 7) == 0 && ((blend_reg >> 16) & 31) == 1 && ((blend_reg >> 24) & 31) == 0 && ((blend_reg >> 21) & 7) == 0) && mode != 5;
  uint32_t texture_slots = active_vertex.usage.texture_slots | (have_ps ? active_pixel.usage.texture_slots : 0);
  for (uint32_t s = 0; s < 16; ++s) {
    if (!((texture_slots >> s) & 1)) continue;
    uint32_t sampler = 0, heap = 0;
    const bool multitap = have_ps && active_pixel.usage.texture_fetch_count[s] >= 2;   // blur / bloom: several taps of one texture
    const uint32_t index = PrepareTexture(&regs[r::SHADER_CONSTANT_FETCH + 6 * s], &sampler, &heap, multitap, blended_draw);
    shared[(heap == 2 ? 32 : 0) + s] = index;
    shared[48 + s] = sampler;
    if (frame == shot_log_frame.load(std::memory_order_relaxed)) {   // the frame of an F9 picture: which texture every slot of every draw reads
      const xenos::TextureFetch tf = xenos::DecodeTextureFetch(&regs[r::SHADER_CONSTANT_FETCH + 6 * s]);
      const TargetImage* snap = FindResolvedAny(tf.base_address << 12);
      std::fprintf(stderr, "SHOTLOG texture for draw #%llu slot %u: base %08X %ux%u format %u type %u -> heap %u%s%s%s\n", (unsigned long long)(stats.draws_issued.load() + 1), s, tf.base_address << 12, tf.width, tf.height, tf.format, tf.type, index,
                   snap ? " (render target snapshot " : "", snap ? (snap->depth ? "depth)" : "colour)") : "", index == 0 ? " FALLBACK TEXEL" : "");
    }
  }
  shared[64] = (regs[r::SHADER_CONSTANT_BOOL] & 0xFFFF) | ((regs[r::SHADER_CONSTANT_BOOL + 4] & 0xFFFF) << 16);
  shared[65] = 0;
  // how the vertex shader's position reaches clip space (xenos shader patch 0003): positions are window coordinates when the viewport transform is off (2D, screen-space vertices),
  // already normalised device coordinates when it is on. Measured on the game's own frames (the FMV, the logos): with the viewport the registers describe, every picture comes out
  // mirrored vertically (and the triangles' winding with it, so the game's back-face culling removes the whole interface), so y is negated on the way to clip space, in both cases.
  // The Xenos samples at pixel centres: no half-pixel correction.
  const uint32_t vte = regs[r::PA_CL_VTE_CNTL];
  const bool viewport_on = (vte & 1) && (vte & 2) && (vte & 4) && (vte & 8);
  const float ndc_scale[2] = {viewport_on ? 1.f : 2.f / kGuestWidth, viewport_on ? -1.f : -2.f / kGuestHeight},   // window coordinates are the guest's: the viewport below spans the (scaled) target
                     ndc_offset[2] = {viewport_on ? 0.f : -1.f, viewport_on ? 0.f : 1.f};
  std::memcpy(&shared[66], ndc_offset, sizeof ndc_offset);
  std::memcpy(&shared[70], ndc_scale, sizeof ndc_scale);
  {   // the translated shader discards when alpha - threshold < 0, which is "alpha >= ref" (function 6); "alpha > ref" (function 4) needs a threshold above ref, or a pixel of alpha exactly 0 survives
    // a test against 0 (the crowd's billboards are cut out that way). Half a step of an 8-bit alpha: the next float up would be a denormal, which the GPU flushes to zero.
    float ref = xenos::AsFloat(regs[r::RB_ALPHA_REF]);
    if ((regs[r::RB_COLORCONTROL] & 7) == 4) ref += 0.5f / 255.f;
    std::memcpy(&shared[68], &ref, 4);
  }
  // r0.x of a Xenos vertex shader is the vertex index; the indices of the draw were rebased to start at 0 (shader patch 0003 adds the base back)
  const float vertex_base = static_cast<float>(first_vertex);
  std::memcpy(&shared[72], &vertex_base, 4);

  lap(3);
  // ---- uploads ----
  reason = 7;
  VkDeviceSize offset = 0;
  for (uint32_t i = 0; i < stream_count; ++i) {
    const uint32_t* fc = &regs[r::SHADER_CONSTANT_FETCH + 2 * streams[i].slot];
    const xenos::VertexFetch vc = xenos::DecodeVertexFetch(fc);
    const uint64_t start = static_cast<uint64_t>(vc.address_dwords) * 4 + static_cast<uint64_t>(first_vertex) * streams[i].stride;
    const uint64_t bytes = static_cast<uint64_t>(vertex_count) * streams[i].stride;
    if (start + bytes > kGuestMemoryLimit) return skip();
    bool plain = !rect && !want_vertex_stats;   // nothing to fix after the swap: guest memory -> swapped -> ring in one pass
    for (uint32_t c = 0; c < swizzle_fix_count && plain; ++c) plain = swizzle_fixes[c].slot != streams[i].slot;
    for (uint32_t c = 0; c < conversion_count && plain; ++c) plain = conversions[c].slot != streams[i].slot;
    if (plain) {
      const uint8_t* source = config.memory.Physical(static_cast<uint32_t>(start));
      uint8_t* dst = RingAlloc(bytes, 16, &offset);
      if (!dst) return skip();
      up_vertex_bytes += bytes;
      xenos::SwapEndianCopy(dst, source, static_cast<size_t>(bytes), vc.endian);
      static const bool verify_upload = std::getenv("GH2_RENDER_VERIFY_UPLOAD") != nullptr;   // test aid: the one-pass upload against the two-step way (reads the ring back: slow)
      if (verify_upload) {
        static std::atomic<uint64_t> checked{0}, bad{0};
        std::vector<uint8_t> reference(static_cast<size_t>(bytes));
        std::memcpy(reference.data(), source, bytes);
        xenos::SwapEndian(reference.data(), reference.size(), vc.endian);
        ++checked;
        if (std::memcmp(reference.data(), dst, reference.size()) != 0) ++bad;
        if ((checked.load() & 1023) == 0 || bad.load() == 1) std::fprintf(stderr, "VERIFY upload: %llu streams checked, %llu different\n", (unsigned long long)checked.load(), (unsigned long long)bad.load());
      }
      streams[i].offset = offset;
      continue;
    }
    // the endian swap happens in ordinary memory: the ring is write-combined, reading it back is very slow
    thread_local std::vector<uint8_t> vertices;
    vertices.resize(static_cast<size_t>(bytes));
    std::memcpy(vertices.data(), config.memory.Physical(static_cast<uint32_t>(start)), bytes);
    xenos::SwapEndian(vertices.data(), vertices.size(), vc.endian);
    const uint32_t stride = streams[i].stride;
    if (want_vertex_stats) {
      for (const auto& a : desc.attributes) {
        if (a.binding != i || (a.location != 0 && a.location != 1) || (a.format != VK_FORMAT_R32G32B32_SFLOAT && a.format != VK_FORMAT_R32G32B32_UINT)) continue;
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        uint32_t bad = 0, used = 0;
        const uint32_t refs = need_indices ? static_cast<uint32_t>(out.size()) : vertex_count;
        for (uint32_t ri = 0; ri < refs; ++ri) {   // only vertices some index refers to (the range min..max may hold unused ones)
          const uint32_t v = need_indices ? out[ri] : ri;
          if (v >= vertex_count) continue;
          ++used;
          for (int k = 0; k < 3; ++k) {
            float f;
            std::memcpy(&f, vertices.data() + static_cast<size_t>(v) * stride + a.offset + 4 * k, 4);
            if (!(f > -1e20f && f < 1e20f)) {   // not a usable coordinate
              ++bad;
              continue;
            }
            lo[k] = std::min(lo[k], f);
            hi[k] = std::max(hi[k], f);
          }
        }
        char buf[220];
        std::snprintf(buf, sizeof buf, " %s[%.3g..%.3g,%.3g..%.3g,%.3g..%.3g bad%u of %u refs, vertices %u, stride %u]", a.location == 0 ? "pos" : "nrm", lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], bad, used * 3,
                      vertex_count, stride);
        vertex_stats += buf;
      }
    }
    for (uint32_t c = 0; c < swizzle_fix_count; ++c)
      if (swizzle_fixes[c].slot == streams[i].slot) ApplySwizzle(vertices.data(), vertex_count, stride, swizzle_fixes[c]);
    for (uint32_t c = 0; c < conversion_count; ++c) {
      if (conversions[c].slot != streams[i].slot) continue;
      for (uint32_t v = 0; v < vertex_count; ++v)
        for (uint32_t k = 0; k < conversions[c].components; ++k) {
          uint8_t* at = vertices.data() + static_cast<size_t>(v) * stride + conversions[c].offset + 4 * k;
          uint32_t raw;
          std::memcpy(&raw, at, 4);
          const float f = conversions[c].is_signed ? static_cast<float>(static_cast<int32_t>(raw)) : static_cast<float>(raw);
          std::memcpy(at, &f, 4);
        }
    }
    if (!rect) {
      uint8_t* dst = RingAlloc(bytes, 16, &offset);
      if (!dst) return skip();
      up_vertex_bytes += bytes;
      RingCopy(dst, vertices.data(), bytes);
      streams[i].offset = offset;
    } else {   // three vertices per rectangle in memory, four on the host
      uint8_t* expanded = RingAlloc(static_cast<uint64_t>(rects) * 4 * stride, 16, &offset);
      if (!expanded) return skip();
      thread_local std::vector<uint8_t> quad;
      quad.resize(static_cast<size_t>(rects) * 4 * stride);
      for (uint32_t k = 0; k < rects; ++k) {
        const uint8_t* v0 = vertices.data() + static_cast<size_t>(3 * k) * stride;
        uint8_t* o = quad.data() + static_cast<size_t>(4 * k) * stride;
        std::memcpy(o, v0, 3ull * stride);
        SynthesizeRectVertex(o + 3ull * stride, v0, v0 + stride, v0 + 2ull * stride, stride, desc.attributes, i);
      }
      std::memcpy(expanded, quad.data(), quad.size());
      streams[i].offset = offset;
    }
  }
  for (uint32_t e = 0; e < 8; ++e) {   // dependent streams: the whole guest buffer, endian swapped, behind a device address
    const DependentStream& ds = dependents[e];
    if (!ds.used) continue;
    const xenos::VertexFetch vc = xenos::DecodeVertexFetch(&regs[r::SHADER_CONSTANT_FETCH + 2 * ds.slot]);
    const uint64_t start = static_cast<uint64_t>(vc.address_dwords) * 4, bytes = static_cast<uint64_t>(vc.size_dwords) * 4;
    if (!bytes || bytes > (8u << 20) || start + bytes > kGuestMemoryLimit) return skip();
    thread_local std::vector<uint8_t> whole;
    whole.assign(static_cast<size_t>(bytes) + 16, 0);
    std::memcpy(whole.data(), config.memory.Physical(static_cast<uint32_t>(start)), bytes);
    xenos::SwapEndian(whole.data(), static_cast<size_t>(bytes), vc.endian);
    if (ds.fix.used && ds.stride) ApplySwizzle(whole.data(), static_cast<size_t>(bytes) / ds.stride, ds.stride, ds.fix);
    uint8_t* dst = RingAlloc(whole.size(), 16, &offset);
    if (!dst) return skip();
    std::memcpy(dst, whole.data(), whole.size());
    const VkDeviceAddress address = RingAddress(offset);
    shared[80 + 4 * e] = static_cast<uint32_t>(address);
    shared[80 + 4 * e + 1] = static_cast<uint32_t>(address >> 32);
    shared[80 + 4 * e + 2] = ds.stride;
    shared[80 + 4 * e + 3] = (ds.format & 0xFF) | (ds.is_signed ? 1u << 8 : 0) | (ds.integer ? 1u << 9 : 0) | (ds.offset << 10);
    ++dependent_fetch_draws;
    dependent_fetch_bytes += whole.size();
  }
  lap(7);   // vertex streams done
  VkDeviceSize constants_offset = 0;
  if (constant_binding) {   // the values of the constants-only attributes (stride 0: every vertex reads the same 16 bytes)
    uint8_t* dst = RingAlloc(16u * constant_count, 16, &constants_offset);
    if (!dst) return skip();
    std::memset(dst, 0, 16u * constant_count);
    for (uint32_t k = 0; k < constant_count; ++k) std::memcpy(dst + 16u * k, constant_attrs[k].values, 16);
  }
  VkDeviceSize index_offset = 0;
  if (need_indices) {
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(index_count) * (wide ? 4 : 2);
    up_index_bytes += bytes;
    uint8_t* dst = RingAlloc(bytes, 4, &index_offset);
    if (!dst) return skip();
    if (wide) RingCopy(dst, out.data(), bytes);
    else {
      uint16_t* d16 = reinterpret_cast<uint16_t*>(dst);
      for (uint32_t i = 0; i < index_count; ++i) d16[i] = out[i] == 0xFFFFFFFFu ? 0xFFFFu : static_cast<uint16_t>(out[i]);
    }
  }
  auto upload_constants = [&](const uint32_t* src, std::vector<uint32_t>& last, VkDeviceAddress& last_address) -> VkDeviceAddress {
    ++up_const_total;
    if (last_address && last.size() == 1024 && std::memcmp(last.data(), src, 4096) == 0) {
      ++up_const_hits;
      return last_address;
    }
    up_const_bytes += 4096;
    VkDeviceSize off = 0;
    uint8_t* dst = RingAlloc(4096, 256, &off);
    if (!dst) return 0;
    RingCopy(dst, src, 4096);
    last.assign(src, src + 1024);
    last_address = RingAddress(off);
    return last_address;
  };
  const VkDeviceAddress vs_constants = upload_constants(&regs[r::SHADER_CONSTANT_VS], last_vs_constants, last_vs_address);
  const VkDeviceAddress ps_constants = upload_constants(&regs[r::SHADER_CONSTANT_PS], last_ps_constants, last_ps_address);
  VkDeviceSize shared_offset = 0;
  uint8_t* shared_dst = RingAlloc(sizeof shared, 256, &shared_offset);
  if (!vs_constants || !ps_constants || !shared_dst) return skip();
  RingCopy(shared_dst, shared, sizeof shared);
  const VkDeviceAddress shared_address = RingAddress(shared_offset);

  lap(4);
  // ---- pipeline ----
  if ((debug & 16) && desc.ps) desc.ps = debug_flat_frag, desc.ps_hash = 0xDEB0000000000010ull;   // (hashes that no cache entry has: such pipelines are never listed)
  if ((debug & 128) && desc.ps) desc.ps = debug_tex0_frag, desc.ps_hash = 0xDEB0000000000080ull;
  if ((debug & 512) && desc.ps) desc.ps = debug_tex1_frag, desc.ps_hash = 0xDEB0000000000200ull;
  desc.rect = rect;
  desc.primitive_restart = restart;
  desc.topology = topology;
  desc.color_format = color ? color->image.format : VK_FORMAT_UNDEFINED;
  desc.color_mask = color_mask;
  const uint32_t blend = regs[r::RB_BLENDCONTROL0];
  const bool blend_off = (blend & 31) == 1 && ((blend >> 8) & 31) == 0 && ((blend >> 5) & 7) == 0 && ((blend >> 16) & 31) == 1 && ((blend >> 24) & 31) == 0 && ((blend >> 21) & 7) == 0;
  desc.blend = blend_off ? 0 : (blend ? blend : 0);
  desc.depth_control = depth_control;
  desc.mode_control = mode_control & ((1u << 13) - 1);
  if (debug & 1) desc.mode_control &= ~3u;
  if (debug & 2) desc.depth_control = 0;
  if (debug & 4) desc.blend = 0;
  if (debug & 8) desc.mode_control ^= 4u;
  reason = 8;
  VkPipeline pipeline = GetPipeline(desc);
  if (!pipeline) return skip();

  lap(5);
  {   // debugging: GH2_RENDER_ONLY="frame:first-last[,frame:first-last...]" draws only these ranges of the frame's issued draws (to find which draws make a picture element)
    static const std::vector<std::array<uint64_t, 3>> only = [] {
      std::vector<std::array<uint64_t, 3>> v;
      if (const char* e = std::getenv("GH2_RENDER_ONLY")) {
        unsigned long long f, a, b;
        int used = 0;
        const char* p = e;
        while (std::sscanf(p, "%llu:%llu-%llu%n", &f, &a, &b, &used) == 3) {
          v.push_back({f, a, b});
          p += used;
          if (*p == ',') ++p;
        }
        std::fprintf(stderr, "RENDER GH2_RENDER_ONLY: %zu range(s) from '%s'\n", v.size(), e);
      }
      return v;
    }();
    const uint32_t index = draw_in_frame++;
    bool listed = false, inside = false;
    for (const auto& o : only)
      if (o[0] == frame) {
        listed = true;
        inside = inside || (index >= o[1] && index <= o[2]);
      }
    // only the scene's tile draws (scissor 640x720) are filtered: the library's clears and the post-processing passes stay, otherwise the targets keep the previous frame
    const uint32_t stl = regs[r::PA_SC_WINDOW_SCISSOR_TL], sbr = regs[r::PA_SC_WINDOW_SCISSOR_BR];
    static const bool strict = std::getenv("GH2_RENDER_ONLY_STRICT") != nullptr;   // the ranges apply to every draw, whatever its scissor (the render-to-texture passes too)
    const bool tile_draw = strict || ((sbr & 0x3FFF) - (stl & 0x3FFF) == 640 && ((sbr >> 16) & 0x3FFF) - ((stl >> 16) & 0x3FFF) == 720);
    if (listed && !inside && tile_draw && active_vertex.info->hash != kBuiltinClearVs) return skip();
    // GH2_RENDER_SKIP="vs:<hex suffix>,ps:<hex suffix>,blend:<RB_BLENDCONTROL0 hex>": debugging, such draws are not issued in any frame (finding which draw makes an artifact)
    static const std::vector<std::pair<std::string, std::string>> skip_list = [] {
      std::vector<std::pair<std::string, std::string>> v;
      if (const char* e = std::getenv("GH2_RENDER_SKIP")) {
        std::string text = e;
        size_t at = 0;
        while (at < text.size()) {
          size_t end = text.find(',', at);
          if (end == std::string::npos) end = text.size();
          const std::string item = text.substr(at, end - at);
          const size_t colon = item.find(':');
          if (colon != std::string::npos) {
            std::string value = item.substr(colon + 1);
            for (char& c : value) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            v.emplace_back(item.substr(0, colon), value);
          }
          at = end + 1;
        }
      }
      return v;
    }();
    if (!skip_list.empty()) {
      auto hex = [](uint64_t h) {
        char b[20];
        std::snprintf(b, sizeof b, "%016llX", static_cast<unsigned long long>(h));
        return std::string(b);
      };
      char blend_text[16];
      std::snprintf(blend_text, sizeof blend_text, "%08X", regs[r::RB_BLENDCONTROL0]);
      const std::string vs_text = hex(active_vertex.info->hash), ps_text = active_pixel.info ? hex(active_pixel.info->hash) : std::string();
      for (const auto& [kind, value] : skip_list) {
        const bool hit = kind == "vs" ? vs_text.size() >= value.size() && vs_text.compare(vs_text.size() - value.size(), value.size(), value) == 0
                       : kind == "ps" ? !ps_text.empty() && ps_text.size() >= value.size() && ps_text.compare(ps_text.size() - value.size(), value.size(), value) == 0
                       : kind == "blend" ? value == blend_text : false;
        if (hit) return skip();
      }
    }
    // GH2_RENDER_ONLY_VS="frame:hexsuffix[+hexsuffix...][,frame:...]": in the listed frames the scene's tile draws are only those whose vertex shader hash ends with one of the suffixes
    static const std::vector<std::pair<uint64_t, std::vector<std::string>>> only_vs = [] {
      std::vector<std::pair<uint64_t, std::vector<std::string>>> v;
      if (const char* e = std::getenv("GH2_RENDER_ONLY_VS")) {
        std::string text = e;
        size_t at = 0;
        while (at < text.size()) {
          size_t end = text.find(',', at);
          if (end == std::string::npos) end = text.size();
          const std::string item = text.substr(at, end - at);
          const size_t colon = item.find(':');
          if (colon != std::string::npos) {
            std::vector<std::string> suffixes;
            size_t q = colon + 1;
            while (q <= item.size()) {
              size_t plus = item.find('+', q);
              if (plus == std::string::npos) plus = item.size();
              suffixes.push_back(item.substr(q, plus - q));
              q = plus + 1;
            }
            v.push_back({std::strtoull(item.c_str(), nullptr, 10), suffixes});
          }
          at = end + 1;
        }
      }
      return v;
    }();
    for (const auto& o : only_vs)
      if (o.first == frame && tile_draw && active_vertex.info->hash != kBuiltinClearVs) {
        char hex[20];
        std::snprintf(hex, sizeof hex, "%016llX", (unsigned long long)active_vertex.info->hash);
        bool match = false;
        for (const std::string& suffixed : o.second) {   // "suffix" or "suffix@first-last": only the draws first..last (counting this shader's draws of the frame)
          std::string suffix = suffixed;
          uint32_t first = 0, last = ~0u;
          const size_t at = suffix.find('@');
          if (at != std::string::npos) {
            std::sscanf(suffix.c_str() + at + 1, "%u-%u", &first, &last);
            suffix.resize(at);
          }
          if (std::strcmp(hex + 16 - suffix.size(), suffix.c_str()) == 0) {
            static uint64_t counted_frame = ~0ull;
            static uint32_t counted = 0;
            if (counted_frame != frame) counted_frame = frame, counted = 0;
            const uint32_t nth = counted++;
            match = match || (nth >= first && nth <= last);
          }
        }
        if (!match) return skip();
      }
  }
  // ---- state and the draw ----
  BeginRendering(color, depth);
  if (pipeline != bound_pipeline) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    bound_pipeline = pipeline;
  }
  VkViewport viewport;
  if (viewport_on) {
    const float xs = xenos::AsFloat(regs[r::PA_CL_VPORT_XSCALE]), xo = xenos::AsFloat(regs[r::PA_CL_VPORT_XOFFSET]);
    const float ys = xenos::AsFloat(regs[r::PA_CL_VPORT_YSCALE]), yo = xenos::AsFloat(regs[r::PA_CL_VPORT_YOFFSET]);
    viewport.x = (xo - xs) * ScaleXf();
    viewport.width = 2.f * xs * ScaleXf();
    viewport.y = (yo - ys) * ScaleYf();      // a negative yscale (y grows downward on screen) becomes a negative height: Vulkan's flipped viewport
    viewport.height = 2.f * ys * ScaleYf();
  } else {
    // no viewport transform: the shader's window coordinates were mapped to NDC over the whole target, so the viewport is the whole target (not flipped: window y already grows downward)
    viewport = {0.f, 0.f, static_cast<float>(frame_width), static_cast<float>(frame_height), 0.f, 1.f};
  }
  viewport.minDepth = 0.f;
  viewport.maxDepth = 1.f;
  // The viewport's z scale/offset also apply to draws whose PA_CL_VTE_CNTL says "off": the sun quads of the intro of Sweet Child O' Mine (window-space sprites at z = 1.0, depth-tested GEQUAL
  // against a scene drawn with scale -0.9 / offset 0.9) are hidden behind the stage as on the console only this way (checked against a console video and the user's frames; before, the glow
  // covered the curtain). The only draws this changes in the Quick Play song and the intro are those quads. GH2_RENDER_ZVTE=0 restores the old rule.
  static const bool z_always_scaled = !std::getenv("GH2_RENDER_ZVTE") || std::getenv("GH2_RENDER_ZVTE")[0] != '0';
  if (((vte & 0x10) && (vte & 0x20)) || z_always_scaled) {
    const float zs = xenos::AsFloat(regs[r::PA_CL_VPORT_ZSCALE]), zo = xenos::AsFloat(regs[r::PA_CL_VPORT_ZOFFSET]);
    viewport.minDepth = std::min(1.f, std::max(0.f, zo));
    viewport.maxDepth = std::min(1.f, std::max(0.f, zo + zs));
  }
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  const uint32_t tl = regs[r::PA_SC_WINDOW_SCISSOR_TL], br = regs[r::PA_SC_WINDOW_SCISSOR_BR];
  const int32_t sx0 = static_cast<int32_t>(ScaleX(std::min<uint32_t>(tl & 0x3FFF, kGuestWidth))), sy0 = static_cast<int32_t>(ScaleY(std::min<uint32_t>((tl >> 16) & 0x3FFF, kGuestHeight)));
  const int32_t sx1 = static_cast<int32_t>(ScaleX(std::min<uint32_t>(br & 0x3FFF, kGuestWidth))), sy1 = static_cast<int32_t>(ScaleY(std::min<uint32_t>((br >> 16) & 0x3FFF, kGuestHeight)));
  VkRect2D scissor = {{sx0, sy0}, {static_cast<uint32_t>(std::max(0, sx1 - sx0)), static_cast<uint32_t>(std::max(0, sy1 - sy0))}};
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  const float blend_constants[4] = {xenos::AsFloat(regs[r::RB_BLEND_RED]), xenos::AsFloat(regs[r::RB_BLEND_RED + 1]), xenos::AsFloat(regs[r::RB_BLEND_RED + 2]), xenos::AsFloat(regs[r::RB_BLEND_RED + 3])};
  vkCmdSetBlendConstants(cmd, blend_constants);
  const uint32_t stencil_front = regs[r::RB_STENCILREFMASK], stencil_back = regs[r::RB_STENCILREFMASK_BF];
  vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, stencil_front & 0xFF);
  vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_BIT, (stencil_front >> 8) & 0xFF);
  vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_BIT, (stencil_front >> 16) & 0xFF);
  const uint32_t back = (depth_control >> 7) & 1 ? stencil_back : stencil_front;
  vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, back & 0xFF);
  vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_BACK_BIT, (back >> 8) & 0xFF);
  vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_BACK_BIT, (back >> 16) & 0xFF);
  vkCmdSetDepthBias(cmd, xenos::AsFloat(regs[r::PA_SU_POLY_OFFSET_FRONT_OFFSET]) * 0.f, 0.f, 0.f);

  const uint64_t push[3] = {vs_constants, ps_constants, shared_address};
  vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, push);
  if (stream_count || constant_binding) {
    VkBuffer buffers[9];
    VkDeviceSize offsets[9];
    for (uint32_t i = 0; i < stream_count; ++i) {
      buffers[i] = cur->ring.buffer;
      offsets[i] = streams[i].offset;
    }
    if (constant_binding) {
      buffers[stream_count] = cur->ring.buffer;
      offsets[stream_count] = constants_offset;
    }
    vkCmdBindVertexBuffers(cmd, 0, stream_count + (constant_binding ? 1 : 0), buffers, offsets);
  }
  if (need_indices) {
    vkCmdBindIndexBuffer(cmd, cur->ring.buffer, index_offset, wide ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
    if (prof_draws) ProfMark("draw vs %08X ps %08X s%u n%u", static_cast<uint32_t>(desc.vs_hash), static_cast<uint32_t>(desc.ps_hash), desc.samples, index_count >= 1000 ? 1000 : (index_count >= 100 ? 100 : 0));
    vkCmdDrawIndexed(cmd, index_count, 1, 0, 0, 0);
  } else {
    if (prof_draws) ProfMark("draw vs %08X ps %08X s%u n%u", static_cast<uint32_t>(desc.vs_hash), static_cast<uint32_t>(desc.ps_hash), desc.samples, count >= 1000 ? 1000 : (count >= 100 ? 100 : 0));
    vkCmdDraw(cmd, count, 1, 0, 0);
  }
  lap(6);
  ++stats.draws_issued;
  ++drawn_this_frame;
  static const uint64_t log_from = std::getenv("GH2_RENDER_LOG_FROM") ? std::strtoull(std::getenv("GH2_RENDER_LOG_FROM"), nullptr, 10) : 0;
  static uint64_t logged = 0;
  static const uint64_t log_frame = std::getenv("GH2_RENDER_LOG_FRAME") ? std::strtoull(std::getenv("GH2_RENDER_LOG_FRAME"), nullptr, 10) : ~0ull;   // every issued draw of this one frame
  static const uint64_t log_span = std::getenv("GH2_RENDER_LOG_SPAN") ? std::strtoull(std::getenv("GH2_RENDER_LOG_SPAN"), nullptr, 10) : 1;
  const uint64_t shot_log = shot_log_frame.load(std::memory_order_relaxed);
  const bool in_log_frame = (frame >= log_frame && frame < log_frame + log_span) || frame == shot_log;
  static const uint64_t log_vs = std::getenv("GH2_RENDER_LOG_VS") ? std::strtoull(std::getenv("GH2_RENDER_LOG_VS"), nullptr, 16) : 0;   // detailed log of the first draws of one vertex shader (hash suffix, hex)
  static const uint64_t log_vs_mask = log_vs ? ~0ull >> (64 - 4 * std::min<size_t>(16, std::strlen(std::getenv("GH2_RENDER_LOG_VS")))) : 0;
  static uint64_t logged_vs = 0;
  static const uint32_t log_vs_count = std::getenv("GH2_RENDER_LOG_VS_N") ? static_cast<uint32_t>(std::atoi(std::getenv("GH2_RENDER_LOG_VS_N"))) : 0;   // ... only draws with this many indices
  const bool vs_match = log_vs && (active_vertex.info->hash & log_vs_mask) == log_vs && (!log_vs_count || count == log_vs_count) && logged_vs < 6;
  if (vs_match) ++logged_vs;
  static const uint64_t log_additive_from = std::getenv("GH2_RENDER_LOG_ADDITIVE") ? std::strtoull(std::getenv("GH2_RENDER_LOG_ADDITIVE"), nullptr, 10) : ~0ull;   // from this frame on
  static const bool log_additive = log_additive_from != ~0ull;   // detailed log of the first 4000 draws that blend additively (the hit flames, sparks)
  static uint64_t logged_additive = 0;
  const bool additive_match = log_additive && blend != 0 && ((blend >> 8) & 31) == 1 && logged_additive < 6000 && frame > log_additive_from;
  if (additive_match) ++logged_additive;
  const bool log_this = (log_draws && frame >= log_from && logged < 30) || in_log_frame || vs_match || additive_match;
  if (log_this && !in_log_frame && !vs_match && !additive_match) ++logged;
  const bool log_detail = log_this && (!in_log_frame || vs_match || additive_match || (frame == shot_log && d.prim_type == 6));   // the F9 frame: rectangle lists (screen-space sprites) in detail
  if (log_detail && stream_count) {
    // the first vertices as the shader sees them (after the endian swap) and the first vertex shader constants
    const float* v = reinterpret_cast<const float*>(cur->ring.mapped + streams[0].offset);
    std::fprintf(stderr, "   vtx stride=%u: %g %g %g %g | %g %g %g %g\n", streams[0].stride, v[0], v[1], v[2], v[3], v[streams[0].stride / 4], v[streams[0].stride / 4 + 1], v[streams[0].stride / 4 + 2],
                 v[streams[0].stride / 4 + 3]);
    for (uint32_t vtx = 0; vtx < 3; ++vtx) {   // the raw dwords (after the endian swap) of the first vertices of stream 0
      const uint32_t* w = reinterpret_cast<const uint32_t*>(cur->ring.mapped + streams[0].offset + vtx * streams[0].stride);
      std::fprintf(stderr, "   RAW vertex %u:", vtx);
      for (uint32_t q = 0; q < streams[0].stride / 4 && q < 20; ++q) std::fprintf(stderr, " %08X", w[q]);
      std::fprintf(stderr, "\n");
    }
    const float* c = reinterpret_cast<const float*>(&regs[r::SHADER_CONSTANT_VS]);
    std::fprintf(stderr, "   vsc0-3: %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g\n", c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9], c[10], c[11], c[12], c[13], c[14], c[15]);
    std::fprintf(stderr, "   vs c24-27 (gModel0 rows): %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g\n", c[96], c[97], c[98], c[99], c[100], c[101], c[102], c[103], c[104], c[105], c[106], c[107], c[108], c[109], c[110], c[111]);
    for (const auto& a : desc.attributes) std::fprintf(stderr, "   attr loc=%u bind=%u fmt=%d off=%u\n", a.location, a.binding, static_cast<int>(a.format), a.offset);
    for (const auto& e : active_vertex.info->elements) std::fprintf(stderr, "   container element instr=%u usage=%u index=%u dst=r%u swz=%03X\n", e.instruction, e.usage, e.usage_index, e.dst, e.dst_swizzle);
    for (const auto& vf : active_vertex.usage.vertex_fetches)
      std::fprintf(stderr, "   microcode fetch instr=%u slot=%u stride=%u off=%d fmt=%u signed=%d int=%d mini=%d dst=r%u swz=%03X\n", vf.instruction, vf.slot, vf.stride, vf.offset, vf.format, vf.is_signed, vf.integer, vf.mini, vf.dst, vf.dst_swizzle);
    {   // the colour constants the game's effects are tinted with (the translated shaders read VS c71 as gEnvAmbient, c0 as gDiffuse)
      const float* vk = reinterpret_cast<const float*>(&regs[r::SHADER_CONSTANT_VS]);
      std::fprintf(stderr, "   TINT vs c0=(%g %g %g %g) c71=(%g %g %g %g)\n", vk[0], vk[1], vk[2], vk[3], vk[284], vk[285], vk[286], vk[287]);
    }
    for (int block = 0; block < 2; ++block) {
      const float* k = reinterpret_cast<const float*>(&regs[block ? r::SHADER_CONSTANT_PS : r::SHADER_CONSTANT_VS]);
      std::fprintf(stderr, "   %s constants:", block ? "ps" : "vs");
      int shown = 0;
      for (int i = 0; i < 256 && shown < 14; ++i)
        if (k[4 * i] != 0.f || k[4 * i + 1] != 0.f || k[4 * i + 2] != 0.f || k[4 * i + 3] != 0.f) {
          std::fprintf(stderr, " c%d=(%g %g %g %g)", i, k[4 * i], k[4 * i + 1], k[4 * i + 2], k[4 * i + 3]);
          ++shown;
        }
      std::fprintf(stderr, "\n");
    }
  }
  if (log_this) {
    for (uint32_t t = 0; t < 16; ++t)
      if ((texture_slots >> t) & 1) {
        const xenos::TextureFetch tf = xenos::DecodeTextureFetch(&regs[r::SHADER_CONSTANT_FETCH + 6 * t]);
        const bool is_resolved = FindResolvedAny(tf.base_address << 12) != nullptr;
        std::fprintf(stderr, "   t%u base %08X %ux%u fmt %u (%s) %s heap %u sampler %u swz %03X\n", t, tf.base_address << 12, tf.width, tf.height, tf.format, xenos::GetTextureFormat(tf.format).name,
                     is_resolved ? "RESOLVED" : "memory", shared[t] ? shared[t] : shared[32 + t], shared[48 + t], tf.swizzle);
      }
  }
  if (log_this) {
    uint32_t signature = 0;   // a short fingerprint of the patched vertex fetches: stride and offsets
    for (const gh2gfx::d3d::VertexFetch& vf : active_vertex.usage.vertex_fetches) signature = signature * 31 + vf.stride * 1000 + static_cast<uint32_t>(vf.offset) * 7 + vf.format;
    if (stream_count) {   // a fingerprint of the first bytes of stream 0 (does the buffer change from frame to frame?)
      uint64_t h = 1469598103934665603ull;
      const uint32_t* w = reinterpret_cast<const uint32_t*>(cur->ring.mapped + streams[0].offset);
      for (uint32_t q = 0; q < 64 && q * 4 < streams[0].stride * vertex_count; ++q) h = (h ^ w[q]) * 1099511628211ull;
      std::fprintf(stderr, "   stream0 fingerprint %08X\n", static_cast<uint32_t>(h ^ (h >> 32)));
    }
    std::fprintf(stderr, "   surface info %08X (%u samples), colour control %08X alpha ref %08X, ps spec %u, color mask %X; vs address %08X ucode-signature %08X | vte %08X vport z scale %g offset %g -> depth range %g..%g, depth image %08X", regs[r::RB_SURFACE_INFO], samples, regs[r::RB_COLORCONTROL], regs[r::RB_ALPHA_REF], desc.ps_spec, color_mask, active_vertex.address, signature,
                 vte, xenos::AsFloat(regs[r::PA_CL_VPORT_ZSCALE]), xenos::AsFloat(regs[r::PA_CL_VPORT_ZOFFSET]), viewport.minDepth, viewport.maxDepth, depth_info);
    for (uint32_t i = 0; i < stream_count; ++i) {
      const xenos::VertexFetch vc = xenos::DecodeVertexFetch(&regs[r::SHADER_CONSTANT_FETCH + 2 * streams[i].slot]);
      std::fprintf(stderr, " | stream %u: slot %u stride %u B, buffer %u B (= %.2f strides), vertices in range %u", i, streams[i].slot, streams[i].stride, vc.size_dwords * 4, vc.size_dwords * 4.0 / streams[i].stride, vertex_count);
    }
    std::fprintf(stderr, "\n");
  }
  if (log_this)
    std::fprintf(stderr, "RENDER draw #%llu f%llu prim=%u n=%u idx=%d vs=%016llX ps=%016llX streams=%u attrs=%zu tex=%08X rt=%08X/%08X vp=%.0f,%.0f %.0fx%.0f scissor=%d,%d %ux%u blend=%08X dc=%08X mask=%X vb=%08X ib=%08X idx[%u..%u bias %d vgt %u..%u]%s\n",
                 (unsigned long long)stats.draws_issued.load(), (unsigned long long)frame, d.prim_type, count, indexed, (unsigned long long)active_vertex.info->hash,
                 (unsigned long long)(have_ps ? active_pixel.info->hash : 0), stream_count, desc.attributes.size(), texture_slots, color_info, depth_info, viewport.x, viewport.y, viewport.width,
                 viewport.height, scissor.offset.x, scissor.offset.y, scissor.extent.width, scissor.extent.height, blend, depth_control, color_mask,
                 stream_count ? xenos::DecodeVertexFetch(&regs[r::SHADER_CONSTANT_FETCH + 2 * streams[0].slot]).address_dwords * 4 : 0u, d.dma_base, min_v, max_v, static_cast<int>(index_bias), regs[r::VGT_MIN_VTX_INDX], regs[r::VGT_MAX_VTX_INDX], vertex_stats.c_str());

  // The EDRAM is shared by all sample counts: the game clears a target with 4x MSAA (a quarter of the pixels to touch, the rectangle is in that surface's own units) and renders into the same
  // bytes at 1x. Here every sample count has its own image, so a clear drawn at one count wipes the whole images the same EDRAM base has at the others (when they exist) with the same
  // colour, depth and stencil values.
  if (active_vertex.info->hash == kBuiltinClearVs && stream_count && streams[0].stride >= 28) {
    const float* v = reinterpret_cast<const float*>(cur->ring.mapped + streams[0].offset);   // first vertex: x y z, then r g b a
    const bool write_color = mode != 5 && (color_mask & 0xF);
    const bool write_depth = ((depth_control >> 1) & 1) && ((depth_control >> 2) & 1);
    for (uint32_t other : {1u, 2u, 4u}) {
      if (other == samples) continue;
      TargetImage* other_color = write_color ? FindColorTarget(xenos::ColorBase(color_info), xenos::ColorFormat(color_info), other) : nullptr;
      TargetImage* other_depth = write_depth ? FindDepthTarget(xenos::DepthBase(depth_info), xenos::DepthFormat(depth_info), other) : nullptr;
      const float color_value[4] = {std::min(1.f, std::max(0.f, v[3])), std::min(1.f, std::max(0.f, v[4])), std::min(1.f, std::max(0.f, v[5])), std::min(1.f, std::max(0.f, v[6]))};
      if (other_color) SetClear(*other_color, color_value, 0.f, 0);
      if (other_depth) SetClear(*other_depth, color_value, std::min(1.f, std::max(0.f, v[2])), regs[r::RB_STENCILREFMASK] & 0xFF);
      if (other_color || other_depth) ++clear_replays;
    }
  }
}

}  // namespace gh2render
