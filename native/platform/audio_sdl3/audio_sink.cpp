#include "audio_sink.h"

#include <SDL3/SDL.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
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

void FutexWait(std::atomic<uint32_t>* addr, uint32_t expected, std::chrono::nanoseconds timeout) {
  timespec ts;
  ts.tv_sec = static_cast<time_t>(timeout.count() / 1'000'000'000);
  ts.tv_nsec = static_cast<long>(timeout.count() % 1'000'000'000);
  syscall(SYS_futex, reinterpret_cast<uint32_t*>(addr), FUTEX_WAIT_PRIVATE, expected, &ts, nullptr, 0);
}

void FutexWake(std::atomic<uint32_t>* addr, int n) { syscall(SYS_futex, reinterpret_cast<uint32_t*>(addr), FUTEX_WAKE_PRIVATE, n, nullptr, nullptr, 0); }

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
  SDL_AudioStream* stream = nullptr;
  bool sdl_audio_started = false;
  std::thread clock;
  std::atomic<bool> stop{false};
  std::atomic<bool> paused{true};
  alignas(64) std::atomic<uint32_t> pops{0};  // futex word: bumped on every consumed frame
  std::atomic<int> waiters{0};
  float silence[kFrameFloats] = {};
  float scratch[kFrameFloats] = {};
  uint32_t out_channels = kChannels;  // 6 or 2, decided in Open() and fixed while open
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
    pthread_setname_np(pthread_self(), "gh2-audioclk");
    const auto period = std::chrono::nanoseconds(static_cast<int64_t>(kFrameSamples * 1e9 / kSampleRate));
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t tick = 0;
    while (!stop.load(std::memory_order_acquire)) {
      ++tick;
      std::this_thread::sleep_until(t0 + period * tick);
      if (!paused.load(std::memory_order_acquire)) Pull(1, nullptr);
    }
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
  if (options.software_clock) {
    d.clock = std::thread([&d] { d.ClockMain(); });
    d.opened.store(true);
    return true;
  }
  if (options.sdl_driver) SDL_SetHint(SDL_HINT_AUDIO_DRIVER, options.sdl_driver);
  if (options.device_frames) SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, std::to_string(options.device_frames).c_str());
  SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");
  if (options.app_name) SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING, options.app_name);
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    d.Log("SDL audio failed to start: %s", SDL_GetError());
    return false;
  }
  d.sdl_audio_started = true;
  auto fail = [&](const char* what) {
    d.Log("%s", what);
    if (d.stream) {
      SDL_DestroyAudioStream(d.stream);
      d.stream = nullptr;
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    d.sdl_audio_started = false;
    return false;
  };
  SDL_AudioSpec spec;
  SDL_zero(spec);
  spec.format = SDL_AUDIO_F32;
  spec.channels = static_cast<int>(options.output_channels ? options.output_channels : kChannels);
  spec.freq = static_cast<int>(kSampleRate);
  d.out_channels = static_cast<uint32_t>(spec.channels);
  d.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &Impl::DeviceCallback, &d);
  if (!d.stream) return fail("SDL_OpenAudioDeviceStream failed");
  SDL_AudioSpec dev;
  int period = 0;
  if (!SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(d.stream), &dev, &period)) {
    dev = spec;
    period = 0;
  }
  // A stereo or mono endpoint gets the fold (a mono one is collapsed by SDL after it): handing SDL 6 channels would use SDL's own mix.
  if (!options.output_channels && dev.channels <= 2) {
    SDL_DestroyAudioStream(d.stream);
    d.stream = nullptr;
    spec.channels = 2;
    d.out_channels = 2;
    d.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &Impl::DeviceCallback, &d);
    if (!d.stream) return fail("SDL_OpenAudioDeviceStream (stereo) failed");
    if (!SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(d.stream), &dev, &period)) period = 0;
  }
  device_period_frames_ = static_cast<uint32_t>(period);
  const char* name = SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(d.stream));
  char line[200];
  snprintf(line, sizeof(line), "SDL audio endpoint '%s': %d ch, %d Hz, period %d frames; submitting %u ch", name ? name : "?", dev.channels, dev.freq, period,
           d.out_channels);
  d.Log("%s", line);
  d.opened.store(true);
  return true;
}

void AudioSink::Start() {
  Impl& d = *impl_;
  d.paused.store(false, std::memory_order_release);
  if (d.stream) SDL_ResumeAudioStreamDevice(d.stream);
}

void AudioSink::Stop() {
  Impl& d = *impl_;
  d.paused.store(true, std::memory_order_release);
  if (d.stream) SDL_PauseAudioStreamDevice(d.stream);
}

void AudioSink::Close() {
  Impl& d = *impl_;
  if (!d.opened.exchange(false)) return;
  d.stop.store(true, std::memory_order_release);
  d.WakeProducer();
  if (d.clock.joinable()) d.clock.join();
  if (d.stream) {
    SDL_DestroyAudioStream(d.stream);  // stops the device callback before returning
    d.stream = nullptr;
  }
  if (d.sdl_audio_started) {
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    d.sdl_audio_started = false;
  }
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
  s.output_channels = d.opened.load(std::memory_order_relaxed) ? d.out_channels : 0;
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
  if (d.stream) ms += 1000.0 * (SDL_GetAudioStreamQueued(d.stream) / static_cast<double>(d.out_channels * sizeof(float))) / kSampleRate;
  return ms;
}

}  // namespace gh2plat::audio
