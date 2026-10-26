// Controller identification and player assignment for the GH2 port.
//
// The SDK's input layer hands every connected device (name, GUID, XInput sub-type, connection order) to a
// DeviceAssignment, which decides which guest "user" (player 1..4) each device feeds. Gh2Assignment keeps the
// SDK's rule "connection order = player number" but, by default, puts guitars ahead of gamepads, so a guitar is
// always player 1 even when a gamepad was plugged in first. The menu shows who is who and lets the user swap
// players 1 and 2; connecting or removing a device pops a short notice on screen.
#pragma once

#include <gh2rt/host_services.h>
#include <rex/input/device_assignment.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/windowed_app_context.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace rex {
class Runtime;
}

struct Gh2ControllerRow {
  uint32_t player = 0;      // 1-based guest player number
  std::string name;         // device name as reported by the OS
  std::string kind;         // "Guitar", "Drum kit", "Gamepad", "Keyboard/mouse", ...
  uint8_t subtype = 0;      // XINPUT_DEVSUBTYPE_* as reported by the SDK
  bool guitar = false;
};

class Gh2Assignment final : public rex::input::DeviceAssignment {
 public:
  void OnDevicesChanged(const std::vector<rex::input::DeviceInfo>& devices) override;
  void DevicesForUser(uint32_t user_index, std::vector<rex::input::DeviceId>& out) const override;

  // Rows for players 1..4 that have a device (empty slots are omitted).
  std::vector<Gh2ControllerRow> Snapshot() const;
  // Re-applies the policy to the devices last seen (after the menu changed a setting).
  void Reassign();
  void SwapPlayers12();
  bool players_swapped() const { return swapped_.load(); }

  // Notices produced by connects/disconnects, drained by the UI thread.
  std::vector<std::string> TakeNotices();

 private:
  void AssignLocked();

  mutable std::mutex mutex_;
  std::vector<rex::input::DeviceInfo> devices_;
  std::vector<std::vector<rex::input::DeviceId>> users_ = std::vector<std::vector<rex::input::DeviceId>>(4);
  std::vector<std::string> notices_;
  std::atomic<bool> swapped_{false};
  bool first_update_ = true;
};

Gh2Assignment* Gh2GetAssignment();
// True when the device feeding guest user `user` is a guitar (lock-free; read on the guest's input thread).
bool Gh2UserIsGuitar(uint32_t user);
// True when a real (not synthetic) device feeds guest user `user`.
bool Gh2UserHasRealDevice(uint32_t user);
// Remembers the UI objects needed to pop connect/disconnect notices (call from OnCreateDialogs).
void Gh2SetControllerNoticeTarget(rex::ui::ImGuiDrawer* drawer, rex::ui::WindowedAppContext* context);
// The pad source for the hooks (gh2rt::HostServices::Pad*): the SDK input system's answer for a guest user, in host byte order.
bool Gh2PadSourceAvailable();
uint32_t Gh2PadGetState(uint32_t user, gh2rt::PadState* out);
uint32_t Gh2PadGetCapabilities(uint32_t user, uint32_t flags, gh2rt::PadCaps* out);
// Installs the assignment into the running input system (call before the guest starts polling).
void Gh2InstallControllerAssignment(rex::Runtime* runtime);
