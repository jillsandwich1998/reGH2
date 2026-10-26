// The game's own threads on native threads (Phase 2.7, K.2): ExCreateThread builds the PCR, KTHREAD, TLS block and stack with gh2k::thread out of gh2k::mem and starts a host thread of
// our own that runs the guest function with a PPCContext of its own, exactly as the runtime's XThread::Execute does; the thread object (KTHREAD, header type 6) is a dispatcher object that
// becomes signalled when the thread ends, so a wait on it is answered by gh2k::sync (objects.h). --threads=native (default) / sdk (the runtime creates them).
//
// What is replaced, with the semantics read from the runtime's sources: ExCreateThread (flags: bit 0 suspended, bit 1 system process -> same process here, 0x80 hand out the KTHREAD
// instead of a handle, bits 24..31 processor mask), ExTerminateThread, NtSuspendThread / NtResumeThread / KeResumeThread (a suspend count mirrored in KTHREAD+0xBC), KeSetAffinityThread,
// KeSetBasePriorityThread, KeDelayExecutionThread and KeGetCurrentProcessType (the last two ask the runtime for "the current thread", which a native thread does not have). Anything that
// belongs to a thread of the runtime (the main thread, its audio and GPU workers) is forwarded.
//
// What is deliberately NOT done (the game does not need it; measured on a 60 s song: 0 APCs among 874 reads, no timers, no mutants): APC delivery, alertable waits that can be interrupted,
// host affinity / priority (the runtime pins each thread to a core; here the OS schedules), linking the thread into the process thread list. Suspending a thread other than the caller is
// cooperative: it takes effect at the target's next wait or delay (the game only suspends itself: 3 NtSuspendThread per song, all on the caller's own handle).
// Needs the native heaps and objects. No SDK dependency.
#include "gh2_pch.h"  // PPCContext, REX_EXTERN

#include <gh2abi/abi.h>
#include <gh2k/mem.h>
#include <gh2k/objects.h>
#include <gh2k/sync.h>
#include <gh2k/thread.h>
#include <gh2rt/boundary_trace.h>
#include <gh2rt/host_services.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "hooks_internal.h"

namespace sy = gh2k::sync;
namespace kt = gh2k::thread;

#define KT_REAL(NAME) REX_EXTERN(__real___imp__##NAME)
KT_REAL(ExCreateThread);
KT_REAL(ExTerminateThread);
KT_REAL(NtSuspendThread);
KT_REAL(NtResumeThread);
KT_REAL(KeResumeThread);
KT_REAL(KeSetAffinityThread);
KT_REAL(KeSetBasePriorityThread);
KT_REAL(KeDelayExecutionThread);
KT_REAL(KeGetCurrentProcessType);
KT_REAL(KeTlsAlloc);
KT_REAL(KeTlsFree);
KT_REAL(KeEnterCriticalRegion);
KT_REAL(KeLeaveCriticalRegion);
KT_REAL(XexGetModuleHandle);
REX_EXTERN(__imp__xstart);   // the game's entry point (0x823B51E0), the recompiled original
KT_REAL(RtlImageXexHeaderField);

