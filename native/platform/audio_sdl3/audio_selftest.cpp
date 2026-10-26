// gh2_audio_selftest - checks of the gh2plat audio sink. Nothing it does is audible.
//
//   gh2_audio_selftest            all checks (no sound device needed); exit status 0 = everything passed
//   gh2_audio_selftest device [S] feeds SILENCE to the real default playback device for S seconds and reports pacing / latency
#include <SDL3/SDL.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "audio_format.h"
#include "audio_sink.h"
#include "frame_ring.h"
#include "xma_port.h"

using namespace gh2plat::audio;
using Clock = std::chrono::steady_clock;

static_assert(kXmaMmioBase == 0x7FEA0000 && kXmaMmioSize == 0x10000 && kXmaRegKick + 4 * 10 <= kXmaRegLock && kXmaRegLock + 4 * 10 <= kXmaRegClear,
              "xma_port.h register layout is coherent");

namespace {

int g_failures = 0;
void Check(bool ok, const std::string& what, const std::string& detail = "") {
  std::printf("[%s] %s%s%s\n", ok ? "ok" : "!!", what.c_str(), detail.empty() ? "" : " - ", detail.c_str());
  if (!ok) ++g_failures;
}

// The game's frame for sequence number `seq`: channel c, sample s = a value that identifies all three; big-endian planar.
// `amp` scales the pattern: 1 = loud (values up to ~4096, past full scale: exercises the clamps), kQuiet = inside [-1, 1] (the 6-channel path clamps like the SDK's)
constexpr float kQuiet = 1.0f / 8192.0f;
float Expected(uint32_t seq, uint32_t c, uint32_t s, float amp = 1.0f) {
  return (static_cast<float>(seq % 4096) + static_cast<float>(c) * 0.125f + static_cast<float>(s) * (1.0f / 1024.0f)) * amp;
}

void MakeGuestFrame(uint32_t seq, uint8_t* out, float amp = 1.0f) {
  for (uint32_t c = 0; c < kChannels; ++c)
    for (uint32_t s = 0; s < kFrameSamples; ++s) {
      const float v = Expected(seq, c, s, amp);
      uint32_t bits;
      std::memcpy(&bits, &v, 4);
      bits = __builtin_bswap32(bits);
      std::memcpy(out + (static_cast<size_t>(c) * kFrameSamples + s) * 4, &bits, 4);
    }
}

// ---------------------------------------------------------------------------------------------------------------- format
void TestFormat() {
  std::printf("== guest frame -> interleaved\n");
  static_assert(kFrameBytes == 6144 && kFrameFloats == 1536, "6 channels x 256 samples x 4 bytes");
  Check(std::fabs(kFrameMs - 5.3333) < 1e-3, "a frame is 5.33 ms at 48 kHz");
  std::vector<uint8_t> buf(kFrameBytes + 8);
  uint8_t* aligned = buf.data();
  MakeGuestFrame(7, aligned);
  std::vector<float> out(kFrameFloats);
  GuestFrameToInterleaved(aligned, out.data());
  bool exact = true;
  for (uint32_t s = 0; s < kFrameSamples && exact; ++s)
    for (uint32_t c = 0; c < kChannels; ++c)
      if (out[s * kChannels + c] != Expected(7, c, s)) {
        exact = false;
        break;
      }
  Check(exact, "byte swap + planar -> interleaved is bit exact");
  std::memmove(buf.data() + 3, buf.data(), kFrameBytes);  // a misaligned guest pointer must work as well
  std::vector<float> out2(kFrameFloats);
  GuestFrameToInterleaved(buf.data() + 3, out2.data());
  Check(out2 == out, "a misaligned source gives the same result");
}

// ---------------------------------------------------------------------------------------------------------------- ring
void TestRing() {
  std::printf("== lock-free frame ring\n");
  FrameRing r(4, 8);
  Check(r.ConsumerPeek() == nullptr && r.SizeApprox() == 0, "starts empty");
  for (int i = 0; i < 4; ++i) {
    float* s = r.ProducerSlot();
    if (!s) {
      Check(false, "four slots are available");
      return;
    }
    s[0] = static_cast<float>(i);
    r.ProducerCommit();
  }
  Check(r.ProducerSlot() == nullptr && r.SizeApprox() == 4, "full after capacity frames");
  bool order = true;
  for (int i = 0; i < 4; ++i) {
    const float* f = r.ConsumerPeek();
    order &= f && f[0] == static_cast<float>(i);
    r.ConsumerPop();
  }
  Check(order && r.ConsumerPeek() == nullptr, "frames come out in order, then empty");

  // one producer, one consumer, 1.5 s: no loss, no duplicate, no corruption
  FrameRing q(64, 32);
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> produced{0};
  std::thread prod([&] {
    uint32_t seq = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      float* s = q.ProducerSlot();
      if (!s) continue;
      std::memcpy(s, &seq, 4);
      for (uint32_t i = 1; i < 32; ++i) s[i] = static_cast<float>(seq) * 0.5f + static_cast<float>(i);
      q.ProducerCommit();
      ++seq;
      produced.store(seq, std::memory_order_relaxed);
    }
  });
  uint32_t expect = 0;
  uint64_t bad = 0, got = 0;
  const auto t0 = Clock::now();
  while (Clock::now() - t0 < std::chrono::milliseconds(1500)) {
    const float* f = q.ConsumerPeek();
    if (!f) continue;
    uint32_t seq;
    std::memcpy(&seq, f, 4);
    if (seq != expect) ++bad;
    for (uint32_t i = 1; i < 32; ++i)
      if (f[i] != static_cast<float>(seq) * 0.5f + static_cast<float>(i)) ++bad;
    expect = seq + 1;
    ++got;
    q.ConsumerPop();
  }
  stop = true;
  prod.join();
  Check(bad == 0 && got > 100000, "SPSC stress: in order, intact", std::to_string(got) + " frames, " + std::to_string(bad) + " bad");
}

