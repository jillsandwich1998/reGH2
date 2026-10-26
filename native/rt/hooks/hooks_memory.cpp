// Memory hooks (moved from app/src/gh2_host.cpp): allocation timing and the guest memcpy. No SDK dependency.
#include "gh2_pch.h"  // PPCContext, REX_EXTERN

#include <gh2rt/boundary_trace.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "hooks_internal.h"

using gh2rt::hooks::SinceStart;

// Allocation recording (GH2_MEM_TRACE=<file>): every call of the guest's memory imports with its arguments and results, one text line each, for the replay test of
// the native memory manager (kernel/mem, gh2_mem_selftest --replay). Lines: "AP flags size prot min max align -> addr" (MmAllocatePhysicalMemoryEx), "FP type addr"
// (MmFreePhysicalMemory), "AV addr_in size_in type prot -> addr size" (NtAllocateVirtualMemory), "FV addr size type -> addr size" (NtFreeVirtualMemory), "QS addr -> size".
namespace {
FILE* MemTrace() {
  static FILE* f = [] {
    const char* p = getenv("GH2_MEM_TRACE");
    return p && *p ? fopen(p, "w") : nullptr;
  }();
  return f;
}
std::mutex g_mem_trace_mu;
}  // namespace
REX_EXTERN(__real___imp__NtAllocateVirtualMemory);
REX_EXTERN(__real___imp__NtFreeVirtualMemory);
REX_EXTERN(__real___imp__MmFreePhysicalMemory);
REX_EXTERN(__real___imp__MmQueryAllocationSize);

REX_EXTERN(__wrap___imp__NtAllocateVirtualMemory) {
  GH2_TRACE_HOOK("NtAllocateVirtualMemory");
  FILE* t = MemTrace();
  const uint32_t pb = ctx.r3.u32, ps = ctx.r4.u32, type = ctx.r5.u32, prot = ctx.r6.u32;
  const uint32_t in_base = t && pb ? REX_LOAD_U32(pb) : 0, in_size = t && ps ? REX_LOAD_U32(ps) : 0;
  __real___imp__NtAllocateVirtualMemory(ctx, base);
  if (t) {
    std::lock_guard<std::mutex> lock(g_mem_trace_mu);
    fprintf(t, "AV %x %x %x %x -> %x %x status %x\n", in_base, in_size, type, prot, pb ? REX_LOAD_U32(pb) : 0, ps ? REX_LOAD_U32(ps) : 0, ctx.r3.u32);
  }
}
REX_EXTERN(__wrap___imp__NtFreeVirtualMemory) {
  GH2_TRACE_HOOK("NtFreeVirtualMemory");
  FILE* t = MemTrace();
  const uint32_t pb = ctx.r3.u32, ps = ctx.r4.u32, type = ctx.r5.u32;
  const uint32_t in_base = t && pb ? REX_LOAD_U32(pb) : 0, in_size = t && ps ? REX_LOAD_U32(ps) : 0;
  __real___imp__NtFreeVirtualMemory(ctx, base);
  if (t) {
    std::lock_guard<std::mutex> lock(g_mem_trace_mu);
    fprintf(t, "FV %x %x %x -> %x %x status %x\n", in_base, in_size, type, pb ? REX_LOAD_U32(pb) : 0, ps ? REX_LOAD_U32(ps) : 0, ctx.r3.u32);
  }
}
REX_EXTERN(__wrap___imp__MmFreePhysicalMemory) {
  GH2_TRACE_HOOK("MmFreePhysicalMemory");
  FILE* t = MemTrace();
  if (t) {
    std::lock_guard<std::mutex> lock(g_mem_trace_mu);
    fprintf(t, "FP %x %x\n", ctx.r3.u32, ctx.r4.u32);
  }
  __real___imp__MmFreePhysicalMemory(ctx, base);
}
REX_EXTERN(__wrap___imp__MmQueryAllocationSize) {
  GH2_TRACE_HOOK("MmQueryAllocationSize");
  const uint32_t addr = ctx.r3.u32;
  __real___imp__MmQueryAllocationSize(ctx, base);
  if (FILE* t = MemTrace()) {
    std::lock_guard<std::mutex> lock(g_mem_trace_mu);
    fprintf(t, "QS %x -> %x\n", addr, ctx.r3.u32);
  }
}

// Allocation timing (GH2_ALLOC_TRACE=1): cost of MmAllocatePhysicalMemoryEx.
REX_EXTERN(__real___imp__MmAllocatePhysicalMemoryEx);

REX_EXTERN(__wrap___imp__MmAllocatePhysicalMemoryEx) {
  GH2_TRACE_HOOK("MmAllocatePhysicalMemoryEx");
  static const bool on = getenv("GH2_ALLOC_TRACE") != nullptr;
  if (FILE* t = MemTrace()) {
    const uint32_t a[6] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
    __real___imp__MmAllocatePhysicalMemoryEx(ctx, base);
    std::lock_guard<std::mutex> lock(g_mem_trace_mu);
    fprintf(t, "AP %x %x %x %x %x %x -> %x\n", a[0], a[1], a[2], a[3], a[4], a[5], ctx.r3.u32);
    return;
  }
  if (!on) {
    __real___imp__MmAllocatePhysicalMemoryEx(ctx, base);
    return;
  }
  static std::atomic<uint64_t> calls{0}, total_us{0}, max_us{0}, bytes{0};
  auto t0 = std::chrono::steady_clock::now();
  __real___imp__MmAllocatePhysicalMemoryEx(ctx, base);
  uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
  uint64_t n = ++calls;
  total_us += us;
  bytes += ctx.r5.u32;
  if (us > max_us) max_us = us;
  if (n % 500 == 0 || us > 20000)
    fprintf(stderr, "ALLOC n=%llu total=%.1fms max=%.2fms last=%lluus size_arg=%x\n", (unsigned long long)n, total_us / 1000.0,
            max_us / 1000.0, (unsigned long long)us, ctx.r5.u32);
}

// Guest memcpy (sub_826A85E8: dst=r3, src=r4, n=r5, returns dst). The recompiled version copies with lvx/vperm/stvx loops; host
// memmove is much faster for the big copies done while loading a song. GH2_COPY_TRACE=1 logs slow copies; GH2_FAST_MEMCPY=0 keeps
// the recompiled loop.
REX_EXTERN(__imp__sub_826A85E8);

REX_EXTERN(sub_826A85E8) {
  static const bool fast = [] {
    const char* e = getenv("GH2_FAST_MEMCPY");
    return !(e && e[0] == '0');
  }();
  static const bool trace = getenv("GH2_COPY_TRACE") != nullptr;
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, n = ctx.r5.u32;
  const auto t0 = std::chrono::steady_clock::now();
  if (fast) {
    std::memmove(base + dst, base + src, n);
    ctx.r3.u32 = dst;
  } else {
    __imp__sub_826A85E8(ctx, base);
  }
  if (trace) {
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    static std::atomic<uint64_t> calls{0}, bytes{0};
    static std::atomic<uint64_t> total_us{0};
    const uint64_t c = ++calls;
    bytes += n;
    total_us += static_cast<uint64_t>(ms * 1000);
    if (ms > 2.0) fprintf(stderr, "T+%.2fs COPY %u bytes took %.1f ms (%s)\n", SinceStart(), n, ms, fast ? "host" : "guest loop");
    if (c % 2000 == 0)
      fprintf(stderr, "T+%.2fs COPYSTAT calls=%llu bytes=%.1f MB total=%.1f ms\n", SinceStart(), (unsigned long long)c,
              bytes.load() / 1048576.0, total_us.load() / 1000.0);
  }
}
