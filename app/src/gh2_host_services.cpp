// Host services for the hooks in native/rt/hooks (gh2rt::HostServices), backed by the ReXGlue SDK: the game options (cvars), the
// guest clock, which device plays which player, the keyboard and the FPS meter. Also the one piece of the old gh2_host.cpp that
// needs the SDK's input system: the logo-skip watcher.

#include <gh2rt/host_services.h>
#include <gh2rt/hooks_api.h>

#include "native_audio.h"

bool Gh2AudioQueueIsAuto();   // graphics_menu.cpp
#include "shader_bootstrap.h"
#include "native_window.h"

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/runtime.h>

#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>

#include "controllers.h"
#include "fps_meter.h"
#include "keyboard.h"

// ---- Game options (cvars): command line `--skip_intro`, env `REX_SKIP_INTRO=1`, or ~/.config/gh2/config.toml ----
REXCVAR_DEFINE_BOOL(skip_intro, false, "GH2",
                    "Skip the splash screens and the intro video; the game stops at the title screen");
REXCVAR_DEFINE_STRING(startup_mode, "normal", "GH2",
                      "Start-up: normal (logos and intro video; any button skips the logos) or menu (skip straight to the "
                      "main menu)")
    .allowed({"normal", "menu"});
REXCVAR_DEFINE_BOOL(autostart, false, "GH2",
                    "Testing aid: after the intro keep pressing A so the menus start a Quick Play song");
REXCVAR_DEFINE_STRING(vfs, "native", "GH2",
                      "Who serves the game's files (d:\\gen\\main_0.ark, videos): native (the native VFS; saves and content still through the runtime; default), "
                      "shadow (the runtime, every read checked byte by byte against the native VFS) or sdk (the runtime)")
    .allowed({"sdk", "shadow", "native"});
REXCVAR_DEFINE_STRING(objects, "native", "GH2",
                      "Who answers events, semaphores and their waits (KeSetEvent, KeWaitForSingleObject, NtCreateEvent ...): native (gh2k::sync; default) or sdk (the runtime)")
    .allowed({"sdk", "native"});
REXCVAR_DEFINE_STRING(threads, "native", "GH2",
                      "Who creates and schedules the game's own threads (ExCreateThread, suspend/resume, delays): native (gh2k; default) or sdk (the runtime)")
    .allowed({"sdk", "native"});
REXCVAR_DEFINE_STRING(shader_cache, "", "GH2",
                      "Directory with shaders.pak (the translated shaders) for gpu=native; GH2_SHADER_CACHE overrides it; empty: <data home>/gh2/shaders");
REXCVAR_DEFINE_STRING(render_resolution, "720p", "GH2",
                      "Internal resolution of the native renderer, and the size of the window in windowed mode: 720p (the console's image), 900p, 1080p, 1440p, 2160p, any <height>p (16:9), or auto "
                      "(the window's own height, e.g. the monitor's in borderless fullscreen)");
REXCVAR_DEFINE_STRING(msaa, "auto", "GH2",
                      "The multisampling (2x/4x MSAA) the game asks for, with the native renderer: auto (on, but off from 2160p up, where edges are already fine and it costs the frame-time peaks), on, or off")
    .allowed({"auto", "on", "off"});
REXCVAR_DEFINE_INT32(render_anisotropy, 0, "GH2", "Anisotropic filtering of the native renderer: 0 = as the game sets each texture, 2/4/8/16 = at least that for every filtered texture")
    .range(0, 16);
REXCVAR_DEFINE_STRING(present_mode, "mailbox", "GH2",
                      "How the native renderer's pictures reach the screen (the game itself always runs at 60 fps): fifo (vertical sync on: no tearing, the picture waits for the refresh), mailbox (no tearing, the newest picture wins: "
                      "lower latency; the default) or immediate (vertical sync off: tearing, lowest latency)")
    .allowed({"fifo", "mailbox", "immediate"});
REXCVAR_DEFINE_STRING(gpu, "native", "GH2",
                      "Who runs the GPU: native (our command processor and Vulkan renderer, the runtime has no graphics system; default; needs the shader cache) or sdk (the runtime's graphics system; "
                      "also what native falls back to when there is no shader cache)")
    .allowed({"sdk", "native"});
REXCVAR_DEFINE_STRING(main_thread, "native", "GH2",
                      "Where the game's main thread runs: native (a thread of ours; the runtime's main thread only waits for it; default; needs threads=native) or sdk (the runtime's own main thread)")
    .allowed({"sdk", "native"});
REXCVAR_DEFINE_STRING(xam_input, "native", "GH2",
                      "Who implements the XamInputGetState / XamInputGetCapabilities imports: native (the hooks; the pad itself comes from the application; default) or sdk (the runtime's own entries)")
    .allowed({"sdk", "native"});
