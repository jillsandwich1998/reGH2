// gh2rt/host_services.h - what the hooks need from the host application: user settings, which device plays which guest player,
// the keyboard player, and a few diagnostics sinks. The application implements it (app/src/gh2_host_services.cpp, backed by the
// ReXGlue SDK today) and registers it before the guest starts; the hooks never see the SDK.
// This header must stay free of SDK headers and of the `rex::` namespace (native/tools/check_isolation.sh).
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace gh2rt {

// What the keyboard contributes to one guest player's pad state.
struct KeyboardState {
  bool active = false;     // the keyboard feeds this guest user
  uint16_t buttons = 0;    // XINPUT_GAMEPAD button bits to OR in
  bool whammy = false;     // whammy key held
  uint32_t changes = 0;    // counts state changes (packet number bump)
};

// What the guest's XamInputGetState / XamInputGetCapabilities report, in host byte order (the hooks write the guest's big-endian structures).
struct PadState {
  uint32_t packet = 0;
  uint16_t buttons = 0;
  uint8_t left_trigger = 0, right_trigger = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};
struct PadCaps {
  uint8_t type = 0, sub_type = 0;
  uint16_t flags = 0;
  PadState gamepad;                     // the masks of the controls the device has (packet unused)
  uint16_t left_motor = 0, right_motor = 0;
};
constexpr uint32_t kPadNotConnected = 0x48F;   // ERROR_DEVICE_NOT_CONNECTED

// The application window the native renderer presents to: the platform surface handles (Wayland wl_display/wl_surface, or Xlib Display/Window) and the live size in pixels.
struct NativeWindow {
  enum class Kind : uint32_t { kNone, kWayland, kXlib } kind = Kind::kNone;
  void* display = nullptr;
  uint64_t surface = 0;
  const std::atomic<uint32_t>* width = nullptr;    // updated by the application when the window is resized
  const std::atomic<uint32_t>* height = nullptr;
};

// User options the hooks consult (cvars and the config file in the application).
struct Settings {
  bool skip_intro = false;
  bool autostart = false;                 // testing aid
  int autostart_seconds = 30;
  std::string startup_mode = "normal";    // "normal" | "menu"
  std::string game_data_root;             // the folder with default.xex, gen/, videos/ (mounted as d: and game:)
  std::string sync = "native";              // who answers the IRQL / spin lock / critical section / TLS calls: "sdk" (the runtime) or "native" (gh2k::sync)
  std::string objects = "native";            // who answers events, semaphores and their waits: "sdk" (the runtime) or "native" (gh2k::sync objects)
  uint32_t render_height = 720;               // internal resolution of the native renderer (height; 16:9): 720 = the console's image; 0 = the height of the window
  std::string shader_cache_dir;               // the directory with shaders.pak (the translated shaders), for --gpu=native
  std::string gpu = "sdk";                    // who executes the GPU command stream and draws: "sdk" (the runtime's graphics system) or "native" (gh2gpu + the Vulkan renderer; the runtime has no graphics system then)
  std::string main_thread = "native";        // where the game's entry point (xstart, its main thread) runs: "native" (a gh2k thread; the runtime's main thread only waits) or "sdk" (on the runtime's main thread)
  std::string threads = "native";            // who creates and schedules the game's threads (ExCreateThread): "native" (gh2k) or "sdk" (the runtime)
  std::string xam_input = "native";          // who implements the XamInputGetState / GetCapabilities imports: "native" (the hooks; the pad itself comes from HostServices::Pad*) or "sdk"
  std::string audio_pump = "native";         // who runs the game's audio render-driver callback: "native" (a native thread feeding the SDL3 sink) or "sdk" (the runtime's AudioSystem)
  bool audio_adaptive = false;               // start the audio queue at audio_queue_frames and raise it (up to 8) when the output starves: the shortest delay the machine can hold
  uint32_t audio_queue_frames = 8;           // 256-sample frames the callback may run ahead of the output (the audio_latency option)
  std::string vfs = "native";                // who serves the game's files: "native" (gh2k::Vfs, default), "shadow" (the runtime, checked against gh2k::Vfs) or "sdk"
};

class HostServices {
 public:
  virtual ~HostServices() = default;
  virtual Settings GetSettings() = 0;                      // read once, at first use
  virtual void SetGuestTimeScalar(double scalar) = 0;      // guest clock speed (intro fast-forward); 1.0 = real time
  virtual KeyboardState PollKeyboard(uint32_t user) = 0;   // called on the guest's input thread
  virtual int KeyboardSlot() = 0;                          // guest user the keyboard plays as, or -1
  virtual void KeyboardNoteInSong() = 0;                   // a song's highway camera is being updated
  virtual bool UserIsGuitar(uint32_t user) = 0;
  virtual bool UserHasRealDevice(uint32_t user) = 0;
  virtual void NoteGuestFrame(double interval_ms) = 0;     // feeds the FPS meter, once per guest frame
  virtual void DumpCvars() = 0;                            // GH2_DUMP_CVARS=1
  // The pad source: the state/capabilities of the device that plays guest user `user` (0..3), including the host's policy (assignment, dialogs owning the controller).
  // Return 0 on success or a Win32 error (kPadNotConnected). `out` may be null (a query). Called on whichever thread polls.
  virtual bool PadSourceAvailable() { return false; }
  virtual uint32_t PadGetState(uint32_t /*user*/, PadState* /*out*/) { return kPadNotConnected; }
  virtual uint32_t PadGetCapabilities(uint32_t /*user*/, uint32_t /*flags*/, PadCaps* /*out*/) { return kPadNotConnected; }
  virtual bool GetNativeWindow(NativeWindow* /*out*/) { return false; }   // the window to present to; callable from any thread (the application hops to its UI thread)
  virtual const std::atomic<bool>* AudioMuteFlag() { return nullptr; }   // set while the user has audio muted (the option can change at any time)
};

// Registers the implementation (call before the guest starts). Until then Host() answers with inert defaults, so the hooks work
// in a bare process (tests).
void SetHostServices(HostServices* services);
HostServices& Host();

}  // namespace gh2rt
