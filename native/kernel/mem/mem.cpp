#include "gh2k/mem.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace gh2k::mem {

namespace {
uint64_t AlignUp(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }
uint64_t AlignDown(uint64_t v, uint64_t a) { return v & ~(a - 1); }

// The 0xE0000000 window's file offset: the console's one page into physical memory where the OS can map it (POSIX), else 64 KB-aligned (Windows; see gh2k/host_addr.h).
#ifdef _WIN32
constexpr uint64_t kEViewFileOffset = 0x100000000ull;
#else
constexpr uint64_t kEViewFileOffset = 0x100001000ull;
#endif

// guest range -> file offset (the runtime's map_info). Every range is a separate host view of the one backing file.
struct View {
  uint64_t start, end, file;
};
const View kViews[] = {
    {0x00000000, 0x3FFFFFFF, 0x000000000}, {0x40000000, 0x7EFFFFFF, 0x040000000}, {0x7F000000, 0x7FFFFFFF, 0x100000000}, {0x80000000, 0x8FFFFFFF, 0x080000000},
    {0x90000000, 0x9FFFFFFF, 0x080000000}, {0xA0000000, 0xBFFFFFFF, 0x100000000}, {0xC0000000, 0xDFFFFFFF, 0x100000000}, {0xE0000000, 0xFFFFFFFF, kEViewFileOffset},
    {0x100000000, 0x11FFFFFFF, 0x100000000}};

#ifdef _WIN32
// ---- Windows: a pagefile-backed section mapped into placeholders of one reservation ----
std::string LastError(const char* what) {
  char buf[256] = {};
  const DWORD code = GetLastError();
  FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, buf, sizeof buf - 1, nullptr);
  for (char* p = buf + std::strlen(buf); p > buf && (p[-1] == '\n' || p[-1] == '\r' || p[-1] == ' '); *--p = 0) {}
  return std::string(what) + ": " + buf + " (" + std::to_string(code) + ")";
}

// VirtualAlloc2 / MapViewOfFile3 / UnmapViewOfFile2 (Windows 10 1803) are looked up at run time: no import library needed.
struct Win10Memory {
  using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
  using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
  using UnmapViewOfFile2Fn = BOOL(WINAPI*)(HANDLE, PVOID, ULONG);
  VirtualAlloc2Fn alloc2 = nullptr;
  MapViewOfFile3Fn map3 = nullptr;
  UnmapViewOfFile2Fn unmap2 = nullptr;
  Win10Memory() {
    HMODULE k = GetModuleHandleW(L"kernelbase.dll");
    if (!k) return;
    alloc2 = reinterpret_cast<VirtualAlloc2Fn>(GetProcAddress(k, "VirtualAlloc2"));
    map3 = reinterpret_cast<MapViewOfFile3Fn>(GetProcAddress(k, "MapViewOfFile3"));
    unmap2 = reinterpret_cast<UnmapViewOfFile2Fn>(GetProcAddress(k, "UnmapViewOfFile2"));
  }
};
const Win10Memory& Win10() {
  static const Win10Memory w;
  return w;
}

DWORD WinProt(uint32_t p) { return p == kNoAccess ? PAGE_NOACCESS : p == kRead ? PAGE_READONLY : PAGE_READWRITE; }

// VirtualProtect works inside ONE view only: cut the range at the view boundaries (the arena's base is needed to know them).
bool ProtectHost(uint8_t* arena_base, uint8_t* p, size_t size, uint32_t protect) {
  const uint64_t lo = static_cast<uint64_t>(p - arena_base), hi = lo + size;
  bool ok = true;
  for (const View& v : kViews) {
    const uint64_t a = std::max<uint64_t>(lo, v.start), b = std::min<uint64_t>(hi, v.end + 1);
    if (a >= b) continue;
    DWORD old;
    if (!VirtualProtect(arena_base + a, static_cast<SIZE_T>(b - a), WinProt(protect), &old)) ok = false;
  }
  return ok;
}
#else
int ProtFlags(uint32_t p) { return p == kNoAccess ? PROT_NONE : p == kRead ? PROT_READ : PROT_READ | PROT_WRITE; }
std::string LastError(const char* what) { return std::string(what) + ": " + std::strerror(errno); }
bool ProtectHost(uint8_t*, uint8_t* p, size_t size, uint32_t protect) { return mprotect(p, size, ProtFlags(protect)) == 0; }
#endif
}  // namespace

