#include "graphics_state.h"

#include <algorithm>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace gh2gfx::d3d {

namespace {

inline uint32_t Be32(const uint8_t* base, uint32_t guest_address) {
  uint32_t v;
  std::memcpy(&v, base + guest_address, sizeof(v));
  return __builtin_bswap32(v);
}
inline uint32_t Be16(const uint8_t* base, uint32_t guest_address) {
  uint16_t v;
  std::memcpy(&v, base + guest_address, sizeof(v));
  return __builtin_bswap16(v);
}
inline uint32_t Be8(const uint8_t* base, uint32_t guest_address) { return base[guest_address]; }

// A guest pointer worth reading: aligned and inside the part of the address space the guest uses.
inline bool Plausible(uint32_t p) { return p >= 0x10000 && p < 0xFFFF0000u && (p & 3) == 0; }

constexpr uint32_t kFetchRegisterBase = 0x4800, kFetchRegisterCount = 32 * 6;
constexpr uint32_t kRbCopyControl = 0x2318;  // RB_COPY_CONTROL, DEST_BASE, DEST_PITCH, DEST_INFO
constexpr uint32_t kPm4DrawIndx = 0x22;

uint32_t Shadow(const uint8_t* base, uint32_t device, uint32_t slot, uint32_t dword) {
  return Be32(base, device + layout::kDeviceFetchConstants + layout::kFetchSlotBytes * slot + 4 * dword);
}

// Texture memory per pixel, for the formats that matter here (xenos::TextureFormat); 0 = not in the table
uint32_t BitsPerPixel(uint32_t f) {
  switch (f) {
    case 2: return 8;
    case 3: case 4: case 10: case 24: case 30: return 16;
    case 6: case 7: case 16: case 17: case 22: case 23: case 25: case 31: case 33: case 36: return 32;
    case 18: return 4;
    case 19: case 20: case 49: return 8;
    case 26: case 32: case 34: case 37: return 64;
    case 35: case 38: return 128;
    default: return 0;
  }
}

}  // namespace

const char* ApiName(Api api) {
  static const char* const kNames[] = {
      "SetVertexDeclaration", "SetFVF", "CreateVertexDeclaration", "CreateVertexBuffer", "CreateIndexBuffer", "SetStreamSource", "SetIndices",
      "CreateTexture", "CreateTextureHeader", "CreateSurface", "SetTexture", "SetSamplerState",
      "SetRenderTarget", "SetDepthStencilSurface", "Resolve", "Clear", "Swap",
      "DrawVertices", "DrawIndexedVertices", "DrawIndexedVerticesUP", "BeginVertices", "EndVertices",
      "LockVertexBuffer", "LockIndexBuffer", "TextureLevelAddress"};
  static_assert(sizeof(kNames) / sizeof(kNames[0]) == static_cast<size_t>(Api::kCount), "one name per Api");
  return static_cast<size_t>(api) < static_cast<size_t>(Api::kCount) ? kNames[static_cast<size_t>(api)] : "?";
}

const char* KindName(Kind kind) {
  switch (kind) {
    case Kind::kVertexBuffer: return "vertex_buffers";
    case Kind::kIndexBuffer: return "index_buffers";
    case Kind::kTexture: return "textures";
    case Kind::kSurface: return "surfaces";
    case Kind::kDeclaration: return "declarations";
    default: return "?";
  }
}

const char* TextureFormatName(uint32_t f) {
  switch (f) {
    case 2: return "8";
    case 3: return "1_5_5_5";
    case 4: return "5_6_5";
    case 6: return "8_8_8_8";
    case 7: return "2_10_10_10";
    case 10: return "8_8";
    case 16: return "10_11_11";
    case 17: return "11_11_10";
    case 18: return "DXT1";
    case 19: return "DXT2_3";
    case 20: return "DXT4_5";
    case 22: return "24_8";
    case 23: return "24_8_FLOAT";
    case 24: return "16";
    case 25: return "16_16";
    case 26: return "16_16_16_16";
    case 30: return "16_FLOAT";
    case 31: return "16_16_FLOAT";
    case 32: return "16_16_16_16_FLOAT";
    case 33: return "32";
    case 34: return "32_32";
    case 35: return "32_32_32_32";
    case 36: return "32_FLOAT";
    case 37: return "32_32_FLOAT";
    case 38: return "32_32_32_32_FLOAT";
    case 49: return "DXN";
    default: return "other";
  }
}

namespace {

enum class Check : uint8_t {
  kDeclaration, kIndexBuffer, kRenderTarget, kDepthStencil, kStreamBuffer, kStreamStride, kTexturePointer,
  kTextureFetch0, kTextureFetch1, kTextureFetch2, kTextureFetch5, kVertexFetchAddress, kVertexFetchSize,
  kDrawPrimitive, kDrawCount, kDrawSource, kDrawDmaBase, kDrawDmaSize, kDrawPacketPresent, kPm4FetchRegister, kResolveDestination,
  kCount
};
const char* const kCheckNames[] = {
    "device.vertex_declaration", "device.index_buffer", "device.render_target", "device.depth_stencil", "device.stream_vertex_buffer", "device.stream_stride",
    "device.texture", "shadow.texture_fetch0", "shadow.texture_fetch1_base_format", "shadow.texture_fetch2_size", "shadow.texture_fetch5_mip_address",
    "shadow.vertex_fetch_address", "shadow.vertex_fetch_size", "pm4.draw_primitive", "pm4.draw_count", "pm4.draw_source", "pm4.draw_dma_base", "pm4.draw_dma_size",
    "pm4.draw_packet_present", "pm4.fetch_register_equals_shadow", "pm4.resolve_destination_equals_texture"};
static_assert(sizeof(kCheckNames) / sizeof(kCheckNames[0]) == static_cast<size_t>(Check::kCount), "one name per check");

uint64_t g_checks[static_cast<size_t>(Check::kCount)], g_mismatches[static_cast<size_t>(Check::kCount)];  // guarded by GraphicsState::mu_ (one instance per process)

}  // namespace

GraphicsState::GraphicsState(bool verify) : verify_(verify) {
  std::memset(g_checks, 0, sizeof(g_checks));
  std::memset(g_mismatches, 0, sizeof(g_mismatches));
}

