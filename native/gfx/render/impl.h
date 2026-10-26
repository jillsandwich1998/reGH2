// The renderer's private state (Phase 3). One Impl, split over renderer.cpp (lifecycle, frames, present), device.cpp (descriptor heaps, samplers, shaders, pipelines, upload ring),
// targets.cpp (render targets, resolves, clears), textures.cpp (texture cache) and draw.cpp (a guest draw -> Vulkan commands). Everything runs on the command processor's thread except
// RegisterShader, which the guest threads call (shader_mutex).
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "fetch_scan.h"
#include "gh2render/renderer.h"
#include "shader_cache.h"
#include "xenos.h"

namespace gh2render {

// Reports an operation that took long enough to cost a frame (the renderer's stalls: first use of a pipeline, a texture, a target)
struct SlowTimer {
  const char* what;
  uint64_t arg;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  SlowTimer(const char* w, uint64_t a = 0) : what(w), arg(a) {}
  ~SlowTimer() {
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (ms > 4.0) std::fprintf(stderr, "RENDER slow: %s %.1f ms (%llX)\n", what, ms, (unsigned long long)arg);
  }
};

constexpr uint32_t kFramesInFlight = 3;
constexpr uint32_t kHeapDescriptors = 8192;     // entries of each bindless heap (2D, 3D, cube, samplers)
// the D3D library's own shaders (no container in the cache): recognised by the hash of their microcode, stood in for by builtin/*.{vert,frag}
constexpr uint64_t kBuiltinClearVs = 0xB171000000000001ull, kBuiltinRectVs = 0xB171000000000002ull, kBuiltinPassPs = 0xB171000000000003ull;

constexpr VkDeviceSize kRingBytes = 48ull << 20;   // per frame: constants, vertices, indices and texture staging

// What the D3D library told us about one shader container.
struct ShaderInfo {
  uint64_t hash = 0;
  bool vertex = false;
  std::vector<VertexElementInfo> elements;
};

// The shader a draw would run: the microcode IM_LOAD made active and what is known about it.
struct ActiveShader {
  std::shared_ptr<const ShaderInfo> info;   // null: the microcode address is not one the game's library created
  uint32_t address = 0, size_dwords = 0;
  bool immediate = false;                   // loaded with IM_LOAD_IMMEDIATE: the microcode below is the truth, memory is not read
  std::vector<uint32_t> at_load;            // the microcode as IM_LOAD saw it (what the GPU would have loaded)
  std::vector<uint32_t> microcode;          // as the last draw saw it (host order); a vertex shader is patched by the library to the current declaration
  gh2gfx::d3d::FetchUsage usage;
  bool scanned = false;
};

// A color or depth image the size of the frame. Render targets are not emulated through the EDRAM: each (base, format) pair the game renders to is its own image, resolves snapshot
// them into images keyed by the destination address.
struct TargetImage {
  gh2vk::Image image;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
  bool depth = false;
  VkImageView sampled_view = VK_NULL_HANDLE;   // resolved images: the view bound in the bindless heap
  uint32_t heap_index = 0;
  uint32_t width = 0, height = 0;
  uint32_t samples = 1;   // MSAA samples of a render target (the game's RB_SURFACE_INFO); resolved snapshots always have one
  uint64_t resolved_frame = 0;
  bool mips_valid = false;   // resolved colour snapshots at a scaled resolution carry a mip chain (see GenerateSnapshotMips); false after every resolve into them
  uint32_t base = 0, format = 0;   // render targets: the EDRAM base and format they stand for (profile labels)
  // Whole-image clears are costly at a high internal resolution (a 4x MSAA image at 4K is a gigabyte), and the game clears targets it never draws into over and over. A clear that would change
  // nothing is dropped: clear_state 1 = the image was cleared to clean_value and no pass has begun on it since (BeginRendering sets 0). Every clear is otherwise issued at once, in the game's order
  // (state 2 is only the moment between recording and issuing).
  uint8_t clear_state = 0;   // 0 the image has contents of its own, 1 it holds clean_value, 2 a clear to clean_value is being issued
  float clean_value[4] = {};
  uint32_t clean_stencil = 0;
};

struct TextureEntry {
  gh2vk::Image image;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  uint64_t hash = 0;
  uint64_t checked_frame = ~0ull;
  uint64_t used_frame = 0;
  uint32_t levels = 1;
  struct View {
    uint32_t swizzle;
    VkImageView view;
    uint32_t index;
  };
  std::vector<View> views;
};

// Everything a graphics pipeline of the renderer depends on. The shader containers are named by hash (vs_hash / ps_hash: a cache entry or one of the kBuiltin* stand-ins), so that a
// description survives the process: pipelines.cpp keeps the ones the game used in a file and builds them ahead of time on the next start.
struct PipelineDesc {
  uint64_t vs_hash = 0, ps_hash = 0;
  VkShaderModule vs = VK_NULL_HANDLE, ps = VK_NULL_HANDLE;
  uint32_t vs_spec = 0, ps_spec = 0;
  uint32_t samples = 1;          // of the render target the draw goes to
  bool rect = false, primitive_restart = false;
  VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkFormat color_format = VK_FORMAT_UNDEFINED;
  uint32_t color_mask = 0xF;
  uint32_t blend = 0;            // RB_BLENDCONTROL0 when blending is on, else 0
  uint32_t depth_control = 0;    // RB_DEPTHCONTROL
  uint32_t mode_control = 0;     // PA_SU_SC_MODE_CNTL
  std::vector<VkVertexInputBindingDescription> bindings;
  std::vector<VkVertexInputAttributeDescription> attributes;
};

// The sample mode the game renders with (RB_SURFACE_INFO bits 16-17: 0 = 1x, 1 = 2x, 2 = 4x). It identifies the image: the game reuses one EDRAM base with and without MSAA (the crowd is drawn at 4x
// into a base that other passes use at 1x), so every mode has its own image and clears are replayed between them whatever the GPU actually does.
inline float LodBias() {
  const char* e = std::getenv("GH2_RENDER_LOD_BIAS");
  return e ? static_cast<float>(std::atof(e)) : 0.f;
}
inline uint32_t TargetSamples(const uint32_t* regs) {
  const uint32_t mode = (regs[xenos::reg::RB_SURFACE_INFO] >> 16) & 3;
  return mode == 1 ? 2u : mode == 2 ? 4u : 1u;
}
// What the GPU draws with: the game's sample count, or one sample per pixel when the player turned multisampling off (GH2_MSAA=off or the menu). The image stays the mode's own.
inline std::atomic<bool>& MsaaFlag() {
  static std::atomic<bool> flag{[] {
    const char* e = std::getenv("GH2_MSAA");
    return !(e && (!std::strcmp(e, "off") || !std::strcmp(e, "0")));
  }()};
  return flag;
}
inline uint32_t HostSamples(uint32_t guest_samples) { return MsaaFlag().load(std::memory_order_relaxed) ? guest_samples : 1u; }

struct StreamBinding {          // one vertex buffer the draw reads
  uint32_t slot = 0;            // vertex fetch constant 0..95
  uint32_t stride = 0;          // bytes
  VkDeviceSize ring_offset = 0;
};

struct Renderer::Impl {
  Config config;
  RendererStats stats;
  gh2vk::Context vk;
  bool vk_ready = false, vk_failed = false;
  std::unique_ptr<gh2gfx::ShaderCache> shader_cache;
  std::mutex reload_mutex;
  std::string reload_dir;               // ReloadShaderCache: the directory to open at the next frame
  std::atomic<bool> reload_pending{false};
  void ApplyShaderReload();             // frame boundary, renderer thread
  uint32_t frame_width = 1280, frame_height = 720;   // size of every render target image: the guest's frame scaled to the internal resolution
  static constexpr uint32_t kGuestWidth = 1280, kGuestHeight = 720;   // the frame the game believes it renders (all its coordinates, scissors, resolve rectangles are in these pixels)
  uint32_t ScaleX(uint32_t guest) const { return static_cast<uint32_t>((static_cast<uint64_t>(guest) * frame_width + kGuestWidth / 2) / kGuestWidth); }
  uint32_t ScaleY(uint32_t guest) const { return static_cast<uint32_t>((static_cast<uint64_t>(guest) * frame_height + kGuestHeight / 2) / kGuestHeight); }
  float ScaleXf() const { return static_cast<float>(frame_width) / kGuestWidth; }
  float ScaleYf() const { return static_cast<float>(frame_height) / kGuestHeight; }

