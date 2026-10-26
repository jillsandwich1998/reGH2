// Native events, semaphores and waits (Phase 2.7, K.2): the dispatcher-object imports answered by gh2k::sync (objects.h) on guest memory instead of the runtime's XEvent / XSemaphore.
// --objects=sdk (default) forwards every call; --objects=native answers those on events and semaphores here. Threads, mutants and everything else the title waits on stay with the runtime:
// a wait on a thread object (header type 6) or a thread handle is forwarded.
//
// Where objects come from: the title initialises most KEVENT / KSEMAPHORE itself, inline, in guest memory (the header is all the state); NtCreateEvent / NtCreateSemaphore objects are
// made here (a guest-memory object from a small pool + a handle in 0x7D000000..). The runtime never signals objects the guest can reach (checked in its sources: its audio, XMA and
// notification code signal host-private objects only), except the completion event of its own asynchronous file reads, which is bridged below (WatchIoCompletion).
// Needs the native heaps (the pool is allocated from gh2k::mem). No SDK dependency.
#include "gh2_pch.h"  // PPCContext, REX_EXTERN, REX_LOAD_*

#include <gh2k/mem.h>
#include <gh2k/objects.h>
#include <gh2rt/boundary_trace.h>
#include <gh2rt/host_services.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "hooks_internal.h"

namespace sy = gh2k::sync;

