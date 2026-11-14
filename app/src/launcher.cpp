// The launcher (launcher.h). Layout: a stage at night. A note highway scrolls behind the right two thirds of the window (a playable easy chart,
// about one note per beat, the frets light up as the notes reach them); the screens sit in a panel on the left. Colours are those of the old
// Tk launcher (launcher/palette.py): stage black, blood red, amber and gold, the five fret colours. Fonts (SIL OFL, src/launcher_fonts):
// New Rocker for the titles, Barlow Condensed for everything else.
#include "launcher.h"

#include "native_overlay.h"
#include "native_window.h"

#include <SDL3/SDL.h>
#include <gh2os/os.h>
#include <gh2render/renderer.h>
#include <gh2rt/render_menu.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <rex/cvar.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/windowed_app_context.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdarg>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

REXCVAR_DEFINE_BOOL(launcher, false, "GH2", "Open the launcher (install the game, settings, updates, play) instead of starting the game");

bool Gh2LauncherRequested() {
  if (const char* e = std::getenv("GH2_LAUNCHER"); e && *e && std::strcmp(e, "0") != 0) return true;
  return REXCVAR_GET(launcher);
}

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#pragma clang diagnostic ignored "-Wc++26-extensions"
const unsigned char kRocker[] = {
#embed "launcher_fonts/NewRocker-Regular.ttf"
};
const unsigned char kBarlowSemi[] = {
#embed "launcher_fonts/BarlowCondensed-SemiBold.ttf"
};
const unsigned char kBarlowMedium[] = {
#embed "launcher_fonts/BarlowCondensed-Medium.ttf"
};
#pragma clang diagnostic pop

// ------------------------------------------------------------------------------------------------------------------------------ palette
ImU32 Rgb(uint32_t rgb, float a = 1.0f) { return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, int(a * 255.0f + 0.5f)); }
ImVec4 Rgb4(uint32_t rgb, float a = 1.0f) { return ImGui::ColorConvertU32ToFloat4(Rgb(rgb, a)); }
constexpr uint32_t kStage = 0x0b0606, kPanel = 0x1a0b0b, kRaised = 0x261212, kTrough = 0x2a1515, kRed = 0xcc1c1a, kDeepRed = 0x661010,
                   kAmber = 0xfa8019, kGold = 0xffd440, kText = 0xf7f2ea, kDim = 0xb09893, kSilver = 0xc9ccd2, kSteel = 0x8b8f98;
constexpr uint32_t kFret[5] = {0x2fbf3a, 0xe5322d, 0xf5d321, 0x2f7bf0, 0xf58a1f};

struct Fonts {
  ImFont* logo = nullptr;     // New Rocker 64
  ImFont* heading = nullptr;  // New Rocker 34
  ImFont* big = nullptr;      // Barlow SemiBold 40 (Play, odometer at x2)
  ImFont* ui = nullptr;       // Barlow SemiBold 22
  ImFont* body = nullptr;     // Barlow Medium 19
  ImFont* small = nullptr;    // Barlow Medium 15
} g_fonts;

void SetupFonts(ImFontAtlas* atlas) {
  auto add = [atlas](const unsigned char* data, size_t size, float px) {
    ImFontConfig c;
    c.FontDataOwnedByAtlas = false;
    c.OversampleH = 2;
    c.OversampleV = 1;
    return atlas->AddFontFromMemoryTTF(const_cast<unsigned char*>(data), int(size), px, &c);
  };
  g_fonts.logo = add(kRocker, sizeof(kRocker), 64);
  g_fonts.heading = add(kRocker, sizeof(kRocker), 34);
  g_fonts.big = add(kBarlowSemi, sizeof(kBarlowSemi), 40);
  g_fonts.ui = add(kBarlowSemi, sizeof(kBarlowSemi), 22);
  g_fonts.body = add(kBarlowMedium, sizeof(kBarlowMedium), 19);
  g_fonts.small = add(kBarlowMedium, sizeof(kBarlowMedium), 15);
}

// ------------------------------------------------------------------------------------------------------------------------------ files
std::string Home() { return gh2os::HomeDir(); }
std::string DataHome() {
  const char* x = std::getenv("XDG_DATA_HOME");
  return x && *x ? std::string(x) : Home() + "/.local/share";
}
std::string Prefix() {   // the installer's default prefix (scripts/gh2install/pipeline.py default_prefix)
  if (const char* p = std::getenv("GH2_PREFIX"); p && *p) return p;
  return DataHome() + "/gh2-recomp";
}
std::string ReadFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}
// The value of a top-level "key": "text" (or number) of install.json / host.json: enough for files the installer writes itself.
std::string JsonValue(const std::string& text, const char* key) {
  const std::string k = std::string("\"") + key + "\"";
  size_t p = text.find(k);
  if (p == std::string::npos) return {};
  p = text.find(':', p + k.size());
  if (p == std::string::npos) return {};
  ++p;
  while (p < text.size() && (text[p] == ' ' || text[p] == '\n')) ++p;
  if (p < text.size() && text[p] == '"') {
    const size_t e = text.find('"', p + 1);
    return e == std::string::npos ? std::string() : text.substr(p + 1, e - p - 1);
  }
  size_t e = p;
  while (e < text.size() && text[e] != ',' && text[e] != '\n' && text[e] != '}') ++e;
  return text.substr(p, e - p);
}

struct InstallInfo {
  bool installed = false;
  std::string version, xex, when, game_dir;
  int seconds = 0;
};
InstallInfo ReadInstall() {
  InstallInfo i;
  const std::string text = ReadFile(Prefix() + "/install.json");
  i.installed = !text.empty() && fs::exists(Prefix() + "/app/gh2");
  i.version = JsonValue(text, "version");
  i.xex = JsonValue(text, "xex_sha1").substr(0, 8);
  i.when = JsonValue(text, "installed").substr(0, 16);
  i.game_dir = JsonValue(text, "game_dir");
  i.seconds = std::atoi(JsonValue(text, "seconds").c_str());
  return i;
}
// The settings the launcher owns, in the game's config file (app/gh2.toml, which the in-game menu also writes). Lines of other keys stay.
class Settings {
 public:
  void Load() {
    path_ = Prefix() + "/app/gh2.toml";
    values_.clear();
    std::istringstream in(ReadFile(path_));
    for (std::string line; std::getline(in, line);) {
      const size_t eq = line.find('=');
      if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
      std::string k = Trim(line.substr(0, eq)), v = Trim(line.substr(eq + 1));
      if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
      values_[k] = v;
    }
  }
  std::string Get(const std::string& key, const std::string& fallback) const {
    auto it = values_.find(key);
    return it == values_.end() || it->second.empty() ? fallback : it->second;
  }
  bool GetBool(const std::string& key, bool fallback) const { return Get(key, fallback ? "true" : "false") == "true"; }
  void Set(const std::string& key, const std::string& value, bool quoted) {
    values_[key] = value;
    std::istringstream in(ReadFile(path_));
    std::string out;
    bool done = false;
    const std::string line_value = key + " = " + (quoted ? "\"" + value + "\"" : value);
    for (std::string line; std::getline(in, line);) {
      const size_t eq = line.find('=');
      if (!done && eq != std::string::npos && line[0] != '#' && Trim(line.substr(0, eq)) == key) {
        out += line_value + "\n";
        done = true;
      } else {
        out += line + "\n";
      }
    }
    if (out.empty()) out = "# GH2 settings (written by the launcher and the in-game menu, F2)\n";
    if (!done) out += line_value + "\n";
    std::error_code ec;
    fs::create_directories(fs::path(path_).parent_path(), ec);
    std::ofstream(path_, std::ios::trunc) << out;
  }

 private:
  static std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) ++b;
    return s.substr(b);
  }
  std::string path_;
  std::map<std::string, std::string> values_;
};

// ------------------------------------------------------------------------------------------------------------------------------ tasks
// The installer's command line: inside the AppImage its AppRun (which sets up the bundled Python), else GH2_INSTALLER_CMD or the sources.
std::vector<std::string> InstallerCommand(const std::vector<std::string>& args) {
  std::vector<std::string> argv;
  if (const char* appdir = std::getenv("APPDIR"); appdir && *appdir && fs::exists(std::string(appdir) + "/AppRun")) {
    argv.push_back(std::string(appdir) + "/AppRun");
  } else if (const char* cmd = std::getenv("GH2_INSTALLER_CMD"); cmd && *cmd) {
    std::istringstream in(cmd);
    for (std::string w; in >> w;) argv.push_back(w);
  } else {
    argv = {"python3", "-m", "gh2install"};
  }
  argv.insert(argv.end(), args.begin(), args.end());
  return argv;
}

