#include "problem_report.h"

#include "native_audio.h"

#include <rex/cvar.h>
#include <sys/utsname.h>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#include "controllers.h"
#include "fps_meter.h"
#include "keyboard.h"
#include "zip_writer.h"

namespace fs = std::filesystem;

namespace {

std::string ReadFile(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::ostringstream s;
  s << f.rdbuf();
  return s.str();
}

std::vector<std::string> Lines(const std::string& text) {
  std::vector<std::string> out;
  std::istringstream in(text);
  for (std::string l; std::getline(in, l);) out.push_back(l);
  return out;
}

std::string Section(const char* title) { return std::string("\n===== ") + title + " =====\n"; }

std::string Tail(const std::vector<std::string>& lines, size_t n) {
  std::string s;
  for (size_t i = lines.size() > n ? lines.size() - n : 0; i < lines.size(); ++i) s += lines[i] + "\n";
  return s;
}

std::string Head(const std::vector<std::string>& lines, size_t n) {
  std::string s;
  for (size_t i = 0; i < std::min(n, lines.size()); ++i) s += lines[i] + "\n";
  return s;
}

// First line of a "key: value" file such as /proc/cpuinfo that starts with `key`.
std::string FirstValue(const std::string& text, const std::string& key) {
  for (const auto& l : Lines(text))
    if (l.rfind(key, 0) == 0) {
      const size_t c = l.find(':');
      return c == std::string::npos ? l : l.substr(c + 2);
    }
  return "?";
}

std::string OsRelease() {
  for (const auto& l : Lines(ReadFile("/etc/os-release")))
    if (l.rfind("PRETTY_NAME=", 0) == 0) {
      std::string v = l.substr(12);
      v.erase(std::remove(v.begin(), v.end(), '"'), v.end());
      return v;
    }
  return "?";
}

std::string Env(const char* name) {
  const char* v = std::getenv(name);
  return v && *v ? v : "-";
}

std::vector<fs::path> LogFiles(const fs::path& dir) {
  std::vector<fs::path> files;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir, ec))
    if (e.is_regular_file() && e.path().filename().string().rfind("gh2_", 0) == 0) files.push_back(e.path());
  std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
    std::error_code e1, e2;
    return fs::last_write_time(a, e1) > fs::last_write_time(b, e2);  // newest first
  });
  return files;
}

}  // namespace

namespace {
Gh2ScreenshotFn g_screenshot_fn = nullptr;
}
void Gh2SetScreenshotFn(Gh2ScreenshotFn fn) { g_screenshot_fn = fn; }