GraphicsState::Device& GraphicsState::Dev(uint32_t device) {
  if (device == last_device_ && last_device_state_) return *last_device_state_;
  Device& d = devices_[device];
  last_device_ = device;
  last_device_state_ = &d;
  return d;
}

// 0 = ok, 1 = not the type asked for, 2 = the type is right but the contents are not plausible
static int ReadHeaderImpl(const uint8_t* base, Entry& e) {
  e.common = Be32(base, e.ptr);
  const uint32_t type = e.common & 0xF;
  switch (e.kind) {
    case Kind::kVertexBuffer:
      if (type != 1) return 1;
      e.words[0] = Be32(base, e.ptr + layout::kBufferAddress);
      e.words[1] = Be32(base, e.ptr + layout::kBufferSize);
      return (e.words[0] & 3) == 3 && (e.words[0] & ~3u) != 0 && (e.words[1] & 0x03FFFFFCu) != 0 ? 0 : 2;
    case Kind::kIndexBuffer:
      if (type != 2) return 1;
      e.words[0] = Be32(base, e.ptr + layout::kBufferAddress);
      e.words[1] = Be32(base, e.ptr + layout::kBufferSize);
      return e.words[0] != 0 && e.words[1] != 0 ? 0 : 2;
    case Kind::kTexture: {
      if (!(type == 3 || (type >= 11 && type <= 14))) return 1;
      for (int i = 0; i < 6; ++i) e.words[i] = Be32(base, e.ptr + layout::kTextureFetchConstant + 4 * i);
      const uint32_t* w = e.words;
      e.pitch = ((w[0] >> 22) & 0x1FF) * 32;
      e.tiled = (w[0] >> 31) != 0;
      e.format = w[1] & 0x3F;
      e.base = w[1] & ~0xFFFu;
      e.width = (w[2] & 0x1FFF) + 1;
      e.height = ((w[2] >> 13) & 0x1FFF) + 1;
      return (w[0] & 3) == 2 ? 0 : 2;  // fetch constant type 2 = a valid texture
    }
    case Kind::kSurface:
      if (type != 4) return 1;
      for (int i = 0; i < 5; ++i) e.words[i] = Be32(base, e.ptr + layout::kSurfaceInfo + 4 * i);
      e.words[5] = Be32(base, e.ptr + layout::kSurfaceFormat);
      return 0;
    case Kind::kDeclaration:
      return type == 5 ? 0 : 1;
    default:
      return 1;
  }
}

bool GraphicsState::ReadHeader(const uint8_t* base, Entry& e) { return ReadHeaderImpl(base, e) == 0; }

Entry* GraphicsState::Create(const uint8_t* base, uint32_t ptr, Kind kind, const uint32_t (&creation)[4]) {
  if (!Plausible(ptr)) return nullptr;
  Entry fresh;
  fresh.kind = kind;
  fresh.ptr = ptr;
  fresh.by_hook = true;
  std::memcpy(fresh.creation, creation, sizeof(creation));
  if (ReadHeaderImpl(base, fresh) != 0) {
    ++invalid_header_[static_cast<size_t>(kind)];
    return nullptr;
  }
  auto it = entries_.find(ptr);
  if (it != entries_.end()) ++reused_[static_cast<size_t>(kind)];
  ++by_hook_[static_cast<size_t>(kind)];
  Entry& slot = entries_[ptr];
  slot = fresh;
  IndexTexture(slot);
  return &slot;
}

Entry* GraphicsState::Bind(const uint8_t* base, uint32_t ptr, Kind want) {
  if (ptr == 0) return nullptr;
  const size_t k = static_cast<size_t>(want);
  if (!Plausible(ptr)) {
    ++invalid_header_[k];
    return nullptr;
  }
  auto it = entries_.find(ptr);
  if (it != entries_.end() && it->second.kind == want) {
    Entry& e = it->second;
    const uint32_t common = Be32(base, ptr);
    if ((common & 0xF) == (e.common & 0xF)) {
      e.common = common;
      ++e.binds;
      return &e;
    }
  }
  // not seen before (created by code that is not hooked, e.g. a header the game filled itself) or reused for another type: validate and adopt it
  Entry fresh;
  fresh.kind = want;
  fresh.ptr = ptr;
  const int status = ReadHeaderImpl(base, fresh);
  if (status != 0) {
    ++(status == 1 ? wrong_type_ : invalid_header_)[k];
    return nullptr;
  }
  if (it != entries_.end()) ++reused_[k];
  ++adopted_[k];
  Entry& slot = entries_[ptr];
  slot = fresh;
  ++slot.binds;
  IndexTexture(slot);
  return &slot;
}

void GraphicsState::IndexTexture(Entry& e) {
  if (e.kind == Kind::kTexture) texture_by_base_[PhysicalAddress(e.words[1]) & ~0xFFFu] = &e;
}

void GraphicsState::RefreshTexture(const uint8_t* base, Entry& e) {
  uint32_t w[6];
  for (int i = 0; i < 6; ++i) w[i] = Be32(base, e.ptr + layout::kTextureFetchConstant + 4 * i);
  if (std::memcmp(w, e.words, sizeof(w)) == 0) return;
  std::memcpy(e.words, w, sizeof(w));
  e.pitch = ((w[0] >> 22) & 0x1FF) * 32;
  e.tiled = (w[0] >> 31) != 0;
  e.format = w[1] & 0x3F;
  e.base = w[1] & ~0xFFFu;
  e.width = (w[2] & 0x1FFF) + 1;
  e.height = ((w[2] >> 13) & 0x1FFF) + 1;
  ++e.header_changes;
  ++texture_header_changes_;
  IndexTexture(e);
}

void GraphicsState::OnPipeline(uint32_t device, const PipelineInfo& pipeline) {
  std::lock_guard<std::mutex> lock(mu_);
  Dev(device).pipeline = pipeline;
}

uint64_t GraphicsState::Draws() const { return draws_; }
uint64_t GraphicsState::CompleteDraws() const { return draws_complete_; }
uint64_t GraphicsState::VerifyMismatches() const { return verify_stats_.mismatches; }

