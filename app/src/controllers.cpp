#include "controllers.h"

#include "keyboard.h"

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/runtime.h>
#include <rex/ui/imgui_dialog.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>

REXCVAR_DEFINE_BOOL(controller_guitars_first, true, "GH2",
                    "Give guitars the first player slots, even when a gamepad was connected first");

namespace {

Gh2Assignment* g_assignment = nullptr;
rex::ui::ImGuiDrawer* g_drawer = nullptr;
rex::ui::WindowedAppContext* g_context = nullptr;

std::atomic<bool> g_user_guitar[4];
std::atomic<bool> g_user_real[4];

bool IsGuitarSubtype(uint8_t s) { return s == 0x06 || s == 0x07 || s == 0x0B; }  // guitar, alternate, bass

std::string KindName(const rex::input::DeviceInfo& d) {
  if (d.synthetic) return "Keyboard / mouse";
  switch (d.subtype) {
    case 0x01: return "Gamepad";
    case 0x02: return "Wheel";
    case 0x03: return "Arcade stick";
    case 0x04: return "Flight stick";
    case 0x05: return "Dance pad";
    case 0x06: return "Guitar";
    case 0x07: return "Guitar (alternate)";
    case 0x08: return "Drum kit";
    case 0x0B: return "Bass guitar";
    case 0x13: return "Arcade pad";
    default: return "Controller";
  }
}

// Short on-screen message after a controller is connected or removed.
class NoticeDialog : public rex::ui::ImGuiDialog {
 public:
  NoticeDialog(rex::ui::ImGuiDrawer* drawer, std::vector<std::string> lines)
      : ImGuiDialog(drawer), lines_(std::move(lines)), t0_(std::chrono::steady_clock::now()) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
    if (s > 7.0) {
      Close();
      return;
    }
    const float alpha = s > 6.0 ? static_cast<float>(7.0 - s) : 1.0f;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float fs = ImGui::GetFontSize() * 1.25f, pad = 10.0f;
    float w = 0;
    for (const auto& l : lines_) w = std::max(w, ImGui::GetFont()->CalcTextSizeA(fs, 1e9f, 0, l.c_str()).x);
    const float h = lines_.size() * (fs + 4.0f) + pad * 2;
    const ImVec2 p0(16.0f, io.DisplaySize.y * 0.12f), p1(p0.x + w + pad * 2, p0.y + h);
    dl->AddRectFilled(p0, p1, IM_COL32(10, 5, 5, static_cast<int>(235 * alpha)), 6.0f);
    dl->AddRect(p0, p1, IM_COL32(205, 28, 25, static_cast<int>(255 * alpha)), 6.0f, 0, 2.0f);
    float y = p0.y + pad;
    for (const auto& l : lines_) {
      dl->AddText(ImGui::GetFont(), fs, ImVec2(p0.x + pad, y), IM_COL32(255, 212, 64, static_cast<int>(255 * alpha)), l.c_str());
      y += fs + 4.0f;
    }
  }

 private:
  std::vector<std::string> lines_;
  std::chrono::steady_clock::time_point t0_;
};

}  // namespace

void Gh2Assignment::AssignLocked() {
  for (auto& u : users_) u.clear();
  std::vector<const rex::input::DeviceInfo*> real;
  for (const auto& d : devices_)
    if (!d.synthetic) real.push_back(&d);
  const bool guitars_first = REXCVAR_GET(controller_guitars_first);
  std::stable_sort(real.begin(), real.end(), [&](const auto* a, const auto* b) {
    if (guitars_first && IsGuitarSubtype(a->subtype) != IsGuitarSubtype(b->subtype)) return IsGuitarSubtype(a->subtype);
    return a->ordinal < b->ordinal;
  });
  // The keyboard plays as one player (auto: player 1 only when no controller is connected); real devices skip its slot.
  const std::string kb_mode = rex::cvar::GetFlagByName("keyboard_player");
  int kb_slot = -1;
  if (kb_mode == "auto") kb_slot = real.empty() ? 0 : -1;
  else if (kb_mode != "off" && !kb_mode.empty()) kb_slot = std::clamp(atoi(kb_mode.c_str()) - 1, 0, 3);
  Gh2SetKeyboardSlot(kb_slot);
  for (size_t i = 0, slot = 0; i < real.size(); ++i, ++slot) {
    if (static_cast<int>(slot) == kb_slot) ++slot;
    if (slot >= users_.size()) break;
    users_[slot].push_back(real[i]->id);
  }
  if (swapped_.load()) std::swap(users_[0], users_[1]);
  // Synthetic devices (keyboard/mouse emulation, the "no controller" stand-in) feed player 1 (after the swap, so a swap never moves them), after the real ones, as in
  // the SDK's own SlotAssignment.
  for (const auto& d : devices_)
    if (d.synthetic) users_[0].push_back(d.id);
  for (size_t u = 0; u < 4; ++u) {
    bool guitar = false;
    for (const auto& id : users_[u])
      for (const auto& d : devices_)
        if (d.id == id && !d.synthetic && IsGuitarSubtype(d.subtype)) guitar = true;
    g_user_guitar[u].store(guitar, std::memory_order_relaxed);
    bool has_real = false;
    for (const auto& id : users_[u])
      for (const auto& d : devices_)
        if (d.id == id && !d.synthetic) has_real = true;
    g_user_real[u].store(has_real, std::memory_order_relaxed);
  }
}