class Task {
 public:
  enum class Kind { kInstall, kRebuild, kUpdate, kReport, kCheckUpdate, kPlay };
  Task(Kind kind, std::vector<std::string> args) : kind_(kind) {
    gh2os::SpawnOptions o;
    o.argv = InstallerCommand(args);
#ifndef _WIN32
    if (kind == Kind::kPlay) {   // the game writes to a log file, not to our pipe: it keeps running (and writing) when the launcher is closed
      const std::string logs = Prefix() + "/app/logs";
      std::error_code ec;
      fs::create_directories(logs, ec);
      o.argv.insert(o.argv.begin(), {"/bin/sh", "-c", "exec \"$0\" \"$@\" >\"$GH2_GAME_OUTPUT\" 2>&1 </dev/null"});
      o.env_set.push_back({"GH2_GAME_OUTPUT", logs + "/game-output.log"});
    }
#endif
    std::string error;
    if (!proc_.Start(o, &error)) {
      Finish(-1, "could not start " + o.argv[0] + ": " + error);
      return;
    }
    reader_ = std::thread([this] { Read(); });
  }
  ~Task() {
    if (kind_ == Kind::kPlay) stop_ = true;    // the game outlives the launcher
    else if (proc_.running()) proc_.Terminate();
    if (reader_.joinable()) reader_.join();
  }
  Kind kind() const { return kind_; }
  bool done() const { return done_.load(); }
  int exit_code() const { return exit_code_.load(); }
  float progress() const { return progress_.load(); }
  void Cancel() {
    cancelled_ = true;
    proc_.Terminate();
  }
  bool cancelled() const { return cancelled_.load(); }
  std::string stage() {
    std::lock_guard<std::mutex> l(m_);
    return stage_;
  }
  std::vector<std::string> tail(size_t n) {
    std::lock_guard<std::mutex> l(m_);
    std::vector<std::string> out;
    for (size_t i = lines_.size() > n ? lines_.size() - n : 0; i < lines_.size(); ++i) out.push_back(lines_[i]);
    return out;
  }
  std::string find_line(const char* needle) {
    std::lock_guard<std::mutex> l(m_);
    for (auto it = all_.rbegin(); it != all_.rend(); ++it)
      if (it->find(needle) != std::string::npos) return *it;
    return {};
  }
  double elapsed() const { return std::chrono::duration<double>((done_ ? end_ : Clock::now()) - start_).count(); }

