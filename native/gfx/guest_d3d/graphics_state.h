// graphics_state.h - everything a frame is built from, as the game's D3D library is told about it (Phase 2.6).
//
// The observer of G.0 (observer.h) knew the two shaders of every draw. This class follows the rest: the vertex declaration, the vertex streams, the index
// buffer, the textures and their samplers, the render targets, the resolves (copies of the render target to a texture), the clears and the swaps. It is fed by
// the hooks of d3d_hooks.cpp through Observer::OnCall and by the shader observer through OnPipeline, keeps a registry of every resource it has seen (a header
// the library wrote, validated by its type) and, at every draw, checks that everything the bound shaders read is bound to a registered resource.
//
// Observation only: nothing is drawn. With verify on (GH2_D3D_VERIFY=1) it also compares, at every draw, its own idea of the bindings with the device
// structure and with the fetch constants the library wrote for the GPU, and compares the packets the draw appended to the command buffer with what the hook
// was told; the counters of those comparisons are the evidence that the hooks see 100% of the state.
//
// Plain C++ (no guest macros, no SDK).
#pragma once

#include <gh2gfx/guest_d3d.h>

#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "fetch_scan.h"

namespace gh2gfx::d3d {

// What the last draw-state commit bound on a device: the shaders and what their microcode reads.
struct PipelineInfo {
  bool valid = false;
  uint64_t vertex_hash = 0, pixel_hash = 0;                   // the shader cache's keys (0 = no shader)
  std::shared_ptr<const FetchUsage> vertex, pixel;            // the microcode as it sits in memory now (vertex: patched to the declaration)
};

enum class Kind : uint8_t { kVertexBuffer, kIndexBuffer, kTexture, kSurface, kDeclaration, kCount };
const char* KindName(Kind kind);
const char* TextureFormatName(uint32_t format);

// A resource object the library gave out (or that was first seen when bound), with the fields of its header the renderer needs.
struct Entry {
  Kind kind = Kind::kTexture;
  uint32_t ptr = 0;
  uint32_t common = 0;
  bool by_hook = false;          // returned by a hooked creation function; else adopted when first bound
  uint32_t words[6] = {};        // texture: the 6 fetch constant dwords; buffers: {address word, size word}; surface: {info, color/depth info, ..., D3DFORMAT in [5]}
  uint32_t creation[4] = {};     // arguments of the creation call (texture: width, height, levels, format; surface: width, height, format, multisample; buffers: length, usage, format)
  uint32_t element_count = 0, stream_mask = 0;   // declaration, from the element array given at creation
  uint64_t binds = 0, draws = 0, locks = 0, header_changes = 0;
  // texture, decoded from words[]
  uint32_t width = 0, height = 0, format = 0, pitch = 0, base = 0;
  bool tiled = false;
};

class GraphicsState {
 public:
  explicit GraphicsState(bool verify);

  void OnCall(const uint8_t* base, const Call& call);
  void OnPipeline(uint32_t device, const PipelineInfo& pipeline);

  // One line for the periodic log, and the "graphics" object of the JSON report (everything between the braces, indented by one space).
  std::string SummaryLine(double seconds);
  void WriteJson(FILE* f, double seconds);

  uint64_t Draws() const;
  uint64_t CompleteDraws() const;
  uint64_t VerifyMismatches() const;

 private:
  struct StreamBinding {
    uint32_t vb = 0, offset = 0, stride = 0;
    Entry* entry = nullptr;
  };
  struct TextureBinding {
    uint32_t texture = 0;
    Entry* entry = nullptr;
  };
  struct Device {
    uint32_t declaration = 0, fvf = 0, index_buffer = 0, depth_stencil = 0;
    Entry *declaration_entry = nullptr, *index_buffer_entry = nullptr, *depth_stencil_entry = nullptr;
    uint32_t render_target[4] = {};
    Entry* render_target_entry[4] = {};
    StreamBinding streams[16];
    TextureBinding textures[26];
    PipelineInfo pipeline;
  };

  Device& Dev(uint32_t device);
  // Looks `ptr` up (or validates its header and registers it) as a resource of kind `want`; null when 0 or not what it should be.
  Entry* Bind(const uint8_t* base, uint32_t ptr, Kind want);
  Entry* Create(const uint8_t* base, uint32_t ptr, Kind kind, const uint32_t (&creation)[4]);
  bool ReadHeader(const uint8_t* base, Entry& e);
  void IndexTexture(Entry& e);
  void RefreshTexture(const uint8_t* base, Entry& e);