void Gh2Assignment::OnDevicesChanged(const std::vector<rex::input::DeviceInfo>& devices) {
  std::vector<std::string> fresh;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto known = [](const std::vector<rex::input::DeviceInfo>& v, rex::input::DeviceId id) {
      return std::any_of(v.begin(), v.end(), [&](const auto& d) { return d.id == id; });
    };
    for (const auto& d : devices)
      if (!d.synthetic && !known(devices_, d.id)) fresh.push_back("CONNECTED: " + KindName(d) + " - " + d.name);
    for (const auto& d : devices_)
      if (!d.synthetic && !known(devices, d.id)) fresh.push_back("REMOVED: " + KindName(d) + " - " + d.name);
    devices_ = devices;
    AssignLocked();
    if (getenv("GH2_CTRL_DEBUG")) {
      for (const auto& d : devices_) fprintf(stderr, "CTRLDBG dev id=%llu ord=%u synth=%d sub=%u name='%s'\n", (unsigned long long)d.id, d.ordinal, d.synthetic, d.subtype, d.name.c_str());
      for (size_t u = 0; u < users_.size(); ++u) fprintf(stderr, "CTRLDBG user%zu n=%zu first=%llu\n", u, users_[u].size(), users_[u].empty() ? 0ull : (unsigned long long)users_[u][0]);
    }
    // Tell the player which slot each new device got.
    for (auto& line : fresh) {
      if (line.rfind("CONNECTED", 0) != 0) continue;
      for (size_t u = 0; u < users_.size(); ++u)
        for (const auto& d : devices_)
          if (!users_[u].empty() && users_[u][0] == d.id && line.find(d.name) != std::string::npos)
            line += "  [player " + std::to_string(u + 1) + "]";
    }
    notices_.insert(notices_.end(), fresh.begin(), fresh.end());
  }
  for (const auto& l : fresh) fprintf(stderr, "CONTROLLER %s\n", l.c_str());
  if (!fresh.empty() && g_context && g_drawer) {
    g_context->CallInUIThread([] {
      if (!g_assignment || !g_drawer) return;
      auto lines = g_assignment->TakeNotices();
      if (!lines.empty()) new NoticeDialog(g_drawer, std::move(lines));  // owns itself, closes after a few seconds
    });
  }
}

void Gh2Assignment::DevicesForUser(uint32_t user_index, std::vector<rex::input::DeviceId>& out) const {
  out.clear();
  if (user_index >= users_.size()) return;
  std::lock_guard<std::mutex> lock(mutex_);
  out = users_[user_index];
}

std::vector<Gh2ControllerRow> Gh2Assignment::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Gh2ControllerRow> rows;
  for (size_t u = 0; u < users_.size(); ++u) {
    for (const auto& id : users_[u]) {
      for (const auto& d : devices_) {
        if (d.id != id || (d.synthetic && d.name == "None")) continue;  // hide the "no controller" stand-in
        Gh2ControllerRow r;
        r.player = static_cast<uint32_t>(u + 1);
        r.name = d.name;
        r.kind = KindName(d);
        r.subtype = d.subtype;
        r.guitar = IsGuitarSubtype(d.subtype);
        rows.push_back(std::move(r));
      }
    }
  }
  const int kb = Gh2KeyboardSlot();
  if (kb >= 0) {
    Gh2ControllerRow r;
    r.player = static_cast<uint32_t>(kb + 1);
    r.name = "Keyboard";
    r.kind = "Keyboard";
    r.subtype = 0x07;
    r.guitar = true;
    rows.push_back(std::move(r));
  }
  return rows;
}

void Gh2Assignment::Reassign() {
  std::lock_guard<std::mutex> lock(mutex_);
  AssignLocked();
}

void Gh2Assignment::SwapPlayers12() {
  swapped_ = !swapped_.load();
  Reassign();
}

std::vector<std::string> Gh2Assignment::TakeNotices() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> out;
  out.swap(notices_);
  return out;
}

Gh2Assignment* Gh2GetAssignment() { return g_assignment; }

bool Gh2UserHasRealDevice(uint32_t user) { return user < 4 && g_user_real[user].load(std::memory_order_relaxed); }
bool Gh2UserIsGuitar(uint32_t user) { return user < 4 && g_user_guitar[user].load(std::memory_order_relaxed); }

void Gh2SetControllerNoticeTarget(rex::ui::ImGuiDrawer* drawer, rex::ui::WindowedAppContext* context) {
  g_drawer = drawer;
  g_context = context;
}

void Gh2InstallControllerAssignment(rex::Runtime* runtime) {
  auto* input = runtime ? static_cast<rex::input::InputSystem*>(runtime->input_system()) : nullptr;
  if (!input) return;
  auto assignment = std::make_unique<Gh2Assignment>();
  g_assignment = assignment.get();
  input->SetDeviceAssignment(std::move(assignment));
  Gh2KeyboardRefreshBindings();
  auto on_kb_change = [](std::string_view, std::string_view) {
    Gh2KeyboardRefreshBindings();
    if (g_assignment) g_assignment->Reassign();
  };
  rex::cvar::RegisterChangeCallback("keyboard_player", on_kb_change);
  rex::cvar::RegisterChangeCallback("keyboard_strum", on_kb_change);
  for (int i = 0; i < kGh2KeyActionCount; ++i) rex::cvar::RegisterChangeCallback(kGh2KeyActions[i].cvar, on_kb_change);
}
