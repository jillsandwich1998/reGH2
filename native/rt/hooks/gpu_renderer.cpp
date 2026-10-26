// Chooses what draws for --gpu=native: the Vulkan renderer when a window is available (GH2_GPU_RENDERER=null forces the counting null sink), and feeds it what only the game's D3D library
// knows (which microcode is which shader container) through the D3D hooks' observer.
#include "gpu_renderer.h"

#include <gh2gfx/guest_d3d.h>
#include <gh2render/renderer.h>
#include <gh2rt/host_services.h>
#include <gh2rt/render_menu.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "fetch_scan.h"
#include "shader_cache.h"

namespace gh2rt::gpu {
namespace {

// The "null GPU": the game runs against a command processor that executes every synchronisation packet and draws nothing. It counts what it was asked to draw.
class NullSink final : public gh2gpu::Sink {
 public:
  void OnDraw(const uint32_t*, const gh2gpu::DrawParams& d) override { ++by_prim_[d.prim_type & 0x3F]; }

 private:
  std::atomic<uint64_t> by_prim_[64] = {};
};

uint32_t Be32(const uint8_t* b, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, b + a, 4);
  return __builtin_bswap32(v);
}

// The renderer and the D3D hooks meet here: shader creation gives the microcode address of every shader object, the CPU-write hooks say when memory the GPU reads has changed.
class RendererObserver final : public gh2gfx::d3d::Observer {
 public:
  explicit RendererObserver(gh2render::Renderer* renderer) : renderer_(renderer) {}
  void OnShaderCreated(const uint8_t* base, gh2gfx::d3d::Stage stage, uint32_t container, uint32_t object) override {
    if (!object) return;
    size_t size = 0;
    gh2gfx::ShaderStage parsed;
    if (!gh2gfx::ShaderCache::ParseContainerHeader(base + container, 1 << 20, &size, &parsed)) return;
    const uint64_t hash = gh2gfx::ShaderCache::HashContainer(base + container, size);
    const bool vertex = stage == gh2gfx::d3d::Stage::kVertex;
    // IM_LOAD carries the object's microcode pointer plus the offset of the microcode inside the container's physical part (0 for the vertex shaders seen, 0x40 for pixel ones)
    const uint32_t shader_offset = Be32(base, container + gh2gfx::d3d::container::kShaderOffset);
    const uint32_t physical_offset = Be32(base, container + shader_offset);
    const uint32_t microcode = Be32(base, object + (vertex ? gh2gfx::d3d::layout::kVertexUcodePointer : gh2gfx::d3d::layout::kPixelUcodePointer)) + physical_offset;
    // a vertex shader's container also lists which vertex fetch instruction feeds which input: the shader struct is
    //   +24 field18 (index of the first element), +28 element count, +36 the table (u32: address:12, usage:4, usage index:4)
    std::vector<gh2render::VertexElementInfo> elements;
    if (vertex) {
      const uint32_t shader = container + shader_offset;
      const uint32_t first = Be32(base, shader + 24), count = Be32(base, shader + 28);
      if (count <= 64 && first <= 64) {
        for (uint32_t i = 0; i < count; ++i) {
          const uint32_t e = Be32(base, shader + 36 + 4 * (first + i));
          elements.push_back({static_cast<uint16_t>(e & 0xFFF), static_cast<uint8_t>((e >> 12) & 0xF), static_cast<uint8_t>((e >> 16) & 0xF), 0, 0});
        }
      }
    }
    // the microcode as compiled (the container's physical part; the library patches its own copy): its fetch instructions say which register each element lands in, and its shape (everything but
    // the fetches) identifies the shader when a patched copy arrives inside the command stream (IM_LOAD_IMMEDIATE)
    uint64_t shape = 0;
    {
      const uint32_t virtual_size = Be32(base, container + gh2gfx::d3d::container::kVirtualSize);
      const uint32_t ucode_bytes = Be32(base, container + shader_offset + 4);
      if (ucode_bytes && ucode_bytes <= 16384 && (ucode_bytes & 3) == 0 && virtual_size + physical_offset + ucode_bytes <= size) {
        std::vector<uint32_t> words(ucode_bytes / 4);
        for (size_t i = 0; i < words.size(); ++i) words[i] = Be32(base, container + virtual_size + physical_offset + 4 * static_cast<uint32_t>(i));
        shape = gh2render::MicrocodeShapeHash(words.data(), words.size());
        if (vertex) {
          const gh2gfx::d3d::FetchUsage original = gh2gfx::d3d::ScanFetches(words.data(), words.size());
          for (const gh2gfx::d3d::VertexFetch& vf : original.vertex_fetches)
            for (gh2render::VertexElementInfo& e : elements)
              if (e.instruction == vf.instruction) {
                e.dst = vf.dst;
                e.dst_swizzle = vf.dst_swizzle;
              }
        }
      }
    }
    renderer_->RegisterShader(gh2gfx::d3d::PhysicalAddress(microcode) & ~3u, hash, vertex, std::move(elements), shape);
  }
  void OnDrawStateCommit(const uint8_t*, uint32_t, uint32_t, uint32_t) override {}

