// GH2 host-side hooks on ReXGlue nightly 0.10.0.24: input fixes, intro skip/autostart (test aid), spin hints, traces.

#include "generated/default/gh2_init.h"

// Xplorer fix: the game must see a guitar (XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE) and the triggers must be
// idle. Set GH2_INPUT_TRACE=1 to log pad changes. Wraps SDK imports via --wrap (CMakeLists.txt).
#include <rex/chrono/clock.h>
#include <rex/cvar.h>

#include <atomic>
#include <bit>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

bool Gh2UserIsGuitar(uint32_t user);  // src/controllers.cpp
#include "controllers.h"
#include "fps_meter.h"
#include "keyboard.h"
REX_EXTERN(__real___imp__XamInputGetState);
REX_EXTERN(__real___imp__XamInputGetCapabilities);

static bool InputTrace() {
  static const bool on = getenv("GH2_INPUT_TRACE") != nullptr;
  return on;
}

// Intro skipping (GH2_SKIP_INTRO=1): the splash screens and the intro video run at ~30 ms per guest frame
// (30 fps), the menus at 16.7 ms. While the guest is still in that slow phase, press A in short pulses so
// every skippable screen is dismissed; stop as soon as the guest runs at menu speed (see VdSwap below),
// or after GH2_SKIP_INTRO_MAX seconds (default 60) as a safety net.
static const std::chrono::steady_clock::time_point g_process_t0 = std::chrono::steady_clock::now();
static double SinceStart() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_process_t0).count();
}
// ---- Game options (cvars): command line `--skip_intro`, env `REX_SKIP_INTRO=1`, or ~/.config/gh2/config.toml ----
REXCVAR_DEFINE_BOOL(skip_intro, false, "GH2",
                    "Skip the splash screens and the intro video; the game stops at the title screen");
REXCVAR_DEFINE_STRING(startup_mode, "normal", "GH2",
                      "Start-up: normal (logos and intro video; any button skips the logos) or menu (skip straight to the "
                      "main menu)")
    .allowed({"normal", "menu"});
REXCVAR_DEFINE_BOOL(autostart, false, "GH2",
                    "Testing aid: after the intro keep pressing A so the menus start a Quick Play song");
REXCVAR_DEFINE_INT32(autostart_seconds, 30, "GH2", "How long the autostart testing aid keeps pressing A");

// Tiny reader for "key = true|false|<number>" lines of the user config file (XDG: ~/.config/gh2/config.toml).
static bool ConfigFileValue(const char* key, std::string* out) {
  static const std::string text = [] {
    std::string path;
    if (const char* x = getenv("XDG_CONFIG_HOME"); x && *x) path = std::string(x) + "/gh2/config.toml";
    else if (const char* h = getenv("HOME"); h && *h) path = std::string(h) + "/.config/gh2/config.toml";
    std::string t;
    if (FILE* f = path.empty() ? nullptr : fopen(path.c_str(), "r")) {
      char buf[256];
      while (fgets(buf, sizeof(buf), f)) t += buf;
      fclose(f);
    }
    return t;
  }();
  size_t pos = 0;
  while (pos < text.size()) {
    size_t eol = text.find('\n', pos);
    if (eol == std::string::npos) eol = text.size();
    std::string line = text.substr(pos, eol - pos);
    pos = eol + 1;
    const size_t hash = line.find('#');
    if (hash != std::string::npos) line.resize(hash);
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    auto trim = [](std::string v) {
      const char* ws = " \t\r";
      v.erase(0, v.find_first_not_of(ws) == std::string::npos ? v.size() : v.find_first_not_of(ws));
      v.erase(v.find_last_not_of(ws) == std::string::npos ? 0 : v.find_last_not_of(ws) + 1);
      return v;
    };
    if (trim(line.substr(0, eq)) == key) {
      *out = trim(line.substr(eq + 1));
      return true;
    }
  }
  return false;
}
static bool ConfigFileBool(const char* key) {
  std::string v;
  return ConfigFileValue(key, &v) && (v == "true" || v == "1" || v == "yes");
}

// SDL installs SIGINT/SIGTERM handlers that only post an SDL_QUIT event, which this windowed app never reads, so
// `kill` and Ctrl+C did nothing. Ask SDL not to install them (must be set before SDL initialises).
__attribute__((constructor)) static void Gh2DisableSdlSignalHandlers() {
  setenv("SDL_NO_SIGNAL_HANDLERS", "1", 0);
}

