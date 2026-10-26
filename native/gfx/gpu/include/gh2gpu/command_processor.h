// gh2gpu/command_processor.h - the GPU's command processor, natively (Phase 3): consumes the ring buffer the game's Direct3D library fills, keeps the register file, executes the
// synchronisation packets (waits, fences, interrupts, the swap) and reports draws, shader loads and swaps to a Sink (the renderer).
//
// It replaces the runtime's GraphicsSystem + CommandProcessor for the game: the kernel imports VdInitializeRingBuffer / VdEnableRingBufferRPtrWriteBack / VdSetGraphicsInterruptCallback and the
// MMIO window 0x7FC80000 land here. The protocol was read from the runtime's behaviour and checked against the game: the ring is a PM4 stream in guest PHYSICAL memory, the read pointer is
// published (in dwords) to a write-back address, CP_RB_WPTR is written through the MMIO window, a vertical blank interrupt and the INTERRUPT packet call the game's callback.
//
// Threading: Run() executes on one thread (the owner makes it a guest thread so that the interrupt callback can run guest code); Mmio*/UpdateWritePointer/Initialize*
// may be called from any guest thread. Plain C++17, no SDK, no guest macros.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>

#include "gh2gpu/registers.h"

namespace gh2gpu {

// Where the guest's memory is on the host. `virt`: host address of guest address 0; `phys`: host address of physical address 0 (what the GPU is given).
struct GuestMemory {
  uint8_t* virt = nullptr;
  uint8_t* phys = nullptr;
  uint8_t* Physical(uint32_t address) const { return phys + address; }
};

// One draw as the packet stream describes it; everything else is in the register file.
struct DrawParams {
  uint32_t draw_initiator = 0;   // VGT_DRAW_INITIATOR
  uint32_t prim_type = 0;
  uint32_t num_indices = 0;
  uint32_t source_select = 0;    // 0 DMA (indices in memory), 1 immediate (not produced by the game), 2 auto index
  bool index32 = false;
  uint32_t dma_base = 0;         // physical address of the indices (source_select 0)
  uint32_t dma_size_bytes = 0;
  Endian dma_endian = Endian::kNone;
  uint32_t viz_query = 0;
};

// What the processor tells the renderer. All calls come from the processor's thread, in stream order. The default implementations ignore everything (a "null GPU": the
// game runs, nothing is drawn).
class Sink {
 public:
  virtual ~Sink() = default;
  // The vertex or pixel shader microcode at `physical` (size in dwords) is now active (IM_LOAD). `type`: 0 vertex, 1 pixel.
  virtual void OnShaderLoad(uint32_t /*type*/, uint32_t /*physical*/, uint32_t /*size_dwords*/) {}
  // IM_LOAD_IMMEDIATE: the microcode travels inside the command stream (host order dwords). The game's library uses this for vertex shaders patched to the vertex declaration of the draw.
  virtual void OnShaderLoadImmediate(uint32_t /*type*/, const uint32_t* /*words*/, uint32_t /*size_dwords*/) {}
  // `regs` is the register file at the moment of the draw.
  virtual void OnDraw(const uint32_t* /*regs*/, const DrawParams& /*draw*/) {}
  // VdSwap: the frame in the front buffer (physical address, width, height in pixels) is complete.
  virtual void OnSwap(const uint32_t* /*regs*/, uint32_t /*front_buffer_physical*/, uint32_t /*width*/, uint32_t /*height*/) {}
  // The processor is going idle: the ring is empty (a good moment to submit batched work).
  virtual void OnIdle() {}
};

struct Callbacks {
  // The game's graphics interrupt callback (source 0: vertical blank, 1: from the command stream; cpu: the hardware thread asked for). Runs on the caller's thread.
  std::function<void(uint32_t source, uint32_t cpu)> interrupt;
};

struct Stats {
  std::atomic<uint64_t> packets_type0{0}, packets_type1{0}, packets_type2{0}, packets_type3{0};
  std::atomic<uint64_t> type3_by_opcode[128] = {};
  std::atomic<uint64_t> draws{0}, swaps{0}, interrupts{0}, indirect_buffers{0}, wait_reg_mem{0}, wait_reg_mem_blocked{0}, wait_reg_mem_ns{0};
  std::atomic<uint64_t> fences{0}, shader_loads{0}, predicated_skipped{0}, unknown_opcodes{0}, bad_packets{0}, ring_dwords{0}, register_writes_out_of_range{0};
  std::atomic<uint64_t> idle_waits{0}, busy_ns{0};
  std::atomic<uint64_t> batches{0}, batch_dwords_sum{0}, batch_dwords_max{0};   // ring dwords found ready each time the processor wakes (how far the game runs ahead of it)
};

class CommandProcessor {
 public:
  CommandProcessor(const GuestMemory& memory, Sink* sink, Callbacks callbacks);
  ~CommandProcessor();
  CommandProcessor(const CommandProcessor&) = delete;
  CommandProcessor& operator=(const CommandProcessor&) = delete;

