// Driving the options menu with the guitar or the gamepad (no mouse, no keyboard).
//
// The pad state the game asks for passes through here first (Gh2PadGetState): holding START and SELECT/BACK together for 0.7 s opens or closes the menu, and while it is open the
// game receives a neutral pad (nothing is strummed into the song behind it) and the buttons move through the menu instead. A guitar reports itself as an Xbox pad: green = A,
// red = B, yellow = Y, blue = X, orange = left bumper, strum = D-pad up/down; the same mapping serves a normal gamepad.
#pragma once

#include <gh2rt/host_services.h>

#include <functional>

struct ImGuiIO;

// Guest thread (any thread): the real state of guest user `user`. Detects the open/close chord and remembers the buttons for the menu.
void Gh2MenuPadFeed(uint32_t user, const gh2rt::PadState& raw, bool guitar);
// True while the menu is open: the game must see a neutral pad.
bool Gh2MenuPadCapturing();
void Gh2MenuPadSetOpen(bool open);                        // the menu says whether it is on screen
void Gh2MenuPadSetToggle(std::function<void()> toggle);   // what the chord does (runs on the UI thread)
// UI thread, inside the menu's frame: hands the buttons to Dear ImGui's gamepad navigation.
void Gh2MenuPadApply(ImGuiIO& io);
// Taps of the previous / next tab buttons since the last call (negative = previous): the menu switches tabs itself, Dear ImGui's tab navigation needs a focused widget.
int Gh2MenuPadTabDelta();
// True once after the chord opened the menu: it should put the navigation cursor on its first widget.
bool Gh2MenuPadTakeOpenedByPad();
// A short text for the menu's footer.
const char* Gh2MenuPadHelp();