namespace {

constexpr uint32_t kStatusNoMemory = 0xC0000017u, kStatusInvalidParameter = 0xC000000Du, kStatusInvalidHandle = 0xC0000008u, kStatusThreadIsTerminating = 0xC000004Bu;
constexpr uint32_t kKthreadTerminated = 0xBE, kKthreadExitStatus = 0x140, kKthreadPriority = 0x70;
constexpr uint32_t kPcrProcessTypeInDpc = 0x0C, kPcrDpcActive = 0x150;
constexpr uint32_t kXexTlsInfo = 0x00020104, kXexDefaultStackSize = 0x00020200;
constexpr uint32_t kFirstThreadId = 0x1000;   // the runtime numbers its own threads from 1: a distinct range keeps the ids of the two apart

struct NThread {
  uint8_t* base = nullptr;
  uint32_t kthread = 0, pcr = 0, handle = 0, thread_id = 0;
  uint32_t xapi = 0, start = 0, context = 0, flags = 0;
  kt::Block blk;
  std::mutex mu;
  std::condition_variable cv;   // the suspend gate
  int refs = 0;                 // 1 while the host thread runs + 1 for the handle (if any)
  int host_priority = 0;
  pthread_t host{};
  bool alive = true;   // the host thread has not finished (guarded by State::mu)
  std::function<void(PPCContext&, uint8_t*)> body;   // set: run this instead of a guest entry point
  std::function<void()> on_finish;                    // runs once when the thread is over, however it ends
  std::atomic<bool> finished{false};
  std::string name;
};

struct State {
  std::mutex mu;
  std::unordered_map<uint32_t, NThread*> by_kthread;
  std::atomic<uint32_t> next_id{kFirstThreadId};
  std::atomic<uint32_t> next_cpu{0};
  std::atomic<uint64_t> created{0}, finished{0}, self_suspends{0}, cooperative_suspends{0}, resumes{0}, delays{0}, foreign_calls{0};
  std::atomic<bool> warned_cooperative{false};
  // from the XEX header, read once
  std::once_flag header_once;
  uint32_t default_stack = 0, tls_slots = 1024, tls_data_size = 0, tls_raw_address = 0, tls_raw_size = 0;
  std::atomic<bool> quiesced{false};
};
State& g_s = *new State;   // never destroyed: native threads may still be running when the process exits

bool Active() {
  static const bool on = [] {
    std::string mode = gh2rt::Host().GetSettings().threads;
    if (const char* e = getenv("GH2_THREADS"); e && *e) mode = e;
    if (mode != "native") return false;
    if (!gh2k::mem::GlobalMemory() || !gh2rt::hooks::detail::NativeObjectsActive()) {
      fprintf(stderr, "THREADS native needs the native heaps and --objects=native: staying with the runtime's\n");
      return false;
    }
    fprintf(stderr, "THREADS native: the game's threads are created, scheduled and waited for by gh2k (PCR/KTHREAD/TLS/stack from gh2k::thread)\n");
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

NThread* Find(uint32_t kthread) {
  std::lock_guard<std::mutex> lock(g_s.mu);
  auto it = g_s.by_kthread.find(kthread);
  return it == g_s.by_kthread.end() ? nullptr : it->second;
}
uint32_t CurrentKthread(const uint8_t* base, const PPCContext& ctx) { return Be32(base, ctx.r13.u32 + kt::pcr::kPrcb + kt::pcr::kCurrentThread); }
thread_local NThread* tl_self = nullptr;   // set by ThreadMain: the native thread this host thread is (null on every other thread)
NThread* Current(const uint8_t*, const PPCContext&) { return tl_self; }

// the runtime's GetFakeCpuNumber: a one-hot processor mask -> core number; no mask -> round robin over 6
uint8_t FakeCpu(uint8_t mask) {
  if (!mask) return static_cast<uint8_t>((g_s.next_cpu.fetch_add(1) + 1) % 6);
  uint8_t cpu = 7;
  while (cpu > 0 && !(mask & (1u << cpu))) --cpu;
  return cpu < 6 ? cpu : 5;
}

// the default stack size and the TLS description of the title come from its XEX header (what the runtime's UserModule / XThread::Create read)
void ReadHeader(PPCContext& ctx, uint8_t* base) {
  std::call_once(g_s.header_once, [&] {
    PPCContext tmp = ctx;   // the real imports take their arguments from, and return in, a context: use a copy so the caller's registers are untouched
    const uint32_t scratch = (ctx.r1.u32 - 0x80) & ~0xFu;   // below the caller's stack pointer: free to use inside an import
    tmp.r3.u64 = 0;
    tmp.r4.u64 = scratch;
    __real___imp__XexGetModuleHandle(tmp, base);
    const uint32_t module = Be32(base, scratch);
    const uint32_t xex_header = module ? Be32(base, module + 0x58) : 0;   // HMODULE is an LDR_DATA_TABLE_ENTRY; +0x58 is the XEX header copy in guest memory
    if (tmp.r3.u32 != 0 || !module || !xex_header) {
      fprintf(stderr, "THREADS: no module handle (status %#x): default stack and TLS description unknown\n", tmp.r3.u32);
      return;
    }
    auto field = [&](uint32_t key) {
      tmp.r3.u64 = xex_header;
      tmp.r4.u64 = key;
      __real___imp__RtlImageXexHeaderField(tmp, base);
      return tmp.r3.u32;
    };
    g_s.default_stack = field(kXexDefaultStackSize);
    if (const uint32_t tls = field(kXexTlsInfo)) {
      g_s.tls_slots = Be32(base, tls + 0x0);
      g_s.tls_raw_address = Be32(base, tls + 0x4);
      g_s.tls_data_size = Be32(base, tls + 0x8);
      g_s.tls_raw_size = Be32(base, tls + 0xC);
    }
    fprintf(stderr, "THREADS: XEX header: default stack %#x, TLS slots %u, static data %#x (template %#x bytes at %08X)\n", g_s.default_stack, g_s.tls_slots, g_s.tls_data_size,
            g_s.tls_raw_size, g_s.tls_raw_address);
  });
}

void Unref(NThread* t) {
  bool free_it = false;
  {
    std::lock_guard<std::mutex> lock(g_s.mu);
    if (--t->refs == 0) {
      g_s.by_kthread.erase(t->kthread);
      free_it = true;
    }
  }
  if (!free_it) return;
  sy::ThreadUnregister(t->kthread);
  kt::Destroy(*gh2k::mem::GlobalMemory(), t->blk);
  delete t;
}

// The thread is over: record the exit status, wake whoever waits for it, give the stack back (as the runtime does) and drop the running reference.
void Finish(NThread* t, uint32_t exit_code) {
  if (t->finished.exchange(true)) return;   // a thread that left through pthread_exit may reach this twice (ExTerminateThread, then the cleanup handler)
  uint8_t* base = t->base;
  {
    std::lock_guard<std::mutex> lock(g_s.mu);
    t->alive = false;
  }
  base[t->kthread + kKthreadTerminated] = 1;
  PutBe32(base, t->kthread + kKthreadExitStatus, exit_code);
  sy::ThreadExited(base, t->kthread);
  if (getenv("GH2_THREADS_TRACE")) fprintf(stderr, "THREADS -%u exit %u\n", t->thread_id, exit_code);
  kt::DestroyStack(*gh2k::mem::GlobalMemory(), &t->blk);
  ++g_s.finished;
  if (t->on_finish) t->on_finish();
  Unref(t);
}

// the suspend gate: blocks while the suspend count (KTHREAD+0xBC) is not zero
void Gate(NThread* t) {
  {
    std::unique_lock<std::mutex> lock(t->mu);
    t->cv.wait(lock, [&] { return t->base[t->kthread + kt::kthread::kSuspendCount] == 0 || g_s.quiesced.load(); });
  }
  gh2rt::hooks::detail::ParkIfQuiescing();
}

void CleanupThunk(void* arg) {
  Finish(static_cast<NThread*>(arg), 0);
  gh2abi::ReleaseCurrentHostThread();
}   // a thread ended by pthread_exit (the runtime's own Terminate path, reached through the borrowed identity)

void RunThread(NThread* t) {
  uint8_t* base = t->base;
  tl_self = t;
  // imports still forwarded to the runtime must find a current thread and a current context: the runtime hands the context this thread runs guest code with
  alignas(64) PPCContext own_context{};
  PPCContext* adopted = gh2abi::AdoptCurrentHostThread(t->thread_id, t->blk.stack.top, t->pcr);
  PPCContext& ctx = adopted ? *adopted : own_context;
  ctx.fpscr.InitHost();
  ctx.r1.u64 = (t->blk.stack.top - 0x100) & ~0xFull;
  ctx.r13.u64 = t->pcr;
  const uint64_t stash = reinterpret_cast<uint64_t>(&ctx);
  std::memcpy(base + t->pcr + kt::pcr::kHostStash, &stash, 8);   // the host's view of this thread's context (the runtime keeps the same there)
  if (!t->name.empty()) pthread_setname_np(pthread_self(), t->name.substr(0, 15).c_str());
  Gate(t);
  if (t->body) {   // a host service on a guest thread: no entry point, no exit code
    t->body(ctx, base);
    Finish(t, 0);
    return;
  }
  uint32_t address;
  bool want_exit_code;
  if (t->xapi) {   // the XDK's start-up trampoline runs the function and ends the thread itself
    address = t->xapi;
    ctx.r3.u64 = t->start;
    ctx.r4.u64 = t->context;
    want_exit_code = false;
  } else {
    address = t->start;
    ctx.r3.u64 = t->context;
    want_exit_code = true;
  }
  PPCFunc* fn = gh2abi::ResolveIndirectFunction(address);
  if (!fn) {
    fprintf(stderr, "THREADS: no function at %08X for thread %#x\n", address, t->thread_id);
    Finish(t, 0);
    return;
  }
  fn(ctx, base);
  Finish(t, want_exit_code ? ctx.r3.u32 : 0);
}

void* ThreadMain(void* arg) {
  NThread* t = static_cast<NThread*>(arg);
  pthread_cleanup_push(CleanupThunk, t);   // (push and pop are one lexical pair: all the work is in RunThread)
  RunThread(t);
  pthread_cleanup_pop(1);   // (runs the handler too: Finish is a no-op by now, the runtime binding is released)
  return nullptr;
}

}  // namespace

namespace gh2rt::hooks::detail {

bool NativeThreadsActive() { return Active(); }

// a native thread that reaches a wait, a delay or a thread creation during shutdown stops here for good
void ParkIfQuiescing() {
  if (tl_self == nullptr || !g_s.quiesced.load(std::memory_order_relaxed)) return;
  for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
}

bool StartGuestHostThread(uint8_t* base, uint32_t caller_pcr, uint32_t stack_size, const char* name, uint32_t start_address, std::function<void(PPCContext&, uint8_t*)> body,
                          std::function<void()> on_finish) {
  if (!gh2k::mem::GlobalMemory()) return false;
  const uint32_t caller = Be32(base, caller_pcr + kt::pcr::kPrcb + kt::pcr::kCurrentThread);
  kt::Params p;
  p.stack_size = stack_size;
  p.tls_slots = g_s.tls_slots;
  p.tls_static_size = g_s.tls_data_size;
  p.tls_template = g_s.tls_raw_address ? base + g_s.tls_raw_address : nullptr;
  p.tls_template_size = g_s.tls_raw_size;
  p.thread_id = g_s.next_id.fetch_add(1);
  p.start_address = start_address;
  p.cpu = FakeCpu(0);
  p.process = Be32(base, caller + 0x84);
  p.process_type = base[caller + kt::kthread::kProcessType];
  p.create_time = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count() / 100) + 116444736000000000ull;
  auto* t = new NThread();
  t->base = base;
  std::string error;
  if (!kt::Create(*gh2k::mem::GlobalMemory(), p, &t->blk, &error)) {
    fprintf(stderr, "THREADS: host thread '%s' failed: %s\n", name, error.c_str());
    delete t;
    return false;
  }
  t->kthread = t->blk.kthread;
  t->pcr = t->blk.pcr;
  t->thread_id = p.thread_id;
  t->start = start_address;
  t->refs = 1;
  t->body = std::move(body);
  t->on_finish = std::move(on_finish);
  t->name = name ? name : "";
  sy::ThreadRegister(t->kthread);
  {
    std::lock_guard<std::mutex> lock(g_s.mu);
    g_s.by_kthread[t->kthread] = t;
  }
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 16u << 20);
  const int rc = pthread_create(&t->host, &attr, ThreadMain, t);
  pthread_attr_destroy(&attr);
  if (rc != 0) {
    {
      std::lock_guard<std::mutex> lock(g_s.mu);
      g_s.by_kthread.erase(t->kthread);
    }
    sy::ThreadUnregister(t->kthread);
    kt::Destroy(*gh2k::mem::GlobalMemory(), t->blk);
    delete t;
    return false;
  }
  pthread_detach(t->host);
  ++g_s.created;
  return true;
}

void ThreadHandleClosed(uint32_t kthread) {
  if (NThread* t = Find(kthread)) Unref(t);
}

void ThreadStats(uint64_t out[6]) {
  out[0] = g_s.created.load();
  out[1] = g_s.finished.load();
  out[2] = g_s.self_suspends.load();
  out[3] = g_s.cooperative_suspends.load();
  out[4] = g_s.resumes.load();
  out[5] = g_s.delays.load();
}

}  // namespace gh2rt::hooks::detail