namespace {

constexpr uint32_t kHandleBase = 0x7D000000u;

struct State {
  std::mutex mu;
  std::unordered_map<uint32_t, uint32_t> handle_to_object;
  std::vector<std::pair<uint32_t, uint32_t>> pages;   // [begin, end) of the pool pages
  uint32_t page = 0, used = 0, next_handle = 0;
  std::atomic<uint64_t> forwarded_waits{0}, creates{0}, bridged_reads{0}, mixed_waits{0};
  std::mutex watch_mu;
  struct Watch { const volatile uint32_t* iosb; uint32_t event; uint8_t* base; };
  std::vector<Watch> watches;
};
State& g_s = *new State;   // never destroyed: native threads may still be inside a hook when the process exits

bool Native() {
  static const bool native = [] {
    std::string mode = gh2rt::Host().GetSettings().objects;   // default "native" (approved 2026-10-24)
    if (const char* e = getenv("GH2_OBJECTS"); e && *e) mode = e;
    if (mode != "native") return false;
    if (!gh2k::mem::GlobalMemory()) {
      fprintf(stderr, "OBJECTS native needs the native heaps (--heaps=native): staying with the runtime's\n");
      return false;
    }
    fprintf(stderr, "OBJECTS native: events, semaphores and their waits are answered by gh2k::sync\n");
    std::thread([] {
      const char* report = getenv("GH2_OBJECTS_REPORT");
      for (int n = 1;; ++n) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        const sy::ObjectCounters c = sy::ObjectStats();
        uint64_t th[6];
        gh2rt::hooks::detail::ThreadStats(th);
        fprintf(stderr, "THREADS T+%ds native threads created %llu, finished %llu | self-suspends %llu, cooperative suspends %llu, resumes %llu, delays %llu\n", n * 10, (unsigned long long)th[0],
                (unsigned long long)th[1], (unsigned long long)th[2], (unsigned long long)th[3], (unsigned long long)th[4], (unsigned long long)th[5]);
        fprintf(stderr, "OBJECTS T+%ds sets %llu resets %llu releases %llu | waits %llu (slept %llu, timeouts %llu, multi %llu) | handles made %llu, forwarded waits %llu (mixed %llu), bridged reads %llu\n", n * 10,
                (unsigned long long)c.sets, (unsigned long long)c.resets, (unsigned long long)c.releases, (unsigned long long)c.waits, (unsigned long long)c.waits_blocked,
                (unsigned long long)c.wait_timeouts, (unsigned long long)c.multi_waits, (unsigned long long)g_s.creates.load(), (unsigned long long)g_s.forwarded_waits.load(),
                (unsigned long long)g_s.mixed_waits.load(), (unsigned long long)g_s.bridged_reads.load());
        if (report && *report)
          if (FILE* f = fopen(report, "w")) {
            fprintf(f, "{\"seconds\": %d, \"sets\": %llu, \"resets\": %llu, \"semaphore_releases\": %llu, \"waits\": %llu, \"waits_that_slept\": %llu, \"wait_timeouts\": %llu, \"multi_object_waits\": %llu, "
                       "\"native_threads_created\": %llu, \"native_threads_finished\": %llu, \"thread_self_suspends\": %llu, \"thread_resumes\": %llu, \"thread_delays\": %llu, \"handles_created\": %llu, \"waits_forwarded_to_the_runtime\": %llu, \"mixed_native_and_runtime_waits\": %llu, \"runtime_reads_bridged\": %llu}\n",
                    n * 10, (unsigned long long)c.sets, (unsigned long long)c.resets, (unsigned long long)c.releases, (unsigned long long)c.waits, (unsigned long long)c.waits_blocked,
                    (unsigned long long)c.wait_timeouts, (unsigned long long)c.multi_waits, (unsigned long long)th[0], (unsigned long long)th[1], (unsigned long long)th[2], (unsigned long long)th[4],
                    (unsigned long long)th[5], (unsigned long long)g_s.creates.load(), (unsigned long long)g_s.forwarded_waits.load(), (unsigned long long)g_s.mixed_waits.load(),
                    (unsigned long long)g_s.bridged_reads.load());
            fclose(f);
          }
      }
    }).detach();
    return true;
  }();
  return native;
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

// 0x20-byte objects carved from 4 KB pages of the system heap
bool AllocObject(uint8_t* base, uint32_t* address) {
  std::lock_guard<std::mutex> lock(g_s.mu);
  if (g_s.page == 0 || g_s.used + 0x20 > 4096) {
    gh2k::mem::Heap& heap = gh2k::mem::GlobalMemory()->v00000000();
    uint32_t page;
    if (!heap.AllocRange(0x30000000u, 0x3FFFFFFFu, 4096, 4096, false, &page) || !heap.Commit(page, 4096, gh2k::mem::kReadWrite)) return false;
    g_s.pages.push_back({page, page + 4096});
    g_s.page = page;
    g_s.used = 0;
  }
  *address = g_s.page + g_s.used;
  g_s.used += 0x20;
  return true;
}

bool IsPoolObject(uint32_t p) {
  std::lock_guard<std::mutex> lock(g_s.mu);
  for (const auto& [b, e] : g_s.pages)
    if (p >= b && p < e) return true;
  return false;
}

uint32_t NewHandle(uint32_t object) {
  std::lock_guard<std::mutex> lock(g_s.mu);
  const uint32_t h = kHandleBase + 4 * (g_s.next_handle++);
  g_s.handle_to_object[h] = object;
  ++g_s.creates;
  return h;
}

uint32_t ObjectOf(uint32_t handle) {
  if (handle < kHandleBase || handle >= kHandleBase + 0x01000000u) return 0;
  std::lock_guard<std::mutex> lock(g_s.mu);
  auto it = g_s.handle_to_object.find(handle);
  return it == g_s.handle_to_object.end() ? 0 : it->second;
}

}  // namespace

namespace {

// Diagnostic ring (GH2_OBJECTS_RING=<seconds>): the last operations on dispatcher objects, printed once after that many seconds. Off by default (one predictable branch).
struct RingEntry { double t; uint32_t pcr, obj; int32_t state_before, state_after; uint8_t type; char what[12]; uint64_t arg; };
const double g_ring_dump_at = [] { const char* e = getenv("GH2_OBJECTS_RING"); return e && *e ? atof(e) : 0.0; }();
std::mutex g_ring_mu;
std::vector<RingEntry> g_ring;
size_t g_ring_pos = 0;
std::atomic<bool> g_ring_dumped{false};
double NowSeconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
double g_ring_t0 = NowSeconds();
int32_t StateOf(const uint8_t* base, uint32_t o) { uint32_t v; std::memcpy(&v, base + o + 4, 4); return static_cast<int32_t>(__builtin_bswap32(v)); }

void Ring(const uint8_t* base, const PPCContext& ctx, const char* what, uint32_t obj, int32_t before, uint64_t arg = 0) {
  if (g_ring_dump_at <= 0) return;
  RingEntry e{NowSeconds() - g_ring_t0, ctx.r13.u32, obj, before, StateOf(base, obj), base[obj], {}, arg};
  std::snprintf(e.what, sizeof e.what, "%s", what);
  std::lock_guard<std::mutex> lock(g_ring_mu);
  if (g_ring.size() < 6000) g_ring.push_back(e); else { g_ring[g_ring_pos] = e; g_ring_pos = (g_ring_pos + 1) % g_ring.size(); }
  if (!g_ring_dumped && e.t >= g_ring_dump_at && !g_ring_dumped.exchange(true)) {
    fprintf(stderr, "OBJECTS RING (%zu entries, oldest first)\n", g_ring.size());
    for (size_t i = 0; i < g_ring.size(); ++i) {
      const RingEntry& r = g_ring[(g_ring_pos + i) % g_ring.size()];
      fprintf(stderr, "R %8.3f pcr %08X %-10s obj %08X type %u state %d->%d arg %llx\n", r.t, r.pcr, r.what, r.obj, r.type, r.state_before, r.state_after, (unsigned long long)r.arg);
    }
  }
}

}  // namespace

