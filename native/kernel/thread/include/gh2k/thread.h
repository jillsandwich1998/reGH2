// gh2k/thread.h - the guest-visible state of a thread, built natively (Phase 2.7, K.2): KPCR, KTHREAD, TLS block and stack.
//
// Translated code reaches all of it through r13: `0(r13)` is the static TLS data, `0x18(r13)` the IRQL, `0x100(r13)` the current KTHREAD, and the kernel calls read the rest
// (stack bounds, TLS slots, the thread id). Offsets are FACTS read from the runtime's headers and checked against a live thread (hooks_threads.cpp compares field by field).
// Everything is allocated from gh2k::mem: the stack from the title-thread range 0x70000000.. with a guard region at both ends, the PCR, the KTHREAD and the TLS block
// from the system part of the 4 KB heap (0x30000000..) like the runtime's SystemHeapAlloc, one page granule each. Plain C++17, no SDK, no guest macros.
#pragma once

#include <cstdint>
#include <string>

#include "gh2k/mem.h"

namespace gh2k::thread {

// KPCR (0x2D8 bytes; r13)
namespace pcr {
inline constexpr uint32_t kSize = 0x2D8;
inline constexpr uint32_t kTlsPtr = 0x000;       // be32: static TLS data (declspec(thread) variables)
inline constexpr uint32_t kCurrentIrql = 0x018;  // u8
inline constexpr uint32_t kPcrPtr = 0x030;       // be64: this PCR's address
inline constexpr uint32_t kHostStash = 0x038;    // host use (the runtime keeps its PPCContext here)
inline constexpr uint32_t kStackBase = 0x070;    // be32: highest stack address + 1
inline constexpr uint32_t kStackEnd = 0x074;     // be32: lowest usable stack address
inline constexpr uint32_t kPrcb = 0x100;         // the KPRCB lives inside the PCR
inline constexpr uint32_t kPrcbPointer = 0x2A8;  // be32: address of the KPRCB
// KPRCB
inline constexpr uint32_t kCurrentThread = 0x000;  // be32: the KTHREAD
inline constexpr uint32_t kCurrentCpu = 0x00C;     // u8
inline constexpr uint32_t kDpcActive = 0x050;      // be32
}  // namespace pcr

// KTHREAD (0xAB0 bytes)
namespace kthread {
inline constexpr uint32_t kSize = 0xAB0;
inline constexpr uint32_t kHeaderType = 0x000;       // u8 = 6 (thread object)
inline constexpr uint32_t kStackBase = 0x05C, kStackLimit = 0x060, kStackKernel = 0x064, kTlsDynamic = 0x068;
inline constexpr uint32_t kThreadState = 0x06C, kPriority = 0x070, kProcessTypeDup = 0x072, kProcessType = 0x073;
inline constexpr uint32_t kApcLists = 0x074;         // two list heads of 8 bytes
inline constexpr uint32_t kProcess = 0x084, kMayQueueApcs = 0x08B, kMsrMask = 0x09C;
inline constexpr uint32_t kSuspendCount = 0x0BC, kCurrentCpu = 0x0BF, kAPrcb = 0x0C0, kAnotherPrcb = 0x0C4, kStackAllocBase = 0x0D0;
inline constexpr uint32_t kCreateTime = 0x130, kTimerList = 0x144, kThreadId = 0x14C, kStartAddress = 0x150, kList154 = 0x154;
inline constexpr uint32_t kLastError = 0x160, kCreationFlags = 0x16C, kUnk17C = 0x17C;
}  // namespace kthread

struct Params {
  uint32_t stack_size = 256 * 1024;
  uint32_t tls_slots = 1024;          // 4-byte dynamic slots (KeTlsAlloc), after the static data
  uint32_t tls_static_size = 0;       // bytes of declspec(thread) data in front of the slots
  const uint8_t* tls_template = nullptr;  // copied into the static part (may be null: zeros)
  uint32_t tls_template_size = 0;     // bytes of the template; 0 = tls_static_size (the XEX's raw_data_size may be shorter than data_size: the rest stays zero)
  uint32_t thread_id = 0;
  uint32_t start_address = 0;
  uint32_t creation_flags = 0;        // bit 0: suspended
  uint32_t cpu = 0;
  uint32_t process = 0;               // guest KPROCESS, or 0
  uint8_t process_type = 1;           // user
  uint64_t create_time = 0;           // guest system time
};

struct Block {
  mem::Memory::Stack stack;
  uint32_t pcr = 0, kthread = 0, tls_static = 0, tls_dynamic = 0, tls_size = 0;
};

// Builds everything; the PCR's r13 value is block->pcr. Fails (and frees what it took) when memory runs out.
bool Create(mem::Memory& memory, const Params& params, Block* block, std::string* error = nullptr);
void Destroy(mem::Memory& memory, const Block& block);
// The stack alone (a thread that ended gives it back at once, the way the runtime does; the PCR / KTHREAD / TLS stay while a handle can still be used). Clears block->stack.
void DestroyStack(mem::Memory& memory, Block* block);

}  // namespace gh2k::thread