 private:
  void Read() {
    char buf[4096];
    std::string partial;
    for (;;) {
      const int n = proc_.ReadOutput(buf, sizeof buf, 200);
      if (n < 0 || stop_) break;
      partial.append(buf, size_t(n));
      for (size_t nl; (nl = partial.find_first_of("\r\n")) != std::string::npos;) {
        Line(partial.substr(0, nl));
        partial.erase(0, nl + 1);
      }
    }
    if (!partial.empty()) Line(partial);
    int code = 0;
    std::string how;
    while (!proc_.Poll(&code, &how)) {
      if (stop_) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    Finish(code, how);
  }
  void Line(const std::string& line) {
    if (line.empty()) return;
    std::lock_guard<std::mutex> l(m_);
    unsigned pct = 0;
    char text[512] = {};
    if (std::sscanf(line.c_str(), "[%u%%] %511[^\n]", &pct, text) == 2) {   // the installer's progress lines
      progress_ = std::min(1.0f, pct / 100.0f);
      stage_ = text;
    }
    lines_.push_back(line);
    if (lines_.size() > 200) lines_.pop_front();
    all_.push_back(line);
    if (all_.size() > 4000) all_.erase(all_.begin(), all_.begin() + 1000);
  }
  void Finish(int code, const std::string& how) {
    {
      std::lock_guard<std::mutex> l(m_);
      if (code != 0 && !how.empty()) lines_.push_back(how);
    }
    exit_code_ = code;
    end_ = Clock::now();
    done_ = true;
  }

  Kind kind_;
  gh2os::Process proc_;
  std::thread reader_;
  std::mutex m_;
  std::deque<std::string> lines_;
  std::vector<std::string> all_;
  std::string stage_;
  std::atomic<float> progress_{0.0f};
  std::atomic<bool> done_{false}, cancelled_{false}, stop_{false};
  std::atomic<int> exit_code_{0};
  Clock::time_point start_ = Clock::now(), end_{};
};

// ------------------------------------------------------------------------------------------------------------------------------ drawing helpers
ImVec2 operator+(ImVec2 a, ImVec2 b) { return {a.x + b.x, a.y + b.y}; }
ImVec2 operator*(ImVec2 a, float k) { return {a.x * k, a.y * k}; }

// A quad with one colour per corner (Dear ImGui has only axis-aligned gradients).
void GradientQuad(ImDrawList* dl, ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 d, ImU32 ca, ImU32 cb, ImU32 cc, ImU32 cd) {
  const ImVec2 uv = dl->_Data->TexUvWhitePixel;
  dl->PrimReserve(6, 4);
  const ImDrawIdx i = ImDrawIdx(dl->_VtxCurrentIdx);
  dl->PrimWriteIdx(i);
  dl->PrimWriteIdx(ImDrawIdx(i + 1));
  dl->PrimWriteIdx(ImDrawIdx(i + 2));
  dl->PrimWriteIdx(i);
  dl->PrimWriteIdx(ImDrawIdx(i + 2));
  dl->PrimWriteIdx(ImDrawIdx(i + 3));
  dl->PrimWriteVtx(a, uv, ca);
  dl->PrimWriteVtx(b, uv, cb);
  dl->PrimWriteVtx(c, uv, cc);
  dl->PrimWriteVtx(d, uv, cd);
}
// A soft round glow: full colour in the middle, transparent at the rim.
void Glow(ImDrawList* dl, ImVec2 c, ImVec2 r, ImU32 inner) {
  const int n = 48;
  const ImVec2 uv = dl->_Data->TexUvWhitePixel;
  const ImU32 outer = inner & ~IM_COL32_A_MASK;
  dl->PrimReserve(n * 3, n + 1);
  const ImDrawIdx base = ImDrawIdx(dl->_VtxCurrentIdx);
  dl->PrimWriteVtx(c, uv, inner);
  for (int k = 0; k < n; ++k) {
    const float t = 2.0f * IM_PI * k / n;
    dl->PrimWriteVtx(ImVec2(c.x + std::cos(t) * r.x, c.y + std::sin(t) * r.y), uv, outer);
  }
  for (int k = 0; k < n; ++k) {
    dl->PrimWriteIdx(base);
    dl->PrimWriteIdx(ImDrawIdx(base + 1 + k));
    dl->PrimWriteIdx(ImDrawIdx(base + 1 + (k + 1) % n));
  }
}

// The highway's chart: 96 BPM, a 16-beat loop, a few chords and sustains (times in beats, lanes 0 green .. 4 orange).
struct ChartNote {
  float beat;
  uint8_t lanes;   // bit per lane
  float sustain;   // beats
};
const ChartNote kChart[] = {{0, 1, 0},     {1, 2, 0},  {2, 4, 0},  {3, 2, 0},   {4, 1 | 4, 1.5f}, {6, 4, 0},  {7, 8, 0},
                            {8, 4, 0},     {8.5f, 2, 0}, {9, 4, 0}, {10, 16, 1.5f}, {12, 8, 0},     {13, 4, 0}, {14, 2 | 8, 1}};
constexpr float kBeat = 60.0f / 96.0f, kLook = 4.5f * kBeat, kLoopBeats = 16.0f;

void DrawStage(ImDrawList* dl, ImVec2 size, float now, bool narrow, float flash) {
  dl->AddRectFilled(ImVec2(0, 0), size, Rgb(kStage));
  const float W = size.x, H = size.y;
  const float cx = narrow ? W * 0.5f : W * 0.66f, horizon = H * (narrow ? 0.12f : 0.2f);
  Glow(dl, ImVec2(cx, horizon), ImVec2(H * 0.95f, H * 0.75f), Rgb(kRed, 0.30f));
  Glow(dl, ImVec2(cx, horizon), ImVec2(H * 0.45f, H * 0.3f), Rgb(kAmber, 0.10f));
  // sweeping spotlights
  for (int i = 0; i < 4; ++i) {
    const float base = W * (0.38f + i * 0.2f), a = std::sin(now * 0.5f + i * 1.7f) * 0.35f;
    const float len = H * 0.95f, spread = H * 0.22f;
    const ImVec2 dir(std::sin(a), std::cos(a)), side(std::cos(a), -std::sin(a));
    const ImVec2 top(base, -10.0f), bottom = top + dir * len;
    const ImU32 col = Rgb(i % 2 ? kGold : kAmber, 0.17f), clear = col & ~IM_COL32_A_MASK;
    GradientQuad(dl, top + side * -6.0f, top + side * 6.0f, bottom + side * spread, bottom + side * -spread, col, col, clear, clear);
  }
  // the highway in perspective
  const float bottom = H + 4.0f, halfB = std::min(W * (narrow ? 0.46f : 0.3f), H * 0.62f), halfT = halfB * 0.12f;
  auto P = [&](float lx, float z, float* half = nullptr) {
    const float k = std::pow(std::max(z, 0.0f), 1.8f);
    const float y = horizon + (bottom - horizon) * k, h = halfT + (halfB - halfT) * k;
    if (half) *half = h;
    return ImVec2(cx + lx * h, y);
  };
  GradientQuad(dl, P(-1, 0), P(1, 0), P(1, 1), P(-1, 1), Rgb(kPanel, 0.0f), Rgb(kPanel, 0.0f), Rgb(kRaised, 0.96f), Rgb(kRaised, 0.96f));
  GradientQuad(dl, P(-1, 0.25f), P(1, 0.25f), P(1, 1), P(-1, 1), Rgb(kPanel, 0.5f), Rgb(kPanel, 0.5f), Rgb(kRaised, 0.0f), Rgb(kRaised, 0.0f));
  for (int i = 0; i <= 5; ++i) {
    const float lx = -1.0f + i * 0.4f;
    const bool rail = i == 0 || i == 5;
    dl->AddLine(P(lx, 0.02f), P(lx, 1), Rgb(kSilver, rail ? 0.55f : 0.12f), rail ? 2.0f : 1.0f);
  }
  const float strike = 0.86f;
  auto zAt = [&](float dt) { return strike * (1.0f - dt / kLook); };
  for (int k = int(std::ceil(now / kBeat)); k * kBeat - now < kLook * 1.05f; ++k) {   // beat lines, a heavier one per bar
    const float z = zAt(k * kBeat - now);
    if (z < 0.03f || z > 1.0f) continue;
    const bool bar = k % 4 == 0;
    dl->AddLine(P(-1, z), P(1, z), Rgb(kSilver, (bar ? 0.12f : 0.05f) + z * 0.12f), (bar ? 2.0f : 1.0f) + z);
  }
  struct Visible {
    uint8_t lanes;
    float hit, end;
  };
  std::vector<Visible> visible;
  float lit[5] = {0, 0, 0, 0, 0};
  const float loop = kLoopBeats * kBeat;
  for (int k = -1; k <= 1; ++k) {
    const float start = (std::floor(now / loop) + k) * loop;
    for (const ChartNote& n : kChart) {
      const float hit = start + n.beat * kBeat, end = hit + n.sustain * kBeat;
      if (end < now - 0.15f || hit - now > kLook) continue;
      if (now >= hit && now <= end + 0.15f)
        for (int l = 0; l < 5; ++l)
          if (n.lanes & (1 << l)) lit[l] = std::max(lit[l], 1.0f - std::max(0.0f, now - end) / 0.15f);
      visible.push_back({n.lanes, hit, end});
    }
  }
  for (int i = 0; i < 5; ++i) {   // the frets
    float half;
    const ImVec2 c = P(-0.8f + i * 0.4f, strike, &half);
    const ImVec2 r(half * 0.17f, half * 0.085f);
    dl->AddEllipseFilled(c, r, Rgb(kStage, 0.9f));
    dl->AddEllipse(c, r, Rgb(kFret[i]), 0.0f, 0, 3.0f);
    const float glow = std::max(lit[i], flash);
    if (glow > 0.0f) {
      dl->AddEllipseFilled(c, r * 0.8f, Rgb(kFret[i], glow * 0.85f));
      Glow(dl, ImVec2(c.x, c.y - r.y * 1.4f), ImVec2(r.x * 0.9f, r.y * 3.2f), Rgb(kFret[i], glow * 0.45f));
    }
  }
  std::sort(visible.begin(), visible.end(), [](const Visible& a, const Visible& b) { return a.hit > b.hit; });
  for (const Visible& n : visible) {
    for (int lane = 0; lane < 5; ++lane) {
      if (!(n.lanes & (1 << lane))) continue;
      const float lx = -0.8f + lane * 0.4f;
      if (n.end > n.hit) {
        const float zh = zAt(std::max(0.0f, n.hit - now)), ze = std::max(0.03f, zAt(n.end - now));
        if (zh > ze) {
          float half;
          const ImVec2 s0 = P(lx, zh, &half);
          dl->AddLine(s0, P(lx, ze), Rgb(kFret[lane], 0.6f), std::max(2.0f, half * 0.05f));
        }
      }
      if (n.hit <= now) continue;   // played: the gem is gone and its fret lights up
      const float z = zAt(n.hit - now);
      if (z < 0.03f) continue;
      float half;
      const ImVec2 c = P(lx, z, &half);
      const ImVec2 r(half * 0.15f, half * 0.0675f);
      dl->AddEllipseFilled(ImVec2(c.x, c.y + r.y * 0.5f), r, IM_COL32(0, 0, 0, 128));
      dl->AddEllipseFilled(c, r, Rgb(kFret[lane]));
      dl->AddEllipseFilled(ImVec2(c.x, c.y - r.y * 0.15f), ImVec2(r.x * 0.42f, r.y * 0.36f), Rgb(kText, 0.85f));
    }
  }
  if (flash > 0.0f) dl->AddRectFilled(ImVec2(0, 0), size, Rgb(kGold, flash * 0.16f));
  // vignette: darker edges
  const ImU32 dark = IM_COL32(0, 0, 0, 160), none = IM_COL32(0, 0, 0, 0);
  dl->AddRectFilledMultiColor(ImVec2(0, H * 0.7f), ImVec2(W, H), none, none, dark, dark);
  dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, H * 0.18f), dark, dark, none, none);
  dl->AddRectFilledMultiColor(ImVec2(W * 0.85f, 0), ImVec2(W, H), none, dark, dark, none);
}

// ------------------------------------------------------------------------------------------------------------------------------ the app
class LauncherApp;

class LauncherDialog final : public rex::ui::ImGuiDialog {
 public:
  LauncherDialog(rex::ui::ImGuiDrawer* drawer, LauncherApp* app) : ImGuiDialog(drawer), app_(app) {}

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  LauncherApp* app_;
};

class LauncherApp final : public rex::ui::WindowedApp, public rex::ui::WindowListener {
 public:
  explicit LauncherApp(rex::ui::WindowedAppContext& context) : WindowedApp(context, "gh2-launcher") {}