namespace gh2rt::hooks {

// At shutdown the runtime stops its own threads and the process runs the static destructors: a native thread still executing guest code would meet freed state. They are not stopped
// by a signal (that could freeze one while it holds a lock the teardown needs, e.g. stdio's): each parks, holding nothing, the next time it waits, sleeps or creates a thread.
void QuiesceNativeThreads() {
  if (g_s.quiesced.exchange(true)) return;
  gh2rt::hooks::detail::StopGpu();
  std::lock_guard<std::mutex> lock(g_s.mu);
  for (auto& [k, t] : g_s.by_kthread) t->cv.notify_all();   // threads in the suspend gate wake up to park
}

}  // namespace gh2rt::hooks

// ExCreateThread(handle*, stack_size, thread_id*, xapi_thread_startup, start_address, start_context, creation_flags)
REX_EXTERN(__wrap___imp__ExCreateThread) {
  GH2_TRACE_HOOK("ExCreateThread");
  if (!Active()) { __real___imp__ExCreateThread(ctx, base); return; }
  const uint32_t handle_ptr = ctx.r3.u32, stack_size = ctx.r4.u32, thread_id_ptr = ctx.r5.u32, xapi = ctx.r6.u32, start = ctx.r7.u32, context = ctx.r8.u32, flags = ctx.r9.u32;
  gh2abi::NoteRuntimeThread();   // (if the creator is a thread of the runtime: the identity native threads borrow for the imports still forwarded)
  gh2rt::hooks::detail::ParkIfQuiescing();
  ReadHeader(ctx, base);
  const uint32_t caller = CurrentKthread(base, ctx);

  kt::Params p;
  uint32_t size = stack_size ? stack_size : g_s.default_stack;
  size = (size + 0xFFF) & 0xFFFFF000u;
  p.stack_size = size < 0x4000 ? 0x4000 : size;
  p.tls_slots = g_s.tls_slots;
  p.tls_static_size = g_s.tls_data_size;
  p.tls_template = g_s.tls_raw_address ? base + g_s.tls_raw_address : nullptr;
  p.tls_template_size = g_s.tls_raw_size;
  p.thread_id = g_s.next_id.fetch_add(1);
  p.start_address = start;
  p.creation_flags = flags;
  p.cpu = FakeCpu(static_cast<uint8_t>(flags >> 24));
  p.process = Be32(base, caller + 0x84);   // the creator's process (the title's; a "system thread" request gets the same one)
  p.process_type = base[caller + kt::kthread::kProcessType];
  p.create_time = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count() / 100) + 116444736000000000ull;

  auto* t = new NThread();
  t->base = base;
  std::string error;
  if (!kt::Create(*gh2k::mem::GlobalMemory(), p, &t->blk, &error)) {
    fprintf(stderr, "THREADS: ExCreateThread failed: %s\n", error.c_str());
    delete t;
    ctx.r3.u64 = kStatusNoMemory;
    return;
  }
  t->kthread = t->blk.kthread;
  t->pcr = t->blk.pcr;
  t->thread_id = p.thread_id;
  t->xapi = xapi;
  t->start = start;
  t->context = context;
  t->flags = flags;
  t->refs = 1;
  sy::ThreadRegister(t->kthread);
  if (handle_ptr) {
    if (flags & 0x80) {
      PutBe32(base, handle_ptr, t->kthread);
    } else {
      t->handle = gh2rt::hooks::detail::ObjectHandleCreate(t->kthread);
      t->refs = 2;
      PutBe32(base, handle_ptr, t->handle);
    }
  }
  if (thread_id_ptr) PutBe32(base, thread_id_ptr, t->thread_id);
  {
    std::lock_guard<std::mutex> lock(g_s.mu);
    g_s.by_kthread[t->kthread] = t;
  }
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 16u << 20);   // the recompiled code keeps its frames on the host stack: as big as the runtime gives its guest threads
  const int rc = pthread_create(&t->host, &attr, ThreadMain, t);
  pthread_attr_destroy(&attr);
  if (rc != 0) {
    fprintf(stderr, "THREADS: pthread_create failed (%d)\n", rc);
    {
      std::lock_guard<std::mutex> lock(g_s.mu);
      g_s.by_kthread.erase(t->kthread);
    }
    sy::ThreadUnregister(t->kthread);
    kt::Destroy(*gh2k::mem::GlobalMemory(), t->blk);
    delete t;
    ctx.r3.u64 = kStatusNoMemory;
    return;
  }
  pthread_detach(t->host);
  ++g_s.created;
  if (getenv("GH2_THREADS_TRACE"))
    fprintf(stderr, "THREADS +%u kthread %08X pcr %08X stack %08X..%08X handle %08X entry %08X (xapi %08X) ctx %08X flags %08X cpu %u by %08X\n", t->thread_id, t->kthread, t->pcr, t->blk.stack.limit,
            t->blk.stack.top, t->handle, start, xapi, context, flags, p.cpu, caller);
  ctx.r3.u64 = 0;
}