// ---- Arena ----
std::unique_ptr<Arena> Arena::Create(std::string* error) {
  auto fail = [&](const char* what) -> std::unique_ptr<Arena> {
    if (error) *error = LastError(what);
    return nullptr;
  };
  std::unique_ptr<Arena> a(new Arena());
#ifdef _WIN32
  const Win10Memory& w = Win10();
  if (!w.alloc2 || !w.map3 || !w.unmap2) {
    if (error) *error = "this Windows has no VirtualAlloc2/MapViewOfFile3 (needs Windows 10 1803 or newer)";
    return nullptr;
  }
  const uint64_t file_size = kReservation;
  HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(file_size >> 32), static_cast<DWORD>(file_size & 0xFFFFFFFFu), nullptr);
  if (!section) return fail("creating the guest memory section");
  a->file_ = reinterpret_cast<FileHandle>(section);
  void* p = w.alloc2(GetCurrentProcess(), nullptr, kReservation, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
  if (!p) return fail("reserving the address space");
  a->base_ = static_cast<uint8_t*>(p);
  // Cut the placeholder into one piece per view, then replace each piece by a view of the section.
  for (const View& v : kViews) {
    const SIZE_T len = static_cast<SIZE_T>(v.end - v.start + 1);
    if (!VirtualFree(a->base_ + v.start, len, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
      // (the first and last pieces need no cut when they already are a whole placeholder)
      MEMORY_BASIC_INFORMATION mbi;
      if (!VirtualQuery(a->base_ + v.start, &mbi, sizeof mbi) || mbi.RegionSize != len) return fail("splitting the reservation");
    }
    if (!w.map3(section, GetCurrentProcess(), a->base_ + v.start, v.file, len, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0)) return fail("mapping a view");
  }
  return a;
#else
  void* p = mmap(nullptr, kReservation, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (p == MAP_FAILED) return fail("reserving the address space");
  a->base_ = static_cast<uint8_t*>(p);
  const int fd = memfd_create("gh2-guest-memory", MFD_CLOEXEC);
  a->file_ = fd;
  if (fd < 0 || ftruncate(fd, kReservation + 0x1000) != 0) return fail("creating the guest memory file");
  for (const View& v : kViews)
    if (mmap(a->base_ + v.start, v.end - v.start + 1, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, static_cast<off_t>(v.file)) == MAP_FAILED) return fail("mapping a view");
  return a;
#endif
}

Arena::~Arena() {
#ifdef _WIN32
  if (base_) {
    const Win10Memory& w = Win10();
    for (const View& v : kViews) w.unmap2(GetCurrentProcess(), base_ + v.start, 0);
  }
  if (file_ != kNoFile) CloseHandle(reinterpret_cast<HANDLE>(file_));
#else
  if (base_) munmap(base_, kReservation);
  if (file_ >= 0) close(static_cast<int>(file_));
#endif
}

// ---- Heap ----
Heap::Heap(uint8_t* host_base, uint32_t start, uint32_t size, uint32_t page_size, bool top_down, bool always_accessible, FileHandle file, int64_t file_delta, bool passive)
    : host_(host_base), start_(start), size_(size), page_size_(page_size), top_down_(top_down), always_accessible_(always_accessible || passive), file_(file), file_delta_(file_delta), passive_(passive) {}

size_t Heap::blocks() const {
  std::lock_guard<std::mutex> lock(mu_);
  return blocks_.size();
}

void Heap::Zero(uint32_t address, uint32_t size) {
  if (passive_) return;
#ifdef _WIN32
  // A section has no "drop these pages": the bytes are written to zero. The range may be inaccessible right now, so it is opened for the write first; the caller sets the
  // protection it wants afterwards (Decommit and Release zero first, then make it inaccessible).
  if (!always_accessible_) ProtectHost(host_, host_ + address, size, kReadWrite);
  std::memset(host_ + address, 0, size);
  (void)file_;
  (void)file_delta_;
#else
  madvise(host_ + address, size, MADV_DONTNEED);
  if (file_ >= 0) fallocate(static_cast<int>(file_), FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, static_cast<off_t>(static_cast<int64_t>(address) + file_delta_), size);   // file-backed: drop the pages
#endif
}

void Heap::Apply(uint32_t address, uint32_t size, uint32_t protect) {
  if (!always_accessible_) ProtectHost(host_, host_ + address, size, protect);
}

bool Heap::FindGap(uint32_t size, uint32_t alignment, uint64_t low, uint64_t end, bool top_down, uint32_t* address) const {
  if (!top_down) {
    uint64_t cursor = low;
    auto it = blocks_.upper_bound(static_cast<uint32_t>(low));
    if (it != blocks_.begin()) {
      auto prev = std::prev(it);
      cursor = std::max<uint64_t>(cursor, static_cast<uint64_t>(prev->first) + prev->second.size);
    }
    for (;; ++it) {
      const uint64_t limit = it == blocks_.end() ? end : std::min<uint64_t>(end, it->first);
      const uint64_t candidate = AlignUp(cursor, alignment);
      if (candidate + size <= limit) {
        *address = static_cast<uint32_t>(candidate);
        return true;
      }
      if (it == blocks_.end() || it->first >= end) return false;
      cursor = std::max<uint64_t>(cursor, static_cast<uint64_t>(it->first) + it->second.size);
    }
  }
  uint64_t upper = end;
  for (auto it = blocks_.rbegin(); it != blocks_.rend(); ++it) {
    const uint64_t b = it->first, block_end = b + it->second.size;
    if (b >= upper) continue;                      // wholly above the window
    if (block_end < upper && upper >= size) {      // the gap between this block and what is above it
      const uint64_t candidate = AlignDown(upper - size, alignment);
      if (candidate >= block_end && candidate >= low) {
        *address = static_cast<uint32_t>(candidate);
        return true;
      }
    }
    upper = b;
    if (upper <= low) return false;
  }
  if (upper < low + size) return false;
  const uint64_t candidate = AlignDown(upper - size, alignment);
  if (candidate < low) return false;
  *address = static_cast<uint32_t>(candidate);
  return true;
}

bool Heap::AllocRange(uint32_t low, uint32_t high, uint32_t size, uint32_t alignment, bool top_down, uint32_t* address) {
  if (size == 0) return false;
  const uint64_t rounded = AlignUp(size, page_size_);
  uint32_t align = std::max(alignment, page_size_);
  if (align & (align - 1)) align = page_size_;
  const uint64_t lo = std::max<uint64_t>(AlignUp(low, align), start_), hi = std::min<uint64_t>(static_cast<uint64_t>(high) + 1, static_cast<uint64_t>(start_) + size_);
  if (hi <= lo || rounded > hi - lo) return false;
  std::lock_guard<std::mutex> lock(mu_);
  uint32_t at;
  if (!FindGap(static_cast<uint32_t>(rounded), align, lo, hi, top_down, &at)) return false;
  Block b;
  b.base = at;
  b.size = static_cast<uint32_t>(rounded);
  b.protect = kNoAccess;
  b.committed = false;
  blocks_[at] = b;
  allocated_ += rounded;
  peak_ = std::max(peak_, allocated_);
  *address = at;
  return true;
}

bool Heap::Alloc(uint32_t size, uint32_t alignment, uint32_t protect, bool commit, uint32_t* address) {
  if (size == 0) return false;
  const uint64_t rounded = AlignUp(size, page_size_);
  if (rounded > size_) return false;
  uint32_t align = std::max(alignment, page_size_);
  if (align & (align - 1)) align = page_size_;   // not a power of two: pages are all we promise
  std::lock_guard<std::mutex> lock(mu_);
  uint32_t at;
  if (!FindGap(static_cast<uint32_t>(rounded), align, start_, static_cast<uint64_t>(start_) + size_, top_down_, &at)) return false;
  Block b;
  b.base = at;
  b.size = static_cast<uint32_t>(rounded);
  b.protect = commit ? protect : kNoAccess;
  b.committed = commit;
  blocks_[at] = b;
  allocated_ += rounded;
  peak_ = std::max(peak_, allocated_);
  if (commit) Apply(at, b.size, protect);
  *address = at;
  return true;
}

bool Heap::AllocFixed(uint32_t address, uint32_t size, uint32_t protect, bool commit) {
  if (size == 0 || address % page_size_ != 0 || address < start_) return false;
  const uint64_t rounded = AlignUp(size, page_size_);
  if (static_cast<uint64_t>(address) + rounded > static_cast<uint64_t>(start_) + size_) return false;
  std::lock_guard<std::mutex> lock(mu_);
  auto next = blocks_.lower_bound(address);
  if (next != blocks_.end() && next->first < address + rounded) return false;
  if (next != blocks_.begin()) {
    auto prev = std::prev(next);
    if (static_cast<uint64_t>(prev->first) + prev->second.size > address) return false;
  }
  Block b;
  b.base = address;
  b.size = static_cast<uint32_t>(rounded);
  b.protect = commit ? protect : kNoAccess;
  b.committed = commit;
  blocks_[address] = b;
  allocated_ += rounded;
  peak_ = std::max(peak_, allocated_);
  if (commit) Apply(address, b.size, protect);
  return true;
}

bool Heap::Query(uint32_t address, Block* block) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = blocks_.upper_bound(address);
  if (it == blocks_.begin()) return false;
  --it;
  if (address >= static_cast<uint64_t>(it->first) + it->second.size) return false;
  *block = it->second;
  return true;
}

bool Heap::Commit(uint32_t address, uint32_t size, uint32_t protect) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = blocks_.upper_bound(address);
  if (it == blocks_.begin()) return false;
  --it;
  const uint64_t end = static_cast<uint64_t>(address) + AlignUp(size, page_size_);
  if (address < it->first || end > static_cast<uint64_t>(it->first) + it->second.size) return false;
  Apply(address, static_cast<uint32_t>(end - address), protect);
  it->second.committed = true;
  it->second.protect = protect;
  return true;
}

