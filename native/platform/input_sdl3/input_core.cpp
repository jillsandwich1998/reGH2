#include "input_core.h"

#include <SDL3/SDL.h>
#include <pthread.h>
#include <time.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <set>
#include <thread>

namespace gh2plat {

namespace {

// XInput button bits (XINPUT_GAMEPAD_*), the values of the SDK's X_INPUT_GAMEPAD_BUTTON.
constexpr uint16_t kDpadUp = 0x0001, kDpadDown = 0x0002, kDpadLeft = 0x0004, kDpadRight = 0x0008, kStart = 0x0010, kBack = 0x0020,
                   kLeftThumb = 0x0040, kRightThumb = 0x0080, kLeftShoulder = 0x0100, kRightShoulder = 0x0200, kGuide = 0x0400,
                   kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000;

// SDL_GamepadButton -> XInput bit, in the SDK driver's table and order (src/input/sdl/sdl_input_driver.cpp): the paddles and misc
// buttons fold onto face buttons / guide as there, so a pad behaves the same under either driver.
constexpr int kMappedButtons = 21;
constexpr uint16_t kXButton[kMappedButtons] = {
    kA, kB, kX, kY,                                       // SOUTH EAST WEST NORTH
    kBack, kGuide, kStart, kLeftThumb, kRightThumb,       // BACK GUIDE START LEFT_STICK RIGHT_STICK
    kLeftShoulder, kRightShoulder,                        // LEFT_SHOULDER RIGHT_SHOULDER
    kDpadUp, kDpadDown, kDpadLeft, kDpadRight,            // DPAD
    kGuide,                                               // MISC1
    kY, kB, kX, kA,                                       // RIGHT_PADDLE1 LEFT_PADDLE1 RIGHT_PADDLE2 LEFT_PADDLE2
    kGuide,                                               // TOUCHPAD
};
static_assert(SDL_GAMEPAD_BUTTON_SOUTH == 0 && SDL_GAMEPAD_BUTTON_DPAD_RIGHT == 14 && SDL_GAMEPAD_BUTTON_TOUCHPAD == 20,
              "SDL_GamepadButton order changed: revisit kXButton");

constexpr int64_t kRescanNs = 2'000'000'000;

SdlKind ToSdlKind(SDL_JoystickType t) {
  switch (t) {
    case SDL_JOYSTICK_TYPE_GAMEPAD: return SdlKind::kGamepad;
    case SDL_JOYSTICK_TYPE_WHEEL: return SdlKind::kWheel;
    case SDL_JOYSTICK_TYPE_ARCADE_STICK: return SdlKind::kArcadeStick;
    case SDL_JOYSTICK_TYPE_FLIGHT_STICK: return SdlKind::kFlightStick;
    case SDL_JOYSTICK_TYPE_DANCE_PAD: return SdlKind::kDancePad;
    case SDL_JOYSTICK_TYPE_GUITAR: return SdlKind::kGuitar;
    case SDL_JOYSTICK_TYPE_DRUM_KIT: return SdlKind::kDrumKit;
    case SDL_JOYSTICK_TYPE_ARCADE_PAD: return SdlKind::kArcadePad;
    case SDL_JOYSTICK_TYPE_THROTTLE: return SdlKind::kThrottle;
    default: return SdlKind::kUnknown;
  }
}

}  // namespace

namespace detail {

// What readers see: immutable once published. Entries outlive every table that lists them, tables are never freed while running
// (a handful per process: one per hot-plug), so readers need no reference counting.
struct Entry {
  DeviceInfo info;
  SnapshotSlot slot;
  std::atomic<bool> wireless{false};
  std::atomic<uint32_t> rumble{0};  // low << 16 | high
  std::atomic<bool> rumble_dirty{false};
};

struct Table {
  std::vector<const Entry*> entries;
};

}  // namespace detail

using detail::Entry;
using detail::Table;

struct InputCore::Impl {
  Options opt;
  std::thread thread;
  std::atomic<bool> stop{false};
  std::atomic<bool> hotplug{true};
  std::atomic<const Table*> table{nullptr};
  std::atomic<uint64_t> generation{0};

  // thread-private
  struct Open {
    SDL_JoystickID id = 0;
    SDL_Gamepad* gamepad = nullptr;
    SDL_Joystick* joystick = nullptr;  // owned only when `gamepad` is null (raw layout)
    Entry* entry = nullptr;
    uint32_t button_mask = 0;          // gamepad buttons that exist
    PadState last;
    uint32_t changes = 0;
    bool published = false;
  };
  std::vector<Open> open;
  std::vector<std::unique_ptr<Entry>> entries;
  std::vector<std::unique_ptr<const Table>> tables;
  uint64_t next_serial = 1;
  std::set<SDL_JoystickID> reported_skipped;