  // ---- frame slots: a command buffer, a fence and an upload ring each ----
  struct FrameSlot {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    gh2vk::Buffer ring;
    uint32_t ring_head = 0;
  } slots[kFramesInFlight];
  uint32_t slot_index = 0;
  FrameSlot* cur = nullptr;      // the slot being recorded; null between a swap and the next command
  VkCommandBuffer cmd = VK_NULL_HANDLE;

  // ---- device objects (device.cpp) ----
  VkDescriptorSetLayout set_layouts[4] = {};   // 2D textures, 3D textures, cube textures, samplers (spaces 0..3 of the translated shaders)
  VkDescriptorPool heap_pool = VK_NULL_HANDLE;
  VkDescriptorSet heap_sets[4] = {};
  uint32_t heap_next[4] = {1, 1, 1, 1};        // index 0 of every heap is the fallback
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkPipelineCache pipeline_cache = VK_NULL_HANDLE;
  gh2vk::Image dummy_2d, dummy_cube;
  VkSampler default_sampler = VK_NULL_HANDLE;
  std::unordered_map<uint32_t, std::pair<VkSampler, uint32_t>> samplers;   // packed sampler state -> (sampler, heap index)
  std::mutex module_mutex;   // shader_modules and missing_logged: the pipeline thread makes shader modules too
  std::unordered_map<uint64_t, VkShaderModule> shader_modules;
  std::unordered_map<std::string, VkPipeline> pipelines;   // the command processor's own view: every pipeline a draw has asked for
  std::unordered_map<uint64_t, bool> missing_logged;
  VkPipeline bound_pipeline = VK_NULL_HANDLE;

