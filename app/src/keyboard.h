// Keyboard play for the GH2 port: the keyboard is emulated as a guitar-style controller (frets + optional strum keys,
// navigation, start/select) and merged into the state of one guest player. See controllers.cpp for who gets which slot.
#pragma once

#include <cstdint>
#include <string>

struct Gh2KbState {
  bool active = false;     // the keyboard feeds this guest user
  uint16_t buttons = 0;    // XINPUT_GAMEPAD button bits to OR in
  bool whammy = false;     // whammy key held
  uint32_t changes = 0;    // counts state changes (packet number bump)
};

// Slot (0..3) the keyboard currently plays as, or -1 (set by the player assignment, lock-free).
int Gh2KeyboardSlot();
void Gh2SetKeyboardSlot(int slot);
// Polls the keyboard for guest user `user` (guest input thread). Inactive unless the keyboard plays as that user.
Gh2KbState Gh2KeyboardPoll(uint32_t user);
// Called while a song's highway camera is updated: enables the "frets only" auto-strum.
void Gh2KeyboardNoteInSong();

// Key bindings (menu). Action ids index kKeyActions.
struct Gh2KeyAction {
  const char* label;
  const char* cvar;
};
extern const Gh2KeyAction kGh2KeyActions[];
extern const int kGh2KeyActionCount;
// Rebinding: Begin snapshots the keys held, Poll returns the name of the first key newly pressed ("" = none yet).
void Gh2KeyboardCaptureBegin();
std::string Gh2KeyboardCapturePoll();
void Gh2KeyboardCaptureEnd();
// Re-reads the key cvars (call after changing them).
void Gh2KeyboardRefreshBindings();
