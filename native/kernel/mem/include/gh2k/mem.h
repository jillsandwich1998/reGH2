// gh2k/mem.h - the guest's 4 GB address space and its heaps, natively (Phase 2.7, K.1).
//
// The translated code addresses guest memory as `base + guest_address` with 32-bit big-endian words, so the host side must provide one flat, contiguous
// reservation in which the Xbox 360 layout holds (FACTS, read from the runtime's memory manager and confirmed by the allocations the game makes):
//   0x00000000-0x3FFFFFFF  virtual, 4 KB pages          0x80000000-0x8FFFFFFF  XEX image, 64 KB pages    0xA0000000-0xBFFFFFFF  physical view, 64 KB pages
//   0x40000000-0x7EFFFFFF  virtual, 64 KB pages         0x90000000-0x9FFFFFFF  XEX, 4 KB pages           0xC0000000-0xDFFFFFFF  physical view, 16 MB pages
//                                                                                                        0xE0000000-0xFFCFFFFF  physical view, 4 KB pages
// The three physical views are windows on the SAME 512 MB of physical memory (guest 0xA0000000 + p, 0xC0000000 + p and 0xE0000000 + p are the same byte p), which is
// also what the GPU is given; here that is a memfd mapped three times. Physical allocations come top-down from physical 0x1FD01000 (where the 4 KB window ends; the 64 KB and 16 MB
// windows share the same allocator: the game's 4 KB, 64 KB and 16 MB requests are interleaved top-down in one range, FACT from its allocation trace).
// A thread's stack comes from the 64 KB virtual heap with an inaccessible guard region at both ends.
// Plain C++17 and POSIX, no SDK, no guest macros.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace gh2k::mem {

enum Protect : uint32_t { kNoAccess = 0, kRead = 1, kReadWrite = 3 };

struct Block {
  uint32_t base = 0, size = 0;   // the allocation that contains the queried address
  uint32_t protect = kNoAccess;
  bool committed = false;
};

// One host reservation of 0x120000000 bytes: guest virtual space (0..4 GB) and, 4 GB above it, the physical memory. Everything is one memfd of 0x120001000 bytes mapped
// the way the runtime's memory manager maps it (rex::system::Memory, map_info): virtual views at their own file offsets, the XEX views (0x80000000 and 0x90000000) on the same
// 256 MB, the physical views and the raw physical range on file offset 0x100000000, and the 0xE0000000 window one 4 KB page further (guest 0xE0000000 + q is physical q + 0x1000).
// The runtime can be told to lay its heaps over this arena (rex::memory::SetExternalArena, patch 0010) instead of making its own.
class Arena {
 public:
  static std::unique_ptr<Arena> Create(std::string* error);
  ~Arena();
  uint8_t* base() const { return base_; }                 // host address of guest address 0
  uint8_t* physical() const { return base_ + 0x100000000ull; }
  static constexpr uint64_t kReservation = 0x120000000ull;
  static constexpr uint32_t kPhysicalSize = 0x20000000u;
  int fd() const { return fd_; }
  int physical_fd() const { return fd_; }

 private:
  Arena() = default;
  uint8_t* base_ = nullptr;
  int fd_ = -1;
};

// A range of guest addresses handed out in pages. Allocation is first fit from the bottom or from the top; reserved ranges are inaccessible until committed.
class Heap {
 public:
  // `host_base` is the host address of guest address 0 for this heap's range; always_accessible: the range is mapped read/write for good (physical windows).
  // `fd` / `file_delta`: the memfd and the offset to add to a heap address to get its file offset (decommit and release punch a hole so the memory reads back zero).
  // `passive`: bookkeeping only - the heap never touches host memory (protection, zeroing); somebody else maps and protects the pages.
  Heap(uint8_t* host_base, uint32_t start, uint32_t size, uint32_t page_size, bool top_down, bool always_accessible = false, int fd = -1, int64_t file_delta = 0, bool passive = false);
  bool Alloc(uint32_t size, uint32_t alignment, uint32_t protect, bool commit, uint32_t* address);
  // The placement the runtime's heaps use: first fit inside [low, high] (inclusive), from the bottom or the top, `alignment` a multiple of the page size.
  bool AllocRange(uint32_t low, uint32_t high, uint32_t size, uint32_t alignment, bool top_down, uint32_t* address);
  bool AllocFixed(uint32_t address, uint32_t size, uint32_t protect, bool commit);
  bool Commit(uint32_t address, uint32_t size, uint32_t protect);      // inside one allocation
  bool Decommit(uint32_t address, uint32_t size);                      // contents are discarded (read back as zero)
  bool Protect(uint32_t address, uint32_t size, uint32_t protect);
  bool Release(uint32_t address, uint32_t* released_size = nullptr);   // `address` is the allocation's base
  bool Query(uint32_t address, Block* block) const;
  uint32_t page_size() const { return page_size_; }
  uint32_t start() const { return start_; }
  uint32_t size() const { return size_; }
  uint64_t allocated_bytes() const { return allocated_; }
  uint64_t peak_bytes() const { return peak_; }
  size_t blocks() const;