  // ---- pipeline cache on disk and pipelines made ahead of time (pipelines.cpp) ----
  // pipeline_cache.bin is the driver's blob (VkPipelineCache), pipelines.list the descriptions of the pipelines the game has used; a background thread builds the listed ones while the
  // game boots, so that the first draw that needs one finds it ready instead of compiling it in the middle of a frame. The same thread saves both files while the game runs.
  struct AheadEntry {   // a pipeline the background thread is building or has built
    VkPipeline pipeline = VK_NULL_HANDLE;
    bool done = false;
  };
  std::string pipeline_cache_dir;
  std::mutex ahead_mutex;
  std::condition_variable ahead_cv;
  std::unordered_map<std::string, std::shared_ptr<AheadEntry>> ahead;
  std::mutex list_mutex;
  std::vector<std::string> pipeline_list;                 // serialized PipelineDesc, in order of first use
  std::unordered_map<std::string, bool> pipeline_listed;  // keys already in pipeline_list
  std::atomic<bool> list_dirty{false};
  std::thread pipeline_thread;
  std::mutex thread_mutex;
  std::condition_variable thread_cv;
  std::atomic<bool> thread_stop{false};
  std::atomic<uint64_t> pipelines_on_demand{0}, on_demand_ns{0}, on_demand_max_ns{0}, pipelines_ahead{0}, ahead_ns{0}, ahead_hits{0}, ahead_wait_ns{0}, pipelines_listed_at_start{0};
  std::atomic<bool> ahead_active{false};
  std::vector<uint8_t> LoadFile(const std::string& path);
  void SaveFile(const std::string& path, const void* data, size_t size);
  void OpenPipelineCache();       // creates pipeline_cache from the file (device objects)
  void StartPipelineThread();     // after the shader cache is open
  void StopPipelineThread();      // saves the files a last time
  void SavePipelineFiles();
  void PipelineThreadMain();
  void NotePipelineUsed(const PipelineDesc& desc, const std::string& key);
  std::string PipelineKey(const PipelineDesc& desc) const;

  // ---- shader identity from the D3D library ----
  std::mutex shader_mutex;
  std::unordered_map<uint32_t, std::shared_ptr<const ShaderInfo>> shaders_by_microcode;
  std::unordered_map<uint64_t, std::shared_ptr<const ShaderInfo>> shaders_by_shape;   // for the microcode that arrives inside the command stream
  ActiveShader active_vertex, active_pixel;

  // ---- render targets and resolves (targets.cpp) ----
  std::unordered_map<uint64_t, std::unique_ptr<TargetImage>> color_targets, depth_targets;
  std::unordered_map<uint32_t, std::unique_ptr<TargetImage>> resolved_color, resolved_depth;   // keyed by the texture's base address (physical)
  std::vector<std::unique_ptr<TargetImage>> retired;   // replaced resolve images: the GPU may still read them, they are freed with the device
  TargetImage* rendering_color = nullptr;
  TargetImage* rendering_depth = nullptr;
  bool rendering = false;

