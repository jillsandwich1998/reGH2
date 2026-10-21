// In-game options menu of the GH2 port (F2): graphics and downloadable content (DLC).
//
// Everything graphical is OPT-IN: the defaults reproduce the console's own rendering (720p, the game's MSAA, no extra
// filtering). Output filters (CAS / FSR 1) are applied at presentation time and take effect at once; the internal
// resolution scale needs a restart. Settings are ordinary SDK cvars, so they can also be given on the command line
// (--present_effect=fsr), in the environment (REX_PRESENT_EFFECT=fsr) or in gh2.toml next to the executable ("Save"
// writes that file). The look follows the original game's menus: black panels, red frames, orange/gold accents.

#include "controllers.h"
#include "fps_meter.h"
#include "problem_report.h"
#include "keyboard.h"

#include <SDL3/SDL_dialog.h>
#include <imgui.h>
#include <rex/cvar.h>
#include <rex/runtime.h>
#include <rex/string/utf8.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_device.h>
#include <rex/system/xam/content_manager.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/presenter.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include <atomic>
#include <cmath>
#include <chrono>
#include <SDL3/SDL_hints.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <set>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Integer cvars on purpose: the SDK parses double cvars with the locale-dependent strtod, so "0.5" reads as 0 on
// systems with a decimal comma (pt_BR).
REXCVAR_DEFINE_INT32(gfx_cas_sharpness, 0, "Graphics", "Extra sharpness of the CAS output filter, percent (0..100)")
    .range(0, 100);
REXCVAR_DEFINE_INT32(gfx_fsr_softness, 2, "Graphics",
                     "FSR output filter: sharpness reduction in tenths of a stop (0 = sharpest, 20 = softest)")
    .range(0, 20);
REXCVAR_DECLARE(bool, controller_guitars_first);
REXCVAR_DEFINE_STRING(install_content, "", "GH2",
                      "Install downloadable content before the game starts: path(s) of STFS packages (LIVE/CON/PIRS), "
                      "separated by ';'");

REXCVAR_DEFINE_STRING(audio_latency, "low", "GH2",
                      "Audio buffering: low (least delay, the default), balanced, or safe (for crackling on slow machines); "
                      "not in the menu, use --audio_latency=balanced on the command line")
    .allowed({"low", "balanced", "safe"});

REXCVAR_DEFINE_BOOL(show_fps, false, "GH2",
                    "Show the frame rate of the game itself (measured in the game, not by the screen) with a graph");

namespace {

using PaintConfig = rex::ui::Presenter::GuestOutputPaintConfig;

rex::ui::Presenter* g_presenter = nullptr;
rex::Runtime* g_runtime = nullptr;
rex::ui::Window* g_window = nullptr;
rex::ui::WindowedAppContext* g_context = nullptr;

// Help text in the dimmed colour that wraps at the window edge (ImGui::TextDisabled would be cut off).
void DimWrapped(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  ImGui::PushTextWrapPos(0.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextV(fmt, args);
  ImGui::PopStyleColor();
  ImGui::PopTextWrapPos();
  va_end(args);
}

// Where the game keeps the player's saves, installed DLC and achievements.
std::filesystem::path Gh2SaveDir() {
  if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) return std::filesystem::path(x) / "gh2";
  const char* h = std::getenv("HOME");
  return std::filesystem::path(h ? h : "") / ".local" / "share" / "gh2";
}

std::string ShortenHome(std::string path) {
  if (const char* h = std::getenv("HOME"); h && *h && path.rfind(h, 0) == 0) path.replace(0, std::strlen(h), "~");
  return path;
}

std::string Get(const char* name) { return rex::cvar::GetFlagByName(name); }
bool GetBool(const char* name) { return Get(name) == "true"; }
int GetInt(const char* name, int fallback) {
  const std::string v = Get(name);
  return v.empty() ? fallback : std::atoi(v.c_str());
}
void Set(const char* name, const std::string& value) { rex::cvar::SetFlagByName(name, value); }

bool g_applied_once = false;

bool ApplyPaintConfig() {
  if (!g_presenter) return false;
  PaintConfig config = g_presenter->GetGuestOutputPaintConfigFromUIThread();
  const std::string filter = Get("present_effect");
  config.SetEffect(filter == "fsr" ? PaintConfig::Effect::kFsr
                                   : filter == "cas" ? PaintConfig::Effect::kCas : PaintConfig::Effect::kBilinear);
  config.SetCasAdditionalSharpness(static_cast<float>(REXCVAR_GET(gfx_cas_sharpness)) / 100.0f);
  config.SetFsrSharpnessReduction(static_cast<float>(REXCVAR_GET(gfx_fsr_softness)) / 10.0f);
  config.SetDither(GetBool("present_dither"));
  g_presenter->SetGuestOutputPaintConfigFromUIThread(config);
  fprintf(stderr, "GFX output filter=%s cas=%d%% fsr_softness=%d/10 dither=%d\n", filter.c_str(),
          REXCVAR_GET(gfx_cas_sharpness), REXCVAR_GET(gfx_fsr_softness), GetBool("present_dither") ? 1 : 0);
  g_applied_once = true;
  return true;
}

// Writes only the settings this menu owns. rex::cvar::SaveConfig would also persist every flag that was forced on the
// command line (--vsync=false, --game_data_root, --gpu_plugin ...) and so freeze test options into later runs.
void SaveMenuSettings(const std::filesystem::path& path) {
  std::string out = "# GH2 graphics settings (written by the in-game menu, F2)\n";
  out += "present_effect = \"" + Get("present_effect") + "\"\n";
  out += "startup_mode = \"" + Get("startup_mode") + "\"\n";
  out += "swap_post_effect = \"" + Get("swap_post_effect") + "\"\n";
  out += "keyboard_player = \"" + Get("keyboard_player") + "\"\n";
  out += "keyboard_strum = \"" + Get("keyboard_strum") + "\"\n";
  for (int i = 0; i < kGh2KeyActionCount; ++i)
    out += std::string(kGh2KeyActions[i].cvar) + " = \"" + Get(kGh2KeyActions[i].cvar) + "\"\n";
  for (const char* name : {"present_dither", "fullscreen", "native_2x_msaa", "anisotropic_override", "resolution_scale",
                           "window_width", "window_height", "gfx_cas_sharpness", "gfx_fsr_softness", "controller_guitars_first", "show_fps"})
    out += std::string(name) + " = " + Get(name) + "\n";
  std::ofstream(path, std::ios::trunc) << out;
}

// ---- Downloadable content -----------------------------------------------------------------------------------------

struct DlcEntry {
  std::string name;     // display name from the package
  std::string file;     // folder / file name on disk
  bool loaded = false;  // opened by the game during this session
};

namespace xam = rex::system::xam;

}  // namespace
bool Gh2ContentOpened(const std::string& file_name);  // src/gh2_host.cpp
namespace {

std::vector<DlcEntry> ListDlc() {
  std::vector<DlcEntry> out;
  if (!g_runtime || !g_runtime->kernel_state()) return out;
  auto* cm = g_runtime->kernel_state()->content_manager();
  if (!cm) return out;
  for (const auto& d : cm->ListContent(static_cast<uint32_t>(xam::DummyDeviceId::HDD), 0,
                                       rex::system::XContentType::kMarketplaceContent)) {
    DlcEntry e;
    e.name = rex::string::to_utf8(d.display_name());
    e.file = d.file_name();
    e.loaded = Gh2ContentOpened(e.file) || cm->IsContentOpen(d);
    out.push_back(std::move(e));
  }
  return out;
}

std::mutex g_install_mutex;
std::string g_install_status;  // last install result, shown in the menu
std::atomic<bool> g_install_busy{false};
std::atomic<bool> g_dlc_dirty{true};

// Xbox 360 packages (STFS): "LIVE" / "PIRS" / "CON " magic; title id at 0x360 and content type at 0x344, big-endian.
constexpr uint32_t kGh2TitleId = 0x415607E7;
constexpr uint32_t kMarketplaceContentType = 0x00000002;

enum class PackageCheck { kOk, kNotAPackage, kOtherTitle, kNotDlc };

PackageCheck CheckPackage(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary);
  unsigned char h[0x380] = {};
  if (!f.read(reinterpret_cast<char*>(h), sizeof(h))) return PackageCheck::kNotAPackage;
  if (memcmp(h, "LIVE", 4) != 0 && memcmp(h, "PIRS", 4) != 0 && memcmp(h, "CON ", 4) != 0) return PackageCheck::kNotAPackage;
  const auto be32 = [&](size_t o) { return uint32_t(h[o]) << 24 | uint32_t(h[o + 1]) << 16 | uint32_t(h[o + 2]) << 8 | h[o + 3]; };
  if (be32(0x360) != kGh2TitleId) return PackageCheck::kOtherTitle;
  if (be32(0x344) != kMarketplaceContentType) return PackageCheck::kNotDlc;
  return PackageCheck::kOk;
}

