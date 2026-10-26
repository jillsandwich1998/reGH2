// Other hooks (moved from app/src/gh2_host.cpp): the spin-wait step, the SMT-yield hint, content (DLC) tracking and the
// in-song detection for the keyboard. No SDK dependency.
#include "gh2_pch.h"  // REX_LOAD_* (generated), PPCContext, REX_EXTERN

#include <gh2rt/boundary_trace.h>
#include <gh2rt/host_services.h>
#include <gh2rt/hooks_api.h>

#include <immintrin.h>
#include <sched.h>
#include <time.h>

#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>

using gh2rt::Host;

// ---- Spin-wait hook ----------------------------------------------------------------------------------------------------------
// sub_823C6B50 is the game's wait-with-timeout step (8x db16cyc delay, then a flag/owner check; returns 1 = keep waiting).
// db16cyc is a nop in the recompiled code, so the main thread polls at full speed and keeps one core at ~100% (78% of its
// samples, GH2_PROFILE). Real PAUSE hints give the core's SMT sibling and the package power budget back without adding wake-up
// latency (no sleeping). GH2_SPIN_PAUSE=0 disables the hook's pause.
REX_EXTERN(__imp__sub_823C6B50);

REX_EXTERN(sub_823C6B50) {
  static const bool pause_on = [] {
    const char* e = getenv("GH2_SPIN_PAUSE");
    return !(e && e[0] == '0');
  }();
  __imp__sub_823C6B50(ctx, base);
  if (pause_on && ctx.r3.u32 == 1) {
    for (int i = 0; i < 16; ++i) _mm_pause();
  }
}

// ---- SMT-yield hint of the guest (`db16cyc`) -----------------------------------------------------------------------------------
// See native/tools/postcodegen.sh. A real PAUSE keeps the sibling hardware thread / package power in check; a yield every 64 hints
// lets the thread the spinner is waiting for run on CPUs with few cores (a 2-core run used to hang in the audio threads' barrier,
// sub_826B10B0). GH2_SPIN_YIELD=0 disables the yield.
extern "C" void gh2_spin_hint() {
  static const bool yield_on = [] {
    const char* e = getenv("GH2_SPIN_YIELD");
    return !(e && e[0] == '0');
  }();
  // A guest thread that has been spinning for a while is really waiting (barrier, lock, timeout loop): after
  // GH2_SPIN_SLEEP_AFTER hints (default 2000, ~50 us) it sleeps GH2_SPIN_SLEEP_US microseconds per hint (default 30; 0 disables)
  // instead of burning a core. The spin counter restarts when the gap between two hints shows the thread did other work in between.
  static const unsigned sleep_after = [] {
    const char* e = getenv("GH2_SPIN_SLEEP_AFTER");
    return e ? unsigned(atoi(e)) : 2000u;
  }();
  static const long sleep_ns = [] {
    const char* e = getenv("GH2_SPIN_SLEEP_US");
    return (e ? atol(e) : 30L) * 1000L;
  }();
  thread_local unsigned n = 0, spins = 0;
  thread_local uint64_t last = 0;
  const uint64_t now = __rdtsc();
  if (now - last > 30000) spins = 0;  // > ~10 us since the previous hint: a new wait
  last = now;
  _mm_pause();
  if (sleep_ns > 0 && ++spins > sleep_after) {
    timespec ts{0, sleep_ns};
    nanosleep(&ts, nullptr);
    last = __rdtsc();
    return;
  }
  if (yield_on && (++n & 63u) == 0) sched_yield();
}

// ---- Content (DLC) usage tracking for the options menu -------------------------------------------------------------------------
// XamContentCreateEnumerator = the game lists packages; XamContentCreateEx(root, XCONTENT_DATA*) = it opens one. The menu shows a
// pack as LOADED once the game has opened it in this session.
REX_EXTERN(__real___imp__XamContentCreateEnumerator);
REX_EXTERN(__real___imp__XamContentCreateEx);

namespace {
std::mutex g_content_mutex;
std::set<std::string> g_content_opened;
std::atomic<uint32_t> g_content_enumerations{0};
}  // namespace

bool Gh2ContentOpened(const std::string& file_name) {
  std::lock_guard<std::mutex> lock(g_content_mutex);
  return g_content_opened.count(file_name) != 0;
}
uint32_t Gh2ContentEnumerations() { return g_content_enumerations.load(); }

REX_EXTERN(__wrap___imp__XamContentCreateEnumerator) {
  GH2_TRACE_HOOK("XamContentCreateEnumerator");
  ++g_content_enumerations;
  __real___imp__XamContentCreateEnumerator(ctx, base);
}

REX_EXTERN(__wrap___imp__XamContentCreateEx) {
  GH2_TRACE_HOOK("XamContentCreateEx");
  // r5 = XCONTENT_DATA { u32 device_id, u32 content_type, u16 display_name[128], char file_name[42] }
  const uint32_t data = ctx.r5.u32;
  if (data) {
    char name[43] = {};
    for (int i = 0; i < 42; ++i) name[i] = static_cast<char>(REX_LOAD_U8(data + 8 + 256 + i));
    const uint32_t type = REX_LOAD_U32(data + 4);
    if (type == 2) {
      std::lock_guard<std::mutex> lock(g_content_mutex);
      if (g_content_opened.insert(name).second) fprintf(stderr, "CONTENT opened by the game: %s\n", name);
    }
  }
  __real___imp__XamContentCreateEx(ctx, base);
}

// ---- In-song detection for the keyboard's frets-only style -----------------------------------------------------------------------
// sub_821D75F8 = RndCam::UpdateProjection(this), run every frame for the cameras in use. The note highway camera is the
// perspective camera with a tight frustum (far plane < 1000) and a 45 degree FOV (field offsets: far +696, y_fov +700). Seeing it
// means a song is on screen. The hook changes nothing.
REX_EXTERN(__imp__sub_821D75F8);
REX_EXTERN(sub_821D75F8) {
  const uint32_t cam = ctx.r3.u32;
  if (cam) {
    const float far_plane = std::bit_cast<float>(REX_LOAD_U32(cam + 696));
    const float fov = std::bit_cast<float>(REX_LOAD_U32(cam + 700));
    if (far_plane < 1000.0f && std::fabs(fov - 0.7854f) < 0.002f) Host().KeyboardNoteInSong();
  }
  __imp__sub_821D75F8(ctx, base);
}