void GraphicsState::OnCall(const uint8_t* base, const Call& c) {
  std::lock_guard<std::mutex> lock(mu_);
  const size_t api = static_cast<size_t>(c.api);
  ++calls_[api];
  {
    thread_local uint32_t t_id = [] {
      static uint32_t next = 0;
      return next++;
    }();
    ++thread_calls_[t_id];
    if (c.api >= Api::kDrawVertices && c.api <= Api::kBeginVertices) draw_threads_ |= 1u << (t_id & 31);
  }
  switch (c.api) {
    case Api::kSetVertexDeclaration: {
      Device& d = Dev(c.r[0]);
      d.declaration = c.r[1];
      d.declaration_entry = Bind(base, c.r[1], Kind::kDeclaration);
      break;
    }
    case Api::kSetFVF: {
      Device& d = Dev(c.r[0]);
      d.fvf = c.r[1];
      d.declaration = Be32(base, c.r[0] + layout::kDeviceVertexDeclaration);  // the library turned the FVF into a declaration of its own
      d.declaration_entry = Bind(base, d.declaration, Kind::kDeclaration);
      break;
    }
    case Api::kCreateVertexDeclaration: {
      const uint32_t creation[4] = {c.r[0], 0, 0, 0};
      Entry* e = c.ret ? Create(base, c.ret, Kind::kDeclaration, creation) : nullptr;
      if (e && Plausible(c.r[0])) {  // D3DVERTEXELEMENT[]: 12 bytes each {stream u16, offset u16, type u32, method u8, usage u8, usage index u8}, ended by stream 0xFF
        for (uint32_t i = 0; i < 32; ++i) {
          const uint32_t at = c.r[0] + 12 * i;
          const uint32_t stream = Be16(base, at);
          if (stream == 0xFF) break;
          if (stream < 16) e->stream_mask |= 1u << stream;
          ++e->element_count;
        }
      }
      break;
    }
    case Api::kCreateVertexBuffer: {
      const uint32_t creation[4] = {c.r[0], c.r[1], 0, 0};
      if (c.ret) Create(base, c.ret, Kind::kVertexBuffer, creation);
      break;
    }
    case Api::kCreateIndexBuffer: {
      const uint32_t creation[4] = {c.r[0], c.r[1], c.r[2], 0};
      if (c.ret) Create(base, c.ret, Kind::kIndexBuffer, creation);
      break;
    }
    case Api::kSetStreamSource: {
      Device& d = Dev(c.r[0]);
      if (c.r[1] >= layout::kStreamCount) break;
      StreamBinding& s = d.streams[c.r[1]];
      s.vb = c.r[2];
      s.offset = c.r[3];
      s.stride = c.r[4];
      ++stream_binds_;
      if (c.r[2] == 0) ++null_stream_binds_;
      s.entry = Bind(base, c.r[2], Kind::kVertexBuffer);
      break;
    }
    case Api::kSetIndices: {
      Device& d = Dev(c.r[0]);
      d.index_buffer = c.r[1];
      ++index_binds_;
      if (c.r[1] == 0) ++null_index_binds_;
      d.index_buffer_entry = Bind(base, c.r[1], Kind::kIndexBuffer);
      break;
    }
    case Api::kCreateTexture: {
      const uint32_t creation[4] = {c.r[0], c.r[1], c.r[2], c.r[5]};
      if (c.ret) Create(base, c.ret, Kind::kTexture, creation);
      break;
    }
    case Api::kCreateTextureHeader:
      break;
    case Api::kCreateSurface: {
      const uint32_t creation[4] = {c.r[0], c.r[1], c.r[2], c.r[3]};
      if (c.ret) Create(base, c.ret, Kind::kSurface, creation);
      break;
    }
    case Api::kSetTexture: {
      Device& d = Dev(c.r[0]);
      if (c.r[1] >= layout::kSamplerCount) break;
      TextureBinding& t = d.textures[c.r[1]];
      t.texture = c.r[2];
      ++texture_binds_;
      if (c.r[2] == 0) {
        ++null_texture_binds_;
        t.entry = nullptr;
      } else {
        t.entry = Bind(base, c.r[2], Kind::kTexture);
      }
      break;
    }
    case Api::kSetSamplerState:
      ++sampler_state_calls_[c.address];
      break;
    case Api::kSetRenderTarget: {
      Device& d = Dev(c.r[0]);
      if (c.r[1] >= layout::kRenderTargetCount) break;
      d.render_target[c.r[1]] = c.r[2];
      d.render_target_entry[c.r[1]] = Bind(base, c.r[2], Kind::kSurface);
      ++render_target_binds_;
      if (c.r[2]) render_targets_seen_.insert(c.r[2]);
      break;
    }
    case Api::kSetDepthStencilSurface: {
      Device& d = Dev(c.r[0]);
      d.depth_stencil = c.r[1];
      d.depth_stencil_entry = Bind(base, c.r[1], Kind::kSurface);
      ++depth_binds_;
      if (c.r[1]) depth_seen_.insert(c.r[1]);
      break;
    }
    case Api::kResolve:
      OnResolve(base, Dev(c.r[0]), c);
      break;
    case Api::kClear:
      ++clears_;
      ++clear_flags_[c.r[1]];
      break;
    case Api::kSwap:
      OnSwap(base, Dev(c.r[0]), c);
      break;
    case Api::kDrawVertices:
    case Api::kDrawIndexedVertices:
    case Api::kDrawIndexedVerticesUP:
    case Api::kBeginVertices:
      OnDraw(base, Dev(c.r[0]), c);
      break;
    case Api::kEndVertices:
      break;
    case Api::kLockVertexBuffer:
    case Api::kLockIndexBuffer:
    case Api::kTextureLevelAddress: {
      const int which = c.api == Api::kLockVertexBuffer ? 0 : c.api == Api::kLockIndexBuffer ? 1 : 2;
      const Kind kind = which == 0 ? Kind::kVertexBuffer : which == 1 ? Kind::kIndexBuffer : Kind::kTexture;
      ++locks_[which];
      locked_[which].insert(c.r[0]);
      if (Entry* e = Bind(base, c.r[0], kind)) ++e->locks;
      break;
    }
    default:
      break;
  }
}

