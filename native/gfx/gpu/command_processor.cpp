#include "gh2gpu/command_processor.h"

#include <algorithm>
#include <vector>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace gh2gpu {

namespace {

inline uint32_t LoadBe32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
inline void StoreHost32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }   // as the hardware stores: the value is already swapped for the memory's byte order
inline void StoreBe32(uint8_t* p, uint32_t v) { StoreHost32(p, __builtin_bswap32(v)); }

}  // namespace

// A window on a PM4 stream in guest memory: the ring (wraps) or an indirect buffer (does not).
struct CommandProcessor::Reader {
  const uint8_t* base = nullptr;
  uint32_t capacity = 0;    // bytes
  uint32_t offset = 0;      // next byte to read
  uint32_t remaining = 0;   // bytes left in this window
  bool underflow = false;

  uint32_t Read() {
    if (remaining < 4) {
      underflow = true;
      return 0;
    }
    const uint32_t v = LoadBe32(base + offset);
    offset += 4;
    if (offset >= capacity) offset -= capacity;
    remaining -= 4;
    return v;
  }
  void Advance(uint32_t dwords) {
    const uint32_t bytes = dwords * 4;
    if (bytes > remaining) {
      underflow = true;
      offset = (offset + remaining) % (capacity ? capacity : 1);
      remaining = 0;
      return;
    }
    offset = static_cast<uint32_t>((static_cast<uint64_t>(offset) + bytes) % capacity);
    remaining -= bytes;
  }
};

CommandProcessor::CommandProcessor(const GuestMemory& memory, Sink* sink, Callbacks callbacks) : memory_(memory), sink_(sink), callbacks_(std::move(callbacks)) {}

CommandProcessor::~CommandProcessor() { RequestStop(); }

void CommandProcessor::InitializeRingBuffer(uint32_t physical, uint32_t size_log2) {
  ring_physical_ = physical & 0x1FFFFFFFu;
  ring_bytes_ = 1u << (size_log2 + 3);
  read_index_ = 0;
}

void CommandProcessor::EnableReadPointerWriteBack(uint32_t physical, uint32_t block_size_log2) {
  rptr_writeback_physical_ = physical & 0x1FFFFFFFu;
  rptr_update_period_ = (1u << block_size_log2) >> 2;
}

void CommandProcessor::UpdateWritePointer(uint32_t dword_index) {
  write_index_.store(dword_index, std::memory_order_release);
  std::lock_guard<std::mutex> lock(wake_mutex_);
  wake_.notify_one();
}

// ---------------------------------------------------------------------------------------------------------------------------------------- MMIO

bool CommandProcessor::MmioRead(uint32_t address, uint32_t* value) {
  if ((address & kMmioMask) != kMmioBase) return false;
  const uint32_t r = (address & 0xFFFF) / 4;
  switch (r) {
    case reg::RB_EDRAM_TIMING: *value = 0x08100748u; return true;
    case reg::RB_BC_CONTROL: *value = 0x0000200Eu; return true;
    case reg::D1MODE_V_COUNTER: *value = display_height_ < 0x0FFFu ? display_height_ : 0x0FFFu; return true;
    case reg::D1MODE_VBLANK_VLINE_STATUS: *value = 1; return true;   // vblank
    case reg::D1MODE_VIEWPORT_SIZE: {
      const uint32_t w = display_width_ < 0x0FFFu ? display_width_ : 0x0FFFu, h = display_height_ < 0x0FFFu ? display_height_ : 0x0FFFu;
      *value = (w << 16) | h;
      return true;
    }
    default: *value = ReadRegisterValue(r); return true;
  }
}

bool CommandProcessor::MmioWrite(uint32_t address, uint32_t value) {
  if ((address & kMmioMask) != kMmioBase) return false;
  const uint32_t r = (address & 0xFFFF) / 4;
  if (r == reg::CP_RB_WPTR) UpdateWritePointer(value);
  regs_values_[r] = value;
  return true;
}

