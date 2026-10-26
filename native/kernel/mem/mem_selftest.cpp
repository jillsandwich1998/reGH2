// gh2_mem_selftest - the native guest memory manager.
//   gh2_mem_selftest                       layout, aliases, heap semantics, stack guards, a random stress, throughput
//   gh2_mem_selftest --replay FILE         also replays the allocations a real run made (GH2_MEM_TRACE=FILE): every address the runtime returned must be
//                                          acceptable to the native heaps, and the native top-down policy is compared with the addresses it chose
// Exit status 0 = everything passed.
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "gh2k/mem.h"

using namespace gh2k::mem;

namespace {
int g_failures = 0;
void Check(bool ok, const std::string& what, const std::string& detail = "") {
  std::printf("[%s] %s%s%s\n", ok ? "ok" : "!!", what.c_str(), detail.empty() ? "" : " - ", detail.c_str());
  if (!ok) ++g_failures;
}

// does touching the guest address kill the process? (a child process, so that the test survives)
bool Faults(uint8_t* host, bool write) {
  const pid_t pid = fork();
  if (pid == 0) {
    signal(SIGSEGV, SIG_DFL);
    volatile uint8_t* p = host;
    if (write) *p = 1; else (void)*p;
    _exit(0);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  return WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV;
}

void Basics(Memory& m) {
  uint8_t* b = m.base();
  // aliases
  uint32_t a64 = 0, a4k = 0, a16m = 0;
  Check(m.PhysicalAlloc(0x20000, 0x10000, 0x10000, kReadWrite, &a64) && a64 >= 0xA0000000u && a64 < 0xC0000000u, "a 64 KB-page physical allocation lives in the 0xA0000000 window", std::to_string(a64));
  const uint32_t off = Memory::PhysicalOffset(a64);
  std::memset(b + a64, 0x5A, 16);
  Check(b[0xC0000000u + off] == 0x5A && b[0xE0000000u + off - 0x1000] == 0x5A && m.arena().physical()[off] == 0x5A, "the three windows and the physical view are the same bytes");
  Check(m.PhysicalAlloc(0x1000, 0x20, 0x1000, kReadWrite, &a4k) && a4k >= 0xE0000000u && a4k < 0xFFD00000u && (std::memset(b + a4k, 0x77, 4), m.arena().physical()[Memory::PhysicalOffset(a4k)] == 0x77), "a 4 KB-page allocation lives in 0xE0000000..0xFFCFFFFF");
  Check(m.PhysicalAllocationSize(a4k) == 0x1000 && m.PhysicalAllocationSize(a64) == 0x20000 && m.PhysicalAllocationSize(a4k + 4) == 0, "MmQueryAllocationSize: size by base address");
  Check(Memory::PhysicalOffset(a4k) + 0x1000 <= off || Memory::PhysicalOffset(a4k) >= off + 0x20000, "different allocations, different physical bytes");
  Check(m.PhysicalAlloc(0x1000000, 0, 0x1000000, kReadWrite, &a16m) && a16m >= 0xC0000000u && (a16m & 0xFFFFFF) == 0, "a 16 MB-page allocation is 16 MB aligned in 0xC0000000..");
  uint32_t released = 0;
  Check(m.PhysicalFree(a64, &released) && released == 0x20000 && b[0xC0000000u + off] == 0 && m.arena().physical()[off] == 0, "free returns the size and the memory reads back as zero (all windows)");
  // virtual heaps
  uint32_t v1 = 0, v2 = 0;
  Check(m.v00000000().Alloc(100, 0, kReadWrite, true, &v1) && v1 % 4096 == 0 && v1 >= 0x10000, "4 KB heap: page aligned, above the null region");
  b[v1] = 7;
  m.v00000000().Release(v1);
  Check(m.v40000000().Alloc(0x1000, 0, kReadWrite, true, &v2) && v2 >= 0x40000000u && v2 % 0x10000 == 0, "64 KB heap: 64 KB granular");
  Block blk;
  Check(m.v40000000().Query(v2 + 0x800, &blk) && blk.base == v2 && blk.size == 0x10000, "Query returns the containing allocation");
  Check(!m.v40000000().AllocFixed(v2 + 0x10000 - 0x10000, 0x10000, kReadWrite, true), "a fixed allocation over an existing one is refused");
  Check(m.v40000000().AllocFixed(v2 + 0x10000, 0x20000, kReadWrite, true) && m.v40000000().Release(v2 + 0x10000), "fixed allocation next to it works and is releasable");
  m.v40000000().Decommit(v2, 0x10000);
  Check(Faults(b + v2, false), "decommitted memory faults");
  Check(m.v40000000().Commit(v2, 0x10000, kReadWrite) && b[v2] == 0, "recommitted memory is zero");
  Check(m.v40000000().Release(v2) && !m.v40000000().Release(v2) && Faults(b + v2, false), "release; a second release fails; the memory is gone");
  // 4 GB layout: the translated code reaches every heap through base + address
  Check(m.HeapFor(0x1000) == &m.v00000000() && m.HeapFor(0x40755000) == &m.v40000000() && m.HeapFor(0x82000000) == &m.v80000000() && m.HeapFor(0x90000000) == &m.v90000000() && m.HeapFor(0xFFC9C000) == nullptr,
        "address to heap: 4K / 64K / XEX / XEX 4K / physical windows");
}

void Stacks(Memory& m) {
  Memory::Stack s;
  Check(m.AllocStack(256 * 1024, &s), "allocate a 256 KB stack");
  uint8_t* b = m.base();
  Check(s.limit == s.alloc_base + 0x10000 && s.top == s.limit + 256 * 1024 && s.alloc_size == 256 * 1024 + 2 * 0x10000 && b[s.limit] == 0xBE && b[s.top - 1] == 0xBE, "layout: guard, 256 KB filled with 0xBE, guard");
  Check(Faults(b + s.limit - 1, true), "writing just below the stack faults (overflow)");
  Check(Faults(b + s.top, true), "writing just above the stack faults");
  b[s.limit] = 1;
  b[s.top - 1] = 1;
  Memory::Stack tiny;
  Check(m.AllocStack(100, &tiny) && tiny.top - tiny.limit == 0x10000, "a tiny request gets 64 KB (16 KB minimum, page rounded)");
  Check(m.FreeStack(s) && m.FreeStack(tiny) && Faults(b + s.limit, false), "freeing a stack unmaps it");
}

void Stress(Memory& m) {
  // random allocate / free against a model: no overlaps, sizes exact, accounting returns to zero
  Heap& h = m.v00000000();
  std::mt19937 rng(1234);
  std::map<uint32_t, uint32_t> live;
  uint64_t ops = 0;
  const auto t0 = std::chrono::steady_clock::now();
  bool ok = true;
  for (int i = 0; i < 200000 && ok; ++i) {
    if (live.size() < 3000 && (rng() % 100 < 55 || live.empty())) {
      const uint32_t size = 1 + rng() % 200000, align = 1u << (rng() % 17);
      uint32_t a;
      if (!h.Alloc(size, align, kReadWrite, true, &a)) continue;
      const uint32_t rounded = (size + 4095) & ~4095u;
      auto next = live.lower_bound(a);
      if (next != live.end() && next->first < a + rounded) ok = false;
      if (next != live.begin() && std::prev(next)->first + std::prev(next)->second > a) ok = false;
      if (a % align != 0) ok = false;
      live[a] = rounded;
      ++ops;
    } else {
      auto it = live.begin();
      std::advance(it, rng() % live.size());
      uint32_t freed = 0;
      if (!h.Release(it->first, &freed) || freed != it->second) ok = false;
      live.erase(it);
      ++ops;
    }
  }
  const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
  uint64_t sum = 0;
  for (auto& [a, s] : live) sum += s;
  Check(ok && h.allocated_bytes() == sum + 0x10000 && h.blocks() == live.size() + 1, "200 000 random allocations/frees: no overlap, alignment honoured, sizes and accounting exact", std::to_string(ops) + " ops, " + std::to_string(us / ops).substr(0, 5) + " us each");
  for (auto& [a, s] : live) h.Release(a);
}

struct Rec {
  char kind[3];
  std::vector<uint64_t> v;
};

void Replay(Memory& m, const char* path) {
  FILE* f = std::fopen(path, "r");
  if (!f) {
    Check(false, std::string("open ") + path);
    return;
  }
  char line[256];
  uint64_t ap = 0, ap_ok = 0, ap_exact = 0, fp = 0, fp_ok = 0, fp_unknown = 0, av = 0, av_ok = 0, fv = 0, fv_ok = 0, qs = 0, qs_ok = 0;
  std::map<uint32_t, uint32_t> sizes;   // runtime address -> our address (physical), for the policy comparison
  // The runtime's own calls (command buffers, frame buffers) came first and are not in the trace: the first physical address it returned tells where the free area started.
  bool first_ap = true;
  while (std::fgets(line, sizeof(line), f)) {
    unsigned a[8] = {};
    char kind[8] = {};
    if (std::sscanf(line, "%7s", kind) != 1) continue;
    if (!std::strcmp(kind, "AP")) {
      unsigned flags, size, prot, mn, mx, align, ret;
      if (std::sscanf(line, "AP %x %x %x %x %x %x -> %x", &flags, &size, &prot, &mn, &mx, &align, &ret) != 7) continue;
      ++ap;
      if (ret == 0) continue;
      const uint32_t view = ret >= 0xE0000000u ? 0x1000 : ret >= 0xC0000000u ? 0x1000000 : 0x10000;
      const uint32_t off = Memory::PhysicalOffset(ret);
      if (first_ap) {   // everything above this was taken by the runtime before the game ran
        first_ap = false;
        const uint32_t end = off + ((size + view - 1) & ~(view - 1));
        m.physical().AllocFixed(end, 0x1FD01000u - end, kReadWrite, true);
      }
      uint32_t mine;
      if (!m.PhysicalAlloc(size, align, view, kReadWrite, &mine)) continue;
      ++ap_ok;
      if (mine == ret) ++ap_exact;
      sizes[ret] = mine;
    } else if (!std::strcmp(kind, "FP")) {
      unsigned type, addr;
      if (std::sscanf(line, "FP %x %x", &type, &addr) != 2) continue;
      ++fp;
      auto it = sizes.find(addr);
      if (it != sizes.end() && m.PhysicalFree(it->second)) {
        ++fp_ok;
        sizes.erase(it);
      } else if (it == sizes.end()) {
        ++fp_unknown;   // never allocated in this trace: made before it started, or by the runtime itself
      }
    } else if (!std::strcmp(kind, "AV")) {
      unsigned ib, is, type, prot, ob, os, status;
      if (std::sscanf(line, "AV %x %x %x %x -> %x %x status %x", &ib, &is, &type, &prot, &ob, &os, &status) != 7) continue;
      ++av;
      if (status != 0) continue;
      Heap* h = m.HeapFor(ob);
      Block blk;
      if (!h) continue;
      if (h->Query(ob, &blk)) {   // MEM_COMMIT inside an earlier reservation
        if (h->Commit(ob, os, kReadWrite)) ++av_ok;
      } else if (h->AllocFixed(ob, os, kReadWrite, true)) {
        ++av_ok;
      }
    } else if (!std::strcmp(kind, "FV")) {
      unsigned ib, is, type, ob, os, status;
      if (std::sscanf(line, "FV %x %x %x -> %x %x status %x", &ib, &is, &type, &ob, &os, &status) != 6) continue;
      ++fv;
      Heap* h = m.HeapFor(ib);
      Block blk;
      // release of the whole allocation (type 0x8000) or a decommit of a part (0x4000)
      if (h && h->Query(ib, &blk)) {
        if ((type & 0x8000) ? h->Release(blk.base) : h->Decommit(ib, is ? is : blk.size)) ++fv_ok;
      }
    } else if (!std::strcmp(kind, "QS")) {
      unsigned addr, size;
      if (std::sscanf(line, "QS %x -> %x", &addr, &size) != 2) continue;
      ++qs;
      auto it = sizes.find(addr);
      if (it != sizes.end() && m.PhysicalAllocationSize(it->second) == size) ++qs_ok;
      else if (it == sizes.end()) ++qs_ok;   // allocated before the trace or already freed
    }
    (void)a;
  }
  std::fclose(f);
  Check(ap_ok == ap, "replay: every MmAllocatePhysicalMemoryEx of the run succeeds", std::to_string(ap_ok) + "/" + std::to_string(ap));
  std::printf("    native top-down policy chose exactly the runtime's address for %llu of %llu allocations (%.2f%%)\n", (unsigned long long)ap_exact, (unsigned long long)ap, ap ? 100.0 * ap_exact / ap : 0.0);
  Check(fp_ok + fp_unknown == fp && fp_unknown * 100 < fp, "replay: every MmFreePhysicalMemory of an allocation seen in the trace finds it", std::to_string(fp_ok) + "/" + std::to_string(fp) + " (" + std::to_string(fp_unknown) + " of memory the trace never saw allocated)");
  Check(av_ok == av, "replay: the NtAllocateVirtualMemory ranges the runtime handed out are accepted as fixed allocations", std::to_string(av_ok) + "/" + std::to_string(av));
  std::printf("    NtFreeVirtualMemory replayed: %llu/%llu; MmQueryAllocationSize agrees: %llu/%llu\n", (unsigned long long)fv_ok, (unsigned long long)fv, (unsigned long long)qs_ok, (unsigned long long)qs);
  Check(qs_ok == qs, "replay: MmQueryAllocationSize returns what the runtime returned");
}
}  // namespace

int main(int argc, char** argv) {
  std::string error;
  auto m = Memory::Create(&error);
  Check(m != nullptr, "the 4.5 GB reservation, the memfd and the windows are created", error);
  if (!m) return 1;
  Basics(*m);
  Stacks(*m);
  Stress(*m);
  if (argc > 2 && !std::strcmp(argv[1], "--replay")) {
    auto fresh = Memory::Create(&error);
    Replay(*fresh, argv[2]);
  }
  std::printf("%s\n", g_failures == 0 ? "PASS" : "FAIL");
  return g_failures == 0 ? 0 : 1;
}