void GraphicsState::OnDraw(const uint8_t* base, Device& d, const Call& c) {
  ++draws_;
  ++draws_by_api_[static_cast<size_t>(c.api)];
  ++frame_draws_;
  const bool inline_vertices = c.api == Api::kDrawIndexedVerticesUP || c.api == Api::kBeginVertices;
  if (c.api == Api::kBeginVertices) {
    ++inline_draws_;
    inline_bytes_ += static_cast<uint64_t>(c.r[2]) * c.r[3];
  }
  bool complete = true;
  if (!d.pipeline.valid) {
    ++missing_.pipeline;
    complete = false;
  } else {
    if (!d.declaration_entry) {
      ++missing_.declaration;
      complete = false;
    }
    // vertex streams: one vertex buffer for every vertex fetch constant the (patched) vertex shader reads; stream n is constant 95 - n
    if (const FetchUsage* vs = d.pipeline.vertex.get()) {
      for (unsigned word = 0; word < 3; ++word) {
        for (uint32_t bits = vs->vertex_slots[word]; bits != 0; bits &= bits - 1) {
          const unsigned j = word * 32 + static_cast<unsigned>(__builtin_ctz(bits));
          ++vertex_slots_used_;
          if (j < 80) {
            ++unmapped_vertex_slots_;
            continue;
          }
          const unsigned stream = 95 - j;
          if (inline_vertices && stream == 0) continue;  // the vertices are in the command buffer
          StreamBinding& s = d.streams[stream];
          if (!s.entry) {
            ++missing_.vertex_stream;
            complete = false;
          } else {
            ++s.entry->draws;
          }
        }
      }
    }
    // index buffer: only the draw that takes its indices from the bound buffer
    if (c.api == Api::kDrawIndexedVertices) {
      if (!d.index_buffer_entry) {
        ++missing_.index_buffer;
        complete = false;
      } else {
        ++d.index_buffer_entry->draws;
      }
    }
    // textures: every fetch constant a texture fetch of either shader reads
    uint32_t slots = 0;
    if (d.pipeline.vertex) slots |= d.pipeline.vertex->texture_slots;
    if (d.pipeline.pixel) slots |= d.pipeline.pixel->texture_slots;
    for (; slots != 0; slots &= slots - 1) {
      const unsigned s = static_cast<unsigned>(__builtin_ctz(slots));
      ++texture_slots_used_;
      if (s >= layout::kSamplerCount) {
        ++unmapped_texture_slots_;
        continue;
      }
      Entry* t = d.textures[s].entry;
      if (!t && (Shadow(base, c.r[0], s, 0) & 3) == 2) {
        // SetTexture(sampler, NULL) clears the pointer but leaves the fetch constant the GPU reads untouched: the shader samples whatever was bound before
        auto it = texture_by_base_.find(Shadow(base, c.r[0], s, 1) & ~0xFFFu);
        if (it != texture_by_base_.end() && it->second->kind == Kind::kTexture) {
          ++stale_texture_resolved_;
          ++it->second->draws;
          continue;
        }
      }
      if (!t) {
        ++missing_.texture;
        complete = false;
        if ((Shadow(base, c.r[0], s, 0) & 3) != 2) ++missing_texture_gpu_invalid_;  // the GPU's own constant says "no texture" either: the game never bound one
        if (missing_examples_.size() < 16) {
          char line[220];
          snprintf(line, sizeof(line), "%s sampler %u texture pointer %08X ps %016" PRIX64 " vs %016" PRIX64 " frame %" PRIu64, ApiName(c.api), s, d.textures[s].texture,
                   d.pipeline.pixel_hash, d.pipeline.vertex_hash, swaps_);
          missing_examples_.emplace_back(line);
        }
      } else {
        RefreshTexture(base, *t);
        ++t->draws;
      }
    }
  }
  if (!d.render_target_entry[0]) {
    ++draws_without_color_target_;  // legitimate for depth-only passes (shadow maps): then a depth surface must be bound
    if (!d.depth_stencil_entry) {
      ++missing_.render_target;
      complete = false;
    }
  }
  if (complete) ++draws_complete_;
  if (verify_) Verify(base, d, c);
}

void GraphicsState::OnResolve(const uint8_t* base, Device& d, const Call& c) {
  ++resolves_;
  ++frame_resolves_;
  ++resolve_flags_[c.r[1]];
  if (d.render_target_entry[0]) ++resolves_source_registered_;
  Entry* dst = c.r[3] ? Bind(base, c.r[3], Kind::kTexture) : nullptr;
  if (dst) {
    ++resolves_to_texture_;
    ++dst->draws;  // a resolve writes it; counted with the uses so "textures written" can be told from "textures only read"
    resolve_destinations_.insert(c.r[3]);
  }
  if (verify_) VerifyResolve(base, d, c, dst);
}

void GraphicsState::OnSwap(const uint8_t* base, Device& d, const Call& c) {
  (void)d;
  ++swaps_;
  front_buffers_.insert(c.r[1]);
  if (Bind(base, c.r[1], Kind::kTexture)) ++swaps_front_registered_;
  if (frame_draws_ == 0) ++frames_without_draws_;
  frame_draws_max_ = std::max(frame_draws_max_, frame_draws_);
  frame_resolves_max_ = std::max(frame_resolves_max_, frame_resolves_);
  frame_draws_ = 0;
  frame_resolves_ = 0;
}

// ---- verify mode ----

void GraphicsState::Expect(size_t index, uint64_t got, uint64_t want, uint32_t detail) {
  ++g_checks[index];
  ++verify_stats_.checks;
  if (got == want) return;
  ++g_mismatches[index];
  ++verify_stats_.mismatches;
  if (g_mismatches[index] <= 3) {
    char line[200];
    snprintf(line, sizeof(line), "%s: tracked/emitted %08" PRIX64 ", expected %08" PRIX64 " (detail %u)", kCheckNames[index], got, want, detail);
    verify_stats_.examples.emplace_back(line);
  }
}

