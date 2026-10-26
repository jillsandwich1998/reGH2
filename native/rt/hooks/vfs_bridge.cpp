#include "vfs_bridge.h"

#include <gh2rt/host_services.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace gh2rt::hooks::vfsbridge {

namespace {

struct State {
  Mode mode = Mode::kSdk;
  std::unique_ptr<gh2k::Vfs> vfs;
  Counters counters;
  std::mutex mu;
  std::unordered_map<uint32_t, uint32_t> shadow;
  std::vector<std::string> unclaimed;
  std::vector<PendingRead> pending;
  std::thread checker;
  std::string report;
  std::thread reporter;
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
};

std::string Esc(const std::string& in) {
  std::string out;
  for (char c : in) {
    if (c == '\\' || c == '"') out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

State& S() {
  static State* state = [] {
    auto* s = new State();   // intentionally leaked: the reporter thread outlives static destruction
    std::string mode = Host().GetSettings().vfs;
    if (const char* e = getenv("GH2_VFS"); e && *e) mode = e;
    if (mode == "shadow") s->mode = Mode::kShadow;
    else if (mode == "native") s->mode = Mode::kNative;
    if (s->mode != Mode::kSdk) {
      const std::string root = Host().GetSettings().game_data_root;
      if (root.empty()) {
        fprintf(stderr, "VFS requested (%s) but no game_data_root: staying with the runtime's files\n", mode.c_str());
        s->mode = Mode::kSdk;
      } else {
        s->vfs = std::make_unique<gh2k::Vfs>();
        s->vfs->Mount("d:", root);
        s->vfs->Mount("game:", root);
        s->vfs->Mount("\\Device\\Cdrom0", root);
        fprintf(stderr, "VFS %s: d:, game: and \\Device\\Cdrom0 -> %s\n", mode.c_str(), root.c_str());
        if (const char* r = getenv("GH2_VFS_REPORT"); r && *r) s->report = r;
        s->checker = std::thread([s] {
          for (;;) {
            std::this_thread::sleep_for(std::chrono::microseconds(300));
            std::vector<PendingRead> work;
            {
              std::lock_guard<std::mutex> lock(s->mu);
              if (s->pending.empty()) continue;
              work.swap(s->pending);
            }
            std::vector<PendingRead> still;
            for (PendingRead& p : work) {
              const uint32_t status = __builtin_bswap32(*p.iosb);
              const auto age = std::chrono::steady_clock::now() - p.issued;
              if (status == 0x103) {
                if (age > std::chrono::seconds(5)) ++s->counters.pending_lost;
                else still.push_back(std::move(p));
                continue;
              }
              const uint32_t info = __builtin_bswap32(*(p.iosb + 1));
              const uint64_t ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(age).count());
              if (ms > s->counters.pending_max_ms) s->counters.pending_max_ms = ms;
              ++s->counters.pending_compared;
              ++s->counters.reads_compared;
              if ((status == 0) != (p.expected_status == 0) || (status == 0 && info != p.expected.size())) {
                if (++s->counters.read_status_mismatches < 10)
                  fprintf(stderr, "VFS pending read status: runtime %08X/%u, native %08X/%zu (offset %llx)\n", status, info, p.expected_status, p.expected.size(), (unsigned long long)p.offset);
              } else if (status == 0) {
                s->counters.bytes_compared += info;
                if (std::memcmp(p.buffer, p.expected.data(), info) != 0 && ++s->counters.read_mismatches < 10) {
                  size_t first = 0, n_diff = 0;
                  for (size_t i = 0; i < info; ++i)
                    if (p.buffer[i] != p.expected[i]) {
                      if (!n_diff) first = i;
                      ++n_diff;
                    }
                  fprintf(stderr, "VFS pending read BYTES DIFFER (offset %llx, %u bytes, %zu differ, first at +%zu, %.1f ms after issue)\n", (unsigned long long)p.offset, info, n_diff, first,
                          std::chrono::duration<double, std::milli>(age).count());
                  if (!s->report.empty())
                    if (FILE* lf = fopen((s->report + ".log").c_str(), "a")) {
                      fprintf(lf, "DIFFER offset %llx bytes %u differ %zu first +%zu age %.2f ms\n", (unsigned long long)p.offset, info, n_diff, first, std::chrono::duration<double, std::milli>(age).count());
                      fprintf(lf, "expected:");
                      for (int i = 0; i < 32; ++i) fprintf(lf, "%02x", p.expected[i]);
                      fprintf(lf, "\nobserved:");
                      for (int i = 0; i < 32; ++i) fprintf(lf, "%02x", p.buffer[i]);
                      fprintf(lf, "\n");
                      fclose(lf);
                    }
                }
              }
            }
            if (!still.empty()) {
              std::lock_guard<std::mutex> lock(s->mu);
              for (auto& p : still) s->pending.push_back(std::move(p));
            }
          }
        });
        s->reporter = std::thread([s] {
          int next = 10;
          for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - s->t0).count();
            if (t < next) continue;
            next += 10;
            const Counters& c = s->counters;
            const gh2k::VfsStats v = s->vfs->Stats();
            fprintf(stderr,
                    "VFS T+%.0fs opens %llu (native %llu, forwarded %llu) reads %llu (compared %llu, %.1f MB, mismatches %llu; native %llu, %.1f MB) queries %llu (mismatches %llu) | host reads %llu, %.1f MB\n",
                    t, (unsigned long long)c.creates.load(), (unsigned long long)c.creates_native.load(), (unsigned long long)c.creates_forwarded.load(), (unsigned long long)c.reads.load(),
                    (unsigned long long)c.reads_compared.load(), c.bytes_compared.load() / 1e6, (unsigned long long)c.read_mismatches.load(), (unsigned long long)c.reads_native.load(),
                    c.bytes_native.load() / 1e6, (unsigned long long)c.queries.load(), (unsigned long long)c.query_mismatches.load(), (unsigned long long)v.reads, v.bytes / 1e6);
            if (!s->report.empty()) {
              if (FILE* f = fopen(s->report.c_str(), "w")) {
                fprintf(f, "{\n \"seconds\": %.1f,\n \"mode\": \"%s\",\n", t, s->mode == Mode::kShadow ? "shadow" : "native");
                fprintf(f, " \"creates\": %llu, \"creates_claimed\": %llu, \"creates_native\": %llu, \"creates_forwarded\": %llu, \"closes\": %llu,\n", (unsigned long long)c.creates.load(),
                        (unsigned long long)c.creates_claimed.load(), (unsigned long long)c.creates_native.load(), (unsigned long long)c.creates_forwarded.load(), (unsigned long long)c.closes.load());
                fprintf(f, " \"reads\": %llu, \"reads_compared\": %llu, \"bytes_compared\": %llu, \"read_mismatches\": %llu, \"read_status_mismatches\": %llu, \"reads_unmapped\": %llu,\n",
                        (unsigned long long)c.reads.load(), (unsigned long long)c.reads_compared.load(), (unsigned long long)c.bytes_compared.load(), (unsigned long long)c.read_mismatches.load(),
                        (unsigned long long)c.read_status_mismatches.load(), (unsigned long long)c.reads_unmapped.load());
                fprintf(f, " \"reads_with_event\": %llu, \"reads_with_apc\": %llu, \"reads_at_own_position\": %llu, \"reads_native\": %llu, \"bytes_native\": %llu,\n",
                        (unsigned long long)c.reads_with_event.load(), (unsigned long long)c.reads_with_apc.load(), (unsigned long long)c.reads_own_position.load(),
                        (unsigned long long)c.reads_native.load(), (unsigned long long)c.bytes_native.load());
                fprintf(f, " \"reads_pending\": %llu, \"pending_compared\": %llu, \"pending_lost\": %llu, \"pending_max_ms\": %llu,\n", (unsigned long long)c.reads_pending.load(),
                        (unsigned long long)c.pending_compared.load(), (unsigned long long)c.pending_lost.load(), (unsigned long long)c.pending_max_ms.load());
                fprintf(f, " \"queries\": %llu, \"queries_native\": %llu, \"query_mismatches\": %llu, \"queries_unmapped\": %llu,\n \"query_classes\": {", (unsigned long long)c.queries.load(),
                        (unsigned long long)c.queries_native.load(), (unsigned long long)c.query_mismatches.load(), (unsigned long long)c.queries_unmapped.load());
                bool first = true;
                for (int k = 0; k < 64; ++k)
                  if (c.query_class[k].load()) {
                    fprintf(f, "%s\"%d\": %llu", first ? "" : ", ", k, (unsigned long long)c.query_class[k].load());
                    first = false;
                  }
                fprintf(f, "},\n \"unclaimed_paths_total\": %llu,\n \"unclaimed_paths\": [", (unsigned long long)c.unclaimed_paths.load());
                {
                  std::lock_guard<std::mutex> lock(s->mu);
                  for (size_t i = 0; i < s->unclaimed.size(); ++i) fprintf(f, "%s\"%s\"", i ? ", " : "", Esc(s->unclaimed[i]).c_str());
                }
                fprintf(f, "],\n \"files\": [\n");
                const auto files = s->vfs->Files();
                for (size_t i = 0; i < files.size(); ++i)
                  fprintf(f, "  {\"path\": \"%s\", \"size\": %llu, \"opens\": %llu, \"reads\": %llu, \"bytes\": %llu}%s\n", Esc(files[i].guest_path).c_str(), (unsigned long long)files[i].size,
                          (unsigned long long)files[i].opens, (unsigned long long)files[i].reads, (unsigned long long)files[i].bytes, i + 1 < files.size() ? "," : "");
                fprintf(f, " ]\n}\n");
                fclose(f);
              }
            }
          }
        });
      }
    }
    return s;
  }();
  return *state;
}

}  // namespace

Mode GetMode() { return S().mode; }
gh2k::Vfs* Get() { return S().vfs.get(); }
Counters& Stats() { return S().counters; }

void ShadowMap(uint32_t sdk_handle, uint32_t native_handle) {
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mu);
  s.shadow[sdk_handle] = native_handle;
}
uint32_t ShadowLookup(uint32_t sdk_handle) {
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mu);
  auto it = s.shadow.find(sdk_handle);
  return it == s.shadow.end() ? 0 : it->second;
}
uint32_t ShadowTake(uint32_t sdk_handle) {
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mu);
  auto it = s.shadow.find(sdk_handle);
  if (it == s.shadow.end()) return 0;
  const uint32_t v = it->second;
  s.shadow.erase(it);
  return v;
}

void AddPending(PendingRead read) {
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mu);
  s.pending.push_back(std::move(read));
}

void NoteUnclaimed(const char* path) {
  State& s = S();
  ++s.counters.unclaimed_paths;
  std::lock_guard<std::mutex> lock(s.mu);
  if (s.unclaimed.size() < 20) {
    for (const auto& u : s.unclaimed)
      if (u == path) return;
    s.unclaimed.emplace_back(path);
  }
}

}  // namespace gh2rt::hooks::vfsbridge