// ---------------------------------------------------------------------------------------------------------------- sink, software clock
void TestSinkClock() {
  std::printf("== sink with the software clock (paced like the render-driver worker)\n");
  std::vector<uint8_t> frame(kFrameBytes);
  {
    AudioSink sink;
    AudioSink::Options o;
    o.software_clock = true;
    o.queue_frames = 4;
    Check(sink.Open(o), "open");
    // pre-fill half the queue, then let the clock run
    uint32_t seq = 0;
    for (; seq < 2; ++seq) {
      MakeGuestFrame(seq, frame.data());
      sink.SubmitGuestFrame(frame.data());
    }
    sink.Start();
    const auto t0 = Clock::now();
    bool timeouts = false;
    while (Clock::now() - t0 < std::chrono::seconds(4)) {
      if (!sink.WaitForRoom(std::chrono::milliseconds(100))) {
        timeouts = true;
        break;
      }
      MakeGuestFrame(seq++, frame.data());
      sink.SubmitGuestFrame(frame.data());
    }
    const SinkStats s = sink.Stats();
    const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    std::printf("   %llu submitted in %.2f s (%.1f/s; the game does 187.5/s), %llu consumed, fill max %u, gap mean %.3f ms max %.2f ms, longest pull %.1f us\n",
                static_cast<unsigned long long>(s.submitted), secs, s.submitted / secs, static_cast<unsigned long long>(s.consumed), s.fill_max, s.submit_gap_mean_ms,
                s.submit_gap_max_ms, s.pull_max_us);
    Check(!timeouts, "WaitForRoom always got room");
    Check(s.underruns == 0 && s.silence_frames == 0 && s.dropped_full == 0, "paced production: no underrun, no drop", "underruns " + std::to_string(s.underruns));
    Check(std::fabs(s.submitted / secs - 187.5) < 4.0, "the pacing settles at the consumption rate (187.5 frames/s)");
    Check(s.fill_max <= o.queue_frames + 1, "the queue stays within queue_frames", "max fill " + std::to_string(s.fill_max));
    sink.Close();
  }
  {  // a stalled producer: starvation is silence + one counted underrun, then recovery
    AudioSink sink;
    AudioSink::Options o;
    o.software_clock = true;
    o.queue_frames = 4;
    sink.Open(o);
    for (uint32_t i = 0; i < 3; ++i) {
      MakeGuestFrame(i, frame.data());
      sink.SubmitGuestFrame(frame.data());
    }
    sink.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(120));  // 3 frames last 16 ms: the rest is starvation
    SinkStats s = sink.Stats();
    Check(s.underruns == 1 && s.silence_frames >= 15 && s.silence_frames <= 27, "a 120 ms stall: one underrun, silence for the missing frames",
          "underruns " + std::to_string(s.underruns) + ", silence frames " + std::to_string(s.silence_frames));
    for (uint32_t i = 0; i < 4; ++i) {
      MakeGuestFrame(i, frame.data());
      sink.SubmitGuestFrame(frame.data());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    s = sink.Stats();
    Check(s.consumed >= 3 && s.dropped_full == 0, "playback resumes with the frames that arrive", "consumed " + std::to_string(s.consumed));
    sink.Close();
  }
  {  // a producer that ignores WaitForRoom: the ring drops, counts, and never overflows
    AudioSink sink;
    AudioSink::Options o;
    o.software_clock = true;
    o.queue_frames = 4;
    sink.Open(o);  // not started: nothing consumes
    int accepted = 0;
    for (uint32_t i = 0; i < 100; ++i) {
      MakeGuestFrame(i, frame.data());
      accepted += sink.SubmitGuestFrame(frame.data());
    }
    const SinkStats s = sink.Stats();
    Check(accepted == 8 && s.dropped_full == 92 && s.fill_now == 8, "unpaced producer: ring of 8 takes 8, drops 92", "accepted " + std::to_string(accepted));
    const auto t0 = Clock::now();
    const bool room = sink.WaitForRoom(std::chrono::milliseconds(40));
    const double waited = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    Check(!room && waited >= 39 && waited < 60, "WaitForRoom times out when nothing consumes", std::to_string(waited) + " ms");
    std::thread closer([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      sink.Close();
    });
    const auto t1 = Clock::now();
    const bool woke = sink.WaitForRoom(std::chrono::seconds(5));
    const double w2 = std::chrono::duration<double, std::milli>(Clock::now() - t1).count();
    closer.join();
    Check(!woke && w2 < 500, "Close() wakes a waiting producer", std::to_string(w2) + " ms");
  }
}

