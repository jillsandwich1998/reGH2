#include "write_watch.h"

#include <dlfcn.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gh2render {

namespace {

constexpr uint32_t kPageShift = 12;
constexpr uint32_t kPageSize = 1u << kPageShift;
constexpr uint32_t kPhysicalBytes = 0x20000000u;
constexpr uint32_t kPages = kPhysicalBytes >> kPageShift;
constexpr uint64_t kWindowsStart = 0x7F000000ull;   // the first guest address that aliases physical memory (see Arena::Create)
constexpr uint64_t kWindowsEnd = 0x100000000ull;
constexpr uint64_t kX64WriteFault = 2;              // bit of the page fault error code

uint8_t* g_base = nullptr;
std::atomic<bool> g_active{false};
std::atomic<uint32_t> g_epoch{1};
std::atomic<uint8_t> g_armed[kPages];
std::atomic<uint32_t> g_written[kPages];
std::atomic_flag g_lock = ATOMIC_FLAG_INIT;   // protection changes only: a fault must not undo an Arm that is halfway through
std::atomic<uint64_t> g_faults{0}, g_touches{0}, g_armed_pages{0}, g_arm_failures{0};
struct sigaction g_previous;
thread_local uintptr_t t_last_fault = 0;

struct Lock {
  Lock() {
    while (g_lock.test_and_set(std::memory_order_acquire)) {
    }
  }
  ~Lock() { g_lock.clear(std::memory_order_release); }
};

// The physical page behind a guest address, or -1 outside the windows. Physical offsets in 0xE0000000 are 0x1000 ahead of the address.
int64_t PageOfGuest(uint64_t guest) {
  uint64_t physical;
  if (guest >= 0xE0000000ull) physical = guest - 0xE0000000ull + 0x1000;
  else if (guest >= 0xC0000000ull) physical = guest - 0xC0000000ull;
  else if (guest >= 0xA0000000ull) physical = guest - 0xA0000000ull;
  else if (guest >= kWindowsStart && guest < 0x80000000ull) physical = guest - kWindowsStart;
  else return -1;
  return physical < kPhysicalBytes ? static_cast<int64_t>(physical >> kPageShift) : -1;
}

int RawProtect(void* address, size_t bytes, int prot) { return static_cast<int>(syscall(SYS_mprotect, address, bytes, prot)); }

bool Protect(uint32_t first, uint32_t count, int prot) {
  const uint64_t offset = static_cast<uint64_t>(first) << kPageShift;
  const uint64_t bytes = static_cast<uint64_t>(count) << kPageShift;
  bool ok = RawProtect(g_base + 0xA0000000ull + offset, bytes, prot) == 0;
  ok = RawProtect(g_base + 0xC0000000ull + offset, bytes, prot) == 0 && ok;
  if (offset >= 0x1000) ok = RawProtect(g_base + 0xE0000000ull + offset - 0x1000, bytes, prot) == 0 && ok;
  else if (count > 1) ok = RawProtect(g_base + 0xE0000000ull, bytes - 0x1000, prot) == 0 && ok;
  if (offset < 0x1000000) {
    const uint64_t end = offset + bytes < 0x1000000 ? offset + bytes : 0x1000000;
    ok = RawProtect(g_base + kWindowsStart + offset, end - offset, prot) == 0 && ok;
  }
  return ok;
}

// Async-signal safe: atomics and mprotect.
void ReleasePage(uint32_t page) {
  Lock lock;
  if (!g_armed[page].load(std::memory_order_relaxed)) return;
  g_written[page].store(g_epoch.fetch_add(1, std::memory_order_relaxed) + 1, std::memory_order_release);
  g_armed[page].store(0, std::memory_order_relaxed);
  Protect(page, 1, PROT_READ | PROT_WRITE);
}

void Chain(int sig, siginfo_t* info, void* context) {
  if (g_previous.sa_flags & SA_SIGINFO) {
    if (g_previous.sa_sigaction) g_previous.sa_sigaction(sig, info, context);
    return;
  }
  if (g_previous.sa_handler == SIG_IGN) return;
  if (g_previous.sa_handler != SIG_DFL) {
    g_previous.sa_handler(sig);
    return;
  }
  signal(sig, SIG_DFL);   // returning faults again, and this time the process ends as it would have
}

void OnFault(int sig, siginfo_t* info, void* context) {
  const uintptr_t address = reinterpret_cast<uintptr_t>(info->si_addr);
  const uintptr_t base = reinterpret_cast<uintptr_t>(g_base);
  const auto* uc = static_cast<const ucontext_t*>(context);
  if (address >= base + kWindowsStart && address < base + kWindowsEnd && (uc->uc_mcontext.gregs[REG_ERR] & kX64WriteFault)) {
    const int64_t page = PageOfGuest(address - base);
    if (page >= 0) {
      if (g_armed[page].load(std::memory_order_relaxed)) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
        t_last_fault = address;
        ReleasePage(static_cast<uint32_t>(page));
        return;
      }
      if (t_last_fault != address) {   // another thread may have released it between the fault and here: try once more before calling it someone else's fault
        t_last_fault = address;
        return;
      }
    }
  }
  t_last_fault = 0;
  Chain(sig, info, context);
}

}  // namespace

