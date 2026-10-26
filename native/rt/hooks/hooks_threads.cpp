// Native guest threads (Phase 2.7, K.2): the first guest code running on threads whose PCR, KTHREAD, TLS block and stack were built by gh2k::thread out of gh2k::mem.
//
// GH2_NATIVE_THREADS=N (default 0 = off) starts N such threads once the game has drawn 120 frames. Each loops, on a host thread of its own with its own PPCContext (r13 = its PCR,
// r1 inside its guard-protected stack): (a) runs the game's own recompiled memcpy (sub_826A85E8, the original body, not the host-memmove hook) on buffers in its stack and checks the copy,
// (b) takes a critical section shared by all native threads, (c) round-trips a TLS slot, (d) raises the IRQL and takes a spin lock. At the start it also builds a thread from the parameters
// of the calling (runtime-made) game thread and compares the PCR and KTHREAD with the live ones word by word (GH2_THREADS_REPORT=<json> gets the result).
// Needs the native heaps (the memory manager over the arena); otherwise it does nothing. No SDK dependency.
#include "gh2_pch.h"  // PPCContext, REX_EXTERN

#include <gh2k/mem.h>
#include <gh2k/sync.h>
#include <gh2k/thread.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "hooks_internal.h"

REX_EXTERN(__imp__sub_826A85E8);   // the guest memcpy, recompiled