std::string Gh2WriteProblemReport(std::string& error, bool with_screenshot, bool* screenshot_saved) {
  const fs::path logs = "logs";
  std::error_code ec;
  fs::create_directories(logs, ec);
  if (screenshot_saved) *screenshot_saved = false;

  std::ostringstream r;
  const std::time_t now = std::time(nullptr);
  char stamp[32], fname[64];
  std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
  std::strftime(fname, sizeof(fname), "report-%Y%m%d-%H%M%S.txt", std::localtime(&now));
  const fs::path png = logs / (std::string(fname).substr(0, std::string(fname).size() - 4) + ".png");
  bool shot = false;
  if (with_screenshot && g_screenshot_fn) shot = g_screenshot_fn(png.string());
  if (screenshot_saved) *screenshot_saved = shot;
  r << "reGH2 problem report\nCreated: " << stamp << "\n";
  r << "Screenshot: " << (shot ? png.filename().string() + " (the game's picture only, no desktop)"
                              : with_screenshot ? std::string("could not be taken") : std::string("not requested")) << "\n";
  r << "Attach the .zip (it holds this file and the screenshot) to a GitHub issue and say what you were doing when the problem happened.\n";

  r << Section("Install");
  // The install record sits next to the game folder when installed by the launcher (prefix/install.json, game in prefix/app).
  const std::string install = ReadFile(fs::path("..") / "install.json");
  r << (install.empty() ? "(no install.json: a development build)\n" : install + "\n");

  r << Section("System");
  utsname u{};
  uname(&u);
  const std::string cpu = ReadFile("/proc/cpuinfo"), mem = ReadFile("/proc/meminfo");
  r << "OS: " << OsRelease() << "\nKernel: " << u.release << " " << u.machine << "\n";
  r << "Session: " << Env("XDG_SESSION_TYPE") << ", desktop " << Env("XDG_CURRENT_DESKTOP") << ", Wayland display "
    << Env("WAYLAND_DISPLAY") << "\n";
  r << "CPU: " << FirstValue(cpu, "model name") << " (" << std::thread::hardware_concurrency() << " threads)\n";
  r << "RAM: total " << FirstValue(mem, "MemTotal") << ", available " << FirstValue(mem, "MemAvailable") << "\n";

  r << Section("Settings in use");
  for (const char* name :
       {"present_effect", "present_dither", "gfx_fsr_softness", "gfx_cas_sharpness", "anisotropic_override", "resolution_scale",
        "native_2x_msaa", "swap_post_effect", "fullscreen", "window_width", "window_height", "vsync", "audio_latency",
        "audio_maxqframes", "audio_output", "input_reader", "startup_mode", "keyboard_player", "keyboard_strum", "show_fps", "controller_guitars_first"})
    r << name << " = " << rex::cvar::GetFlagByName(name) << "\n";

  r << Section("Controllers");
  if (auto* a = Gh2GetAssignment()) {
    const auto rows = a->Snapshot();
    if (rows.empty()) r << "none\n";
    for (const auto& row : rows) r << "player " << row.player << ": " << row.kind << " - " << row.name << "\n";
  }
  r << "keyboard: " << (Gh2KeyboardSlot() < 0 ? std::string("not playing") : "plays as player " + std::to_string(Gh2KeyboardSlot() + 1))
    << "\n";

  r << Section("Audio");
  r << (Gh2AudioStatsLine().empty() ? std::string("audio: played by the SDK's driver (or not started yet)") : Gh2AudioStatsLine()) << "\n";

  r << Section("Frame rate of the game right now");
  Gh2FpsView v;
  Gh2FpsRead(v);
  r << "fps " << v.fps << ", frame " << v.frame_ms << " ms, 1% low " << v.low1_fps << ", worst of the last 5 s " << v.worst_ms
    << " ms\n";

  r << Section("Slowdown log (logs/perf.log, last 40 lines)");
  r << Tail(Lines(ReadFile(logs / "perf.log")), 40);

  const auto files = LogFiles(logs);
  r << Section("This session's log (first 70 lines, then the last 300)");
  if (!files.empty()) {
    const auto lines = Lines(ReadFile(files[0]));
    r << "file: " << files[0].filename().string() << ", " << lines.size() << " lines\n" << Head(lines, 70);
    if (lines.size() > 370) r << "[...]\n";
    r << Tail(std::vector<std::string>(lines.begin() + std::min<size_t>(70, lines.size()), lines.end()), 300);
  } else {
    r << "no log file found\n";
  }
  if (files.size() > 1) {
    r << Section("Previous session's log (last 200 lines; useful after a crash)");
    r << "file: " << files[1].filename().string() << "\n" << Tail(Lines(ReadFile(files[1])), 200);
  }

  // Keep the user's name out of a public issue: the home folder becomes "~".
  std::string text = r.str();
  if (const char* home = std::getenv("HOME"); home && *home && std::string(home) != "/") {
    const std::string h = home;
    for (size_t pos = 0; (pos = text.find(h, pos)) != std::string::npos; pos += 1) text.replace(pos, h.size(), "~");
  }
  const fs::path out = logs / fname;
  std::ofstream f(out, std::ios::binary);
  if (!f) {
    error = "could not write " + out.string();
    return "";
  }
  f << text;
  f.close();
  // One file to attach: the report and, if it was taken, the screenshot.
  std::vector<std::pair<std::string, std::string>> entries = {{out.filename().string(), out.string()}};
  if (shot) entries.push_back({png.filename().string(), png.string()});
  const fs::path zip = out.parent_path() / (out.stem().string() + ".zip");
  if (Gh2WriteZip(zip.string(), entries)) return fs::absolute(zip, ec).string();
  return fs::absolute(out, ec).string();
}