uint32_t CommandProcessor::ReadRegisterValue(uint32_t index) const {
  return index < kRegisterCount ? const_cast<const volatile uint32_t&>(regs_values_[index]) : 0;
}

void CommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  if (index >= kRegisterCount) {
    ++stats_.register_writes_out_of_range;
    return;
  }
  const_cast<volatile uint32_t&>(regs_values_[index]) = value;   // volatile for the WAIT_REG_MEM poll
  if (index >= reg::SCRATCH_REG0 && index <= reg::SCRATCH_REG0 + 7) {
    const uint32_t n = index - reg::SCRATCH_REG0;
    if ((1u << n) & regs_values_[reg::SCRATCH_UMSK]) StoreBe32(memory_.Physical((regs_values_[reg::SCRATCH_ADDR] + n * 4) & 0x1FFFFFFFu), value);   // scratch write-back
  } else if (index == reg::COHER_STATUS_HOST) {
    const_cast<volatile uint32_t&>(regs_values_[index]) |= 0x80000000u;   // dirty: the next WAIT_REG_MEM on it makes the memory coherent
  }
}

void CommandProcessor::MakeCoherent() {
  volatile uint32_t* regs = regs_values_;
  if (regs[reg::COHER_STATUS_HOST] & 0x80000000u) regs[reg::COHER_STATUS_HOST] = 0;   // nothing to flush on the host: the memory is the memory
}

bool CommandProcessor::Compare(uint32_t function, uint32_t value, uint32_t mask, uint32_t reference) const {
  switch (function & 7) {
    case 0: return false;                           // never
    case 1: return (value & mask) < reference;
    case 2: return (value & mask) <= reference;
    case 3: return (value & mask) == reference;
    case 4: return (value & mask) != reference;
    case 5: return (value & mask) >= reference;
    case 6: return (value & mask) > reference;
    default: return true;                           // always
  }
}

void CommandProcessor::RaiseInterrupt(uint32_t source, uint32_t cpu) {
  ++stats_.interrupts;
  if (callbacks_.interrupt) callbacks_.interrupt(source, cpu);
}

// ---------------------------------------------------------------------------------------------------------------------------------- the processor

void CommandProcessor::RequestStop() {
  stop_.store(true);
  std::lock_guard<std::mutex> lock(wake_mutex_);
  wake_.notify_all();
}

void CommandProcessor::Run() {
  running_.store(true);
  bool idle_reported = false;
  while (!stop_.load()) {
    const uint32_t write_index = write_index_.load(std::memory_order_acquire);
    if (write_index == 0xBAADF00Du || ring_bytes_ == 0 || read_index_ == write_index) {
      if (!idle_reported) {
        idle_reported = true;
        sink_->OnIdle();
      }
      // spin a little (a new frame is usually microseconds away), then sleep on the write pointer
      for (int i = 0; i < 200 && !stop_.load(); ++i) {
        if (write_index_.load(std::memory_order_acquire) != read_index_) break;
        std::this_thread::yield();
      }
      if (write_index_.load(std::memory_order_acquire) == read_index_ || write_index_.load() == 0xBAADF00Du) {
        ++stats_.idle_waits;
        std::unique_lock<std::mutex> lock(wake_mutex_);
        wake_.wait_for(lock, std::chrono::milliseconds(2), [&] { return stop_.load() || (write_index_.load() != read_index_ && write_index_.load() != 0xBAADF00Du); });
      }
      continue;
    }
    idle_reported = false;
    const auto t0 = std::chrono::steady_clock::now();
    const uint32_t ring_dwords = ring_bytes_ / 4;
    const uint32_t available = (write_index + ring_dwords - read_index_) % ring_dwords;
    Reader reader;
    reader.base = memory_.Physical(ring_physical_);
    reader.capacity = ring_bytes_;
    reader.offset = read_index_ * 4;
    reader.remaining = available * 4;
    stats_.ring_dwords += available;
    ++stats_.batches;
    stats_.batch_dwords_sum += available;
    if (available > stats_.batch_dwords_max.load(std::memory_order_relaxed)) stats_.batch_dwords_max.store(available, std::memory_order_relaxed);
    // publish the read pointer as the stream is consumed (a block at a time, like the hardware), so the game sees ring space come back during a long run of packets
    const uint32_t period = rptr_update_period_ ? rptr_update_period_ : 16;
    uint32_t since_publish = 0;
    while (reader.remaining > 0 && !stop_.load()) {
      const uint32_t before = reader.remaining;
      if (!ExecutePacket(reader)) {
        ++stats_.bad_packets;
        break;
      }
      since_publish += (before - reader.remaining) / 4;
      if (since_publish >= period && rptr_writeback_physical_) {
        since_publish = 0;
        read_index_ = reader.offset / 4;
        StoreBe32(memory_.Physical(rptr_writeback_physical_), read_index_);
      }
    }
    read_index_ = reader.offset / 4;
    if (rptr_writeback_physical_) StoreBe32(memory_.Physical(rptr_writeback_physical_), read_index_);
    stats_.busy_ns += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
  }
  running_.store(false);
}