// A path may be one package, or a folder holding any number of packages in any sub-folders (e.g. one folder per pack).
std::vector<std::filesystem::path> CollectFiles(const std::vector<std::string>& paths) {
  std::vector<std::filesystem::path> files;
  std::error_code ec;
  for (const auto& p : paths) {
    if (std::filesystem::is_directory(p, ec)) {
      for (std::filesystem::recursive_directory_iterator it(p, std::filesystem::directory_options::skip_permission_denied, ec), end;
           !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && it->file_size(ec) >= 0x1000) files.push_back(it->path());
    } else {
      files.push_back(p);
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

std::string InstallPackages(const std::vector<std::string>& paths) {
  if (!g_runtime || !g_runtime->kernel_state()) return "The game is not ready yet.";
  g_install_busy = true;
  int installed = 0, already = 0, other_title = 0, not_dlc = 0, failed = 0, not_pkg = 0;
  std::set<std::string> have;
  for (const auto& e : ListDlc()) have.insert(e.file);
  const auto files = CollectFiles(paths);
  const bool single = paths.size() == 1 && !std::filesystem::is_directory(paths[0]);
  for (const auto& file : files) {
    const std::string name = file.filename().string();
    switch (CheckPackage(file)) {
      case PackageCheck::kNotAPackage: ++not_pkg; continue;  // other files lying around in the folder
      case PackageCheck::kOtherTitle: ++other_title; fprintf(stderr, "DLC skip (another game): %s\n", file.c_str()); continue;
      case PackageCheck::kNotDlc: ++not_dlc; fprintf(stderr, "DLC skip (not downloadable content): %s\n", file.c_str()); continue;
      case PackageCheck::kOk: break;
    }
    if (have.count(name)) {
      ++already;
      continue;
    }
    const auto result = g_runtime->kernel_state()->content_manager()->InstallContent(file);
    fprintf(stderr, "DLC install %s: %s\n", file.c_str(), result == 0 ? "ok" : "FAILED");
    if (result == 0) {
      ++installed;
      have.insert(name);
    } else {
      ++failed;
    }
  }
  std::string msg;
  if (single && installed == 0 && already == 0 && failed == 0)
    msg = not_pkg ? "That file is not an Xbox 360 content package." : other_title ? "That package belongs to another game."
                                                                                  : "That package is not downloadable content.";
  else
    msg = "Installed " + std::to_string(installed) + ", already installed " + std::to_string(already) +
          (other_title + not_dlc ? ", skipped " + std::to_string(other_title + not_dlc) + " (not Guitar Hero II DLC)" : "") +
          (failed ? ", FAILED " + std::to_string(failed) : "") + ". Restart the game to load new packs.";
  {
    std::lock_guard<std::mutex> lock(g_install_mutex);
    g_install_status = msg;
  }
  fprintf(stderr, "DLC: %s\n", msg.c_str());
  g_dlc_dirty = true;
  g_install_busy = false;
  return msg;
}

void SDLCALL OnPathsPicked(void*, const char* const* files, int) {
  if (!files) return;
  std::vector<std::string> paths;
  for (; *files; ++files) paths.push_back(*files);
  if (!paths.empty()) std::thread([paths] { InstallPackages(paths); }).detach();
}

// ---- Look: the original game's menus ------------------------------------------------------------------------------

struct Gh2Theme {
  Gh2Theme() {
    ImGuiStyle& s = ImGui::GetStyle();
    saved_ = s;
    s.WindowRounding = 8.0f;
    s.FrameRounding = 5.0f;
    s.GrabRounding = 5.0f;
    s.TabRounding = 5.0f;
    s.WindowBorderSize = 2.0f;
    s.FrameBorderSize = 1.0f;
    s.WindowPadding = ImVec2(16, 14);
    s.ItemSpacing = ImVec2(10, 9);
    s.FramePadding = ImVec2(10, 6);
    ImVec4* c = s.Colors;
    const ImVec4 black(0.04f, 0.02f, 0.02f, 0.96f), red(0.80f, 0.11f, 0.10f, 1.0f),
        dark_red(0.40f, 0.05f, 0.05f, 1.0f), orange(0.98f, 0.50f, 0.10f, 1.0f), gold(1.0f, 0.83f, 0.25f, 1.0f),
        frame(0.17f, 0.07f, 0.07f, 1.0f);
    c[ImGuiCol_WindowBg] = black;
    c[ImGuiCol_Border] = red;
    c[ImGuiCol_Text] = ImVec4(0.97f, 0.95f, 0.92f, 1.0f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.62f, 0.52f, 0.50f, 1.0f);
    c[ImGuiCol_TitleBg] = dark_red;
    c[ImGuiCol_TitleBgActive] = red;
    c[ImGuiCol_TitleBgCollapsed] = dark_red;
    c[ImGuiCol_FrameBg] = frame;
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.32f, 0.12f, 0.08f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.45f, 0.16f, 0.08f, 1.0f);
    c[ImGuiCol_CheckMark] = gold;
    c[ImGuiCol_SliderGrab] = orange;
    c[ImGuiCol_SliderGrabActive] = gold;
    c[ImGuiCol_Button] = dark_red;
    c[ImGuiCol_ButtonHovered] = orange;
    c[ImGuiCol_ButtonActive] = gold;
    c[ImGuiCol_Header] = dark_red;
    c[ImGuiCol_HeaderHovered] = ImVec4(0.65f, 0.25f, 0.08f, 1.0f);
    c[ImGuiCol_HeaderActive] = orange;
    c[ImGuiCol_Separator] = ImVec4(0.55f, 0.10f, 0.08f, 1.0f);
    c[ImGuiCol_Tab] = dark_red;
    c[ImGuiCol_TabHovered] = orange;
    c[ImGuiCol_TabActive] = red;
    c[ImGuiCol_TabUnfocused] = dark_red;
    c[ImGuiCol_TabUnfocusedActive] = red;
    c[ImGuiCol_PopupBg] = black;
    c[ImGuiCol_TableHeaderBg] = dark_red;
    c[ImGuiCol_TableBorderLight] = ImVec4(0.30f, 0.08f, 0.07f, 1.0f);
    c[ImGuiCol_TableBorderStrong] = ImVec4(0.55f, 0.10f, 0.08f, 1.0f);
  }
  ~Gh2Theme() { ImGui::GetStyle() = saved_; }

 private:
  ImGuiStyle saved_;
};

const ImVec4 kGold(1.0f, 0.83f, 0.25f, 1.0f);
const ImVec4 kGreen(0.40f, 0.95f, 0.35f, 1.0f);
const ImVec4 kOrange(1.0f, 0.62f, 0.15f, 1.0f);

void Heading(const char* text) {
  ImGui::Spacing();
  ImGui::TextColored(kGold, "%s", text);
  ImGui::Separator();
}

class GraphicsDialog;
GraphicsDialog* g_dialog = nullptr;  // self-owned, see GraphicsDialog::RequestClose

class GraphicsDialog : public rex::ui::ImGuiDialog {
 public:
  GraphicsDialog(rex::ui::ImGuiDrawer* drawer, std::filesystem::path config_path)
      : ImGuiDialog(drawer), config_path_(std::move(config_path)) {}

  // Dialogs own themselves: Close() marks them and ImGuiDialog::Draw deletes them once OnDraw has returned. Never
  // destroy one from inside its own OnDraw (the Close button) or from another thread while it is being drawn.
  void RequestClose() { Close(); }

 protected:
  void OnClose() override {
    Gh2KeyboardCaptureEnd();
    g_dialog = nullptr;
  }

  void OnDraw(ImGuiIO& io) override {
    Gh2Theme theme;
    // Movable, not resizable: fixed width, height fits the content (scrolls if the screen is too small). Kept centred
    // until the user drags it: the display size reported on the first frames is not final, so a one-time placement
    // ends up off-centre.
    constexpr float kMenuWidth = 720.0f;
    ImGui::SetNextWindowSizeConstraints(ImVec2(kMenuWidth, 0.0f), ImVec2(kMenuWidth, io.DisplaySize.y * 0.94f));
    ImGui::SetNextWindowSize(ImVec2(kMenuWidth, 0.0f), ImGuiCond_Always);  // height auto-fits the current tab
    const ImVec2 center(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
    if (!user_moved_) ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ++frames_;
    if (std::getenv("GH2_MENU_DEBUG") && frames_ % 60 == 1)
      fprintf(stderr, "MENU display=%.0fx%.0f scale=%.2f fontsize=%.1f\n", io.DisplaySize.x, io.DisplaySize.y, text_scale_, ImGui::GetFontSize());
    ImGui::Begin("GUITAR HERO II  -  OPTIONS  (F2 closes, drag the title bar to move)", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize);
    if (!user_moved_ && frames_ > 3 && ImGui::IsMouseDown(0)) {
      const ImVec2 p = ImGui::GetWindowPos(), z = ImGui::GetWindowSize();
      if (std::abs(p.x + z.x * 0.5f - center.x) > 4.0f || std::abs(p.y + z.y * 0.5f - center.y) > 4.0f ||
          ImGui::IsMouseDragging(0, 2.0f))
        user_moved_ = ImGui::IsWindowFocused();
    }
    ImGui::SetWindowFontScale(text_scale_);
    if (std::getenv("GH2_MENU_DEBUG") && frames_ % 60 == 1)
      fprintf(stderr, "MENU after: fontsize=%.1f globalscale=%.2f winsize=%.0fx%.0f\n", ImGui::GetFontSize(), io.FontGlobalScale, ImGui::GetWindowSize().x, ImGui::GetWindowSize().y);
    if (ImGui::BeginTabBar("##tabs")) {
      if (ImGui::BeginTabItem("GRAPHICS")) {
        DrawGraphics();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("GAME", nullptr, std::getenv("GH2_TEST_TAB_GAME") ? ImGuiTabItemFlags_SetSelected : 0)) {  // env: testing aid
        DrawGame();
        ImGui::EndTabItem();
      }
      static bool first_c = true;  // testing aid: GH2_OPEN_TAB=controllers opens the controller tab
      const char* want_c = std::getenv("GH2_OPEN_TAB");
      const bool select_c = first_c && want_c && std::string(want_c) == "controllers";
      first_c = false;
      if (ImGui::BeginTabItem("CONTROLLERS", nullptr, select_c ? ImGuiTabItemFlags_SetSelected : 0)) {
        DrawControllers();
        ImGui::EndTabItem();
      }
      static bool first = true;  // testing aid: GH2_OPEN_TAB=content opens the DLC tab
      const char* want = std::getenv("GH2_OPEN_TAB");
      const bool select = first && want && std::string(want) == "content";
      first = false;
      if (ImGui::BeginTabItem("DOWNLOADABLE CONTENT", nullptr, select ? ImGuiTabItemFlags_SetSelected : 0)) {
        DrawContent();
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
    ImGui::Separator();
    DrawFooter();
    ImGui::End();
  }

 private:
  void DrawGraphics() {
    ImGui::TextWrapped(
        "The defaults reproduce the original console image (720p). Everything here is optional and costs GPU "
        "performance.");
    ImGui::PushItemWidth(300.0f);

    Heading("OUTPUT FILTER  (applies immediately)");
    static const char* kFilters[] = {"Original (bilinear)", "CAS - sharpening", "FSR 1 - upscaling with sharpening"};
    static const char* kFilterValues[] = {"bilinear", "cas", "fsr"};
    int filter = 0;
    for (int i = 0; i < 3; ++i) filter = Get("present_effect") == kFilterValues[i] ? i : filter;
    if (ImGui::Combo("Filter", &filter, kFilters, 3)) Set("present_effect", kFilterValues[filter]);
    if (filter == 1) {
      int v = REXCVAR_GET(gfx_cas_sharpness);
      if (ImGui::SliderInt("Extra CAS sharpness (%)", &v, 0, 100)) REXCVAR_SET(gfx_cas_sharpness, v), ApplyPaintConfig();
    } else if (filter == 2) {
      int v = REXCVAR_GET(gfx_fsr_softness);
      if (ImGui::SliderInt("FSR softness (0 = sharpest)", &v, 0, 20)) REXCVAR_SET(gfx_fsr_softness, v), ApplyPaintConfig();
      DimWrapped("FSR already includes a sharpening pass, so it replaces CAS.");
    }
    bool dither = GetBool("present_dither");
    if (ImGui::Checkbox("Dithering (reduces colour banding)", &dither)) Set("present_dither", dither ? "true" : "false");
    static const char* kModes[] = {"Fullscreen - borderless (recommended)", "Windowed"};
    int display_mode = GetBool("fullscreen") ? 0 : 1;
    if (ImGui::Combo("Display mode", &display_mode, kModes, 2)) {
      Set("fullscreen_exclusive", "false");  // never switch the monitor's video mode
      Set("fullscreen", display_mode == 0 ? "true" : "false");
    }
    DimWrapped("Borderless fullscreen fills the screen at the desktop resolution (no mode switch, quick alt-tab). "
                        "Windowed uses the size chosen below.");

    Heading("TEXTURE FILTERING");
    static const char* kAf[] = {"Original (as the game sets it)", "2x", "4x", "8x", "16x"};
    static const int kAfValues[] = {-1, 2, 3, 4, 5};
    int af = 0;
    for (int i = 0; i < 5; ++i) af = GetInt("anisotropic_override", -1) == kAfValues[i] ? i : af;
    if (ImGui::Combo("Anisotropic filtering", &af, kAf, 5)) Set("anisotropic_override", std::to_string(kAfValues[af]));
    DimWrapped("Sharper textures at steep angles (up to 16x). This is not anti-aliasing.");

    Heading("OUTPUT RESOLUTION");
    // Size of the game window (windowed mode). In fullscreen the picture always fills the monitor. 0x0 = the default.
    struct Size { const char* label; int w, h; };
    static const Size kSizes[] = {{"Default (1280x720)", 0, 0}, {"900p (1600x900)", 1600, 900}, {"1080p (1920x1080)", 1920, 1080},
                                  {"1440p (2560x1440)", 2560, 1440}, {"4K (3840x2160)", 3840, 2160}, {"Custom...", -1, -1}};
    const int cw = GetInt("window_width", 0), ch = GetInt("window_height", 0);
    int size = 5;
    for (int i = 0; i < 5; ++i)
      if (kSizes[i].w == cw && kSizes[i].h == ch) size = i;
    if (custom_size_) size = 5;
    if (ImGui::Combo("Window size (windowed mode)", &size, [](void*, int i) { return kSizes[i].label; }, nullptr, 6)) {
      custom_size_ = size == 5;
      if (size < 5) {
        Set("window_width", std::to_string(kSizes[size].w));
        Set("window_height", std::to_string(kSizes[size].h));
      } else if (cw == 0) {
        Set("window_width", "1280");
        Set("window_height", "720");
      }
    }
    if (size == 5) {
      int wh[2] = {cw, ch};
      ImGui::SetNextItemWidth(220.0f);
      if (ImGui::InputInt2("Width x height", wh, ImGuiInputTextFlags_EnterReturnsTrue)) {
        Set("window_width", std::to_string(std::clamp(wh[0], 640, 7680)));
        Set("window_height", std::to_string(std::clamp(wh[1], 360, 4320)));
      }
      DimWrapped("Press Enter to apply.");
    }
    DimWrapped("Applies immediately in windowed mode. The game still renders at the internal resolution below and is scaled to fit.");

    Heading("INTERNAL RESOLUTION");
    static const char* kScale[] = {"1x - 720p (original)", "2x - 1440p (heavy)"};
    int scale = GetInt("resolution_scale", 1) >= 2 ? 1 : 0;
    if (ImGui::Combo("Internal resolution (restart required)", &scale, kScale, 2))
      Set("resolution_scale", std::to_string(scale + 1));
    if (scale > 0)
      ImGui::TextColored(kOrange, "Only recommended for high-end GPUs: mid-range cards drop below 60 fps.");

    Heading("ANTI-ALIASING");
    ImGui::TextWrapped(
        "Method: MSAA (multisampling, requested by the game itself, as on the Xbox 360). FXAA is not used.");
    bool native_msaa = GetBool("native_2x_msaa");
    if (ImGui::Checkbox("Native 2x MSAA on the GPU (restart required)", &native_msaa))
      Set("native_2x_msaa", native_msaa ? "true" : "false");
    DimWrapped("Off = 2x MSAA is emulated with 4 samples (slower).");
    static const char* kFxaa[] = {"Off (original)", "FXAA", "FXAA - extreme quality"};
    static const char* kFxaaValues[] = {"none", "fxaa", "fxaa_extreme"};
    int fxaa = 0;
    for (int i = 0; i < 3; ++i) fxaa = Get("swap_post_effect") == kFxaaValues[i] ? i : fxaa;
    if (ImGui::Combo("Extra FXAA smoothing (restart required)", &fxaa, kFxaa, 3)) Set("swap_post_effect", kFxaaValues[fxaa]);
    DimWrapped("FXAA is a post-process filter on top of MSAA; it also softens the picture slightly.");
    ImGui::PopItemWidth();
  }

  void DrawGame() {
    ImGui::TextWrapped("Start-up behaviour. Takes effect the next time the game starts.");
    Heading("START-UP");
    static const char* kModes[] = {"Normal - logos and intro video (any button skips the logos)",
                                   "Skip straight to the main menu"};
    static const char* kModeValues[] = {"normal", "menu"};
    int mode = 0;
    for (int i = 0; i < 2; ++i) mode = Get("startup_mode") == kModeValues[i] ? i : mode;
    ImGui::PushItemWidth(520.0f);
    if (ImGui::Combo("When the game starts", &mode, kModes, 2)) Set("startup_mode", kModeValues[mode]);
    ImGui::PopItemWidth();
    DimWrapped("In every mode the intro video still skips with a button once it plays, as in the original.");

    Heading("PERFORMANCE");
    bool fps = GetBool("show_fps");
    if (ImGui::Checkbox("Show FPS counter (with graph)", &fps)) Set("show_fps", fps ? "true" : "false");

    Heading("SAVE GAME");
    const std::filesystem::path save_dir = Gh2SaveDir();
    if (ImGui::Button("Open saves folder")) {
      const std::string dir = save_dir.string();
      std::thread([dir] { std::system(("xdg-open '" + dir + "' >/dev/null 2>&1").c_str()); }).detach();
    }
    ImGui::TextDisabled("%s", ShortenHome(save_dir.string()).c_str());

    Heading("SUPPORT");
    if (ImGui::Button("Save problem report")) {
      report_error_.clear();
      report_path_ = Gh2WriteProblemReport(report_error_);
    }
    DimWrapped("If something goes wrong: make it happen again, then press this right away and attach the file to a GitHub issue, "
               "saying at what moment of the game it happens.");
    if (!report_path_.empty()) {
      ImGui::TextWrapped("Saved: %s", report_path_.c_str());
      if (ImGui::Button("Open folder")) {
        const std::string dir = std::filesystem::path(report_path_).parent_path().string();
        std::thread([dir] { std::system(("xdg-open '" + dir + "' >/dev/null 2>&1").c_str()); }).detach();
      }
    }
    if (!report_error_.empty()) ImGui::TextColored(kOrange, "%s", report_error_.c_str());

  }

  void DrawControllers() {
    Gh2Assignment* assign = Gh2GetAssignment();
    ImGui::TextWrapped(
        "Controllers the game sees right now. A guitar is presented to the game as a guitar; a gamepad or joystick as a "
        "standard controller. Plug or unplug devices at any time.");
    Heading("PLAYERS");
    if (!assign) {
      DimWrapped("Input system not ready yet.");
      return;
    }
    const auto rows = assign->Snapshot();
    if (ImGui::BeginTable("##players", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
      ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthFixed, 90.0f);
      ImGui::TableSetupColumn("Device", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 170.0f);
      ImGui::TableSetupColumn("Game sees it as", ImGuiTableColumnFlags_WidthFixed, 150.0f);
      ImGui::TableHeadersRow();
      for (uint32_t p = 1; p <= 4; ++p) {
        const Gh2ControllerRow* row = nullptr;
        for (const auto& r : rows)
          if (r.player == p) row = &r;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("Player %u", p);
        ImGui::TableNextColumn();
        if (row) ImGui::TextUnformatted(row->name.c_str()); else DimWrapped("-");
        ImGui::TableNextColumn();
        if (row) ImGui::TextColored(row->guitar ? kGold : kGreen, "%s", row->kind.c_str()); else DimWrapped("-");
        ImGui::TableNextColumn();
        if (row) ImGui::TextUnformatted(row->guitar ? "Guitar" : "Controller"); else DimWrapped("-");
      }
      ImGui::EndTable();
    }
    Heading("ASSIGNMENT");
    bool guitars_first = REXCVAR_GET(controller_guitars_first);
    if (ImGui::Checkbox("Guitars take the first player slots", &guitars_first)) {
      REXCVAR_SET(controller_guitars_first, guitars_first);
      assign->Reassign();
    }
    DimWrapped("Off = players follow the order the devices were connected.");
    if (ImGui::Button("Swap players 1 and 2")) assign->SwapPlayers12();
    if (assign->players_swapped()) {
      ImGui::SameLine();
      ImGui::TextColored(kOrange, "players 1 and 2 are swapped");
    }
    DrawKeyboard();
  }

  void DrawKeyboard() {
    Heading("KEYBOARD");
    ImGui::TextWrapped(
        "The keyboard plays as a guitar and is a separate device: a real guitar or gamepad always keeps the first slot "
        "unless you put the keyboard there.");
    ImGui::PushItemWidth(380.0f);
    static const char* kWho[] = {"Automatic - player 1, only when no controller is connected", "Off", "Player 1",
                                 "Player 2", "Player 3", "Player 4"};
    static const char* kWhoValues[] = {"auto", "off", "1", "2", "3", "4"};
    int who = 0;
    for (int i = 0; i < 6; ++i) who = Get("keyboard_player") == kWhoValues[i] ? i : who;
    if (ImGui::Combo("Keyboard plays as", &who, kWho, 6)) Set("keyboard_player", kWhoValues[who]);
    static const char* kStyle[] = {"With strum keys (strum like a real guitar)", "Frets only (pressing a fret strums)"};
    static const char* kStyleValues[] = {"pick", "frets_only"};
    int style = Get("keyboard_strum") == "frets_only" ? 1 : 0;
    if (ImGui::Combo("Playing style", &style, kStyle, 2)) Set("keyboard_strum", kStyleValues[style]);
    ImGui::PopItemWidth();
    DimWrapped("Frets only strums by itself inside songs; the arrow keys still move through the menus.");
    if (ImGui::BeginTable("##keys", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
      ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 200.0f);
      ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 170.0f);
      ImGui::TableHeadersRow();
      for (int i = 0; i < kGh2KeyActionCount; ++i) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(kGh2KeyActions[i].label);
        ImGui::TableNextColumn();
        if (capture_ == i)
          ImGui::TextColored(kOrange, "press a key (Esc cancels)");
        else
          ImGui::TextColored(kGold, "%s", Get(kGh2KeyActions[i].cvar).c_str());
        ImGui::TableNextColumn();
        ImGui::PushID(i);
        if (ImGui::Button(capture_ == i ? "Cancel" : "Change key")) {
          if (capture_ == i) {
            capture_ = -1;
            Gh2KeyboardCaptureEnd();
          } else {
            capture_ = i;
            key_note_.clear();
            Gh2KeyboardCaptureBegin();
          }
        }
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
    if (capture_ >= 0) {
      const std::string key = Gh2KeyboardCapturePoll();
      if (!key.empty()) {
        if (key == "Escape") {
          capture_ = -1;
          Gh2KeyboardCaptureEnd();
        } else if (key == "F2" || key == "F3" || key == "F4" || key == "F7" || key == "`") {
          key_note_ = key + " is reserved for the game's own menus; pick another key.";
        } else {
          // One key can only do one thing: if another action already uses it, the two actions swap keys.
          const char* cvar = kGh2KeyActions[capture_].cvar;
          const std::string old_key = Get(cvar);
          key_note_.clear();
          for (int j = 0; j < kGh2KeyActionCount; ++j) {
            if (j == capture_ || Get(kGh2KeyActions[j].cvar) != key) continue;
            Set(kGh2KeyActions[j].cvar, old_key);
            key_note_ = std::string(key) + " was used for \"" + kGh2KeyActions[j].label + "\", which now uses " +
                        (old_key.empty() ? "no key" : old_key) + ".";
          }
          Set(cvar, key);
          capture_ = -1;
          Gh2KeyboardCaptureEnd();
        }
      }
    }
    if (!key_note_.empty()) ImGui::TextColored(kOrange, "%s", key_note_.c_str());
    if (ImGui::Button("Reset keys to defaults")) {
      for (int i = 0; i < kGh2KeyActionCount; ++i) rex::cvar::ResetToDefault(kGh2KeyActions[i].cvar);
      capture_ = -1;
      Gh2KeyboardCaptureEnd();
    }
  }

  void DrawContent() {
    const auto now = std::chrono::steady_clock::now();
    if (g_dlc_dirty.exchange(false) || now - last_scan_ > std::chrono::seconds(3)) {
      dlc_ = ListDlc();
      last_scan_ = now;
    }
    ImGui::TextWrapped(
        "Downloadable song packs, as sold on Xbox Live (STFS packages: LIVE / CON / PIRS). Installing extracts the "
        "pack into the game's content folder; the game reads its packs when it starts. Only Guitar Hero II DLC is accepted.");
    Heading("INSTALL");
    ImGui::BeginDisabled(g_install_busy.load());
    if (ImGui::Button("Install package(s)...")) {
      static const SDL_DialogFileFilter kFilters[] = {{"All files", "*"}};
      SDL_ShowOpenFileDialog(OnPathsPicked, nullptr, nullptr, kFilters, 1, nullptr, true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Install a whole folder...")) SDL_ShowOpenFolderDialog(OnPathsPicked, nullptr, nullptr, nullptr, false);
    ImGui::EndDisabled();
    ImGui::SameLine();
    DimWrapped("A folder is scanned including sub-folders; or start with --install_content=<file or folder>");
    {
      std::lock_guard<std::mutex> lock(g_install_mutex);
      if (!g_install_status.empty()) ImGui::TextColored(kOrange, "%s", g_install_status.c_str());
    }

    int loaded = 0;
    for (const auto& e : dlc_) loaded += e.loaded;
    char title[96];
    snprintf(title, sizeof(title), "INSTALLED PACKS  (%zu installed, %d loaded now)", dlc_.size(), loaded);
    Heading(title);
    if (dlc_.empty()) {
      DimWrapped("No downloadable content installed yet.");
    } else if (ImGui::BeginTable("##dlc", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
      ImGui::TableSetupColumn("Pack", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Folder", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 150.0f);
      ImGui::TableHeadersRow();
      for (const auto& e : dlc_) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(e.name.empty() ? "(unnamed)" : e.name.c_str());
        ImGui::TableNextColumn();
        DimWrapped("%s", e.file.c_str());
        ImGui::TableNextColumn();
        if (e.loaded)
          ImGui::TextColored(kGreen, "LOADED");
        else
          ImGui::TextColored(kOrange, "INSTALLED");
      }
      ImGui::EndTable();
    }
    DimWrapped("LOADED = opened by the game in this session.  INSTALLED = on disk, loads at the next start.");
  }

  // "Are you sure?" before closing the game: a small window of its own, in front of the menu. The default answer is No, so a
  // stray click does not lose a song in progress.
  void DrawQuitConfirmation(double test_yes_after_s) {
    if (!quit_confirm_) return;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowFocus();
    ImGui::Begin("Quit the game?", nullptr,
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::Text("Close Guitar Hero 2?");
    ImGui::Spacing();
    DimWrapped("Anything the game has not saved yet, such as a song in progress, will be lost.");
    ImGui::Spacing();
    if (ImGui::Button("No, keep playing", ImVec2(180, 0))) quit_confirm_ = false;
    ImGui::SameLine();
    bool yes = ImGui::Button("Yes, quit", ImVec2(180, 0));
    if (!yes && test_yes_after_s >= 0 &&
        std::chrono::duration<double>(std::chrono::steady_clock::now() - quit_opened_).count() > test_yes_after_s)
      yes = true;
    if (yes) {
      quit_confirm_ = false;
      if (g_window && g_context) g_context->CallInUIThread([] { g_window->RequestClose(); });  // after this frame is drawn
    }
    ImGui::End();
  }

  void DrawFooter() {
    if (ImGui::Button("Save")) {
      SaveMenuSettings(config_path_);
      saved_ = true;
    }
    ImGui::SameLine();
    std::vector<const char*> kManaged = {"present_effect", "present_dither", "anisotropic_override", "resolution_scale",
                                         "window_width", "window_height", "native_2x_msaa", "gfx_cas_sharpness", "gfx_fsr_softness",
                                         "controller_guitars_first", "show_fps", "startup_mode", "keyboard_player", "keyboard_strum",
                                         "swap_post_effect"};
    for (int i = 0; i < kGh2KeyActionCount; ++i) kManaged.push_back(kGh2KeyActions[i].cvar);
    if (ImGui::Button("Restore originals")) {
      undo_.clear();  // remember what was set, so the restore can be undone
      for (const char* n : kManaged) undo_.emplace_back(n, Get(n));
      for (const char* n : kManaged) rex::cvar::ResetToDefault(n);
      ApplyPaintConfig();
      saved_ = false;
    }
    if (!undo_.empty()) {
      ImGui::SameLine();
      if (ImGui::Button("Undo restore")) {
        for (const auto& [n, v] : undo_) Set(n.c_str(), v);
        undo_.clear();
        ApplyPaintConfig();
      }
    }
    ImGui::SameLine();
    static const bool auto_close = std::getenv("GH2_TEST_CLOSE") != nullptr;  // testing aid
    static const auto t_open = std::chrono::steady_clock::now();
    if (ImGui::Button("Close") || (auto_close && std::chrono::steady_clock::now() - t_open > std::chrono::seconds(6)))
      Close();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.1f, 0.1f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.15f, 0.15f, 1.0f));
    const bool quit_clicked = ImGui::Button("Quit game");
    ImGui::PopStyleColor(2);
    static const bool test_popup = std::getenv("GH2_TEST_QUIT_POPUP") != nullptr;  // testing aids: open the question on its own,
    static const char* test_yes = std::getenv("GH2_TEST_QUIT_YES");                 // and answer yes after N seconds
    static bool test_opened = false;
    static int test_frames = 0;
    if (quit_clicked || (test_popup && !test_opened && ++test_frames > 300)) {
      test_opened = true;
      quit_opened_ = std::chrono::steady_clock::now();
      quit_confirm_ = true;
    }
    DrawQuitConfirmation(test_yes ? std::atof(test_yes) : -1.0);
    if (saved_) {
      ImGui::SameLine();
      DimWrapped("saved to %s", config_path_.filename().string().c_str());
    }
  }

  std::string report_path_, report_error_;
  bool quit_confirm_ = false;
  std::chrono::steady_clock::time_point quit_opened_;
  std::filesystem::path config_path_;
  int capture_ = -1;  // key action being rebound, or -1
  std::string key_note_;
  int frames_ = 0;
  bool user_moved_ = false;
  const float text_scale_ = 1.0f;
  bool saved_ = false;
  bool custom_size_ = false;
  std::vector<std::pair<std::string, std::string>> undo_;  // settings before "Restore originals"
  std::vector<DlcEntry> dlc_;
  std::chrono::steady_clock::time_point last_scan_{};
};

// A short on-screen hint after start-up, so players find the menu.
// Small click-through panel: the game's frame rate right now, the 1% low, and a line graph of the last 40 seconds.
class FpsDialog;
FpsDialog* g_fps_dialog = nullptr;  // self-owned, like the other dialogs

class FpsDialog : public rex::ui::ImGuiDialog {
 public:
  explicit FpsDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) { g_fps_dialog = this; }
  void RequestClose() { Close(); }

 protected:
  void OnClose() override { g_fps_dialog = nullptr; }

  void OnDraw(ImGuiIO&) override {
    Gh2FpsView v;
    Gh2FpsRead(v);
    ImGuiStyle& st = ImGui::GetStyle();
    const ImGuiStyle saved = st;
    st.WindowRounding = 6.0f;
    st.WindowPadding = ImVec2(10, 8);
    st.WindowBorderSize = 1.0f;
    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.6f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin("##gh2fps", nullptr, kFlags)) {
      // Green at the game's own 60, orange when it is slipping, red when it is clearly slow.
      const ImVec4 colour = v.fps >= 57.0f ? ImVec4(0.35f, 0.9f, 0.35f, 1) : v.fps >= 45.0f ? ImVec4(1.0f, 0.65f, 0.15f, 1)
                                                                                           : ImVec4(1.0f, 0.3f, 0.3f, 1);
      ImGui::SetWindowFontScale(1.6f);
      ImGui::TextColored(colour, "%.0f FPS", v.fps);
      ImGui::SetWindowFontScale(1.0f);
      ImGui::TextDisabled("game, not screen");
      ImGui::Text("frame %.1f ms   1%% low %.0f   worst %.0f ms", v.frame_ms, v.low1_fps, v.worst_ms);
      if (v.history_n > 1) {
        ImGui::PushStyleColor(ImGuiCol_PlotLines, colour);
        ImGui::PlotLines("##fpsgraph", v.history, v.history_n, 0, nullptr, 0.0f, 75.0f, ImVec2(250, 48));
        ImGui::PopStyleColor();
      }
    }
    ImGui::End();
    st = saved;
  }
};

class HintDialog : public rex::ui::ImGuiDialog {
 public:
  explicit HintDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer), t0_(std::chrono::steady_clock::now()) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (!g_applied_once) ApplyPaintConfig();  // the presenter does not exist yet when the dialogs are created
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
    if (s > 8.0) {
      Close();
      return;
    }
    ImGui::GetForegroundDrawList()->AddText(ImVec2(12.0f, io.DisplaySize.y - 28.0f), IM_COL32(255, 210, 70, 220),
                                            "F2: options (graphics, DLC)");
  }

 private:
  std::chrono::steady_clock::time_point t0_;
};

}  // namespace

// Called before the guest starts (ReXApp::OnPreLaunchModule): installs packages named by --install_content.
void Gh2ApplyAudioLatency() {
  // Two buffers add up: the SDK's queue of 256-sample frames (5.3 ms each, minimum 4) and the output stream's buffer that
  // SDL asks the sound server for. The game's SDL default is 1024 samples (21 ms) and the SDK's queue is 8 frames (43 ms).
  const std::string mode = rex::cvar::GetFlagByName("audio_latency");
  const char* sdl_frames = mode == "safe" ? "1024" : mode == "balanced" ? "512" : "256";
  const char* queue = mode == "safe" ? "16" : mode == "balanced" ? "8" : "4";
  // SDL 3 keeps its own copy of the environment, so a later setenv() is not seen: use its hint call (an explicit
  // SDL_AUDIO_DEVICE_SAMPLE_FRAMES in the environment still wins over this default-priority hint).
  SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, sdl_frames);
  if (!rex::cvar::HasNonDefaultValue("audio_maxqframes")) rex::cvar::SetFlagByName("audio_maxqframes", queue);
}

void Gh2InstallRequestedContent(rex::Runtime* runtime) {
  g_runtime = runtime;
  const std::string list = REXCVAR_GET(install_content);
  std::vector<std::string> paths;
  size_t pos = 0;
  while (pos < list.size()) {
    size_t end = list.find(';', pos);
    if (end == std::string::npos) end = list.size();
    if (end > pos) paths.push_back(list.substr(pos, end - pos));
    pos = end + 1;
  }
  if (!paths.empty()) InstallPackages(paths);
}

void Gh2InstallGraphicsMenu(rex::Runtime* runtime, rex::ui::Presenter* presenter, rex::ui::Window* window,
                            rex::ui::WindowedAppContext& context, rex::ui::ImGuiDrawer* drawer,
                            const std::filesystem::path& config_path) {
  g_runtime = runtime;
  g_presenter = presenter;
  g_window = window;
  g_context = &context;
  ApplyPaintConfig();

  // Live changes (menu, F4 overlay, console) come from any thread: marshal onto the UI thread.
  auto on_change = [&context](std::string_view, std::string_view) { context.CallInUIThread([] { ApplyPaintConfig(); }); };
  for (const char* name : {"present_effect", "present_dither", "gfx_cas_sharpness", "gfx_fsr_softness"})
    rex::cvar::RegisterChangeCallback(name, on_change);

  // The FPS counter exists only while the option is on (a registered dialog makes the presenter repaint every refresh).
  auto sync_fps = [drawer] {
    const bool want = GetBool("show_fps");
    if (want && !g_fps_dialog) new FpsDialog(drawer);
    else if (!want && g_fps_dialog) g_fps_dialog->RequestClose();
  };
  rex::cvar::RegisterChangeCallback("show_fps", [&context, sync_fps](std::string_view, std::string_view) {
    context.CallInUIThread(sync_fps);
  });
  sync_fps();

  rex::ui::RegisterBind("bind_gh2_graphics", "F2", "Toggle options menu", [drawer, config_path] {
    if (g_dialog) {
      g_dialog->RequestClose();
    } else {
      g_dialog = new GraphicsDialog(drawer, config_path);  // owns itself until closed
    }
  });

  if (std::getenv("GH2_OPEN_GFX_MENU")) g_dialog = new GraphicsDialog(drawer, config_path);  // testing aid
  if (std::getenv("GH2_GFX_TEST_SWITCH")) {  // testing aid: change the output filter from another thread
    std::thread([] {
      for (const char* v : {"fsr", "cas", "bilinear"}) {
        std::this_thread::sleep_for(std::chrono::seconds(4));
        Set("present_effect", v);
      }
    }).detach();
  }
  if (const char* spec = std::getenv("GH2_TEST_SET")) {  // testing aid: "name=value@seconds;..." applied from a thread
    std::string list = spec;
    std::thread([list] {
      size_t pos = 0;
      double done = 0;
      while (pos < list.size()) {
        size_t end = list.find(';', pos);
        if (end == std::string::npos) end = list.size();
        const std::string item = list.substr(pos, end - pos);
        pos = end + 1;
        const size_t eq = item.find('='), at = item.find('@');
        if (eq == std::string::npos || at == std::string::npos) continue;
        const double when = std::atof(item.c_str() + at + 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(std::max(0.0, when - done) * 1000)));
        done = when;
        Set(item.substr(0, eq).c_str(), item.substr(eq + 1, at - eq - 1));
        fprintf(stderr, "TESTSET %s\n", item.c_str());
      }
    }).detach();
  }
  if (const char* t = std::getenv("GH2_TEST_REPORT")) {  // testing aid: write a problem report after N seconds
    const int delay = std::max(1, std::atoi(t));
    std::thread([delay] {
      std::this_thread::sleep_for(std::chrono::seconds(delay));
      std::string error;
      const std::string path = Gh2WriteProblemReport(error);
      fprintf(stderr, "TEST_REPORT %s %s\n", path.c_str(), error.c_str());
    }).detach();
  }
  new HintDialog(drawer);  // owns itself, closes after a few seconds
}
