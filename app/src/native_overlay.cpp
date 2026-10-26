#include "native_overlay.h"

#include <gh2rt/render_menu.h>
#include <rex/cvar.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/presenter.h>
#include <rex/ui/windowed_app_context.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <ctime>
#include <filesystem>

#include "png_writer.h"

uint32_t Gh2RenderHeight();   // gh2_host_services.cpp

namespace {

using SteadyClock = std::chrono::steady_clock;
std::atomic<int64_t> g_options_due_ns{0};   // when the renderer should follow a window resize (0: nothing pending)
int64_t NowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(SteadyClock::now().time_since_epoch()).count(); }

rex::ui::ImGuiDrawer* g_drawer = nullptr;
rex::ui::WindowedAppContext* g_context = nullptr;
std::atomic<bool> g_tick_pending{false};

class OverlayTexture final : public rex::ui::ImmediateTexture {
 public:
  OverlayTexture(uint32_t w, uint32_t h, uint32_t id) : ImmediateTexture(w, h), id_(id) {}
  ~OverlayTexture() override {
    if (id_)
      if (auto* r = gh2rt::gpu::ActiveRenderer()) r->OverlayDestroyTexture(id_);
  }
  uint32_t id() const { return id_; }

 private:
  uint32_t id_;
};

// Records what the ImGui drawer sends (batches of vertices and indices, draws with a texture and a scissor) into the renderer's frame description. Runs on the UI thread.
class OverlayDrawer final : public rex::ui::ImmediateDrawer {
 public:
  std::unique_ptr<rex::ui::ImmediateTexture> CreateTexture(uint32_t width, uint32_t height, rex::ui::ImmediateTextureFilter filter, bool, const uint8_t* data) override {
    uint32_t id = 0;
    if (auto* r = gh2rt::gpu::ActiveRenderer()) id = r->OverlayCreateTexture(width, height, filter == rex::ui::ImmediateTextureFilter::kLinear, data);
    return std::make_unique<OverlayTexture>(width, height, id);
  }

  void Begin(rex::ui::UIDrawContext& context, float width, float height) override {
    rex::ui::ImmediateDrawer::Begin(context, width, height);
    frame_ = std::make_shared<gh2render::OverlayFrame>();
    frame_->width = coordinate_space_width();
    frame_->height = coordinate_space_height();
  }

  void BeginDrawBatch(const rex::ui::ImmediateDrawBatch& batch) override {
    if (!frame_) return;
    vertex_base_ = static_cast<int32_t>(frame_->vertices.size());
    index_base_ = static_cast<uint32_t>(frame_->indices.size());
    static_assert(sizeof(gh2render::OverlayVertex) == sizeof(rex::ui::ImmediateVertex), "the draw lists are copied as they are");
    const auto* v = reinterpret_cast<const gh2render::OverlayVertex*>(batch.vertices);
    frame_->vertices.insert(frame_->vertices.end(), v, v + batch.vertex_count);
    if (batch.indices) frame_->indices.insert(frame_->indices.end(), batch.indices, batch.indices + batch.index_count);
  }

  void Draw(const rex::ui::ImmediateDraw& draw) override {
    if (!frame_ || draw.primitive_type != rex::ui::ImmediatePrimitiveType::kTriangles || draw.count <= 0) return;
    gh2render::OverlayDraw d;
    d.texture = draw.texture ? static_cast<OverlayTexture*>(draw.texture)->id() : 0;
    d.index_offset = index_base_ + static_cast<uint32_t>(draw.index_offset);
    d.index_count = static_cast<uint32_t>(draw.count);
    d.base_vertex = vertex_base_ + draw.base_vertex;
    if (draw.scissor) {
      if (draw.scissor_right <= draw.scissor_left || draw.scissor_bottom <= draw.scissor_top) return;
      d.clip[0] = draw.scissor_left;
      d.clip[1] = draw.scissor_top;
      d.clip[2] = draw.scissor_right;
      d.clip[3] = draw.scissor_bottom;
    } else {
      d.clip[2] = frame_->width;
      d.clip[3] = frame_->height;
    }
    frame_->draws.push_back(d);
  }

  void EndDrawBatch() override {}

  void End() override {
    rex::ui::ImmediateDrawer::End();
    if (auto* r = gh2rt::gpu::ActiveRenderer()) r->OverlaySubmit(std::move(frame_));
    frame_.reset();
    produced_ = true;
    shown_ = true;
  }

  // One menu frame: the dialogs draw (or not), and the renderer ends up with the matching picture (nothing when no dialog is open).
  void Tick(rex::ui::ImGuiDrawer* drawer, uint32_t width, uint32_t height) {
    if (!options_applied_) {   // the renderer exists now: hand it the options of the cvars (the config file's msaa and anisotropy)
      options_applied_ = true;
      Gh2ApplyRenderOptions();
    }
    if (const int64_t due = g_options_due_ns.load(); due && NowNs() >= due) {
      g_options_due_ns.store(0);
      Gh2ApplyRenderOptions();
    }
    produced_ = false;
    rex::ui::AppUIDrawContext context(width, height);
    drawer->Draw(context);
    if (!produced_ && shown_) {
      if (auto* r = gh2rt::gpu::ActiveRenderer()) r->OverlaySubmit(nullptr);
      shown_ = false;
    }
  }

 private:
  std::shared_ptr<gh2render::OverlayFrame> frame_;
  int32_t vertex_base_ = 0;
  uint32_t index_base_ = 0;
  bool produced_ = false, shown_ = false, options_applied_ = false;
};

