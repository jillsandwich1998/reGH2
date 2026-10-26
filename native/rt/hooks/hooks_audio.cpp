// The game's audio client, natively (Phase 2.7, K.2): XAudioRegisterRenderDriverClient / XAudioSubmitRenderDriverFrame / XAudioUnregisterRenderDriverClient and the two volume queries are
// answered here instead of by the runtime's AudioSystem. --audio_pump=native (default) / sdk.
//
// What the runtime does, and what replaces it:
//   * RegisterClient: a client semaphore is filled with `queue_frames` credits, a driver is created, the game's callback and a pointer to a 4-byte cell holding its argument are stored.
//     -> here: the same AudioSink the app's driver used (SDL3 stream, lock-free 256-sample x 6-channel frame ring), the same number of credits, the same cell.
//   * Audio worker thread (a runtime thread): waits for a credit (500 ms at most), then runs the callback with the cell's address as its argument; the callback produces one frame and hands
//     it to XAudioSubmitRenderDriverFrame; a credit comes back every time the output takes a real frame.
//     -> here: a native guest thread (gh2k::thread: PCR, KTHREAD, TLS, stack) runs that loop; the sink's consumer callback returns the credits.
//   * XAudioSubmitRenderDriverFrame -> AudioSink::SubmitGuestFrame on the guest frame (byte swap + channel interleave in the sink, unchanged).
// Not replaced: XMA (its decoder keeps running in the runtime, the game reaches it through MMIO), and a second client (the game registers one; a second registration goes to the runtime).
// Needs --threads=native. No SDK dependency.
#include "gh2_pch.h"  // PPCContext, REX_EXTERN

#include <audio_sink.h>
#include <gh2abi/abi.h>
#include <gh2rt/boundary_trace.h>
#include <gh2rt/host_services.h>

#include <atomic>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <semaphore>
#include <string>
#include <thread>

#include "hooks_internal.h"

#define AUDIO_REAL(NAME) REX_EXTERN(__real___imp__##NAME)
AUDIO_REAL(XAudioRegisterRenderDriverClient);
AUDIO_REAL(XAudioUnregisterRenderDriverClient);
AUDIO_REAL(XAudioSubmitRenderDriverFrame);
AUDIO_REAL(XAudioGetVoiceCategoryVolumeChangeMask);
AUDIO_REAL(XAudioGetVoiceCategoryVolume);

namespace {

constexpr uint32_t kDriverToken = 0x41550000u;   // the runtime's driver handles are 0x41550000 | client index
constexpr uint32_t kErrorInvalidArg = 0x80070057u;
constexpr uint32_t kMaxQueuedFrames = 64;

struct Client {
  gh2plat::audio::AudioSink sink;
  std::counting_semaphore<kMaxQueuedFrames> credits{0};
  std::atomic<bool> stop{false}, pump_done{false};
  uint32_t callback = 0, cell = 0;
  std::atomic<uint64_t> callbacks{0}, submitted_calls{0};
  std::thread reporter, adapter;
};

std::mutex g_mu;
Client* g_client = nullptr;   // at most one; never freed once created (a late submit must find it)

bool Active() {
  static const bool on = [] {
    const gh2rt::Settings s = gh2rt::Host().GetSettings();
    std::string mode = s.audio_pump;
    if (const char* e = getenv("GH2_AUDIO_PUMP"); e && *e) mode = e;
    if (mode != "native") return false;
    if (!gh2rt::hooks::detail::NativeThreadsActive()) {
      fprintf(stderr, "AUDIO pump native needs --threads=native: staying with the runtime's\n");
      return false;
    }
    fprintf(stderr, "AUDIO pump native: the game's render-driver callback runs on a native thread and feeds the SDL3 sink directly\n");
    return true;
  }();
  return on;
}

uint32_t Be32(const uint8_t* b, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, b + a, 4);
  return __builtin_bswap32(v);
}
void PutBe32(uint8_t* b, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(b + a, &v, 4);
}

std::string StatsLineOf(Client& c) {
  const gh2plat::audio::SinkStats s = c.sink.Stats();
  char buf[360];
  snprintf(buf, sizeof(buf), "audio: %u ch, %llu frames submitted, %llu played, %llu underruns, %llu dropped, queue %u (max %u), submit gap %.3f ms (max %.2f) | %llu callbacks",
           s.output_channels, (unsigned long long)s.submitted, (unsigned long long)s.consumed, (unsigned long long)s.underruns, (unsigned long long)s.dropped_full, s.fill_now, s.fill_max,
           s.submit_gap_mean_ms, s.submit_gap_max_ms, (unsigned long long)c.callbacks.load());
  return buf;
}

}  // namespace

namespace gh2rt::hooks {
std::string AudioStatsLine() {
  Client* c = g_client;
  return c ? StatsLineOf(*c) : std::string();
}
}  // namespace gh2rt::hooks

