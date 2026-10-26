// VdSwap hook (moved from app/src/gh2_host.cpp): guest frame pacing, the always-on slow-frame monitor, intro-skip frame logic.
// No SDK dependency.
//
// Guest frame pacing (GH2_FRAME_TRACE=1): intervals between the game's VdSwap calls, i.e. the frame rate the game really runs at
// (MangoHud only sees the host presenter, which repeats frames when the guest is late).
#include "gh2_pch.h"  // PPCContext, REX_EXTERN

#include <gh2rt/boundary_trace.h>
#include <gh2gfx/d3d_probe.h>
#include <gh2rt/host_services.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <vector>

#include "hooks_internal.h"

using gh2rt::Host;
using gh2rt::hooks::SinceStart;
using namespace gh2rt::hooks::detail;

REX_EXTERN(__real___imp__VdSwap);

namespace {

// Always-on, nearly free: when the game itself (not the host presenter) produces fewer than ~55 frames in a second, or one frame
// takes over 50 ms, a line with the wall-clock time goes to logs/perf.log. The host's own log has timestamps too ("Achievement
// unlocked", ...), so a slowdown can be matched with what the game was doing.
void GuestFrameMonitor() {
  using clock = std::chrono::steady_clock;
  static clock::time_point window_start{}, last{};
  static int frames = 0;
  static double worst_ms = 0.0;
  const auto now = clock::now();
  if (window_start == clock::time_point{}) {
    window_start = last = now;
    return;
  }
  const double iv = std::chrono::duration<double, std::milli>(now - last).count();
  last = now;
  Host().NoteGuestFrame(iv);
  ++frames;
  worst_ms = std::max(worst_ms, iv);
  const double elapsed = std::chrono::duration<double>(now - window_start).count();
  if (elapsed < 1.0) return;
  const double fps = frames / elapsed;
  if (SinceStart() > 20.0 && (fps < 55.0 || worst_ms > 50.0)) {  // not during start-up and logos
    if (FILE* f = fopen("logs/perf.log", "a")) {
      const std::time_t t = std::time(nullptr);
      char stamp[32];
      std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
      fprintf(f, "[%s] game ran at %.1f fps over %.1f s, worst frame %.0f ms\n", stamp, fps, elapsed, worst_ms);
      fclose(f);
    }
  }
  window_start = now;
  frames = 0;
  worst_ms = 0.0;
}

}  // namespace

REX_EXTERN(__wrap___imp__VdSwap) {
  GH2_TRACE_HOOK("VdSwap");
  if (gh2gfx::d3d::ProbeEnabled()) {  // the D3D discovery probe counts frames by the game's swaps (GH2_D3D_PROBE)
    const uint32_t a[6] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
    gh2gfx::d3d::ProbeFrameMark(a, base);
  }
  Heartbeat();
  gh2rt::hooks::detail::NativeThreadsOnFrame(ctx.r13.u32, base);
  GuestFrameMonitor();
  static const bool dump_cvars = getenv("GH2_DUMP_CVARS") != nullptr;
  if (dump_cvars) {
    static std::atomic<bool> done{false};
    if (!done.exchange(true)) Host().DumpCvars();
  }  // also a frame heartbeat (the logos phase polls no input)
  static const bool timeline = getenv("GH2_TIMELINE") != nullptr;
  if (timeline) {
    static std::atomic<uint32_t> swaps{0};
    const uint32_t n = ++swaps;
    if (n == 1 || n == 60 || n == 600) fprintf(stderr, "T+%.1fs VdSwap #%u\n", SinceStart(), n);
  }
  if (gh2rt::hooks::SkipIntroEnabled()) {
    static std::atomic<bool> started{false};
    if (!started.exchange(true)) {
      const char* e = getenv("GH2_SKIP_INTRO_SPEED");
      const double speed = e ? atof(e) : 16.0;
      if (speed > 1.0) gh2rt::hooks::FastForwardBegin(speed, "startup mode");
    }
  }
  {
    static const double max_ff = [] {
      const char* e = getenv("GH2_SKIP_INTRO_FF_MAX");
      return e ? atof(e) : 8.0;  // safety net: never fast-forward longer than this many real seconds
    }();
    if (g_fast_forward.load() && SinceStart() - g_ff_started_s.load() > max_ff) FastForwardEnd("timeout");
  }
  if (gh2rt::hooks::SkipIntroEnabled() && !g_intro_done.load(std::memory_order_relaxed)) {
    // Menu speed = guest frames at ~16.7 ms. Require a run of fast frames after a slow phase was seen.
    using clock = std::chrono::steady_clock;
    static clock::time_point last{};
    static int slow_frames = 0, fast_run = 0;
    const auto now = clock::now();
    if (last.time_since_epoch().count() != 0) {
      const double ms = std::chrono::duration<double, std::milli>(now - last).count();
      const bool video_open = g_video_open_s.load() >= 0;
      if (ms > 24.0) {
        if (video_open) ++slow_frames;  // slow frames of the video itself (the logos before it don't count)
        fast_run = 0;
      } else if (ms < 20.0) {
        ++fast_run;
      }
      if (slow_frames >= 3 && fast_run >= 12) {  // ~0.2 s of menu-speed frames: the menus are up, stop pressing
        g_menu_reached_s = SinceStart();
        g_intro_done = true;
        fprintf(stderr, "T+%.1fs INTRO done: guest reached menu speed\n", SinceStart());
      }
    }
    last = now;
  }
  static const bool on = getenv("GH2_FRAME_TRACE") != nullptr;
  if (on) {
    using clock = std::chrono::steady_clock;
    static clock::time_point last{}, window_start = clock::now();
    static std::vector<double> iv;
    auto now = clock::now();
    if (last.time_since_epoch().count() != 0) {
      iv.push_back(std::chrono::duration<double, std::milli>(now - last).count());
      static const bool timeline_hitch = getenv("GH2_TIMELINE") != nullptr;
      if (timeline_hitch && iv.back() > 60.0) fprintf(stderr, "T+%.2fs HITCH guest frame interval %.0f ms\n", SinceStart(), iv.back());
    }
    last = now;
    static FILE* dump = [] {
      const char* p = getenv("GH2_FRAME_DUMP");
      return p ? fopen(p, "w") : nullptr;
    }();
    if (dump && !iv.empty()) fprintf(dump, "%.4f %.2f\n", SinceStart(), iv.back());
    if (now - window_start >= std::chrono::seconds(2) && !iv.empty()) {
      std::vector<double> sorted = iv;
      std::sort(sorted.begin(), sorted.end());
      double sum = 0;
      int over20 = 0, over25 = 0, over33 = 0;
      for (double v : iv) {
        sum += v;
        over20 += v > 20;
        over25 += v > 25;
        over33 += v > 33.4;
      }
      fprintf(stderr, "SWAP n=%zu mean=%.2f p95=%.2f max=%.1f >20ms=%d >25ms=%d >33ms=%d\n", iv.size(), sum / iv.size(),
              sorted[size_t(sorted.size() * 0.95)], sorted.back(), over20, over25, over33);
      iv.clear();
      window_start = now;
    }
  }
  __real___imp__VdSwap(ctx, base);
}
