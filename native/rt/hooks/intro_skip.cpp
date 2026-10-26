// Intro skipping and the start-up modes (moved from app/src/gh2_host.cpp). No SDK dependency: the user's options come from the host
// services, the guest clock is driven through them.
//
// The splash screens and the intro video run at ~30 ms per guest frame, the menus at 16.7 ms. The logos are busy-wait loops on the
// guest timebase (`mftb`: "spin until N seconds elapsed", loc_82120378 in sub_82120208), so running guest time faster shortens
// them without touching game code. While the guest is still in the slow phase, A is pressed in short pulses so every skippable
// screen is dismissed (hooks_input.cpp); the pulses stop when the guest runs at menu speed (hooks_video.cpp) or the intro video
// handle is closed (hooks_file.cpp).
#include <gh2rt/host_services.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "hooks_internal.h"

namespace gh2rt::hooks {
namespace detail {
std::atomic<bool> g_intro_done{false};
std::atomic<uint32_t> g_intro_video_handle{0};
std::atomic<double> g_video_open_s{-1.0};
std::atomic<double> g_menu_reached_s{-1.0};
std::atomic<bool> g_fast_forward{false};
std::atomic<uint32_t> g_intro_poll_count{0};
std::atomic<double> g_ff_started_s{0.0};
}  // namespace detail

namespace {

const std::chrono::steady_clock::time_point g_process_t0 = std::chrono::steady_clock::now();

const Settings& Cfg() {
  static const Settings settings = Host().GetSettings();
  return settings;
}

// Tiny reader for "key = true|false|<number>" lines of the user config file (XDG: ~/.config/gh2/config.toml).
bool ConfigFileValue(const char* key, std::string* out) {
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

bool ConfigFileBool(const char* key) {
  std::string v;
  return ConfigFileValue(key, &v) && (v == "true" || v == "1" || v == "yes");
}

// GH2_AUTOSTART=1 (testing aid, implies skip-intro): keep pulsing A for GH2_AUTOSTART_SECONDS (default 30) after the menus appear,
// which walks Quick Play -> song -> difficulty until the song loads.
bool AutostartEnabled() {
  static const bool on = [] {
    const char* e = getenv("GH2_AUTOSTART");
    return Cfg().autostart || (e && e[0] != '0') || ConfigFileBool("autostart");
  }();
  return on;
}

}  // namespace

double SinceStart() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_process_t0).count();
}

bool InputTrace() {
  static const bool on = getenv("GH2_INPUT_TRACE") != nullptr;
  return on;
}

bool SkipIntroEnabled() {
  static const bool on = [] {
    const char* e = getenv("GH2_SKIP_INTRO");
    return Cfg().skip_intro || (e && e[0] != '0') || ConfigFileBool("skip_intro") || AutostartEnabled() ||
           Cfg().startup_mode != "normal";
  }();
  return on;
}

bool IntroDone() { return detail::g_intro_done.load(std::memory_order_relaxed); }
bool IntroVideoOpened() { return detail::g_video_open_s.load() >= 0; }

void FastForwardBegin(double speed, const char* why) {
  if (detail::g_fast_forward.exchange(true)) return;
  detail::g_ff_started_s = SinceStart();
  Host().SetGuestTimeScalar(speed);
  fprintf(stderr, "T+%.1fs intro fast-forward x%.0f (%s)\n", SinceStart(), speed, why);
}

namespace detail {

void FastForwardEnd(const char* why) {
  if (g_fast_forward.exchange(false)) {
    Host().SetGuestTimeScalar(1.0);
    fprintf(stderr, "T+%.1fs intro fast-forward off (%s)\n", SinceStart(), why);
  }
}

bool IntroSkipActive() {
  if (!SkipIntroEnabled()) return false;
  if (g_intro_done.load(std::memory_order_relaxed)) {
    // Past the intro: only the autostart testing aid keeps pressing, for a limited time.
    static const double autostart_s = [] {
      const char* e = getenv("GH2_AUTOSTART_SECONDS");
      return e ? atof(e) : static_cast<double>(Cfg().autostart_seconds);
    }();
    const double reached = g_menu_reached_s.load();
    // startup_mode = menu: one short press of A at the title screen takes the player to the main menu.
    static const bool to_menu = Cfg().startup_mode == "menu";
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

}  // namespace detail
}  // namespace gh2rt::hooks