// ExTerminateThread(exit_code): does not return
REX_EXTERN(__wrap___imp__ExTerminateThread) {
  GH2_TRACE_HOOK("ExTerminateThread");
  NThread* t = Active() ? Current(base, ctx) : nullptr;
  if (!t) { __real___imp__ExTerminateThread(ctx, base); return; }
  Finish(t, ctx.r3.u32);
  pthread_exit(nullptr);
}

// NtSuspendThread(handle, previous_count*)
REX_EXTERN(__wrap___imp__NtSuspendThread) {
  GH2_TRACE_HOOK("NtSuspendThread");
  NThread* t = Active() ? Find(gh2rt::hooks::detail::ObjectOfHandle(ctx.r3.u32)) : nullptr;
  if (!t) { __real___imp__NtSuspendThread(ctx, base); return; }
  const uint32_t previous_ptr = ctx.r4.u32;
  if (base[t->kthread + kKthreadTerminated]) { ctx.r3.u64 = kStatusThreadIsTerminating; return; }
  const bool self = CurrentKthread(base, ctx) == t->kthread;
  std::unique_lock<std::mutex> lock(t->mu);
  const uint32_t previous = base[t->kthread + kt::kthread::kSuspendCount];
  base[t->kthread + kt::kthread::kSuspendCount] = static_cast<uint8_t>(previous + 1);
  if (previous_ptr) PutBe32(base, previous_ptr, previous);
  if (self) {
    ++g_s.self_suspends;
    t->cv.wait(lock, [&] { return base[t->kthread + kt::kthread::kSuspendCount] == 0 || g_s.quiesced.load(); });
  } else {
    ++g_s.cooperative_suspends;
    if (!g_s.warned_cooperative.exchange(true)) fprintf(stderr, "THREADS: a thread other than the caller was suspended (cooperative: it stops at its next wait or delay)\n");
  }
  ctx.r3.u64 = 0;
}

