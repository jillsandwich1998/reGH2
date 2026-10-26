// Boundary tracer core (see gh2rt/boundary_trace.h). No SDK dependency.
#include <gh2rt/boundary_trace.h>

#include <fcntl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gh2rt::trace {

extern const char* const kImportNames[];  // import_trace.gen.cpp
extern const size_t kImportNameCount;

namespace detail {
bool g_enabled = false;
}

namespace {

constexpr size_t kMaxIds = 512;

struct Event {
  uint64_t t_ns;   // since the trace started
  uint32_t seq;    // global order of recording
  uint32_t tid;    // OS thread id
  uint16_t id;     // index in the name table
  uint16_t flags;  // bit0 hook, bit1 pseudo
  uint32_t a[6];   // r3..r8 on entry (pseudo: a[0] address, a[1] value)
  uint32_t ret;    // r3 on exit
};
static_assert(sizeof(Event) == 48, "trace event layout is part of the file format");

// Constant-initialised, so usable whatever the order of static construction.
std::atomic<uint64_t> g_counts[kMaxIds];
std::atomic<uint64_t> g_recorded[kMaxIds];
std::atomic<uint32_t> g_seq{0};
std::atomic<bool> g_stop{false};

// Everything with a constructor lives here, built on first use (Init() runs before ordinary static constructors).
struct State {
  std::mutex mutex;
  std::vector<Event> buffer;
  std::vector<std::string> names;
  std::map<uint32_t, std::string> thread_names;
  std::thread flusher;
  std::string path;
  int fd = -1;
  uint64_t t0_ns = 0;
  uint64_t events_written = 0;
  uint64_t full = 2000;
  uint64_t sample = 128;
};
State& S() {
  static State s;
  return s;
}

uint64_t NowNs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

uint32_t ThreadId() {
  thread_local const uint32_t tid = static_cast<uint32_t>(syscall(SYS_gettid));
  return tid;
}

void WriteAll(int fd, const void* data, size_t n) {
  const char* p = static_cast<const char*>(data);
  while (n) {
    const ssize_t w = write(fd, p, n);
    if (w <= 0) return;
    p += w;
    n -= static_cast<size_t>(w);
  }
}

std::string ReadComm(uint32_t tid) {
  char path[64], comm[32] = {};
  snprintf(path, sizeof(path), "/proc/self/task/%u/comm", tid);
  if (FILE* f = fopen(path, "r")) {
    if (fgets(comm, sizeof(comm), f)) {
    }
    fclose(f);
  }
  std::string s(comm);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  return s;
}

std::string JsonEscape(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    o += (static_cast<unsigned char>(c) < 32) ? ' ' : c;
  }
  return o;
}

void WriteAtomically(const std::string& path, const std::string& text) {
  const std::string tmp = path + ".tmp";
  if (FILE* f = fopen(tmp.c_str(), "w")) {
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    rename(tmp.c_str(), path.c_str());
  }
}

void WriteCounts() {
  State& s = S();
  std::string j = "{\"events_written\":" + std::to_string(s.events_written) + ",\"counts\":{";
  bool first = true;
  for (size_t i = 0; i < s.names.size(); ++i) {
    const uint64_t c = g_counts[i].load(std::memory_order_relaxed);
    if (!c) continue;
    j += std::string(first ? "" : ",") + "\"" + JsonEscape(s.names[i]) + "\":[" + std::to_string(c) + "," +
         std::to_string(g_recorded[i].load(std::memory_order_relaxed)) + "]";
    first = false;
  }
  j += "},\"threads\":{";
  first = true;
  for (const auto& [tid, name] : s.thread_names) {
    j += std::string(first ? "" : ",") + "\"" + std::to_string(tid) + "\":\"" + JsonEscape(name) + "\"";
    first = false;
  }
  j += "}}\n";
  WriteAtomically(s.path + ".counts.json", j);
}

void FlushOnce() {
  State& s = S();
  std::vector<Event> local;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    local.swap(s.buffer);
    s.buffer.reserve(1u << 14);
  }
  for (const Event& e : local) {
    if (!s.thread_names.count(e.tid)) s.thread_names[e.tid] = ReadComm(e.tid);
  }
  if (!local.empty()) {
    WriteAll(s.fd, local.data(), local.size() * sizeof(Event));
    s.events_written += local.size();
  }
}