// The mixer thread gets real-time round-robin scheduling at a low priority when the user may (rtprio limit, as audio users and rtkit-less setups have), else a better nice value.
// RLIMIT_RTTIME makes a runaway real-time thread die with SIGXCPU instead of freezing the machine; the mixer sleeps on the credit semaphore every frame, far below the limit.
// GH2_AUDIO_RT=0 turns this off.
void RaiseAudioThreadPriority() {
  if (const char* e = getenv("GH2_AUDIO_RT"); e && e[0] == '0') return;
  struct rlimit rt = {};
  if (getrlimit(RLIMIT_RTTIME, &rt) == 0 && (rt.rlim_cur == RLIM_INFINITY || rt.rlim_cur > 1000000)) {
    rt.rlim_cur = 1000000;   // 1 s of CPU without sleeping
    if (rt.rlim_max != RLIM_INFINITY && rt.rlim_max < rt.rlim_cur) rt.rlim_cur = rt.rlim_max;
    setrlimit(RLIMIT_RTTIME, &rt);
  }
  sched_param param = {};
  param.sched_priority = 4;
  if (pthread_setschedparam(pthread_self(), SCHED_RR, &param) == 0) {
    fprintf(stderr, "AUDIO mixer thread: real-time round-robin priority %d\n", param.sched_priority);
    return;
  }
  if (setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), -8) == 0) fprintf(stderr, "AUDIO mixer thread: nice -8 (no real-time permission)\n");
  else fprintf(stderr, "AUDIO mixer thread: normal priority (no permission to raise it)\n");
}

// XAudioRegisterRenderDriverClient(callback_ptr -> {callback, argument}, driver_ptr*) -> status
REX_EXTERN(__wrap___imp__XAudioRegisterRenderDriverClient) {
  GH2_TRACE_HOOK("XAudioRegisterRenderDriverClient");
  std::lock_guard<std::mutex> lock(g_mu);
  if (!Active() || (g_client && !g_client->stop.load())) { __real___imp__XAudioRegisterRenderDriverClient(ctx, base); return; }
  const uint32_t callback_ptr = ctx.r3.u32, driver_ptr = ctx.r4.u32;
  if (!callback_ptr) { ctx.r3.u64 = kErrorInvalidArg; return; }
  const uint32_t callback = Be32(base, callback_ptr), argument = Be32(base, callback_ptr + 4);
  if (!callback) { ctx.r3.u64 = kErrorInvalidArg; return; }
  const uint32_t cell = gh2rt::hooks::detail::GuestScratchAlloc(base);
  if (!cell) { ctx.r3.u64 = 0xC0000017u; return; }
  PutBe32(base, cell, argument);   // the callback receives the address of a cell that holds the game's own argument

  const gh2rt::Settings settings = gh2rt::Host().GetSettings();
  const uint32_t queue_frames = settings.audio_queue_frames < 2 ? 2 : settings.audio_queue_frames > kMaxQueuedFrames ? kMaxQueuedFrames : settings.audio_queue_frames;
  auto* client = new Client();
  client->callback = callback;
  client->cell = cell;
  gh2plat::audio::AudioSink::Options options;
  options.queue_frames = queue_frames;
  options.queue_frames_max = settings.audio_adaptive ? 8 : queue_frames;   // room for the adaptive growth
  options.device_frames = 0;       // the device period comes from the application's SDL hint (audio_latency)
  options.app_name = "rexglue";    // what the sound server has always called the stream (routing rules may match it)
  options.external_mute = gh2rt::Host().AudioMuteFlag();
  options.on_frame_consumed = [](void* self) { static_cast<Client*>(self)->credits.release(); };   // the output took a real frame: the game may produce another
  options.on_frame_consumed_user = client;
  options.log = [](const char* line) { fprintf(stderr, "AUDIO %s\n", line); };
  if (!client->sink.Open(options)) {
    fprintf(stderr, "AUDIO pump native: the sink did not open; leaving the audio to the runtime\n");
    delete client;
    __real___imp__XAudioRegisterRenderDriverClient(ctx, base);
    return;
  }
  client->sink.Start();
  client->credits.release(static_cast<std::ptrdiff_t>(queue_frames));   // the runtime hands the game a full queue's worth of credits at registration

  PPCFunc* fn = gh2abi::ResolveIndirectFunction(callback);
  if (!fn) {
    fprintf(stderr, "AUDIO pump native: no function at %08X\n", callback);
    client->sink.Close();
    delete client;
    __real___imp__XAudioRegisterRenderDriverClient(ctx, base);
    return;
  }
  const bool started = gh2rt::hooks::detail::StartGuestHostThread(
      base, ctx.r13.u32, 0x40000, "gh2-audio", callback, [client, fn, cell](PPCContext& tctx, uint8_t* tbase) {
        RaiseAudioThreadPriority();   // a late frame is a click: the mixer must not wait behind the game's own threads
        const uint64_t sp = tctx.r1.u64;
        while (!client->stop.load()) {
          gh2rt::hooks::detail::ParkIfQuiescing();
          if (!client->credits.try_acquire_for(std::chrono::milliseconds(500))) continue;
          if (client->stop.load()) break;
          tctx.r1.u64 = sp - (64 + 112);   // the dispatcher pads the frame a little below the stack pointer before calling a guest function
          tctx.lr = 0xBCBCBCBC;
          tctx.r3.u64 = cell;
          fn(tctx, tbase);
          tctx.r1.u64 = sp;
          ++client->callbacks;
        }
        client->pump_done.store(true);
      });
  if (!started) {
    client->sink.Close();
    delete client;
    __real___imp__XAudioRegisterRenderDriverClient(ctx, base);
    return;
  }
  if (settings.audio_adaptive) {   // starved output: one more frame of delay, for good (a click is worse than 5 ms)
    client->adapter = std::thread([client] {
      uint64_t seen = client->sink.Stats().underruns, window_start = seen;
      int window = 0;
      while (!client->stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const uint64_t now = client->sink.Stats().underruns;
        if (++window >= 10) {   // the burst window: 5 s
          window = 0;
          window_start = now;
        }
        if (now - window_start >= 2 && client->sink.QueueFrames() < 8) {   // two starved moments inside one window
          const uint32_t q = client->sink.QueueFrames() + 1;
          client->sink.SetQueueFrames(q);
          client->credits.release(1);
          fprintf(stderr, "AUDIO adaptive: the output starved %llu times, queue raised to %u frames (%.1f ms)\n", (unsigned long long)(now - window_start), q, q * gh2plat::audio::kFrameMs);
          window_start = now;
          window = 0;
        }
        seen = now;
      }
    });
  }
  if (const char* e = getenv("GH2_AUDIO_TRACE"); e && e[0] && e[0] != '0')
    client->reporter = std::thread([client] {
      const auto t0 = std::chrono::steady_clock::now();
      int next = 10;
      while (!client->stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (t < next) continue;
        next += 10;
        fprintf(stderr, "AUDIO T+%.0fs %s | latency estimate %.1f ms, longest device callback %.1f us\n", t, StatsLineOf(*client).c_str(), client->sink.LatencyMs(),
                client->sink.Stats().pull_max_us);
      }
    });
  g_client = client;
  PutBe32(base, driver_ptr, kDriverToken);
  ctx.r3.u64 = 0;
  fprintf(stderr, "AUDIO client registered natively: callback %08X, %u queued frames\n", callback, queue_frames);
}