namespace {
// returns the suspend count before; 0xFFFFFFFF if the thread is not ours
uint32_t ResumeNative(uint8_t* base, NThread* t) {
  std::lock_guard<std::mutex> lock(t->mu);
  const uint32_t previous = base[t->kthread + kt::kthread::kSuspendCount];
  if (previous > 0) base[t->kthread + kt::kthread::kSuspendCount] = static_cast<uint8_t>(previous - 1);
  ++g_s.resumes;
  t->cv.notify_all();
  return previous;
}
}  // namespace

// NtResumeThread(handle, previous_count*)
REX_EXTERN(__wrap___imp__NtResumeThread) {
  GH2_TRACE_HOOK("NtResumeThread");
  NThread* t = Active() ? Find(gh2rt::hooks::detail::ObjectOfHandle(ctx.r3.u32)) : nullptr;
  if (!t) { __real___imp__NtResumeThread(ctx, base); return; }
  const uint32_t previous = ResumeNative(base, t);
  if (ctx.r4.u32) PutBe32(base, ctx.r4.u32, previous);
  ctx.r3.u64 = 0;
}

// KeResumeThread(kthread*) -> previous suspend count
REX_EXTERN(__wrap___imp__KeResumeThread) {
  GH2_TRACE_HOOK("KeResumeThread");
  NThread* t = Active() ? Find(ctx.r3.u32) : nullptr;
  if (!t) { __real___imp__KeResumeThread(ctx, base); return; }
  ctx.r3.u64 = ResumeNative(base, t);
}

