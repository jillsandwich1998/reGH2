// gh2os on Linux/POSIX: the code that used to be spread over the runtime (futex, pthread names, /proc readers, XDG directories, posix_spawn), moved here unchanged in behaviour.
#include "gh2os/os.h"

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/futex.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

extern char** environ;

namespace gh2os {

void InitProcess() {}

// ---- threads ----
void SetCurrentThreadName(const char* name) {
  char buf[16];
  std::strncpy(buf, name, sizeof buf - 1);
  buf[sizeof buf - 1] = 0;
  pthread_setname_np(pthread_self(), buf);
}

uint32_t CurrentThreadId() { return static_cast<uint32_t>(syscall(SYS_gettid)); }

uint64_t CurrentThreadCpuNs() {
  timespec ts;
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
  return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(ts.tv_nsec);
}

void LowerCurrentThreadPriority() { setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10); }

// The mixer thread gets real-time round-robin scheduling at a low priority when the user may (rtprio limit, as audio users and rtkit-less setups have), else a better nice value.
// RLIMIT_RTTIME makes a runaway real-time thread die with SIGXCPU instead of freezing the machine; the mixer sleeps on the credit semaphore every frame, far below the limit.
std::string RaiseCurrentThreadForAudio() {
  struct rlimit rt = {};
  if (getrlimit(RLIMIT_RTTIME, &rt) == 0 && (rt.rlim_cur == RLIM_INFINITY || rt.rlim_cur > 1000000)) {
    rt.rlim_cur = 1000000;   // 1 s of CPU without sleeping
    if (rt.rlim_max != RLIM_INFINITY && rt.rlim_max < rt.rlim_cur) rt.rlim_cur = rt.rlim_max;
    setrlimit(RLIMIT_RTTIME, &rt);
  }
  sched_param param = {};
  param.sched_priority = 4;
  if (pthread_setschedparam(pthread_self(), SCHED_RR, &param) == 0) return "real-time round-robin priority 4";
  if (setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), -8) == 0) return "nice -8 (no real-time permission)";
  return "";
}

void SleepNs(int64_t ns) {
  timespec ts{static_cast<time_t>(ns / 1'000'000'000), static_cast<long>(ns % 1'000'000'000)};
  nanosleep(&ts, nullptr);
}

void YieldThread() { sched_yield(); }

