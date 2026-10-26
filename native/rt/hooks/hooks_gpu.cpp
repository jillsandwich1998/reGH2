// The GPU, natively (Phase 3): with --gpu=native the runtime has no graphics system and the three kernel imports that feed one (VdInitializeRingBuffer, VdEnableRingBufferRPtrWriteBack,
// VdSetGraphicsInterruptCallback) and the MMIO window of the GPU registers (0x7FC80000) are answered here, by gh2gpu::CommandProcessor.
//
// Threads: the command processor and the vertical-blank clock each run on a native guest thread (gh2k::thread), because the game's graphics interrupt callback is guest code that must run
// on a thread with a PCR, a KTHREAD and a context. The callback is dispatched the way the runtime did it: under the global critical region ("interrupts disabled"), with the TLS pointer
// zeroed (some games test it to know they are in an interrupt), the stack padded by 176 bytes and LR = 0xBCBCBCBC.
// What draws (the Sink) is chosen in gpu_renderer_install (the Vulkan renderer when present, otherwise a counting null sink: the game runs, nothing is drawn).
// No SDK dependency.
#include "gh2_pch.h"  // PPCContext, REX_EXTERN

#include <gh2abi/abi.h>
#include <gh2gpu/command_processor.h>
#include <gh2k/mem.h>
#include <gh2rt/boundary_trace.h>
#include <gh2rt/host_services.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <unistd.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "gpu_renderer.h"
#include "hooks_internal.h"

REX_EXTERN(__real___imp__VdInitializeRingBuffer);
REX_EXTERN(__real___imp__VdEnableRingBufferRPtrWriteBack);
REX_EXTERN(__real___imp__VdSetGraphicsInterruptCallback);
REX_EXTERN(__real___imp__VdQueryVideoMode);
REX_EXTERN(__real___imp__KeQueryPerformanceFrequency);