static std::atomic<bool> g_intro_done{false};
static std::atomic<uint32_t> g_intro_video_handle{0};
static std::atomic<double> g_video_open_s{-1.0};
static std::atomic<double> g_menu_reached_s{-1.0};  // SinceStart() when the guest first ran at menu speed
// GH2_AUTOSTART=1 (testing aid, implies GH2_SKIP_INTRO): keep pulsing A for GH2_AUTOSTART_SECONDS (default 30)
// after the menus appear, which walks Quick Play -> song -> difficulty until the song loads.
static bool AutostartEnabled() {
  static const bool on = [] {
    const char* e = getenv("GH2_AUTOSTART");
    return REXCVAR_GET(autostart) || (e && e[0] != '0') || ConfigFileBool("autostart");
  }();
  return on;
}
// The splash screens are busy-wait loops on the guest timebase (mftb): "spin until N seconds elapsed" (see
// loc_82120378 in sub_82120208). Running guest time faster shortens them without touching game code.
static std::atomic<bool> g_fast_forward{false};
static void FastForwardEnd(const char* why) {
  if (g_fast_forward.exchange(false)) {
    rex::chrono::Clock::set_guest_time_scalar(1.0);
    fprintf(stderr, "T+%.1fs intro fast-forward off (%s)\n", SinceStart(), why);
  }
}
static std::atomic<uint32_t> g_intro_poll_count{0};
static std::atomic<double> g_ff_started_s{0.0};
static void FastForwardBegin(double speed, const char* why) {
  if (g_fast_forward.exchange(true)) return;
  g_ff_started_s = SinceStart();
  rex::chrono::Clock::set_guest_time_scalar(speed);
  fprintf(stderr, "T+%.1fs intro fast-forward x%.0f (%s)\n", SinceStart(), speed, why);
}
static bool SkipIntroEnabled() {
  static const bool on = [] {
    const char* e = getenv("GH2_SKIP_INTRO");
    return REXCVAR_GET(skip_intro) || (e && e[0] != '0') || ConfigFileBool("skip_intro") || AutostartEnabled() ||
           REXCVAR_GET(startup_mode) != "normal";
  }();
  return on;
}
static bool IntroSkipActive() {
  if (!SkipIntroEnabled()) return false;
  if (g_intro_done.load(std::memory_order_relaxed)) {
    // Past the intro: only the autostart testing aid keeps pressing, for a limited time.
    static const double autostart_s = [] {
      const char* e = getenv("GH2_AUTOSTART_SECONDS");
      return e ? atof(e) : static_cast<double>(REXCVAR_GET(autostart_seconds));
    }();
    const double reached = g_menu_reached_s.load();
    // startup_mode = menu: one short press of A at the title screen takes the player to the main menu.
    static const bool to_menu = REXCVAR_GET(startup_mode) == "menu";
    if (to_menu && reached >= 0 && SinceStart() - reached >= 1.2 && SinceStart() - reached < 1.7) return true;
    return AutostartEnabled() && reached >= 0 && SinceStart() - reached < autostart_s;
  }
  static const auto start = std::chrono::steady_clock::now();
  static const double max_s = [] {
    const char* e = getenv("GH2_SKIP_INTRO_MAX");
    return e ? atof(e) : 60.0;
  }();
  if (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() > max_s) {
    g_intro_done = true;
    return false;
  }
  return true;
}