REXCVAR_DEFINE_STRING(audio_pump, "native", "GH2",
                      "Who runs the game's audio render-driver callback: native (a native thread of ours feeding the SDL3 sink; default; needs audio_output=native) or sdk (the runtime's AudioSystem)")
    .allowed({"sdk", "native"});
REXCVAR_DEFINE_STRING(sync, "native", "GH2",
                      "Who answers the kernel's IRQL, spin lock, critical section and TLS slot calls: native (gh2k::sync; default) or sdk (the runtime)")
    .allowed({"sdk", "native"});
REXCVAR_DEFINE_INT32(autostart_seconds, 30, "GH2", "How long the autostart testing aid keeps pressing A");

// SDL installs SIGINT/SIGTERM handlers that only post an SDL_QUIT event, which this windowed app never reads, so `kill` and Ctrl+C
// did nothing. Ask SDL not to install them (must be set before SDL initialises).
__attribute__((constructor)) static void Gh2DisableSdlSignalHandlers() { setenv("SDL_NO_SIGNAL_HANDLERS", "1", 0); }

// Where shaders.pak is looked for: GH2_SHADER_CACHE, the shader_cache option, then <data home>/gh2/shaders (what the installer writes).
std::string Gh2ShaderCacheDir() {
  if (const char* e = std::getenv("GH2_SHADER_CACHE"); e && *e) return e;
  std::string dir = REXCVAR_GET(shader_cache);
  if (!dir.empty()) return dir;
  const char* xdg = std::getenv("XDG_DATA_HOME");
  const char* home = std::getenv("HOME");
  if (xdg && *xdg) return std::string(xdg) + "/gh2/shaders";
  if (home && *home) return std::string(home) + "/.local/share/gh2/shaders";
  return {};
}

// The native GPU needs the translated shaders: without them the window would stay black, so the SDK's graphics system is used instead (and says why).
bool Gh2NativeGpuSelected() {
  static const bool selected = [] {
    std::string choice = REXCVAR_GET(gpu);
    if (const char* e = std::getenv("GH2_GPU"); e && *e) choice = e;
    if (choice != "native") return false;
    const std::string dir = Gh2ShaderCacheDir();
    if (Gh2ShaderCacheCurrent(dir)) return true;
    if (Gh2ShaderBuildBegin(dir)) return true;   // missing or old: made from the game in the background; the renderer opens the cache when it is ready (a dialog shows the progress)
    std::fprintf(stderr, "GPU: native selected but there is no current shader cache (shaders.pak) in '%s': using the SDK's renderer instead. It is made automatically at the start when the game's tools are available, "
                 "or with native/tools/shader_pipeline.py / GH2_SHADER_CACHE.\n", dir.c_str());
    return false;
  }();
  return selected;
}

// The `render_resolution` cvar as a height: "720p", "1080p", "1440", "4k", "auto" (0: the window's height). Anything unreadable is 720p.
uint32_t Gh2RenderHeight() {
  std::string v = REXCVAR_GET(render_resolution);
  for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (v == "auto") {   // fullscreen: the monitor's height (the window is not at its final size yet when the renderer starts); windowed: the window size asked for, else the console's 720p
    const int window_h = std::atoi(rex::cvar::GetFlagByName("window_height").c_str());
    if (rex::cvar::GetFlagByName("fullscreen") == "true") {
      if (const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay())) return static_cast<uint32_t>(std::min(2160, std::max(720, mode->h)));
    }
    if (const uint32_t live = Gh2NativeWindowPixelHeight()) return std::min(2160u, std::max(720u, live));   // the window as it is now
    return window_h > 0 ? static_cast<uint32_t>(std::min(2160, std::max(720, window_h))) : 720u;
  }
  if (v == "4k" || v == "uhd") return 2160;
  if (v == "2k" || v == "qhd") return 1440;
  if (v == "hd") return 720;
  if (v == "fhd") return 1080;
  const size_t x = v.find('x');   // "1920x1080" (the height counts)
  const uint32_t h = static_cast<uint32_t>(std::atoi(x == std::string::npos ? v.c_str() : v.c_str() + x + 1));
  return h >= 360 && h <= 4320 ? h : 720;
}

