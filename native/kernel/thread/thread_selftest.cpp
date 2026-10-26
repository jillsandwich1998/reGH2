// gh2_thread_selftest - PCR / KTHREAD / TLS / stack built natively, then used by the native sync primitives from real host threads.
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "gh2k/sync.h"
#include "gh2k/thread.h"

using namespace gh2k;

namespace {
int g_failures = 0;
void Check(bool ok, const std::string& what, const std::string& detail = "") {
  std::printf("[%s] %s%s%s\n", ok ? "ok" : "!!", what.c_str(), detail.empty() ? "" : " - ", detail.c_str());
  if (!ok) ++g_failures;
}
uint32_t Be32(const uint8_t* b, uint32_t a) { uint32_t v; std::memcpy(&v, b + a, 4); return __builtin_bswap32(v); }
void PutBe32(uint8_t* b, uint32_t a, uint32_t v) { v = __builtin_bswap32(v); std::memcpy(b + a, &v, 4); }
bool Faults(uint8_t* host) {
  const pid_t pid = fork();
  if (pid == 0) { signal(SIGSEGV, SIG_DFL); *reinterpret_cast<volatile uint8_t*>(host) = 1; _exit(0); }
  int st = 0;
  waitpid(pid, &st, 0);
  return WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV;
}
}  // namespace

int main() {
  std::string error;
  auto m = mem::Memory::Create(&error);
  Check(m != nullptr, "memory manager", error);
  if (!m) return 1;
  uint8_t* b = m->base();
  thread::Params p;
  p.stack_size = 128 * 1024;
  p.tls_static_size = 64;
  p.thread_id = 0x1234;
  p.start_address = 0x82001000;
  uint8_t tpl[64];
  std::memset(tpl, 0x5A, sizeof(tpl));
  p.tls_template = tpl;
  thread::Block t;
  Check(thread::Create(*m, p, &t, &error), "create a thread", error);
  Check(Be32(b, t.pcr + thread::pcr::kTlsPtr) == t.tls_static && b[t.tls_static] == 0x5A && b[t.tls_static + 63] == 0x5A && b[t.tls_dynamic] == 0, "PCR.tls_ptr -> static TLS data (template copied), slots start after it and are zero");
  Check(Be32(b, t.pcr + thread::pcr::kPrcb + thread::pcr::kCurrentThread) == t.kthread && Be32(b, t.pcr + thread::pcr::kPrcbPointer) == t.pcr + 0x100, "PCR -> KPRCB -> current KTHREAD");
  Check(Be32(b, t.kthread + thread::kthread::kTlsDynamic) == t.tls_dynamic && Be32(b, t.kthread + thread::kthread::kStackBase) == t.stack.top && Be32(b, t.kthread + thread::kthread::kStackLimit) == t.stack.limit, "KTHREAD: TLS slots and stack bounds");
  Check(b[t.kthread] == 6 && Be32(b, t.kthread + thread::kthread::kThreadId) == 0x1234 && Be32(b, t.kthread + thread::kthread::kStartAddress) == 0x82001000, "KTHREAD: type 6, thread id, start address");
  Check(Be32(b, t.kthread + thread::kthread::kApcLists) == t.kthread + thread::kthread::kApcLists, "APC lists start empty (self-linked)");
  Check(Faults(b + t.stack.limit - 1) && Faults(b + t.stack.top), "stack guard regions fault on both sides");
  b[t.stack.limit] = 1;
  b[t.stack.top - 1] = 1;

  // the native primitives use it: TLS slots per thread, IRQL in the PCR, a critical section owned by the KTHREAD
  thread::Block t2;
  Check(thread::Create(*m, p, &t2, &error), "create a second thread");
  Check(t.pcr != t2.pcr && t.kthread != t2.kthread && t.stack.alloc_base != t2.stack.alloc_base, "distinct PCR, KTHREAD and stack");
  sync::TlsSet(b, t.pcr, 5, 0xAAAA);
  sync::TlsSet(b, t2.pcr, 5, 0xBBBB);
  Check(sync::TlsGet(b, t.pcr, 5) == 0xAAAA && sync::TlsGet(b, t2.pcr, 5) == 0xBBBB, "KeTlsSetValue/GetValue are per thread");
  const uint32_t old = sync::RaiseIrql(b, t.pcr, sync::kIrqlDispatch);
  Check(old == 0 && b[t.pcr + 0x18] == 2 && b[t2.pcr + 0x18] == 0, "IRQL lives in each thread's own PCR");
  sync::LowerIrql(b, t.pcr, old);
  uint32_t cs;
  Check(m->v00000000().AllocRange(0x30000000u, 0x3FFFFFFFu, 4096, 4096, false, &cs) && m->v00000000().Commit(cs, 4096, mem::kReadWrite), "a guest critical section");
  sync::CritInit(b, cs, 0);
  PutBe32(b, cs + 0x40, 0);   // shared counter
  const int kIters = 50000;
  std::vector<std::thread> workers;
  for (const thread::Block* blk : {&t, &t2})
    workers.emplace_back([&, blk] {
      for (int i = 0; i < kIters; ++i) {
        sync::CritEnter(b, blk->pcr, cs);
        PutBe32(b, cs + 0x40, Be32(b, cs + 0x40) + 1);
        sync::CritLeave(b, blk->pcr, cs);
      }
    });
  for (auto& w : workers) w.join();
  Check(Be32(b, cs + 0x40) == 2u * kIters && Be32(b, cs + 0x18) == 0, "two native threads contend on a critical section keyed by their KTHREADs: no lost update");
  thread::Destroy(*m, t);
  thread::Destroy(*m, t2);
  Check(Faults(b + t.stack.limit), "destroying a thread unmaps its stack");
  std::printf("%s\n", g_failures == 0 ? "PASS" : "FAIL");
  return g_failures == 0 ? 0 : 1;
}
