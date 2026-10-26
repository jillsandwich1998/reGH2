#include "native_input.h"

#include <SDL3/SDL.h>
#include <rex/cvar.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/runtime.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>

#include "input_core.h"

REXCVAR_DEFINE_STRING(input_reader, "native", "GH2",
                      "Who reads the controllers: native (a dedicated SDL3 thread, ~1 kHz, lock-free snapshots; the default) or sdk (the SDK's SDL "
                      "driver, refreshed by the UI thread on every guest poll; kept as a fallback)")
    .allowed({"native", "sdk"});

namespace {

using rex::X_RESULT;  // the X_ERROR_* / X_STATUS_* macros expand to casts to these
using rex::X_STATUS;
using rex::input::DeviceId;

constexpr uint64_t kIdTag = 1ull << 62;  // keeps ids apart from the other drivers' (InputSystem compares ids across drivers)
DeviceId IdOf(uint64_t serial) { return static_cast<DeviceId>(kIdTag | serial); }
uint64_t SerialOf(DeviceId id) {
  const uint64_t v = static_cast<uint64_t>(id);
  return (v & kIdTag) ? (v & ~kIdTag) : 0;
}

class NativeDriver final : public rex::input::InputDriver {
 public:
  NativeDriver() : InputDriver(nullptr, 0) {}
  ~NativeDriver() override {
    core_.Stop();
    if (sdl_up_) gh2plat::InputCore::QuitSdl();
  }

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  // Same place and thread the SDK's driver starts SDL's gamepad subsystem on.
  void OnWindowAvailable(rex::ui::Window* window) override {
    if (started_.exchange(true)) return;
    auto start = [this] {
      gh2plat::InputCore::Options options;
      options.guide_button = rex::cvar::GetFlagByName("guide_button") == "true";
      options.log = [](const char* line) { fprintf(stderr, "INPUT %s\n", line); };
      if (!gh2plat::InputCore::InitSdl(options)) {
        fprintf(stderr, "INPUT native reader: SDL gamepad subsystem failed to start: %s\n", SDL_GetError());
        return;
      }
      sdl_up_ = true;
      guide_ = options.guide_button;
      if (!core_.Start(options)) fprintf(stderr, "INPUT native reader: the polling thread did not start\n");
      else fprintf(stderr, "INPUT native reader: dedicated SDL3 thread running (%d Hz), guide button %s\n", options.poll_hz, guide_ ? "on" : "off");
    };
    if (window) window->app_context().CallInUIThreadSynchronous(start);
    else start();
  }

  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override {
    if (!core_.running()) return;
    for (const auto& d : core_.Devices()) {
      rex::input::DeviceInfo info;
      info.id = IdOf(d.serial);
      info.name = d.name;
      info.guid = d.guid;
      info.subtype = d.cls.xinput_subtype;
      info.synthetic = false;
      out.push_back(std::move(info));
    }
  }

