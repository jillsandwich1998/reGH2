// gh2_input_selftest - checks of the gh2plat input core.
//
//   gh2_input_selftest unit                    classifier tables, snapshot-slot stress, the core against SDL virtual joysticks
//   gh2_input_selftest list [SECONDS]          what the core makes of the devices plugged in now (state changes are printed)
//   gh2_input_selftest e2e [EVENTS] [HZ]       press-to-snapshot latency through the kernel with a uinput X-plorer clone
//   gh2_input_selftest inject HZ SECONDS LOG [DELAY_S] [PRODUCT_HEX]
//   gh2_input_selftest play HZ SECONDS [DELAY_S]     (random frets + strum: a crude player for performance tests)
//                                              uinput guitar (ids 1430:PRODUCT, default 4749) that waits DELAY_S, then toggles the orange fret
//                                              about HZ times a second and logs the steady-clock ns of each write (to correlate with what the
//                                              game's input hook saw: GH2_INPUT_LOG)
// Exit status 0 = everything passed.
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "guitar_classifier.h"
#include "input_core.h"
#include "snapshot_slot.h"
#include "uinput_guitar.h"

using namespace gh2plat;
using Clock = std::chrono::steady_clock;

namespace {

int g_failures = 0;
void Check(bool ok, const std::string& what, const std::string& detail = "") {
  std::printf("[%s] %s%s%s\n", ok ? "ok" : "!!", what.c_str(), detail.empty() ? "" : " - ", detail.c_str());
  if (!ok) ++g_failures;
}

double Percentile(std::vector<double> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, static_cast<size_t>(p / 100.0 * v.size()))];
}