namespace gh2rt::hooks::detail {
namespace {

constexpr int kMaxThreads = 16;
struct Counters {
  std::atomic<uint64_t> iterations{0}, copies{0}, copy_errors{0}, bytes{0}, cs_enters{0}, tls_errors{0}, irql_errors{0}, spin_sections{0};
} g_c;
std::atomic<bool> g_started{false};
std::atomic<uint32_t> g_frames{0};
uint32_t g_shared_cs = 0, g_shared_lock = 0;
int g_count = 0;
double g_t0 = 0;
std::string g_compare_json;

uint32_t Be32(const uint8_t* b, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, b + a, 4);
  return __builtin_bswap32(v);
}
void PutBe32(uint8_t* b, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(b + a, &v, 4);
}

void Worker(int index, uint8_t* base, gh2k::thread::Block blk) {
  auto* ctx = new PPCContext();
  ctx->fpscr.InitHost();
  ctx->r1.u64 = (blk.stack.top - 0x100) & ~0xFull;
  ctx->r13.u64 = blk.pcr;
  constexpr uint32_t kLen = 8192;
  const uint32_t src = blk.stack.limit + 0x1000, dst = src + 0x4000;
  uint64_t iter = 0;
  while (true) {
    if (index == 0 && g_frames.load() == 0xFFFFFFFFu) break;
    for (uint32_t i = 0; i < kLen; ++i) base[src + i] = static_cast<uint8_t>(i * 31 + iter + index);
    std::memset(base + dst, 0, kLen);
    // (a) the game's own recompiled memcpy, on this thread's context
    ctx->r3.u64 = dst;
    ctx->r4.u64 = src;
    ctx->r5.u64 = kLen;
    __imp__sub_826A85E8(*ctx, base);
    ++g_c.copies;
    g_c.bytes += kLen;
    if (ctx->r3.u32 != dst || std::memcmp(base + dst, base + src, kLen) != 0) ++g_c.copy_errors;
    // (b) a critical section shared by all native threads, owned through the KTHREAD
    gh2k::sync::CritEnter(base, blk.pcr, g_shared_cs);
    PutBe32(base, g_shared_cs + 0x40, Be32(base, g_shared_cs + 0x40) + 1);
    gh2k::sync::CritLeave(base, blk.pcr, g_shared_cs);
    ++g_c.cs_enters;
    // (c) thread-local slot round trip
    const uint32_t value = static_cast<uint32_t>(iter * 2654435761u) ^ index;
    gh2k::sync::TlsSet(base, blk.pcr, 3 + index, value);
    if (gh2k::sync::TlsGet(base, blk.pcr, 3 + index) != value) ++g_c.tls_errors;
    // (d) IRQL and a spin lock
    const uint32_t old = gh2k::sync::SpinAcquireRaising(base, blk.pcr, g_shared_lock);
    PutBe32(base, g_shared_lock + 8, Be32(base, g_shared_lock + 8) + 1);
    gh2k::sync::SpinReleaseLowering(base, blk.pcr, g_shared_lock, old);
    if (base[blk.pcr + 0x18] != old) ++g_c.irql_errors;
    ++g_c.spin_sections;
    ++g_c.iterations;
    ++iter;
    std::this_thread::sleep_for(std::chrono::microseconds(700));
  }
}

// Builds a thread with the live thread's parameters and lists the words that differ.
void CompareWithLive(gh2k::mem::Memory& mem, uint32_t live_pcr, uint8_t* base) {
  const uint32_t live_kthread = Be32(base, live_pcr + 0x100);
  gh2k::thread::Params p;
  const uint32_t live_tls = Be32(base, live_pcr), live_dyn = Be32(base, live_kthread + 0x68);
  p.tls_static_size = live_dyn - live_tls;
  p.stack_size = Be32(base, live_kthread + 0x5C) - Be32(base, live_kthread + 0x60);
  p.thread_id = Be32(base, live_kthread + 0x14C);
  p.start_address = Be32(base, live_kthread + 0x150);
  p.creation_flags = Be32(base, live_kthread + 0x16C);
  p.cpu = base[live_kthread + 0xBF];
  p.process = Be32(base, live_kthread + 0x84);
  p.process_type = base[live_kthread + 0x73];
  uint64_t ct;
  std::memcpy(&ct, base + live_kthread + 0x130, 8);
  p.create_time = __builtin_bswap64(ct);
  gh2k::thread::Block mine;
  std::string error;
  if (!gh2k::thread::Create(mem, p, &mine, &error)) {
    g_compare_json = "{\"error\": \"" + error + "\"}";
    return;
  }
  struct Region {
    const char* name;
    uint32_t live, ours, size;
  } regions[] = {{"pcr", live_pcr, mine.pcr, gh2k::thread::pcr::kSize}, {"kthread", live_kthread, mine.kthread, gh2k::thread::kthread::kSize}};
  // a word is "relocated equal" when it points at the same place relative to the same object in both threads
  auto relative = [&](uint32_t a, uint32_t b) {
    return a == b || (a - live_pcr == b - mine.pcr && a - live_pcr < 0x1000) || (a - live_kthread == b - mine.kthread && a - live_kthread < 0x1000) ||
           (a - Be32(base, live_kthread + 0x5C) == b - mine.stack.top && a - Be32(base, live_kthread + 0x5C) < 0x1000) ||
           (a - live_tls == b - mine.tls_static && a - live_tls < 0x100000);
  };
  char buf[160];
  std::string out = "{\n";
  snprintf(buf, sizeof(buf), " \"live_thread_id\": %u, \"live_tls_static_bytes\": %u, \"live_stack_bytes\": %u,\n", p.thread_id, p.tls_static_size, p.stack_size);
  out += buf;
  for (const Region& r : regions) {
    uint32_t same = 0, relocated = 0, live_only = 0, differ = 0;
    std::string list;
    for (uint32_t o = 0; o + 4 <= r.size; o += 4) {
      const uint32_t a = Be32(base, r.live + o), b = Be32(base, r.ours + o);
      if (a == b) ++same;
      else if (relative(a, b)) ++relocated;
      else if (b == 0) {
        ++live_only;   // the live thread has something there that the builder does not set (the runtime sets it elsewhere)
        if (list.size() < 1500) { snprintf(buf, sizeof(buf), "%s\"+%X: live %08X, ours 0\"", list.empty() ? "" : ", ", o, a); list += buf; }
      } else {
        ++differ;
        if (list.size() < 1500) { snprintf(buf, sizeof(buf), "%s\"+%X: live %08X, ours %08X\"", list.empty() ? "" : ", ", o, a, b); list += buf; }
      }
    }
    snprintf(buf, sizeof(buf), " \"%s\": {\"words\": %u, \"identical\": %u, \"same_relative_pointer\": %u, \"live_only\": %u, \"different\": %u, \"first_differences\": [", r.name, r.size / 4, same, relocated, live_only, differ);
    out += buf + list + "]},\n";
  }
  out += " \"x\": 0\n}";
  g_compare_json = out;
  gh2k::thread::Destroy(mem, mine);
}

void Start(uint32_t live_pcr, uint8_t* base) {
  gh2k::mem::Memory* mem = gh2k::mem::GlobalMemory();
  if (!mem) {
    fprintf(stderr, "NTHREADS needs the native heaps (--heaps=native): not starting\n");
    return;
  }
  CompareWithLive(*mem, live_pcr, base);
  if (!mem->v00000000().AllocRange(0x30000000u, 0x3FFFFFFFu, 4096, 4096, false, &g_shared_cs) || !mem->v00000000().Commit(g_shared_cs, 4096, gh2k::mem::kReadWrite)) return;
  g_shared_lock = g_shared_cs + 0x80;
  gh2k::sync::CritInit(base, g_shared_cs, 0);
  PutBe32(base, g_shared_cs + 0x40, 0);
  PutBe32(base, g_shared_lock, 0);
  PutBe32(base, g_shared_lock + 8, 0);
  const auto now = std::chrono::steady_clock::now();
  g_t0 = std::chrono::duration<double>(now.time_since_epoch()).count();
  int started = 0;
  for (int i = 0; i < g_count; ++i) {
    gh2k::thread::Params p;
    p.stack_size = 128 * 1024;
    p.tls_static_size = 0;
    p.tls_slots = 64;
    p.thread_id = 0x7000 + i;
    p.cpu = i % 6;
    gh2k::thread::Block blk;
    std::string error;
    if (!gh2k::thread::Create(*mem, p, &blk, &error)) {
      fprintf(stderr, "NTHREADS thread %d: %s\n", i, error.c_str());
      continue;
    }
    std::thread(Worker, i, base, blk).detach();
    ++started;
  }
  fprintf(stderr, "NTHREADS started %d native guest threads (PCR/KTHREAD/TLS/stack from gh2k::mem)\n", started);
  std::thread([started, base] {
    const char* report = getenv("GH2_THREADS_REPORT");
    for (int n = 1;; ++n) {
      std::this_thread::sleep_for(std::chrono::seconds(10));
      fprintf(stderr, "NTHREADS T+%ds iterations %llu, guest memcpy %llu (%.1f MB, errors %llu), critical sections %llu, tls errors %llu, irql errors %llu, spin sections %llu\n", n * 10,
              (unsigned long long)g_c.iterations.load(), (unsigned long long)g_c.copies.load(), g_c.bytes.load() / 1e6, (unsigned long long)g_c.copy_errors.load(),
              (unsigned long long)g_c.cs_enters.load(), (unsigned long long)g_c.tls_errors.load(), (unsigned long long)g_c.irql_errors.load(), (unsigned long long)g_c.spin_sections.load());
      if (report && *report)
        if (FILE* f = fopen(report, "w")) {
          fprintf(f, "{\"threads\": %d, \"seconds\": %d, \"iterations\": %llu, \"guest_memcpy_calls\": %llu, \"bytes_copied\": %llu, \"copy_errors\": %llu, \"critical_section_enters\": %llu, "
                     "\"tls_errors\": %llu, \"irql_errors\": %llu, \"spin_lock_sections\": %llu, \"shared_counter_in_guest_memory\": %u, \"shared_spin_counter_in_guest_memory\": %u, \"live_comparison\": %s}\n",
                  started, n * 10, (unsigned long long)g_c.iterations.load(), (unsigned long long)g_c.copies.load(), (unsigned long long)g_c.bytes.load(),
                  (unsigned long long)g_c.copy_errors.load(), (unsigned long long)g_c.cs_enters.load(), (unsigned long long)g_c.tls_errors.load(), (unsigned long long)g_c.irql_errors.load(),
                  (unsigned long long)g_c.spin_sections.load(), Be32(base, g_shared_cs + 0x40), Be32(base, g_shared_lock + 8), g_compare_json.c_str());
          fclose(f);
        }
    }
  }).detach();
}

}  // namespace

void NativeThreadsOnFrame(uint32_t live_pcr, uint8_t* base) {
  static const int n = [] {
    const char* e = getenv("GH2_NATIVE_THREADS");
    return e ? std::min(atoi(e), kMaxThreads) : 0;
  }();
  if (n <= 0) return;
  if (g_started.load(std::memory_order_relaxed)) return;
  if (++g_frames < 120) return;
  if (g_started.exchange(true)) return;
  g_count = n;
  Start(live_pcr, base);
}

}  // namespace gh2rt::hooks::detail
