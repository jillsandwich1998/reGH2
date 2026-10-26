#include "native_arena.h"

#include <gh2k/mem.h>
#include <rex/cvar.h>
#include <rex/system/xmemory.h>

#include <atomic>
#include <chrono>
#include <thread>

#include <cstdio>
#include <memory>
#include <string>

REXCVAR_DEFINE_STRING(arena, "native", "GH2",
                      "Who reserves the guest's 4 GB address space: native (gh2k::mem; the runtime's heaps are laid over it; default) or sdk (the runtime makes its own)")
    .allowed({"sdk", "native"});

REXCVAR_DEFINE_STRING(heaps, "native", "GH2",
                      "Who decides where guest allocations go and keeps their registry: native (gh2k::mem; the runtime's page tables only mirror it; default) or sdk (the runtime)")
    .allowed({"sdk", "native"});

namespace {

// The allocation registry of the runtime's five address-space heaps, kept by gh2k::mem::Heap. The runtime's BaseHeap still records commit state, protection and write
// watches in its page table, but where an allocation goes, and whether a fixed one is possible, is decided here.
class NativeHeapAuthority final : public rex::memory::HeapAuthority {
 public:
  explicit NativeHeapAuthority(gh2k::mem::Arena& arena) : memory_(gh2k::mem::Memory::Over(arena)) {
    reporter_ = std::thread([this] {
      const auto t0 = std::chrono::steady_clock::now();
      for (int next = 10;; next += 10) {
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < next) std::this_thread::sleep_for(std::chrono::milliseconds(500));
        fprintf(stderr, "HEAPS T+%ds native allocations %llu (fixed %llu), releases %llu, refused %llu | live blocks: 4K %zu, 64K %zu, xex %zu+%zu, physical %zu (%.1f MB)\n", next,
                (unsigned long long)allocs_.load(), (unsigned long long)fixed_.load(), (unsigned long long)releases_.load(), (unsigned long long)refused_.load(), memory_->v00000000().blocks(),
                memory_->v40000000().blocks(), memory_->v80000000().blocks(), memory_->v90000000().blocks(), memory_->physical_heap()->blocks(), memory_->physical_heap()->allocated_bytes() / 1048576.0);
      }
    });
    reporter_.detach();
  }
  gh2k::mem::Memory& memory() { return *memory_; }
  bool Handles(const rex::memory::HeapDescriptor& h) override { return Find(h) != nullptr; }
  bool AllocRange(const rex::memory::HeapDescriptor& h, uint32_t low, uint32_t high, uint32_t size, uint32_t alignment, bool top_down, uint32_t* address) override {
    gh2k::mem::Heap* heap = Find(h);
    if (!heap || !heap->AllocRange(low, high, size, alignment, top_down, address)) {
      ++refused_;
      return false;
    }
    ++allocs_;
    return true;
  }
  bool AllocFixed(const rex::memory::HeapDescriptor& h, uint32_t address, uint32_t size) override {
    gh2k::mem::Heap* heap = Find(h);
    if (!heap || !heap->AllocFixed(address, size, gh2k::mem::kReadWrite, false)) {
      ++refused_;
      return false;
    }
    ++fixed_;
    return true;
  }
  void Release(const rex::memory::HeapDescriptor& h, uint32_t address) override {
    if (gh2k::mem::Heap* heap = Find(h)) {
      heap->Release(address);
      ++releases_;
    }
  }

 private:
  gh2k::mem::Heap* Find(const rex::memory::HeapDescriptor& h) {
    if (h.physical_parent) return h.base == 0 ? memory_->physical_heap() : nullptr;
    switch (h.base) {
      case 0x00000000u: return h.page_size == 4096 ? &memory_->v00000000() : nullptr;
      case 0x40000000u: return &memory_->v40000000();
      case 0x80000000u: return &memory_->v80000000();
      case 0x90000000u: return &memory_->v90000000();
      default: return nullptr;   // the physical windows pin their parent's allocations
    }
  }
  std::unique_ptr<gh2k::mem::Memory> memory_;
  std::thread reporter_;
  std::atomic<uint64_t> allocs_{0}, fixed_{0}, releases_{0}, refused_{0};
};

}  // namespace

bool Gh2NativeArenaSelected() { return REXCVAR_GET(arena) == "native"; }

bool Gh2InstallNativeArena() {
  static std::unique_ptr<gh2k::mem::Arena> arena;   // lives as long as the process: the runtime never unmaps it
  if (arena) return true;
  std::string error;
  arena = gh2k::mem::Arena::Create(&error);
  if (!arena) {
    fprintf(stderr, "ARENA native reservation failed (%s): the runtime makes its own\n", error.c_str());
    return false;
  }
  const rex::memory::ExternalArena external{arena->base(), arena->fd()};
  rex::memory::SetExternalArena(&external);
  fprintf(stderr, "ARENA native: guest address space at %p (gh2k::mem)\n", static_cast<void*>(arena->base()));
  if (REXCVAR_GET(heaps) == "native") {
    static NativeHeapAuthority authority(*arena);
    rex::memory::SetHeapAuthority(&authority);
    gh2k::mem::SetGlobalMemory(&authority.memory());   // native threads (gh2k::thread) build their state from the same registry
    fprintf(stderr, "HEAPS native: gh2k::mem decides every allocation of the five address-space heaps\n");
  }
  return true;
}