// KeSetAffinityThread(kthread*, affinity, previous_affinity*) -> status
REX_EXTERN(__wrap___imp__KeSetAffinityThread) {
  GH2_TRACE_HOOK("KeSetAffinityThread");
  NThread* t = Active() ? Find(ctx.r3.u32) : nullptr;
  if (!t) { __real___imp__KeSetAffinityThread(ctx, base); return; }
  const uint32_t affinity = ctx.r4.u32, previous_ptr = ctx.r5.u32;
  if (!affinity) { ctx.r3.u64 = kStatusInvalidParameter; return; }
  if (previous_ptr) PutBe32(base, previous_ptr, 1u << base[t->kthread + kt::kthread::kCurrentCpu]);
  const uint8_t cpu = FakeCpu(static_cast<uint8_t>(affinity));
  base[t->kthread + kt::kthread::kCurrentCpu] = cpu;
  base[t->pcr + kt::pcr::kPrcb + kt::pcr::kCurrentCpu] = cpu;
  ctx.r3.u64 = 0;
}

// KeSetBasePriorityThread(kthread*, increment) -> the previous priority, as the runtime reports it (its host priority class: 0 normal)
REX_EXTERN(__wrap___imp__KeSetBasePriorityThread) {
  GH2_TRACE_HOOK("KeSetBasePriorityThread");
  NThread* t = Active() ? Find(ctx.r3.u32) : nullptr;
  if (!t) { __real___imp__KeSetBasePriorityThread(ctx, base); return; }
  const int32_t increment = static_cast<int32_t>(ctx.r4.u32);
  const int previous = t->host_priority;
  t->host_priority = increment > 0x22 ? 2 : increment > 0x11 ? 1 : increment < -0x22 ? -2 : increment < -0x11 ? -1 : 0;
  base[t->kthread + kKthreadPriority] = static_cast<uint8_t>(increment < 0 ? 0 : increment > 31 ? 31 : increment);
  ctx.r3.u64 = static_cast<uint32_t>(previous);
}