namespace {

// Wait() with a diagnostic for waits that last a long time (GH2_OBJECTS_SLOW=<seconds>, off by default): says what is waiting for what, once.
uint32_t WaitLogged(uint8_t* base, const uint32_t* objects, uint32_t count, bool wait_all, uint64_t timeout, const char* api, const PPCContext& ctx) {
  gh2rt::hooks::detail::ParkIfQuiescing();
  static const double slow = [] { const char* e = getenv("GH2_OBJECTS_SLOW"); return e && *e ? atof(e) : 0.0; }();
  if (g_ring_dump_at > 0) for (uint32_t i = 0; i < count && i < 3; ++i) Ring(base, ctx, api[0] == 'N' ? "NtWait" : "KeWait", objects[i], StateOf(base, objects[i]), timeout);
  if (slow <= 0) {
    const uint32_t r = sy::Wait(base, objects, count, wait_all, timeout);
    if (g_ring_dump_at > 0) Ring(base, ctx, "waitDone", objects[0], StateOf(base, objects[0]), r);
    return r;
  }
  const uint64_t slice = static_cast<uint64_t>(slow * 1e7);
  uint64_t remaining = timeout;
  bool reported = false;
  for (;;) {
    const uint64_t step = (timeout == sy::kInfinite) ? slice : (remaining < slice ? remaining : slice);
    const uint32_t r = sy::Wait(base, objects, count, wait_all, step);
    if (r != sy::kStatusTimeout || timeout == 0) return r;
    if (timeout != sy::kInfinite) {
      if (remaining <= step) return r;
      remaining -= step;
    }
    if (!reported) {
      reported = true;
      fprintf(stderr, "OBJECTS SLOW WAIT %s on thread pcr %08X (lr %08X): %u object(s)", api, ctx.r13.u32, static_cast<uint32_t>(ctx.lr), count);
      for (uint32_t i = 0; i < count && i < 4; ++i) {
        uint32_t st;
        std::memcpy(&st, base + objects[i] + 4, 4);
        fprintf(stderr, " [%08X type %u state %d]", objects[i], base[objects[i]], static_cast<int32_t>(__builtin_bswap32(st)));
      }
      fprintf(stderr, "\n");
    }
  }
}

}  // namespace