  // ---- textures (textures.cpp) ----
  std::unordered_map<std::string, std::unique_ptr<TextureEntry>> textures;

  // ---- per-draw caches: the constants uploaded this frame ----
  uint32_t vs_constants_hash_valid = 0;
  std::vector<uint32_t> last_vs_constants, last_ps_constants;
  VkDeviceAddress last_vs_address = 0, last_ps_address = 0;

  // ---- debugging: GH2_RENDER_DUMP=<prefix> writes the frame the game shows to <prefix>_<frame>.ppm every GH2_RENDER_DUMP_EVERY frames (at most GH2_RENDER_DUMP_MAX) ----
  std::string dump_prefix;
  uint32_t dump_every = 0, dump_max = 0, dump_count = 0, dump_from = 0;
  gh2vk::Buffer dump_buffer;
  void DumpImage(const TargetImage& image, const char* name);   // after the frame's fence has been waited for; recorded on a one-shot command buffer
  bool dump_targets = false;                                     // GH2_RENDER_DUMP_TARGETS=1: also every color target and resolved image
  uint32_t debug = 0;                                            // GH2_RENDER_DEBUG bits: 1 no culling, 2 no depth/stencil, 4 no blending, 8 flip the front face, 16 every pixel shader writes magenta, 64 skip draws without the viewport transform, 128 pixel shaders show texture slot 0, 512 show pixel constant c5.y / texture alpha, 1024 do not mark the vertex position invariant
  VkShaderModule builtin_clear_vs = VK_NULL_HANDLE, builtin_rect_vs = VK_NULL_HANDLE, builtin_pass_ps = VK_NULL_HANDLE;
  VkShaderModule debug_flat_frag = VK_NULL_HANDLE, debug_tex0_frag = VK_NULL_HANDLE, debug_tex1_frag = VK_NULL_HANDLE;
  bool log_draws = false;                                        // GH2_RENDER_LOG=1: one line per issued draw (first 400)
  uint64_t skip_reasons[12] = {};
  uint32_t draw_in_frame = 0;
  std::atomic<uint64_t> memexport_draws{0}, dependent_fetch_draws{0}, dependent_fetch_bytes{0}, up_vertex_bytes{0}, up_const_bytes{0}, up_index_bytes{0}, up_const_hits{0}, up_const_total{0}, clear_replays{0}, clears_skipped{0}, alpha_to_mask_draws{0}, snapshot_mips_made{0}, clears_reopened{0}, vs_loads{0}, vs_loads_changed{0}, immediate_loads{0}, immediate_unknown{0};
  std::atomic<uint64_t> tex_hash_ns{0}, tex_decode_ns{0}, tex_decode_bytes{0};   // textures: re-hashing of every texture in use each frame; decoding (tiling, endian) of the ones that changed
  std::atomic<uint64_t> microcode_changed{0}, microcode_checked{0};   // draws whose microcode in memory differs from what IM_LOAD saw (the library re-patched it before the draw ran)
  std::atomic<uint64_t> tex_fallbacks[9] = {};   // texture bindings that read the fallback texel, by reason: type, 3D/format, range, image, staging, upload, heap, view, resolved heap   // issued draws so far in this frame (GH2_RENDER_ONLY)

  // ---- frame statistics for the periodic log line (window since the previous line) ----
  static constexpr int kBuckets = 8;   // < 12, 15, 18, 21, 26, 34, 50 ms, >= 50 ms
  std::atomic<uint64_t> swap_hist[kBuckets] = {}, cpu_hist[kBuckets] = {};
  std::atomic<uint64_t> swap_ns_sum{0}, swap_ns_max{0}, swap_count{0};
  std::atomic<uint64_t> cpu_ns_sum{0}, cpu_ns_max{0};          // renderer CPU time per frame (recording + submit)
  std::atomic<uint64_t> gpu_ns_sum{0}, gpu_ns_max{0}, gpu_frames{0};   // GPU time of a frame's command buffer (first to last timestamp)
  uint64_t last_swap_ns = 0, frame_cpu_ns = 0;
  std::atomic<uint64_t> section_ns[8] = {};   // draw cost by phase: shaders, indices, shader modules + targets, textures, uploads, pipeline, record, resolves
  uint64_t section_prev[8] = {}, hitch_textures_prev = 0, hitch_pipelines_prev = 0, hitches_logged = 0;   // the hitch lines (GH2_RENDER_HITCH_MS): what a long frame spent its time on
  VkQueryPool ts_pool = VK_NULL_HANDLE;
  bool ts_valid[kFramesInFlight] = {};
  static int Bucket(uint64_t ns) {
    const double ms = ns / 1e6;
    return ms < 12 ? 0 : ms < 15 ? 1 : ms < 18 ? 2 : ms < 21 ? 3 : ms < 26 ? 4 : ms < 34 ? 5 : ms < 50 ? 6 : 7;
  }