void GraphicsState::Verify(const uint8_t* base, Device& d, const Call& c) {
  const uint32_t device = c.r[0];
  ++verify_stats_.draws;
  auto expect = [&](Check check, uint64_t got, uint64_t want, uint32_t detail = 0) { Expect(static_cast<size_t>(check), got, want, detail); };

  // 1. the bindings the hooks tracked equal the library's own fields
  expect(Check::kDeclaration, d.declaration, Be32(base, device + layout::kDeviceVertexDeclaration));
  expect(Check::kIndexBuffer, d.index_buffer, Be32(base, device + layout::kDeviceIndexBuffer));
  for (uint32_t i = 0; i < layout::kRenderTargetCount; ++i) expect(Check::kRenderTarget, d.render_target[i], Be32(base, device + layout::kDeviceRenderTarget + 4 * i), i);
  expect(Check::kDepthStencil, d.depth_stencil, Be32(base, device + layout::kDeviceDepthStencil));
  for (uint32_t s = 0; s < layout::kStreamCount; ++s) {
    expect(Check::kStreamBuffer, d.streams[s].vb, Be32(base, device + layout::kDeviceStreamSource + 4 * s), s);
    if (d.streams[s].vb) expect(Check::kStreamStride, d.streams[s].stride / 4, Be8(base, device + layout::kDeviceStreamStride + s), s);
  }
  for (uint32_t s = 0; s < layout::kSamplerCount; ++s) expect(Check::kTexturePointer, d.textures[s].texture, Be32(base, device + layout::kDeviceTexture + 4 * s), s);

  // 2. the fetch constants the library keeps for the GPU agree with the headers of the resources bound, for what the shaders read
  if (d.pipeline.valid) {
    uint32_t slots = 0;
    if (d.pipeline.vertex) slots |= d.pipeline.vertex->texture_slots;
    if (d.pipeline.pixel) slots |= d.pipeline.pixel->texture_slots;
    for (; slots != 0; slots &= slots - 1) {
      const unsigned s = static_cast<unsigned>(__builtin_ctz(slots));
      if (s >= layout::kSamplerCount || !d.textures[s].entry) continue;
      const Entry& t = *d.textures[s].entry;
      // SetTexture keeps the sampler state bits of the old constant (dword 0 bits 21:10, dword 1 bit 11, dword 5 bits 8:0)
      expect(Check::kTextureFetch0, Shadow(base, device, s, 0) & ~0x003FFC00u, t.words[0] & ~0x003FFC00u, s);
      expect(Check::kTextureFetch1, Shadow(base, device, s, 1) & ~0x800u, PhysicalAddress(t.words[1]) & ~0x800u, s);
      expect(Check::kTextureFetch2, Shadow(base, device, s, 2), t.words[2], s);
      expect(Check::kTextureFetch5, Shadow(base, device, s, 5) & ~0x1FFu, PhysicalAddress(t.words[5]) & ~0x1FFu, s);
    }
    if (const FetchUsage* vs = d.pipeline.vertex.get()) {
      const bool inline_vertices = c.api == Api::kDrawIndexedVerticesUP || c.api == Api::kBeginVertices;
      for (unsigned j = 80; j < 96; ++j) {
        if (!vs->UsesVertexSlot(j)) continue;
        const unsigned stream = 95 - j;
        if (inline_vertices && stream == 0) continue;
        const StreamBinding& sb = d.streams[stream];
        if (!sb.entry) continue;
        const uint32_t slot = j / 3, first = 2 * (j % 3);
        expect(Check::kVertexFetchAddress, Shadow(base, device, slot, first) & ~3u, PhysicalAddress(sb.entry->words[0] + sb.offset) & ~3u, stream);
        expect(Check::kVertexFetchSize, Shadow(base, device, slot, first + 1) & 0x03FFFFFCu, (sb.entry->words[1] - sb.offset) & 0x03FFFFFCu, stream);
      }
    }
  }

  // 3. the packets this draw appended: the DRAW_INDX is the draw we were told about, and every fetch constant register it wrote holds the shadow's value
  if (c.api != Api::kDrawVertices && c.api != Api::kDrawIndexedVertices) {
    ++verify_stats_.pm4_skipped;  // inline vertices: the draw header is followed by vertex data, not by packets
    return;
  }
  if (c.cmd_after <= c.cmd_before) {
    ++verify_stats_.pm4_skipped;
    return;
  }
  // The command buffer is written in chunks; a draw that did not fit in the rest of one moved the pointer to the next chunk (tens of KB further on, the
  // gap unwritten), so its packets are not one contiguous range. A draw's own packets are a few KB at most (the biggest seen: 10 KB).
  if (c.cmd_after - c.cmd_before > 32768) {
    ++verify_stats_.pm4_segment_switch;
    return;
  }
  ++verify_stats_.pm4_draws;
  // a draw whose packets continue in another command buffer segment (INDIRECT_BUFFER) is not contiguous in memory: counted, not parsed
  for (uint32_t p = c.cmd_before + 4; p <= c.cmd_after;) {
    const uint32_t header = Be32(base, p);
    const uint32_t type = header >> 30;
    if (type == 2) { p += 4; continue; }
    if (type == 1) { p += 8; continue; }
    const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
    if (type == 3 && ((header >> 8) & 0x7F) == 0x3F) {
      ++verify_stats_.pm4_segment_switch;
      --verify_stats_.pm4_draws;
      return;
    }
    p += 4 * (1 + count);
  }
  uint64_t packets = 0, indices = 0;
  uint32_t first_dma_base = 0, first_dma_size = 0, first_source = 0, first_primitive = 0;
  for (uint32_t p = c.cmd_before + 4; p <= c.cmd_after;) {
    const uint32_t header = Be32(base, p);
    const uint32_t type = header >> 30;
    if (type == 2) {
      p += 4;
      continue;
    }
    if (type == 1) {
      p += 8;
      continue;
    }
    const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
    if (p + 4 * count > c.cmd_after) break;
    if (type == 0) {
      const uint32_t reg = header & 0x7FFF, one_register = (header >> 15) & 1;
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t r = one_register ? reg : reg + i;
        if (r < kFetchRegisterBase || r >= kFetchRegisterBase + kFetchRegisterCount) continue;
        ++verify_stats_.pm4_fetch_registers;
        expect(Check::kPm4FetchRegister, Be32(base, p + 4 + 4 * i), Be32(base, device + layout::kDeviceFetchConstants + 4 * (r - kFetchRegisterBase)), r);
      }
    } else if (((header >> 8) & 0x7F) == kPm4DrawIndx && count >= 2) {
      const uint32_t initiator = Be32(base, p + 8);
      if (packets == 0) {
        first_primitive = initiator & 0x3F;
        first_source = (initiator >> 6) & 3;
        if (count >= 4) {
          first_dma_base = Be32(base, p + 12);
          first_dma_size = Be32(base, p + 16);
        }
      }
      ++packets;
      indices += initiator >> 16;
    }
    p += 4 * (1 + count);
  }
  verify_stats_.pm4_draw_packets += packets;
  const bool suspicious = packets == 0 || first_primitive != c.r[1] || (c.api == Api::kDrawIndexedVertices ? indices != c.r[4] : indices != c.r[3]);
  static int s_debug = getenv("GH2_D3D_VERIFY_DEBUG") ? atoi(getenv("GH2_D3D_VERIFY_DEBUG")) : 0;
  if (suspicious && s_debug > 0) {
    --s_debug;
    const char* rep = getenv("GH2_D3D_REPORT");
    FILE* dbg = rep ? fopen((std::string(rep) + ".debug").c_str(), "a") : stderr;
    fprintf(dbg, "D3DGFX-DEBUG %s device %08X args %08X %08X %08X %08X %08X range %08X..%08X (%u bytes) draw packets %" PRIu64 " indices %" PRIu64 " prim %u frame %" PRIu64 "\n",
            ApiName(c.api), device, c.r[1], c.r[2], c.r[3], c.r[4], c.r[5], c.cmd_before, c.cmd_after, c.cmd_after - c.cmd_before, packets, indices, first_primitive, swaps_);
    for (uint32_t p = c.cmd_before + 4; p <= c.cmd_after && p < c.cmd_before + 4 + 4 * 400;) {
      const uint32_t header = Be32(base, p), type = header >> 30;
      if (type == 2) { p += 4; continue; }
      if (type == 1) { p += 8; continue; }
      const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
      fprintf(dbg, "    %08X: %s %02X x%u%s\n", p, type == 0 ? "reg" : "op", type == 0 ? (header & 0x7FFF) : ((header >> 8) & 0x7F), count, "");
      p += 4 * (1 + count);
    }
    if (dbg != stderr) fclose(dbg);
  }
  expect(Check::kDrawPacketPresent, packets != 0, 1);
  if (packets == 0) return;
  expect(Check::kDrawPrimitive, first_primitive, c.r[1]);
  if (c.api == Api::kDrawIndexedVertices) {
    expect(Check::kDrawSource, first_source, 0);  // DMA
    expect(Check::kDrawCount, indices, c.r[4]);
    if (d.index_buffer_entry) {
      expect(Check::kDrawDmaBase, first_dma_base & ~3u, PhysicalAddress(d.index_buffer_entry->words[0] + 2 * c.r[3]) & ~3u);
    }
    expect(Check::kDrawDmaSize, first_dma_size & 0x00FFFFFFu, packets == 1 ? c.r[4] : first_dma_size & 0x00FFFFFFu);
  } else {  // DrawVertices
    expect(Check::kDrawSource, first_source, 2);  // auto index
    expect(Check::kDrawCount, indices, c.r[3]);
  }
}

