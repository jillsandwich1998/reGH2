#include "shader_bootstrap.h"

#include <gh2rt/render_menu.h>
#include <imgui.h>
#include <rex/cvar.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <gh2os/os.h>
#ifndef _WIN32
#include <unistd.h>   // access
#endif

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

REXCVAR_DEFINE_BOOL(shader_autobuild, true, "GH2",
                    "Make the translated shaders (shaders.pak) from the game when they are missing or from an older version, at start-up, with a progress window (the native renderer needs them)");

namespace {

namespace fs = std::filesystem;

constexpr uint32_t kPakVersion = 2;   // keep equal to kVersion of native/gfx/shader_cache/shader_cache.cpp

fs::path ExeDir() { return fs::path(gh2os::ExeDir()); }   // (/proc/self/exe | GetModuleFileName)

// native/tools: beside the executable (installed), or where this build was made from.
fs::path FindToolsDir() {
  std::vector<fs::path> candidates;
  if (const char* e = std::getenv("GH2_NATIVE_TOOLS"); e && *e) candidates.emplace_back(e);
  const fs::path exe = ExeDir();
  if (!exe.empty()) {
    candidates.push_back(exe / "shader-tools" / "native" / "tools");   // the installer's copy of the shader tools bundle (packaging/build-shader-tools.sh)
    candidates.push_back(exe / "native" / "tools");
    candidates.push_back(exe / "tools");
    candidates.push_back(exe.parent_path() / "share" / "gh2" / "native" / "tools");
  }
#ifdef GH2_NATIVE_DIR
  candidates.push_back(fs::path(GH2_NATIVE_DIR) / "tools");
#endif
  for (const fs::path& c : candidates)
    if (fs::exists(c / "shader_pipeline.py")) return c;
  return {};
}

// The Python that runs the shader pipeline: GH2_PYTHON (the AppImage's portable one, set by the installer's launcher), else python3 on the PATH
// (Windows: python.exe, or the `py -3` launcher). Returns the command and its leading arguments, empty if there is none.
std::vector<std::string> FindPython();

#ifdef _WIN32
constexpr char kPathSeparator = ';';
#else
constexpr char kPathSeparator = ':';
#endif

bool IsExecutableFile(const fs::path& p) {
  std::error_code ec;
  if (!fs::is_regular_file(p, ec)) return false;
#ifdef _WIN32
  return true;
#else
  return access(p.c_str(), X_OK) == 0;
#endif
}

bool OnPath(const char* program) {
  const char* path = std::getenv("PATH");
  if (!path) return false;
  std::string list = path;
  size_t at = 0;
  while (at <= list.size()) {
    size_t end = list.find(kPathSeparator, at);
    if (end == std::string::npos) end = list.size();
    const std::string dir = list.substr(at, end - at);
    if (!dir.empty()) {
      fs::path candidate = fs::path(dir) / program;
#ifdef _WIN32
      candidate += ".exe";
#endif
      if (IsExecutableFile(candidate)) return true;
    }
    at = end + 1;
  }
  return false;
}

std::vector<std::string> FindPython() {
  if (const char* e = std::getenv("GH2_PYTHON"); e && *e && IsExecutableFile(e)) return {e};
#ifdef _WIN32
  if (OnPath("python")) return {"python"};
  if (OnPath("py")) return {"py", "-3"};
  return {};
#else
  return OnPath("python3") ? std::vector<std::string>{"python3"} : std::vector<std::string>{};
#endif
}

fs::path DataHome() { return fs::path(gh2os::DataDir()); }   // $XDG_DATA_HOME/gh2 or ~/.local/share/gh2 | %LOCALAPPDATA%\gh2


// ---- the build, in the background ---------------------------------------------------------------------------------------------------------------------------------------------------

struct Stage {
  const char* name;
  const char* label;
  double from, to;    // share of the bar this stage covers
  double expected_s;  // how long it usually takes (the bar creeps towards `to` over that time)
};
const Stage kStages[] = {{"extract", "Reading the game's shaders", 0.00, 0.12, 4}, {"xex", "Looking for shaders in the executable", 0.12, 0.15, 1}, {"recompile", "Translating the shaders for the graphics card", 0.15, 0.96, 14},
                         {"validate", "Checking the translated shaders", 0.96, 0.97, 1},   {"cache", "Saving", 0.97, 1.00, 1}};

struct Build {
  std::atomic<int> state{0};   // 0 not started, 1 running, 2 done, 3 failed
  std::atomic<int> stage{-1};
  std::atomic<int64_t> start_ns{0}, stage_start_ns{0}, end_ns{0};
  std::mutex mutex;
  std::string error;           // under mutex
};
Build g_build;
std::vector<std::string> g_planned_args;   // the pipeline's command line, waiting for the executable's image
fs::path g_planned_work;
std::string g_planned_dir;
std::mutex g_planned_mutex;
constexpr uint32_t kImageBase = 0x82000000u, kImageBytes = 9109504u;   // default.xex of the supported version (CLAUDE.md: PE image, 9109504 bytes at 0x82000000)

int64_t NowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

fs::path FailureMarker() { return DataHome() / "shader_build_failed"; }

void RunBuild(std::vector<std::string> args, fs::path work, std::string dir) {
  auto fail = [&](const std::string& why) {
    {
      std::lock_guard<std::mutex> lock(g_build.mutex);
      g_build.error = why;
    }
    std::fprintf(stderr, "SHADERS: %s\n", why.c_str());
    std::error_code ec;
    fs::create_directories(DataHome(), ec);
    std::ofstream(FailureMarker()) << why << "\n";   // the next start does not try again (it would only show the same dialog): delete this file to retry
    g_build.end_ns = NowNs();
    g_build.state = 3;
  };
  // The pipeline runs in a process group / job of its own (so that it can end everything it started when this process goes away: GH2_PARENT_PID, native/tools/shader_pipeline.py).
  // On POSIX a Ctrl+C in the terminal therefore no longer reaches it directly: the pipeline notices that its parent is gone within half a second. On Windows closing this process closes
  // the job object and ends the whole tree.
  gh2os::SpawnOptions spawn;
  spawn.argv = args;
  spawn.env_unset = {"LD_LIBRARY_PATH", "LD_PRELOAD"};   // the game's libraries must not reach the portable Python
  spawn.env_set = {{"GH2_PARENT_PID", std::to_string(gh2os::CurrentProcessId())}};
  gh2os::Process pipeline;
  std::string spawn_error;
  if (!pipeline.Start(spawn, &spawn_error)) return fail("cannot start the shader pipeline: " + spawn_error);
  std::string pending, last_error;
  char buf[2048];
  for (int n; (n = pipeline.ReadOutput(buf, sizeof buf, 1000)) >= 0;) {
    for (int i = 0; i < n; ++i) {
      if (buf[i] == '\r') continue;   // (Windows line ends)
      if (buf[i] != '\n') {
        pending += buf[i];
        continue;
      }
      if (pending.rfind("STAGE ", 0) == 0) {
        const std::string name = pending.substr(6);
        for (int k = 0; k < 5; ++k)
          if (name == kStages[k].name) {
            g_build.stage_start_ns = NowNs();
            g_build.stage = k;
          }
        std::fprintf(stderr, "SHADERS: %s\n", name.c_str());
      } else if (!pending.empty()) {
        if (pending.rfind("ERROR", 0) == 0 || pending.find("Error") != std::string::npos || pending.find("Traceback") != std::string::npos) last_error = pending;
        std::fprintf(stderr, "SHADERS:   %s\n", pending.c_str());
      }
      pending.clear();
    }
  }
  int exit_code = -1;
  std::string how;
  while (!pipeline.Poll(&exit_code, &how)) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  std::error_code ec;
  if (!(exit_code == 0 && fs::exists(work / "cache" / "shaders.pak")))
    return fail(std::string("the shader pipeline failed (") + how + ")" + (last_error.empty() ? "" : ": " + last_error));
  fs::create_directories(dir, ec);
  for (const char* name : {"shaders.pak", "manifest.json", "index.tsv"}) {
    const fs::path from = work / "cache" / name, to = fs::path(dir) / name, tmp = fs::path(dir) / (std::string(name) + ".tmp");
    if (!fs::exists(from)) continue;
    fs::copy_file(from, tmp, fs::copy_options::overwrite_existing, ec);
    if (!ec) fs::rename(tmp, to, ec);
  }
  fs::remove_all(work, ec);
  if (!Gh2ShaderCacheCurrent(dir)) return fail("the shader cache could not be saved in " + dir);
  g_build.end_ns = NowNs();
  g_build.state = 2;
  std::fprintf(stderr, "SHADERS: shaders.pak made in %.1f s\n", (g_build.end_ns - g_build.start_ns) / 1e9);
  if (auto* renderer = gh2rt::gpu::ActiveRenderer()) renderer->ReloadShaderCache(dir);   // otherwise the renderer, not made yet, finds the file itself
}

// The dialog: what is happening, a bar, and what to do if it failed.
class ShaderBuildDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit ShaderBuildDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    const int state = g_build.state.load();
    const int64_t now = NowNs();
    if (state == 2 && now - g_build.end_ns.load() > 3'000'000'000ll) {   // let it be read, then go
      Close();
      return;
    }
    const float k = std::min(3.0f, std::max(1.0f, std::round(io.DisplaySize.y / 720.0f * 4.0f) / 4.0f));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(640.0f * k, 0.0f), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.04f, 0.02f, 0.02f, 0.96f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.80f, 0.11f, 0.10f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.98f, 0.50f, 0.10f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f * k);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f * k, 16.0f * k));
    ImGui::Begin("##shaderbuild", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);
    ImGui::SetWindowFontScale(k);
    ImGui::TextColored(ImVec4(1.0f, 0.83f, 0.25f, 1.0f), "GUITAR HERO II  -  FIRST START");
    ImGui::Spacing();
    if (state == 3) {
      std::string why;
      {
        std::lock_guard<std::mutex> lock(g_build.mutex);
        why = g_build.error;
      }
      ImGui::TextWrapped("The graphics could not be prepared, so nothing can be drawn with the new renderer this time.");
      ImGui::TextWrapped("Close the game and start it again: it will use the compatibility renderer. (%s)", why.c_str());
      ImGui::TextDisabled("To try again later, delete %s", FailureMarker().string().c_str());
    } else {
      double fraction = state == 2 ? 1.0 : 0.0;
      const char* label = state == 2 ? "Ready" : "Starting";
      const int stage = g_build.stage.load();
      if (state != 2 && stage >= 0) {
        const Stage& s = kStages[stage];
        const double in_stage = (now - g_build.stage_start_ns.load()) / 1e9;
        fraction = s.from + (s.to - s.from) * (1.0 - std::exp(-in_stage / s.expected_s));   // creeps towards the end of the stage, never past it
        label = s.label;
      }
      ImGui::TextWrapped("Preparing the graphics from your game. This happens only once and takes about 15 seconds; the picture appears when it is done.");
      ImGui::Spacing();
      ImGui::Text("%s", label);
      char text[32];
      std::snprintf(text, sizeof text, "%.0f%%", fraction * 100.0);
      ImGui::ProgressBar(static_cast<float>(fraction), ImVec2(-1.0f, 24.0f * k), text);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);
  }
};

}  // namespace