  // ---- trace and counters ----
  FILE* trace = nullptr;
  uint32_t trace_frames_left = 0, trace_skip = 0;
  uint64_t frame = 0, draws_this_frame = 0, resolves_this_frame = 0, drawn_this_frame = 0;

  // ---- GPU profile (GH2_RENDER_PROFILE=1): timestamps at the boundaries of render passes and resolves, the time between two marks goes to the label of the first ----
  static constexpr uint32_t kProfMax = 4096;
  bool prof = false, prof_draws = false, load_stats = false;   // GH2_RENDER_PROFILE=2: a mark before every draw too (a label per vertex/pixel shader pair)
  VkQueryPool prof_pool = VK_NULL_HANDLE;
  struct ProfSlot {
    uint32_t count = 0;
    std::vector<std::string> labels;
  } prof_slots[kFramesInFlight];
  std::mutex prof_mutex;
  std::map<std::string, std::pair<uint64_t, uint64_t>> prof_acc;   // label -> (GPU ns, segments)
  uint64_t prof_frames = 0;
  std::map<uint32_t, std::pair<uint64_t, uint64_t>> depth_use;   // depth snapshot address -> (times resolved, times a draw sampled it); GH2_RENDER_PROFILE
  void ProfMark(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
  void ProfCollect(uint32_t slot);   // the previous frame of this slot (its fence has been waited for)

  // ---- on-screen menu (overlay.cpp) ----
  struct OverlayTexture {
    gh2vk::Image image;
    VkDescriptorSet set = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    bool linear = true;
    std::vector<uint8_t> pixels;   // until the render thread has uploaded them
  };
  std::mutex overlay_mutex;
  std::shared_ptr<const OverlayFrame> overlay_frame;
  std::unordered_map<uint32_t, OverlayTexture> overlay_textures;   // id 0 = a white texel
  uint32_t overlay_next_id = 1;
  std::vector<uint32_t> overlay_dead;
  VkDescriptorSetLayout overlay_set_layout = VK_NULL_HANDLE;
  VkDescriptorPool overlay_pool = VK_NULL_HANDLE;
  VkPipelineLayout overlay_pipeline_layout = VK_NULL_HANDLE;
  VkPipeline overlay_pipeline = VK_NULL_HANDLE;
  VkFormat overlay_format = VK_FORMAT_UNDEFINED;
  VkShaderModule overlay_vs = VK_NULL_HANDLE, overlay_fs = VK_NULL_HANDLE;
  VkSampler overlay_samplers[2] = {};   // nearest, linear
  VkImageLayout DrawOverlay(const gh2vk::Context::Frame& target, VkExtent2D extent);   // records into cmd; the swapchain image comes in TRANSFER_DST; returns the layout it is left in (COLOR_ATTACHMENT when the menu was drawn)
  bool overlay_failed = false;
  // the application's screenshot key: the next presented picture (the game's own image at the internal resolution, no menu, no overlay) is read back and handed over
  std::atomic<bool> shot_pending{false};
  std::atomic<uint64_t> shot_after{0};         // the frame in progress when the screenshot was asked for: the picture is the next complete frame, whose draws are logged
  std::atomic<uint64_t> shot_log_frame{~0ull};
  std::mutex shot_mutex;
  std::deque<std::function<void(uint32_t, uint32_t, std::vector<uint8_t>)>> shot_callbacks;   // one picture per request, in order
  std::vector<uint8_t>* dump_out = nullptr;   // DumpImage: keep the 8-bit RGB here instead of writing a file
  bool dump_final = false;           // GH2_RENDER_DUMP_FINAL=1: the dumps also include the swapchain image (menu included)
  bool final_pending = false;
  gh2vk::Buffer final_buffer;
  VkExtent2D final_extent = {0, 0};
  void PrepareFinalDump(VkExtent2D extent, VkImage image, VkImageLayout* layout);
  void WriteFinalDump();
  void DestroyOverlay();
  bool InitOverlay(VkFormat format);

  // ---- options the player changes while the game runs ----
  std::atomic<uint32_t> opt_height{720}, opt_anisotropy{0}, opt_present{1};
  std::atomic<bool> opt_msaa{MsaaFlag().load()};
  void ApplyOptions();               // between two frames
  void DestroyTargets();             // every render target and resolved snapshot (a new internal resolution makes them all the wrong size)

  // lifecycle (renderer.cpp)
  bool InitVulkan();
  bool BeginFrame();                       // opens the current slot's command buffer if needed
  void SubmitFrame(bool present, const TargetImage* front);
  ~Impl();
  void Tracef(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
  bool Tracing() const { return trace != nullptr && trace_skip == 0 && trace_frames_left > 0; }

  // device (device.cpp)
  bool InitDeviceObjects();
  void DestroyDeviceObjects();
  uint8_t* RingAlloc(VkDeviceSize size, VkDeviceSize align, VkDeviceSize* offset);
  VkDeviceAddress RingAddress(VkDeviceSize offset) const { return cur->ring.address + offset; }
  uint32_t AllocHeapIndex(uint32_t heap);
  void WriteImageDescriptor(uint32_t heap, uint32_t index, VkImageView view, VkImageLayout layout);
  // lod_fixed >= 0: sample at exactly that level (resolved snapshots); base_only: never above level 0 (a snapshot's other levels are not always current)
  uint32_t GetSamplerIndex(const xenos::TextureFetch& t, float lod_fixed = -1.f, bool base_only = false, float lod_bias = 0.f);
  VkShaderModule GetShaderModule(uint64_t hash, gh2gfx::ShaderStage stage, uint32_t* spec_mask);
  uint32_t DependentMask(uint64_t vertex_shader_hash);   // elements of the shader read through dependent vertex fetches (cache entry, patch 0004)
  std::unordered_map<uint64_t, uint32_t> dependent_masks;
  void ImageBarrier(VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to, uint32_t levels = 1);
  void Transition(TargetImage& t, VkImageLayout to);
  bool OneShotClearInit();

  // targets (targets.cpp)
  static VkSampleCountFlagBits SampleBits(uint32_t samples);
  TargetImage* FindColorTarget(uint32_t base, uint32_t format, uint32_t samples);   // existing images only
  TargetImage* FindDepthTarget(uint32_t base, uint32_t format, uint32_t samples);
  TargetImage* GetColorTarget(uint32_t base, uint32_t format, uint32_t samples = 1);
  TargetImage* GetDepthTarget(uint32_t base, uint32_t format, uint32_t samples = 1);
  TargetImage* GetResolved(uint32_t virtual_base, bool depth, VkFormat format, uint32_t width, uint32_t height);
  void SetClear(TargetImage& t, const float color[4], float depth, uint32_t stencil);   // clear unless the image already holds that value (see TargetImage::clear_state)
  void MaterializeClear(TargetImage& t);                                               // issues the clear of state 2
  void BeginRendering(TargetImage* color, TargetImage* depth);
  void EndRendering();
  void Resolve(const uint32_t* regs);
  TargetImage* FindResolved(uint32_t physical_base);        // color snapshots only (what a swap can show)
  TargetImage* FindResolvedAny(uint32_t physical_base);     // color or depth (what a texture fetch can read)

  // textures (textures.cpp). Returns the heap index to use for the fetch constant (in the 2D heap, or the cube heap when *heap is 2); 0 = fallback
  uint32_t PrepareTexture(const uint32_t* fetch_constant, uint32_t* sampler_index, uint32_t* heap, bool multitap = false, bool blended = false);
  void GenerateSnapshotMips(TargetImage& t);

  // draw (draw.cpp)
  void Draw(const uint32_t* regs, const gh2gpu::DrawParams& d);
  bool RefreshShader(ActiveShader& shader, bool vertex);
  VkPipeline GetPipeline(const PipelineDesc& desc);     // pipelines.cpp: the draw's pipeline (already built ahead of time, or built now)
  VkPipeline BuildPipeline(const PipelineDesc& desc);   // the Vulkan call (any thread)
};

}  // namespace gh2render
