#include "audio_sink.h"

#include <SDL3/SDL.h>
#include <gh2os/os.h>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace gh2plat::audio {

namespace {

uint64_t NowNs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint32_t NextPow2(uint32_t v) {
  uint32_t p = 1;
  while (p < v) p <<= 1;
  return p;
}

void FutexWait(std::atomic<uint32_t>* addr, uint32_t expected, std::chrono::nanoseconds timeout) { gh2os::WaitOnWord(addr, expected, timeout); }

void FutexWake(std::atomic<uint32_t>* addr, int n) { gh2os::WakeWord(addr, n); }

void RaiseMax(std::atomic<uint64_t>& a, uint64_t v) {
  uint64_t prev = a.load(std::memory_order_relaxed);
  while (v > prev && !a.compare_exchange_weak(prev, v, std::memory_order_relaxed)) {}
}

}  // namespace

struct AudioSink::Impl {
  AudioSink* owner = nullptr;
  Options opt;
  std::unique_ptr<FrameRing> ring;
  std::vector<std::unique_ptr<FrameRing>> retired;  // rings of earlier Open() calls: a producer may still hold a pointer, so they live until the sink dies
  std::atomic<bool> opened{false};
  std::atomic<uint32_t> queue_frames{8};
  uint32_t queue_cap = 8;   // frames the ring was sized for
  std::atomic<SDL_AudioStream*> stream{nullptr};
  bool sdl_audio_started = false;
  std::thread clock;
  std::atomic<bool> stop{false};
  std::atomic<bool> paused{true};
  alignas(64) std::atomic<uint32_t> pops{0};  // futex word: bumped on every consumed frame
  std::atomic<int> waiters{0};
  float silence[kFrameFloats] = {};
  float scratch[kFrameFloats] = {};
  std::atomic<uint32_t> out_channels{kChannels};  // 6 or 2: decided when the device opens (the clock's 6 until then)

  // clock_until_device: the clock consumes until a device opens. A probe thread opens it (paused) and leaves it in `found`; the clock thread,
  // the consumer so far, takes it over and resumes it, then ends: the device callback is the consumer from then on (never two at once).
  bool look_for_device = false;
  std::chrono::steady_clock::time_point opened_at;
  std::mutex mode_mu;   // the hand-over vs Start / Stop / Close
  std::thread probe;
  std::atomic<bool> probing{false};
  std::atomic<SDL_AudioStream*> found{nullptr};
  uint32_t found_channels = 0;
  int found_period = 0;
  std::string found_name;
  std::atomic<bool> device_mode{false};
  bool in_underrun = false;  // consumer-private

  // stats
  std::atomic<uint64_t> submitted{0}, dropped_full{0}, consumed{0}, underruns{0}, silence_frames{0}, pulls{0}, fill_max{0}, pull_max_ns{0};
  std::atomic<uint64_t> gap_max_ns{0}, gap_sum_ns{0}, gap_count{0};
  uint64_t last_submit_ns = 0;  // producer-private

  void Log(const char* fmt, const char* a = "", const char* b = "") const {
    if (!opt.log) return;
    char buf[300];
    snprintf(buf, sizeof(buf), fmt, a, b);
    opt.log(buf);
  }

  void WakeProducer() {
    pops.fetch_add(1, std::memory_order_seq_cst);
    if (waiters.load(std::memory_order_seq_cst) != 0) FutexWake(&pops, 1);
  }

  // The one consumer path: `frames` frames are due. Played into `s` (device mode) or discarded (clock mode, s == nullptr).
  void Pull(uint32_t frames, SDL_AudioStream* s) noexcept {
    const uint64_t t0 = NowNs();
    pulls.fetch_add(1, std::memory_order_relaxed);
    const bool mute = owner->mute_.load(std::memory_order_relaxed) || opt.mute || (opt.external_mute && opt.external_mute->load(std::memory_order_relaxed));
    const float gain = owner->gain_.load(std::memory_order_relaxed);
    const int out_bytes = static_cast<int>(kFrameSamples * out_channels * sizeof(float));
    for (uint32_t i = 0; i < frames; ++i) {
      const float* f = ring->ConsumerPeek();
      if (f) {
        in_underrun = false;
        if (s) {
          if (mute) {
            SDL_PutAudioStreamData(s, silence, out_bytes);
          } else if (out_channels == 2) {
            FoldToStereo(f, scratch, opt.fold, gain);
            SDL_PutAudioStreamData(s, scratch, out_bytes);
          } else {
            ScaleClamp6(f, scratch, gain);
            SDL_PutAudioStreamData(s, scratch, out_bytes);
          }
        }
        ring->ConsumerPop();
        consumed.fetch_add(1, std::memory_order_relaxed);
        WakeProducer();
        if (opt.on_frame_consumed) opt.on_frame_consumed(opt.on_frame_consumed_user);
      } else {
        if (s) SDL_PutAudioStreamData(s, silence, out_bytes);
        silence_frames.fetch_add(1, std::memory_order_relaxed);
        if (!in_underrun) {
          underruns.fetch_add(1, std::memory_order_relaxed);
          in_underrun = true;
        }
      }
    }
    RaiseMax(pull_max_ns, NowNs() - t0);
  }

