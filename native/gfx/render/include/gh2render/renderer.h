// gh2render::Renderer - the native renderer (Phase 3): a gh2gpu::Sink that turns the command stream into Vulkan.
//
// It receives what the GPU would execute (state registers, draws, shader loads, the swap) from the command processor and draws it with the translated shaders (SPIR-V from the shader
// cache). Resource identity that only the game's D3D library knows (which microcode belongs to which shader container, when the CPU rewrote a buffer) arrives through the hooks'
// observer (gh2gfx::d3d), see rt/hooks/gpu_renderer.cpp. Plain C++17 + Vulkan; no SDK.
#pragma once

#include <gh2gpu/command_processor.h>
#include <gh2vk/context.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gh2render {

// One entry of a vertex shader container's element table: which vertex fetch instruction (by index in the microcode) feeds which input (D3D declaration usage / usage index).
struct VertexElementInfo {
  uint16_t instruction;   // index of the fetch instruction in the shader as compiled
  uint8_t usage;
  uint8_t usage_index;
  uint8_t dst;            // that instruction's destination register and swizzle: the library reorders the fetches when it patches the shader to a vertex declaration, so a patched fetch is matched by these
  uint16_t dst_swizzle;
};

// A hash of a microcode that ignores the vertex fetch instructions (which the library rewrites): the same for a vertex shader as compiled and for any of its patched copies.
uint64_t MicrocodeShapeHash(const uint32_t* words, size_t count);

// The on-screen menu: draw lists the application records (Dear ImGui's, through the SDK's ImmediateDrawer) and the renderer draws over the finished picture at the window's resolution.
struct OverlayVertex {
  float x, y, u, v;
  uint32_t color;   // R in the low byte
};
struct OverlayDraw {
  uint32_t texture = 0;   // from Renderer::OverlayCreateTexture; 0 = flat colour
  uint32_t index_offset = 0, index_count = 0;
  int32_t base_vertex = 0;
  float clip[4] = {0, 0, 0, 0};   // left, top, right, bottom in the coordinate space of the frame
};
struct OverlayFrame {
  std::vector<OverlayVertex> vertices;
  std::vector<uint16_t> indices;
  std::vector<OverlayDraw> draws;
  float width = 0, height = 0;   // the coordinate space (the window's size in pixels)
};

// What the player can change while the game runs (the application's menu); applied between two frames.
struct RenderOptions {
  uint32_t height = 720;       // internal resolution (16:9)
  bool msaa = true;            // the multisampling the game asks for
  uint32_t anisotropy = 0;     // 0 = as the game sets each sampler; 2..16 = at least that for every filtered texture
  uint32_t present_mode = 1;   // 0 vertical sync (FIFO), 1 mailbox (no tearing, lower latency), 2 off (immediate: tearing, lowest latency); the game itself stays at 60 fps
};

struct Config {
  gh2gpu::GuestMemory memory;
  gh2vk::WindowInfo window;
  std::filesystem::path shader_cache_dir;     // holds shaders.pak
  std::filesystem::path pipeline_cache_dir;   // pipeline_cache.bin and pipelines.list (empty: GH2_PIPELINE_CACHE_DIR, else $XDG_CACHE_HOME/gh2, else ~/.cache/gh2; GH2_PIPELINE_CACHE=off disables)
  std::string trace_path;                     // GH2_GPU_TRACE: one text line per draw/resolve/clear/swap of the first frames
  uint32_t trace_frames = 0;
  std::function<void(uint32_t width, uint32_t height)> on_present;   // after every swap, on the command processor's thread: the window's size in pixels (the application schedules its menu's frame)
  uint32_t render_height = 720;               // internal resolution: the height of every render target (the game's 1280x720 frame, scaled by render_height / 720; width = 16:9). 720 = the console's own image
};

// Counters the application reports (read from other threads).
struct RendererStats {
  std::atomic<uint64_t> frames_presented{0}, frames_dropped{0};
  std::atomic<uint64_t> draws_seen{0}, draws_issued{0}, draws_skipped{0};
  std::atomic<uint64_t> resolves{0}, clears{0};
  std::atomic<uint64_t> shader_misses{0}, pipelines_created{0}, textures_uploaded{0}, ring_overflows{0};
  std::atomic<uint64_t> gpu_memory_bytes{0}, ring_peak_bytes{0};
  std::atomic<uint64_t> record_ns{0};   // CPU time the renderer spent turning draws into Vulkan commands
};

class Renderer : public gh2gpu::Sink {
 public:
  static std::unique_ptr<Renderer> Create(const Config& config, std::string* error);
  ~Renderer() override;

  void OnShaderLoad(uint32_t type, uint32_t physical, uint32_t size_dwords) override;
  void OnShaderLoadImmediate(uint32_t type, const uint32_t* words, uint32_t size_dwords) override;
  void OnDraw(const uint32_t* regs, const gh2gpu::DrawParams& draw) override;
  void OnSwap(const uint32_t* regs, uint32_t front_buffer_physical, uint32_t width, uint32_t height) override;
  void OnIdle() override;

  // From the game's D3D library (any thread): a shader container was turned into a shader object whose microcode lives at `microcode_physical`.
  void RegisterShader(uint32_t microcode_physical, uint64_t container_hash, bool vertex, std::vector<VertexElementInfo> elements, uint64_t shape_hash);
  // The CPU may have rewritten [physical, physical + size): cached copies of it are stale.
  void NoteCpuWrite(uint32_t physical, uint32_t size);

  // On-screen menu (any thread). Textures are RGBA8; the frame replaces the previous one and is drawn at every swap until replaced (nullptr removes the menu).
  uint32_t OverlayCreateTexture(uint32_t width, uint32_t height, bool linear, const uint8_t* rgba);
  void OverlayDestroyTexture(uint32_t id);
  void OverlaySubmit(std::shared_ptr<const OverlayFrame> frame);
  // Asks for the next presented picture, as the game drew it (internal resolution, without the menu): 8-bit RGB, rows top to bottom. `done` runs on the renderer's thread.
  void RequestScreenshot(std::function<void(uint32_t width, uint32_t height, std::vector<uint8_t> rgb)> done);
  // The shader cache was made while the game runs (the application's first-start build): open it from `dir` at the next frame. Any thread.
  void ReloadShaderCache(const std::string& dir);
  void SetOptions(const RenderOptions& options);   // takes effect at the next frame
  RenderOptions GetOptions() const;

  const RendererStats& stats() const;
  std::string StatsLine() const;

 private:
  Renderer();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace gh2render
