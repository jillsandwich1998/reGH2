// gh2plat input core: ONE dedicated thread polls the SDL3 joystick layer (SDL_UpdateJoysticks, ~1 kHz) and publishes each device's
// latest XInput-style state in a lock-free slot; the guest's XamInputGetState path only reads slots.
//
// Why: the SDK's own SDL driver refreshes a pad by asking the UI thread to SDL_PumpEvents() on every guest poll and answers from
// what earlier pumps delivered, so a press reaches the guest one poll (a frame) late at best, and later whenever the UI thread is busy.
//
// No SDK (rex::) dependency, SDL3 public API only (the SDK's SDL3 build: see CMakeLists.txt). Reading never takes a lock the
// polling thread can hold: slots are seqlocks, the device table is an immutable snapshot swapped with one atomic store.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "guitar_classifier.h"
#include "snapshot_slot.h"

namespace gh2plat {

struct DeviceInfo {
  uint64_t serial = 0;       // never reused within the process; the guest-facing id derives from it
  uint32_t instance_id = 0;  // SDL_JoystickID
  std::string name;
  std::string guid;
  std::string path;          // /dev/input/eventN, hidapi path, ...
  uint16_t vendor = 0, product = 0, version = 0;
  Classification cls;
  bool raw_layout = false;   // no SDL gamepad mapping: read as a joystick in the standard (SDL HIDAPI) layout
  bool wireless = false;
  bool has_navigation = true;  // start/back/d-pad all exist (XInput caps NO_NAVIGATION otherwise)
  bool can_rumble = false;
};

struct CoreStats {
  uint64_t ticks = 0;
  uint64_t overruns = 0;           // ticks that started more than one period late
  uint64_t max_tick_ns = 0;        // longest SDL_UpdateJoysticks + reads
  uint64_t state_changes = 0;
  uint64_t scans = 0;
  uint64_t devices_opened = 0;
  uint64_t devices_closed = 0;
  uint64_t heartbeat_ns = 0;       // steady_clock ns of the last tick
  uint64_t thread_cpu_ns = 0;      // CPU time used by the polling thread so far
  std::vector<std::string> skipped;  // joysticks SDL lists that were not opened, with the reason
};

class InputCore {
 public:
  struct Options {
    int poll_hz = 1000;
    bool guide_button = false;           // pass the guide button on (the SDK's `guide_button` cvar)
    bool allow_background_events = true; // keep reading while the window has no focus; callers decide what to expose
    bool raw_fallback = true;            // open instruments SDL has no gamepad mapping for, standard layout
    void (*log)(const char* line) = nullptr;
  };

  InputCore();
  ~InputCore();  // Stop()
  InputCore(const InputCore&) = delete;
  InputCore& operator=(const InputCore&) = delete;

  // SDL side: hints + SDL_InitSubSystem(SDL_INIT_GAMEPAD). Call on the thread SDL wants subsystems started on (the UI thread when
  // a window exists); QuitSdl() balances it. Safe to call several times (SDL reference counts).
  static bool InitSdl(const Options& options);
  static void QuitSdl();

  // Starts the polling thread (SDL already initialised). The first device scan has finished when this returns (bounded wait).
  bool Start(const Options& options);
  // Joins the thread and closes the devices. Readers may still be inside Read() / Devices(): their memory stays valid until the InputCore
  // is destroyed, and from now on they see no device.
  void Stop();
  bool running() const { return running_.load(std::memory_order_acquire); }

  // ---- readers (any thread, lock-free) ----
  uint64_t Generation() const;                         // bumps whenever the set of devices changes
  std::vector<DeviceInfo> Devices() const;             // connection order
  bool Read(uint64_t serial, Sample* out) const;       // false: no such device (any more)
  void Rumble(uint64_t serial, uint16_t low, uint16_t high);  // applied by the polling thread on its next tick
  CoreStats Stats() const;

  static uint64_t NowNs();                             // the clock of Sample::first_seen_ns (steady_clock)

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::atomic<bool> running_{false};
};

}  // namespace gh2plat