REX_EXTERN(__wrap___imp__XamInputGetState) {
  static const bool timeline = getenv("GH2_TIMELINE") != nullptr;
  if (timeline) {
    static std::atomic<bool> first{true};
    if (first.exchange(false)) fprintf(stderr, "T+%.1fs first XamInputGetState\n", SinceStart());
  }
  uint32_t sp = ctx.r5.u32;
  const uint32_t user = ctx.r3.u32;
  __real___imp__XamInputGetState(ctx, base);
  // Keyboard play: the keyboard is a guitar-style device for one player (src/keyboard.cpp). It is merged into that
  // player's state, or becomes the player's whole state when no device is connected there.
  const Gh2KbState kb = Gh2KeyboardPoll(user);
  if (kb.active) {
    if (ctx.r3.u32 != 0) {
      for (uint32_t i = 0; i < 16; ++i) REX_STORE_U8(sp + i, 0);
      ctx.r3.u64 = 0;
    }
    REX_STORE_U32(sp, REX_LOAD_U32(sp) + kb.changes);
    REX_STORE_U16(sp + 4, REX_LOAD_U16(sp + 4) | kb.buttons);
    if (kb.whammy) REX_STORE_U16(sp + 12, 0x7FFF);  // right stick X = whammy axis on guitars
  }
  if (ctx.r3.u32 != 0) return;
  // A guitar's unused axes read as pressed triggers (the X-plorer reports LT/RT garbage), so they are zeroed for
  // guitars only; a gamepad's triggers are real buttons (green / blue in the game's gamepad mode).
  if (Gh2UserIsGuitar(user)) {
    REX_STORE_U8(sp + 6, 0);
    REX_STORE_U8(sp + 7, 0);
  }
  if (InputTrace()) {
    static std::atomic<uint32_t> calls{0};
    static auto t0 = std::chrono::steady_clock::now();
    const uint32_t c = ++calls;
    if (c == 1 || c % 200 == 0)
      fprintf(stderr, "GETSTATE call #%u at %.1fs\n", c,
              std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  }
  if (IntroSkipActive()) {
    // 4 polls pressed, 4 released: the game sees clean button-down edges.
    const uint32_t n = g_intro_poll_count.fetch_add(1, std::memory_order_relaxed);
    static const uint16_t mask = [] {
      const char* e = getenv("GH2_SKIP_INTRO_BUTTONS");  // XINPUT wButtons bits; default A
      return e ? static_cast<uint16_t>(strtoul(e, nullptr, 0)) : static_cast<uint16_t>(0x1000);
    }();
    // Games usually re-read the pad only when dwPacketNumber changes, so bump it on every injected edge.
    static std::atomic<uint32_t> bias{0};
    static std::atomic<int> last_pressed{-1};
    static const uint32_t period = [] {  // polls per pressed/released phase; 0 = hold pressed
      const char* e = getenv("GH2_SKIP_INTRO_PERIOD");
      return e ? static_cast<uint32_t>(strtoul(e, nullptr, 10)) : 4u;
    }();
    const int pressed = period == 0 ? 1 : (((n / period) & 1) == 0);
    if (last_pressed.exchange(pressed) != pressed) ++bias;
    REX_STORE_U32(sp, REX_LOAD_U32(sp) + bias.load());
    if (pressed) REX_STORE_U16(sp + 4, REX_LOAD_U16(sp + 4) | mask);
  }
  if (!InputTrace()) return;
  static uint32_t last_btn = 0xFFFFFFFF;
  static uint64_t last_axes = ~0ull;
  uint32_t btn = REX_LOAD_U16(sp + 4);
  uint64_t axes = ((uint64_t)REX_LOAD_U16(sp + 8) << 32) | ((uint64_t)REX_LOAD_U16(sp + 10) << 16) |
                  REX_LOAD_U16(sp + 12);
  axes = (axes << 16) ^ REX_LOAD_U16(sp + 14);
  if (btn != last_btn || axes != last_axes) {
    last_btn = btn;
    last_axes = axes;
    fprintf(stderr, "PAD btn=%04x LX=%d LY=%d RX=%d RY=%d\n", btn, (int16_t)REX_LOAD_U16(sp + 8),
            (int16_t)REX_LOAD_U16(sp + 10), (int16_t)REX_LOAD_U16(sp + 12),
            (int16_t)REX_LOAD_U16(sp + 14));
  }
}

// Stall tracing (GH2_STALL_TRACE=1): the per-frame input poll is a heartbeat of the guest main thread.
// If it stops for >30 ms a watchdog samples the main thread's native stack (raw addresses; resolve
// with scripts in work/analysis against the binary's symbols).
#include <execinfo.h>
#include <link.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <thread>

static std::atomic<int64_t> g_beat_ms{0};
static pthread_t g_main_thread;
static std::atomic<bool> g_watchdog_started{false};

static int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

static void StallSampleHandler(int) {
  void* frames[96];
  int n = backtrace(frames, 96);
  char line[2048];
  char tname[32] = {};
  prctl(PR_GET_NAME, tname, 0, 0, 0);
  for (char* c = tname; *c; ++c) if (*c == ' ') *c = '_';
  int len = snprintf(line, sizeof(line), "STALLBT t=%lld th=%s", (long long)NowMs(), tname);
  for (int i = 0; i < n && len < 2000; i++) len += snprintf(line + len, sizeof(line) - len, " %p", frames[i]);
  line[len++] = '\n';
  (void)!write(2, line, len);
}

static void StallWatchdog() {
  int samples = 0;
  // GH2_PROFILE=1: sample the main thread continuously (every ~2 ms) instead of only during stalls.
  static const bool profile = getenv("GH2_PROFILE") != nullptr;
  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(profile ? 2 : 8));
    int64_t gap = NowMs() - g_beat_ms.load();
    if (profile) {
      // GH2_PROFILE_THREAD=<name prefix>: sample that thread (e.g. "Audio") instead of the guest main thread.
      static const char* want = getenv("GH2_PROFILE_THREAD");
      if (want) {
        if (DIR* d = opendir("/proc/self/task")) {
          while (dirent* e = readdir(d)) {
            const pid_t tid = static_cast<pid_t>(atoi(e->d_name));
            if (tid <= 0) continue;
            char path[64], comm[32] = {};
            snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
            if (FILE* f = fopen(path, "r")) {
              if (fgets(comm, sizeof(comm), f)) {}
              fclose(f);
            }
            if (strncmp(comm, want, strlen(want)) == 0) syscall(SYS_tgkill, getpid(), tid, SIGUSR2);
          }
          closedir(d);
        }
      } else {
        pthread_kill(g_main_thread, SIGUSR2);
      }
      continue;
    }
    // GH2_STALL_TRAP=MIN:MAX (seconds since start): when a frame gap >40 ms happens in that window, raise SIGTRAP
    // once so a debugger can stop the whole process at the hitch (run under gdb with debuginfod for driver symbols).
    static const char* trap_env = getenv("GH2_STALL_TRAP");
    static bool trapped = false;
    if (trap_env && !trapped && gap > 40) {
      double lo = 0, hi = 1e9;
      sscanf(trap_env, "%lf:%lf", &lo, &hi);
      const double t = SinceStart();
      if (t >= lo && t <= hi) {
        trapped = true;
        raise(SIGTRAP);
      }
    }
    if (gap > 30) {
      if (samples < 40) {
        static const bool all_threads = getenv("GH2_STALL_ALL") != nullptr;
        if (all_threads) {
          // Sample every thread of the process (not the watchdog itself): who is the main thread waiting for?
          if (samples < 6) {
            const pid_t self = static_cast<pid_t>(syscall(SYS_gettid));
            if (DIR* d = opendir("/proc/self/task")) {
              while (dirent* e = readdir(d)) {
                const pid_t tid = static_cast<pid_t>(atoi(e->d_name));
                if (tid > 0 && tid != self) syscall(SYS_tgkill, getpid(), tid, SIGUSR2);
              }
              closedir(d);
            }
          }
        } else {
          pthread_kill(g_main_thread, SIGUSR2);
        }
        samples++;
      }
    } else {
      samples = 0;
    }
  }
}

