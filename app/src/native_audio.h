// The game's audio output: gh2plat's lock-free AudioSink (native/platform/audio_sdl3) instead of the SDK's SDL audio driver.
//
// The SDK's AudioSystem keeps what needs guest threads (client registration, the worker that calls the game's render callback, the XMA
// unit); only the DRIVER it creates per client is ours: the frame the game submits goes into the sink's ring, the SDL device callback
// plays it (6 channels, or the same stereo fold the PoC used on a stereo endpoint), and every real frame played releases the
// SDK's per-client semaphore, which is what makes the worker ask the game for the next frame.
// `native` is the default; `--audio_output=sdk` brings the SDK's driver back. GH2_AUDIO_TRACE=1 prints the sink's counters every 10 s.
#pragma once

#include <atomic>
#include <string>

namespace rex {
struct RuntimeConfig;
}

bool Gh2NativeAudioSelected();
// Call from OnPreSetup, after Gh2ApplyAudioLatency(): the runtime's audio system creates the native driver.
void Gh2InstallNativeAudio(rex::RuntimeConfig& config);
// One line with the sink's counters ("audio: ..."), or empty when the SDK's driver plays the sound (for the problem report).
std::string Gh2AudioStatsLine();
const std::atomic<bool>* Gh2AudioMuteFlag();   // true while the audio_mute option is on