  bool OnInitialize() override {
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, std::getenv("GH2_INPUT_HIDAPI") ? "1" : "0");   // HIDAPI's libusb scan held the first frame for 1-2 minutes
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    window_ =rex::ui::Window::Create(app_context(), "reGH2");
    if (!window_) return false;
    window_->SetDesiredLogicalSize(1280, 720);
    window_->AddListener(this);
    if (!window_->Open()) return false;
    Gh2NativeWindowOpened(window_.get());
    gh2rt::NativeWindow nw;
    if (!Gh2GetNativeWindow(&nw)) {
      std::fprintf(stderr, "LAUNCHER: no native window\n");
      return false;
    }
    gh2render::Config config;
    config.overlay_only = true;
    config.window.kind = nw.kind == gh2rt::NativeWindow::Kind::kWin32     ? gh2vk::WindowInfo::Kind::kWin32
                         : nw.kind == gh2rt::NativeWindow::Kind::kWayland ? gh2vk::WindowInfo::Kind::kWayland
                                                                          : gh2vk::WindowInfo::Kind::kXlib;
    config.window.display = nw.display;
    config.window.surface = nw.surface;
    config.window.width = nw.width;
    config.window.height = nw.height;
    std::string error;
    auto renderer = gh2render::Renderer::Create(config, &error);
    if (!renderer) {
      std::fprintf(stderr, "LAUNCHER: renderer: %s\n", error.c_str());
      return false;
    }
    renderer_ = renderer.get();
    gh2rt::gpu::AdoptOverlayRenderer(std::move(renderer));
    rex::cvar::SetFlagByName("present_mode", "fifo");   // the launcher waits for the display instead of drawing as fast as it can
    rex::cvar::SetFlagByName("msaa", "off");
    overlay_drawer_ = Gh2CreateNativeOverlayDrawer();
    imgui_ = std::make_unique<rex::ui::ImGuiDrawer>(window_.get(), 0, SetupFonts);
    imgui_->SetImmediateDrawer(overlay_drawer_.get());
    ImGuiIO& io = imgui_->GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    ApplyStyle();
    dialog_ = std::make_unique<LauncherDialog>(imgui_.get(), this);
    install_ = ReadInstall();
    settings_.Load();
    screen_ = install_.installed ? Screen::kHome : Screen::kSetup;
    if (const char* s = std::getenv("GH2_LAUNCHER_SCREEN")) {   // testing aid: open on a screen (home, setup, build, settings, playing)
      const std::string v = s;
      screen_ = v == "setup" ? Screen::kSetup : v == "build" ? Screen::kTask : v == "settings" ? Screen::kSettings : v == "playing" ? Screen::kPlaying : Screen::kHome;
      demo_ = v == "build" || v == "playing";
    }
    if (!std::getenv("GH2_NO_UPDATE_CHECK")) update_check_ = std::make_unique<Task>(Task::Kind::kCheckUpdate, std::vector<std::string>{"update"});
    if (const char* s = std::getenv("GH2_TEST_LAUNCHER_PLAY"); s && *s) {   // testing aid: press Play after N seconds
      const int secs = std::max(1, std::atoi(s));
      std::thread([this, secs] {
        std::this_thread::sleep_for(std::chrono::seconds(secs));
        app_context().CallInUIThread([this] {
          std::fprintf(stderr, "LAUNCHER test: Play\n");
          Play();
        });
      }).detach();
    }
    render_thread_ = std::thread([this] { RenderLoop(); });
    return true;
  }

  void OnDestroy() override {
    quit_ = true;
    tick_cv_.notify_all();
    if (render_thread_.joinable()) render_thread_.join();
    task_.reset();
    update_check_.reset();
    dialog_.reset();
    imgui_.reset();
  }

  // window events (UI thread)
  void OnClosing(rex::ui::UIEvent&) override {
    quit_ = true;
    app_context().QuitFromUIThread();
  }
  void OnResize(rex::ui::UISetupEvent&) override {
    if (window_) Gh2NativeWindowResized(window_->GetActualPhysicalWidth(), window_->GetActualPhysicalHeight());
  }
  void OnGotFocus(rex::ui::UISetupEvent&) override { focused_ = true; }
  void OnLostFocus(rex::ui::UISetupEvent&) override { focused_ = false; }
  void OnFileDrop(rex::ui::FileDropEvent& e) override { SetSource(e.filename().string()); }

  void Draw(ImGuiIO& io);

 private:
  enum class Screen { kHome, kSetup, kTask, kSettings, kPlaying };

  // A frame per display refresh, at most 60 per second; 10 while the window is in the background or the game runs.
  void RenderLoop() {
    auto next = Clock::now();
    while (!quit_) {
      {
        std::unique_lock<std::mutex> lock(tick_mutex_);
        tick_done_ = false;
        app_context().CallInUIThread([this] {
          if (!quit_ && imgui_ && window_)
            Gh2NativeOverlayTick(imgui_.get(), window_->GetActualPhysicalWidth(), window_->GetActualPhysicalHeight());
          std::lock_guard<std::mutex> l(tick_mutex_);
          tick_done_ = true;
          tick_cv_.notify_all();
        });
        tick_cv_.wait_for(lock, std::chrono::milliseconds(250), [this] { return tick_done_ || quit_.load(); });
      }
      if (quit_) break;
      renderer_->PresentOverlay();
      const bool slow = !focused_.load() || screen_ == Screen::kPlaying;
      next += std::chrono::microseconds(slow ? 100000 : 16667);
      const auto now = Clock::now();
      if (next < now) next = now;
      std::this_thread::sleep_until(next);
    }
  }

  void ApplyStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0;
    s.WindowBorderSize = 0;
    s.WindowPadding = ImVec2(0, 0);
    s.FrameRounding = 8;
    s.FrameBorderSize = 0;
    s.ItemSpacing = ImVec2(10, 12);
    s.FramePadding = ImVec2(16, 10);
    s.ScrollbarSize = 10;
    s.ScrollbarRounding = 6;
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = Rgb4(kStage, 0.0f);
    c[ImGuiCol_ChildBg] = Rgb4(kStage, 0.0f);
    c[ImGuiCol_Text] = Rgb4(kText);
    c[ImGuiCol_TextDisabled] = Rgb4(kSteel);
    c[ImGuiCol_Button] = Rgb4(kPanel, 0.95f);
    c[ImGuiCol_ButtonHovered] = Rgb4(kRaised);
    c[ImGuiCol_ButtonActive] = Rgb4(kDeepRed);
    c[ImGuiCol_FrameBg] = Rgb4(kTrough);
    c[ImGuiCol_NavCursor] = Rgb4(kGold);
    c[ImGuiCol_Border] = Rgb4(kText, 0.12f);
    c[ImGuiCol_ScrollbarBg] = Rgb4(kStage, 0.0f);
    c[ImGuiCol_ScrollbarGrab] = Rgb4(kTrough);
  }

  void PollGamepads(ImGuiIO& io) {   // guitar or pad: green/A activates, red/B goes back, strum/D-pad/stick move
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; i < count; ++i)
      if (!pads_.count(ids[i])) {
        pads_[ids[i]] = SDL_OpenGamepad(ids[i]);
        const char* name = SDL_GetGamepadNameForID(ids[i]);
        std::fprintf(stderr, "LAUNCHER controller %s: %s\n", pads_[ids[i]] ? "opened" : "failed to open", name ? name : "?");
      }
    SDL_free(ids);
    SDL_UpdateGamepads();
    bool b[6] = {false, false, false, false, false, false};
    for (auto& [id, pad] : pads_) {
      if (!pad) continue;
      b[0] |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH);
      b[1] |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST);
      b[2] |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP) || SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) < -20000;
      b[3] |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN) || SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) > 20000;
      b[4] |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
      b[5] |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    }
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, b[0]);
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, b[1]);
    io.AddKeyEvent(ImGuiKey_GamepadDpadUp, b[2]);
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown, b[3]);
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, b[4]);
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight, b[5]);
    static const bool log_pad = std::getenv("GH2_LAUNCHER_PAD_LOG") != nullptr;   // testing aid: what the launcher saw of the pad
    const int bits = b[0] | b[1] << 1 | b[2] << 2 | b[3] << 3 | b[4] << 4 | b[5] << 5;
    if (log_pad && bits != pad_bits_) {
      const ImGuiContext& g = *ImGui::GetCurrentContext();
      std::fprintf(stderr, "LAUNCHER pad %02x | sdl keyboard focus %d | imgui nav window %s id %08x cursor %d hovered %08x app focus lost %d\n", bits,
                   SDL_GetKeyboardFocus() != nullptr, g.NavWindow ? g.NavWindow->Name : "-", g.NavId, g.NavCursorVisible, g.HoveredId,
                   io.AppFocusLost);
    }
    pad_bits_ = bits;
    back_pressed_ = b[1] && !back_prev_;
    back_prev_ = b[1];
  }

  void SetSource(const std::string& path) {
    std::lock_guard<std::mutex> l(source_mutex_);
    source_ = path;
    if (screen_ == Screen::kHome && !install_.installed) screen_ = Screen::kSetup;
  }
  std::string source() {
    std::lock_guard<std::mutex> l(source_mutex_);
    return source_;
  }
  void PickFolder() {
    SDL_Window* w = FirstSdlWindow();
    SDL_ShowOpenFolderDialog(&LauncherApp::OnPicked, this, w, Home().c_str(), false);
  }
  void PickFile() {
    static const SDL_DialogFileFilter filters[] = {{"Disc image or default.xex", "iso;xex"}};
    SDL_ShowOpenFileDialog(&LauncherApp::OnPicked, this, FirstSdlWindow(), filters, 1, Home().c_str(), false);
  }
  static void SDLCALL OnPicked(void* self, const char* const* files, int) {
    auto* app = static_cast<LauncherApp*>(self);
    if (!files) {
      app->pick_error_ = "The file chooser could not open here. Drag the game folder or file onto this window instead.";
      return;
    }
    if (*files) app->SetSource(*files);
  }
  static SDL_Window* FirstSdlWindow() {
    int n = 0;
    SDL_Window** ws = SDL_GetWindows(&n);
    SDL_Window* w = ws && n > 0 ? ws[0] : nullptr;
    SDL_free(ws);
    return w;
  }

  void StartTask(Task::Kind kind, std::vector<std::string> args) {
    task_ = std::make_unique<Task>(kind, std::move(args));
    task_title_ = kind == Task::Kind::kInstall ? "Building the game" : kind == Task::Kind::kRebuild ? "Rebuilding the game" : kind == Task::Kind::kUpdate ? "Updating reGH2" : "Saving a problem report";
    screen_ = Screen::kTask;
  }
  void Play() {
    std::vector<std::string> args{"launch"};
    if (const char* extra = std::getenv("GH2_LAUNCHER_GAME_ARGS")) {   // options for the game (tests: --user_data_root=<a copy of the saves>)
      std::istringstream in(extra);
      for (std::string w; in >> w;) args.push_back(w);
    }
    play_ = std::make_unique<Task>(Task::Kind::kPlay, args);
    play_started_ = Clock::now();
    screen_ = Screen::kPlaying;
    flash_ = 1.0f;
  }

  // ---- widgets
  float k_ = 1.0f;   // the layout's scale: 1 at 1280x720
  void Text(ImFont* f, uint32_t color, const char* fmt, ...) IM_FMTARGS(4) {
    char buf[1024];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, a);
    va_end(a);
    ImGui::PushFont(f, f->LegacySize * k_);
    ImGui::PushStyleColor(ImGuiCol_Text, Rgb4(color));
    ImGui::TextWrapped("%s", buf);
    ImGui::PopStyleColor();
    ImGui::PopFont();
  }
  void Eyebrow(const char* text) {
    ImGui::PushFont(g_fonts.small, g_fonts.small->LegacySize * k_);
    ImGui::PushStyleColor(ImGuiCol_Text, Rgb4(kAmber));
    std::string up;
    for (const char* p = text; *p; ++p) up += char(std::toupper(static_cast<unsigned char>(*p)));
    ImGui::TextUnformatted(up.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
  }
  bool Button(const char* label, bool primary, float width = 0.0f) {
    ImGui::PushFont(g_fonts.ui, g_fonts.ui->LegacySize * k_);
    ImGui::PushStyleColor(ImGuiCol_Button, primary ? Rgb4(kGold) : Rgb4(kPanel, 0.9f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, primary ? Rgb4(0xffe27a) : Rgb4(kRaised));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, primary ? Rgb4(kAmber) : Rgb4(kDeepRed));
    ImGui::PushStyleColor(ImGuiCol_Text, primary ? Rgb4(kStage) : Rgb4(kSilver));
    const bool pressed = ImGui::Button(label, ImVec2(width, 0));
    ImGui::PopStyleColor(4);
    ImGui::PopFont();
    return pressed;
  }
  // The big red button: label left, a two-line note right.
  bool BigButton(const char* id, const char* label, const char* note1, const char* note2, bool enabled = true) {
    const float w = ImGui::GetContentRegionAvail().x, h = 76.0f * k_;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (!enabled) ImGui::BeginDisabled();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h), ImGuiButtonFlags_EnableNav);   // (ImGui leaves invisible buttons out of keyboard / gamepad navigation otherwise)
    if (!enabled) ImGui::EndDisabled();
    const bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = 10.0f * k_;
    if (hot) dl->AddRectFilled(p + ImVec2(-3, -3) * k_, p + ImVec2(w, h) + ImVec2(3, 3) * k_, Rgb(kAmber, 0.35f), r + 3);
    dl->AddRectFilled(p, p + ImVec2(w, h), Rgb(enabled ? (hot ? 0xe2281f : kRed) : kTrough), r);
    dl->AddRectFilled(p + ImVec2(2, 2), p + ImVec2(w - 2, h * 0.48f), Rgb(0xffffff, 0.10f), r);
    dl->AddRectFilled(p + ImVec2(0, h * 0.62f), p + ImVec2(w, h), Rgb(0x000000, 0.18f), r, ImDrawFlags_RoundCornersBottom);
    const float fs = g_fonts.big->LegacySize * k_;
    dl->AddText(g_fonts.big, fs, p + ImVec2(22 * k_, (h - fs) * 0.5f), Rgb(kText), label);
    const float ns = g_fonts.small->LegacySize * k_;
    const float n1 = g_fonts.small->CalcTextSizeA(ns, FLT_MAX, 0, note1).x, n2 = g_fonts.small->CalcTextSizeA(ns, FLT_MAX, 0, note2).x;
    dl->AddText(g_fonts.small, ns, p + ImVec2(w - 20 * k_ - n1, h * 0.5f - ns - 1), Rgb(kText, 0.85f), note1);
    dl->AddText(g_fonts.small, ns, p + ImVec2(w - 20 * k_ - n2, h * 0.5f + 1), Rgb(kText, 0.85f), note2);
    return pressed && enabled;
  }
  // A row of the home screen: a fret gem, a label, a note at the right.
  bool Row(const char* label, const char* note, int fret) {
    const float w = ImGui::GetContentRegionAvail().x, h = 44.0f * k_;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    const bool pressed = ImGui::InvisibleButton("row", ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    ImGui::PopID();
    const bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine(p, p + ImVec2(w, 0), Rgb(kText, 0.12f));
    if (hot) dl->AddRectFilled(p + ImVec2(0, 1), p + ImVec2(w, h), Rgb(kRaised, 0.7f), 4.0f);
    const ImVec2 gem = p + ImVec2(10 * k_, h * 0.5f);
    Glow(dl, gem, ImVec2(14, 14) * k_, Rgb(kFret[fret], hot ? 0.6f : 0.3f));
    dl->AddCircleFilled(gem, 6.5f * k_, Rgb(kFret[fret]));
    dl->AddCircleFilled(gem + ImVec2(0, -1.5f) * k_, 2.5f * k_, Rgb(kText, 0.8f));
    const float fs = g_fonts.ui->LegacySize * k_, ns = g_fonts.small->LegacySize * k_;
    dl->AddText(g_fonts.ui, fs, p + ImVec2(28 * k_, (h - fs) * 0.5f), Rgb(hot ? kText : kSilver), label);
    const float nw = g_fonts.small->CalcTextSizeA(ns, FLT_MAX, 0, note).x;
    dl->AddText(g_fonts.small, ns, p + ImVec2(w - nw - 6 * k_, (h - ns) * 0.5f), Rgb(kSteel), note);
    return pressed;
  }
  void Stats(const std::vector<std::pair<const char*, std::string>>& cells, int ok_cell = 0) {
    const float w = ImGui::GetContentRegionAvail().x, h = 58.0f * k_;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, p + ImVec2(w, h), Rgb(kText, 0.12f), 8.0f * k_);
    const float cw = (w - (cells.size() - 1)) / cells.size();
    for (size_t i = 0; i < cells.size(); ++i) {
      const ImVec2 a = p + ImVec2(i * (cw + 1), 0);
      dl->AddRectFilled(a + ImVec2(i == 0 ? 0 : 0, 0), a + ImVec2(cw, h), Rgb(kPanel, 0.96f),
                        8.0f * k_, i == 0 ? ImDrawFlags_RoundCornersLeft : i + 1 == cells.size() ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersNone);
      const float ks = g_fonts.small->LegacySize * k_ * 0.85f, vs = g_fonts.ui->LegacySize * k_;
      std::string key;
      for (const char* c = cells[i].first; *c; ++c) key += char(std::toupper(static_cast<unsigned char>(*c)));
      dl->AddText(g_fonts.small, ks, a + ImVec2(12 * k_, 9 * k_), Rgb(kSteel), key.c_str());
      dl->AddText(g_fonts.ui, vs, a + ImVec2(12 * k_, 27 * k_), Rgb(int(i) == ok_cell - 1 ? kFret[0] : kText), cells[i].second.c_str());
    }
    ImGui::Dummy(ImVec2(w, h));
  }
  // A segmented control: returns the chosen index.
  int Segments(const char* id, const char* const* labels, int n, int current) {
    ImGui::PushID(id);
    ImGui::PushFont(g_fonts.ui, g_fonts.ui->LegacySize * 0.85f * k_);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4 * k_, 4 * k_));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10 * k_, 6 * k_));
    int chosen = current;
    for (int i = 0; i < n; ++i) {
      if (i) ImGui::SameLine();
      const bool on = i == current;
      ImGui::PushStyleColor(ImGuiCol_Button, on ? Rgb4(kRed) : Rgb4(kTrough));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? Rgb4(0xe2281f) : Rgb4(kRaised));
      ImGui::PushStyleColor(ImGuiCol_Text, on ? Rgb4(kText) : Rgb4(kSteel));
      if (ImGui::Button(labels[i])) chosen = i;
      ImGui::PopStyleColor(3);
    }
    ImGui::PopStyleVar(2);
    ImGui::PopFont();
    ImGui::PopID();
    return chosen;
  }
  bool Switch(const char* id, bool on) {
    const float h = 24 * k_, w = 44 * k_;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, p + ImVec2(w, h), Rgb(on ? kDeepRed : kTrough), h * 0.5f);
    if (ImGui::IsItemFocused() || ImGui::IsItemHovered()) dl->AddRect(p, p + ImVec2(w, h), Rgb(kAmber), h * 0.5f, 0, 1.5f);
    dl->AddCircleFilled(p + ImVec2(on ? w - h * 0.5f : h * 0.5f, h * 0.5f), h * 0.36f, Rgb(on ? kGold : kSteel));
    return pressed;
  }
  void SettingLabel(const char* label, const char* hint) {
    ImGui::BeginGroup();
    Text(g_fonts.ui, kText, "%s", label);
    if (hint && *hint) {
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 10 * k_);
      Text(g_fonts.small, kSteel, "%s", hint);
    }
    ImGui::EndGroup();
  }

  // ---- screens
  void DrawHome();
  void DrawSetup();
  void DrawTask();
  void DrawSettings();
  void DrawPlaying();
  void DrawTicker(ImVec2 size);

  std::unique_ptr<rex::ui::Window> window_;
  std::unique_ptr<rex::ui::ImmediateDrawer> overlay_drawer_;
  std::unique_ptr<rex::ui::ImGuiDrawer> imgui_;
  std::unique_ptr<LauncherDialog> dialog_;
  gh2render::Renderer* renderer_ = nullptr;
  std::thread render_thread_;
  std::mutex tick_mutex_;
  std::condition_variable tick_cv_;
  bool tick_done_ = false;
  std::atomic<bool> quit_{false}, focused_{true};
  std::map<SDL_JoystickID, SDL_Gamepad*> pads_;
  bool back_pressed_ = false, back_prev_ = false;
  std::atomic<Screen> screen_{Screen::kHome};
  Clock::time_point start_ = Clock::now(), play_started_{};
  float flash_ = 0.0f;
  bool demo_ = false;
  bool focus_first_ = true;
  int pad_bits_ = 0;
  Screen last_screen_ = Screen::kHome;
  InstallInfo install_;
  Settings settings_;
  std::mutex source_mutex_;
  std::string source_;
  std::string pick_error_;
  std::unique_ptr<Task> task_, update_check_, play_, side_task_;
  std::string task_title_, side_message_, update_version_;
};

