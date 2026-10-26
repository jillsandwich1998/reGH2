#include "menu_pad.h"

#include <imgui.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>

namespace {

constexpr uint16_t kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008, kStart = 0x0010, kBack = 0x0020, kLb = 0x0100, kRb = 0x0200, kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000;
constexpr double kChordSeconds = 0.7;

struct User {
  uint16_t buttons = 0;
  int16_t lx = 0, ly = 0;
  bool guitar = false;
  std::chrono::steady_clock::time_point chord_since{};
  bool chord_down = false, chord_fired = false;
};

std::atomic<bool> g_open{false};
std::atomic<uint32_t> g_nav_buttons{0};            // merged over the users, in ImGui terms (see Apply)
std::atomic<int> g_nav_x{0}, g_nav_y{0};           // left stick: -1, 0, 1
std::atomic<int> g_tab_delta{0};
std::atomic<bool> g_opened_by_pad{false};
uint32_t g_prev_merged = 0;
std::mutex g_mutex;
User g_users[4];
std::function<void()> g_toggle;

}  // namespace

void Gh2MenuPadSetToggle(std::function<void()> toggle) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_toggle = std::move(toggle);
}

void Gh2MenuPadSetOpen(bool open) {
  g_open = open;
  if (!open) {
    g_nav_buttons = 0;
    g_nav_x = g_nav_y = 0;
  }
}

bool Gh2MenuPadCapturing() { return g_open.load(std::memory_order_relaxed); }

void Gh2MenuPadFeed(uint32_t user, const gh2rt::PadState& raw, bool guitar) {
  if (user >= 4) return;
  std::function<void()> fire;
  uint32_t merged = 0;
  int nx = 0, ny = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    User& u = g_users[user];
    u.buttons = raw.buttons;
    u.lx = raw.lx;
    u.ly = raw.ly;
    u.guitar = guitar;
    const bool chord = (raw.buttons & (kStart | kBack)) == (kStart | kBack);
    const auto now = std::chrono::steady_clock::now();
    if (chord && !u.chord_down) {
      u.chord_down = true;
      u.chord_fired = false;
      u.chord_since = now;
    } else if (!chord) {
      u.chord_down = false;
    }
    if (u.chord_down && !u.chord_fired && std::chrono::duration<double>(now - u.chord_since).count() >= kChordSeconds) {
      u.chord_fired = true;   // once per hold
      fire = g_toggle;
      if (!g_open.load()) g_opened_by_pad = true;
    }
    if (g_open.load()) {
      for (const User& v : g_users) {
        // bit i of `merged` = ImGuiKey_Gamepad* in the order of the table in Apply()
        auto has = [&](uint16_t b) { return (v.buttons & b) != 0; };
        merged |= (has(kUp) ? 1u << 0 : 0) | (has(kDown) ? 1u << 1 : 0) | (has(kLeft) ? 1u << 2 : 0) | (has(kRight) ? 1u << 3 : 0) | (has(kA) ? 1u << 4 : 0) | (has(kB) ? 1u << 5 : 0);
        if (v.guitar) {   // a guitar has no bumpers and no D-pad sides: yellow / orange go to the previous / next tab, blue = left, green stays "select"
          merged |= (has(kY) ? 1u << 6 : 0) | (has(kLb) ? 1u << 7 : 0) | (has(kX) ? 1u << 2 : 0);
        } else {
          merged |= (has(kLb) ? 1u << 6 : 0) | (has(kRb) ? 1u << 7 : 0) | (has(kX) ? 1u << 8 : 0) | (has(kY) ? 1u << 9 : 0);
          if (v.ly > 16000) ny = -1;
          else if (v.ly < -16000) ny = 1;
          if (v.lx > 16000) nx = 1;
          else if (v.lx < -16000) nx = -1;
        }
      }
      const uint32_t rising = merged & ~g_prev_merged;
      if (rising & (1u << 6)) g_tab_delta -= 1;
      if (rising & (1u << 7)) g_tab_delta += 1;
      g_prev_merged = merged;
      g_nav_buttons = merged & ~((1u << 6) | (1u << 7));   // the tab buttons are the menu's own business
      g_nav_x = nx;
      g_nav_y = ny;
    }
  }
  if (fire) fire();   // outside the lock: it posts to the UI thread
}

int Gh2MenuPadTabDelta() { return g_tab_delta.exchange(0); }
bool Gh2MenuPadTakeOpenedByPad() { return g_opened_by_pad.exchange(false); }

void Gh2MenuPadApply(ImGuiIO& io) {
  static bool was_active = false;
  const bool open = g_open.load();
  if (open) {
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  }
  static const ImGuiKey kKeys[10] = {ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadFaceDown, ImGuiKey_GamepadFaceRight,
                                     ImGuiKey_GamepadL1,     ImGuiKey_GamepadR1,       ImGuiKey_GamepadFaceLeft,  ImGuiKey_GamepadFaceUp};
  const uint32_t bits = open ? g_nav_buttons.load() : 0;
  const int nx = open ? g_nav_x.load() : 0, ny = open ? g_nav_y.load() : 0;
  if (!open && !was_active) return;
  for (int i = 0; i < 10; ++i) io.AddKeyEvent(kKeys[i], (bits >> i) & 1);
  io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, nx < 0, nx < 0 ? 1.f : 0.f);
  io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, nx > 0, nx > 0 ? 1.f : 0.f);
  io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, ny < 0, ny < 0 ? 1.f : 0.f);
  io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, ny > 0, ny > 0 ? 1.f : 0.f);
  was_active = open;
}

const char* Gh2MenuPadHelp() {
  return "Guitar / gamepad: strum or D-pad = move, green (A) = select, red (B) = back, yellow / orange (or bumpers) = previous / next tab. Hold START + SELECT to close.";
}