namespace {

class Gh2HostServices final : public gh2rt::HostServices {
 public:
  gh2rt::Settings GetSettings() override {
    gh2rt::Settings s;
    s.skip_intro = REXCVAR_GET(skip_intro);
    s.autostart = REXCVAR_GET(autostart);
    s.autostart_seconds = REXCVAR_GET(autostart_seconds);
    s.startup_mode = REXCVAR_GET(startup_mode);
    s.game_data_root = REXCVAR_GET(game_data_root);
    s.vfs = REXCVAR_GET(vfs);
    s.sync = REXCVAR_GET(sync);
    s.objects = REXCVAR_GET(objects);
    s.threads = REXCVAR_GET(threads);
    s.gpu = Gh2NativeGpuSelected() ? "native" : "sdk";
    s.shader_cache_dir = Gh2ShaderCacheDir();
    s.render_height = Gh2RenderHeight();
    s.main_thread = REXCVAR_GET(main_thread);
    s.xam_input = REXCVAR_GET(xam_input);
    s.audio_pump = Gh2NativeAudioSelected() ? REXCVAR_GET(audio_pump) : std::string("sdk");
    {
      const int q = std::atoi(rex::cvar::GetFlagByName("audio_maxqframes").c_str());
      s.audio_queue_frames = static_cast<uint32_t>(q < 2 ? 2 : q > 64 ? 64 : q);
      s.audio_adaptive = Gh2AudioQueueIsAuto() && !(std::getenv("GH2_AUDIO_ADAPTIVE") && std::getenv("GH2_AUDIO_ADAPTIVE")[0] == '0');
    }
    return s;
  }
  bool GetNativeWindow(gh2rt::NativeWindow* out) override { return Gh2GetNativeWindow(out); }
  bool PadSourceAvailable() override { return Gh2PadSourceAvailable(); }
  uint32_t PadGetState(uint32_t user, gh2rt::PadState* out) override { return Gh2PadGetState(user, out); }
  uint32_t PadGetCapabilities(uint32_t user, uint32_t flags, gh2rt::PadCaps* out) override { return Gh2PadGetCapabilities(user, flags, out); }
  const std::atomic<bool>* AudioMuteFlag() override { return Gh2AudioMuteFlag(); }
  void SetGuestTimeScalar(double scalar) override { rex::chrono::Clock::set_guest_time_scalar(scalar); }
  gh2rt::KeyboardState PollKeyboard(uint32_t user) override {
    const Gh2KbState kb = Gh2KeyboardPoll(user);
    return {kb.active, kb.buttons, kb.whammy, kb.changes};
  }
  int KeyboardSlot() override { return Gh2KeyboardSlot(); }
  void KeyboardNoteInSong() override { Gh2KeyboardNoteInSong(); }
  bool UserIsGuitar(uint32_t user) override { return Gh2UserIsGuitar(user); }
  bool UserHasRealDevice(uint32_t user) override { return Gh2UserHasRealDevice(user); }
  void NoteGuestFrame(double interval_ms) override { Gh2FpsNoteFrame(interval_ms); }
  void DumpCvars() override {
    for (const auto& f : rex::cvar::GetRegistry())
      fprintf(stderr, "CVAR [%s] %s = %s (default %s) -- %s\n", f.category.c_str(), f.name.c_str(), f.getter().c_str(),
              f.default_value.c_str(), f.description.c_str());
  }
};

Gh2HostServices g_services;
// Registered at load time, long before the guest starts polling.
[[maybe_unused]] const bool g_registered = (gh2rt::SetHostServices(&g_services), true);

}  // namespace

#ifdef GH2_HAVE_D3D_OBSERVER
#include <observer.h>
void Gh2InstallD3DObserver() { gh2gfx::d3d::InstallObserverFromEnvironment(); }
#else
void Gh2InstallD3DObserver() {}
#endif

// ---- Skipping the logos with a button (default start-up) -------------------------------------------------------------
// The game does not read the controller during the logos (only once the intro video starts), so a button pressed there went
// unnoticed. In the default start-up mode a host thread polls the controllers itself and, on the first button, runs the guest
// clock fast until the intro video opens (the same fast-forward the skip modes use). The video then plays at normal speed and the
// game's own "press a button to skip" works.
void Gh2StartLogoSkipWatcher(rex::Runtime* runtime) {
  auto* input = runtime ? static_cast<rex::input::InputSystem*>(runtime->input_system()) : nullptr;
  if (!input || gh2rt::hooks::SkipIntroEnabled()) return;  // the skip modes fast-forward on their own
  std::thread([input] {
    using namespace gh2rt::hooks;
    while (SinceStart() < 40.0 && !IntroVideoOpened() && !IntroDone()) {
      static const double fake_press = [] {
        const char* e = getenv("GH2_TEST_LOGO_PRESS");
        return e ? atof(e) : -1.0;
      }();
      if (fake_press >= 0 && SinceStart() >= fake_press) {  // testing aid: pretend a button was pressed
        FastForwardBegin(16.0, "simulated button press");
        return;
      }
      for (uint32_t user = 0; user < 4; ++user) {
        rex::input::X_INPUT_STATE st = {};
        const auto rc = input->GetStateForUI(user, &st);
        if (InputTrace()) {
          static bool shown[4] = {};
          if (!shown[user] && rc == 0) {
            shown[user] = true;
            fprintf(stderr, "LOGOWATCH polling user %u: connected, buttons=%04x\n", user, static_cast<unsigned>(st.gamepad.buttons));
          }
        }
        if (rc == 0 && st.gamepad.buttons != 0) {
          FastForwardBegin(16.0, "button pressed during the logos");
          return;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
  }).detach();
}