OverlayDrawer* g_overlay = nullptr;

}  // namespace

std::unique_ptr<rex::ui::ImmediateDrawer> Gh2CreateNativeOverlayDrawer() {
  auto drawer = std::make_unique<OverlayDrawer>();
  g_overlay = drawer.get();
  return drawer;
}

void Gh2NativeOverlayStart(rex::ui::ImGuiDrawer* drawer, rex::ui::WindowedAppContext* context) {
  g_drawer = drawer;
  g_context = context;
  gh2rt::gpu::SetPresentCallback([](uint32_t width, uint32_t height) {   // the command processor's thread: only hand the frame to the UI thread (one in flight)
    if (!g_overlay || !g_drawer || !g_context || g_tick_pending.exchange(true)) return;
    g_context->CallInUIThread([width, height] {
      if (g_overlay && g_drawer) g_overlay->Tick(g_drawer, width, height);
      g_tick_pending.store(false);
    });
  });
}

void Gh2ApplyRenderOptions() {
  auto* renderer = gh2rt::gpu::ActiveRenderer();
  if (!renderer) return;
  gh2render::RenderOptions o;
  o.height = Gh2RenderHeight();
  const std::string msaa = rex::cvar::GetFlagByName("msaa");
  o.msaa = msaa == "on" || msaa == "true" || ((msaa == "auto" || msaa.empty()) && o.height < 2160);   // auto: not at 4K
  if (const char* e = std::getenv("GH2_MSAA"); e && (std::string(e) == "off" || std::string(e) == "0")) o.msaa = false;   // the testing switch wins over the menu
  const int af = std::atoi(rex::cvar::GetFlagByName("render_anisotropy").c_str());   // 0: as the game sets it; 2, 4, 8, 16
  o.anisotropy = af >= 2 ? static_cast<uint32_t>(std::min(af, 16)) : 0u;
  {
    const std::string pm = rex::cvar::GetFlagByName("present_mode");
    o.present_mode = pm == "fifo" ? 0u : pm == "immediate" ? 2u : 1u;
  }
  const gh2render::RenderOptions now = renderer->GetOptions();
  renderer->SetOptions(o);
  {   // for the performance overlay (MangoHud: exec=cat $XDG_RUNTIME_DIR/gh2_render.txt): what the game really renders at, not the size of the window
    const char* dir = std::getenv("XDG_RUNTIME_DIR");
    const std::string path = std::string(dir && *dir ? dir : "/tmp") + "/gh2_render.txt";
    if (FILE* f = std::fopen(path.c_str(), "w")) {
      std::fprintf(f, "%ux%u MSAA %s\n", (o.height * 16 + 4) / 9, o.height, o.msaa ? "on" : "off");
      std::fclose(f);
    }
  }
  if (now.height != o.height || now.msaa != o.msaa || now.anisotropy != o.anisotropy || now.present_mode != o.present_mode)
    std::fprintf(stderr, "GFX render options: %up, msaa %s, anisotropy %ux (0 = as the game sets), present %s\n", o.height, o.msaa ? "on" : "off", o.anisotropy, o.present_mode == 0 ? "fifo" : o.present_mode == 2 ? "immediate" : "mailbox");
}

void Gh2RenderOptionsWindowChanged() {
  std::string v = rex::cvar::GetFlagByName("render_resolution");
  for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (v == "auto") g_options_due_ns.store(NowNs() + 600'000'000);   // a drag of the window edge changes the size at every step; the targets are rebuilt once it settles
}

// F9: the next picture as the game draws it (internal resolution, no menu, no MangoHud) as a PNG in ~/gh2_shots, for reporting a graphics problem exactly as it is.
void Gh2NativeScreenshot() {
  auto* renderer = gh2rt::gpu::ActiveRenderer();
  if (!renderer) return;
  const gh2render::RenderOptions o = renderer->GetOptions();   // in the file name: what the picture was drawn with (so a pair of pictures can be told apart)
  const std::string tag = std::string("_msaa-") + (o.msaa ? "on" : "off") + "_vsync-" + (o.present_mode == 0 ? "fifo" : o.present_mode == 2 ? "off" : "mailbox");
  renderer->RequestScreenshot([tag](uint32_t width, uint32_t height, std::vector<uint8_t> rgb) {
    std::fprintf(stderr, "SCREENSHOT captured at %.3f s (steady clock)\n", std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
    std::thread([width, height, tag, rgb = std::move(rgb)] {   // the renderer's thread must not wait for the disk
      const char* home = std::getenv("HOME");
      const std::filesystem::path dir = std::filesystem::path(home && *home ? home : "/tmp") / "gh2_shots";
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      const std::time_t now = std::time(nullptr);
      char stamp[32];
      std::strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", std::localtime(&now));
      const std::filesystem::path path = dir / (std::string("gh2_") + stamp + "_" + std::to_string(width) + "x" + std::to_string(height) + ".png");
      static std::atomic<int> serial{0};
      const std::filesystem::path unique = dir / (std::string("gh2_") + stamp + "_" + std::to_string(width) + "x" + std::to_string(height) + tag + "_" + std::to_string(serial++) + ".png");
      const bool ok = Gh2WritePngRgb(unique.string(), width, height, rgb.data());
      std::fprintf(stderr, "SCREENSHOT %s %s\n", ok ? "saved" : "FAILED", unique.c_str());
    }).detach();
  });
}

void Gh2NativeOverlayStop() {
  gh2rt::gpu::SetPresentCallback(nullptr);
  g_drawer = nullptr;
  g_context = nullptr;
}
