// gh2 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/cvar.h>
#include <rex/rex_app.h>
#include <rex/system/gpu_plugin.h>

#include "native_overlay.h"
#include "shader_bootstrap.h"
#include "native_window.h"
#include <gh2rt/hooks_api.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

// Player-facing graphics menu (src/graphics_menu.cpp).
void Gh2InstallRequestedContent(rex::Runtime* runtime);
// Sets the audio buffer sizes from the `audio_latency` option (call before the audio system starts).
void Gh2ApplyAudioLatency();
void Gh2InstallControllerAssignment(rex::Runtime* runtime);
void Gh2StartLogoSkipWatcher(rex::Runtime* runtime);
bool Gh2NativeInputSelected();
void Gh2InstallNativeInput(rex::RuntimeConfig& config);
bool Gh2NativeArenaSelected();
bool Gh2InstallNativeArena();
bool Gh2NativeAudioSelected();
bool Gh2NativeGpuSelected();
uint32_t Gh2RenderHeight();      // the `render_resolution` cvar: internal height (0 = auto: the window's)   // --gpu=native: the runtime gets no graphics system (detached mode)
void Gh2InstallNativeAudio(rex::RuntimeConfig& config);
void Gh2InstallD3DObserver();  // GH2_D3D_OBSERVE=<shader cache dir>: observe the game's D3D shader binds (G.0)
void Gh2SetControllerNoticeTarget(rex::ui::ImGuiDrawer* drawer, rex::ui::WindowedAppContext* context);
void Gh2InstallGraphicsMenu(rex::Runtime* runtime, rex::ui::Presenter* presenter, rex::ui::Window* window, rex::ui::WindowedAppContext& context,
                            rex::ui::ImGuiDrawer* drawer, const std::filesystem::path& config_path);