void LauncherDialog::OnDraw(ImGuiIO& io) { app_->Draw(io); }

void LauncherApp::Draw(ImGuiIO& io) {
  const ImVec2 size = io.DisplaySize;
  k_ = std::clamp(std::min(size.x / 1280.0f, size.y / 720.0f), 0.7f, 2.0f);
  PollGamepads(io);
  const bool still = std::getenv("GH2_LAUNCHER_STILL") != nullptr || screen_ == Screen::kPlaying;   // (a fixed picture for the frame dumps of the tests)
  const float now = still ? 3.1f : std::chrono::duration<float>(Clock::now() - start_).count();
  DrawStage(ImGui::GetBackgroundDrawList(), size, now, size.x < 760.0f, flash_);
  flash_ = std::max(0.0f, flash_ - io.DeltaTime * 1.6f);

  if (update_check_ && update_check_->done()) {   // "reGH2 0.3.1 is available (this is 0.3.0): <page>"
    const std::string line = update_check_->find_line(" is available ");
    if (!line.empty()) {
      const size_t a = line.find("reGH2 "), b = line.find(" is available");
      if (a != std::string::npos && b != std::string::npos && b > a + 6) update_version_ = line.substr(a + 6, b - a - 6);
    }
    update_check_.reset();
  }
  if (play_ && play_->done() && screen_ == Screen::kPlaying) {
    std::fprintf(stderr, "LAUNCHER: the game ended (exit code %d)\n", play_->exit_code());
    if (play_->exit_code() != 0) side_message_ = "The game stopped with an error (exit code " + std::to_string(play_->exit_code()) + "). A problem report collects its log.";
    screen_ = Screen::kHome;
  }
  if (side_task_ && side_task_->done()) {
    const std::string saved = side_task_->find_line("/");
    side_message_ = side_task_->exit_code() == 0 ? "Problem report saved: " + saved : "The problem report could not be saved.";
    side_task_.reset();
  }

  // the panel on the left, darkened towards the highway
  const float panel_w = std::min(size.x, (size.x < 760.0f ? size.x : 460.0f * k_));
  ImDrawList* bg = ImGui::GetBackgroundDrawList();
  bg->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(panel_w * 0.75f, size.y), Rgb(kStage, 0.95f), Rgb(kStage, 0.88f), Rgb(kStage, 0.88f), Rgb(kStage, 0.95f));
  bg->AddRectFilledMultiColor(ImVec2(panel_w * 0.75f, 0), ImVec2(panel_w + 80 * k_, size.y), Rgb(kStage, 0.88f), Rgb(kStage, 0.0f), Rgb(kStage, 0.0f), Rgb(kStage, 0.88f));

  ImGui::SetNextWindowPos(ImVec2(34 * k_, 30 * k_));
  ImGui::SetNextWindowSize(ImVec2(panel_w - 68 * k_, size.y - 60 * k_));
  ImGui::Begin("##launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                          ImGuiWindowFlags_NoBringToFrontOnFocus);
  const Screen screen = screen_;
  if (screen != last_screen_) {
    focus_first_ = true;
    last_screen_ = screen;
  }
  switch (screen) {
    case Screen::kHome: DrawHome(); break;
    case Screen::kSetup: DrawSetup(); break;
    case Screen::kTask: DrawTask(); break;
    case Screen::kSettings: DrawSettings(); break;
    case Screen::kPlaying: DrawPlaying(); break;
  }
  focus_first_ = false;
  ImGui::End();
  DrawTicker(size);
  const bool back_key = ImGui::IsKeyPressed(ImGuiKey_Escape, false) || back_pressed_;
  if (back_key && (screen == Screen::kSettings || (screen == Screen::kSetup && install_.installed))) screen_ = Screen::kHome;
}