namespace gh2rt::hooks::detail {

bool NativeObjectsActive() { return Native(); }

uint32_t ObjectHandleCreate(uint32_t guest_object) { return NewHandle(guest_object); }
uint32_t GuestScratchAlloc(uint8_t* base) {
  uint32_t address = 0;
  if (!AllocObject(base, &address)) return 0;
  std::memset(base + address, 0, 0x20);
  return address;
}
uint32_t ObjectOfHandle(uint32_t handle) { return Native() ? ObjectOf(handle) : 0; }

bool NativeObjectClose(uint32_t handle) {
  if (!Native() || handle < kHandleBase || handle >= kHandleBase + 0x01000000u) return false;
  uint32_t object = 0;
  {
    std::lock_guard<std::mutex> lock(g_s.mu);
    auto it = g_s.handle_to_object.find(handle);
    if (it != g_s.handle_to_object.end()) {
      object = it->second;
      g_s.handle_to_object.erase(it);   // the guest object stays: a pointer taken with ObReferenceObjectByHandle may outlive the handle
    }
  }
  if (object && sy::IsRegisteredThread(object)) ThreadHandleClosed(object);   // a native thread: the handle's reference goes
  return true;
}

bool NativeEventSetByHandle(uint8_t* base, uint32_t handle) {
  if (!Native()) return false;
  const uint32_t object = ObjectOf(handle);
  if (!object) return false;
  sy::EventSet(base, object);
  return true;
}

bool NativeEventHandle(uint32_t handle) { return Native() && ObjectOf(handle) != 0; }

// The runtime completes its own file reads asynchronously and signals the event handle it was given; it does not know ours. The read is issued without the event and a watcher signals
// ours when the io status block leaves STATUS_PENDING.
void WatchIoCompletion(uint8_t* base, uint32_t iosb, uint32_t event_handle) {
  ++g_s.bridged_reads;
  const uint32_t object = ObjectOf(event_handle);
  if (!object || !iosb) return;
  static std::once_flag once;
  std::call_once(once, [] {
    std::thread([] {
      for (;;) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        std::vector<State::Watch> work;
        {
          std::lock_guard<std::mutex> lock(g_s.watch_mu);
          work.swap(g_s.watches);
        }
        std::vector<State::Watch> still;
        for (auto& w : work) {
          if (__builtin_bswap32(*w.iosb) == 0x103) still.push_back(w);
          else sy::EventSet(w.base, w.event);
        }
        if (!still.empty()) {
          std::lock_guard<std::mutex> lock(g_s.watch_mu);
          for (auto& w : still) g_s.watches.push_back(w);
        }
      }
    }).detach();
  });
  std::lock_guard<std::mutex> lock(g_s.watch_mu);
  g_s.watches.push_back({reinterpret_cast<const volatile uint32_t*>(REX_RAW_ADDR(iosb)), object, base});
}

}  // namespace gh2rt::hooks::detail

#define OBJ_REAL(NAME) REX_EXTERN(__real___imp__##NAME)
OBJ_REAL(NtCreateEvent);
OBJ_REAL(NtCreateSemaphore);
OBJ_REAL(NtSetEvent);
OBJ_REAL(NtClearEvent);
OBJ_REAL(NtReleaseSemaphore);
OBJ_REAL(NtWaitForSingleObjectEx);
OBJ_REAL(KeSetEvent);
OBJ_REAL(KeResetEvent);
OBJ_REAL(KeReleaseSemaphore);
OBJ_REAL(KeInitializeSemaphore);
OBJ_REAL(KeWaitForSingleObject);
OBJ_REAL(KeWaitForMultipleObjects);
OBJ_REAL(ObReferenceObjectByHandle);
OBJ_REAL(ObDereferenceObject);