void GraphicsState::VerifyResolve(const uint8_t* base, Device& d, const Call& c, Entry* destination) {
  (void)d;
  ++verify_stats_.resolves;
  if (!destination || c.cmd_after <= c.cmd_before || c.cmd_after - c.cmd_before > (1u << 20)) return;
  for (uint32_t p = c.cmd_before + 4; p <= c.cmd_after;) {
    const uint32_t header = Be32(base, p);
    const uint32_t type = header >> 30;
    if (type == 2) {
      p += 4;
      continue;
    }
    if (type == 1) {
      p += 8;
      continue;
    }
    const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
    if (p + 4 * count > c.cmd_after) break;
    if (type == 0 && (header & 0x7FFF) == kRbCopyControl && count >= 2 && ((header >> 15) & 1) == 0) {
      // RB_COPY_DEST_BASE is where the EDRAM contents land: the destination texture's base address
      // The copy lands at the texture's base, or, when the resolve targets a mip level (r8 = level, seen: 1..3 with the rectangle halving each time), inside its mip chain.
      const uint32_t want = PhysicalAddress(destination->words[1] & ~0xFFFu), got = Be32(base, p + 8);
      const uint32_t bpp = BitsPerPixel(destination->format);
      const uint64_t size = static_cast<uint64_t>(destination->width) * destination->height * (bpp ? bpp : 32) / 8;
      const bool mip_level = c.r[5] != 0;
      if (mip_level) ++verify_stats_.resolve_to_mip_level;
      if (got == want) ++verify_stats_.resolve_dest_exact;
      else if (getenv("GH2_D3D_VERIFY_DEBUG") && verify_stats_.resolves % 4001 == 0) {
        const char* rep = getenv("GH2_D3D_REPORT");
        if (FILE* dbg = rep ? fopen((std::string(rep) + ".debug").c_str(), "a") : nullptr) {
          fprintf(dbg, "RESOLVE flags %X rect(%u,%u,%u,%u) point %08X dest %ux%u fmt %u pitch %u base %08X got %08X delta %X args %08X %08X %08X\n", c.r[1], Be32(base, c.r[2]), Be32(base, c.r[2] + 4),
                  Be32(base, c.r[2] + 8), Be32(base, c.r[2] + 12), c.r[4], destination->width, destination->height, destination->format, destination->pitch, want, got, got - want, c.r[5], c.r[6], c.r[7]);
          fclose(dbg);
        }
      }
      Expect(static_cast<size_t>(Check::kResolveDestination), mip_level && got >= want && got < want + size * 4 / 3 + 0x1000 ? want : got, want, Plausible(c.r[2]) ? (Be32(base, c.r[2]) << 16 | (Be32(base, c.r[2] + 4) & 0xFFFF)) : 0xFFFFFFFFu);  // detail: source rect x1<<16|y1
    }
    p += 4 * (1 + count);
  }
}

// ---- reports ----