  void OnDraw(const uint8_t* base, Device& d, const Call& c);
  void OnResolve(const uint8_t* base, Device& d, const Call& c);
  void OnSwap(const uint8_t* base, Device& d, const Call& c);

  // verify mode
  void Verify(const uint8_t* base, Device& d, const Call& c);
  void VerifyResolve(const uint8_t* base, Device& d, const Call& c, Entry* destination);
  void Expect(size_t check, uint64_t got, uint64_t want, uint32_t detail = 0);   // `check` is a Check of graphics_state.cpp

  const bool verify_;
  std::mutex mu_;
  std::unordered_map<uint32_t, Entry> entries_;        // node based: Entry* stays valid
  std::unordered_map<uint32_t, Device> devices_;
  uint32_t last_device_ = 0;
  Device* last_device_state_ = nullptr;

  // counters
  uint64_t calls_[static_cast<size_t>(Api::kCount)] = {};
  std::map<uint32_t, uint64_t> sampler_state_calls_;   // by guest function
  uint64_t draws_ = 0, draws_complete_ = 0;
  uint64_t draws_by_api_[static_cast<size_t>(Api::kCount)] = {};
  struct Missing {
    uint64_t pipeline = 0, declaration = 0, index_buffer = 0, vertex_stream = 0, texture = 0, render_target = 0;
  } missing_;
  std::unordered_map<uint32_t, Entry*> texture_by_base_;   // physical base address of a texture -> its entry
  uint64_t stale_texture_resolved_ = 0;
  uint64_t draws_without_color_target_ = 0, missing_texture_gpu_invalid_ = 0;
  std::vector<std::string> missing_examples_;
  uint64_t unmapped_vertex_slots_ = 0, unmapped_texture_slots_ = 0, vertex_slots_used_ = 0, texture_slots_used_ = 0;
  uint64_t adopted_[static_cast<size_t>(Kind::kCount)] = {}, by_hook_[static_cast<size_t>(Kind::kCount)] = {};
  uint64_t invalid_header_[static_cast<size_t>(Kind::kCount)] = {}, wrong_type_[static_cast<size_t>(Kind::kCount)] = {}, reused_[static_cast<size_t>(Kind::kCount)] = {};
  uint64_t texture_header_changes_ = 0;
  uint64_t null_texture_binds_ = 0, texture_binds_ = 0, stream_binds_ = 0, null_stream_binds_ = 0, index_binds_ = 0, null_index_binds_ = 0;
  uint64_t render_target_binds_ = 0, depth_binds_ = 0;
  std::unordered_set<uint32_t> render_targets_seen_, depth_seen_;
  // resolves, clears, swaps
  uint64_t resolves_ = 0, resolves_to_texture_ = 0, resolves_source_registered_ = 0;
  std::map<uint32_t, uint64_t> resolve_flags_, clear_flags_;
  std::unordered_set<uint32_t> resolve_destinations_, front_buffers_;
  uint64_t clears_ = 0, swaps_ = 0, swaps_front_registered_ = 0;
  uint64_t frame_draws_ = 0, frame_resolves_ = 0, frame_draws_max_ = 0, frame_resolves_max_ = 0, frames_without_draws_ = 0;
  // inline vertices
  uint64_t inline_draws_ = 0, inline_bytes_ = 0;
  // CPU access to GPU-visible memory
  uint64_t locks_[3] = {};   // vertex buffer, index buffer, texture level address
  std::unordered_set<uint32_t> locked_[3];
  // threads
  std::map<uint32_t, uint64_t> thread_calls_;
  uint32_t draw_threads_ = 0;

  // verify
  struct VerifyStats {
    uint64_t draws = 0, checks = 0, mismatches = 0;
    uint64_t pm4_draws = 0, pm4_draw_packets = 0, pm4_fetch_registers = 0, pm4_skipped = 0, pm4_segment_switch = 0, resolve_dest_exact = 0, resolve_to_mip_level = 0, resolves = 0;
    std::map<std::string, std::pair<uint64_t, uint64_t>> by_check;   // name -> {checks, mismatches}
    std::vector<std::string> examples;
  } verify_stats_;
};

}  // namespace gh2gfx::d3d