void LauncherApp::DrawHome() {
  ImGui::PushFont(g_fonts.logo, g_fonts.logo->LegacySize * k_);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = g_fonts.logo->LegacySize * k_;
  Glow(ImGui::GetBackgroundDrawList(), p + ImVec2(105, 34) * k_, ImVec2(190, 80) * k_, Rgb(kRed, 0.30f));   // (outside the panel's clip rectangle)
  dl->AddText(g_fonts.logo, fs, p + ImVec2(0, 3 * k_), Rgb(kDeepRed), "re");
  const float re_w = g_fonts.logo->CalcTextSizeA(fs, FLT_MAX, 0, "re").x;
  dl->AddText(g_fonts.logo, fs, p + ImVec2(re_w, 3 * k_), Rgb(kDeepRed), "GH2");
  dl->AddText(g_fonts.logo, fs, p, Rgb(kText), "re");
  dl->AddText(g_fonts.logo, fs, p + ImVec2(re_w, 0), Rgb(kRed), "GH2");
  ImGui::Dummy(ImVec2(10, fs * 0.95f));
  ImGui::PopFont();
  Eyebrow("Native PC port · built from your copy");
  ImGui::Dummy(ImVec2(1, 6 * k_));

  if (focus_first_) ImGui::SetKeyboardFocusHere();
  if (demo_ || install_.installed) {
    const std::string when = install_.when.empty() ? "Ready" : "Built " + install_.when.substr(0, 10);
    if (BigButton("##play", "PLAY", "Green fret or Enter", when.c_str())) Play();
  } else if (BigButton("##setup", "SET UP", "Build the game from", "your own copy")) {
    screen_ = Screen::kSetup;
  }
  ImGui::Dummy(ImVec2(1, 2 * k_));
  const std::string version = install_.version.empty() ? "-" : install_.version;
  Stats({{"Game", install_.installed ? std::string("Built") : std::string("Not built")}, {"Version", version}, {"Disc", install_.xex.empty() ? std::string("-") : install_.xex}},
        install_.installed ? 1 : 0);
  ImGui::Dummy(ImVec2(1, 4 * k_));
  if (Row("Settings", "Resolution, fullscreen, intro", 0)) {
    settings_.Load();
    screen_ = Screen::kSettings;
  }
  if (Row("Saves", "Open the folder", 1)) gh2os::OpenWithDesktop(DataHome() + "/gh2");
  const std::string upd = update_version_.empty() ? std::string("You have the newest version") : "Version " + update_version_ + " is out";
  if (Row(update_version_.empty() ? "Updates" : "Update available", upd.c_str(), 2) && !update_version_.empty()) StartTask(Task::Kind::kUpdate, {"update", "--apply"});
  if (Row("Problem report", side_task_ ? "Saving..." : "For a GitHub issue", 3) && !side_task_)
    side_task_ = std::make_unique<Task>(Task::Kind::kReport, std::vector<std::string>{"report"});
  if (Row(install_.installed ? "Rebuild the game" : "Install from your copy", install_.installed ? "After an update, or to repair" : "ISO, folder or default.xex", 4)) {
    if (install_.installed && !install_.game_dir.empty()) StartTask(Task::Kind::kRebuild, {"install", install_.game_dir, "--rebuild"});
    else screen_ = Screen::kSetup;
  }
  if (!side_message_.empty()) {
    ImGui::Dummy(ImVec2(1, 4 * k_));
    Text(g_fonts.small, kDim, "%s", side_message_.c_str());
  }
}