static void Heartbeat() {
  static const bool on = getenv("GH2_STALL_TRACE") != nullptr || getenv("GH2_PROFILE") != nullptr;
  if (!on) return;
  g_beat_ms.store(NowMs());
  if (!g_watchdog_started.exchange(true)) {
    g_main_thread = pthread_self();
    signal(SIGUSR2, StallSampleHandler);
    // Map base of the executable, to resolve PIE addresses offline.
    fprintf(stderr, "STALLBASE %p\n", (void*)&Heartbeat);
    dl_iterate_phdr(  // load bases of the SDK libraries, to resolve their frames offline
        [](dl_phdr_info* info, size_t, void*) {
          if (info->dlpi_name && strstr(info->dlpi_name, ".so"))
            fprintf(stderr, "LIBBASE %s 0x%lx\n", info->dlpi_name, static_cast<unsigned long>(info->dlpi_addr));
          return 0;
        },
        nullptr);
    std::thread(StallWatchdog).detach();
  }
}

REX_EXTERN(__wrap___imp__XamInputGetCapabilities) {
  Heartbeat();
  uint32_t cp = ctx.r5.u32;
  const uint32_t cap_user = ctx.r3.u32;
  __real___imp__XamInputGetCapabilities(ctx, base);
  if (Gh2KeyboardSlot() >= 0 && static_cast<uint32_t>(Gh2KeyboardSlot()) == cap_user && !Gh2UserHasRealDevice(cap_user)) {
    // No real device at the keyboard's slot (the SDK may still offer its "None" stand-in, which is a plain gamepad and would
    // put the game in gamepad mode, with other fret buttons): report a guitar (type 1 = gamepad class, sub-type 7) with every
    // control present.
    for (uint32_t i = 0; i < 20; ++i) REX_STORE_U8(cp + i, 0);
    REX_STORE_U8(cp, 1);
    REX_STORE_U8(cp + 1, 7);
    REX_STORE_U16(cp + 4, 0xF3FF);
    REX_STORE_U8(cp + 6, 0xFF);
    REX_STORE_U8(cp + 7, 0xFF);
    for (uint32_t i = 8; i < 16; ++i) REX_STORE_U8(cp + i, 0xFF);
    ctx.r3.u64 = 0;
  }
  // The game's pad routine only treats a device as a guitar when SubType == 7 (GUITAR_ALTERNATE). The SDK reports real
  // guitars as 6 (or 7 / 11 for bass); present all of them as 7. Gamepads (subtype 1) stay gamepads.
  if (ctx.r3.u32 == 0 && REX_LOAD_U8(cp) == 1) {
    const uint8_t st = REX_LOAD_U8(cp + 1);
    if (st == 0x06 || st == 0x07 || st == 0x0B) REX_STORE_U8(cp + 1, 7);
  }
  if (InputTrace()) {  // what the game is told about each player's device (capabilities struct, 20 bytes)
    static std::atomic<int> printed[4];
    if (cap_user < 4 && printed[cap_user].fetch_add(1) < 2) {
      char hex[64] = {};
      for (uint32_t i = 0; i < 20; ++i) snprintf(hex + i * 3, 4, "%02x ", REX_LOAD_U8(cp + i));
      fprintf(stderr, "CAPS user=%u result=%u %s\n", cap_user, ctx.r3.u32, hex);
    }
  }
}

// Allocation timing (GH2_ALLOC_TRACE=1): cost of MmAllocatePhysicalMemoryEx (mmap/mprotect per call).
REX_EXTERN(__real___imp__MmAllocatePhysicalMemoryEx);