bool CommandProcessor::ExecutePacket(Reader& reader) {
  const uint32_t packet = reader.Read();
  if (packet == 0) return true;
  switch (packet >> 30) {
    case 0: ++stats_.packets_type0; return ExecutePacketType0(reader, packet);
    case 1: ++stats_.packets_type1; return ExecutePacketType1(reader, packet);
    case 2: ++stats_.packets_type2; return true;   // no-op
    default: ++stats_.packets_type3; return ExecutePacketType3(reader, packet);
  }
}

bool CommandProcessor::ExecutePacketType0(Reader& reader, uint32_t packet) {
  const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
  if (reader.remaining < count * 4) return false;
  const uint32_t base_index = packet & 0x7FFF;
  const bool one_register = (packet >> 15) & 1;
  for (uint32_t m = 0; m < count; ++m) WriteRegister(one_register ? base_index : base_index + m, reader.Read());
  return true;
}

bool CommandProcessor::ExecutePacketType1(Reader& reader, uint32_t packet) {
  const uint32_t r1 = packet & 0x7FF, r2 = (packet >> 11) & 0x7FF;
  const uint32_t d1 = reader.Read(), d2 = reader.Read();
  WriteRegister(r1, d1);
  WriteRegister(r2, d2);
  return true;
}

void CommandProcessor::WriteRegistersFromMemory(uint32_t first, const uint8_t* big_endian, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) WriteRegister(first + i, LoadBe32(big_endian + 4 * i));
}