  static void SDLCALL DeviceCallback(void* userdata, SDL_AudioStream* s, int additional_amount, int /*total_amount*/) {
    if (additional_amount <= 0) return;
    auto* self = static_cast<Impl*>(userdata);
    const int frame_bytes = static_cast<int>(kFrameSamples * self->out_channels * sizeof(float));
    self->Pull(static_cast<uint32_t>((additional_amount + frame_bytes - 1) / frame_bytes), s);
  }

  void ClockMain() {
    gh2os::SetCurrentThreadName("gh2-audioclk");
    const auto period = std::chrono::nanoseconds(static_cast<int64_t>(kFrameSamples * 1e9 / kSampleRate));
    const auto t0 = std::chrono::steady_clock::now();
    auto next_look = t0 + std::chrono::seconds(2);
    uint64_t tick = 0;
    while (!stop.load(std::memory_order_acquire)) {
      ++tick;
      std::this_thread::sleep_until(t0 + period * tick);
      if (!paused.load(std::memory_order_acquire)) Pull(1, nullptr);
      if (!look_for_device) continue;
      if (SDL_AudioStream* s = found.exchange(nullptr, std::memory_order_acq_rel)) {
        HandOver(s);
        return;   // the device callback consumes from now on
      }
      const auto now = std::chrono::steady_clock::now();
      if (now >= next_look && !probing.load(std::memory_order_acquire)) {   // (opening a device can take a while: never on this thread, the clock must keep time)
        if (probe.joinable()) probe.join();
        probing.store(true, std::memory_order_release);
        probe = std::thread([this] { Probe(); });
        next_look = now + std::chrono::seconds(2);
      }
    }
  }

  // SDL's audio subsystem, with the hints of the options (they only count before it starts)
  bool StartSdl() {
    if (sdl_audio_started) return true;
    if (opt.sdl_driver) SDL_SetHint(SDL_HINT_AUDIO_DRIVER, opt.sdl_driver);
    if (opt.device_frames) SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, std::to_string(opt.device_frames).c_str());
    SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");
    if (opt.app_name) SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING, opt.app_name);
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return false;
    sdl_audio_started = true;
    return true;
  }

  void StopSdl() {
    if (!sdl_audio_started) return;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    sdl_audio_started = false;
  }

  bool PretendNoDevice() const {
    return opt.pretend_no_device_ms && std::chrono::steady_clock::now() < opened_at + std::chrono::milliseconds(opt.pretend_no_device_ms);
  }

  // A 6-channel float32 48 kHz stream on the default playback device (2 channels with the fold on a stereo or mono endpoint), paused.
  // Null when there is no device; `quiet` leaves the failure out of the log (the probe tries every 2 s).
  SDL_AudioStream* OpenStream(uint32_t* channels, int* period, std::string* name, bool quiet) {
    auto fail = [&](const char* what, SDL_AudioStream* s) -> SDL_AudioStream* {
      if (!quiet) Log("%s", what);
      if (s) SDL_DestroyAudioStream(s);
      return nullptr;
    };
    SDL_AudioSpec spec;
    SDL_zero(spec);
    spec.format = SDL_AUDIO_F32;
    spec.channels = static_cast<int>(opt.output_channels ? opt.output_channels : kChannels);
    spec.freq = static_cast<int>(kSampleRate);
    SDL_AudioStream* s = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &Impl::DeviceCallback, this);
    if (!s) return fail("SDL_OpenAudioDeviceStream failed", nullptr);
    SDL_AudioSpec dev;
    int p = 0;
    if (!SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(s), &dev, &p)) {
      dev = spec;
      p = 0;
    }
    // A stereo or mono endpoint gets the fold (a mono one is collapsed by SDL after it): handing SDL 6 channels would use SDL's own mix.
    if (!opt.output_channels && dev.channels <= 2) {
      SDL_DestroyAudioStream(s);
      spec.channels = 2;
      s = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &Impl::DeviceCallback, this);
      if (!s) return fail("SDL_OpenAudioDeviceStream (stereo) failed", nullptr);
      if (!SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(s), &dev, &p)) p = 0;
    }
    *channels = static_cast<uint32_t>(spec.channels);
    *period = p;
    const char* n = SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(s));
    *name = n ? n : "?";
    char line[200];
    snprintf(line, sizeof(line), "SDL audio endpoint '%s': %d ch, %d Hz, period %d frames; submitting %d ch", name->c_str(), dev.channels, dev.freq, p, spec.channels);
    Log("%s", line);
    return s;
  }

  // probe thread: is there a device now? Leaves an opened (paused) stream in `found` for the clock thread.
  void Probe() {
    gh2os::SetCurrentThreadName("gh2-audioprobe");
    if (!stop.load(std::memory_order_acquire) && !PretendNoDevice() && StartSdl()) {   // (SDL stays started: its device list follows what is plugged in)
      int n = 0;
      if (SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&n)) SDL_free(ids);
      uint32_t channels = 0;
      int period = 0;
      std::string name;
      if (n > 0) {
        if (SDL_AudioStream* s = OpenStream(&channels, &period, &name, true)) {
          found_channels = channels;
          found_period = period;
          found_name = name;
          found.store(s, std::memory_order_release);
        }
      }
    }
    probing.store(false, std::memory_order_release);
  }

  // clock thread, the consumer until now: the device stream takes over
  void HandOver(SDL_AudioStream* s) {
    std::lock_guard<std::mutex> lock(mode_mu);
    out_channels.store(found_channels);
    owner->device_period_frames_ = static_cast<uint32_t>(found_period);
    stream.store(s);
    device_mode.store(true);
    Log("audio output device '%s' is here: sound starts", found_name.c_str());
    if (!paused.load(std::memory_order_acquire)) SDL_ResumeAudioStreamDevice(s);
  }
};