void LauncherApp::DrawSetup() {
  Eyebrow("First start");
  Text(g_fonts.heading, kText, "Build your game");
  Text(g_fonts.body, kDim, "reGH2 ships no part of the game. It translates your own copy of Guitar Hero II (Xbox 360) into a native program on this PC.");
  ImGui::Dummy(ImVec2(1, 4 * k_));
  const std::string src = source();
  struct Step {
    const char* title;
    std::string text;
    int state;   // 0 later, 1 now, 2 done
  };
  const Step steps[] = {
      {"Your game copy", src.empty() ? std::string("A disc image (.iso), the extracted game folder, or its default.xex. You can drag it onto this window.") : src, src.empty() ? 1 : 2},
      {"Graphics driver", "Vulkan works on this PC: this window is drawn with it.", 2},
      {"Build the game", "About 1 to 2 minutes. Nothing is downloaded: the compiler travels with reGH2.", src.empty() ? 0 : 1},
      {"Shaders", "Made at the first start from the game's own files (about 12 s).", 0},
  };
  ImDrawList* dl = ImGui::GetWindowDrawList();
  for (int i = 0; i < 4; ++i) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float d = 30 * k_;
    if (i < 3) dl->AddLine(p + ImVec2(d * 0.5f, d + 2), p + ImVec2(d * 0.5f, d + 56 * k_), Rgb(kTrough), 2.0f * k_);
    const ImVec2 c = p + ImVec2(d * 0.5f, d * 0.5f);
    const Step& s = steps[i];
    if (s.state == 2) {   // a tick
      dl->AddCircleFilled(c, d * 0.5f, Rgb(kFret[0]));
      const ImVec2 tick[3] = {c + ImVec2(-6.5f, 0.5f) * k_, c + ImVec2(-2, 5) * k_, c + ImVec2(7, -5) * k_};
      dl->AddPolyline(tick, 3, Rgb(kStage), ImDrawFlags_None, 3.0f * k_);
    } else if (s.state == 1) {
      Glow(dl, c, ImVec2(d, d), Rgb(kGold, 0.35f));
      dl->AddCircle(c, d * 0.5f - 1, Rgb(kGold), 0, 2.0f * k_);
      dl->AddText(g_fonts.ui, g_fonts.ui->LegacySize * k_, c + ImVec2(-5, -12) * k_, Rgb(kGold), std::to_string(i + 1).c_str());
    } else {
      dl->AddCircleFilled(c, d * 0.5f, Rgb(kTrough));
      dl->AddText(g_fonts.ui, g_fonts.ui->LegacySize * k_, c + ImVec2(-5, -12) * k_, Rgb(kSteel), std::to_string(i + 1).c_str());
    }
    ImGui::SetCursorScreenPos(p + ImVec2(d + 14 * k_, -2 * k_));
    ImGui::BeginGroup();
    Text(g_fonts.ui, s.state == 0 ? kSteel : kText, "%s", s.title);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 10 * k_);
    Text(g_fonts.small, kDim, "%s", s.text.c_str());
    ImGui::EndGroup();
    ImGui::SetCursorScreenPos(ImVec2(p.x, std::max(ImGui::GetItemRectMax().y + 10 * k_, p.y + 66 * k_)));
  }
  if (focus_first_) ImGui::SetKeyboardFocusHere();
  if (Button("Choose folder", src.empty())) PickFolder();
  ImGui::SameLine();
  if (Button("Choose .iso or .xex", false)) PickFile();
  if (!src.empty()) {
    ImGui::SameLine();
    if (Button("Build now", true)) StartTask(Task::Kind::kInstall, {"install", src});
  }
  if (!pick_error_.empty()) Text(g_fonts.small, kAmber, "%s", pick_error_.c_str());
  if (install_.installed) {
    ImGui::Dummy(ImVec2(1, 4 * k_));
    if (Button("Back", false)) screen_ = Screen::kHome;
  }
}

void LauncherApp::DrawTask() {
  static const char* kStages[][2] = {{"Looking for the game", "Find default.xex"},          {"Checking the game executable", "Check default.xex"},
                                     {"Recompiling the game", "Translate PowerPC to C++"}, {"Compiling", "Compile the game"},
                                     {"Done", "Shortcuts and saves"}};
  const bool demo = demo_ && !task_;
  const float elapsed = demo ? 41.0f : float(task_ ? task_->elapsed() : 0.0);
  const float progress = demo ? 0.62f : (task_ ? task_->progress() : 0.0f);
  const std::string stage = demo ? "Compiling 58/109" : (task_ ? task_->stage() : std::string());
  const bool done = !demo && task_ && task_->done();
  const bool ok = done && task_->exit_code() == 0 && !task_->cancelled();
  Eyebrow(demo ? "Building the game" : task_title_.c_str());
  // the odometer
  char pct[16];
  std::snprintf(pct, sizeof pct, "%d", int(progress * 100.0f + 0.5f));
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float fs = g_fonts.big->LegacySize * 2.1f * k_;
  ImGui::GetWindowDrawList()->AddText(g_fonts.big, fs, p, Rgb(done && !ok ? kRed : kText), pct);
  const float pw = g_fonts.big->CalcTextSizeA(fs, FLT_MAX, 0, pct).x;
  ImGui::GetWindowDrawList()->AddText(g_fonts.big, fs * 0.4f, p + ImVec2(pw + 4 * k_, fs * 0.12f), Rgb(kAmber), "%");
  ImGui::Dummy(ImVec2(1, fs * 0.92f));
  // the meter, in the fret colours
  {
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, h = 10 * k_;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, a + ImVec2(w, h), Rgb(kTrough), h * 0.5f);
    const float fill = w * progress;
    for (int i = 0; i < 4 && fill > 0; ++i) {
      const float x0 = w * i / 4.0f, x1 = std::min(fill, w * (i + 1) / 4.0f);
      if (x1 > x0) dl->AddRectFilledMultiColor(a + ImVec2(x0, 0), a + ImVec2(x1, h), Rgb(kFret[i]), Rgb(kFret[i + 1]), Rgb(kFret[i + 1]), Rgb(kFret[i]));
    }
    ImGui::Dummy(ImVec2(w, h));
  }
  ImGui::Dummy(ImVec2(1, 2 * k_));
  // the stages: done, now, later
  int current = 0;
  for (int i = 0; i < 5; ++i)
    if (stage.find(kStages[i][0]) == 0) current = i;
  if (stage.rfind("Preparing", 0) == 0) current = stage.find("build") != std::string::npos ? 3 : 2;
  if (ok) current = 5;
  for (int i = 0; i < 5; ++i) {
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const float s = 8 * k_, mid = a.y + 12 * k_;
    const uint32_t col = i < current ? kFret[0] : i == current ? kGold : kTrough;
    const ImVec2 c(a.x + s, mid);
    ImGui::GetWindowDrawList()->AddQuadFilled(c + ImVec2(0, -s * 0.7f), c + ImVec2(s * 0.7f, 0), c + ImVec2(0, s * 0.7f), c + ImVec2(-s * 0.7f, 0), Rgb(col));
    ImGui::SetCursorScreenPos(a + ImVec2(26 * k_, 0));
    const std::string label = i == 3 && current == 3 && stage.rfind("Compiling", 0) == 0 ? "Compile the game  (" + stage.substr(10) + ")" : kStages[i][1];
    Text(g_fonts.ui, i < current ? kSilver : i == current ? kGold : kSteel, "%s", label.c_str());
  }
  char line[96];
  std::snprintf(line, sizeof line, "%d:%02d", int(elapsed) / 60, int(elapsed) % 60);
  Text(g_fonts.small, kSteel, "Elapsed %s", line);
  // the last lines of the log
  {
    std::vector<std::string> tail = demo ? std::vector<std::string>{"$ rexglue codegen gh2_manifest.toml", "spin hints patched in 104 files",
                                                                      "building the game module with 6 parallel jobs", "[57/109] Building CXX object obj/gh2_recomp.55.o"}
                                         : (task_ ? task_->tail(5) : std::vector<std::string>{});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Rgb4(0x000000, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6 * k_);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8) * k_);
    ImGui::BeginChild("##log", ImVec2(0, 104 * k_), ImGuiChildFlags_AlwaysUseWindowPadding);
    for (const std::string& l : tail) {
      std::string s = l.size() > 70 ? l.substr(0, 67) + "..." : l;
      Text(g_fonts.small, kDim, "%s", s.c_str());
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 12 * k_);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
  }
  if (focus_first_) ImGui::SetKeyboardFocusHere();
  if (!done) {
    if (Button("Cancel", false) && task_) task_->Cancel();
  } else if (ok) {
    Text(g_fonts.ui, kFret[0], "%s", task_->kind() == Task::Kind::kUpdate ? "Updated. Close and open reGH2 again to use the new version." : "The game is ready.");
    install_ = ReadInstall();
    if (task_->kind() != Task::Kind::kUpdate && Button("Play", true)) Play();
    ImGui::SameLine();
    if (Button("Home", false)) screen_ = Screen::kHome;
  } else {
    Text(g_fonts.ui, kRed, "%s", task_->cancelled() ? "Cancelled." : "This step failed. The lines above say why; a problem report has the whole log.");
    if (Button("Back", false)) screen_ = install_.installed ? Screen::kHome : Screen::kSetup;
  }
}