REX_EXTERN(__wrap___imp__MmAllocatePhysicalMemoryEx) {
  static const bool on = getenv("GH2_ALLOC_TRACE") != nullptr;
  if (!on) {
    __real___imp__MmAllocatePhysicalMemoryEx(ctx, base);
    return;
  }
  static std::atomic<uint64_t> calls{0}, total_us{0}, max_us{0}, bytes{0};
  auto t0 = std::chrono::steady_clock::now();
  __real___imp__MmAllocatePhysicalMemoryEx(ctx, base);
  uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - t0).count();
  uint64_t n = ++calls;
  total_us += us;
  bytes += ctx.r5.u32;
  if (us > max_us) max_us = us;
  if (n % 500 == 0 || us > 20000)
    fprintf(stderr, "ALLOC n=%llu total=%.1fms max=%.2fms last=%lluus size_arg=%x\n",
            (unsigned long long)n, total_us / 1000.0, max_us / 1000.0, (unsigned long long)us,
            ctx.r5.u32);
}

// Guest frame pacing (GH2_FRAME_TRACE=1): intervals between the game's VdSwap calls, i.e. the frame rate
// the game really runs at (MangoHud only sees the host presenter, which repeats frames when the guest is late).
#include <algorithm>
#include <ctime>
#include <vector>

REX_EXTERN(__real___imp__VdSwap);

// Always-on, nearly free: when the game itself (not the host presenter) produces fewer than ~55 frames in a second, or one
// frame takes over 50 ms, a line with the wall-clock time goes to logs/perf.log. The SDK's own log has timestamps too
// ("Achievement unlocked", ...), so a slowdown can be matched with what the game was doing.
static void GuestFrameMonitor() {
  using clock = std::chrono::steady_clock;
  static clock::time_point window_start{}, last{};
  static int frames = 0;
  static double worst_ms = 0.0;
  const auto now = clock::now();
  if (window_start == clock::time_point{}) {
    window_start = last = now;
    return;
  }
  const double iv = std::chrono::duration<double, std::milli>(now - last).count();
  last = now;
  Gh2FpsNoteFrame(iv);
  ++frames;
  worst_ms = std::max(worst_ms, iv);
  const double elapsed = std::chrono::duration<double>(now - window_start).count();
  if (elapsed < 1.0) return;
  const double fps = frames / elapsed;
  if (SinceStart() > 20.0 && (fps < 55.0 || worst_ms > 50.0)) {  // not during start-up and logos
    if (FILE* f = fopen("logs/perf.log", "a")) {
      const std::time_t t = std::time(nullptr);
      char stamp[32];
      std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
      fprintf(f, "[%s] game ran at %.1f fps over %.1f s, worst frame %.0f ms\n", stamp, fps, elapsed, worst_ms);
      fclose(f);
    }
  }
  window_start = now;
  frames = 0;
  worst_ms = 0.0;
}

