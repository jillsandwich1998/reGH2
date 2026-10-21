#include "keyboard.h"

#include <SDL3/SDL_keyboard.h>
#include <rex/cvar.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <cstdlib>

REXCVAR_DEFINE_STRING(keyboard_player, "auto", "GH2",
                      "Who plays on the keyboard: auto (player 1, only when no controller is connected), off, or 1..4")
    .allowed({"auto", "off", "1", "2", "3", "4"});
REXCVAR_DEFINE_STRING(keyboard_strum, "pick", "GH2",
                      "Keyboard style: pick (strum keys needed) or frets_only (a fret key press strums by itself)")
    .allowed({"pick", "frets_only"});
REXCVAR_DEFINE_STRING(kb_green, "A", "GH2", "Keyboard key: green fret");
REXCVAR_DEFINE_STRING(kb_red, "S", "GH2", "Keyboard key: red fret");
REXCVAR_DEFINE_STRING(kb_yellow, "D", "GH2", "Keyboard key: yellow fret");
REXCVAR_DEFINE_STRING(kb_blue, "F", "GH2", "Keyboard key: blue fret");
REXCVAR_DEFINE_STRING(kb_orange, "G", "GH2", "Keyboard key: orange fret");
REXCVAR_DEFINE_STRING(kb_strum_up, "Up", "GH2", "Keyboard key: strum up (also menu up)");
REXCVAR_DEFINE_STRING(kb_strum_down, "Down", "GH2", "Keyboard key: strum down (also menu down)");
REXCVAR_DEFINE_STRING(kb_start, "Return", "GH2", "Keyboard key: start / pause");
REXCVAR_DEFINE_STRING(kb_select, "Space", "GH2", "Keyboard key: select (star power)");
REXCVAR_DEFINE_STRING(kb_whammy, "Left Shift", "GH2", "Keyboard key: whammy bar");

const Gh2KeyAction kGh2KeyActions[] = {
    {"Green fret", "kb_green"},       {"Red fret", "kb_red"},     {"Yellow fret", "kb_yellow"},
    {"Blue fret", "kb_blue"},         {"Orange fret", "kb_orange"}, {"Strum up / menu up", "kb_strum_up"},
    {"Strum down / menu down", "kb_strum_down"}, {"Start / pause", "kb_start"},
    {"Select / star power", "kb_select"},        {"Whammy bar", "kb_whammy"},
};
const int kGh2KeyActionCount = sizeof(kGh2KeyActions) / sizeof(kGh2KeyActions[0]);

namespace {

std::atomic<int> g_slot{-1};
std::atomic<int> g_scancode[10];  // by action index
std::atomic<bool> g_frets_only{false};
std::atomic<bool> g_capturing{false};
std::atomic<int64_t> g_in_song_ms{-1000000};
bool g_prev_keys[SDL_SCANCODE_COUNT];

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

int Gh2KeyboardSlot() { return g_slot.load(std::memory_order_relaxed); }
void Gh2SetKeyboardSlot(int slot) { g_slot.store(slot); }
void Gh2KeyboardNoteInSong() { g_in_song_ms.store(NowMs(), std::memory_order_relaxed); }

void Gh2KeyboardRefreshBindings() {
  for (int i = 0; i < kGh2KeyActionCount; ++i) {
    const std::string name = rex::cvar::GetFlagByName(kGh2KeyActions[i].cvar);
    g_scancode[i].store(name.empty() ? 0 : static_cast<int>(SDL_GetScancodeFromName(name.c_str())));
  }
  g_frets_only.store(rex::cvar::GetFlagByName("keyboard_strum") == "frets_only");
}

// Recording helper: when GH2_CTRL_R_CMD is set (scripts/record_gh2.sh), Ctrl+R in the game window runs that command once per
// press (it stops the OBS recording). Read here because the window has the keyboard focus while playing, which works on
// Wayland where a global hotkey cannot be seen.
static void CheckCtrlRHotkey() {
  static const char* cmd = std::getenv("GH2_CTRL_R_CMD");
  if (!cmd || !*cmd || SDL_GetKeyboardFocus() == nullptr) return;
  const bool* keys = SDL_GetKeyboardState(nullptr);
  if (!keys) return;
  static std::atomic<bool> prev{false};
  const bool now = keys[SDL_SCANCODE_R] && (keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL]);
  if (now && !prev.exchange(true)) std::thread([c = std::string(cmd)] { std::system(c.c_str()); }).detach();
  if (!now) prev.store(false);
}

Gh2KbState Gh2KeyboardPoll(uint32_t user) {
  CheckCtrlRHotkey();
  Gh2KbState out;
  const int slot = g_slot.load(std::memory_order_relaxed);
  if (slot < 0 || static_cast<uint32_t>(slot) != user) return out;
  out.active = true;
  if (g_capturing.load() || SDL_GetKeyboardFocus() == nullptr) return out;  // rebinding, or the game window is not focused
  const bool* keys = SDL_GetKeyboardState(nullptr);
  if (!keys) return out;
  const auto down = [&](int action) {
    const int sc = g_scancode[action].load(std::memory_order_relaxed);
    return sc > 0 && sc < SDL_SCANCODE_COUNT && keys[sc];
  };
  static const uint16_t kFretBits[5] = {0x1000, 0x2000, 0x8000, 0x4000, 0x0100};  // green red yellow blue orange
  uint16_t frets = 0;
  for (int i = 0; i < 5; ++i)
    if (down(i)) frets |= kFretBits[i];
  uint16_t b = frets;
  if (down(5)) b |= 0x0001;  // dpad up = strum up
  if (down(6)) b |= 0x0002;  // dpad down = strum down
  if (down(7)) b |= 0x0010;  // start
  if (down(8)) b |= 0x0020;  // back / select
  out.whammy = down(9);
  // Frets-only style: a new fret press strums by itself, but only inside a song (in the menus the green and red keys
  // are "continue" and "back" and must not also move the selection).
  static uint16_t prev_frets = 0;
  static int64_t strum_until = 0;
  const int64_t now = NowMs();
  if (g_frets_only.load() && (frets & ~prev_frets) != 0 && now - g_in_song_ms.load() < 500) strum_until = now + 60;
  prev_frets = frets;
  if (now < strum_until) b |= 0x0002;
  out.buttons = b;
  // Packet number: any change of the combined state counts.
  static uint32_t counter = 0;
  static uint32_t last = 0xFFFFFFFF;
  const uint32_t key = b | (out.whammy ? 0x10000u : 0u);
  if (key != last) {
    last = key;
    ++counter;
  }
  out.changes = counter;
  return out;
}

void Gh2KeyboardCaptureBegin() {
  const bool* keys = SDL_GetKeyboardState(nullptr);
  for (int i = 0; i < SDL_SCANCODE_COUNT; ++i) g_prev_keys[i] = keys && keys[i];
  g_capturing = true;
}

std::string Gh2KeyboardCapturePoll() {
  const bool* keys = SDL_GetKeyboardState(nullptr);
  if (!keys) return "";
  for (int i = 1; i < SDL_SCANCODE_COUNT; ++i) {
    const bool pressed = keys[i];
    if (pressed && !g_prev_keys[i]) {
      g_prev_keys[i] = true;
      return SDL_GetScancodeName(static_cast<SDL_Scancode>(i));
    }
    g_prev_keys[i] = pressed;
  }
  return "";
}

void Gh2KeyboardCaptureEnd() { g_capturing = false; }