AudioSink::AudioSink() : impl_(std::make_unique<Impl>()) { impl_->owner = this; }
AudioSink::~AudioSink() { Close(); }

void AudioSink::SetQueueFrames(uint32_t frames) noexcept { impl_->queue_frames.store(std::clamp<uint32_t>(frames, 2, impl_->queue_cap), std::memory_order_relaxed); }
uint32_t AudioSink::QueueFrames() const noexcept { return impl_->queue_frames.load(std::memory_order_relaxed); }

bool AudioSink::Open(const Options& options) {
  Impl& d = *impl_;
  if (d.opened.load()) return true;
  d.opt = options;
  d.queue_frames = std::max<uint32_t>(2, options.queue_frames);
  d.queue_cap = std::max<uint32_t>(d.queue_frames.load(), options.queue_frames_max);
  if (d.ring) d.retired.push_back(std::move(d.ring));
  d.ring = std::make_unique<FrameRing>(NextPow2(d.queue_cap * 2), kFrameFloats);
  gain_.store(options.gain);
  mute_.store(options.mute);
  d.stop.store(false);
  d.paused.store(true);
  d.out_channels = options.output_channels ? options.output_channels : kChannels;
  d.device_mode.store(false);
  d.look_for_device = false;
  d.opened_at = std::chrono::steady_clock::now();
  if (!options.software_clock) {
    const bool started = !d.PretendNoDevice() && d.StartSdl();
    if (!started && !d.PretendNoDevice()) d.Log("SDL audio failed to start: %s", SDL_GetError());
    uint32_t channels = 0;
    int period = 0;
    std::string name;
    SDL_AudioStream* s = started ? d.OpenStream(&channels, &period, &name, false) : nullptr;
    if (s) {
      d.out_channels = channels;
      device_period_frames_ = static_cast<uint32_t>(period);
      d.stream = s;
      d.device_mode.store(true);
      d.opened.store(true);
      return true;
    }
    d.StopSdl();
    if (!options.clock_until_device) return false;
    d.Log("no audio output device could be opened: the game runs silent (a software clock takes the frames) and a device is looked for every 2 s");
    d.look_for_device = true;
  }
  d.clock = std::thread([&d] { d.ClockMain(); });
  d.opened.store(true);
  return true;
}

void AudioSink::Start() {
  Impl& d = *impl_;
  std::lock_guard<std::mutex> lock(d.mode_mu);
  d.paused.store(false, std::memory_order_release);
  if (d.stream) SDL_ResumeAudioStreamDevice(d.stream);
}

void AudioSink::Stop() {
  Impl& d = *impl_;
  std::lock_guard<std::mutex> lock(d.mode_mu);
  d.paused.store(true, std::memory_order_release);
  if (d.stream) SDL_PauseAudioStreamDevice(d.stream);
}

bool AudioSink::UsingDevice() const noexcept { return impl_->device_mode.load(); }