void Sleep(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

void LogLine(const char* line) { std::fprintf(stderr, "    core: %s\n", line); }

// ---------------------------------------------------------------------------------------------------------------- classifier
void TestClassifier() {
  std::printf("== classifier\n");
  struct Case {
    const char* what;
    DeviceIdentity id;
    DeviceKind kind;
    Evidence evidence;
    uint8_t subtype;
  };
  const Case cases[] = {
      {"real X-plorer ids", {0x1430, 0x4748, "RedOctane Guitar Hero X-plorer", SdlKind::kGamepad}, DeviceKind::kGuitar, Evidence::kUsbId, 6},
      {"X-plorer, ids unknown, by name", {0x0001, 0x0002, "Guitar Hero X-plorer", SdlKind::kGamepad}, DeviceKind::kGuitar, Evidence::kName, 6},
      {"Rock Band Stratocaster, name only", {0x0001, 0x0002, "Mad Catz Fender Stratocaster", SdlKind::kUnknown}, DeviceKind::kGuitar, Evidence::kName, 6},
      {"Xbox 360 pad", {0x045e, 0x028e, "Xbox 360 Controller", SdlKind::kGamepad}, DeviceKind::kGamepad, Evidence::kDefault, 1},
      {"DualShock", {0x054c, 0x09cc, "Sony Interactive Entertainment Wireless Controller", SdlKind::kGamepad}, DeviceKind::kGamepad, Evidence::kDefault, 1},
      {"SDL says guitar (HIDAPI GIP)", {0x0e6f, 0x0248, "PDP Riffmaster", SdlKind::kGuitar}, DeviceKind::kGuitar, Evidence::kSdlType, 6},
      {"SDL says guitar, nothing else known", {0x1111, 0x2222, "Some Instrument", SdlKind::kGuitar}, DeviceKind::kGuitar, Evidence::kSdlType, 6},
      {"SDL says drums", {0x1111, 0x2222, "Some Kit", SdlKind::kDrumKit}, DeviceKind::kDrumKit, Evidence::kSdlType, 8},
      {"PS3 Guitar Hero guitar by ids", {0x12ba, 0x0100, "Sony PLAYSTATION(R)3 Controller", SdlKind::kUnknown}, DeviceKind::kGuitar, Evidence::kUsbId, 6},
      {"PS3 Guitar Hero drums by ids", {0x12ba, 0x0120, "Sony PLAYSTATION(R)3 Controller", SdlKind::kGamepad}, DeviceKind::kDrumKit, Evidence::kUsbId, 8},
      {"Rock Band drums (Xbox 360) by ids", {0x1bad, 0x0003, "Harmonix Drum Kit", SdlKind::kGamepad}, DeviceKind::kDrumKit, Evidence::kUsbId, 8},
      {"Riffmaster by ids", {0x0e6f, 0x0248, "Controller", SdlKind::kGamepad}, DeviceKind::kGuitar, Evidence::kUsbId, 6},
      {"drums named after the guitar series", {0x0001, 0x0002, "Guitar Hero World Tour Drums", SdlKind::kGamepad}, DeviceKind::kDrumKit, Evidence::kName, 8},
      {"XInput Guitar #1 (Windows naming)", {0, 0, "XInput Guitar #1", SdlKind::kGamepad}, DeviceKind::kGuitar, Evidence::kName, 6},
      {"upper case", {0, 0, "GUITAR HERO", SdlKind::kUnknown}, DeviceKind::kGuitar, Evidence::kName, 6},
      {"arcade stick by name", {0, 0, "XInput ArcadeStick #1", SdlKind::kGamepad}, DeviceKind::kArcadeStick, Evidence::kName, 3},
      {"arcade pad by name", {0, 0, "XInput ArcadePad #1", SdlKind::kGamepad}, DeviceKind::kArcadePad, Evidence::kName, 0x13},
      {"wheel by name", {0, 0, "Logitech G29 Racing Wheel", SdlKind::kGamepad}, DeviceKind::kWheel, Evidence::kName, 2},
      {"dance pad by name", {0, 0, "Dance Pad", SdlKind::kUnknown}, DeviceKind::kDancePad, Evidence::kName, 5},
      {"flight stick by name", {0, 0, "Thrustmaster HOTAS Warthog", SdlKind::kGamepad}, DeviceKind::kFlightStick, Evidence::kName, 4},
      {"SDL says wheel", {0, 0, "x", SdlKind::kWheel}, DeviceKind::kWheel, Evidence::kSdlType, 2},
      {"empty name, unknown ids", {0, 0, "", SdlKind::kUnknown}, DeviceKind::kGamepad, Evidence::kDefault, 1},
      {"a name that merely contains 'gui'", {0, 0, "Guild Controller", SdlKind::kGamepad}, DeviceKind::kGamepad, Evidence::kDefault, 1},
  };
  int bad = 0;
  for (const auto& c : cases) {
    const auto r = Classify(c.id);
    const bool ok = r.kind == c.kind && r.evidence == c.evidence && r.xinput_subtype == c.subtype;
    if (!ok) {
      ++bad;
      std::printf("  MISMATCH %-40s got %s/%s/%u, wanted %s/%s/%u\n", c.what, KindName(r.kind), EvidenceName(r.evidence), r.xinput_subtype,
                  KindName(c.kind), EvidenceName(c.evidence), c.subtype);
    }
  }
  Check(bad == 0, "classifier table", std::to_string(sizeof(cases) / sizeof(cases[0]) - bad) + "/" + std::to_string(sizeof(cases) / sizeof(cases[0])) + " cases");
  Check(IsGuitarSubtype(6) && IsGuitarSubtype(7) && IsGuitarSubtype(0x0B) && !IsGuitarSubtype(1) && !IsGuitarSubtype(8), "IsGuitarSubtype matches the app's rule");
}

// ---------------------------------------------------------------------------------------------------------------- slot
Sample MakeSample(uint32_t i) {
  Sample s;
  s.pad.buttons = static_cast<uint16_t>(i);
  s.pad.left_trigger = static_cast<uint8_t>(i * 3);
  s.pad.right_trigger = static_cast<uint8_t>(i * 5);
  s.pad.lx = static_cast<int16_t>(i * 7);
  s.pad.ly = static_cast<int16_t>(i * 11);
  s.pad.rx = static_cast<int16_t>(i * 13);
  s.pad.ry = static_cast<int16_t>(i * 17);
  s.changes = i;
  s.first_seen_ns = static_cast<uint64_t>(i) * 1000003ull;
  return s;
}

void TestSlotStress() {
  std::printf("== snapshot slot stress\n");
  SnapshotSlot slot;
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> reads{0}, torn{0};
  uint64_t writes = 0;
  std::vector<std::thread> readers;
  for (int r = 0; r < 4; ++r)
    readers.emplace_back([&] {
      uint64_t local = 0, bad = 0;
      uint32_t last = 0;
      bool regress = false;
      while (!stop.load(std::memory_order_relaxed)) {
        const Sample s = slot.Read();
        if (s.changes != 0 || s.first_seen_ns != 0) {
          const Sample want = MakeSample(s.changes);
          if (!(s.pad == want.pad) || s.first_seen_ns != want.first_seen_ns) ++bad;
        }
        if (s.changes < last) regress = true;
        last = s.changes;
        ++local;
      }
      reads += local;
      torn += bad + (regress ? 1 : 0);
    });
  const auto t0 = Clock::now();
  uint32_t i = 1;
  while (Clock::now() - t0 < std::chrono::milliseconds(1500)) {
    for (int k = 0; k < 64; ++k) slot.Write(MakeSample(i++)), ++writes;
  }
  stop = true;
  for (auto& t : readers) t.join();
  Check(torn == 0, "no torn or regressing read under 4 readers and a hot writer",
        std::to_string(reads.load()) + " reads, " + std::to_string(writes) + " writes, " + std::to_string(torn.load()) + " bad");
}

// ---------------------------------------------------------------------------------------------------------------- virtual SDL devices
struct Virtual {
  SDL_JoystickID id = 0;
  SDL_Joystick* js = nullptr;
  bool Attach(const char* name, SDL_JoystickType type, int naxes, int nbuttons, int nhats, uint16_t vid, uint16_t pid) {
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = static_cast<Uint16>(type);
    d.naxes = static_cast<Uint16>(naxes);
    d.nbuttons = static_cast<Uint16>(nbuttons);
    d.nhats = static_cast<Uint16>(nhats);
    d.vendor_id = vid;
    d.product_id = pid;
    d.name = name;
    id = SDL_AttachVirtualJoystick(&d);
    if (!id) return false;
    js = SDL_OpenJoystick(id);
    return js != nullptr;
  }
  void Detach() {
    if (js) SDL_CloseJoystick(js);
    if (id) SDL_DetachVirtualJoystick(id);
    js = nullptr;
    id = 0;
  }
  void Button(int b, bool down) { SDL_SetJoystickVirtualButton(js, b, down); }
  void Axis(int a, int v) { SDL_SetJoystickVirtualAxis(js, a, static_cast<Sint16>(v)); }
  void Hat(int v) { SDL_SetJoystickVirtualHat(js, 0, static_cast<Uint8>(v)); }
};

bool FindDevice(const InputCore& core, const std::string& name, DeviceInfo* out, int timeout_ms = 1500) {
  const auto t0 = Clock::now();
  while (Clock::now() - t0 < std::chrono::milliseconds(timeout_ms)) {
    for (const auto& d : core.Devices())
      if (d.name == name) {
        *out = d;
        return true;
      }
    Sleep(2);
  }
  return false;
}

bool WaitState(const InputCore& core, uint64_t serial, const std::function<bool(const PadState&)>& pred, int timeout_ms = 300) {
  const auto t0 = Clock::now();
  while (Clock::now() - t0 < std::chrono::milliseconds(timeout_ms)) {
    Sample s;
    if (core.Read(serial, &s) && pred(s.pad)) return true;
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  return false;
}

std::string Describe(const PadState& p) {
  char b[120];
  snprintf(b, sizeof(b), "btn=%04x lt=%u rt=%u l=(%d,%d) r=(%d,%d)", p.buttons, p.left_trigger, p.right_trigger, p.lx, p.ly, p.rx, p.ry);
  return b;
}

void TestCoreWithVirtualDevices() {
  std::printf("== core against SDL virtual joysticks (the SDK's SDL3 %d.%d.%d)\n", SDL_GetVersion() / 1000000, (SDL_GetVersion() / 1000) % 1000,
              SDL_GetVersion() % 1000);
  InputCore::Options opt;
  opt.log = LogLine;
  Check(InputCore::InitSdl(opt), "SDL gamepad subsystem starts", SDL_GetError());
  InputCore core;
  Check(core.Start(opt), "polling thread starts");
  const auto real = core.Devices();
  std::printf("   real devices seen at start: %zu\n", real.size());
  for (const auto& d : real)
    std::printf("     serial %llu '%s' %04x:%04x -> %s (%s: %s), %s\n", static_cast<unsigned long long>(d.serial), d.name.c_str(), d.vendor, d.product,
                KindName(d.cls.kind), EvidenceName(d.cls.evidence), d.cls.detail, d.path.c_str());

  // ---- (a) a standard gamepad: every button and axis conversion
  {
    Virtual pad;
    Check(pad.Attach("GH2 selftest pad", SDL_JOYSTICK_TYPE_GAMEPAD, SDL_GAMEPAD_AXIS_COUNT, 21, 0, 0x7a57, 0x0001), "attach a virtual gamepad", SDL_GetError());
    DeviceInfo info;
    if (FindDevice(core, "GH2 selftest pad", &info)) {
      Check(!info.raw_layout && info.cls.kind == DeviceKind::kGamepad && info.cls.xinput_subtype == 1, "virtual gamepad: opened as a gamepad, kind Gamepad / sub-type 1");
      struct B { int sdl; uint16_t x; const char* n; };
      const B map[] = {{0, 0x1000, "A"}, {1, 0x2000, "B"}, {2, 0x4000, "X"}, {3, 0x8000, "Y"}, {4, 0x0020, "Back"}, {6, 0x0010, "Start"}, {7, 0x0040, "LeftThumb"},
                       {8, 0x0080, "RightThumb"}, {9, 0x0100, "LeftShoulder"}, {10, 0x0200, "RightShoulder"}, {11, 0x0001, "DpadUp"}, {12, 0x0002, "DpadDown"},
                       {13, 0x0004, "DpadLeft"}, {14, 0x0008, "DpadRight"}, {16, 0x8000, "RightPaddle1->Y"}, {17, 0x2000, "LeftPaddle1->B"},
                       {18, 0x4000, "RightPaddle2->X"}, {19, 0x1000, "LeftPaddle2->A"}};
      int wrong = 0;
      for (const auto& m : map) {
        pad.Button(m.sdl, true);
        const bool down = WaitState(core, info.serial, [&](const PadState& p) { return p.buttons == m.x; });
        pad.Button(m.sdl, false);
        const bool up = WaitState(core, info.serial, [&](const PadState& p) { return p.buttons == 0; });
        if (!down || !up) {
          ++wrong;
          std::printf("   button %s: %s\n", m.n, !down ? "never arrived" : "never released");
        }
      }
      Check(wrong == 0, "every SDL button maps to the XInput bit the SDK driver uses", std::to_string(sizeof(map) / sizeof(map[0]) - wrong) + "/" + std::to_string(sizeof(map) / sizeof(map[0])));
      pad.Button(5, true);  // guide: not passed on by default
      Sleep(20);
      Sample s;
      core.Read(info.serial, &s);
      Check(s.pad.buttons == 0, "guide button is not passed on by default (SDK cvar guide_button = false)");
      pad.Button(5, false);
      pad.Axis(SDL_GAMEPAD_AXIS_LEFTX, 12345);
      pad.Axis(SDL_GAMEPAD_AXIS_LEFTY, -12345);
      pad.Axis(SDL_GAMEPAD_AXIS_RIGHTX, -32768);
      pad.Axis(SDL_GAMEPAD_AXIS_RIGHTY, 32767);
      pad.Axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
      pad.Axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 16384);
      // SDL maps a trigger axis' full range (-32768..32767) onto 0..32767 itself, so the right-trigger expectation is read from SDL
      SDL_Gamepad* gp = SDL_OpenGamepad(pad.id);
      Sleep(5);
      const int want_rt = gp ? std::max<int>(0, SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) >> 7 : -1;
      if (gp) SDL_CloseGamepad(gp);
      const bool axes = WaitState(core, info.serial, [&](const PadState& p) {
        return p.lx == 12345 && p.ly == static_cast<int16_t>(~(-12345)) && p.rx == -32768 && p.ry == static_cast<int16_t>(~32767) && p.left_trigger == 255 &&
               p.right_trigger == want_rt;
      });
      core.Read(info.serial, &s);
      Check(axes && want_rt > 0 && want_rt < 255, "axes: Y inverted like the SDK (~v), triggers = SDL's 0..32767 value >> 7", Describe(s.pad) + ", SDL gives rt " + std::to_string(want_rt));
      pad.Detach();
      const auto t0 = Clock::now();
      bool gone = false;
      while (Clock::now() - t0 < std::chrono::milliseconds(600) && !gone) {
        gone = true;
        for (const auto& d : core.Devices()) gone &= d.name != "GH2 selftest pad";
        Sleep(2);
      }
      Check(gone, "device is dropped after it is detached");
      Sample dead;
      Check(!core.Read(info.serial, &dead), "Read() of a removed serial reports 'no device'");
    } else {
      Check(false, "virtual gamepad shows up in the core");
      pad.Detach();
    }
  }

  // ---- (b) an instrument SDL types as GUITAR but has no gamepad mapping for: the raw standard layout
  {
    Virtual g;
    Check(g.Attach("GH2 selftest instrument", SDL_JOYSTICK_TYPE_GUITAR, 6, 15, 1, 0x7a57, 0x0002), "attach a virtual SDL_JOYSTICK_TYPE_GUITAR", SDL_GetError());
    DeviceInfo info;
    if (FindDevice(core, "GH2 selftest instrument", &info)) {
      Check(info.raw_layout && info.cls.kind == DeviceKind::kGuitar && info.cls.evidence == Evidence::kSdlType && info.cls.xinput_subtype == 6,
            "SDL type GUITAR without a gamepad mapping: opened as a raw joystick, guitar by sdl-type, sub-type 6");
      g.Button(0, true);
      bool ok = WaitState(core, info.serial, [](const PadState& p) { return p.buttons == 0x1000; });
      g.Button(0, false);
      g.Button(2, true);
      ok &= WaitState(core, info.serial, [](const PadState& p) { return p.buttons == 0x4000; });
      g.Button(2, false);
      g.Button(9, true);  // LEFT_SHOULDER = the orange fret on GIP guitars
      ok &= WaitState(core, info.serial, [](const PadState& p) { return p.buttons == 0x0100; });
      g.Button(9, false);
      g.Hat(SDL_HAT_UP);
      ok &= WaitState(core, info.serial, [](const PadState& p) { return p.buttons == 0x0001; });
      g.Hat(SDL_HAT_DOWN);
      ok &= WaitState(core, info.serial, [](const PadState& p) { return p.buttons == 0x0002; });
      g.Hat(SDL_HAT_CENTERED);
      g.Axis(2, 20000);   // whammy (SDL_GAMEPAD_AXIS_RIGHTX)
      g.Axis(4, 32767);   // left trigger axis
      g.Axis(5, -32768);
      ok &= WaitState(core, info.serial, [](const PadState& p) { return p.buttons == 0 && p.rx == 20000 && p.left_trigger == 255 && p.right_trigger == 0; });
      Sample s;
      core.Read(info.serial, &s);
      Check(ok, "raw layout: frets, strum hat, whammy axis, trigger scaling", Describe(s.pad));
    } else {
      Check(false, "virtual guitar shows up in the core");
    }
    g.Detach();
  }

  // ---- (c) a gamepad whose name says guitar, ids unknown
  {
    Virtual g;
    g.Attach("GH2 Guitar Hero Test Controller", SDL_JOYSTICK_TYPE_GAMEPAD, SDL_GAMEPAD_AXIS_COUNT, 21, 0, 0x7a57, 0x0003);
    DeviceInfo info;
    if (FindDevice(core, "GH2 Guitar Hero Test Controller", &info))
      Check(info.cls.kind == DeviceKind::kGuitar && info.cls.evidence == Evidence::kName && info.cls.xinput_subtype == 6, "gamepad named '...Guitar...': guitar by name, sub-type 6");
    else
      Check(false, "named virtual gamepad shows up in the core");
    g.Detach();
  }

  // ---- (d) latency of the thread itself: set a virtual button, time until the snapshot has it
  {
    Virtual pad;
    pad.Attach("GH2 selftest latency pad", SDL_JOYSTICK_TYPE_GAMEPAD, SDL_GAMEPAD_AXIS_COUNT, 21, 0, 0x7a57, 0x0004);
    DeviceInfo info;
    if (FindDevice(core, "GH2 selftest latency pad", &info)) {
      std::mt19937 rng(7);
      std::vector<double> seen_us, visible_us;
      Sample s;
      core.Read(info.serial, &s);
      uint32_t last = s.changes;
      int timeouts = 0;
      for (int i = 0; i < 1500; ++i) {
        std::this_thread::sleep_for(std::chrono::microseconds(1500 + rng() % 2500));
        const uint64_t t_set = InputCore::NowNs();
        pad.Button(0, (i & 1) == 0);
        uint64_t t_vis = 0;
        while (InputCore::NowNs() - t_set < 50'000'000ull) {
          core.Read(info.serial, &s);
          if (s.changes != last) {
            t_vis = InputCore::NowNs();
            break;
          }
        }
        if (!t_vis) {
          ++timeouts;
          continue;
        }
        last = s.changes;
        seen_us.push_back((static_cast<double>(s.first_seen_ns) - static_cast<double>(t_set)) / 1000.0);
        visible_us.push_back((static_cast<double>(t_vis) - static_cast<double>(t_set)) / 1000.0);
      }
      std::printf("   virtual device -> snapshot, %zu events: seen by the thread p50 %.0f us, p99 %.0f us, max %.0f us; readable p50 %.0f us, p99 %.0f us\n",
                  seen_us.size(), Percentile(seen_us, 50), Percentile(seen_us, 99), Percentile(seen_us, 100), Percentile(visible_us, 50), Percentile(visible_us, 99));
      Check(timeouts == 0 && Percentile(visible_us, 99) < 3000, "every change reaches the snapshot, p99 under 3 ms (1 kHz thread)", std::to_string(timeouts) + " timeouts");
    } else {
      Check(false, "latency pad shows up in the core");
    }
    pad.Detach();
  }

  // ---- (e)+(f) hot-plug churn with readers hammering Devices()/Read()
  {
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> reads{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < 3; ++r)
      readers.emplace_back([&] {
        uint64_t n = 0;
        while (!stop.load(std::memory_order_relaxed)) {
          for (const auto& d : core.Devices()) {
            Sample s;
            core.Read(d.serial, &s);
          }
          ++n;
        }
        reads += n;
      });
    int missing = 0, stale = 0;
    for (int i = 0; i < 40; ++i) {
      Virtual v;
      const std::string name = "GH2 churn " + std::to_string(i);
      v.Attach(name.c_str(), SDL_JOYSTICK_TYPE_GAMEPAD, SDL_GAMEPAD_AXIS_COUNT, 21, 0, 0x7a57, static_cast<uint16_t>(0x0100 + i));
      DeviceInfo info;
      if (!FindDevice(core, name, &info, 800)) ++missing;
      v.Detach();
      const auto t0 = Clock::now();
      bool gone = false;
      while (Clock::now() - t0 < std::chrono::milliseconds(800) && !gone) {
        gone = true;
        for (const auto& d : core.Devices()) gone &= d.name != name;
        Sleep(1);
      }
      if (!gone) ++stale;
    }
    stop = true;
    for (auto& t : readers) t.join();
    Check(missing == 0 && stale == 0, "40 attach/detach cycles under 3 hammering readers: every device appears and disappears",
          std::to_string(missing) + " never appeared, " + std::to_string(stale) + " never left, " + std::to_string(reads.load()) + " reader loops");
  }

  // ---- (g) Stop() / Start() cycles while readers keep reading: memory stays valid, the devices come back
  {
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> reads{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < 3; ++r)
      readers.emplace_back([&] {
        uint64_t n = 0;
        while (!stop.load(std::memory_order_relaxed)) {
          for (const auto& d : core.Devices()) {
            Sample s;
            core.Read(d.serial, &s);
          }
          ++n;
        }
        reads += n;
      });
    const size_t before = core.Devices().size();
    bool back = true;
    for (int i = 0; i < 5; ++i) {
      core.Stop();
      back &= core.Devices().empty() && !core.running();
      back &= core.Start(opt) && core.Devices().size() == before;
    }
    stop = true;
    for (auto& t : readers) t.join();
    Check(back && before >= 1, "5 Stop()/Start() cycles under 3 readers: no device while stopped, the same devices after each start",
          std::to_string(before) + " devices, " + std::to_string(reads.load()) + " reader loops");
  }

  const CoreStats st = core.Stats();
  const double cpu_ms = st.thread_cpu_ns / 1e6;
  std::printf("   thread: %llu ticks, %llu overruns, longest tick %.0f us, %.0f ms CPU, %llu scans, %llu devices opened / %llu closed\n",
              static_cast<unsigned long long>(st.ticks), static_cast<unsigned long long>(st.overruns), st.max_tick_ns / 1000.0, cpu_ms,
              static_cast<unsigned long long>(st.scans), static_cast<unsigned long long>(st.devices_opened), static_cast<unsigned long long>(st.devices_closed));
  for (const auto& s : st.skipped) std::printf("   skipped: %s\n", s.c_str());
  core.Stop();
  InputCore::QuitSdl();
  Check(!core.running(), "core stops cleanly");
}

// ---------------------------------------------------------------------------------------------------------------- list
int List(double seconds) {
  InputCore::Options opt;
  opt.log = LogLine;
  if (!InputCore::InitSdl(opt)) return std::fprintf(stderr, "SDL: %s\n", SDL_GetError()), 1;
  InputCore core;
  core.Start(opt);
  const auto devices = core.Devices();
  std::printf("devices: %zu\n", devices.size());
  for (const auto& d : devices) {
    Sample s;
    core.Read(d.serial, &s);
    std::printf("  serial %llu '%s'\n    ids %04x:%04x v%04x  guid %s  path %s\n    -> %s, XInput sub-type %u  (evidence: %s = %s)%s%s%s\n    idle: %s\n",
                static_cast<unsigned long long>(d.serial), d.name.c_str(), d.vendor, d.product, d.version, d.guid.c_str(), d.path.c_str(), KindName(d.cls.kind),
                d.cls.xinput_subtype, EvidenceName(d.cls.evidence), d.cls.detail, d.raw_layout ? "  [raw layout]" : "", d.wireless ? "  [wireless]" : "",
                d.has_navigation ? "" : "  [no navigation buttons]", Describe(s.pad).c_str());
  }
  for (const auto& s : core.Stats().skipped) std::printf("  not opened: %s\n", s.c_str());
  if (seconds > 0) {
    std::printf("watching %.0f s - press things\n", seconds);
    const auto t0 = Clock::now();
    std::vector<uint32_t> last(devices.size() + 1, 0);
    while (std::chrono::duration<double>(Clock::now() - t0).count() < seconds) {
      for (size_t i = 0; i < devices.size(); ++i) {
        Sample s;
        if (core.Read(devices[i].serial, &s) && s.changes != last[i]) {
          last[i] = s.changes;
          std::printf("%8.3f  serial %llu  %s\n", std::chrono::duration<double>(Clock::now() - t0).count(), static_cast<unsigned long long>(devices[i].serial),
                      Describe(s.pad).c_str());
        }
      }
      Sleep(1);
    }
  }
  core.Stop();
  InputCore::QuitSdl();
  return 0;
}

// ---------------------------------------------------------------------------------------------------------------- uinput
int E2E(int events, int hz) {
  std::printf("== kernel -> snapshot with a uinput X-plorer clone\n");
  UinputGuitar guitar;
  if (!guitar.Create("GH2 uinput X-plorer clone")) {
    std::printf("cannot create the uinput device: %s\n", guitar.error().c_str());
    return 2;
  }
  std::printf("   virtual device node: %s\n", guitar.event_path().c_str());
  InputCore::Options opt;
  opt.poll_hz = hz;
  opt.log = LogLine;
  InputCore::InitSdl(opt);
  InputCore core;
  core.Start(opt);
  DeviceInfo clone;
  const bool found = FindDevice(core, "GH2 uinput X-plorer clone", &clone, 4000);
  Check(found, "the core sees the uinput device (SDL's evdev driver opened it)");
  if (!found) {
    for (const auto& d : core.Devices()) std::printf("   seen: '%s'\n", d.name.c_str());
    for (const auto& s : core.Stats().skipped) std::printf("   skipped: %s\n", s.c_str());
    return 1;
  }
  std::printf("   %s %04x:%04x guid %s -> %s (%s: %s), sub-type %u\n", clone.path.c_str(), clone.vendor, clone.product, clone.guid.c_str(), KindName(clone.cls.kind),
              EvidenceName(clone.cls.evidence), clone.cls.detail, clone.cls.xinput_subtype);
  Check(clone.cls.kind == DeviceKind::kGuitar && clone.cls.xinput_subtype == 6, "the X-plorer ids are classified as a guitar, sub-type 6");

  Sample idle;
  core.Read(clone.serial, &idle);
  std::printf("   clone idle state: %s\n", Describe(idle.pad).c_str());
  for (const auto& d : core.Devices()) {
    if (d.serial == clone.serial || d.vendor != clone.vendor || d.product != clone.product) continue;
    Sample real;
    core.Read(d.serial, &real);
    std::printf("   real '%s' idle: %s\n", d.name.c_str(), Describe(real.pad).c_str());
    Check(real.pad == idle.pad, "the clone idles exactly like the real X-plorer (same axes after SDL's mapping)");
  }

  // functional mapping of the guitar controls
  auto expect = [&](const char* what, const std::function<void()>& act, const std::function<bool(const PadState&)>& pred) {
    act();
    const bool ok = WaitState(core, clone.serial, pred, 300);
    Sample s;
    core.Read(clone.serial, &s);
    Check(ok, what, Describe(s.pad));
  };
  using G = UinputGuitar;
  expect("green fret -> A (0x1000)", [&] { guitar.SetButton(G::kGreen, true); }, [](const PadState& p) { return p.buttons == 0x1000; });
  expect("green released", [&] { guitar.SetButton(G::kGreen, false); }, [](const PadState& p) { return p.buttons == 0; });
  expect("red fret -> B (0x2000)", [&] { guitar.SetButton(G::kRed, true); }, [](const PadState& p) { return p.buttons == 0x2000; });
  guitar.SetButton(G::kRed, false);
  expect("yellow fret -> Y (0x8000)", [&] { guitar.SetButton(G::kYellow, true); }, [](const PadState& p) { return (p.buttons & 0x8000) != 0; });
  guitar.SetButton(G::kYellow, false);
  expect("blue fret -> X (0x4000)", [&] { guitar.SetButton(G::kBlue, true); }, [](const PadState& p) { return (p.buttons & 0x4000) != 0; });
  guitar.SetButton(G::kBlue, false);
  expect("orange fret -> left shoulder (0x0100)", [&] { guitar.SetButton(G::kOrange, true); }, [](const PadState& p) { return (p.buttons & 0x0100) != 0; });
  guitar.SetButton(G::kOrange, false);
  expect("strum up -> d-pad up (0x0001)", [&] { guitar.SetStrum(-1); }, [](const PadState& p) { return (p.buttons & 0x0001) != 0; });
  expect("strum down -> d-pad down (0x0002)", [&] { guitar.SetStrum(1); }, [](const PadState& p) { return (p.buttons & 0x0002) != 0; });
  expect("strum released", [&] { guitar.SetStrum(0); }, [](const PadState& p) { return (p.buttons & 3) == 0; });
  expect("start -> 0x0010", [&] { guitar.SetButton(G::kStart, true); }, [](const PadState& p) { return (p.buttons & 0x0010) != 0; });
  guitar.SetButton(G::kStart, false);
  expect("whammy bar full -> right stick X at its maximum", [&] { guitar.SetWhammy(32767); }, [](const PadState& p) { return p.rx == 32767; });
  expect("whammy bar released", [&] { guitar.SetWhammy(-32768); }, [](const PadState& p) { return p.rx == -32768; });

  // latency: random phase relative to the 1 kHz ticks
  std::mt19937 rng(11);
  std::vector<double> seen_us, visible_us;
  Sample s;
  core.Read(clone.serial, &s);
  uint32_t last = s.changes;
  int timeouts = 0;
  const auto t_start = Clock::now();
  for (int i = 0; i < events; ++i) {
    std::this_thread::sleep_for(std::chrono::microseconds(2000 + rng() % 9000));
    const uint64_t t_inj = guitar.SetButton(G::kGreen, (i & 1) == 0);
    uint64_t t_vis = 0;
    while (InputCore::NowNs() - t_inj < 100'000'000ull) {
      core.Read(clone.serial, &s);
      if (s.changes != last) {
        t_vis = InputCore::NowNs();
        break;
      }
    }
    if (!t_vis) {
      ++timeouts;
      continue;
    }
    last = s.changes;
    seen_us.push_back((static_cast<double>(s.first_seen_ns) - static_cast<double>(t_inj)) / 1000.0);
    visible_us.push_back((static_cast<double>(t_vis) - static_cast<double>(t_inj)) / 1000.0);
  }
  const double secs = std::chrono::duration<double>(Clock::now() - t_start).count();
  const CoreStats st = core.Stats();
  std::printf("   %zu presses/releases through /dev/uinput -> evdev -> SDL -> thread (%d Hz):\n", seen_us.size(), hz);
  std::printf("     first seen by the thread : p50 %6.0f us  p90 %6.0f us  p99 %6.0f us  max %6.0f us\n", Percentile(seen_us, 50), Percentile(seen_us, 90),
              Percentile(seen_us, 99), Percentile(seen_us, 100));
  std::printf("     readable by the guest    : p50 %6.0f us  p90 %6.0f us  p99 %6.0f us  max %6.0f us\n", Percentile(visible_us, 50), Percentile(visible_us, 90),
              Percentile(visible_us, 99), Percentile(visible_us, 100));
  std::printf("     polling thread: %.2f%% of one core, %llu ticks, %llu overruns, longest tick %.0f us\n", 100.0 * (st.thread_cpu_ns / 1e9) / secs,
              static_cast<unsigned long long>(st.ticks), static_cast<unsigned long long>(st.overruns), st.max_tick_ns / 1000.0);
  Check(timeouts == 0, "every injected change reached the snapshot", std::to_string(timeouts) + " timeouts");
  Check(Percentile(visible_us, 99) < 4000, "p99 under 4 ms from the kernel write to a readable snapshot");
  core.Stop();
  InputCore::QuitSdl();
  return g_failures ? 1 : 0;
}

int Inject(int hz, double seconds, const char* log_path, double delay_s, uint16_t product, UinputGuitar::Button button) {
  UinputGuitar guitar;
  if (!guitar.Create("GH2 Injector Guitar", 0x1430, product, 0x3122)) return std::fprintf(stderr, "uinput: %s\n", guitar.error().c_str()), 2;
  std::ofstream log(log_path);
  log << "# steady_clock ns of each write(), then the state written (1 = fret down)\n";
  log.flush();
  std::fprintf(stderr, "injector ready (%s); starts in %.1f s\n", guitar.event_path().c_str(), delay_s);
  std::this_thread::sleep_for(std::chrono::duration<double>(delay_s));
  const auto period = std::chrono::microseconds(1'000'000 / std::max(hz, 1));
  const auto t0 = Clock::now();
  auto next = t0;
  bool down = false;
  int n = 0;
  std::mt19937 rng(5);
  while (std::chrono::duration<double>(Clock::now() - t0).count() < seconds) {
    // random phase against the game's frames: period + up to 7 ms jitter
    next += period + std::chrono::microseconds(rng() % 7000);
    std::this_thread::sleep_until(next);
    down = !down;
    const uint64_t t = guitar.SetButton(button, down);
    log << t << ' ' << (down ? 1 : 0) << '\n';
    log.flush();
    ++n;
  }
  guitar.SetButton(button, false);
  std::fprintf(stderr, "injected %d events\n", n);
  return 0;
}

// A crude player for performance tests (the effects of hitting notes are not on screen when nobody plays): every period it presses one to two random frets
// (green, red and yellow more often) and strums down, then lets go.
int Play(int hz, double seconds, double delay_s) {
  UinputGuitar guitar;
  if (!guitar.Create("GH2 Injector Guitar", 0x1430, 0x4749, 0x3122)) return std::fprintf(stderr, "uinput: %s\n", guitar.error().c_str()), 2;
  std::fprintf(stderr, "player ready (%s); starts in %.1f s\n", guitar.event_path().c_str(), delay_s);
  std::this_thread::sleep_for(std::chrono::duration<double>(delay_s));
  const auto period = std::chrono::microseconds(1'000'000 / std::max(hz, 1));
  const auto t0 = Clock::now();
  auto next = t0;
  std::mt19937 rng(11);
  static const UinputGuitar::Button frets[8] = {UinputGuitar::kGreen,  UinputGuitar::kGreen, UinputGuitar::kRed,    UinputGuitar::kRed,
                                                UinputGuitar::kYellow, UinputGuitar::kYellow, UinputGuitar::kBlue, UinputGuitar::kOrange};
  int n = 0;
  while (std::chrono::duration<double>(Clock::now() - t0).count() < seconds) {
    next += period;
    std::this_thread::sleep_until(next);
    const UinputGuitar::Button a = frets[rng() % 8], b = frets[rng() % 8];
    guitar.SetButton(a, true);
    if (b != a && (rng() % 3) == 0) guitar.SetButton(b, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(12));
    guitar.SetStrum(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(45));
    guitar.SetStrum(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    guitar.SetButton(a, false);
    guitar.SetButton(b, false);
    ++n;
  }
  std::fprintf(stderr, "played %d strums\n", n);
  return 0;
}

}  // namespace

// Scripted taps for menu navigation in tests: "SECONDS:name,..." with name = green|red|yellow|blue|orange|start|back|down|up (a tap lasts 120 ms); then the process stays alive for HOLD_S seconds.
int Script(const std::string& steps, double delay_s, double hold_s) {
  UinputGuitar guitar;
  if (!guitar.Create("GH2 Injector Guitar", 0x1430, 0x4749, 0x3122)) return std::fprintf(stderr, "uinput: %s\n", guitar.error().c_str()), 2;
  std::fprintf(stderr, "script ready (%s)\n", guitar.event_path().c_str());
  struct Step { double at; std::string name; };
  std::vector<Step> list;
  size_t pos = 0;
  while (pos < steps.size()) {
    size_t end = steps.find(',', pos);
    if (end == std::string::npos) end = steps.size();
    const std::string item = steps.substr(pos, end - pos);
    pos = end + 1;
    const size_t colon = item.find(':');
    if (colon != std::string::npos) list.push_back({std::atof(item.c_str()), item.substr(colon + 1)});
  }
  std::sort(list.begin(), list.end(), [](const Step& a, const Step& b) { return a.at < b.at; });
  const auto t0 = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(delay_s));
  for (const Step& st : list) {
    std::this_thread::sleep_until(t0 + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(st.at)));
    if (st.name == "chord") {   // START + SELECT held for a second (opens / closes the options menu)
      guitar.SetButton(UinputGuitar::kStart, true);
      guitar.SetButton(UinputGuitar::kBack, true);
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      guitar.SetButton(UinputGuitar::kStart, false);
      guitar.SetButton(UinputGuitar::kBack, false);
    } else if (st.name == "down" || st.name == "up") {
      guitar.SetStrum(st.name == "down" ? 1 : -1);
      std::this_thread::sleep_for(std::chrono::milliseconds(120));
      guitar.SetStrum(0);
    } else {
      UinputGuitar::Button b = st.name == "red" ? UinputGuitar::kRed : st.name == "yellow" ? UinputGuitar::kYellow : st.name == "blue" ? UinputGuitar::kBlue
                              : st.name == "orange" ? UinputGuitar::kOrange : st.name == "start" ? UinputGuitar::kStart : st.name == "back" ? UinputGuitar::kBack : UinputGuitar::kGreen;
      guitar.SetButton(b, true);
      std::this_thread::sleep_for(std::chrono::milliseconds(120));
      guitar.SetButton(b, false);
    }
    std::fprintf(stderr, "tap %s at %.1f s\n", st.name.c_str(), st.at);
  }
  std::this_thread::sleep_for(std::chrono::duration<double>(hold_s));
  return 0;
}

