// Native kernel synchronisation (Phase 2.7, K.2): the IRQL / spin lock group, the critical sections and the TLS slot accessors of the kernel, answered by
// gh2k::sync on the guest structures instead of by the runtime. --sync=sdk (default) forwards every call; --sync=native answers them here.
// Creating and scheduling the guest threads, events, semaphores and waits are still the runtime's. No SDK dependency.
#include "gh2_pch.h"  // PPCContext, REX_EXTERN, REX_RAW_ADDR

#include <gh2k/sync.h>
#include <gh2rt/boundary_trace.h>
#include <gh2rt/host_services.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace {

bool Native() {
  static const bool native = [] {
    std::string mode = gh2rt::Host().GetSettings().sync;
    if (const char* e = getenv("GH2_SYNC"); e && *e) mode = e;
    if (mode != "native") return false;
    fprintf(stderr, "SYNC native: IRQL, spin locks, critical sections and TLS slots are answered by gh2k::sync\n");
    std::thread([] {
      const auto t0 = std::chrono::steady_clock::now();
      for (int next = 10;; next += 10) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (t < next) {
          next -= 10;
          continue;
        }
        const gh2k::sync::Counters c = gh2k::sync::Stats();
        fprintf(stderr, "SYNC T+%.0fs critical sections that had to sleep %llu, spin lock retries %llu\n", t, (unsigned long long)c.contended_enters, (unsigned long long)c.spin_waits);
        if (const char* r = getenv("GH2_SYNC_REPORT"); r && *r)
          if (FILE* f = fopen(r, "w")) {
            fprintf(f, "{\"seconds\": %.0f, \"critical_sections_that_slept\": %llu, \"spin_lock_retries\": %llu}\n", t, (unsigned long long)c.contended_enters, (unsigned long long)c.spin_waits);
            fclose(f);
          }
      }
    }).detach();
    return true;
  }();
  return native;
}

}  // namespace

#define GH2_SYNC_HOOK(NAME)                \
  REX_EXTERN(__real___imp__##NAME);        \
  REX_EXTERN(__wrap___imp__##NAME) {       \
    GH2_TRACE_HOOK(#NAME);                 \
    if (!Native()) {                       \
      __real___imp__##NAME(ctx, base);     \
      return;                              \
    }

#define GH2_SYNC_END }

namespace sy = gh2k::sync;

GH2_SYNC_HOOK(KeRaiseIrqlToDpcLevel)
  ctx.r3.u64 = sy::RaiseIrql(base, ctx.r13.u32, sy::kIrqlDispatch);
GH2_SYNC_END

GH2_SYNC_HOOK(KfLowerIrql)
  sy::LowerIrql(base, ctx.r13.u32, ctx.r3.u32 & 0xFF);
GH2_SYNC_END

GH2_SYNC_HOOK(KfAcquireSpinLock)
  ctx.r3.u64 = sy::SpinAcquireRaising(base, ctx.r13.u32, ctx.r3.u32);
GH2_SYNC_END

GH2_SYNC_HOOK(KfReleaseSpinLock)
  sy::SpinReleaseLowering(base, ctx.r13.u32, ctx.r3.u32, ctx.r4.u32);
GH2_SYNC_END

GH2_SYNC_HOOK(KeAcquireSpinLockAtRaisedIrql)
  sy::SpinAcquire(base, ctx.r13.u32, ctx.r3.u32);
GH2_SYNC_END

GH2_SYNC_HOOK(KeReleaseSpinLockFromRaisedIrql)
  sy::SpinRelease(base, ctx.r3.u32);
GH2_SYNC_END

GH2_SYNC_HOOK(KeTryToAcquireSpinLockAtRaisedIrql)
  ctx.r3.u64 = sy::SpinTryAcquire(base, ctx.r13.u32, ctx.r3.u32) ? 1 : 0;
GH2_SYNC_END

GH2_SYNC_HOOK(RtlInitializeCriticalSection)
  sy::CritInit(base, ctx.r3.u32, 0);
GH2_SYNC_END

GH2_SYNC_HOOK(RtlEnterCriticalSection)
  sy::CritEnter(base, ctx.r13.u32, ctx.r3.u32);
GH2_SYNC_END

GH2_SYNC_HOOK(RtlLeaveCriticalSection)
  sy::CritLeave(base, ctx.r13.u32, ctx.r3.u32);
GH2_SYNC_END

GH2_SYNC_HOOK(RtlTryEnterCriticalSection)
  ctx.r3.u64 = sy::CritTryEnter(base, ctx.r13.u32, ctx.r3.u32) ? 1 : 0;
GH2_SYNC_END

GH2_SYNC_HOOK(KeTlsGetValue)
  ctx.r3.u64 = sy::TlsGet(base, ctx.r13.u32, ctx.r3.u32);
GH2_SYNC_END

GH2_SYNC_HOOK(KeTlsSetValue)
  sy::TlsSet(base, ctx.r13.u32, ctx.r3.u32, ctx.r4.u32);
  ctx.r3.u64 = 1;
GH2_SYNC_END
