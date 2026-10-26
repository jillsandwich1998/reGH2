// gh2rt/hooks_api.h - state the hooks keep that the host application also needs (the logo-skip watcher, the options menu).
// This header must stay free of SDK headers and of the `rex::` namespace (native/tools/check_isolation.sh).
#pragma once

#include <cstdint>
#include <string>

namespace gh2rt::hooks {

double SinceStart();                                  // seconds since the hooks were loaded
bool InputTrace();                                    // GH2_INPUT_TRACE is set
bool SkipIntroEnabled();                              // any of the skip/autostart/startup-mode options is on
bool IntroDone();                                     // the intro (logos + video) is over for the skip logic
bool IntroVideoOpened();                              // the game opened intro.wmv
void FastForwardBegin(double speed, const char* why); // run the guest clock `speed` times faster until the intro video opens
std::string AudioStatsLine();                         // the native audio client's counters, empty when the runtime plays the audio (hooks_audio.cpp)
void QuiesceNativeThreads();                          // shutdown: freeze the game's native threads where they are (hooks_kthreads.cpp)

}  // namespace gh2rt::hooks

// Game packages (DLC) the game has opened in this session, for the options menu.
bool Gh2ContentOpened(const std::string& file_name);
uint32_t Gh2ContentEnumerations();