 private:
  bool FindGap(uint32_t size, uint32_t alignment, uint64_t low, uint64_t high_exclusive, bool top_down, uint32_t* address) const;
  void Apply(uint32_t address, uint32_t size, uint32_t protect);
  void Zero(uint32_t address, uint32_t size);
  uint8_t* const host_;
  const uint32_t start_, size_, page_size_;
  const bool top_down_, always_accessible_;
  const int fd_;
  const int64_t file_delta_;
  const bool passive_;
  mutable std::mutex mu_;
  std::map<uint32_t, Block> blocks_;   // by base address
  uint64_t allocated_ = 0, peak_ = 0;
};

// The whole layout.
class Memory {
 public:
  static std::unique_ptr<Memory> Create(std::string* error);
  // The heaps' bookkeeping over an arena somebody else owns and protects (the runtime's memory manager): same ranges, nothing is mapped, protected or zeroed.
  static std::unique_ptr<Memory> Over(Arena& arena);
  Arena& arena() { return *arena_; }
  Heap* physical_heap() { return physical_.get(); }
  uint8_t* base() const { return arena_->base(); }

  Heap& v00000000() { return *virtual4k_; }
  Heap& v40000000() { return *virtual64k_; }
  Heap& v80000000() { return *xex64k_; }
  Heap& v90000000() { return *xex4k_; }
  Heap& physical() { return *physical_; }     // physical offsets, 4 KB pages, top-down
  Heap* HeapFor(uint32_t guest_address);       // the virtual heap that owns the address (physical views: null, use PhysicalAlloc/Free)

  // MmAllocatePhysicalMemoryEx / MmFreePhysicalMemory: view_page_size selects the window (64 KB: 0xA0000000, 16 MB: 0xC0000000, 4 KB: 0xE0000000).
  bool PhysicalAlloc(uint32_t size, uint32_t alignment, uint32_t view_page_size, uint32_t protect, uint32_t* guest_address);
  bool PhysicalFree(uint32_t guest_address, uint32_t* released_size = nullptr);
  uint32_t PhysicalAllocationSize(uint32_t guest_address) const;   // MmQueryAllocationSize; 0 when not an allocation
  static bool IsPhysicalView(uint32_t guest_address) { return guest_address >= 0xA0000000u; }
  static uint32_t PhysicalOffset(uint32_t guest_address);          // guest view address -> physical byte (E window: + 0x1000); the physical heap is addressed in these

  // Thread stacks: `size` usable bytes between two guard regions (the heap's page size each); filled with 0xBE like the runtime does.
  struct Stack {
    uint32_t alloc_base = 0, alloc_size = 0;   // the whole allocation, guards included
    uint32_t limit = 0;                        // lowest usable address
    uint32_t top = 0;                          // highest usable address + 1 (the initial stack pointer is below it)
  };
  bool AllocStack(uint32_t size, Stack* stack);
  bool FreeStack(const Stack& stack);

 private:
  Memory() = default;
  std::unique_ptr<Arena> owned_;
  Arena* arena_ = nullptr;
  std::unique_ptr<Heap> virtual4k_, virtual64k_, xex64k_, xex4k_, physical_;
};

// The process-wide memory manager over the runtime's arena (set by the application when it installs the native heaps; null before).
Memory* GlobalMemory();
void SetGlobalMemory(Memory* memory);

}  // namespace gh2k::mem