int main(int argc, char** argv) {
  const std::string cmd = argc > 1 ? argv[1] : "";
  if (cmd == "unit") {
    TestClassifier();
    TestSlotStress();
    TestCoreWithVirtualDevices();
    std::printf("selftest: %s\n", g_failures ? "FAILED" : "PASSED");
    return g_failures ? 1 : 0;
  }
  if (cmd == "list") return List(argc > 2 ? std::atof(argv[2]) : 0);
  if (cmd == "e2e") {
    const int rc = E2E(argc > 2 ? std::atoi(argv[2]) : 600, argc > 3 ? std::atoi(argv[3]) : 1000);
    std::printf("e2e: %s\n", rc ? "FAILED" : "PASSED");
    return rc;
  }
  if (cmd == "script" && argc >= 5) return Script(argv[4], std::atof(argv[2]), std::atof(argv[3]));
  if (cmd == "play" && argc >= 4) return Play(std::atoi(argv[2]), std::atof(argv[3]), argc > 4 ? std::atof(argv[4]) : 3.0);
  if (cmd == "inject" && argc >= 5)
    return Inject(std::atoi(argv[2]), std::atof(argv[3]), argv[4], argc > 5 ? std::atof(argv[5]) : 3.0,
                  argc > 6 ? static_cast<uint16_t>(std::strtoul(argv[6], nullptr, 16)) : 0x4749, UinputGuitar::kOrange);
  std::fprintf(stderr, "usage: gh2_input_selftest unit | list [SECONDS] | e2e [EVENTS] [HZ] | inject HZ SECONDS LOGFILE [DELAY_S] [PRODUCT_HEX] | play HZ SECONDS [DELAY_S] | script DELAY_S HOLD_S \"T:name,...\"\n");
  return 2;
}