void AudioSink::Close() {
  Impl& d = *impl_;
  if (!d.opened.exchange(false)) return;
  d.stop.store(true, std::memory_order_release);
  d.WakeProducer();
  if (d.clock.joinable()) d.clock.join();   // (it may have handed over to a device first)
  if (d.probe.joinable()) d.probe.join();
  if (SDL_AudioStream* s = d.found.exchange(nullptr)) SDL_DestroyAudioStream(s);   // opened by the probe, never handed over
  std::lock_guard<std::mutex> lock(d.mode_mu);
  if (SDL_AudioStream* s = d.stream.exchange(nullptr)) SDL_DestroyAudioStream(s);  // stops the device callback before returning
  d.device_mode.store(false);
  d.StopSdl();
  // The ring is NOT freed here: a producer blocked in WaitForRoom() (woken above) or inside SubmitGuestFrame() may still be reading it.
}

void AudioSink::PullInto(SDL_AudioStream* stream, uint32_t frames) noexcept { impl_->Pull(frames, stream); }

bool AudioSink::WaitForRoom(std::chrono::milliseconds timeout) noexcept {
  Impl& d = *impl_;
  if (!d.ring || d.stop.load(std::memory_order_acquire)) return false;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!d.stop.load(std::memory_order_acquire)) {
    if (d.ring->SizeApprox() < d.queue_frames.load(std::memory_order_relaxed)) return true;
    d.waiters.fetch_add(1, std::memory_order_seq_cst);
    const uint32_t seen = d.pops.load(std::memory_order_seq_cst);
    const auto now = std::chrono::steady_clock::now();
    if (d.ring->SizeApprox() >= d.queue_frames.load(std::memory_order_relaxed) && !d.stop.load(std::memory_order_acquire) && now < deadline)
      FutexWait(&d.pops, seen, std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now));
    d.waiters.fetch_sub(1, std::memory_order_seq_cst);
    if (std::chrono::steady_clock::now() >= deadline) return d.ring->SizeApprox() < d.queue_frames.load(std::memory_order_relaxed);
  }
  return false;
}

bool AudioSink::SubmitGuestFrame(const uint8_t* frame) noexcept {
  Impl& d = *impl_;
  if (!d.ring || d.stop.load(std::memory_order_acquire)) return false;
  const uint64_t now = NowNs();
  if (d.last_submit_ns) {
    const uint64_t gap = now - d.last_submit_ns;
    RaiseMax(d.gap_max_ns, gap);
    d.gap_sum_ns.fetch_add(gap, std::memory_order_relaxed);
    d.gap_count.fetch_add(1, std::memory_order_relaxed);
  }
  d.last_submit_ns = now;
  float* slot = d.ring->ProducerSlot();
  if (!slot) {
    d.dropped_full.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  GuestFrameToInterleaved(frame, slot);
  d.ring->ProducerCommit();
  d.submitted.fetch_add(1, std::memory_order_relaxed);
  RaiseMax(d.fill_max, d.ring->SizeApprox());
  return true;
}

SinkStats AudioSink::Stats() const noexcept {
  const Impl& d = *impl_;
  SinkStats s;
  s.submitted = d.submitted.load(std::memory_order_relaxed);
  s.dropped_full = d.dropped_full.load(std::memory_order_relaxed);
  s.consumed = d.consumed.load(std::memory_order_relaxed);
  s.underruns = d.underruns.load(std::memory_order_relaxed);
  s.silence_frames = d.silence_frames.load(std::memory_order_relaxed);
  s.pulls = d.pulls.load(std::memory_order_relaxed);
  s.output_channels = d.opened.load(std::memory_order_relaxed) ? d.out_channels.load(std::memory_order_relaxed) : 0;
  s.fill_now = d.ring ? d.ring->SizeApprox() : 0;
  s.fill_max = static_cast<uint32_t>(d.fill_max.load(std::memory_order_relaxed));
  s.pull_max_us = d.pull_max_ns.load(std::memory_order_relaxed) / 1000.0;
  s.submit_gap_max_ms = d.gap_max_ns.load(std::memory_order_relaxed) / 1e6;
  const uint64_t n = d.gap_count.load(std::memory_order_relaxed);
  s.submit_gap_mean_ms = n ? d.gap_sum_ns.load(std::memory_order_relaxed) / 1e6 / static_cast<double>(n) : 0;
  return s;
}

double AudioSink::LatencyMs() const noexcept {
  const Impl& d = *impl_;
  double ms = d.ring ? d.ring->SizeApprox() * kFrameMs : 0;
  ms += 1000.0 * device_period_frames_ / kSampleRate;
  if (SDL_AudioStream* st = d.stream.load()) ms += 1000.0 * (SDL_GetAudioStreamQueued(st) / static_cast<double>(d.out_channels.load() * sizeof(float))) / kSampleRate;
  return ms;
}

}  // namespace gh2plat::audio