// XAudioUnregisterRenderDriverClient(driver) -> status
REX_EXTERN(__wrap___imp__XAudioUnregisterRenderDriverClient) {
  GH2_TRACE_HOOK("XAudioUnregisterRenderDriverClient");
  Client* c = g_client;
  if (!Active() || !c || (ctx.r3.u32 & 0xFFFF0000u) != kDriverToken) { __real___imp__XAudioUnregisterRenderDriverClient(ctx, base); return; }
  c->stop.store(true);
  c->credits.release();
  for (int i = 0; i < 200 && !c->pump_done.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  c->sink.Close();
  if (c->reporter.joinable()) c->reporter.join();
  if (c->adapter.joinable()) c->adapter.join();
  ctx.r3.u64 = 0;   // the Client object is kept: a stray submit after this must not touch freed memory
}

// XAudioSubmitRenderDriverFrame(driver, samples) -> status
REX_EXTERN(__wrap___imp__XAudioSubmitRenderDriverFrame) {
  GH2_TRACE_HOOK("XAudioSubmitRenderDriverFrame");
  Client* c = g_client;
  if (!Active() || !c || (ctx.r3.u32 & 0xFFFF0000u) != kDriverToken) { __real___imp__XAudioSubmitRenderDriverFrame(ctx, base); return; }
  c->sink.SubmitGuestFrame(base + ctx.r4.u32);
  ++c->submitted_calls;
  ctx.r3.u64 = 0;
}

// XAudioGetVoiceCategoryVolumeChangeMask(driver, out*) -> status; no category volume ever changes. The game polls this in a loop: the runtime yields for a microsecond.
REX_EXTERN(__wrap___imp__XAudioGetVoiceCategoryVolumeChangeMask) {
  GH2_TRACE_HOOK("XAudioGetVoiceCategoryVolumeChangeMask");
  if (!Active() || !g_client || (ctx.r3.u32 & 0xFFFF0000u) != kDriverToken) { __real___imp__XAudioGetVoiceCategoryVolumeChangeMask(ctx, base); return; }
  std::this_thread::sleep_for(std::chrono::microseconds(1));
  PutBe32(base, ctx.r4.u32, 0);
  ctx.r3.u64 = 0;
}

// XAudioGetVoiceCategoryVolume(category, out*) -> status; full volume
REX_EXTERN(__wrap___imp__XAudioGetVoiceCategoryVolume) {
  GH2_TRACE_HOOK("XAudioGetVoiceCategoryVolume");
  if (!Active() || !g_client) { __real___imp__XAudioGetVoiceCategoryVolume(ctx, base); return; }
  PutBe32(base, ctx.r4.u32, 0x3F800000u);   // 1.0f
  ctx.r3.u64 = 0;
}