REX_EXTERN(__wrap___imp__VdSwap) {
  Heartbeat();
  GuestFrameMonitor();
  static const bool dump_cvars = getenv("GH2_DUMP_CVARS") != nullptr;
  if (dump_cvars) {
    static std::atomic<bool> done{false};
    if (!done.exchange(true)) {
      for (const auto& f : rex::cvar::GetRegistry())
        fprintf(stderr, "CVAR [%s] %s = %s (default %s) -- %s\n", f.category.c_str(), f.name.c_str(), f.getter().c_str(),
                f.default_value.c_str(), f.description.c_str());
    }
  }  // also a frame heartbeat (the logos phase polls no input)
  static const bool timeline = getenv("GH2_TIMELINE") != nullptr;
  if (timeline) {
    static std::atomic<uint32_t> swaps{0};
    const uint32_t n = ++swaps;
    if (n == 1 || n == 60 || n == 600) fprintf(stderr, "T+%.1fs VdSwap #%u\n", SinceStart(), n);
  }
  if (SkipIntroEnabled()) {
    static std::atomic<bool> started{false};
    if (!started.exchange(true)) {
      const char* e = getenv("GH2_SKIP_INTRO_SPEED");
      const double speed = e ? atof(e) : 16.0;
      if (speed > 1.0) FastForwardBegin(speed, "startup mode");
    }
  }
  {
    static const double max_ff = [] {
      const char* e = getenv("GH2_SKIP_INTRO_FF_MAX");
      return e ? atof(e) : 8.0;  // safety net: never fast-forward longer than this many real seconds
    }();
    if (g_fast_forward.load() && SinceStart() - g_ff_started_s.load() > max_ff) FastForwardEnd("timeout");
  }
  if (SkipIntroEnabled() && !g_intro_done.load(std::memory_order_relaxed)) {
    // Menu speed = guest frames at ~16.7 ms. Require a run of fast frames after a slow phase was seen.
    using clock = std::chrono::steady_clock;
    static clock::time_point last{};
    static int slow_frames = 0, fast_run = 0;
    const auto now = clock::now();
    if (last.time_since_epoch().count() != 0) {
      const double ms = std::chrono::duration<double, std::milli>(now - last).count();
      const bool video_open = g_video_open_s.load() >= 0;
      if (ms > 24.0) {
        if (video_open) ++slow_frames;  // slow frames of the video itself (the logos before it don't count)
        fast_run = 0;
      } else if (ms < 20.0) {
        ++fast_run;
      }
      if (slow_frames >= 3 && fast_run >= 12) {  // ~0.2 s of menu-speed frames: the menus are up, stop pressing
        g_menu_reached_s = SinceStart();
        g_intro_done = true;
        static const auto t_start = clock::now();
        fprintf(stderr, "T+%.1fs INTRO done: guest reached menu speed\n", SinceStart());
      }
    }
    last = now;
  }
  static const bool on = getenv("GH2_FRAME_TRACE") != nullptr;
  if (on) {
    using clock = std::chrono::steady_clock;
    static clock::time_point last{}, window_start = clock::now();
    static std::vector<double> iv;
    auto now = clock::now();
    if (last.time_since_epoch().count() != 0) {
      iv.push_back(std::chrono::duration<double, std::milli>(now - last).count());
      static const bool timeline_hitch = getenv("GH2_TIMELINE") != nullptr;
      if (timeline_hitch && iv.back() > 60.0) fprintf(stderr, "T+%.2fs HITCH guest frame interval %.0f ms\n", SinceStart(), iv.back());
    }
    last = now;
    static FILE* dump = [] { const char* p = getenv("GH2_FRAME_DUMP"); return p ? fopen(p, "w") : nullptr; }();
    if (dump && !iv.empty()) fprintf(dump, "%.4f %.2f\n", SinceStart(), iv.back());
    if (now - window_start >= std::chrono::seconds(2) && !iv.empty()) {
      std::vector<double> sorted = iv;
      std::sort(sorted.begin(), sorted.end());
      double sum = 0;
      int over20 = 0, over25 = 0, over33 = 0;
      for (double v : iv) {
        sum += v;
        over20 += v > 20;
        over25 += v > 25;
        over33 += v > 33.4;
      }
      fprintf(stderr, "SWAP n=%zu mean=%.2f p95=%.2f max=%.1f >20ms=%d >25ms=%d >33ms=%d\n", iv.size(),
              sum / iv.size(), sorted[size_t(sorted.size() * 0.95)], sorted.back(), over20, over25,
              over33);
      iv.clear();
      window_start = now;
    }
  }
  __real___imp__VdSwap(ctx, base);
}

// Spin-wait hook. sub_823C6B50 is the game's wait-with-timeout step (8x db16cyc delay, then a flag/owner
// check; returns 1 = keep waiting). db16cyc is a nop in the recompiled code, so the main thread polls at
// full speed and keeps one core at ~100% (78% of its samples, GH2_PROFILE). Real PAUSE hints give the
// core's SMT sibling and the package power budget back without adding wake-up latency (no sleeping).
// GH2_SPIN_PAUSE=0 disables the hook's pause.
#include <immintrin.h>
#include <sched.h>

REX_EXTERN(__imp__sub_823C6B50);

REX_EXTERN(sub_823C6B50) {
  static const bool pause_on = [] {
    const char* e = getenv("GH2_SPIN_PAUSE");
    return !(e && e[0] == '0');
  }();
  __imp__sub_823C6B50(ctx, base);
  if (pause_on && ctx.r3.u32 == 1) {
    for (int i = 0; i < 16; ++i) _mm_pause();
  }
}

// Intro video skipping (GH2_SKIP_INTRO_VIDEO=1): make d:\videos\intro.wmv "not found" so the game moves on
// without playing it. Experimental: the game's reaction to a missing video is what this tests.
REX_EXTERN(__real___imp__NtCreateFile);

REX_EXTERN(__wrap___imp__NtCreateFile) {
  static const bool hide_video = [] {
    const char* e = getenv("GH2_SKIP_INTRO_VIDEO");
    return e && e[0] != '0';
  }();
  if (hide_video || g_fast_forward.load()) {
    // r5 = OBJECT_ATTRIBUTES { root_dir, name_ptr -> ANSI_STRING { u16 len, u16 max, u32 buffer }, attrs }
    const uint32_t oa = ctx.r5.u32;
    const uint32_t name_ptr = REX_LOAD_U32(oa + 4);
    if (name_ptr) {
      const uint32_t len = REX_LOAD_U16(name_ptr);
      const uint32_t buf = REX_LOAD_U32(name_ptr + 4);
      char path[160] = {};
      for (uint32_t i = 0; i < len && i < sizeof(path) - 1; ++i) path[i] = static_cast<char>(REX_LOAD_U8(buf + i));
      if (strstr(path, "intro.wmv")) {
        FastForwardEnd("intro video reached");  // the video must play (or be skipped) at normal speed
        if (hide_video) {
          fprintf(stderr, "T+%.1fs SKIP: hiding %s\n", SinceStart(), path);
          ctx.r3.u64 = 0xC0000034u;  // STATUS_OBJECT_NAME_NOT_FOUND
          return;
        }
      }
    }
  }
  const uint32_t handle_ptr = ctx.r3.u32;
  const uint32_t oa2 = ctx.r5.u32;
  __real___imp__NtCreateFile(ctx, base);
  if (SkipIntroEnabled() || g_fast_forward.load() || g_video_open_s.load() < 0 || getenv("GH2_TIMELINE")) {
    const uint32_t np = REX_LOAD_U32(oa2 + 4);
    if (np && ctx.r3.u32 == 0) {
      const uint32_t len = REX_LOAD_U16(np);
      const uint32_t buf = REX_LOAD_U32(np + 4);
      char path[160] = {};
      for (uint32_t i = 0; i < len && i < sizeof(path) - 1; ++i) path[i] = static_cast<char>(REX_LOAD_U8(buf + i));
      if (strstr(path, "intro.wmv")) {
        g_intro_video_handle = REX_LOAD_U32(handle_ptr);
        if (g_video_open_s.load() < 0) g_video_open_s = SinceStart();
        fprintf(stderr, "T+%.1fs open intro.wmv handle=%x\n", SinceStart(), g_intro_video_handle.load());
      }
    }
  }
}