bool CommandProcessor::ExecutePacketType3(Reader& reader, uint32_t packet) {
  const uint32_t opcode = (packet >> 8) & 0x7F;
  const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
  if (reader.remaining < count * 4) return false;
  ++stats_.type3_by_opcode[opcode];
  const uint32_t end_remaining = reader.remaining - count * 4;

  // bit 0 = predicate: executed only if some bin passes; a predicated swap is never valid
  if (packet & 1) {
    if (!((bin_select_ & bin_mask_) != 0) || opcode == pm4::XE_SWAP) {
      ++stats_.predicated_skipped;
      reader.Advance(count);
      return true;
    }
  }

  bool ok = true;
  switch (opcode) {
    case pm4::ME_INIT:
    case pm4::NOP:
    case pm4::WAIT_FOR_IDLE:
    case pm4::CONTEXT_UPDATE:
    case pm4::INVALIDATE_STATE:
      reader.Advance(count);
      break;
    case pm4::INTERRUPT: {
      const uint32_t cpu_mask = reader.Read();
      for (uint32_t n = 0; n < 6; ++n)
        if (cpu_mask & (1u << n)) RaiseInterrupt(1, n);
    } break;
    case pm4::XE_SWAP:
      ok = ExecuteSwap(reader, count);
      break;
    case pm4::INDIRECT_BUFFER:
    case pm4::INDIRECT_BUFFER_PFD: {
      const uint32_t list_ptr = reader.Read() & 0x1FFFFFFFu;
      const uint32_t length = reader.Read() & 0xFFFFFu;
      ++stats_.indirect_buffers;
      if (depth_ >= 8) {
        ok = false;
        break;
      }
      Reader inner;
      inner.base = memory_.Physical(list_ptr);
      inner.capacity = length * 4;
      inner.offset = 0;
      inner.remaining = length * 4;
      ++depth_;
      while (inner.remaining > 0 && !stop_.load()) {
        if (!ExecutePacket(inner)) {
          ++stats_.bad_packets;
          break;
        }
      }
      --depth_;
    } break;
    case pm4::WAIT_REG_MEM:
      ok = ExecuteWaitRegMem(reader);
      break;
    case pm4::REG_RMW: {
      const uint32_t rmw_info = reader.Read(), and_mask = reader.Read(), or_mask = reader.Read();
      uint32_t value = ReadRegisterValue(rmw_info & 0x1FFF);
      value &= ((rmw_info >> 31) & 1) ? ReadRegisterValue(and_mask & 0x1FFF) : and_mask;
      value |= ((rmw_info >> 30) & 1) ? ReadRegisterValue(or_mask & 0x1FFF) : or_mask;
      WriteRegister(rmw_info & 0x1FFF, value);
    } break;
    case pm4::REG_TO_MEM: {
      const uint32_t reg_addr = reader.Read();
      uint32_t mem_addr = reader.Read();
      const uint32_t value = GpuSwap(ReadRegisterValue(reg_addr), static_cast<Endian>(mem_addr & 3));
      StoreHost32(memory_.Physical((mem_addr & ~3u) & 0x1FFFFFFFu), value);
    } break;
    case pm4::MEM_WRITE: {
      uint32_t write_addr = reader.Read();
      for (uint32_t i = 0; i + 1 < count; ++i) {
        const uint32_t data = GpuSwap(reader.Read(), static_cast<Endian>(write_addr & 3));
        StoreHost32(memory_.Physical((write_addr & ~3u) & 0x1FFFFFFFu), data);
        write_addr += 4;
      }
    } break;
    case pm4::COND_WRITE:
      ok = ExecuteCondWrite(reader);
      break;
    case pm4::EVENT_WRITE: {
      const uint32_t initiator = reader.Read();
      WriteRegister(reg::VGT_EVENT_INITIATOR, initiator & 0x3F);
    } break;
    case pm4::EVENT_WRITE_SHD: {
      const uint32_t initiator = reader.Read(), address = reader.Read(), value = reader.Read();
      WriteRegister(reg::VGT_EVENT_INITIATOR, initiator & 0x3F);
      const uint32_t data = ((initiator >> 31) & 1) ? counter_.load() : value;   // a fence: the swap counter, or the value the game chose
      StoreHost32(memory_.Physical((address & ~3u) & 0x1FFFFFFFu), GpuSwap(data, static_cast<Endian>(address & 3)));
      ++stats_.fences;
    } break;
    case pm4::EVENT_WRITE_EXT: {
      const uint32_t initiator = reader.Read(), address = reader.Read();
      WriteRegister(reg::VGT_EVENT_INITIATOR, initiator & 0x3F);
      // the screen extents the hardware reports, big endian 16-bit: the whole 8192 x 8192 surface in units of 8
      const uint16_t extents[6] = {0, 8192 >> 3, 0, 8192 >> 3, 0, 1};
      uint8_t* out = memory_.Physical((address & ~3u) & 0x1FFFFFFFu);
      for (int i = 0; i < 6; ++i) {
        out[2 * i] = static_cast<uint8_t>(extents[i] >> 8);
        out[2 * i + 1] = static_cast<uint8_t>(extents[i]);
      }
    } break;
    case pm4::EVENT_WRITE_ZPD: {
      const uint32_t initiator = reader.Read();
      WriteRegister(reg::VGT_EVENT_INITIATOR, initiator & 0x3F);
      // occlusion queries: no sample counting on the host yet; the game gets the fake count the runtime has always given it (1000 samples passed)
      if (const uint32_t counts_physical = regs_values_[reg::RB_SAMPLE_COUNT_ADDR]) {
        uint8_t* counts = memory_.Physical(counts_physical & 0x1FFFFFFFu);
        uint32_t words[8];
        std::memcpy(words, counts, sizeof words);   // Total_A, Total_B, ZFail_A, ZFail_B, ZPass_A, ZPass_B, StencilFail_A, StencilFail_B (little endian fields)
        const uint32_t finished = __builtin_bswap32(0xFFFFFEEDu);
        const bool end = words[4] == finished || words[5] == finished || words[2] == finished || words[3] == finished;
        std::memset(counts, 0, sizeof words);
        if (end) {
          const uint32_t fake = 1000;
          std::memcpy(counts + 16, &fake, 4);   // ZPass_A
          std::memcpy(counts + 0, &fake, 4);    // Total_A
        }
      }
    } break;
    case pm4::DRAW_INDX:
    case pm4::DRAW_INDX_2:
      ok = ExecuteDraw(reader, opcode, count);
      break;
    case pm4::SET_CONSTANT:
      ok = ExecuteSetConstant(reader, count);
      break;
    case pm4::SET_CONSTANT2:
    case pm4::SET_SHADER_CONSTANTS: {
      const uint32_t index = reader.Read() & 0xFFFF;
      for (uint32_t i = 0; i + 1 < count; ++i) WriteRegister(index + i, reader.Read());
    } break;
    case pm4::LOAD_ALU_CONSTANT:
      ok = ExecuteLoadAluConstant(reader);
      break;
    case pm4::IM_LOAD:
      ok = ExecuteImLoad(reader);
      break;
    case pm4::IM_LOAD_IMMEDIATE:
      ok = ExecuteImLoadImmediate(reader, count);
      break;
    case pm4::VIZ_QUERY: {
      const uint32_t dword0 = reader.Read();
      const uint32_t id = dword0 & 0x3F;
      if (dword0 & 0x100) {   // end of the query: report "finished"
        if (id < 32) regs_values_[reg::PA_SC_VIZ_QUERY_STATUS_0] |= 1u << id;
        else regs_values_[reg::PA_SC_VIZ_QUERY_STATUS_1] |= 1u << (id - 32);
      }
    } break;
    case pm4::SET_BIN_MASK_LO: bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | reader.Read(); reader.Advance(count - 1); break;
    case pm4::SET_BIN_MASK_HI: bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (static_cast<uint64_t>(reader.Read()) << 32); reader.Advance(count - 1); break;
    case pm4::SET_BIN_SELECT_LO: bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | reader.Read(); reader.Advance(count - 1); break;
    case pm4::SET_BIN_SELECT_HI: bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (static_cast<uint64_t>(reader.Read()) << 32); reader.Advance(count - 1); break;
    case pm4::SET_BIN_MASK: {
      const uint64_t hi = reader.Read(), lo = reader.Read();
      bin_mask_ = (hi << 32) | lo;
    } break;
    case pm4::SET_BIN_SELECT: {
      const uint64_t hi = reader.Read(), lo = reader.Read();
      bin_select_ = (hi << 32) | lo;
    } break;
    default:
      ++stats_.unknown_opcodes;
      reader.Advance(count);
      break;
  }
  // whatever a handler left unread of its packet is skipped, so the stream stays in step
  if (reader.remaining > end_remaining) reader.Advance((reader.remaining - end_remaining) / 4);
  return ok && !reader.underflow;
}

