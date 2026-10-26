// Stall tracing and sampling profiler (moved from app/src/gh2_host.cpp). Pure POSIX, no SDK dependency.
//
// GH2_STALL_TRACE=1: the per-frame calls of Heartbeat() are a heartbeat of the guest main thread. If it stops for >30 ms a watchdog
// samples the main thread's native stack (raw addresses; resolve with the scripts in work/analysis against the binary's symbols).
// GH2_PROFILE=1 samples continuously (every ~2 ms); GH2_PROFILE_THREAD=<name prefix> samples that thread instead of the main one;
// GH2_STALL_ALL=1 samples every thread during a stall; GH2_STALL_TRAP=MIN:MAX raises SIGTRAP once at a >40 ms hitch inside that
// window (seconds since start) so a debugger can stop the process there.
#include <dirent.h>
#include <execinfo.h>
#include <link.h>
#include <pthread.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "hooks_internal.h"

namespace gh2rt::hooks::detail {
namespace {

std::atomic<int64_t> g_beat_ms{0};
pthread_t g_main_thread;
std::atomic<bool> g_watchdog_started{false};

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void StallSampleHandler(int) {
  void* frames[96];
  int n = backtrace(frames, 96);
  char line[2048];
  char tname[32] = {};
  prctl(PR_GET_NAME, tname, 0, 0, 0);
  for (char* c = tname; *c; ++c)
    if (*c == ' ') *c = '_';
  int len = snprintf(line, sizeof(line), "STALLBT t=%lld th=%s", (long long)NowMs(), tname);
  for (int i = 0; i < n && len < 2000; i++) len += snprintf(line + len, sizeof(line) - len, " %p", frames[i]);
  line[len++] = '\n';
  (void)!write(2, line, len);
}

void StallWatchdog() {
  int samples = 0;
  // GH2_PROFILE=1: sample the main thread continuously (every ~2 ms) instead of only during stalls.
  static const bool profile = getenv("GH2_PROFILE") != nullptr;
  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(profile ? 2 : 8));
    int64_t gap = NowMs() - g_beat_ms.load();
    if (profile) {
      // GH2_PROFILE_THREAD=<name prefix>: sample that thread (e.g. "Audio") instead of the guest main thread.
      static const char* want = getenv("GH2_PROFILE_THREAD");
      if (want) {
        if (DIR* d = opendir("/proc/self/task")) {
          while (dirent* e = readdir(d)) {
            const pid_t tid = static_cast<pid_t>(atoi(e->d_name));
            if (tid <= 0) continue;
            char path[64], comm[32] = {};
            snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
            if (FILE* f = fopen(path, "r")) {
              if (fgets(comm, sizeof(comm), f)) {
              }
              fclose(f);
            }
            if (strncmp(comm, want, strlen(want)) == 0) syscall(SYS_tgkill, getpid(), tid, SIGUSR2);
          }
          closedir(d);
        }
      } else {
        pthread_kill(g_main_thread, SIGUSR2);
      }
      continue;
    }
    // GH2_STALL_TRAP=MIN:MAX (seconds since start): when a frame gap >40 ms happens in that window, raise SIGTRAP once so a
    // debugger can stop the whole process at the hitch (run under gdb with debuginfod for driver symbols).
    static const char* trap_env = getenv("GH2_STALL_TRAP");
    static bool trapped = false;
    if (trap_env && !trapped && gap > 40) {
      double lo = 0, hi = 1e9;
      sscanf(trap_env, "%lf:%lf", &lo, &hi);
      const double t = SinceStart();
      if (t >= lo && t <= hi) {
        trapped = true;
        raise(SIGTRAP);
      }
    }
    if (gap > 30) {
      if (samples < 40) {
        static const bool all_threads = getenv("GH2_STALL_ALL") != nullptr;
        if (all_threads) {
          // Sample every thread of the process (not the watchdog itself): who is the main thread waiting for?
          if (samples < 6) {
            const pid_t self = static_cast<pid_t>(syscall(SYS_gettid));
            if (DIR* d = opendir("/proc/self/task")) {
              while (dirent* e = readdir(d)) {
                const pid_t tid = static_cast<pid_t>(atoi(e->d_name));
                if (tid > 0 && tid != self) syscall(SYS_tgkill, getpid(), tid, SIGUSR2);
              }
              closedir(d);
            }
          }
        } else {
          pthread_kill(g_main_thread, SIGUSR2);
        }
        samples++;
      }
    } else {
      samples = 0;
    }
  }
}

}  // namespace

void Heartbeat() {
  static const bool on = getenv("GH2_STALL_TRACE") != nullptr || getenv("GH2_PROFILE") != nullptr;
  if (!on) return;
  g_beat_ms.store(NowMs());
  if (!g_watchdog_started.exchange(true)) {
    g_main_thread = pthread_self();
    signal(SIGUSR2, StallSampleHandler);
    // Map base of the executable, to resolve PIE addresses offline.
    fprintf(stderr, "STALLBASE %p\n", (void*)&Heartbeat);
    dl_iterate_phdr(  // load bases of the shared libraries, to resolve their frames offline
        [](dl_phdr_info* info, size_t, void*) {
          if (info->dlpi_name && strstr(info->dlpi_name, ".so"))
            fprintf(stderr, "LIBBASE %s 0x%lx\n", info->dlpi_name, static_cast<unsigned long>(info->dlpi_addr));
          return 0;
        },
        nullptr);
    std::thread(StallWatchdog).detach();
  }
}

}  // namespace gh2rt::hooks::detail