bool Gh2ShaderCacheCurrent(const std::string& dir) {
  if (dir.empty()) return false;
  std::ifstream f(fs::path(dir) / "shaders.pak", std::ios::binary);
  char head[12] = {};
  if (!f.read(head, sizeof head)) return false;
  uint32_t version = 0;
  std::memcpy(&version, head + 8, 4);
  return std::memcmp(head, "GH2SHDR1", 8) == 0 && version == kPakVersion;
}

bool Gh2ShaderBuildBegin(const std::string& dir) {
  if (Gh2ShaderCacheCurrent(dir) || dir.empty()) return false;
  const char* off = std::getenv("GH2_SHADER_AUTOBUILD");
  if ((off && std::strcmp(off, "0") == 0) || !REXCVAR_GET(shader_autobuild)) return false;
  std::error_code ec;
  if (fs::exists(FailureMarker(), ec)) return std::fprintf(stderr, "SHADERS: the last attempt to make shaders.pak failed (%s exists): the compatibility renderer is used\n", FailureMarker().string().c_str()), false;

  const fs::path tools = FindToolsDir();
  const fs::path game_root = rex::cvar::GetFlagByName("game_data_root");
  const fs::path gen = game_root / "gen";
  if (tools.empty()) return std::fprintf(stderr, "SHADERS: no shaders.pak in '%s' and the shader tools (native/tools/shader_pipeline.py) were not found: set GH2_NATIVE_TOOLS\n", dir.c_str()), false;
  if (game_root.empty() || !fs::exists(gen)) return std::fprintf(stderr, "SHADERS: cannot make shaders.pak: the game's folder '%s' has no gen/ (is --game_data_root set?)\n", game_root.string().c_str()), false;
  const std::vector<std::string> python = FindPython();
  if (python.empty()) return std::fprintf(stderr, "SHADERS: cannot make shaders.pak: python3 is not installed (and GH2_PYTHON is not set)\n"), false;
#ifdef _WIN32
  const char* kXenosRecompBinary = "third_party/build/xenosrecomp/XenosRecomp/XenosRecomp.exe";
#else
  const char* kXenosRecompBinary = "third_party/build/xenosrecomp/XenosRecomp/XenosRecomp";
#endif
  if (!fs::exists(tools.parent_path() / kXenosRecompBinary)) return std::fprintf(stderr, "SHADERS: cannot make shaders.pak: the XenosRecomp binary is not built (native/tools/build_xenosrecomp.sh)\n"), false;

  const fs::path work = DataHome() / "shader_build";
  fs::remove_all(work, ec);
  fs::create_directories(work, ec);
  std::vector<std::string> args = python;   // (the child's environment is edited by RunBuild: no LD_LIBRARY_PATH / LD_PRELOAD, GH2_PARENT_PID set)
  args.push_back("-u");
  for (const std::string& a : {(tools / "shader_pipeline.py").string(), std::string("--gen-dir"), gen.string(), std::string("--out"), work.string(), std::string("--stages"), std::string("extract,xex,recompile,cache"),
                              std::string("--no-build")})   // no `validate`: spirv-val is a developer check, a player may not have it
    args.push_back(a);
  std::fprintf(stderr, "SHADERS: %s: making it from the game in the background (first start, about 15 s)\n", fs::exists(fs::path(dir) / "shaders.pak") ? "shaders.pak is from an older version" : "no shaders.pak");
  {
    std::lock_guard<std::mutex> lock(g_planned_mutex);
    g_planned_args = std::move(args);
    g_planned_work = work;
    g_planned_dir = dir;
  }
  g_build.start_ns = NowNs();
  g_build.state = 1;   // planned: it starts when the executable is in memory (Gh2ShaderBuildStart)
  return true;
}

