#include <gh2gfx/d3d_probe.h>

#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gh2gfx::d3d {

namespace detail {
bool g_probe_on = false;
}

namespace {

constexpr uint32_t kPerFunctionCap = 1500;      // calls recorded per function outside the window (entry + exit each)
constexpr size_t kRingEvents = 1u << 21;        // 128 MB
constexpr uint16_t kFrameMarkFunction = 0xFFFF;
constexpr uint32_t kDeviceCommandPointer = 48;  // D3DDevice: address of the last dword written to the command buffer (guest_d3d.h)

struct Recorder {
  std::string prefix;
  std::unique_ptr<ProbeEvent[]> ring;
  std::atomic<uint32_t> seq{0};
  std::atomic<uint32_t> written{0};             // events flushed to disk
  std::vector<std::atomic<uint32_t>> calls;     // per function, capped counting (decides what is recorded outside the window)
  std::vector<std::atomic<uint64_t>> total;     // per function, exact
  std::atomic<uint32_t> frame{0};
  std::atomic<uint32_t> device{0};
  uint32_t window_first = 0xFFFFFFFFu, window_frames = 0;
  std::thread flusher;
  std::atomic<bool> stop{false};
  FILE* events = nullptr;
  FILE* dumps = nullptr;
  FILE* pm4 = nullptr;
  std::mutex side_mutex;                        // dumps and pm4 (window mode only)

  Recorder() : calls(kProbeFunctionCount), total(kProbeFunctionCount) {
    prefix = getenv("GH2_D3D_PROBE");
    if (const char* w = getenv("GH2_D3D_PROBE_WINDOW")) {
      unsigned a = 0, b = 0;
      if (sscanf(w, "%u:%u", &a, &b) == 2) {
        window_first = a;
        window_frames = b;
      }
    }
    ring = std::make_unique<ProbeEvent[]>(kRingEvents);
    if (FILE* f = fopen((prefix + ".functions").c_str(), "w")) {
      for (uint32_t i = 0; i < kProbeFunctionCount; ++i) fprintf(f, "%u %08X\n", i, kProbeFunctions[i]);
      fclose(f);
    }
    events = fopen((prefix + ".events").c_str(), "wb");
    if (window_frames) {
      dumps = fopen((prefix + ".dumps").c_str(), "wb");
      pm4 = fopen((prefix + ".pm4").c_str(), "wb");
    }
    flusher = std::thread([this] {
      while (!stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        Flush();
      }
    });
    fprintf(stderr, "D3DPROBE recording the %u functions of the D3D block to %s.* (first %u calls of each%s)\n", kProbeFunctionCount, prefix.c_str(),
            kPerFunctionCap, window_frames ? "; every call, with pointer dumps and PM4, in frames " : "");
    if (window_frames) fprintf(stderr, "D3DPROBE window: frames %u to %u\n", window_first, window_first + window_frames - 1);
  }
  ~Recorder() {
    stop = true;
    if (flusher.joinable()) flusher.join();
    Flush();
    if (events) fclose(events);
    if (dumps) fclose(dumps);
    if (pm4) fclose(pm4);
  }
  bool InWindow() const {
    const uint32_t f = frame.load(std::memory_order_relaxed);
    return window_frames && f >= window_first && f < window_first + window_frames;
  }
  void Flush() {
    if (events) {
      const uint32_t end = std::min<uint32_t>(seq.load(std::memory_order_acquire), kRingEvents);
      uint32_t from = written.load();
      if (end > from) {
        fwrite(&ring[from], sizeof(ProbeEvent), end - from, events);
        fflush(events);
        written.store(end);
      }
    }
    {
      std::lock_guard<std::mutex> lock(side_mutex);
      if (dumps) fflush(dumps);
      if (pm4) fflush(pm4);
    }
    if (FILE* f = fopen((prefix + ".counts").c_str(), "w")) {
      for (uint32_t i = 0; i < kProbeFunctionCount; ++i) {
        const uint64_t n = total[i].load(std::memory_order_relaxed);
        if (n) fprintf(f, "%u %08X %llu\n", i, kProbeFunctions[i], static_cast<unsigned long long>(n));
      }
      fprintf(f, "frames %u\n", frame.load());
      if (window_frames) fprintf(f, "window %u %u\n", window_first, window_frames);
      fclose(f);
    }
  }
};

Recorder* g_rec = nullptr;

// Reads guest memory without risking a fault (the guest's address space is only partly committed).
bool SafeRead(const uint8_t* base, uint32_t addr, void* out, size_t n) {
  if (addr < 0x10000 || addr >= 0xFFFF0000u) return false;
  iovec local{out, n};
  iovec remote{const_cast<uint8_t*>(base) + addr, n};
  return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(n);
}
bool SafeReadBE32(const uint8_t* base, uint32_t addr, uint32_t* out) {
  if ((addr & 3) != 0) return false;
  uint32_t v;
  if (!SafeRead(base, addr, &v, sizeof(v))) return false;
  *out = __builtin_bswap32(v);
  return true;
}

uint8_t ThreadNumber() {
  static std::atomic<uint8_t> next{0};
  thread_local uint8_t mine = next.fetch_add(1);
  return mine;
}

// window mode: the command pointer at the entry of every nested call of this thread
thread_local std::vector<uint32_t> t_cmd_stack;

uint32_t Record(uint16_t index, uint8_t phase, uint32_t ret, const uint32_t (&args)[6], const uint8_t* base) {
  Recorder& r = *g_rec;
  const uint32_t n = r.seq.fetch_add(1, std::memory_order_acq_rel);
  if (n >= kRingEvents) return n;
  ProbeEvent& e = r.ring[n];
  e.seq = n;
  e.function = index;
  e.phase = phase;
  e.thread = ThreadNumber();
  std::memcpy(e.args, args, sizeof(e.args));
  e.ret = ret;
  for (int i = 0; i < 6; ++i) {
    uint32_t v;
    e.deref[i] = SafeReadBE32(base, args[i], &v) ? v : 0xFFFFFFFFu;
  }
  e.reserved = r.frame.load(std::memory_order_relaxed);
  return n;
}

void DumpPointers(uint32_t seq, uint8_t phase, const uint32_t (&args)[6], const uint8_t* base) {
  Recorder& r = *g_rec;
  const uint32_t device = r.device.load(std::memory_order_relaxed);
  for (uint8_t i = 0; i < 6; ++i) {
    if (args[i] == device || (args[i] & 3) != 0) continue;
    ProbeDump d{};
    d.seq = seq;
    d.phase = phase;
    d.arg = i;
    if (!SafeRead(base, args[i], d.bytes, sizeof(d.bytes))) continue;
    std::lock_guard<std::mutex> lock(r.side_mutex);
    if (r.dumps) fwrite(&d, sizeof(d), 1, r.dumps);
  }
}

// Constructed before main(): the environment decides, the generated hooks only test a bool.
struct Init {
  Init() {
    const char* p = getenv("GH2_D3D_PROBE");
    if (p && *p) {
      g_rec = new Recorder();
      detail::g_probe_on = true;
    }
  }
} g_init;

}  // namespace