bool CommandProcessor::ExecuteWaitRegMem(Reader& reader) {
  const uint32_t wait_info = reader.Read(), poll_address = reader.Read(), reference = reader.Read(), mask = reader.Read(), wait = reader.Read();
  ++stats_.wait_reg_mem;
  const bool is_memory = (wait_info & 0x10) != 0;
  bool blocked = false;
  std::chrono::steady_clock::time_point t0;
  uint32_t spins = 0;
  for (;;) {
    uint32_t value;
    if (is_memory) {
      std::memcpy(&value, memory_.Physical((poll_address & ~3u) & 0x1FFFFFFFu), 4);   // raw load, then the endian swap the packet asks for
      __atomic_thread_fence(__ATOMIC_ACQUIRE);
      value = GpuSwap(value, static_cast<Endian>(poll_address & 3));
    } else {
      value = ReadRegisterValue(poll_address);
      if (poll_address == reg::COHER_STATUS_HOST) {
        MakeCoherent();
        value = ReadRegisterValue(poll_address);
      }
    }
    if (Compare(wait_info, value, mask, reference)) break;
    if (!blocked) {
      blocked = true;
      ++stats_.wait_reg_mem_blocked;
      t0 = std::chrono::steady_clock::now();
    }
    if (stop_.load()) return false;
    if (++spins < 4000) std::this_thread::yield();
    else std::this_thread::sleep_for(std::chrono::microseconds(wait >= 0x100 ? 200 : 50));
  }
  if (blocked) stats_.wait_reg_mem_ns += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
  return true;
}