  // ---- the kernel imports ----
  void InitializeRingBuffer(uint32_t physical, uint32_t size_log2);            // VdInitializeRingBuffer: size is 1 << (size_log2 + 3) bytes
  void EnableReadPointerWriteBack(uint32_t physical, uint32_t block_size_log2);  // VdEnableRingBufferRPtrWriteBack
  void UpdateWritePointer(uint32_t dword_index);                               // CP_RB_WPTR, from the MMIO window

  // ---- the MMIO window (addresses 0x7FC80000..0x7FC8FFFF); return true when handled ----
  bool MmioRead(uint32_t address, uint32_t* value);
  bool MmioWrite(uint32_t address, uint32_t value);
  void SetDisplaySize(uint32_t width, uint32_t height) { display_width_ = width; display_height_ = height; }
  void NoteVblank() { ++vblank_counter_; }   // the owner's vertical blank clock ticked

  // ---- the processor thread ----
  void Run();            // returns after RequestStop()
  void RequestStop();

  // ---- state for the renderer (valid on the processor's thread) ----
  const uint32_t* registers() const { return regs_values_; }
  uint32_t frame_counter() const { return counter_.load(); }
  const Stats& stats() const { return stats_; }
  void WriteRegister(uint32_t index, uint32_t value);
  uint32_t ReadRegisterValue(uint32_t index) const;

 private:
  struct Reader;
  uint32_t ExecuteRange(Reader& reader);
  bool ExecutePacket(Reader& reader);
  bool ExecutePacketType0(Reader& reader, uint32_t packet);
  bool ExecutePacketType1(Reader& reader, uint32_t packet);
  bool ExecutePacketType3(Reader& reader, uint32_t packet);
  bool ExecuteDraw(Reader& reader, uint32_t opcode, uint32_t count);
  bool ExecuteWaitRegMem(Reader& reader);
  bool ExecuteCondWrite(Reader& reader);
  bool ExecuteSwap(Reader& reader, uint32_t count);
  bool ExecuteSetConstant(Reader& reader, uint32_t count);
  bool ExecuteLoadAluConstant(Reader& reader);
  bool ExecuteImLoad(Reader& reader);
  bool ExecuteImLoadImmediate(Reader& reader, uint32_t count);
  void WriteRegistersFromMemory(uint32_t first, const uint8_t* big_endian, uint32_t count);
  bool Compare(uint32_t function, uint32_t value, uint32_t mask, uint32_t reference) const;
  void MakeCoherent();
  void RaiseInterrupt(uint32_t source, uint32_t cpu);

  GuestMemory memory_;
  Sink* sink_;
  Callbacks callbacks_;

  // the register file; written by the processor thread (and the MMIO window), polled by WAIT_REG_MEM
  uint32_t regs_values_[kRegisterCount] = {};

  // the primary ring
  uint32_t ring_physical_ = 0;
  uint32_t ring_bytes_ = 0;
  uint32_t read_index_ = 0;                 // dwords
  std::atomic<uint32_t> write_index_{0xBAADF00Du};
  uint32_t rptr_writeback_physical_ = 0;
  uint32_t rptr_update_period_ = 0;
  std::mutex wake_mutex_;
  std::condition_variable wake_;
  std::atomic<bool> running_{false}, stop_{false};

  uint64_t bin_mask_ = 0, bin_select_ = 0;
  std::atomic<uint32_t> counter_{0};         // swaps completed (the value EVENT_WRITE_SHD can write)
  std::atomic<uint32_t> vblank_counter_{0};
  uint32_t display_width_ = 1280, display_height_ = 720;
  uint32_t depth_ = 0;                       // indirect buffer nesting
  Stats stats_;
};

}  // namespace gh2gpu
