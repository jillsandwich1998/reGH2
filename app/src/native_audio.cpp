#include "native_audio.h"

#include <rex/audio/audio_driver.h>
#include <rex/audio/audio_system.h>
#include <rex/cvar.h>
#include <rex/memory.h>
#include <rex/runtime.h>
#include <rex/thread.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

#include <gh2rt/hooks_api.h>

#include "audio_sink.h"

REXCVAR_DEFINE_STRING(audio_output, "native", "GH2",
                      "Who plays the game's audio: native (a lock-free SDL3 sink of ours; the default) or sdk (the SDK's SDL audio driver; kept as a "
                      "fallback)")
    .allowed({"native", "sdk"});

namespace {

using rex::X_STATUS;  // the X_STATUS_* macros expand to casts to this

class NativeAudioDriver;
std::atomic<NativeAudioDriver*> g_driver{nullptr};

// The SDK's `audio_mute` option (the F4 overlay / console can flip it). One process-wide flag fed by one callback registered once, so no callback can
// outlive a driver.
std::atomic<bool> g_mute{false};
void FollowMuteOption() {
  static std::once_flag once;
  std::call_once(once, [] {
    g_mute.store(rex::cvar::GetFlagByName("audio_mute") == "true");
    rex::cvar::RegisterChangeCallback("audio_mute", [](std::string_view, std::string_view value) { g_mute.store(value == "true"); });
  });
}

class NativeAudioDriver final : public rex::audio::AudioDriver {
 public:
  NativeAudioDriver(rex::memory::Memory* memory, rex::thread::Semaphore* semaphore) : AudioDriver(memory), semaphore_(semaphore) {}
  ~NativeAudioDriver() override { Shutdown(); }

  bool Initialize(uint32_t queue_frames) {
    gh2plat::audio::AudioSink::Options options;
    options.queue_frames = queue_frames;
    options.device_frames = 0;  // the device period comes from the app's SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES (audio_latency)
    options.app_name = "rexglue";  // what the sound server has always called the stream (routing rules may match it)
    FollowMuteOption();
    options.external_mute = &g_mute;
    options.on_frame_consumed = [](void* self) { static_cast<NativeAudioDriver*>(self)->semaphore_->Release(1, nullptr); };
    options.on_frame_consumed_user = this;
    options.log = [](const char* line) { fprintf(stderr, "AUDIO %s\n", line); };
    if (!sink_.Open(options)) return false;
    sink_.Start();
    g_driver.store(this);
    if (const char* e = getenv("GH2_AUDIO_TRACE"); e && e[0] && e[0] != '0') reporter_ = std::thread([this] { Report(); });
    return true;
  }

  void Shutdown() {
    NativeAudioDriver* self = this;
    g_driver.compare_exchange_strong(self, nullptr);
    stop_.store(true);
    if (reporter_.joinable()) reporter_.join();
    sink_.Close();
  }

  void SubmitFrame(uint32_t samples_ptr) override { sink_.SubmitGuestFrame(memory_->TranslateVirtual(samples_ptr)); }

  std::string StatsLine() const {
    const gh2plat::audio::SinkStats s = sink_.Stats();
    char buf[320];
    snprintf(buf, sizeof(buf), "audio: %u ch, %llu frames submitted, %llu played, %llu underruns, %llu dropped, queue %u (max %u), submit gap %.3f ms (max %.2f)",
             s.output_channels, static_cast<unsigned long long>(s.submitted), static_cast<unsigned long long>(s.consumed),
             static_cast<unsigned long long>(s.underruns), static_cast<unsigned long long>(s.dropped_full), s.fill_now, s.fill_max, s.submit_gap_mean_ms,
             s.submit_gap_max_ms);
    return buf;
  }

 private:
  void Report() {
    const auto t0 = std::chrono::steady_clock::now();
    int next = 10;
    while (!stop_.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      if (t < next) continue;
      next += 10;
      fprintf(stderr, "AUDIO T+%.0fs %s | latency estimate %.1f ms, longest device callback %.1f us\n", t, StatsLine().c_str(), sink_.LatencyMs(),
              sink_.Stats().pull_max_us);
    }
  }

  rex::thread::Semaphore* semaphore_;
  gh2plat::audio::AudioSink sink_;
  std::thread reporter_;
  std::atomic<bool> stop_{false};
};

class NativeAudioSystem final : public rex::audio::AudioSystem {
 public:
  static std::unique_ptr<rex::audio::AudioSystem> Create(rex::runtime::FunctionDispatcher* function_dispatcher) {
    return std::make_unique<NativeAudioSystem>(function_dispatcher);
  }
  explicit NativeAudioSystem(rex::runtime::FunctionDispatcher* function_dispatcher) : AudioSystem(function_dispatcher) {}

 protected:
  X_STATUS CreateDriver(size_t /*index*/, rex::thread::Semaphore* semaphore, rex::audio::AudioDriver** out_driver) override {
    auto driver = std::make_unique<NativeAudioDriver>(memory_, semaphore);
    if (!driver->Initialize(queued_frames_)) return X_STATUS_UNSUCCESSFUL;
    *out_driver = driver.release();
    return X_STATUS_SUCCESS;
  }
  void DestroyDriver(rex::audio::AudioDriver* driver) override { delete static_cast<NativeAudioDriver*>(driver); }
};

}  // namespace

bool Gh2NativeAudioSelected() { return REXCVAR_GET(audio_output) == "native"; }

void Gh2InstallNativeAudio(rex::RuntimeConfig& config) { config.audio_factory = REX_AUDIO_BACKEND(NativeAudioSystem); }

const std::atomic<bool>* Gh2AudioMuteFlag() {
  FollowMuteOption();
  return &g_mute;
}

std::string Gh2AudioStatsLine() {
  NativeAudioDriver* d = g_driver.load();
  if (d) return d->StatsLine();
  return gh2rt::hooks::AudioStatsLine();   // the native audio client (hooks_audio.cpp) keeps its own sink
}