void WaitOnWord(std::atomic<uint32_t>* addr, uint32_t expected, std::chrono::nanoseconds timeout) {
  timespec ts;
  ts.tv_sec = static_cast<time_t>(timeout.count() / 1'000'000'000);
  ts.tv_nsec = static_cast<long>(timeout.count() % 1'000'000'000);
  syscall(SYS_futex, reinterpret_cast<uint32_t*>(addr), FUTEX_WAIT_PRIVATE, expected, &ts, nullptr, 0);
}

void WakeWord(std::atomic<uint32_t>* addr, int n) { syscall(SYS_futex, reinterpret_cast<uint32_t*>(addr), FUTEX_WAKE_PRIVATE, n, nullptr, nullptr, 0); }

// ---- places ----
static std::string Env(const char* name) {
  const char* v = std::getenv(name);
  return v && *v ? v : "";
}

std::string HomeDir() { return Env("HOME"); }

std::string DataDir() {
  if (std::string x = Env("XDG_DATA_HOME"); !x.empty()) return x + "/gh2";
  return HomeDir() + "/.local/share/gh2";
}

std::string CacheDir() {
  if (std::string x = Env("XDG_CACHE_HOME"); !x.empty()) return x + "/gh2";
  if (std::string h = HomeDir(); !h.empty()) return h + "/.cache/gh2";
  return {};
}

std::string ConfigDir() {
  if (std::string x = Env("XDG_CONFIG_HOME"); !x.empty()) return x + "/gh2";
  if (std::string h = HomeDir(); !h.empty()) return h + "/.config/gh2";
  return {};
}

std::string RuntimeDir() {
  if (std::string x = Env("XDG_RUNTIME_DIR"); !x.empty()) return x;
  return "/tmp";
}

std::string ExePath() {
  char buf[4096];
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n <= 0) return {};
  buf[n] = 0;
  return buf;
}

std::string ExeDir() {
  std::string p = ExePath();
  const size_t slash = p.rfind('/');
  return slash == std::string::npos ? std::string() : p.substr(0, slash);
}

// ---- the process ----
bool QueryProcessStats(ProcessStats* out) {
  unsigned long pages_total = 0, pages_resident = 0;
  if (FILE* f = fopen("/proc/self/statm", "r")) {
    if (fscanf(f, "%lu %lu", &pages_total, &pages_resident) != 2) pages_total = pages_resident = 0;
    fclose(f);
  }
  int threads = 0;
  if (FILE* f = fopen("/proc/self/status", "r")) {
    char line[256];
    while (fgets(line, sizeof line, f))
      if (sscanf(line, "Threads: %d", &threads) == 1) break;
    fclose(f);
  }
  int fds = 0;
  if (DIR* d = opendir("/proc/self/fd")) {
    while (readdir(d)) ++fds;
    closedir(d);
  }
  const double page_mb = static_cast<double>(sysconf(_SC_PAGESIZE)) / 1048576.0;
  out->resident_mb = static_cast<double>(pages_resident) * page_mb;
  out->virtual_mb = static_cast<double>(pages_total) * page_mb;
  out->threads = threads;
  out->open_files = fds > 2 ? fds - 2 : 0;   // (. and ..)
  return true;
}

void SetEnv(const char* name, const char* value, bool overwrite) { setenv(name, value, overwrite ? 1 : 0); }

uint32_t CurrentProcessId() { return static_cast<uint32_t>(getpid()); }

namespace {
std::string ReadWholeFile(const char* path) {
  std::ifstream f(path, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
// First line of a "key: value" file such as /proc/cpuinfo that starts with `key`.
std::string FirstValue(const std::string& text, const std::string& key) {
  std::istringstream in(text);
  for (std::string l; std::getline(in, l);)
    if (l.rfind(key, 0) == 0) {
      const size_t c = l.find(':');
      return c == std::string::npos ? l : l.substr(c + 2);
    }
  return "?";
}
std::string EnvOrDash(const char* name) {
  const char* v = std::getenv(name);
  return v && *v ? v : "-";
}
std::string OsRelease() {
  std::istringstream in(ReadWholeFile("/etc/os-release"));
  for (std::string l; std::getline(in, l);)
    if (l.rfind("PRETTY_NAME=", 0) == 0) {
      std::string v = l.substr(12);
      v.erase(std::remove(v.begin(), v.end(), '"'), v.end());
      return v;
    }
  return "?";
}
}  // namespace

std::string DescribeSystem() {
  utsname u{};
  uname(&u);
  const std::string cpu = ReadWholeFile("/proc/cpuinfo"), mem = ReadWholeFile("/proc/meminfo");
  std::ostringstream r;
  r << "OS: " << OsRelease() << "\nKernel: " << u.release << " " << u.machine << "\n";
  r << "Session: " << EnvOrDash("XDG_SESSION_TYPE") << ", desktop " << EnvOrDash("XDG_CURRENT_DESKTOP") << ", Wayland display " << EnvOrDash("WAYLAND_DISPLAY") << "\n";
  r << "CPU: " << FirstValue(cpu, "model name") << " (" << std::thread::hardware_concurrency() << " threads)\n";
  r << "RAM: total " << FirstValue(mem, "MemTotal") << ", available " << FirstValue(mem, "MemAvailable") << "\n";
  return r.str();
}

std::string CommandLine() {
  std::string s;
  if (FILE* f = fopen("/proc/self/cmdline", "r")) {
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
  }
  for (char& c : s)
    if (c == 0) c = ' ';
  return s;
}

bool SameFile(const std::string& a, const std::string& b) {
  struct stat sa, sb;
  return stat(a.c_str(), &sa) == 0 && stat(b.c_str(), &sb) == 0 && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

// The environment a child gets: ours, minus env_unset, plus env_set.
static std::vector<std::string> BuildEnvironment(const SpawnOptions& o) {
  std::vector<std::string> out;
  for (char** e = environ; e && *e; ++e) {
    std::string entry = *e;
    const std::string name = entry.substr(0, entry.find('='));
    bool drop = false;
    for (const auto& u : o.env_unset) drop = drop || u == name;
    for (const auto& kv : o.env_set) drop = drop || kv.first == name;
    if (!drop) out.push_back(std::move(entry));
  }
  for (const auto& kv : o.env_set) out.push_back(kv.first + "=" + kv.second);
  return out;
}

bool OpenWithDesktop(const std::string& target) {
  SpawnOptions o;
  o.argv = {"xdg-open", target};
  o.env_unset = {"LD_LIBRARY_PATH", "LD_PRELOAD"};   // the game's own libraries must not leak into the desktop's helper
  auto* proc = new Process();
  std::string error;
  if (!proc->Start(o, &error)) {
    delete proc;
    return false;
  }
  std::thread([proc] {   // reaped in the background; the helper's output is not needed
    char buf[256];
    while (proc->ReadOutput(buf, sizeof buf, 1000) >= 0) {}
    int code;
    std::string why;
    while (!proc->Poll(&code, &why)) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    delete proc;
  }).detach();
  return true;
}

// ---- child processes ----
struct Process::Impl {
  pid_t pid = -1;
  int fd = -1;   // read end of the output pipe
  bool reaped = false;
  int status = 0;
};

Process::Process() : impl_(new Impl) {}
Process::~Process() {
  if (impl_->fd >= 0) close(impl_->fd);
  delete impl_;
}

bool Process::Start(const SpawnOptions& options, std::string* error) {
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    if (error) *error = "cannot make a pipe";
    return false;
  }
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, pipefd[0]);
  // A process group of its own, so that the child can end everything it started when this process goes away; a Ctrl+C in the terminal then no longer reaches it directly.
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  if (options.new_process_group) {
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);
  }
  std::vector<std::string> env = BuildEnvironment(options);
  std::vector<char*> envp;
  for (std::string& e : env) envp.push_back(e.data());
  envp.push_back(nullptr);
  std::vector<std::string> args = options.argv;
  std::vector<char*> argv;
  for (std::string& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);
  const int rc = posix_spawnp(&impl_->pid, argv[0], &actions, &attr, argv.data(), envp.data());
  posix_spawnattr_destroy(&attr);
  posix_spawn_file_actions_destroy(&actions);
  close(pipefd[1]);
  if (rc != 0) {
    close(pipefd[0]);
    if (error) *error = std::strerror(rc);
    impl_->pid = -1;
    return false;
  }
  impl_->fd = pipefd[0];
  return true;
}

int Process::ReadOutput(char* buffer, int size, int timeout_ms) {
  if (impl_->fd < 0) return -1;
  pollfd p = {impl_->fd, POLLIN, 0};
  const int r = poll(&p, 1, timeout_ms);
  if (r == 0) return 0;
  if (r < 0) return errno == EINTR ? 0 : -1;
  const ssize_t n = read(impl_->fd, buffer, static_cast<size_t>(size));
  if (n > 0) return static_cast<int>(n);
  if (n < 0 && (errno == EINTR || errno == EAGAIN)) return 0;
  return -1;
}

bool Process::Poll(int* exit_code, std::string* description) {
  if (impl_->pid < 0) return true;
  if (!impl_->reaped) {
    const pid_t r = waitpid(impl_->pid, &impl_->status, WNOHANG);
    if (r == 0) return false;
    impl_->reaped = true;
  }
  if (WIFEXITED(impl_->status)) {
    *exit_code = WEXITSTATUS(impl_->status);
    if (description) *description = "exit " + std::to_string(*exit_code);
  } else {
    *exit_code = -WTERMSIG(impl_->status);
    if (description) *description = std::string("killed by signal ") + std::to_string(WTERMSIG(impl_->status)) + " " + strsignal(WTERMSIG(impl_->status));
  }
  return true;
}

void Process::Terminate() {
  if (impl_->pid > 0 && !impl_->reaped) kill(-impl_->pid, SIGTERM);
}

bool Process::running() const { return impl_->pid > 0 && !impl_->reaped; }

}  // namespace gh2os