bool Heap::Decommit(uint32_t address, uint32_t size) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = blocks_.upper_bound(address);
  if (it == blocks_.begin()) return false;
  --it;
  const uint64_t end = static_cast<uint64_t>(address) + AlignUp(size, page_size_);
  if (address < it->first || end > static_cast<uint64_t>(it->first) + it->second.size) return false;
  if (!always_accessible_) {
    Zero(address, static_cast<uint32_t>(end - address));   // (before the protection: a Windows section is zeroed by writing)
    ProtectHost(host_, host_ + address, end - address, kNoAccess);
  }
  if (address == it->first && end == static_cast<uint64_t>(it->first) + it->second.size) it->second.committed = false;
  return true;
}

bool Heap::Protect(uint32_t address, uint32_t size, uint32_t protect) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = blocks_.upper_bound(address);
  if (it == blocks_.begin()) return false;
  --it;
  const uint64_t end = static_cast<uint64_t>(address) + AlignUp(size, page_size_);
  if (address < it->first || end > static_cast<uint64_t>(it->first) + it->second.size) return false;
  Apply(address, static_cast<uint32_t>(end - address), protect);
  it->second.protect = protect;
  return true;
}

bool Heap::Release(uint32_t address, uint32_t* released_size) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = blocks_.find(address);
  if (it == blocks_.end()) return false;
  if (released_size) *released_size = it->second.size;
  if (!always_accessible_) {
    Zero(address, it->second.size);
    ProtectHost(host_, host_ + address, it->second.size, kNoAccess);
  } else {
    Zero(address, it->second.size);
  }
  allocated_ -= it->second.size;
  blocks_.erase(it);
  return true;
}

