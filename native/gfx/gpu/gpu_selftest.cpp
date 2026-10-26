// gh2_gpu_selftest: the command processor against hand-built PM4 streams in a fake guest physical memory.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "gh2gpu/command_processor.h"

using namespace gh2gpu;

static int g_failures = 0;
#define CHECK(cond, what)                                      \
  do {                                                         \
    if (!(cond)) {                                             \
      std::printf("FAIL: %s (line %d)\n", what, __LINE__);     \
      ++g_failures;                                            \
    } else {                                                   \
      std::printf("ok:   %s\n", what);                         \
    }                                                          \
  } while (0)

namespace {

constexpr uint32_t kRingPhysical = 0x1000, kRingLog2 = 9;   // 1 << (9 + 3) = 4096 bytes = 1024 dwords
constexpr uint32_t kRingDwords = 1024;
constexpr uint32_t kRptrAddress = 0x100;

std::vector<uint8_t> g_mem(1 << 20);

uint32_t Header3(uint32_t opcode, uint32_t count) { return (3u << 30) | ((count - 1) << 16) | (opcode << 8); }
uint32_t Header0(uint32_t first, uint32_t count) { return ((count - 1) << 16) | first; }
void PutBe(uint32_t physical, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(&g_mem[physical], &v, 4);
}
uint32_t GetBe(uint32_t physical) {
  uint32_t v;
  std::memcpy(&v, &g_mem[physical], 4);
  return __builtin_bswap32(v);
}
uint32_t GetRaw(uint32_t physical) {
  uint32_t v;
  std::memcpy(&v, &g_mem[physical], 4);
  return v;
}

struct Recorder : Sink {
  std::mutex mu;
  std::vector<DrawParams> draws;
  std::vector<uint32_t> swaps_front, swaps_w, swaps_h;
  std::vector<std::vector<uint32_t>> shader_loads;
  uint32_t const0_at_draw = 0, fetch0_at_draw = 0;
  void OnShaderLoad(uint32_t type, uint32_t physical, uint32_t size) override {
    std::lock_guard<std::mutex> lock(mu);
    shader_loads.push_back({type, physical, size});
  }
  void OnDraw(const uint32_t* regs, const DrawParams& d) override {
    std::lock_guard<std::mutex> lock(mu);
    draws.push_back(d);
    const0_at_draw = regs[reg::SHADER_CONSTANT_000_X + 4];
    fetch0_at_draw = regs[reg::SHADER_CONSTANT_FETCH_00_0 + 1];
  }
  void OnSwap(const uint32_t*, uint32_t front, uint32_t w, uint32_t h) override {
    std::lock_guard<std::mutex> lock(mu);
    swaps_front.push_back(front);
    swaps_w.push_back(w);
    swaps_h.push_back(h);
  }
};

// writes packets into the ring and publishes the write pointer; returns the new write index
struct RingWriter {
  CommandProcessor& cp;
  uint32_t write = 0;
  void Dword(uint32_t v) {
    PutBe(kRingPhysical + 4 * write, v);
    write = (write + 1) % kRingDwords;
  }
  void Publish() {
    uint32_t addr = kMmioBase + 4 * reg::CP_RB_WPTR;
    cp.MmioWrite(addr, write);
  }
};

bool WaitFor(const std::function<bool()>& f, int ms = 3000) {
  const auto t0 = std::chrono::steady_clock::now();
  while (!f()) {
    if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(ms)) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

}  // namespace

int main() {
  GuestMemory memory;
  memory.virt = g_mem.data();
  memory.phys = g_mem.data();
  Recorder sink;
  std::atomic<int> interrupts_source0{0}, interrupts_source1{0};
  std::atomic<uint32_t> cpus_seen{0};
  Callbacks callbacks;
  callbacks.interrupt = [&](uint32_t source, uint32_t cpu) {
    (source == 0 ? interrupts_source0 : interrupts_source1)++;
    cpus_seen |= 1u << cpu;
  };
  CommandProcessor cp(memory, &sink, callbacks);
  cp.InitializeRingBuffer(kRingPhysical, kRingLog2);
  cp.EnableReadPointerWriteBack(kRptrAddress, 6);
  std::thread processor([&] { cp.Run(); });
  RingWriter w{cp};

  // ---- registers through the MMIO window ----
  uint32_t v = 0;
  CHECK(cp.MmioRead(kMmioBase + 4 * reg::RB_EDRAM_TIMING, &v) && v == 0x08100748u, "RB_EDRAM_TIMING reads the fixed value");
  CHECK(cp.MmioRead(kMmioBase + 4 * reg::D1MODE_V_COUNTER, &v) && v == 720, "the vertical counter reports the display height");
  CHECK(cp.MmioRead(kMmioBase + 4 * reg::D1MODE_VIEWPORT_SIZE, &v) && v == ((1280u << 16) | 720u), "the viewport size register packs width and height");
  CHECK(!cp.MmioRead(0x7FD00000, &v), "an address outside the window is not handled");
  cp.MmioWrite(kMmioBase + 4 * 0x1234, 0xCAFE);
  CHECK(cp.MmioRead(kMmioBase + 4 * 0x1234, &v) && v == 0xCAFE, "other registers store what is written");

  // ---- a fence, an interrupt and a state write, then check the read pointer comes back ----
  constexpr uint32_t kFence = 0x2000;
  w.Dword(Header3(pm4::EVENT_WRITE_SHD, 3)); w.Dword(0); w.Dword(kFence | 2); w.Dword(0x11223344);   // address endian 2 = 8in32
  w.Dword(Header3(pm4::INTERRUPT, 1)); w.Dword(0x05);                                                    // cpus 0 and 2
  w.Dword(Header0(0x0578, 1)); w.Dword(0xABCD);                                                          // SCRATCH_REG0 (write-back disabled: nothing in memory)
  w.Publish();
  CHECK(WaitFor([&] { return GetBe(kFence) == 0x11223344u; }), "EVENT_WRITE_SHD stores the fence value (8in32: the guest reads it big endian)");
  CHECK(WaitFor([&] { return interrupts_source1.load() == 2; }) && cpus_seen.load() == 0x5, "INTERRUPT calls the callback once per cpu bit (0 and 2) with source 1");
  CHECK(WaitFor([&] { return GetBe(kRptrAddress) == w.write; }), "the read pointer is written back (big endian) when the stream is consumed");

  // ---- a WAIT_REG_MEM that blocks until the CPU writes memory ----
  constexpr uint32_t kFlag = 0x3000;
  PutBe(kFlag, 0);
  w.Dword(Header3(pm4::WAIT_REG_MEM, 5)); w.Dword(0x13 /* equal + memory */); w.Dword(kFlag | 2); w.Dword(0xBEEF); w.Dword(0xFFFFFFFF); w.Dword(0x100);
  w.Dword(Header3(pm4::EVENT_WRITE_SHD, 3)); w.Dword(0); w.Dword(kFence | 2); w.Dword(0x55667788);
  w.Publish();
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  CHECK(GetBe(kFence) == 0x11223344u, "a blocked WAIT_REG_MEM holds back the packets behind it");
  CHECK(cp.stats().wait_reg_mem_blocked.load() >= 1, "the wait is counted as blocked");
  PutBe(kFlag, 0xBEEF);   // the CPU writes the flag (big endian in guest memory)
  CHECK(WaitFor([&] { return GetBe(kFence) == 0x55667788u; }), "writing the flag releases the wait and the next fence is stored");

  // ---- state packets, shader load and a draw ----
  w.Dword(Header3(pm4::SET_CONSTANT, 3)); w.Dword((0u << 16) | 4); w.Dword(0x3F800000); w.Dword(0x40000000);   // ALU constants 4,5
  w.Dword(Header3(pm4::SET_CONSTANT, 3)); w.Dword((1u << 16) | 1); w.Dword(0xAAAA0001); w.Dword(0xBBBB0002);   // fetch constants dwords 1,2
  w.Dword(Header3(pm4::IM_LOAD, 2)); w.Dword(0x0004A000 | 1); w.Dword((0u << 16) | 120);                       // pixel shader at physical 0x4A000, 120 dwords
  w.Dword(Header3(pm4::DRAW_INDX, 4)); w.Dword(0); w.Dword((6u) | (0u << 6) | (1u << 11) | (36u << 16)); w.Dword(0x5000 | 0); w.Dword((2u << 30) | 36);   // triangle strip(6), DMA, 32-bit, 36 indices, 8in32
  w.Dword(Header3(pm4::DRAW_INDX_2, 1)); w.Dword((4u) | (2u << 6) | (3u << 16));                                  // triangle list, auto index, 3 indices
  w.Publish();
  CHECK(WaitFor([&] { std::lock_guard<std::mutex> l(sink.mu); return sink.draws.size() == 2; }), "two draws reach the sink");
  {
    std::lock_guard<std::mutex> l(sink.mu);
    CHECK(sink.const0_at_draw == 0x3F800000u, "SET_CONSTANT type ALU writes the float constant file (register 0x4004)");
    CHECK(sink.fetch0_at_draw == 0xAAAA0001u, "SET_CONSTANT type FETCH writes the fetch constants (register 0x4801)");
    CHECK(sink.shader_loads.size() == 1 && sink.shader_loads[0][0] == 1 && sink.shader_loads[0][1] == 0x4A000 && sink.shader_loads[0][2] == 120, "IM_LOAD reports (pixel, physical address, size)");
    const DrawParams& d0 = sink.draws[0];
    CHECK(d0.prim_type == 6 && d0.source_select == 0 && d0.index32 && d0.num_indices == 36 && d0.dma_base == 0x5000 && d0.dma_size_bytes == 36 * 4 && d0.dma_endian == Endian::k8in32, "DRAW_INDX decodes primitive, DMA base, size and endian");
    const DrawParams& d1 = sink.draws[1];
    CHECK(d1.prim_type == 4 && d1.source_select == 2 && d1.num_indices == 3, "DRAW_INDX_2 with auto index");
  }

  // ---- an indirect buffer, memory-to-register copies, conditional write, register read-modify-write ----
  constexpr uint32_t kIb = 0x6000;
  PutBe(kIb + 0, Header3(pm4::MEM_WRITE, 3)); PutBe(kIb + 4, 0x7000 | 2); PutBe(kIb + 8, 0x01020304); PutBe(kIb + 12, 0x0A0B0C0D);
  w.Dword(Header3(pm4::INDIRECT_BUFFER, 2)); w.Dword(kIb); w.Dword(4);
  w.Dword(Header3(pm4::REG_RMW, 3)); w.Dword(0x1100); w.Dword(0x0000FFFF); w.Dword(0x00010000);   // regs[0x1100] = (regs[0x1100] & 0xFFFF) | 0x10000
  w.Dword(Header3(pm4::REG_TO_MEM, 2)); w.Dword(0x1100); w.Dword(0x7100 | 2);
  cp.MmioWrite(kMmioBase + 4 * 0x1100, 0xFFFF1234);
  w.Publish();
  CHECK(WaitFor([&] { return GetBe(0x7000) == 0x01020304u && GetBe(0x7004) == 0x0A0B0C0Du; }), "an INDIRECT_BUFFER runs its MEM_WRITE (endian-swapped words)");
  CHECK(cp.stats().indirect_buffers.load() == 1, "one indirect buffer counted");
  CHECK(WaitFor([&] { return GetBe(0x7100) == 0x00011234u; }), "REG_RMW then REG_TO_MEM: the register (0x10000 | low half) ends in memory");

  // ---- COHER_STATUS_HOST: writing it makes it dirty; waiting on it makes the memory coherent and clears it ----
  w.Dword(Header0(reg::COHER_STATUS_HOST, 1)); w.Dword(0x01000000);
  w.Dword(Header3(pm4::WAIT_REG_MEM, 5)); w.Dword(0x3 /* equal, register */); w.Dword(reg::COHER_STATUS_HOST); w.Dword(0); w.Dword(0x80000000); w.Dword(0x100);
  w.Dword(Header3(pm4::EVENT_WRITE_SHD, 3)); w.Dword(0); w.Dword(kFence | 2); w.Dword(0xC0DE0001);
  w.Publish();
  CHECK(WaitFor([&] { return GetBe(kFence) == 0xC0DE0001u; }), "a WAIT_REG_MEM on COHER_STATUS_HOST completes (the dirty flag is cleared by MakeCoherent)");

  // ---- the swap packet ----
  w.Dword(Header3(pm4::XE_SWAP, 4)); w.Dword(pm4::kSwapSignature); w.Dword(0x1B00000); w.Dword(1280); w.Dword(720);
  for (int i = 0; i < 60; ++i) w.Dword(1u << 31);   // the rest of VdSwap's 64 dwords are type-2 no-ops
  w.Publish();
  CHECK(WaitFor([&] { std::lock_guard<std::mutex> l(sink.mu); return sink.swaps_front.size() == 1; }), "XE_SWAP reaches the sink");
  {
    std::lock_guard<std::mutex> l(sink.mu);
    CHECK(sink.swaps_front[0] == 0x1B00000 && sink.swaps_w[0] == 1280 && sink.swaps_h[0] == 720, "with the front buffer address and size");
  }
  CHECK(cp.frame_counter() == 1, "the swap counter advances");

  // ---- a fence that carries the swap counter, occlusion query results and the screen extents ----
  w.Dword(Header3(pm4::EVENT_WRITE_SHD, 3)); w.Dword(1u << 31); w.Dword(kFence | 2); w.Dword(0);
  PutBe(0x8000 + 0, 0);
  cp.MmioWrite(kMmioBase + 4 * reg::RB_SAMPLE_COUNT_ADDR, 0x8000);
  std::memset(&g_mem[0x8000], 0, 32);
  { uint32_t fin = __builtin_bswap32(0xFFFFFEEDu); std::memcpy(&g_mem[0x8000 + 16], &fin, 4); }   // ZPass_A holds the "query finished" marker
  w.Dword(Header3(pm4::EVENT_WRITE_ZPD, 1)); w.Dword(0);
  w.Dword(Header3(pm4::EVENT_WRITE_EXT, 2)); w.Dword(0); w.Dword(0x8100 | 1);
  w.Publish();
  CHECK(WaitFor([&] { return GetBe(kFence) == 1u; }), "EVENT_WRITE_SHD with bit 31 writes the swap counter");
  CHECK(WaitFor([&] { return GetRaw(0x8000 + 16) == 1000u; }), "a finished occlusion query reports the fake 1000 samples");
  CHECK(WaitFor([&] { return g_mem[0x8100 + 2] == 0x04; }), "(extents written)");
  CHECK(g_mem[0x8100 + 2] == 0x04 && g_mem[0x8100 + 3] == 0x00, "EVENT_WRITE_EXT stores the 8192-pixel extent as 1024 big endian");

  // ---- ring wrap: push more than one ring of NOPs and fences ----
  uint32_t last_fence = 0;
  for (int round = 0; round < 6; ++round) {
    for (int i = 0; i < 100; ++i) w.Dword(Header3(pm4::NOP, 2)), w.Dword(0xDEAD), w.Dword(0xBEEF);
    last_fence = 0xF00D0000u + round;
    w.Dword(Header3(pm4::EVENT_WRITE_SHD, 3)); w.Dword(0); w.Dword(kFence | 2); w.Dword(last_fence);
    w.Publish();
    CHECK(WaitFor([&] { return GetBe(kFence) == last_fence; }), "ring wrap round");
  }
  CHECK(cp.stats().bad_packets.load() == 0 && cp.stats().unknown_opcodes.load() == 0, "no malformed or unknown packets in the whole run");

  cp.RequestStop();
  processor.join();
  std::printf("%s\n", g_failures == 0 ? "PASS" : "FAIL");
  return g_failures == 0 ? 0 : 1;
}