 private:
  gh2render::Renderer* renderer_;
};

std::unique_ptr<RendererObserver> g_observer;
std::mutex g_present_mutex;
std::shared_ptr<std::function<void(uint32_t, uint32_t)>> g_present;   // the application's menu tick
std::unique_ptr<gh2render::Renderer> g_renderer_owner;

// The renderer is the sink; the observer must outlive the guest threads that call it, so both live for the process.
class RendererSink final : public gh2gpu::Sink {
 public:
  explicit RendererSink(gh2render::Renderer* r) : r_(r) {}
  void OnShaderLoad(uint32_t t, uint32_t p, uint32_t s) override { r_->OnShaderLoad(t, p, s); }
  void OnShaderLoadImmediate(uint32_t t, const uint32_t* w, uint32_t n) override { r_->OnShaderLoadImmediate(t, w, n); }
  void OnDraw(const uint32_t* regs, const gh2gpu::DrawParams& d) override { r_->OnDraw(regs, d); }
  void OnSwap(const uint32_t* regs, uint32_t f, uint32_t w, uint32_t h) override { r_->OnSwap(regs, f, w, h); }
  void OnIdle() override { r_->OnIdle(); }

 private:
  gh2render::Renderer* r_;
};

}  // namespace

void SetPresentCallback(std::function<void(uint32_t, uint32_t)> callback) {
  std::lock_guard<std::mutex> lock(g_present_mutex);
  g_present = callback ? std::make_shared<std::function<void(uint32_t, uint32_t)>>(std::move(callback)) : nullptr;
}

gh2render::Renderer* ActiveRenderer() { return g_renderer_owner.get(); }

std::string RendererStatsLine() { return g_renderer_owner ? g_renderer_owner->StatsLine() : std::string(); }

std::unique_ptr<gh2gpu::Sink> CreateSink(const gh2gpu::GuestMemory& memory) {
  if (const char* e = std::getenv("GH2_GPU_RENDERER"); e && std::string(e) == "null") {
    std::fprintf(stderr, "GPU renderer: null (nothing is drawn)\n");
    return std::make_unique<NullSink>();
  }
  gh2rt::NativeWindow window;
  if (!gh2rt::Host().GetNativeWindow(&window) || window.kind == gh2rt::NativeWindow::Kind::kNone) {
    std::fprintf(stderr, "GPU renderer: no window to present to: nothing is drawn\n");
    return std::make_unique<NullSink>();
  }
  gh2render::Config config;
  config.memory = memory;
  config.window.kind = window.kind == gh2rt::NativeWindow::Kind::kWayland ? gh2vk::WindowInfo::Kind::kWayland : gh2vk::WindowInfo::Kind::kXlib;
  config.window.display = window.display;
  config.window.surface = window.surface;
  config.window.width = window.width;
  config.window.height = window.height;
  config.render_height = gh2rt::Host().GetSettings().render_height;
  if (config.render_height == 0) config.render_height = window.height ? std::min(2160u, std::max(720u, window.height->load())) : 720u;   // "auto": as tall as the window
  std::string cache = gh2rt::Host().GetSettings().shader_cache_dir;
  if (const char* e = std::getenv("GH2_SHADER_CACHE"); e && *e) cache = e;
  config.shader_cache_dir = cache;
  config.on_present = [](uint32_t w, uint32_t h) {
    std::shared_ptr<std::function<void(uint32_t, uint32_t)>> cb;
    {
      std::lock_guard<std::mutex> lock(g_present_mutex);
      cb = g_present;
    }
    if (cb) (*cb)(w, h);
  };
  if (const char* t = std::getenv("GH2_GPU_TRACE"); t && *t) {
    config.trace_path = t;
    const char* n = std::getenv("GH2_GPU_TRACE_FRAMES");
    config.trace_frames = n ? static_cast<uint32_t>(std::atoi(n)) : 120;
  }
  std::string error;
  g_renderer_owner = gh2render::Renderer::Create(config, &error);
  if (!g_renderer_owner) {
    std::fprintf(stderr, "GPU renderer: %s: nothing is drawn\n", error.c_str());
    return std::make_unique<NullSink>();
  }
  g_observer = std::make_unique<RendererObserver>(g_renderer_owner.get());
  gh2gfx::d3d::SetObserver(g_observer.get());
  std::fprintf(stderr, "GPU renderer: Vulkan (shader cache '%s')\n", cache.c_str());
  return std::make_unique<RendererSink>(g_renderer_owner.get());
}

}  // namespace gh2rt::gpu