// ---- Memory ----
std::unique_ptr<Memory> Memory::Over(Arena& arena) {
  std::unique_ptr<Memory> m(new Memory());
  m->arena_ = &arena;
  uint8_t* b = arena.base();
  // the same ranges the runtime's heaps cover; the physical heap spans the whole 512 MB (the windows' limits come with the ranges the runtime asks for)
  m->virtual4k_ = std::make_unique<Heap>(b, 0x00000000u, 0x40000000u, 4096, false, false, -1, 0, true);
  m->virtual64k_ = std::make_unique<Heap>(b, 0x40000000u, 0x3F000000u, 64 * 1024, false, false, -1, 0, true);
  m->xex64k_ = std::make_unique<Heap>(b, 0x80000000u, 0x10000000u, 64 * 1024, false, false, -1, 0, true);
  m->xex4k_ = std::make_unique<Heap>(b, 0x90000000u, 0x10000000u, 4096, false, false, -1, 0, true);
  m->physical_ = std::make_unique<Heap>(arena.physical(), 0x00000000u, 0x20000000u, 4096, true, true, -1, 0, true);
  return m;
}

std::unique_ptr<Memory> Memory::Create(std::string* error) {
  std::unique_ptr<Memory> m(new Memory());
  m->owned_ = Arena::Create(error);
  if (!m->owned_) return nullptr;
  m->arena_ = m->owned_.get();
  uint8_t* b = m->arena_->base();
  const FileHandle fd = m->arena_->file();
  m->virtual4k_ = std::make_unique<Heap>(b, 0x00000000u, 0x40000000u, 4096, false, false, fd, 0);
  m->virtual64k_ = std::make_unique<Heap>(b, 0x40000000u, 0x3F000000u, 64 * 1024, false, false, fd, 0);
  m->xex64k_ = std::make_unique<Heap>(b, 0x80000000u, 0x10000000u, 64 * 1024, false, false, fd, 0);
  m->xex4k_ = std::make_unique<Heap>(b, 0x90000000u, 0x10000000u, 4096, false, false, fd, -0x10000000ll);
  m->physical_ = std::make_unique<Heap>(m->arena_->physical(), 0x00000000u, 0x1FD00000u + kEWindowSkew, 4096, true, true, fd, 0x100000000ll);   // physical bytes; all windows allocate below what the 4 KB one reaches
  for (uint32_t start : {0x00000000u, 0x40000000u}) ProtectHost(b, b + start, start ? 0x3F000000u : 0x40000000u, kNoAccess);   // reserved until committed
  ProtectHost(b, b + 0x80000000u, 0x20000000u, kNoAccess);
  m->virtual4k_->AllocFixed(0, 0x10000, kNoAccess, false);                       // the null page region
  return m;
}