// ---------------------------------------------------------------------------------------------------------------- sink, SDL stream
// The SDK's SDL3 has no 'disk' or 'dummy' audio driver (pipewire, pulseaudio, alsa, sndio, jack only), so the consumer step is driven
// by hand into a device-less SDL_AudioStream and its output is read back: the exact code of the device callback, bit-exact content.
void TestSinkStreamContent(float gain, bool mute) {
  std::printf("== consumer step into a real SDL_AudioStream (gain %.2f%s)\n", gain, mute ? ", muted" : "");
  Check(SDL_InitSubSystem(SDL_INIT_AUDIO), "SDL audio subsystem initialises", SDL_GetError());
  SDL_AudioSpec spec;
  SDL_zero(spec);
  spec.format = SDL_AUDIO_F32;
  spec.channels = static_cast<int>(kChannels);
  spec.freq = static_cast<int>(kSampleRate);
  SDL_AudioStream* stream = SDL_CreateAudioStream(&spec, &spec);
  Check(stream != nullptr, "device-less stream", SDL_GetError());
  if (!stream) return;
  AudioSink sink;
  AudioSink::Options o;
  o.software_clock = true;  // never started: the test is the only consumer
  o.queue_frames = 8;
  o.gain = gain;
  o.mute = mute;
  sink.Open(o);
  std::vector<uint8_t> frame(kFrameBytes);
  const uint32_t n = 8;
  bool accepted = true;
  for (uint32_t i = 0; i < n; ++i) {
    MakeGuestFrame(i + 100, frame.data(), kQuiet);
    accepted &= sink.SubmitGuestFrame(frame.data());
  }
  Check(accepted, "all frames accepted");
  sink.PullInto(stream, n + 2);  // two more than queued: two silent frames
  SinkStats s = sink.Stats();
  Check(s.consumed == n && s.silence_frames == 2 && s.underruns == 1, "n frames consumed, the two missing ones are silence, one underrun",
        "consumed " + std::to_string(s.consumed) + ", silence " + std::to_string(s.silence_frames) + ", underruns " + std::to_string(s.underruns));
  const int want = static_cast<int>((n + 2) * kFrameBytes);
  Check(SDL_GetAudioStreamAvailable(stream) == want, "the stream holds (n + 2) frames", std::to_string(SDL_GetAudioStreamAvailable(stream)));
  std::vector<float> pcm((n + 2) * kFrameFloats);
  const int got = SDL_GetAudioStreamData(stream, pcm.data(), want);
  Check(got == want, "read the stream back");
  bool exact = got == want;
  for (uint32_t i = 0; i < n && exact; ++i)
    for (uint32_t smp = 0; smp < kFrameSamples && exact; ++smp)
      for (uint32_t c = 0; c < kChannels; ++c) {
        const float expect = mute ? 0.0f : Expected(i + 100, c, smp, kQuiet) * gain;
        if (pcm[i * kFrameFloats + smp * kChannels + c] != expect) {
          exact = false;
          break;
        }
      }
  Check(exact, mute ? "muted: every consumed frame plays as silence" : "every played sample equals the guest's, times the gain, bit exact");
  bool silent = true;
  for (uint32_t i = n * kFrameFloats; i < pcm.size(); ++i) silent &= pcm[i] == 0.0f;
  Check(silent, "the starved frames are zeros");
  sink.Close();
  SDL_DestroyAudioStream(stream);
  SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

// A hot mix must clip at full scale in the 6-channel path too (the SDK clamps there; otherwise SDL would be handed values past 1.0).
void TestSixChannelClamp() {
  std::printf("== 6-channel path clips at full scale\n");
  Check(SDL_InitSubSystem(SDL_INIT_AUDIO), "SDL audio subsystem initialises", SDL_GetError());
  SDL_AudioSpec spec;
  SDL_zero(spec);
  spec.format = SDL_AUDIO_F32;
  spec.channels = static_cast<int>(kChannels);
  spec.freq = static_cast<int>(kSampleRate);
  SDL_AudioStream* stream = SDL_CreateAudioStream(&spec, &spec);
  AudioSink sink;
  AudioSink::Options o;
  o.software_clock = true;
  sink.Open(o);
  std::vector<uint8_t> frame(kFrameBytes);
  MakeGuestFrame(9, frame.data(), 1.0f);  // up to ~4096: far past full scale
  sink.SubmitGuestFrame(frame.data());
  sink.PullInto(stream, 1);
  std::vector<float> pcm(kFrameFloats);
  const int got = SDL_GetAudioStreamData(stream, pcm.data(), static_cast<int>(kFrameBytes));
  bool ok = got == static_cast<int>(kFrameBytes);
  bool saw_clip = false;
  for (uint32_t smp = 0; smp < kFrameSamples && ok; ++smp)
    for (uint32_t c = 0; c < kChannels; ++c) {
      const float want = std::clamp(Expected(9, c, smp), -1.0f, 1.0f);
      saw_clip |= want == 1.0f;
      if (pcm[smp * kChannels + c] != want) ok = false;
    }
  Check(ok && saw_clip, "every sample equals the guest's value clamped to [-1, 1]");
  sink.Close();
  SDL_DestroyAudioStream(stream);
  SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

// The stereo fold of a stereo / mono endpoint and the consumed-frame callback (the SDK's flow control hangs on it).
void TestSinkFoldAndCallback() {
  std::printf("== stereo fold and consumed-frame callback\n");
  Check(SDL_InitSubSystem(SDL_INIT_AUDIO), "SDL audio subsystem initialises", SDL_GetError());
  SDL_AudioSpec spec;
  SDL_zero(spec);
  spec.format = SDL_AUDIO_F32;
  spec.channels = 2;
  spec.freq = static_cast<int>(kSampleRate);
  SDL_AudioStream* stream = SDL_CreateAudioStream(&spec, &spec);
  Check(stream != nullptr, "device-less stereo stream", SDL_GetError());
  if (!stream) return;
  static std::atomic<int> consumed_calls{0};
  consumed_calls = 0;
  AudioSink sink;
  AudioSink::Options o;
  o.software_clock = true;
  o.output_channels = 2;
  o.queue_frames = 8;
  o.gain = 0.8f;
  o.on_frame_consumed = [](void*) { consumed_calls.fetch_add(1); };
  sink.Open(o);
  std::vector<uint8_t> frame(kFrameBytes);
  const uint32_t n = 6;
  for (uint32_t i = 0; i < n; ++i) {
    MakeGuestFrame(i + 1, frame.data());
    sink.SubmitGuestFrame(frame.data());
  }
  sink.PullInto(stream, n + 1);  // one starved frame at the end
  SinkStats s = sink.Stats();
  Check(s.output_channels == 2 && s.consumed == n && s.silence_frames == 1 && consumed_calls.load() == static_cast<int>(n),
        "n frames consumed, the callback fired once per real frame and not for the silent one",
        "callbacks " + std::to_string(consumed_calls.load()) + ", consumed " + std::to_string(s.consumed));
  const int bytes = static_cast<int>((n + 1) * kFrameSamples * 2 * sizeof(float));
  Check(SDL_GetAudioStreamAvailable(stream) == bytes, "the stereo stream holds (n + 1) frames of 2 channels", std::to_string(SDL_GetAudioStreamAvailable(stream)));
  std::vector<float> pcm((n + 1) * kFrameSamples * 2);
  const int got = SDL_GetAudioStreamData(stream, pcm.data(), bytes);
  Check(got == bytes, "read the stream back");
  // reference in double precision: L = (FL + FC*c + LFE*lfe + BL*sur) * scale * gain, clamped
  const StereoFold fold;
  double worst = 0;
  bool clamped_seen = false;
  for (uint32_t i = 0; i < n; ++i)
    for (uint32_t smp = 0; smp < kFrameSamples; ++smp) {
      auto ch = [&](uint32_t c) { return static_cast<double>(Expected(i + 1, c, smp)); };
      const double mid = ch(2) * fold.center + ch(3) * fold.lfe;
      double l = (ch(0) + mid + ch(4) * fold.surround) * fold.scale * 0.8;
      double r = (ch(1) + mid + ch(5) * fold.surround) * fold.scale * 0.8;
      if (std::fabs(l) > 1.0 || std::fabs(r) > 1.0) clamped_seen = true;
      l = std::clamp(l, -1.0, 1.0);
      r = std::clamp(r, -1.0, 1.0);
      worst = std::max({worst, std::fabs(l - pcm[(i * kFrameSamples + smp) * 2]), std::fabs(r - pcm[(i * kFrameSamples + smp) * 2 + 1])});
    }
  Check(worst < 5e-4, "the fold matches the reference formula (centre / surround -3 dB, LFE dropped, scale, gain, clamp)", "worst difference " + std::to_string(worst));
  Check(clamped_seen, "the test frames are loud enough to exercise the clamp");
  bool silent = true;
  for (uint32_t k = n * kFrameSamples * 2; k < pcm.size(); ++k) silent &= pcm[k] == 0.0f;
  Check(silent, "the starved frame is two-channel silence");
  sink.Close();
  SDL_DestroyAudioStream(stream);
  SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

// The real default playback device, fed silence (mute): nothing audible. Shows the device's period, the pacing against the device
// clock and the latency estimate. Needs a running sound server; not part of the default run.
void TestDevice(double seconds) {
  std::printf("== the default playback device, muted (%.0f s)\n", seconds);
  std::vector<uint8_t> frame(kFrameBytes);
  AudioSink sink;
  AudioSink::Options o;
  o.queue_frames = 8;
  o.device_frames = 512;
  o.mute = true;
  o.log = [](const char* line) { std::printf("   sink: %s\n", line); };
  if (!sink.Open(o)) return (void)Check(false, "open the default playback device", SDL_GetError());
  uint32_t seq = 0;
  for (; seq < 4; ++seq) {
    MakeGuestFrame(seq, frame.data());
    sink.SubmitGuestFrame(frame.data());
  }
  sink.Start();
  const auto t0 = Clock::now();
  double latency = 0;
  while (Clock::now() - t0 < std::chrono::duration<double>(seconds)) {
    if (!sink.WaitForRoom(std::chrono::milliseconds(100))) continue;
    MakeGuestFrame(seq++, frame.data());
    sink.SubmitGuestFrame(frame.data());
    if (seq == 400) latency = sink.LatencyMs();
  }
  const SinkStats s = sink.Stats();
  const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
  std::printf("   device period %u frames; %llu submitted in %.1f s (%.2f/s), consumed %llu, underruns %llu, dropped %llu, fill max %u, latency estimate %.1f ms, "
              "longest callback %.1f us, gap mean %.3f max %.2f ms\n",
              sink.device_period_frames(), static_cast<unsigned long long>(s.submitted), secs, s.submitted / secs, static_cast<unsigned long long>(s.consumed),
              static_cast<unsigned long long>(s.underruns), static_cast<unsigned long long>(s.dropped_full), s.fill_max, latency, s.pull_max_us, s.submit_gap_mean_ms,
              s.submit_gap_max_ms);
  Check(s.dropped_full == 0 && s.underruns <= 1, "the device clock and the paced producer agree: nothing dropped, at most the start-up underrun");
  sink.Close();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "device") {
    TestDevice(argc > 2 ? std::atof(argv[2]) : 8.0);
    std::printf("device test: %s\n", g_failures ? "FAILED" : "PASSED");
    return g_failures ? 1 : 0;
  }
  TestFormat();
  TestRing();
  TestSinkClock();
  TestSinkStreamContent(1.0f, false);
  TestSinkStreamContent(0.5f, false);
  TestSinkStreamContent(1.0f, true);
  TestSixChannelClamp();
  TestSinkFoldAndCallback();
  std::printf("selftest: %s\n", g_failures ? "FAILED" : "PASSED");
  return g_failures ? 1 : 0;
}