std::string GraphicsState::SummaryLine(double seconds) {
  std::lock_guard<std::mutex> lock(mu_);
  size_t counts[static_cast<size_t>(Kind::kCount)] = {};
  for (const auto& kv : entries_) ++counts[static_cast<size_t>(kv.second.kind)];
  char line[640];
  snprintf(line, sizeof(line),
           "D3DGFX T+%.0fs frames %" PRIu64 " | draws %" PRIu64 " complete %" PRIu64 " (missing: pipeline %" PRIu64 " decl %" PRIu64 " ib %" PRIu64 " vb %" PRIu64 " tex %" PRIu64 " rt %" PRIu64
           ") | resources: tex %zu vb %zu ib %zu surf %zu decl %zu | resolves %" PRIu64 " (to texture %" PRIu64 ") clears %" PRIu64 " | verify checks %" PRIu64 " mismatches %" PRIu64,
           seconds, swaps_, draws_, draws_complete_, missing_.pipeline, missing_.declaration, missing_.index_buffer, missing_.vertex_stream, missing_.texture, missing_.render_target,
           counts[static_cast<size_t>(Kind::kTexture)], counts[static_cast<size_t>(Kind::kVertexBuffer)], counts[static_cast<size_t>(Kind::kIndexBuffer)],
           counts[static_cast<size_t>(Kind::kSurface)], counts[static_cast<size_t>(Kind::kDeclaration)], resolves_, resolves_to_texture_, clears_, verify_stats_.checks,
           verify_stats_.mismatches);
  return line;
}

namespace {

// A tiny JSON writer: objects and scalars, commas handled
class Json {
 public:
  explicit Json(FILE* f) : f_(f) {}
  void Open(const char* key = nullptr) {
    Prefix(key);
    fputc('{', f_);
    first_.push_back(true);
  }
  void Close() {
    first_.pop_back();
    fputc('\n', f_);
    Indent(first_.size());
    fputc('}', f_);
  }
  void U(const char* key, uint64_t v) {
    Prefix(key);
    fprintf(f_, "%" PRIu64, v);
  }
  void D(const char* key, double v) {
    Prefix(key);
    fprintf(f_, "%.2f", v);
  }
  void B(const char* key, bool v) {
    Prefix(key);
    fputs(v ? "true" : "false", f_);
  }
  void S(const char* key, const std::string& v) {
    Prefix(key);
    fputc('"', f_);
    for (char ch : v) {
      if (ch == '"' || ch == '\\') fputc('\\', f_);
      fputc(ch, f_);
    }
    fputc('"', f_);
  }

 private:
  void Indent(size_t n) {
    for (size_t i = 0; i < n; ++i) fputs("  ", f_);
  }
  void Prefix(const char* key) {
    if (!first_.empty()) {
      if (!first_.back()) fputc(',', f_);
      first_.back() = false;
      fputc('\n', f_);
      Indent(first_.size());
    }
    if (key) fprintf(f_, "\"%s\": ", key);
  }
  FILE* f_;
  std::vector<bool> first_;
};

}  // namespace