Heap* Memory::HeapFor(uint32_t a) {
  if (a < 0x40000000u) return virtual4k_.get();
  if (a < 0x7F000000u) return virtual64k_.get();
  if (a >= 0x80000000u && a < 0x90000000u) return xex64k_.get();
  if (a >= 0x90000000u && a < 0xA0000000u) return xex4k_.get();
  return nullptr;
}

uint32_t Memory::PhysicalOffset(uint32_t a) { return a >= 0xE0000000u ? a - 0xE0000000u + kEWindowSkew : a >= 0xC0000000u ? a - 0xC0000000u : a - 0xA0000000u; }

bool Memory::PhysicalAlloc(uint32_t size, uint32_t alignment, uint32_t view_page_size, uint32_t protect, uint32_t* guest_address) {
  const uint32_t view = view_page_size >= 16u * 1024 * 1024 ? 0xC0000000u : view_page_size >= 64 * 1024 ? 0xA0000000u : 0xE0000000u;
  const uint32_t page = view == 0xC0000000u ? 16u * 1024 * 1024 : view == 0xA0000000u ? 64 * 1024 : 4096;
  const uint64_t rounded = AlignUp(size, page);
  if (rounded > 0xFFFFFFFFull) return false;
  uint32_t offset;
  if (!physical_->Alloc(static_cast<uint32_t>(rounded), std::max(alignment, page), protect, true, &offset)) return false;
  if (view == 0xE0000000u && offset < kEWindowSkew) {   // below what the 4 KB window can address
    physical_->Release(offset);
    return false;
  }
  *guest_address = view == 0xE0000000u ? view + offset - kEWindowSkew : view + offset;
  return true;
}