namespace {

struct Gpu {
  std::unique_ptr<gh2gpu::Sink> sink;
  std::unique_ptr<gh2gpu::CommandProcessor> cp;
  std::atomic<uint32_t> interrupt_callback{0}, interrupt_user{0};
  std::atomic<bool> stop{false};
  uint64_t tick_frequency = 50000000;
  std::once_flag started;
  std::atomic<uint64_t> vblanks{0};
};
Gpu& G() {
  static Gpu& g = *new Gpu;   // never destroyed: its threads may still run when the process exits
  return g;
}

thread_local PPCContext* tl_ctx = nullptr;   // the context of the native guest thread this host thread is
thread_local uint8_t* tl_base = nullptr;

bool Active() {
  static const bool on = [] {
    std::string mode = gh2rt::Host().GetSettings().gpu;
    if (const char* e = getenv("GH2_GPU"); e && *e) mode = e;
    if (mode != "native") return false;
    if (!gh2rt::hooks::detail::NativeThreadsActive()) {
      fprintf(stderr, "GPU native needs --threads=native: staying with the runtime's\n");
      return false;
    }
    fprintf(stderr, "GPU native: gh2gpu executes the command stream (no graphics system in the runtime)\n");
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

// The graphics interrupt callback: guest code on the calling native thread.
void DispatchInterrupt(uint32_t source, uint32_t cpu) {
  Gpu& g = G();
  const uint32_t callback = g.interrupt_callback.load();
  if (!callback || !tl_ctx) return;
  PPCContext& ctx = *tl_ctx;
  uint8_t* base = tl_base;
  PPCFunc* fn = gh2abi::ResolveIndirectFunction(callback);
  if (!fn) return;
  std::unique_lock<std::recursive_mutex> lock(gh2abi::global_critical_region::mutex());   // held for the whole callback, as the runtime does
  const uint32_t pcr = ctx.r13.u32;
  const uint32_t kthread = Be32(base, pcr + 0x100);
  base[kthread + 0xBF] = static_cast<uint8_t>(cpu);   // the thread now "runs on" this hardware thread
  base[pcr + 0x10C] = static_cast<uint8_t>(cpu);
  const uint32_t old_tls = Be32(base, pcr);
  PutBe32(base, pcr, 0);                              // the TLS pointer is zero in an interrupt
  const uint64_t sp = ctx.r1.u64, lr = ctx.lr;
  ctx.r3.u64 = source;
  ctx.r4.u64 = g.interrupt_user.load();
  ctx.r1.u64 = sp - (64 + 112);
  ctx.lr = 0xBCBCBCBC;
  fn(ctx, base);
  ctx.lr = lr;
  ctx.r1.u64 = sp;
  PutBe32(base, pcr, old_tls);
}

void StartThreads(PPCContext& ctx, uint8_t* base) {
  Gpu& g = G();
  // the display size and the guest clock frequency, from the runtime's own answers
  {
    PPCContext tmp = ctx;
    const uint32_t scratch = (ctx.r1.u32 - 0x100) & ~0xFu;
    tmp.r3.u64 = scratch;
    __real___imp__VdQueryVideoMode(tmp, base);
    const uint32_t w = Be32(base, scratch), h = Be32(base, scratch + 4);
    if (w && h) g.cp->SetDisplaySize(w, h);
    PPCContext tmp2 = ctx;
    __real___imp__KeQueryPerformanceFrequency(tmp2, base);
    if (tmp2.r3.u32) g.tick_frequency = tmp2.r3.u32;
  }
  gh2rt::hooks::detail::StartGuestHostThread(
      base, ctx.r13.u32, 0x40000, "gh2-gpu", 0, [](PPCContext& tctx, uint8_t* tbase) {
        tl_ctx = &tctx;
        tl_base = tbase;
        G().cp->Run();
      });
  gh2rt::hooks::detail::StartGuestHostThread(
      base, ctx.r13.u32, 0x40000, "gh2-vblank", 0, [](PPCContext& tctx, uint8_t* tbase) {
        tl_ctx = &tctx;
        tl_base = tbase;
        Gpu& gpu = G();
        static const double hz = std::getenv("GH2_VBLANK_HZ") ? std::max(20.0, std::min(480.0, std::atof(std::getenv("GH2_VBLANK_HZ")))) : 60.0;   // experiment: the vertical blank clock the game paces itself on
        const uint64_t interval = static_cast<uint64_t>(gpu.tick_frequency / hz);   // 60 Hz on the guest clock (it runs faster during the intro fast-forward)
        uint64_t last = gh2abi::Clock::QueryGuestTickCount();
        while (!gpu.stop.load()) {
          gh2rt::hooks::detail::ParkIfQuiescing();
          uint64_t now = gh2abi::Clock::QueryGuestTickCount();
          while (now - last >= interval && !gpu.stop.load()) {
            gpu.cp->NoteVblank();
            ++gpu.vblanks;
            DispatchInterrupt(0, 2);
            last += interval;
            if (now - last > interval * 10) last = now;   // never try to catch up on a long stall
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      });
  // a line every 10 s with what the processor did
  std::thread([] {
    for (int n = 1;; ++n) {
      std::this_thread::sleep_for(std::chrono::seconds(10));
      Gpu& gpu = G();
      const gh2gpu::Stats& s = gpu.cp->stats();
      fprintf(stderr,
              "GPU T+%ds ring %llu dwords | packets t0 %llu t3 %llu | draws %llu swaps %llu fences %llu interrupts %llu vblanks %llu | indirect %llu shader loads %llu | wait_reg_mem %llu (blocked %llu, %.1f ms) | "
              "busy %.1f%% idle waits %llu | bad %llu unknown %llu\n",
              n * 10, (unsigned long long)s.ring_dwords.load(), (unsigned long long)s.packets_type0.load(), (unsigned long long)s.packets_type3.load(), (unsigned long long)s.draws.load(),
              (unsigned long long)s.swaps.load(), (unsigned long long)s.fences.load(), (unsigned long long)s.interrupts.load(), (unsigned long long)gpu.vblanks.load(),
              (unsigned long long)s.indirect_buffers.load(), (unsigned long long)s.shader_loads.load(), (unsigned long long)s.wait_reg_mem.load(), (unsigned long long)s.wait_reg_mem_blocked.load(),
              s.wait_reg_mem_ns.load() / 1e6, 100.0 * s.busy_ns.load() / (n * 10e9), (unsigned long long)s.idle_waits.load(), (unsigned long long)s.bad_packets.load(),
              (unsigned long long)s.unknown_opcodes.load());
      fprintf(stderr, "GPU T+%ds command processor wakes: %llu, ring dwords ready per wake avg %.0f max %llu\n", n * 10, (unsigned long long)s.batches.load(),
              s.batches.load() ? double(s.batch_dwords_sum.load()) / double(s.batches.load()) : 0.0, (unsigned long long)s.batch_dwords_max.load());
      fprintf(stderr, "GPU T+%ds packets by opcode: IM_LOAD %llu, IM_LOAD_IMMEDIATE %llu, LOAD_ALU_CONSTANT %llu, SET_CONSTANT %llu, DRAW_INDX %llu, DRAW_INDX_2 %llu, INDIRECT_BUFFER %llu\n", n * 10,
              (unsigned long long)s.type3_by_opcode[0x27].load(), (unsigned long long)s.type3_by_opcode[0x2B].load(), (unsigned long long)s.type3_by_opcode[0x2F].load(), (unsigned long long)s.type3_by_opcode[0x2D].load(),
              (unsigned long long)s.type3_by_opcode[0x22].load(), (unsigned long long)s.type3_by_opcode[0x36].load(), (unsigned long long)s.type3_by_opcode[0x3F].load());
      {   // the process's own memory, for the long sessions (a slow climb that never levels off is a leak): resident set, virtual size, threads, open files
        unsigned long pages_total = 0, pages_resident = 0;
        if (FILE* f = fopen("/proc/self/statm", "r")) {
          if (fscanf(f, "%lu %lu", &pages_total, &pages_resident) != 2) pages_total = pages_resident = 0;
          fclose(f);
        }
        int threads = 0;
        if (FILE* f = fopen("/proc/self/status", "r")) {
          char line[256];
          while (fgets(line, sizeof line, f))
            if (sscanf(line, "Threads: %d", &threads) == 1) break;
          fclose(f);
        }
        int fds = 0;
        if (DIR* d = opendir("/proc/self/fd")) {
          while (readdir(d)) ++fds;
          closedir(d);
        }
        const double page_mb = sysconf(_SC_PAGESIZE) / 1048576.0;
        fprintf(stderr, "PROC T+%ds resident %.0f MB, virtual %.0f MB, %d threads, %d open files\n", n * 10, pages_resident * page_mb, pages_total * page_mb, threads, fds > 2 ? fds - 2 : 0);
      }
      const std::string render = gh2rt::gpu::RendererStatsLine();
      if (!render.empty()) fprintf(stderr, "GPU T+%ds %s\n", n * 10, render.c_str());
      if (const char* r = getenv("GH2_GPU_REPORT"); r && *r)
        if (FILE* f = fopen(r, "w")) {
          fprintf(f, "{\"seconds\": %d, \"ring_dwords\": %llu, \"draws\": %llu, \"swaps\": %llu, \"fences\": %llu, \"interrupts\": %llu, \"vblanks\": %llu, \"indirect_buffers\": %llu, \"shader_loads\": %llu, "
                     "\"wait_reg_mem\": %llu, \"wait_reg_mem_blocked\": %llu, \"wait_reg_mem_ms\": %.1f, \"busy_percent\": %.2f, \"bad_packets\": %llu, \"unknown_opcodes\": %llu, \"type3_by_opcode\": {",
                  n * 10, (unsigned long long)s.ring_dwords.load(), (unsigned long long)s.draws.load(), (unsigned long long)s.swaps.load(), (unsigned long long)s.fences.load(),
                  (unsigned long long)s.interrupts.load(), (unsigned long long)gpu.vblanks.load(), (unsigned long long)s.indirect_buffers.load(), (unsigned long long)s.shader_loads.load(),
                  (unsigned long long)s.wait_reg_mem.load(), (unsigned long long)s.wait_reg_mem_blocked.load(), s.wait_reg_mem_ns.load() / 1e6, 100.0 * s.busy_ns.load() / (n * 10e9),
                  (unsigned long long)s.bad_packets.load(), (unsigned long long)s.unknown_opcodes.load());
          bool first = true;
          for (int op = 0; op < 128; ++op)
            if (s.type3_by_opcode[op].load()) {
              fprintf(f, "%s\"0x%02X\": %llu", first ? "" : ", ", op, (unsigned long long)s.type3_by_opcode[op].load());
              first = false;
            }
          fprintf(f, "}}\n");
          fclose(f);
        }
    }
  }).detach();
}

// Everything is created on first use, from the first of the three imports the game calls (VdInitializeRingBuffer or VdSetGraphicsInterruptCallback).
void EnsureStarted(PPCContext& ctx, uint8_t* base) {
  Gpu& g = G();
  std::call_once(g.started, [&] {
    gh2k::mem::Memory* memory = gh2k::mem::GlobalMemory();
    gh2gpu::GuestMemory guest;
    guest.virt = base;
    guest.phys = memory->base() + 0x100000000ull;
    g.sink = gh2rt::gpu::CreateSink(guest);
    gh2gpu::Callbacks callbacks;
    callbacks.interrupt = [](uint32_t source, uint32_t cpu) { DispatchInterrupt(source, cpu); };
    g.cp = std::make_unique<gh2gpu::CommandProcessor>(guest, g.sink.get(), callbacks);
    gh2abi::NativeMmio mmio;
    mmio.user = &G();
    mmio.load = [](void* user, uint32_t address, uint32_t* value) { return static_cast<Gpu*>(user)->cp->MmioRead(address, value); };
    mmio.store = [](void* user, uint32_t address, uint32_t value) { return static_cast<Gpu*>(user)->cp->MmioWrite(address, value); };
    gh2abi::SetNativeMmio(mmio);
    StartThreads(ctx, base);
  });
}

}  // namespace

namespace gh2rt::hooks::detail {
void StopGpu() {
  Gpu& g = G();
  g.stop.store(true);
  if (g.cp) g.cp->RequestStop();
}
}  // namespace gh2rt::hooks::detail

// VdInitializeRingBuffer(physical address of the ring, log2 of its size in quadwords)
REX_EXTERN(__wrap___imp__VdInitializeRingBuffer) {
  GH2_TRACE_HOOK("VdInitializeRingBuffer");
  if (!Active()) { __real___imp__VdInitializeRingBuffer(ctx, base); return; }
  EnsureStarted(ctx, base);
  G().cp->InitializeRingBuffer(ctx.r3.u32, ctx.r4.u32);
}

// VdEnableRingBufferRPtrWriteBack(physical address for the read pointer, log2 of the block size)
REX_EXTERN(__wrap___imp__VdEnableRingBufferRPtrWriteBack) {
  GH2_TRACE_HOOK("VdEnableRingBufferRPtrWriteBack");
  if (!Active()) { __real___imp__VdEnableRingBufferRPtrWriteBack(ctx, base); return; }
  EnsureStarted(ctx, base);
  G().cp->EnableReadPointerWriteBack(ctx.r3.u32, ctx.r4.u32);
}

// VdSetGraphicsInterruptCallback(callback, user data): the callback takes (source, user data)
REX_EXTERN(__wrap___imp__VdSetGraphicsInterruptCallback) {
  GH2_TRACE_HOOK("VdSetGraphicsInterruptCallback");
  if (!Active()) { __real___imp__VdSetGraphicsInterruptCallback(ctx, base); return; }
  EnsureStarted(ctx, base);
  G().interrupt_callback.store(ctx.r3.u32);
  G().interrupt_user.store(ctx.r4.u32);
}