// KeDelayExecutionThread(processor_mode, alertable, interval*) -> status. Relative times only (the runtime asserts on absolute ones); alertable has no APC to wake for.
REX_EXTERN(__wrap___imp__KeDelayExecutionThread) {
  GH2_TRACE_HOOK("KeDelayExecutionThread");
  NThread* t = Active() ? Current(base, ctx) : nullptr;
  if (!t) { __real___imp__KeDelayExecutionThread(ctx, base); return; }
  ++g_s.delays;
  uint64_t raw;
  std::memcpy(&raw, base + ctx.r5.u32, 8);
  const int64_t interval = static_cast<int64_t>(__builtin_bswap64(raw));
  Gate(t);
  if (interval < 0) {
    const uint32_t ms = static_cast<uint32_t>(-interval / 10000);
    if (ms) std::this_thread::sleep_for(std::chrono::milliseconds(ms)); else std::this_thread::yield();
  } else {
    std::this_thread::yield();
  }
  Gate(t);
  ctx.r3.u64 = 0;
}

// KeGetCurrentProcessType() -> the process type of the current thread (or the DPC's, when a DPC is active)
REX_EXTERN(__wrap___imp__KeGetCurrentProcessType) {
  GH2_TRACE_HOOK("KeGetCurrentProcessType");
  NThread* t = Active() ? Current(base, ctx) : nullptr;
  if (!t) { __real___imp__KeGetCurrentProcessType(ctx, base); return; }
  const uint32_t pcr = ctx.r13.u32;
  ctx.r3.u64 = Be32(base, pcr + kPcrDpcActive) ? base[pcr + kPcrProcessTypeInDpc] : base[t->kthread + kt::kthread::kProcessType];
}

// KeEnterCriticalRegion / KeLeaveCriticalRegion(): the runtime counts in the current thread's KTHREAD (apc_disable_count, a host-order int at +0xB0)
REX_EXTERN(__wrap___imp__KeEnterCriticalRegion) {
  GH2_TRACE_HOOK("KeEnterCriticalRegion");
  NThread* t = Active() ? Current(base, ctx) : nullptr;
  if (!t) { __real___imp__KeEnterCriticalRegion(ctx, base); return; }
  --*reinterpret_cast<int32_t*>(base + t->kthread + 0xB0);
}
REX_EXTERN(__wrap___imp__KeLeaveCriticalRegion) {
  GH2_TRACE_HOOK("KeLeaveCriticalRegion");
  NThread* t = Active() ? Current(base, ctx) : nullptr;
  if (!t) { __real___imp__KeLeaveCriticalRegion(ctx, base); return; }
  ++*reinterpret_cast<int32_t*>(base + t->kthread + 0xB0);
}

// xstart: the title's entry point. The runtime starts it on its own main thread (an XThread it created); with --main_thread=native the original runs on a native thread instead and the
// runtime's main thread only waits for it, so every guest thread of the game is ours. The runtime still sees "its" main thread alive until the game's ends, which is what its launcher waits for.
REX_EXTERN(xstart) {
  static const bool native_main = [] {
    std::string mode = gh2rt::Host().GetSettings().main_thread;
    if (const char* e = getenv("GH2_MAIN_THREAD"); e && *e) mode = e;
    return mode == "native";
  }();
  if (!native_main || !Active()) { __imp__xstart(ctx, base); return; }
  gh2abi::NoteRuntimeThread();   // the runtime's main thread is the identity native threads borrow for the imports still forwarded
  ReadHeader(ctx, base);
  struct Shared {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    uint32_t result = 0;
  };
  auto shared = std::make_shared<Shared>();
  // the entry's arguments (r3..r10); everything else of the new thread comes from its own PCR and stack
  uint64_t args[8];
  const uint64_t* regs[8] = {&ctx.r3.u64, &ctx.r4.u64, &ctx.r5.u64, &ctx.r6.u64, &ctx.r7.u64, &ctx.r8.u64, &ctx.r9.u64, &ctx.r10.u64};
  for (int i = 0; i < 8; ++i) args[i] = *regs[i];
  const bool started = gh2rt::hooks::detail::StartGuestHostThread(
      base, ctx.r13.u32, g_s.default_stack ? g_s.default_stack : 0x100000, "gh2-main", 0x823B51E0,
      [shared, args](PPCContext& nctx, uint8_t* nbase) {
        nctx.r3.u64 = args[0]; nctx.r4.u64 = args[1]; nctx.r5.u64 = args[2]; nctx.r6.u64 = args[3];
        nctx.r7.u64 = args[4]; nctx.r8.u64 = args[5]; nctx.r9.u64 = args[6]; nctx.r10.u64 = args[7];
        __imp__xstart(nctx, nbase);
        std::lock_guard<std::mutex> lock(shared->mu);
        shared->result = nctx.r3.u32;
      },
      [shared] {
        std::lock_guard<std::mutex> lock(shared->mu);
        shared->done = true;
        shared->cv.notify_all();
      });
  if (!started) { __imp__xstart(ctx, base); return; }
  fprintf(stderr, "THREADS: the game's main thread (xstart) runs on a native thread; the runtime's main thread waits for it\n");
  std::unique_lock<std::mutex> lock(shared->mu);
  shared->cv.wait(lock, [&] { return shared->done; });
  ctx.r3.u64 = shared->result;
}