class Gh2App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Gh2App>(new Gh2App(ctx, "gh2",
        PPCImageConfig));
  }

  // Load the GPU plugin here (as ReXApp would right after) to keep a pointer to its presenter for the graphics menu.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (Gh2NativeGpuSelected()) config.gpu_plugin.clear();   // detached mode: our own command processor and renderer; the SDK loads no graphics plugin
    if (!config.graphics && !config.gpu_plugin.empty()) config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin);
    graphics_ = config.graphics.get();
    // The game's logic advances once per rendered frame (it ran faster in light scenes and slower in heavy ones with
    // vsync off), so an unlocked frame rate is not supported: ignore a stale `vsync = false` from a config file.
    // A command line `--vsync=...` is still honoured for experiments.
    std::ifstream cmd("/proc/self/cmdline", std::ios::binary);
    const std::string args((std::istreambuf_iterator<char>(cmd)), std::istreambuf_iterator<char>());
    if (args.find("--vsync") == std::string::npos) rex::cvar::SetFlagByName("vsync", "true");
    if (Gh2NativeGpuSelected()) {   // windowed mode opens at the internal resolution (an explicit --window_width/--window_height wins; fullscreen is the monitor's and the picture is fitted to it)
      const uint32_t h = Gh2RenderHeight();
      const bool fullscreen = rex::cvar::GetFlagByName("fullscreen") == "true";
      if (h && !fullscreen && std::atoi(rex::cvar::GetFlagByName("window_width").c_str()) == 0 && std::atoi(rex::cvar::GetFlagByName("window_height").c_str()) == 0) {
        rex::cvar::SetFlagByName("window_width", std::to_string((h * 16 + 4) / 9));
        rex::cvar::SetFlagByName("window_height", std::to_string(h));
      }
    }
    Gh2ApplyAudioLatency();
    if (Gh2NativeArenaSelected()) Gh2InstallNativeArena();
    if (Gh2NativeInputSelected()) Gh2InstallNativeInput(config);
    if (Gh2NativeAudioSelected()) Gh2InstallNativeAudio(config);
  }

  // Just before the guest starts: install packages given with --install_content so the game sees them at boot.
  void OnPreLaunchModule() override {
    if (Gh2NativeGpuSelected()) app_context().CallInUIThreadSynchronous([this] { Gh2NativeWindowOpened(window()); });   // the native renderer presents to this window
    Gh2InstallD3DObserver();
    if (Gh2NativeGpuSelected()) Gh2ShaderBuildStart(runtime()->virtual_membase());   // the executable is in memory now: the first-start shader build (if planned) can read the intro video's shaders from it
    Gh2InstallControllerAssignment(runtime());
    Gh2StartLogoSkipWatcher(runtime());
    Gh2InstallRequestedContent(runtime());
  }

  void OnConfigurePaths(rex::PathConfig& paths) override { config_path_ = paths.config_path; }
  void OnWindowPixelSizeChanged(uint32_t width, uint32_t height) override { Gh2NativeWindowResized(width, height); }

  // --gpu=native has no presenter: the menu's draw lists go to our drawer, which hands them to the renderer.
  std::unique_ptr<rex::ui::ImmediateDrawer> OnCreateImmediateDrawer() override {
    if (Gh2NativeGpuSelected()) return Gh2CreateNativeOverlayDrawer();
    return nullptr;
  }

  // The game never uses the mouse: hide the cursor (it reappears briefly on mouse motion, e.g. for the F4 overlay).
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    Gh2SetControllerNoticeTarget(drawer, &app_context());
    Gh2InstallGraphicsMenu(runtime(), graphics_ ? graphics_->presenter() : nullptr, window(), app_context(), drawer, config_path_);
    if (Gh2NativeGpuSelected()) {
      Gh2NativeOverlayStart(drawer, &app_context());
      Gh2ShaderBuildShowDialog(drawer);   // only when the first-start shader build is running
    }
    if (auto* w = window()) {
      w->SetCursorAutoHideDelayMs(500);
      w->SetCursorVisibility(rex::ui::Window::CursorVisibility::kAutoHidden);
    }
    if (const char* quit = std::getenv("GH2_TEST_QUIT")) {   // testing aid: leave through the window-close path (OnClosing: TerminateTitle + hard exit) after N seconds
      const int delay = std::max(1, std::atoi(quit));
      std::thread([this, delay] {
        std::this_thread::sleep_for(std::chrono::seconds(delay));
        std::fprintf(stderr, "QUIT test: asking the application to quit\n");
        app_context().CallInUIThread([this]() {   // what closing the window does: terminate the title, then hard-exit
          rex::ui::UIEvent closing(window());
          static_cast<rex::ui::WindowListener&>(*this).OnClosing(closing);
        });
      }).detach();
    }
    if (const char* toast = std::getenv("GH2_TEST_TOAST")) {  // testing aid: an achievement toast after N seconds (default 20)
      const int delay = std::max(1, std::atoi(toast) > 1 ? std::atoi(toast) : 20);
      std::thread([this, delay] {
        std::this_thread::sleep_for(std::chrono::seconds(delay));
        const auto list = achievements().ListAchievements();
        std::fprintf(stderr, "TOAST test: %zu achievements known\n", list.size());
        const char* want = std::getenv("GH2_TEST_TOAST_NAME");  // optional: the achievement whose label contains this text
        size_t pick = 0;
        for (size_t i = 0; i < list.size(); ++i) {
          std::fprintf(stderr, "TOAST test: #%zu id=%u image=%u icon='%s' label='%s'\n", i, list[i].id, list[i].image_id,
                       list[i].icon_path.c_str(), list[i].label.c_str());
          if (want && pick == 0 && list[i].label.find(want) != std::string::npos) pick = i;
        }
        const int every = std::getenv("GH2_TEST_TOAST_EVERY") ? std::atoi(std::getenv("GH2_TEST_TOAST_EVERY")) : 0;
        for (int n = 0; n < (every > 0 ? 12 : 1); ++n) {  // optionally repeat every N seconds (12 times)
          if (n) std::this_thread::sleep_for(std::chrono::seconds(every));
          if (!list.empty()) achievements().ShowAchievementNotification(list[pick].id);
          std::fprintf(stderr, "TOAST test: shown #%d\n", n + 1);
        }
      }).detach();
    }
  }

  // The game's native threads (--threads=native) run guest code on host threads the runtime does not know: stop them before the process tears the runtime down.
  void OnShutdown() override {
    Gh2NativeOverlayStop();
    gh2rt::hooks::QuiesceNativeThreads();
  }

  std::filesystem::path config_path_;
  rex::system::IGraphicsSystem* graphics_ = nullptr;

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnPostSetup() override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // void OnShutdown() override {}
};