void GraphicsState::WriteJson(FILE* f, double seconds) {
  std::lock_guard<std::mutex> lock(mu_);
  Json j(f);
  j.Open();
  j.D("seconds", seconds);
  j.B("verify", verify_);
  j.U("frames", swaps_);

  j.Open("calls");
  for (size_t i = 0; i < static_cast<size_t>(Api::kCount); ++i) j.U(ApiName(static_cast<Api>(i)), calls_[i]);
  j.Close();
  j.Open("sampler_state_calls_by_function");
  for (const auto& [address, n] : sampler_state_calls_) {
    char key[16];
    snprintf(key, sizeof(key), "%08X", address);
    j.U(key, n);
  }
  j.Close();

  j.Open("draws");
  j.U("total", draws_);
  j.U("complete", draws_complete_);
  j.D("complete_percent", draws_ ? 100.0 * static_cast<double>(draws_complete_) / static_cast<double>(draws_) : 100.0);
  j.Open("by_function");
  for (Api a : {Api::kDrawVertices, Api::kDrawIndexedVertices, Api::kDrawIndexedVerticesUP, Api::kBeginVertices}) j.U(ApiName(a), draws_by_api_[static_cast<size_t>(a)]);
  j.Close();
  j.Open("missing");
  j.U("pipeline", missing_.pipeline);
  j.U("vertex_declaration", missing_.declaration);
  j.U("index_buffer", missing_.index_buffer);
  j.U("vertex_stream", missing_.vertex_stream);
  j.U("texture", missing_.texture);
  j.U("render_target", missing_.render_target);
  j.Close();
  j.U("without_color_target_depth_only", draws_without_color_target_);
  j.U("missing_texture_where_gpu_constant_is_also_invalid", missing_texture_gpu_invalid_);
  j.U("sampler_pointer_null_but_gpu_constant_still_the_previous_texture", stale_texture_resolved_);
  j.Open("missing_texture_examples");
  for (size_t i = 0; i < missing_examples_.size(); ++i) j.S(std::to_string(i).c_str(), missing_examples_[i]);
  j.Close();
  j.U("vertex_fetch_slots_read", vertex_slots_used_);
  j.U("vertex_fetch_slots_not_a_stream", unmapped_vertex_slots_);
  j.U("texture_fetch_slots_read", texture_slots_used_);
  j.U("texture_fetch_slots_beyond_samplers", unmapped_texture_slots_);
  j.U("per_frame_max", frame_draws_max_);
  j.U("frames_without_draws", frames_without_draws_);
  j.U("inline_vertex_draws", inline_draws_);
  j.U("inline_vertex_bytes", inline_bytes_);
  {
    std::string threads;
    for (uint32_t t = 0; t < 32; ++t)
      if (draw_threads_ & (1u << t)) threads += (threads.empty() ? "" : ",") + std::to_string(t);
    j.S("draw_threads", threads);
  }
  j.Close();

  // resources: what the registry holds at this moment
  j.Open("resources");
  for (size_t k = 0; k < static_cast<size_t>(Kind::kCount); ++k) {
    uint64_t seen = 0, used = 0, bytes = 0, locks = 0, header_changes = 0;
    for (const auto& kv : entries_) {
      const Entry& e = kv.second;
      if (static_cast<size_t>(e.kind) != k) continue;
      ++seen;
      if (e.draws) ++used;
      locks += e.locks;
      header_changes += e.header_changes;
      if (e.kind == Kind::kVertexBuffer) bytes += e.words[1] & 0x03FFFFFCu;
      else if (e.kind == Kind::kIndexBuffer) bytes += e.words[1];
      else if (e.kind == Kind::kTexture) {
        const uint32_t bpp = BitsPerPixel(e.format);
        bytes += static_cast<uint64_t>(e.width) * e.height * (bpp ? bpp : 32) / 8;
      }
    }
    j.Open(KindName(static_cast<Kind>(k)));
    j.U("registered", seen);
    j.U("created_through_hooks", by_hook_[k]);
    j.U("adopted_when_first_bound", adopted_[k]);
    j.U("used_by_draws", used);
    j.U("wrong_type_when_bound", wrong_type_[k]);
    j.U("invalid_header", invalid_header_[k]);
    j.U("address_reused", reused_[k]);
    j.U("lock_events", locks);
    if (k == static_cast<size_t>(Kind::kVertexBuffer) || k == static_cast<size_t>(Kind::kIndexBuffer)) j.U("bytes", bytes);
    if (k == static_cast<size_t>(Kind::kTexture)) {
      j.U("level0_bytes_estimate", bytes);
      j.U("header_changes", header_changes);
      std::map<std::string, uint64_t> formats, sizes;
      uint64_t tiled = 0, mipmapped = 0;
      for (const auto& kv : entries_) {
        const Entry& e = kv.second;
        if (e.kind != Kind::kTexture) continue;
        ++formats[TextureFormatName(e.format)];
        char size[24];
        snprintf(size, sizeof(size), "%ux%u", e.width, e.height);
        ++sizes[size];
        tiled += e.tiled;
        mipmapped += ((e.words[4] >> 2) & 0xF) != ((e.words[4] >> 6) & 0xF);
      }
      j.U("tiled", tiled);
      j.Open("formats");
      for (const auto& [name, n] : formats) j.U(name.c_str(), n);
      j.Close();
      std::vector<std::pair<uint64_t, std::string>> top;
      for (const auto& [name, n] : sizes) top.emplace_back(n, name);
      std::sort(top.rbegin(), top.rend());
      j.Open("most_common_sizes");
      for (size_t i = 0; i < top.size() && i < 12; ++i) j.U(top[i].second.c_str(), top[i].first);
      j.Close();
    }
    if (k == static_cast<size_t>(Kind::kDeclaration)) {
      uint64_t elements = 0;
      for (const auto& kv : entries_)
        if (kv.second.kind == Kind::kDeclaration) elements += kv.second.element_count;
      j.U("elements", elements);
    }
    j.Close();
  }
  j.Close();

  j.Open("bindings");
  j.U("vertex_stream_binds", stream_binds_);
  j.U("vertex_stream_null_binds", null_stream_binds_);
  j.U("index_buffer_binds", index_binds_);
  j.U("index_buffer_null_binds", null_index_binds_);
  j.U("texture_binds", texture_binds_);
  j.U("texture_null_binds", null_texture_binds_);
  j.U("texture_header_changes_seen_at_draw", texture_header_changes_);
  j.U("render_target_binds", render_target_binds_);
  j.U("depth_stencil_binds", depth_binds_);
  j.U("distinct_render_targets", render_targets_seen_.size());
  j.U("distinct_depth_stencil_surfaces", depth_seen_.size());
  j.Close();

  j.Open("resolves");
  j.U("total", resolves_);
  j.U("to_registered_texture", resolves_to_texture_);
  j.U("with_render_target_bound", resolves_source_registered_);
  j.U("distinct_destination_textures", resolve_destinations_.size());
  j.U("per_frame_max", frame_resolves_max_);
  j.Open("by_flags");
  for (const auto& [flags, n] : resolve_flags_) {
    char key[16];
    snprintf(key, sizeof(key), "0x%X", flags);
    j.U(key, n);
  }
  j.Close();
  j.Close();

  j.Open("clears");
  j.U("total", clears_);
  j.Open("by_flags");
  for (const auto& [flags, n] : clear_flags_) {
    char key[16];
    snprintf(key, sizeof(key), "0x%X", flags);
    j.U(key, n);
  }
  j.Close();
  j.Close();

  j.Open("swaps");
  j.U("total", swaps_);
  j.U("front_buffer_registered", swaps_front_registered_);
  j.U("distinct_front_buffers", front_buffers_.size());
  j.Close();

  j.Open("cpu_access");
  j.U("vertex_buffer_locks", locks_[0]);
  j.U("distinct_vertex_buffers_locked", locked_[0].size());
  j.U("index_buffer_locks", locks_[1]);
  j.U("distinct_index_buffers_locked", locked_[1].size());
  j.U("texture_level_address_queries", locks_[2]);
  j.U("distinct_textures_queried", locked_[2].size());
  j.Close();

  j.Open("threads");
  for (const auto& [t, n] : thread_calls_) j.U(("guest_thread_" + std::to_string(t)).c_str(), n);
  j.Close();

  if (verify_) {
    j.Open("verify");
    j.U("draws_checked", verify_stats_.draws);
    j.U("checks", verify_stats_.checks);
    j.U("mismatches", verify_stats_.mismatches);
    j.U("pm4_draws_parsed", verify_stats_.pm4_draws);
    j.U("pm4_draws_skipped_inline_or_empty", verify_stats_.pm4_skipped);
    j.U("pm4_draw_packets", verify_stats_.pm4_draw_packets);
    j.U("pm4_fetch_registers_compared", verify_stats_.pm4_fetch_registers);
    j.U("resolves_checked", verify_stats_.resolves);
    j.U("pm4_draws_skipped_segment_switch", verify_stats_.pm4_segment_switch);
    j.U("resolve_destination_exactly_the_texture_base", verify_stats_.resolve_dest_exact);
    j.U("resolves_to_a_mip_level_above_0", verify_stats_.resolve_to_mip_level);
    j.Open("by_check");
    for (size_t i = 0; i < static_cast<size_t>(Check::kCount); ++i) {
      j.Open(kCheckNames[i]);
      j.U("checks", g_checks[i]);
      j.U("mismatches", g_mismatches[i]);
      j.Close();
    }
    j.Close();
    j.Open("first_mismatches");
    for (size_t i = 0; i < verify_stats_.examples.size(); ++i) j.S(std::to_string(i).c_str(), verify_stats_.examples[i]);
    j.Close();
    j.Close();
  }
  j.Close();
  fputc('\n', f);
}

}  // namespace gh2gfx::d3d