// The video is done (played to the end or skipped) when the game closes the intro.wmv handle.
REX_EXTERN(__real___imp__NtClose);

REX_EXTERN(__wrap___imp__NtClose) {
  const uint32_t h = ctx.r3.u32;
  if (h != 0 && h == g_intro_video_handle.load()) {
    g_intro_video_handle = 0;
    fprintf(stderr, "T+%.1fs close intro.wmv\n", SinceStart());
    if (SkipIntroEnabled() && !g_intro_done.exchange(true)) {
      g_menu_reached_s = SinceStart();
      fprintf(stderr, "T+%.1fs INTRO done: intro video closed\n", SinceStart());
    }
  }
  __real___imp__NtClose(ctx, base);
}

// Guest memcpy (sub_826A85E8: dst=r3, src=r4, n=r5, returns dst). The recompiled version copies with
// lvx/vperm/stvx loops; host memmove is much faster for the big copies done while loading a song.
// GH2_COPY_TRACE=1 logs slow copies; GH2_FAST_MEMCPY=0 keeps the recompiled loop.
REX_EXTERN(__imp__sub_826A85E8);

REX_EXTERN(sub_826A85E8) {
  static const bool fast = [] {
    const char* e = getenv("GH2_FAST_MEMCPY");
    return !(e && e[0] == '0');
  }();
  static const bool trace = getenv("GH2_COPY_TRACE") != nullptr;
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, n = ctx.r5.u32;
  const auto t0 = std::chrono::steady_clock::now();
  if (fast) {
    std::memmove(base + dst, base + src, n);
    ctx.r3.u32 = dst;
  } else {
    __imp__sub_826A85E8(ctx, base);
  }
  if (trace) {
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    static std::atomic<uint64_t> calls{0}, bytes{0};
    static std::atomic<uint64_t> total_us{0};
    const uint64_t c = ++calls;
    bytes += n;
    total_us += static_cast<uint64_t>(ms * 1000);
    if (ms > 2.0) fprintf(stderr, "T+%.2fs COPY %u bytes took %.1f ms (%s)\n", SinceStart(), n, ms, fast ? "host" : "guest loop");
    if (c % 2000 == 0)
      fprintf(stderr, "T+%.2fs COPYSTAT calls=%llu bytes=%.1f MB total=%.1f ms\n", SinceStart(), (unsigned long long)c,
              bytes.load() / 1048576.0, total_us.load() / 1000.0);
  }
}

// SMT-yield hint of the guest (`db16cyc`), see work/analysis/postcodegen.sh. A real PAUSE keeps the sibling hardware
// thread / package power in check; a yield every 64 hints lets the thread the spinner is waiting for run on CPUs with
// few cores (a 2-core run used to hang in the audio threads' barrier, sub_826B10B0). GH2_SPIN_YIELD=0 disables the yield.
extern "C" void gh2_spin_hint() {
  static const bool yield_on = [] {
    const char* e = getenv("GH2_SPIN_YIELD");
    return !(e && e[0] == '0');
  }();
  // A guest thread that has been spinning for a while is really waiting (barrier, lock, timeout loop): after
  // GH2_SPIN_SLEEP_AFTER hints (default 2000, ~50 us) it sleeps GH2_SPIN_SLEEP_US microseconds per hint (default 30; 0
  // disables) instead of burning a core. The spin counter restarts when the gap between two hints shows the thread did
  // other work in between.
  static const unsigned sleep_after = [] { const char* e = getenv("GH2_SPIN_SLEEP_AFTER"); return e ? unsigned(atoi(e)) : 2000u; }();
  static const long sleep_ns = [] { const char* e = getenv("GH2_SPIN_SLEEP_US"); return (e ? atol(e) : 30L) * 1000L; }();
  thread_local unsigned n = 0, spins = 0;
  thread_local uint64_t last = 0;
  const uint64_t now = __rdtsc();
  if (now - last > 30000) spins = 0;  // > ~10 us since the previous hint: a new wait
  last = now;
  _mm_pause();
  if (sleep_ns > 0 && ++spins > sleep_after) {
    timespec ts{0, sleep_ns};
    nanosleep(&ts, nullptr);
    last = __rdtsc();
    return;
  }
  if (yield_on && (++n & 63u) == 0) sched_yield();
}