bool Memory::PhysicalFree(uint32_t a, uint32_t* released) {
  if (a < 0xA0000000u) return false;
  uint32_t size = 0;
  const uint32_t offset = PhysicalOffset(a);
  if (!physical_->Release(offset, &size)) return false;
  if (released) *released = size;
#ifdef _WIN32
  std::memset(arena_->physical() + offset, 0, size);   // freed memory reads back as zero (the physical view is always read/write)
#else
  fallocate(static_cast<int>(arena_->file()), FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, static_cast<off_t>(0x100000000ull + offset), size);   // freed memory reads back as zero
#endif
  return true;
}

uint32_t Memory::PhysicalAllocationSize(uint32_t a) const {
  if (a < 0xA0000000u) return 0;
  Block b;
  if (!physical_->Query(PhysicalOffset(a), &b) || b.base != PhysicalOffset(a)) return 0;
  return b.size;
}

static Memory* g_global_memory = nullptr;
Memory* GlobalMemory() { return g_global_memory; }
void SetGlobalMemory(Memory* memory) { g_global_memory = memory; }

bool Memory::AllocStack(uint32_t size, Stack* s) {
  const uint32_t page = virtual64k_->page_size();
  const uint32_t usable = static_cast<uint32_t>(AlignUp(std::max<uint32_t>(size, 16 * 1024), page));
  const uint32_t total = usable + 2 * page;
  uint32_t at;
  if (!virtual64k_->AllocRange(0x70000000u, 0x7EFFFFFFu, total, page, false, &at)) return false;   // the range the runtime gives title threads' stacks
  // host protection is done here, not by the (possibly passive) heap: guards inaccessible, the stack itself read/write
  uint8_t* const b = arena_->base();
  ProtectHost(b, b + at, page, kNoAccess);
  ProtectHost(b, b + at + page + usable, page, kNoAccess);
  if (!ProtectHost(b, b + at + page, usable, kReadWrite)) {
    virtual64k_->Release(at);
    return false;
  }
  std::memset(arena_->base() + at + page, 0xBE, usable);
  s->alloc_base = at;
  s->alloc_size = total;
  s->limit = at + page;
  s->top = at + page + usable;
  return true;
}

bool Memory::FreeStack(const Stack& s) {
  uint8_t* const b = arena_->base();
#ifdef _WIN32
  ProtectHost(b, b + s.alloc_base, s.alloc_size, kReadWrite);   // (zeroing is a write; the guard pages are part of the allocation)
  std::memset(b + s.alloc_base, 0, s.alloc_size);
  ProtectHost(b, b + s.alloc_base, s.alloc_size, kNoAccess);
#else
  ProtectHost(b, b + s.alloc_base, s.alloc_size, kNoAccess);
  madvise(b + s.alloc_base, s.alloc_size, MADV_DONTNEED);
#endif
  return virtual64k_->Release(s.alloc_base);
}

}  // namespace gh2k::mem