// NtCreateEvent(handle*, object_attributes, event_type (0 notification, 1 synchronization), initial_state)
REX_EXTERN(__wrap___imp__NtCreateEvent) {
  GH2_TRACE_HOOK("NtCreateEvent");
  if (!Native()) { __real___imp__NtCreateEvent(ctx, base); return; }
  uint32_t object;
  if (!AllocObject(base, &object)) { ctx.r3.u64 = 0xC0000017u; return; }   // STATUS_NO_MEMORY
  sy::InitEvent(base, object, ctx.r5.u32 ? sy::kTypeSynchronizationEvent : sy::kTypeNotificationEvent, ctx.r6.u32 != 0);
  if (ctx.r3.u32) PutBe32(base, ctx.r3.u32, NewHandle(object));
  ctx.r3.u64 = 0;
}
// NtCreateSemaphore(handle*, object_attributes, count, limit)
REX_EXTERN(__wrap___imp__NtCreateSemaphore) {
  GH2_TRACE_HOOK("NtCreateSemaphore");
  if (!Native()) { __real___imp__NtCreateSemaphore(ctx, base); return; }
  uint32_t object;
  if (!AllocObject(base, &object)) { ctx.r3.u64 = 0xC0000017u; return; }
  sy::InitSemaphore(base, object, static_cast<int32_t>(ctx.r5.u32), static_cast<int32_t>(ctx.r6.u32));
  if (ctx.r3.u32) PutBe32(base, ctx.r3.u32, NewHandle(object));
  ctx.r3.u64 = 0;
}
// NtSetEvent(handle, previous_state*)
REX_EXTERN(__wrap___imp__NtSetEvent) {
  GH2_TRACE_HOOK("NtSetEvent");
  const uint32_t object = Native() ? ObjectOf(ctx.r3.u32) : 0;
  if (!object) { __real___imp__NtSetEvent(ctx, base); return; }
  const int32_t previous = sy::EventSet(base, object);
  Ring(base, ctx, "NtSetEvent", object, previous);
  if (ctx.r4.u32) PutBe32(base, ctx.r4.u32, static_cast<uint32_t>(previous));
  ctx.r3.u64 = 0;
}
REX_EXTERN(__wrap___imp__NtClearEvent) {
  GH2_TRACE_HOOK("NtClearEvent");
  const uint32_t object = Native() ? ObjectOf(ctx.r3.u32) : 0;
  if (!object) { __real___imp__NtClearEvent(ctx, base); return; }
  { const int32_t before = StateOf(base, object); sy::EventReset(base, object); Ring(base, ctx, "NtClearEvt", object, before); }
  ctx.r3.u64 = 0;
}
// NtReleaseSemaphore(handle, release_count, previous_count*)
REX_EXTERN(__wrap___imp__NtReleaseSemaphore) {
  GH2_TRACE_HOOK("NtReleaseSemaphore");
  const uint32_t object = Native() ? ObjectOf(ctx.r3.u32) : 0;
  if (!object) { __real___imp__NtReleaseSemaphore(ctx, base); return; }
  int32_t previous = 0;
  const uint32_t status = sy::SemaphoreRelease(base, object, static_cast<int32_t>(ctx.r4.u32), &previous);
  Ring(base, ctx, "NtRelSem", object, previous, ctx.r4.u32);
  if (ctx.r5.u32) PutBe32(base, ctx.r5.u32, static_cast<uint32_t>(previous));
  ctx.r3.u64 = status;
}
// NtWaitForSingleObjectEx(handle, wait_mode, alertable, timeout*)
REX_EXTERN(__wrap___imp__NtWaitForSingleObjectEx) {
  GH2_TRACE_HOOK("NtWaitForSingleObjectEx");
  const uint32_t object = Native() ? ObjectOf(ctx.r3.u32) : 0;
  if (!object) {
    if (Native()) ++g_s.forwarded_waits;
    __real___imp__NtWaitForSingleObjectEx(ctx, base);
    return;
  }
  ctx.r3.u64 = WaitLogged(base, &object, 1, false, sy::TimeoutFromGuest(base, ctx.r6.u32), "NtWaitForSingleObjectEx", ctx);
}
// KeSetEvent(event*, increment, wait) -> previous state
REX_EXTERN(__wrap___imp__KeSetEvent) {
  GH2_TRACE_HOOK("KeSetEvent");
  if (!Native() || !sy::IsNative(base, ctx.r3.u32)) { __real___imp__KeSetEvent(ctx, base); return; }
  { const int32_t before = StateOf(base, ctx.r3.u32); const uint32_t o = ctx.r3.u32; ctx.r3.u64 = static_cast<uint32_t>(sy::EventSet(base, o)); Ring(base, ctx, "KeSetEvent", o, before); }
}
REX_EXTERN(__wrap___imp__KeResetEvent) {
  GH2_TRACE_HOOK("KeResetEvent");
  if (!Native() || !sy::IsNative(base, ctx.r3.u32)) { __real___imp__KeResetEvent(ctx, base); return; }
  { const int32_t before = StateOf(base, ctx.r3.u32); const uint32_t o = ctx.r3.u32; ctx.r3.u64 = static_cast<uint32_t>(sy::EventReset(base, o)); Ring(base, ctx, "KeResetEvt", o, before); }
}
// KeReleaseSemaphore(semaphore*, increment, adjustment, wait) -> previous count
REX_EXTERN(__wrap___imp__KeReleaseSemaphore) {
  GH2_TRACE_HOOK("KeReleaseSemaphore");
  if (!Native() || !sy::IsNative(base, ctx.r3.u32)) { __real___imp__KeReleaseSemaphore(ctx, base); return; }
  int32_t previous = 0;
  sy::SemaphoreRelease(base, ctx.r3.u32, static_cast<int32_t>(ctx.r5.u32), &previous);
  Ring(base, ctx, "KeRelSem", ctx.r3.u32, previous, ctx.r5.u32);
  ctx.r3.u64 = static_cast<uint32_t>(previous);
}
// KeInitializeSemaphore(semaphore*, count, limit)
REX_EXTERN(__wrap___imp__KeInitializeSemaphore) {
  GH2_TRACE_HOOK("KeInitializeSemaphore");
  if (!Native()) { __real___imp__KeInitializeSemaphore(ctx, base); return; }
  sy::InitSemaphore(base, ctx.r3.u32, static_cast<int32_t>(ctx.r4.u32), static_cast<int32_t>(ctx.r5.u32));
}
// KeWaitForSingleObject(object*, wait_reason, processor_mode, alertable, timeout*)
REX_EXTERN(__wrap___imp__KeWaitForSingleObject) {
  GH2_TRACE_HOOK("KeWaitForSingleObject");
  const uint32_t object = ctx.r3.u32;
  if (!Native() || !object || !sy::IsNative(base, object)) {
    if (Native()) ++g_s.forwarded_waits;
    __real___imp__KeWaitForSingleObject(ctx, base);
    return;
  }
  ctx.r3.u64 = WaitLogged(base, &object, 1, false, sy::TimeoutFromGuest(base, ctx.r7.u32), "KeWaitForSingleObject", ctx);
}
// KeWaitForMultipleObjects(count, objects**, wait_type (0 all, 1 any), wait_reason, wait_mode, alertable, timeout*, wait_blocks*)
REX_EXTERN(__wrap___imp__KeWaitForMultipleObjects) {
  GH2_TRACE_HOOK("KeWaitForMultipleObjects");
  const uint32_t count = ctx.r3.u32, list = ctx.r4.u32;
  bool all_native = Native() && count >= 1 && count <= 64 && list;
  uint32_t objects[64];
  for (uint32_t i = 0; all_native && i < count; ++i) {
    objects[i] = Be32(base, list + 4 * i);
    all_native = objects[i] && sy::IsNative(base, objects[i]);
  }
  if (!all_native) {
    if (Native()) {
      ++g_s.forwarded_waits;
      bool any_native = false;
      for (uint32_t i = 0; count <= 64 && list && i < count; ++i) any_native |= sy::IsNative(base, Be32(base, list + 4 * i));
      if (any_native && g_s.mixed_waits++ == 0)   // the runtime would never see our signals: say so (once)
        fprintf(stderr, "OBJECTS WARNING: KeWaitForMultipleObjects mixes native objects with others (count %u); forwarded to the runtime\n", count);
    }
    __real___imp__KeWaitForMultipleObjects(ctx, base);
    return;
  }
  ctx.r3.u64 = WaitLogged(base, objects, count, ctx.r5.u32 == 0, sy::TimeoutFromGuest(base, ctx.r9.u32), "KeWaitForMultipleObjects", ctx);
}
// ObReferenceObjectByHandle(handle, object_type, object**)
REX_EXTERN(__wrap___imp__ObReferenceObjectByHandle) {
  GH2_TRACE_HOOK("ObReferenceObjectByHandle");
  uint32_t object = Native() ? ObjectOf(ctx.r3.u32) : 0;
  if (!object && ctx.r3.u32 == 0xFFFFFFFEu && Native()) {   // the "current thread" pseudo handle: ours if the caller is a native thread
    const uint32_t current = Be32(base, ctx.r13.u32 + 0x100);
    if (sy::IsRegisteredThread(current)) object = current;
  }
  if (!object) { __real___imp__ObReferenceObjectByHandle(ctx, base); return; }
  if (ctx.r5.u32) PutBe32(base, ctx.r5.u32, object);
  ctx.r3.u64 = 0;
}
REX_EXTERN(__wrap___imp__ObDereferenceObject) {
  GH2_TRACE_HOOK("ObDereferenceObject");
  if (Native() && IsPoolObject(ctx.r3.u32)) return;   // our objects are not reference counted: they live for the process
  __real___imp__ObDereferenceObject(ctx, base);
}