void ProbeEnter(uint32_t index, const uint32_t (&args)[6], const uint8_t* base) {
  if (!g_rec) return;
  g_rec->total[index].fetch_add(1, std::memory_order_relaxed);
  const bool window = g_rec->InWindow();
  if (window) {
    uint32_t cmd = 0;
    const uint32_t device = g_rec->device.load(std::memory_order_relaxed);
    if (device) SafeReadBE32(base, device + kDeviceCommandPointer, &cmd);
    t_cmd_stack.push_back(cmd);
    g_rec->calls[index].fetch_add(1, std::memory_order_relaxed);
    const uint32_t seq = Record(static_cast<uint16_t>(index), 0, 0, args, base);
    DumpPointers(seq, 0, args, base);
    return;
  }
  if (g_rec->calls[index].fetch_add(1, std::memory_order_relaxed) >= kPerFunctionCap) return;
  Record(static_cast<uint16_t>(index), 0, 0, args, base);
}

void ProbeLeave(uint32_t index, uint32_t ret, const uint32_t (&args)[6], const uint8_t* base) {
  if (!g_rec) return;
  if (!t_cmd_stack.empty() && g_rec->InWindow()) {
    const uint32_t cmd0 = t_cmd_stack.back();
    t_cmd_stack.pop_back();
    const uint32_t seq = Record(static_cast<uint16_t>(index), 1, ret, args, base);
    DumpPointers(seq, 1, args, base);
    uint32_t cmd1 = 0;
    const uint32_t device = g_rec->device.load(std::memory_order_relaxed);
    if (device && cmd0 && SafeReadBE32(base, device + kDeviceCommandPointer, &cmd1) && cmd1 > cmd0 && cmd1 - cmd0 <= 16384) {
      std::vector<uint32_t> words((cmd1 - cmd0) / 4);
      if (SafeRead(base, cmd0 + 4, words.data(), words.size() * 4)) {
        ProbePm4Header h{seq, cmd0 + 4, static_cast<uint32_t>(words.size())};
        std::lock_guard<std::mutex> lock(g_rec->side_mutex);
        if (g_rec->pm4) {
          fwrite(&h, sizeof(h), 1, g_rec->pm4);
          fwrite(words.data(), 4, words.size(), g_rec->pm4);
        }
      }
    }
    return;
  }
  if (g_rec->calls[index].load(std::memory_order_relaxed) > kPerFunctionCap) return;
  Record(static_cast<uint16_t>(index), 1, ret, args, base);
}

void ProbeFrameMark(const uint32_t (&args)[6], const uint8_t* base) {
  if (!g_rec) return;
  const uint32_t frame = g_rec->frame.fetch_add(1, std::memory_order_acq_rel) + 1;
  uint32_t a[6] = {frame, args[0], args[1], args[2], args[3], args[4]};
  // one extra argument is worth keeping: the swap's fetch constant pointer is args[1]; Record() dereferences all of them
  const uint32_t seq = Record(kFrameMarkFunction, 2, frame, a, base);
  if (g_rec->InWindow()) DumpPointers(seq, 2, a, base);
}

uint32_t ProbeIndexOf(uint32_t address) {
  const uint32_t* end = kProbeFunctions + kProbeFunctionCount;
  const uint32_t* it = std::lower_bound(kProbeFunctions, end, address);
  return it != end && *it == address ? static_cast<uint32_t>(it - kProbeFunctions) : 0xFFFFFFFFu;
}

void ProbeSetDevice(uint32_t device) {
  if (g_rec) g_rec->device.store(device, std::memory_order_relaxed);
}

}  // namespace gh2gfx::d3d