// KeTlsAlloc() -> slot (0xFFFFFFFF when none is free) / KeTlsFree(slot) -> 1. The process (KPROCESS+0x30) keeps 256 slot bits in eight big-endian words, a set bit meaning FREE, slot n being
// bit 31 - n%32 of word n/32 (read from the runtime's KernelState::AllocateTLS / FreeTLS). The runtime's version asks for the current runtime thread; these work from the PCR. A new slot reads
// 0 in the thread that allocated it; freeing one zeroes it in the threads of the process that this layer knows (the native ones and the caller).
namespace {
std::mutex& TlsMutex() {
  static std::mutex& m = *new std::mutex;
  return m;
}
constexpr uint32_t kProcessTlsBitmap = 0x30, kKthreadProcess = 0x84, kNoSlot = 0xFFFFFFFFu;
}  // namespace

REX_EXTERN(__wrap___imp__KeTlsAlloc) {
  GH2_TRACE_HOOK("KeTlsAlloc");
  if (!Active()) { __real___imp__KeTlsAlloc(ctx, base); return; }
  const uint32_t process = Be32(base, CurrentKthread(base, ctx) + kKthreadProcess);
  uint32_t slot = kNoSlot;
  if (process) {
    std::lock_guard<std::mutex> lock(TlsMutex());
    for (uint32_t word = 0; word < 8 && slot == kNoSlot; ++word) {
      const uint32_t bits = Be32(base, process + kProcessTlsBitmap + 4 * word);
      if (!bits) continue;
      const uint32_t lz = static_cast<uint32_t>(__builtin_clz(bits));
      if (word * 32 + lz >= 256) continue;
      PutBe32(base, process + kProcessTlsBitmap + 4 * word, bits & ~(1u << (31 - lz)));
      slot = word * 32 + lz;
    }
  }
  if (slot != kNoSlot) gh2k::sync::TlsSet(base, ctx.r13.u32, slot, 0);
  ctx.r3.u64 = slot;
}

REX_EXTERN(__wrap___imp__KeTlsFree) {
  GH2_TRACE_HOOK("KeTlsFree");
  if (!Active()) { __real___imp__KeTlsFree(ctx, base); return; }
  const uint32_t slot = ctx.r3.u32;
  if (slot == kNoSlot) { ctx.r3.u64 = 0; return; }
  const uint32_t process = Be32(base, CurrentKthread(base, ctx) + kKthreadProcess);
  if (slot < 256 && process) {
    std::lock_guard<std::mutex> lock(TlsMutex());
    const uint32_t word = slot / 32, mask = 1u << (31 - slot % 32);
    const uint32_t bits = Be32(base, process + kProcessTlsBitmap + 4 * word);
    if (!(bits & mask)) {   // in use: forget the value in every thread, then free the slot
      gh2k::sync::TlsSet(base, ctx.r13.u32, slot, 0);
      std::vector<uint32_t> pcrs;
      {
        std::lock_guard<std::mutex> threads_lock(g_s.mu);
        for (auto& [k, t] : g_s.by_kthread)
          if (t->alive && Be32(base, k + kKthreadProcess) == process) pcrs.push_back(t->pcr);
      }
      for (uint32_t pcr : pcrs) gh2k::sync::TlsSet(base, pcr, slot, 0);
      PutBe32(base, process + kProcessTlsBitmap + 4 * word, bits | mask);
    }
  }
  ctx.r3.u64 = 1;
}