  // handshake with Start()
  std::mutex mu;
  std::condition_variable cv;
  bool first_scan_done = false;

  // stats
  std::atomic<uint64_t> ticks{0}, overruns{0}, max_tick_ns{0}, state_changes{0}, scans{0}, opened{0}, closed{0}, heartbeat_ns{0}, cpu_ns{0};
  mutable std::mutex skipped_mu;
  std::vector<std::string> skipped;

  void Log(const char* fmt, ...) const __attribute__((format(printf, 2, 3))) {
    if (!opt.log) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    opt.log(buf);
  }

  static bool Watch(void* userdata, SDL_Event* e) {
    switch (e->type) {
      case SDL_EVENT_JOYSTICK_ADDED:
      case SDL_EVENT_JOYSTICK_REMOVED:
      case SDL_EVENT_GAMEPAD_ADDED:
      case SDL_EVENT_GAMEPAD_REMOVED:
      case SDL_EVENT_GAMEPAD_REMAPPED:
        static_cast<Impl*>(userdata)->hotplug.store(true, std::memory_order_relaxed);
        break;
      default: break;
    }
    return true;
  }

  void Publish() {
    auto t = std::make_unique<Table>();
    for (const auto& o : open) t->entries.push_back(o.entry);
    const Table* raw = t.get();
    tables.push_back(std::move(t));
    table.store(raw, std::memory_order_release);
    generation.fetch_add(1, std::memory_order_release);
  }

  void NoteSkipped(SDL_JoystickID id, const std::string& why) {
    if (!reported_skipped.insert(id).second) return;
    std::lock_guard<std::mutex> lock(skipped_mu);
    skipped.push_back(why);
    Log("not opened: %s", why.c_str());
  }

  static void CloseOpen(Open& o) {
    if (o.gamepad) SDL_CloseGamepad(o.gamepad);
    else if (o.joystick) SDL_CloseJoystick(o.joystick);
    o.gamepad = nullptr;
    o.joystick = nullptr;
  }

  bool DeviceIsWireless(SDL_Joystick* js) const {
    if (SDL_GetJoystickConnectionState(js) == SDL_JOYSTICK_CONNECTION_WIRELESS) return true;
    // Backends that leave the connection state unknown still report power (same rule as the SDK's driver).
    const SDL_PowerState p = SDL_GetJoystickPowerInfo(js, nullptr);
    return p == SDL_POWERSTATE_ON_BATTERY || p == SDL_POWERSTATE_CHARGING || p == SDL_POWERSTATE_CHARGED;
  }

  void Scan() {
    scans.fetch_add(1, std::memory_order_relaxed);
    int n = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&n);
    std::set<SDL_JoystickID> present;
    for (int i = 0; i < n; ++i) present.insert(ids[i]);
    bool changed = false;

    for (auto it = open.begin(); it != open.end();) {
      if (present.count(it->id)) {
        ++it;
        continue;
      }
      Log("removed: %s (id %u)", it->entry->info.name.c_str(), it->id);
      CloseOpen(*it);
      closed.fetch_add(1, std::memory_order_relaxed);
      it = open.erase(it);
      changed = true;
    }

    for (int i = 0; i < n; ++i) {
      const SDL_JoystickID id = ids[i];
      if (std::any_of(open.begin(), open.end(), [&](const Open& o) { return o.id == id; })) continue;
      const char* name = SDL_GetJoystickNameForID(id);
      DeviceInfo info;
      info.instance_id = id;
      info.name = name ? name : "";
      info.vendor = SDL_GetJoystickVendorForID(id);
      info.product = SDL_GetJoystickProductForID(id);
      info.version = SDL_GetJoystickProductVersionForID(id);
      const char* path = SDL_GetJoystickPathForID(id);
      info.path = path ? path : "";
      char guid[40] = {};
      SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid, sizeof(guid));
      info.guid = guid;
      info.cls = Classify({info.vendor, info.product, info.name, ToSdlKind(SDL_GetJoystickTypeForID(id))});