bool CommandProcessor::ExecuteCondWrite(Reader& reader) {
  const uint32_t wait_info = reader.Read(), poll_address = reader.Read(), reference = reader.Read(), mask = reader.Read(), write_address = reader.Read();
  uint32_t write_data = reader.Read();
  uint32_t value;
  if (wait_info & 0x10) {
    std::memcpy(&value, memory_.Physical((poll_address & ~3u) & 0x1FFFFFFFu), 4);
    value = GpuSwap(value, static_cast<Endian>(poll_address & 3));
  } else {
    value = ReadRegisterValue(poll_address);
  }
  if (Compare(wait_info, value, mask, reference)) {
    if (wait_info & 0x100) StoreHost32(memory_.Physical((write_address & ~3u) & 0x1FFFFFFFu), GpuSwap(write_data, static_cast<Endian>(write_address & 3)));
    else WriteRegister(write_address, write_data);
  }
  return true;
}

bool CommandProcessor::ExecuteSwap(Reader& reader, uint32_t count) {
  const uint32_t magic = reader.Read();
  const uint32_t front = reader.Read(), width = reader.Read(), height = reader.Read();
  reader.Advance(count > 4 ? count - 4 : 0);
  if (magic != pm4::kSwapSignature) return false;
  ++stats_.swaps;
  sink_->OnSwap(regs_values_, front & 0x1FFFFFFFu, width, height);
  counter_.fetch_add(1);
  return true;
}

