// gh2plat audio sink: the host end of XAudioSubmitRenderDriverFrame.
//
//   guest render thread --SubmitGuestFrame--> [lock-free FrameRing, 256-sample x 6-channel frames] --Pull--> SDL3 audio stream
//
// * The guest thread converts a frame straight into a ring slot (byte swap + interleave, ~2 us) and never blocks; WaitForRoom() is the
//   pacing primitive the host's render-driver worker uses to ask the game for the next frame only when the queue has room (the
//   role of the SDK's per-client semaphore): queue_frames bounds the latency.
// * The consumer is the SDL audio callback (device mode) or a software clock thread (clock mode: no device, for tests and for
//   running next to the SDK's own output). Both use the same Pull(), which never allocates and never takes a lock.
// * Starvation is audible silence plus a counted underrun, never a stall; a full ring drops the new frame and counts it.
// * Channels: a surround device gets the 6 channels untouched (times the gain); a stereo or mono device gets the same stereo fold the
//   PoC always used (centre and surround at -3 dB, LFE dropped, scaled by 1/(1+0.707), clamped) instead of SDL's own mix.
// SDL3 public API only (the SDK's SDL3 build); no SDK.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>

#include "audio_format.h"
#include "frame_ring.h"

struct SDL_AudioStream;

namespace gh2plat::audio {

struct SinkStats {
  uint64_t submitted = 0;       // frames accepted into the ring
  uint64_t dropped_full = 0;    // frames refused because the ring was full (producer ran ahead without WaitForRoom)
  uint64_t consumed = 0;        // frames played (or discarded by the software clock)
  uint64_t underruns = 0;       // starvation events (consecutive missing frames count once)
  uint64_t silence_frames = 0;  // silent frames inserted while starved
  uint64_t pulls = 0;           // device callbacks / clock ticks
  uint32_t fill_now = 0;
  uint32_t fill_max = 0;
  double pull_max_us = 0;       // longest Pull()
  double submit_gap_max_ms = 0; // longest time between two submitted frames
  double submit_gap_mean_ms = 0;
  uint32_t output_channels = 0;  // 6 (surround), 2 (folded), 0 = not opened
};

class AudioSink {
 public:
  struct Options {
    uint32_t queue_frames = 8;     // frames the producer may run ahead of the consumer (WaitForRoom); ring capacity is the next power of two >= 2x
    uint32_t queue_frames_max = 0; // the ring is sized for this many queued frames so that SetQueueFrames can raise the queue while it plays (0 = queue_frames)
    uint32_t device_frames = 512;  // SDL device period in samples (SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES); 0 = SDL's default
    bool software_clock = false;   // no SDL device: a thread consumes one frame every 5.33 ms
    bool mute = false;             // consume and discard
    const std::atomic<bool>* external_mute = nullptr;  // also mute while this flag is set (the owner's option; must outlive the sink)
    float gain = 1.0f;
    uint32_t output_channels = 0;  // 0 = follow the device (6, or 2 with the fold on a stereo / mono device); 2 or 6 force it (tests)
    StereoFold fold;
    const char* app_name = nullptr;     // SDL_PROP_APP_METADATA_NAME_STRING, how the sound server names the stream; null = leave it alone
    // Called on the consumer thread after every REAL frame taken from the ring (not for silence): the owner's flow control
    // (the SDK's per-client semaphore today). Must not block.
    void (*on_frame_consumed)(void* user) = nullptr;
    void* on_frame_consumed_user = nullptr;
    const char* sdl_driver = nullptr;  // SDL_AUDIO_DRIVER override for tests; only honoured before SDL audio starts
    void (*log)(const char*) = nullptr;
  };

  // Changes how far the producer may run ahead (2 .. queue_frames_max): the owner raises it when the output starved, a lower value is a shorter delay.
  void SetQueueFrames(uint32_t frames) noexcept;
  uint32_t QueueFrames() const noexcept;

  AudioSink();
  ~AudioSink();  // Close()
  AudioSink(const AudioSink&) = delete;
  AudioSink& operator=(const AudioSink&) = delete;

  // Device mode: starts SDL's audio subsystem and opens a 6-channel float32 48 kHz stream on the default playback device (paused until
  // Start()). SDL converts / mixes down to what the device really is. Clock mode: nothing of SDL is touched.
  bool Open(const Options& options);
  void Start();
  void Stop();   // pauses; queued frames stay
  void Close();  // may be called from any thread while the producer is inside WaitForRoom(): it is woken and returns false

  // producer thread (exactly one)
  bool WaitForRoom(std::chrono::milliseconds timeout) noexcept;      // false on timeout or Close()
  bool SubmitGuestFrame(const uint8_t* big_endian_planar_frame) noexcept;  // false if dropped

  // The consumer step shared by the SDL device callback and the software clock: `frames` frames are due; they are played into `stream`
  // (queued with SDL_PutAudioStreamData; silence for a missing frame) or, with a null stream, discarded. Public so tests can drive it
  // without a device. Exactly one thread may be the consumer at a time.
  void PullInto(SDL_AudioStream* stream, uint32_t frames) noexcept;

  void SetGain(float gain) noexcept { gain_.store(gain, std::memory_order_relaxed); }
  void SetMute(bool mute) noexcept { mute_.store(mute, std::memory_order_relaxed); }
  SinkStats Stats() const noexcept;
  // queued frames + the device's own buffer, in ms (device mode)
  double LatencyMs() const noexcept;
  uint32_t device_period_frames() const { return device_period_frames_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::atomic<float> gain_{1.0f};
  std::atomic<bool> mute_{false};
  uint32_t device_period_frames_ = 0;
};

}  // namespace gh2plat::audio