// The kernel does not fault for a system call that writes to a read-only page, it fails the call: file reads give their pages back first.
void Touch(const void* host, size_t bytes) {
  if (!g_active.load(std::memory_order_acquire) || bytes == 0) return;
  const uintptr_t start = reinterpret_cast<uintptr_t>(host);
  const uintptr_t base = reinterpret_cast<uintptr_t>(g_base);
  if (start + bytes <= base + kWindowsStart || start >= base + kWindowsEnd) return;
  const uintptr_t first = (start > base + kWindowsStart ? start : base + kWindowsStart) & ~static_cast<uintptr_t>(kPageSize - 1);
  const uintptr_t end = start + bytes < base + kWindowsEnd ? start + bytes : base + kWindowsEnd;
  for (uintptr_t at = first; at < end; at += kPageSize) {
    const int64_t page = PageOfGuest(at - base);
    if (page >= 0 && g_armed[page].load(std::memory_order_relaxed)) {
      g_touches.fetch_add(1, std::memory_order_relaxed);
      ReleasePage(static_cast<uint32_t>(page));
    }
  }
}

bool WriteWatch::Start(uint8_t* guest_base) {
  if (g_active.load()) return true;
  const char* off = std::getenv("GH2_TEX_WATCH");
  if (!guest_base || (off && off[0] == '0')) return false;
  g_base = guest_base;
  struct sigaction action = {};
  action.sa_sigaction = OnFault;
  action.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGSEGV, &action, &g_previous) != 0) return false;
  g_active.store(true, std::memory_order_release);
  return true;
}

bool WriteWatch::Active() { return g_active.load(std::memory_order_acquire); }

uint32_t WriteWatch::Epoch() { return g_epoch.load(std::memory_order_acquire); }

bool WriteWatch::Arm(uint32_t physical, uint64_t bytes) {
  if (bytes == 0 || physical >= kPhysicalBytes || physical + bytes > kPhysicalBytes) return false;
  const uint32_t first = physical >> kPageShift;
  const uint32_t last = static_cast<uint32_t>((physical + bytes - 1) >> kPageShift);
  for (uint32_t page = first; page <= last;) {
    if (g_armed[page].load(std::memory_order_relaxed)) {
      ++page;
      continue;
    }
    uint32_t end = page;
    while (end <= last && !g_armed[end].load(std::memory_order_relaxed)) ++end;
    Lock lock;
    for (uint32_t i = page; i < end; ++i) g_armed[i].store(1, std::memory_order_relaxed);
    if (!Protect(page, end - page, PROT_READ)) {
      for (uint32_t i = page; i < end; ++i) g_armed[i].store(0, std::memory_order_relaxed);
      Protect(page, end - page, PROT_READ | PROT_WRITE);
      g_arm_failures.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    g_armed_pages.fetch_add(end - page, std::memory_order_relaxed);
    page = end;
  }
  return true;
}

bool WriteWatch::Written(uint32_t physical, uint64_t bytes, uint32_t epoch) {
  if (bytes == 0) return false;
  const uint32_t first = physical >> kPageShift;
  const uint32_t last = static_cast<uint32_t>((physical + bytes - 1) >> kPageShift);
  for (uint32_t page = first; page <= last && page < kPages; ++page)
    if (g_written[page].load(std::memory_order_acquire) > epoch || !g_armed[page].load(std::memory_order_relaxed)) return true;
  return false;
}

WriteWatch::Stats WriteWatch::GetStats() { return {g_faults.load(), g_touches.load(), g_armed_pages.load(), g_arm_failures.load()}; }

}  // namespace gh2render

// Reads into guest memory, by this program and by the runtime's library: the executable's definitions come first for both.
extern "C" {

ssize_t read(int fd, void* buffer, size_t bytes) {
  gh2render::Touch(buffer, bytes);
  return syscall(SYS_read, fd, buffer, bytes);
}

ssize_t pread(int fd, void* buffer, size_t bytes, off_t offset) {
  gh2render::Touch(buffer, bytes);
  return syscall(SYS_pread64, fd, buffer, bytes, offset);
}

ssize_t pread64(int fd, void* buffer, size_t bytes, off_t offset) { return pread(fd, buffer, bytes, offset); }

// The runtime changes protection when memory is freed and allocated again: that would leave a page armed here and writable there, and its next writes unseen.
int mprotect(void* address, size_t bytes, int prot) {
  gh2render::Touch(address, bytes);
  return static_cast<int>(syscall(SYS_mprotect, address, bytes, prot));
}

size_t fread(void* buffer, size_t size, size_t count, FILE* file) {
  static auto real = reinterpret_cast<size_t (*)(void*, size_t, size_t, FILE*)>(dlsym(RTLD_NEXT, "fread"));
  gh2render::Touch(buffer, size * count);
  return real(buffer, size, count, file);
}

}  // extern "C"