      Open o;
      o.id = id;
      if (SDL_IsGamepad(id)) {
        o.gamepad = SDL_OpenGamepad(id);
        if (!o.gamepad) {
          NoteSkipped(id, info.name + ": SDL_OpenGamepad failed: " + SDL_GetError());
          continue;
        }
        for (int b = 0; b < kMappedButtons; ++b)
          if (SDL_GamepadHasButton(o.gamepad, static_cast<SDL_GamepadButton>(b))) o.button_mask |= 1u << b;
        info.has_navigation = true;
        for (SDL_GamepadButton nav : {SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
                                      SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT})
          if (!SDL_GamepadHasButton(o.gamepad, nav)) info.has_navigation = false;
        SDL_Joystick* js = SDL_GetGamepadJoystick(o.gamepad);
        info.wireless = DeviceIsWireless(js);
        info.can_rumble = SDL_GetBooleanProperty(SDL_GetJoystickProperties(js), SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN, false);
      } else if (opt.raw_fallback && info.cls.kind != DeviceKind::kGamepad) {
        // An instrument without a gamepad mapping (what an HIDAPI GIP / PS3 guitar can look like): SDL's HIDAPI drivers fill the
        // joystick in the standard gamepad layout, so read that.
        o.joystick = SDL_OpenJoystick(id);
        if (!o.joystick) {
          NoteSkipped(id, info.name + ": SDL_OpenJoystick failed: " + SDL_GetError());
          continue;
        }
        info.raw_layout = true;
        info.wireless = DeviceIsWireless(o.joystick);
        info.can_rumble = SDL_GetBooleanProperty(SDL_GetJoystickProperties(o.joystick), SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN, false);
      } else {
        char ids_text[64];
        snprintf(ids_text, sizeof(ids_text), " (%04x:%04x)", info.vendor, info.product);
        NoteSkipped(id, "'" + info.name + "'" + ids_text + " is not a gamepad in SDL (no mapping), kind " + KindName(info.cls.kind));
        continue;
      }
      auto entry = std::make_unique<Entry>();
      info.serial = next_serial++;
      entry->info = info;
      entry->wireless.store(info.wireless, std::memory_order_relaxed);
      o.entry = entry.get();
      entries.push_back(std::move(entry));
      Log("added: serial %llu '%s' %04x:%04x %s%s -> %s (%s: %s)", static_cast<unsigned long long>(info.serial), info.name.c_str(), info.vendor,
          info.product, info.path.c_str(), info.raw_layout ? " [raw layout]" : "", KindName(info.cls.kind), EvidenceName(info.cls.evidence),
          info.cls.detail);
      opened.fetch_add(1, std::memory_order_relaxed);
      open.push_back(std::move(o));
      changed = true;
    }
    SDL_free(ids);
    // A device must not be listed before it has a state: a joystick that was just opened reports zeros until the next update, so pump once and take
    // the first sample here (otherwise a reader that wins the race with the next tick sees an all-zero pad with changes == 0).
    bool any_unpublished = false;
    for (const auto& o : open) any_unpublished |= !o.published;
    if (any_unpublished) {
      SDL_UpdateJoysticks();
      const uint64_t now = NowNs();
      for (auto& o : open)
        if (!o.published) Poll(o, now);
    }
    for (auto& o : open) {  // power / connection information can arrive late
      SDL_Joystick* js = o.gamepad ? SDL_GetGamepadJoystick(o.gamepad) : o.joystick;
      o.entry->wireless.store(DeviceIsWireless(js), std::memory_order_relaxed);
    }
    if (changed) Publish();
  }

  PadState ReadGamepad(const Open& o) const {
    PadState p;
    for (uint32_t m = o.button_mask; m; m &= m - 1) {
      const int b = __builtin_ctz(m);
      if (!SDL_GetGamepadButton(o.gamepad, static_cast<SDL_GamepadButton>(b))) continue;
      if (kXButton[b] == kGuide && !opt.guide_button) continue;
      p.buttons |= kXButton[b];
    }
    // Same conventions as the SDK driver: SDL's Y axes point down, XInput's up (bitwise not = negate - 1); triggers 0..32767 -> 0..255.
    p.lx = SDL_GetGamepadAxis(o.gamepad, SDL_GAMEPAD_AXIS_LEFTX);
    p.ly = static_cast<int16_t>(~SDL_GetGamepadAxis(o.gamepad, SDL_GAMEPAD_AXIS_LEFTY));
    p.rx = SDL_GetGamepadAxis(o.gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
    p.ry = static_cast<int16_t>(~SDL_GetGamepadAxis(o.gamepad, SDL_GAMEPAD_AXIS_RIGHTY));
    p.left_trigger = static_cast<uint8_t>(std::max<int>(0, SDL_GetGamepadAxis(o.gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)) >> 7);
    p.right_trigger = static_cast<uint8_t>(std::max<int>(0, SDL_GetGamepadAxis(o.gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) >> 7);
    return p;
  }

  // Standard layout of SDL's HIDAPI joystick drivers: button i = SDL_GamepadButton i, axes in SDL_GamepadAxis order, hat 0 = d-pad.
  PadState ReadRaw(const Open& o) const {
    PadState p;
    SDL_Joystick* js = o.joystick;
    const int nb = std::min(SDL_GetNumJoystickButtons(js), 15);
    for (int b = 0; b < nb; ++b) {
      if (!SDL_GetJoystickButton(js, b)) continue;
      if (kXButton[b] == kGuide && !opt.guide_button) continue;
      p.buttons |= kXButton[b];
    }
    if (SDL_GetNumJoystickHats(js) > 0) {
      const Uint8 hat = SDL_GetJoystickHat(js, 0);
      if (hat & SDL_HAT_UP) p.buttons |= kDpadUp;
      if (hat & SDL_HAT_DOWN) p.buttons |= kDpadDown;
      if (hat & SDL_HAT_LEFT) p.buttons |= kDpadLeft;
      if (hat & SDL_HAT_RIGHT) p.buttons |= kDpadRight;
    }
    const int na = SDL_GetNumJoystickAxes(js);
    auto axis = [&](int i) -> int { return i < na ? SDL_GetJoystickAxis(js, i) : 0; };
    p.lx = static_cast<int16_t>(axis(0));
    p.ly = static_cast<int16_t>(~axis(1));
    p.rx = static_cast<int16_t>(axis(2));
    p.ry = static_cast<int16_t>(~axis(3));
    p.left_trigger = na > 4 ? static_cast<uint8_t>((axis(4) + 32768) >> 8) : 0;
    p.right_trigger = na > 5 ? static_cast<uint8_t>((axis(5) + 32768) >> 8) : 0;
    return p;
  }

  void Poll(Open& o, uint64_t now_ns) {
    const PadState cur = o.gamepad ? ReadGamepad(o) : ReadRaw(o);
    if (!o.published || !(cur == o.last)) {
      o.last = cur;
      ++o.changes;
      o.entry->slot.Write({cur, o.changes, now_ns});
      if (o.published) state_changes.fetch_add(1, std::memory_order_relaxed);
      o.published = true;
    }
    if (o.entry->rumble_dirty.exchange(false, std::memory_order_acquire)) {
      const uint32_t r = o.entry->rumble.load(std::memory_order_relaxed);
      const uint16_t lo = static_cast<uint16_t>(r >> 16), hi = static_cast<uint16_t>(r);
      if (o.gamepad) SDL_RumbleGamepad(o.gamepad, lo, hi, 0xFFFF);
      else SDL_RumbleJoystick(o.joystick, lo, hi, 0xFFFF);
    }
  }

  void Run() {
    pthread_setname_np(pthread_self(), "gh2-input");
    const int hz = std::clamp(opt.poll_hz, 10, 8000);
    const auto period = std::chrono::nanoseconds(1'000'000'000LL / hz);
    // udev / HIDAPI enumeration needs a few pumps before the first scan sees everything that is plugged in
    for (int i = 0; i < 3; ++i) {
      SDL_UpdateJoysticks();
      std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    Scan();
    {
      std::lock_guard<std::mutex> lock(mu);
      first_scan_done = true;
    }
    cv.notify_all();

    uint64_t last_scan = NowNs();
    auto next = std::chrono::steady_clock::now() + period;
    while (!stop.load(std::memory_order_acquire)) {
      const uint64_t t0 = NowNs();
      SDL_UpdateJoysticks();
      const uint64_t pumped = NowNs();
      if (hotplug.exchange(false, std::memory_order_relaxed) || pumped - last_scan > static_cast<uint64_t>(kRescanNs)) {
        Scan();
        last_scan = pumped;
      }
      for (auto& o : open) Poll(o, pumped);
      const uint64_t t1 = NowNs();
      heartbeat_ns.store(t1, std::memory_order_relaxed);
      const uint64_t n = ticks.fetch_add(1, std::memory_order_relaxed) + 1;
      const uint64_t dur = t1 - t0;
      uint64_t prev = max_tick_ns.load(std::memory_order_relaxed);
      while (dur > prev && !max_tick_ns.compare_exchange_weak(prev, dur, std::memory_order_relaxed)) {}
      if ((n & 255) == 0) {
        timespec ts;
        if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0)
          cpu_ns.store(static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(ts.tv_nsec), std::memory_order_relaxed);
      }
      const auto now = std::chrono::steady_clock::now();
      if (now > next + period) {  // a whole tick late: do not try to catch up
        overruns.fetch_add(1, std::memory_order_relaxed);
        next = now;
      }
      next += period;
      std::this_thread::sleep_until(next);
    }
    for (auto& o : open) CloseOpen(o);
    open.clear();
  }
};

InputCore::InputCore() : impl_(std::make_unique<Impl>()) {}
InputCore::~InputCore() { Stop(); }

uint64_t InputCore::NowNs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool InputCore::InitSdl(const Options& options) {
  if (options.allow_background_events) SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  // SDL's HIDAPI joystick drivers enumerate every USB device through libusb at start, and that enumeration (a blocking read of a device's descriptors) hangs for a minute or more when a device was left in
  // a bad state by a process that died without closing it: the game then sat before its first frame. The kernel's evdev path (xpad: the X-plorer, Xbox 360 pads) needs none of it. GH2_INPUT_HIDAPI=1 turns it back on
  // (devices that only HIDAPI drives: wireless / GIP Xbox One instruments, some PS3 ones).
  if (!std::getenv("GH2_INPUT_HIDAPI")) {
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    SDL_SetHint(SDL_HINT_HIDAPI_LIBUSB, "0");
  }
  return SDL_InitSubSystem(SDL_INIT_GAMEPAD);
}

void InputCore::QuitSdl() { SDL_QuitSubSystem(SDL_INIT_GAMEPAD); }

bool InputCore::Start(const Options& options) {
  if (running_.load()) return true;
  if (!SDL_WasInit(SDL_INIT_GAMEPAD)) return false;
  Impl& d = *impl_;
  d.opt = options;
  d.stop.store(false);
  d.hotplug.store(true);
  d.first_scan_done = false;
  SDL_AddEventWatch(&Impl::Watch, &d);
  d.thread = std::thread([&d] { d.Run(); });
  {
    std::unique_lock<std::mutex> lock(d.mu);
    d.cv.wait_for(lock, std::chrono::milliseconds(1500), [&] { return d.first_scan_done; });
  }
  running_.store(true, std::memory_order_release);
  return true;
}

void InputCore::Stop() {
  if (!running_.exchange(false)) return;
  Impl& d = *impl_;
  SDL_RemoveEventWatch(&Impl::Watch, &d);
  d.stop.store(true, std::memory_order_release);
  if (d.thread.joinable()) d.thread.join();
  // Readers may still be inside Devices() / Read() of the table they loaded: nothing is freed here (tables and entries live until the
  // InputCore is destroyed); an empty table is published so that new readers see no device.
  d.Publish();
}

uint64_t InputCore::Generation() const { return impl_->generation.load(std::memory_order_acquire); }

std::vector<DeviceInfo> InputCore::Devices() const {
  std::vector<DeviceInfo> out;
  const Table* t = impl_->table.load(std::memory_order_acquire);
  if (!t) return out;
  for (const Entry* e : t->entries) {
    DeviceInfo i = e->info;
    i.wireless = e->wireless.load(std::memory_order_relaxed);
    out.push_back(std::move(i));
  }
  return out;
}

bool InputCore::Read(uint64_t serial, Sample* out) const {
  const Table* t = impl_->table.load(std::memory_order_acquire);
  if (!t) return false;
  for (const Entry* e : t->entries) {
    if (e->info.serial != serial) continue;
    *out = e->slot.Read();
    return true;
  }
  return false;
}

void InputCore::Rumble(uint64_t serial, uint16_t low, uint16_t high) {
  const Table* t = impl_->table.load(std::memory_order_acquire);
  if (!t) return;
  for (const Entry* e : t->entries) {
    if (e->info.serial != serial) continue;
    auto* m = const_cast<Entry*>(e);
    m->rumble.store(static_cast<uint32_t>(low) << 16 | high, std::memory_order_relaxed);
    m->rumble_dirty.store(true, std::memory_order_release);
    return;
  }
}

CoreStats InputCore::Stats() const {
  const Impl& d = *impl_;
  CoreStats s;
  s.ticks = d.ticks.load(std::memory_order_relaxed);
  s.overruns = d.overruns.load(std::memory_order_relaxed);
  s.max_tick_ns = d.max_tick_ns.load(std::memory_order_relaxed);
  s.state_changes = d.state_changes.load(std::memory_order_relaxed);
  s.scans = d.scans.load(std::memory_order_relaxed);
  s.devices_opened = d.opened.load(std::memory_order_relaxed);
  s.devices_closed = d.closed.load(std::memory_order_relaxed);
  s.heartbeat_ns = d.heartbeat_ns.load(std::memory_order_relaxed);
  s.thread_cpu_ns = d.cpu_ns.load(std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(d.skipped_mu);
  s.skipped = d.skipped;
  return s;
}

}  // namespace gh2plat