bool CommandProcessor::ExecuteDraw(Reader& reader, uint32_t opcode, uint32_t count) {
  uint32_t left = count;
  DrawParams d;
  if (opcode == pm4::DRAW_INDX) {   // the first dword is the visibility query token
    if (left == 0) return false;
    d.viz_query = reader.Read();
    --left;
  }
  if (left == 0) return false;
  d.draw_initiator = reader.Read();
  --left;
  WriteRegister(reg::VGT_DRAW_INITIATOR, d.draw_initiator);
  d.prim_type = DrawPrimType(d.draw_initiator);
  d.source_select = DrawSourceSelect(d.draw_initiator);
  d.index32 = DrawIndex32(d.draw_initiator);
  d.num_indices = DrawNumIndices(d.draw_initiator);
  bool ok = true;
  if (d.source_select == 0) {   // DMA: base and size follow
    if (left < 2) return false;
    d.dma_base = reader.Read();
    --left;
    const uint32_t size = reader.Read();
    --left;
    WriteRegister(reg::VGT_DMA_BASE, d.dma_base);
    WriteRegister(reg::VGT_DMA_SIZE, size);
    const uint32_t bytes_per_index = d.index32 ? 4 : 2;
    d.dma_base &= ~(bytes_per_index - 1);
    d.dma_endian = static_cast<Endian>(size >> 30);
    d.dma_size_bytes = (size & 0xFFFFFFu) * bytes_per_index;
  } else if (d.source_select == 1) {
    ok = false;   // inline indices: the game does not produce them
  }
  reader.Advance(left);
  if (ok) {
    // a draw whose only purpose is an occlusion test is discarded after the test
    const uint32_t viz = regs_values_[reg::PA_SC_VIZ_QUERY];
    if (!((viz & 1) && ((viz >> 7) & 1))) {
      ++stats_.draws;
      sink_->OnDraw(regs_values_, d);
    }
  }
  return true;
}

bool CommandProcessor::ExecuteSetConstant(Reader& reader, uint32_t count) {
  const uint32_t offset_type = reader.Read();
  const uint32_t index = offset_type & 0x7FF;
  const uint32_t type = (offset_type >> 16) & 0xFF;
  uint32_t base;
  switch (type) {
    case 0: base = reg::SHADER_CONSTANT_000_X; break;
    case 1: base = reg::SHADER_CONSTANT_FETCH_00_0; break;
    case 2: base = reg::SHADER_CONSTANT_BOOL_000_031; break;
    case 3: base = reg::SHADER_CONSTANT_LOOP_00; break;
    case 4: base = 0x2000; break;   // plain registers
    default: reader.Advance(count - 1); return true;
  }
  for (uint32_t i = 0; i + 1 < count; ++i) WriteRegister(base + index + i, reader.Read());
  return true;
}

bool CommandProcessor::ExecuteLoadAluConstant(Reader& reader) {
  const uint32_t address = reader.Read() & 0x3FFFFFFFu;
  const uint32_t offset_type = reader.Read();
  const uint32_t size_dwords = reader.Read() & 0xFFFu;
  const uint32_t index = offset_type & 0x7FF;
  const uint32_t type = (offset_type >> 16) & 0xFF;
  uint32_t base;
  switch (type) {
    case 0: base = reg::SHADER_CONSTANT_000_X; break;
    case 1: base = reg::SHADER_CONSTANT_FETCH_00_0; break;
    case 2: base = reg::SHADER_CONSTANT_BOOL_000_031; break;
    case 3: base = reg::SHADER_CONSTANT_LOOP_00; break;
    case 4: base = 0x2000; break;
    default: return true;
  }
  WriteRegistersFromMemory(base + index, memory_.Physical(address & 0x1FFFFFFFu), size_dwords);
  return true;
}

bool CommandProcessor::ExecuteImLoad(Reader& reader) {
  const uint32_t address_type = reader.Read();
  const uint32_t start_size = reader.Read();
  ++stats_.shader_loads;
  sink_->OnShaderLoad(address_type & 3, (address_type & ~3u) & 0x1FFFFFFFu, start_size & 0xFFFF);
  return true;
}

bool CommandProcessor::ExecuteImLoadImmediate(Reader& reader, uint32_t count) {
  if (count < 2) return false;
  const uint32_t type = reader.Read() & 3;
  const uint32_t start_size = reader.Read();
  const uint32_t size = std::min(start_size & 0xFFFFu, count - 2);
  thread_local std::vector<uint32_t> words;
  words.resize(size);
  for (uint32_t i = 0; i < size; ++i) words[i] = reader.Read();
  ++stats_.shader_loads;
  sink_->OnShaderLoadImmediate(type, words.data(), size);
  return true;
}

}  // namespace gh2gpu