  X_RESULT GetDeviceState(DeviceId id, rex::input::X_INPUT_STATE* out) override {
    const uint64_t serial = SerialOf(id);
    gh2plat::Sample s;
    if (!serial || !core_.running() || !core_.Read(serial, &s)) return X_ERROR_DEVICE_NOT_CONNECTED;
    // The console has no notion of window focus: a guitar must keep playing when a notification or another window takes the focus in the middle of a song (the SDK driver neutralised the pad
    // then, and the X-plorer "stopped working" mid-song). GH2_INPUT_REQUIRE_FOCUS=1 brings the old behaviour back.
    static const bool require_focus = std::getenv("GH2_INPUT_REQUIRE_FOCUS") != nullptr && std::getenv("GH2_INPUT_IGNORE_FOCUS") == nullptr;
    const bool window_active = is_active();
    {
      static std::atomic<int> last_window_state{-1};   // a line each time the window's focus changes, whatever the policy: it is what a mid-song silence of the pad used to be
      const int now = window_active ? 1 : 0;
      if (last_window_state.exchange(now) != now && now != -1) fprintf(stderr, "INPUT window focus %s (the pad is %s)\n", window_active ? "gained" : "lost", require_focus && !window_active ? "ignored until it is back" : "still read");
    }
    const bool active = !require_focus || window_active;
    uint32_t packet;
    {
      // Packet number as the SDK driver keeps it: +1 per poll that finds a change (or a focus change); changes seen while the
      // window is inactive stay pending until it is active again.
      std::lock_guard<std::mutex> lock(mu_);
      auto& p = per_device_[serial];
      if (active != p.active || (active && s.changes != p.changes)) {
        ++p.packet;
        p.active = active;
        p.changes = s.changes;
      }
      packet = p.packet;
    }
    out->packet_number = packet;
    const gh2plat::PadState& pad = s.pad;
    out->gamepad.buttons = active ? pad.buttons : 0;
    out->gamepad.left_trigger = active ? pad.left_trigger : 0;
    out->gamepad.right_trigger = active ? pad.right_trigger : 0;
    out->gamepad.thumb_lx = active ? pad.lx : 0;
    out->gamepad.thumb_ly = active ? pad.ly : 0;
    out->gamepad.thumb_rx = active ? pad.rx : 0;
    out->gamepad.thumb_ry = active ? pad.ry : 0;
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t /*flags*/, rex::input::X_INPUT_CAPABILITIES* out) override {
    if (!out) return X_ERROR_BAD_ARGUMENTS;
    const uint64_t serial = SerialOf(id);
    gh2plat::DeviceInfo info;
    bool found = false;
    if (serial && core_.running()) {
      for (auto& d : core_.Devices())
        if (d.serial == serial) {
          info = std::move(d);
          found = true;
          break;
        }
    }
    if (!found) return X_ERROR_DEVICE_NOT_CONNECTED;
    uint16_t flags = 0;
    if (info.wireless) flags |= rex::input::X_INPUT_CAPS_WIRELESS;
    if (!info.has_navigation) flags |= rex::input::X_INPUT_CAPS_NO_NAVIGATION;
    out->type = rex::input::XINPUT_DEVTYPE_GAMEPAD;
    out->sub_type = info.cls.xinput_subtype;
    out->flags = flags;
    out->gamepad.buttons = static_cast<uint16_t>(0xF3FF | (guide_ ? rex::input::X_INPUT_GAMEPAD_GUIDE : 0));
    out->gamepad.left_trigger = 0xFF;
    out->gamepad.right_trigger = 0xFF;
    out->gamepad.thumb_lx = static_cast<int16_t>(0xFFFFu);
    out->gamepad.thumb_ly = static_cast<int16_t>(0xFFFFu);
    out->gamepad.thumb_rx = static_cast<int16_t>(0xFFFFu);
    out->gamepad.thumb_ry = static_cast<int16_t>(0xFFFFu);
    out->vibration.left_motor_speed = 0xFFFFu;
    out->vibration.right_motor_speed = 0xFFFFu;
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetDeviceVibration(DeviceId id, rex::input::X_INPUT_VIBRATION* vibration) override {
    const uint64_t serial = SerialOf(id);
    if (!serial || !core_.running()) return X_ERROR_DEVICE_NOT_CONNECTED;
    core_.Rumble(serial, vibration->left_motor_speed, vibration->right_motor_speed);
    return X_ERROR_SUCCESS;
  }

  // No guest title of this project reads keystrokes (the game imports only XamInputGetState / GetCapabilities).
  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t /*flags*/, rex::input::X_INPUT_KEYSTROKE* /*out*/) override {
    gh2plat::Sample s;
    return (SerialOf(id) && core_.running() && core_.Read(SerialOf(id), &s)) ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }

 private:
  struct PerDevice {
    uint32_t packet = 0;
    uint32_t changes = 0;
    bool active = false;
  };
  gh2plat::InputCore core_;
  std::atomic<bool> started_{false};
  bool sdl_up_ = false;
  bool guide_ = false;
  std::mutex mu_;
  std::unordered_map<uint64_t, PerDevice> per_device_;
};

}  // namespace

bool Gh2NativeInputSelected() { return REXCVAR_GET(input_reader) == "native"; }

void Gh2InstallNativeInput(rex::RuntimeConfig& config) {
  auto base = config.input_factory;
  config.input_factory = [base](bool tool_mode) {
    // CreateDefaultInputSystem adds the SDK's SDL driver for input_backend == "sdl" and nothing for "xinput" (Windows-only code).
    // The value is restored right after, so nothing about it can reach the config file.
    const std::string previous = rex::cvar::GetFlagByName("input_backend");
    rex::cvar::SetFlagByName("input_backend", "xinput");
    auto system = base(tool_mode);
    rex::cvar::SetFlagByName("input_backend", previous);
    if (!tool_mode)
      if (auto* input = static_cast<rex::input::InputSystem*>(system.get())) input->AddDriver(std::make_unique<NativeDriver>());
    return system;
  };
}
