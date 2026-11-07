// The write watch on a reservation laid out like the guest's (see Arena::Create): the windows alias, a write through any of them is seen, and system calls still work on armed pages.
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

#include "write_watch.h"

using gh2render::WriteWatch;

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++failures;
}

constexpr uint64_t kReservation = 0x120000000ull;
constexpr uint64_t kPhysicalFile = 0x100000000ull;

uint8_t* MakeGuest() {
  auto* base = static_cast<uint8_t*>(mmap(nullptr, kReservation, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
  const int fd = memfd_create("write-watch-selftest", MFD_CLOEXEC);
  if (base == MAP_FAILED || fd < 0 || ftruncate(fd, kReservation + 0x1000) != 0) return nullptr;
  const struct { uint64_t start, size, file; } views[] = {{0x7F000000, 0x01000000, kPhysicalFile}, {0xA0000000, 0x20000000, kPhysicalFile}, {0xC0000000, 0x20000000, kPhysicalFile},
                                                         {0xE0000000, 0x20000000, kPhysicalFile + 0x1000}, {0x100000000, 0x20000000, kPhysicalFile}};
  for (const auto& v : views)
    if (mmap(base + v.start, v.size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, static_cast<off_t>(v.file)) == MAP_FAILED) return nullptr;
  return base;
}

// Arms one page, writes through `window`, and checks what the renderer would see.
void WriteThrough(uint8_t* base, uint32_t physical, uint64_t window, const char* what) {
  const uint32_t epoch = WriteWatch::Epoch();
  Check(WriteWatch::Arm(physical, 4096), "arming a page");
  Check(!WriteWatch::Written(physical, 4096, epoch), "an armed page nobody wrote reads as unwritten");
  *reinterpret_cast<volatile uint8_t*>(base + window + 16) = 0x5A;
  Check(WriteWatch::Written(physical, 4096, epoch), what);
  Check(base[0x100000000ull + physical + 16] == 0x5A, "the write reached memory");
}

}  // namespace

int main() {
  uint8_t* base = MakeGuest();
  Check(base != nullptr, "the guest reservation");
  if (!base) return 1;
  Check(WriteWatch::Start(base), "the fault handler is installed");

  WriteThrough(base, 0x00100000, 0xA0000000ull + 0x00100000, "a write through 0xA0000000 is seen");
  WriteThrough(base, 0x00200000, 0xC0000000ull + 0x00200000, "a write through 0xC0000000 is seen");
  WriteThrough(base, 0x00300000, 0xE0000000ull + 0x00300000 - 0x1000, "a write through 0xE0000000 (0x1000 behind) is seen");
  WriteThrough(base, 0x00400000, 0x7F000000ull + 0x00400000, "a write through 0x7F000000 is seen");

  const uint32_t neighbour = 0x00500000;
  uint32_t epoch = WriteWatch::Epoch();
  WriteWatch::Arm(neighbour, 8192);
  base[0xA0000000ull + neighbour + 4096] = 1;
  Check(WriteWatch::Written(neighbour, 8192, epoch) && !WriteWatch::Written(neighbour, 4096, epoch), "only the written page is reported");

  int pipes[2];
  Check(pipe(pipes) == 0 && write(pipes[1], "abcd", 4) == 4, "a pipe to read from");
  const uint32_t target = 0x00600000;
  epoch = WriteWatch::Epoch();
  WriteWatch::Arm(target, 4096);
  Check(read(pipes[0], base + 0xA0000000ull + target, 4) == 4, "read(2) into an armed page succeeds");
  Check(WriteWatch::Written(target, 4096, epoch) && std::memcmp(base + 0x100000000ull + target, "abcd", 4) == 0, "and the page is reported written");

  const uint32_t reprotected = 0x00700000;
  epoch = WriteWatch::Epoch();
  WriteWatch::Arm(reprotected, 4096);
  Check(mprotect(base + 0xA0000000ull + reprotected, 4096, PROT_READ | PROT_WRITE) == 0, "someone else changes the protection of an armed page");
  Check(WriteWatch::Written(reprotected, 4096, epoch), "the page counts as written, its later writes would not fault");

  std::printf("%s\n", failures ? "FAILED" : "all passed");
  return failures ? 1 : 0;
}