void LauncherApp::DrawSettings() {
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10 * k_, 7 * k_));
  Eyebrow("Settings");
  Text(g_fonts.heading, kText, "How the game runs");
  Text(g_fonts.small, kDim, "The same settings as the in-game menu (F2). They apply the next time the game starts.");
  ImGui::Dummy(ImVec2(1, 6 * k_));
  const float right = ImGui::GetContentRegionAvail().x;
  auto row = [&](const char* label, const char* hint) {
    ImGui::SetNextItemWidth(right);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(p, p + ImVec2(right, 0), Rgb(kText, 0.12f));
    ImGui::Dummy(ImVec2(1, 4 * k_));
    SettingLabel(label, hint);
  };
  if (focus_first_) ImGui::SetKeyboardFocusHere();
  {
    static const char* kRes[] = {"720p", "1080p", "1440p", "4K", "Window"};
    static const char* kResValues[] = {"720p", "1080p", "1440p", "2160p", "auto"};
    const std::string v = settings_.Get("render_resolution", "720p");
    int cur = 0;
    for (int i = 0; i < 5; ++i)
      if (v == kResValues[i]) cur = i;
    row("Resolution", "What the game renders at (720p is the console's own picture)");
    const int c = Segments("res", kRes, 5, cur);
    if (c != cur) settings_.Set("render_resolution", kResValues[c], true);
  }
  {
    static const char* kAa[] = {"Auto", "On", "Off"};
    static const char* kAaValues[] = {"auto", "on", "off"};
    const std::string v = settings_.Get("msaa", "auto");
    int cur = 0;
    for (int i = 0; i < 3; ++i)
      if (v == kAaValues[i]) cur = i;
    row("Anti-aliasing", "The game's own MSAA (Auto turns it off at 4K)");
    const int c = Segments("msaa", kAa, 3, cur);
    if (c != cur) settings_.Set("msaa", kAaValues[c], true);
  }
  {
    static const char* kStart[] = {"Logos and intro", "Straight to the menu"};
    static const char* kStartValues[] = {"normal", "menu"};
    const int cur = settings_.Get("startup_mode", "normal") == "menu" ? 1 : 0;
    row("When the game starts", nullptr);
    const int c = Segments("start", kStart, 2, cur);
    if (c != cur) settings_.Set("startup_mode", kStartValues[c], true);
  }
  struct Toggle {
    const char* key;
    const char* label;
    const char* hint;
    bool fallback;
  };
  static const Toggle kToggles[] = {{"fullscreen", "Fullscreen", "Borderless, on the monitor the window is on", false},
                                    {"controller_guitars_first", "Guitars before gamepads", "A guitar becomes player 1 when both are plugged in", true},
                                    {"show_fps", "Show the frame rate", "A small counter in a corner of the game", false}};
  for (const Toggle& t : kToggles) {
    const bool on = settings_.GetBool(t.key, t.fallback);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    row(t.label, t.hint);
    const ImVec2 after = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(p.x + right - 46 * k_, p.y + 16 * k_));
    if (Switch(t.key, on)) settings_.Set(t.key, on ? "false" : "true", false);
    ImGui::SetCursorScreenPos(after);
  }
  ImGui::Dummy(ImVec2(1, 2 * k_));
  if (Button("Back", true)) screen_ = Screen::kHome;
  ImGui::PopStyleVar();
}

void LauncherApp::DrawPlaying() {
  Eyebrow("Now playing");
  Text(g_fonts.heading, kText, "Guitar Hero II");
  const float secs = demo_ && !play_ ? 754.0f : std::chrono::duration<float>(Clock::now() - play_started_).count();
  Text(g_fonts.body, kDim, "The game is running in its own window. This one waits and comes back when the game closes.");
  char t[32];
  std::snprintf(t, sizeof t, "%d:%02d", int(secs) / 60, int(secs) % 60);
  ImGui::Dummy(ImVec2(1, 6 * k_));
  Stats({{"Session", t}, {"Resolution", settings_.Get("render_resolution", "720p")}, {"Fullscreen", settings_.GetBool("fullscreen", false) ? "Yes" : "No"}});
}

void LauncherApp::DrawTicker(ImVec2 size) {
  const char* head = "Tip";
  const char* text = "Strum to move, green fret to choose, red to go back";
  switch (screen_.load()) {
    case Screen::kSetup: head = "Your copy"; text = "ISO, extracted folder or default.xex"; break;
    case Screen::kTask: head = "Nothing downloaded"; text = "clang 20 travels inside reGH2"; break;
    case Screen::kSettings: head = "Same settings"; text = "as the F2 menu inside the game"; break;
    case Screen::kPlaying: head = "Rock on"; text = "Close the game to come back here"; break;
    default: break;
  }
  if (size.x < 760.0f) return;
  const float fs = g_fonts.small->LegacySize * k_;
  const std::string upper_head = [&] {
    std::string s;
    for (const char* c = head; *c; ++c) s += char(std::toupper(static_cast<unsigned char>(*c)));
    return s;
  }();
  const std::string body = "  ·  " + std::string(text);
  const float w = g_fonts.small->CalcTextSizeA(fs, FLT_MAX, 0, upper_head.c_str()).x + g_fonts.small->CalcTextSizeA(fs, FLT_MAX, 0, body.c_str()).x + 26 * k_;
  const ImVec2 b(size.x - 20 * k_, size.y - 18 * k_), a(b.x - w, b.y - fs - 16 * k_);
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddRectFilled(a, b, Rgb(kStage, 0.78f), 6 * k_);
  dl->AddRect(a, b, Rgb(kText, 0.12f), 6 * k_);
  const float hw = g_fonts.small->CalcTextSizeA(fs, FLT_MAX, 0, upper_head.c_str()).x;
  dl->AddText(g_fonts.small, fs, a + ImVec2(13 * k_, 8 * k_), Rgb(kGold), upper_head.c_str());
  dl->AddText(g_fonts.small, fs, a + ImVec2(13 * k_ + hw, 8 * k_), Rgb(kSilver), body.c_str());
}

}  // namespace

std::unique_ptr<rex::ui::WindowedApp> Gh2CreateLauncherApp(rex::ui::WindowedAppContext& context) { return std::make_unique<LauncherApp>(context); }