void Gh2ShaderBuildStart(const unsigned char* guest_base) {
  std::vector<std::string> args;
  fs::path work;
  std::string dir;
  {
    std::lock_guard<std::mutex> lock(g_planned_mutex);
    if (g_planned_args.empty()) return;
    args = std::move(g_planned_args);
    g_planned_args.clear();
    work = g_planned_work;
    dir = g_planned_dir;
  }
  fs::path pe;
  if (const char* e = std::getenv("GH2_SHADER_PE"); e && *e) {
    pe = e;
  } else if (guest_base) {   // the decoded executable, as the runtime loaded it: the same bytes the PE file has
    pe = work / "default.pe";
    std::ofstream out(pe, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(guest_base + kImageBase), kImageBytes);
    if (!out) pe.clear();
  }
  if (!pe.empty()) {
    args.push_back("--pe");
    args.push_back(pe.string());
  } else {
    std::fprintf(stderr, "SHADERS: no executable image: the three shaders of the intro video are left out\n");
  }
  g_build.start_ns = NowNs();
  std::thread(RunBuild, std::move(args), work, dir).detach();
}

void Gh2ShaderBuildShowDialog(rex::ui::ImGuiDrawer* drawer) {
  if (g_build.state.load() == 0) return;
  new ShaderBuildDialog(drawer);   // owns itself until it closes
}