void FlusherLoop() {
  int ticks = 0;
  while (!g_stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    FlushOnce();
    if (++ticks % 40 == 0) WriteCounts();
  }
}

void Shutdown() {
  if (!detail::g_enabled) return;
  State& s = S();
  g_stop = true;
  if (s.flusher.joinable()) s.flusher.join();
  FlushOnce();
  WriteCounts();
  if (s.fd >= 0) close(s.fd);
  s.fd = -1;
  detail::g_enabled = false;
}

uint64_t EnvU64(const char* name, uint64_t fallback) {
  const char* e = getenv(name);
  return (e && *e) ? strtoull(e, nullptr, 10) : fallback;
}

__attribute__((constructor(101))) void Init() {
  const char* path = getenv("GH2_BOUNDARY_TRACE");
  if (!path || !*path) return;
  State& s = S();
  s.names.assign(kImportNames, kImportNames + kImportNameCount);
  s.names.push_back(kPseudoMmioLoad);
  s.names.push_back(kPseudoMmioStore);
  s.names.push_back(kPseudoResolveIndirect);
  s.path = path;
  s.fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (s.fd < 0) {
    perror("GH2_BOUNDARY_TRACE: open");
    return;
  }
  s.full = EnvU64("GH2_BOUNDARY_TRACE_FULL", 2000);
  s.sample = EnvU64("GH2_BOUNDARY_TRACE_SAMPLE", 128);
  if (s.sample == 0) s.sample = 1;
  s.t0_ns = NowNs();
  s.buffer.reserve(1u << 14);

  std::string argv;
  if (FILE* f = fopen("/proc/self/cmdline", "r")) {
    char buf[4096];
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    for (size_t i = 0; i < n; ++i) argv += buf[i] ? buf[i] : ' ';
  }
  std::string meta = "{\"version\":1,\"event_size\":48,\"full\":" + std::to_string(s.full) + ",\"sample\":" +
                     std::to_string(s.sample) + ",\"pid\":" + std::to_string(getpid()) + ",\"argv\":\"" + JsonEscape(argv) +
                     "\",\"imports\":[";
  for (size_t i = 0; i < s.names.size(); ++i) meta += std::string(i ? "," : "") + "\"" + JsonEscape(s.names[i]) + "\"";
  meta += "]}\n";
  WriteAtomically(s.path + ".meta.json", meta);

  detail::g_enabled = true;
  s.flusher = std::thread(FlusherLoop);
  atexit(Shutdown);
}

}  // namespace

uint16_t IdOf(const char* name) {
  // The table is static data, usable even when tracing is off (hooks resolve their ids once, at their first call).
  for (size_t i = 0; i < kImportNameCount; ++i)
    if (std::strcmp(kImportNames[i], name) == 0) return static_cast<uint16_t>(i);
  if (std::strcmp(name, kPseudoMmioLoad) == 0) return static_cast<uint16_t>(kImportNameCount);
  if (std::strcmp(name, kPseudoMmioStore) == 0) return static_cast<uint16_t>(kImportNameCount + 1);
  if (std::strcmp(name, kPseudoResolveIndirect) == 0) return static_cast<uint16_t>(kImportNameCount + 2);
  return kUnknownId;
}

void Record(uint16_t id, const uint32_t (&args)[6], uint32_t ret, uint16_t flags) {
  if (!detail::g_enabled || id >= kMaxIds) return;
  State& s = S();
  const uint64_t n = g_counts[id].fetch_add(1, std::memory_order_relaxed);
  if (n >= s.full && ((n - s.full) % s.sample) != 0) return;
  g_recorded[id].fetch_add(1, std::memory_order_relaxed);
  Event e;
  e.t_ns = NowNs() - s.t0_ns;
  e.seq = g_seq.fetch_add(1, std::memory_order_relaxed);
  e.tid = ThreadId();
  e.id = id;
  e.flags = flags;
  std::memcpy(e.a, args, sizeof(e.a));
  e.ret = ret;
  std::lock_guard<std::mutex> lock(s.mutex);
  s.buffer.push_back(e);
}

void RecordPseudo(uint16_t id, uint32_t a0, uint32_t a1, uint32_t ret) {
  const uint32_t args[6] = {a0, a1, 0, 0, 0, 0};
  Record(id, args, ret, 2);
}

}  // namespace gh2rt::trace