// ---- Content (DLC) usage tracking for the options menu -------------------------------------------------------------
// XamContentCreateEnumerator = the game lists packages; XamContentCreateEx(root, XCONTENT_DATA*) = it opens one. The menu
// shows a pack as LOADED once the game has opened it in this session.
#include <mutex>
#include <set>

REX_EXTERN(__real___imp__XamContentCreateEnumerator);
REX_EXTERN(__real___imp__XamContentCreateEx);

static std::mutex g_content_mutex;
static std::set<std::string> g_content_opened;
static std::atomic<uint32_t> g_content_enumerations{0};

bool Gh2ContentOpened(const std::string& file_name) {
  std::lock_guard<std::mutex> lock(g_content_mutex);
  return g_content_opened.count(file_name) != 0;
}
uint32_t Gh2ContentEnumerations() { return g_content_enumerations.load(); }

REX_EXTERN(__wrap___imp__XamContentCreateEnumerator) {
  ++g_content_enumerations;
  __real___imp__XamContentCreateEnumerator(ctx, base);
}

REX_EXTERN(__wrap___imp__XamContentCreateEx) {
  // r5 = XCONTENT_DATA { u32 device_id, u32 content_type, u16 display_name[128], char file_name[42] }
  const uint32_t data = ctx.r5.u32;
  if (data) {
    char name[43] = {};
    for (int i = 0; i < 42; ++i) name[i] = static_cast<char>(REX_LOAD_U8(data + 8 + 256 + i));
    const uint32_t type = REX_LOAD_U32(data + 4);
    if (type == 2) {
      std::lock_guard<std::mutex> lock(g_content_mutex);
      if (g_content_opened.insert(name).second) fprintf(stderr, "CONTENT opened by the game: %s\n", name);
    }
  }
  __real___imp__XamContentCreateEx(ctx, base);
}

// ---- Skipping the logos with a button (default start-up) -------------------------------------------------------------
// The game does not read the controller during the logos (only once the intro video starts), so a button pressed there
// went unnoticed. In the default start-up mode a host thread polls the controllers itself and, on the first button, runs
// the guest clock fast until the intro video opens (the same fast-forward the skip modes use). The video then plays at
// normal speed and the game's own "press a button to skip" works.
#include <rex/input/input_system.h>
#include <rex/runtime.h>

void Gh2StartLogoSkipWatcher(rex::Runtime* runtime) {
  auto* input = runtime ? static_cast<rex::input::InputSystem*>(runtime->input_system()) : nullptr;
  if (!input || SkipIntroEnabled()) return;  // the skip modes fast-forward on their own
  std::thread([input] {
    while (SinceStart() < 40.0 && g_video_open_s.load() < 0 && !g_intro_done.load()) {
      static const double fake_press = [] { const char* e = getenv("GH2_TEST_LOGO_PRESS"); return e ? atof(e) : -1.0; }();
      if (fake_press >= 0 && SinceStart() >= fake_press) {  // testing aid: pretend a button was pressed
        FastForwardBegin(16.0, "simulated button press");
        return;
      }
      for (uint32_t user = 0; user < 4; ++user) {
        rex::input::X_INPUT_STATE st = {};
        const auto rc = input->GetStateForUI(user, &st);
        if (InputTrace()) {
          static bool shown[4] = {};
          if (!shown[user] && rc == 0) { shown[user] = true; fprintf(stderr, "LOGOWATCH polling user %u: connected, buttons=%04x\n", user, st.gamepad.buttons); }
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

// ---- In-song detection for the keyboard's frets-only style ------------------------------------------------------------
// sub_821D75F8 = RndCam::UpdateProjection(this), run every frame for the cameras in use. The note highway camera is the
// perspective camera with a tight frustum (far plane < 1000) and a 45 degree FOV (field offsets: far +696, y_fov +700).
// Seeing it means a song is on screen. The hook changes nothing.
REX_EXTERN(__imp__sub_821D75F8);
REX_EXTERN(sub_821D75F8) {
  const uint32_t cam = ctx.r3.u32;
  if (cam) {
    const float far_plane = std::bit_cast<float>(REX_LOAD_U32(cam + 696));
    const float fov = std::bit_cast<float>(REX_LOAD_U32(cam + 700));
    if (far_plane < 1000.0f && std::fabs(fov - 0.7854f) < 0.002f) Gh2KeyboardNoteInSong();
  }
  __imp__sub_821D75F8(ctx, base);
}
