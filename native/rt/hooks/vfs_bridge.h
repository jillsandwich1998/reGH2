// vfs_bridge.h - the hooks' view of the native VFS (kernel/vfs): which mode runs, the mounted file system, and the counters of the shadow comparison.
// Host side only (no guest macros); no SDK header, no `rex::` (native/tools/check_isolation.sh).
#pragma once

#include <gh2k/vfs.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <chrono>

namespace gh2rt::hooks::vfsbridge {

enum class Mode { kSdk, kShadow, kNative };

struct Counters {
  std::atomic<uint64_t> creates{0}, creates_claimed{0}, creates_native{0}, creates_forwarded{0}, closes{0};
  std::atomic<uint64_t> reads{0}, reads_compared{0}, bytes_compared{0}, read_mismatches{0}, read_status_mismatches{0}, reads_unmapped{0};
  std::atomic<uint64_t> reads_with_event{0}, reads_with_apc{0}, reads_own_position{0}, reads_native{0}, bytes_native{0};
  std::atomic<uint64_t> queries{0}, queries_native{0}, query_mismatches{0}, queries_unmapped{0};
  std::atomic<uint64_t> query_class[64] = {};
  std::atomic<uint64_t> unclaimed_paths{0};
  std::atomic<uint64_t> reads_pending{0}, pending_compared{0}, pending_lost{0}, pending_max_ms{0};
};

Mode GetMode();                 // resolved once: GH2_VFS=sdk|shadow|native overrides the --vfs option
gh2k::Vfs* Get();               // null in sdk mode or when there is no game folder
Counters& Stats();

// shadow mode: the runtime's handle -> the native handle opened for the same path
void ShadowMap(uint32_t sdk_handle, uint32_t native_handle);
uint32_t ShadowLookup(uint32_t sdk_handle);   // 0 = none
uint32_t ShadowTake(uint32_t sdk_handle);     // lookup + forget

// A read the runtime answered with STATUS_PENDING: it completes later (the io status block's first dword leaves 0x103), and only then is the buffer filled.
// The native read was done at issue time; a checker thread compares the two the moment the runtime reports completion.
struct PendingRead {
  const volatile uint32_t* iosb = nullptr;   // host pointer to the guest IO_STATUS_BLOCK (big endian)
  const uint8_t* buffer = nullptr;           // host pointer to the guest buffer
  std::vector<uint8_t> expected;             // what the native VFS read
  uint32_t expected_status = 0;
  uint64_t offset = 0;
  std::chrono::steady_clock::time_point issued;
};
void AddPending(PendingRead read);

// Remembers a path that no mount claims (first few only, for the report).
void NoteUnclaimed(const char* path);

}  // namespace gh2rt::hooks::vfsbridge
