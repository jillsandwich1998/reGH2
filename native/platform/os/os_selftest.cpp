// gh2_os_selftest - gh2os on the machine it runs on: directories, process stats, the futex-like word, thread names, a child process whose output is read.
// Exit status 0 = everything passed.
#include <gh2os/os.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace {
int g_failures = 0;
void Check(bool ok, const std::string& what, const std::string& detail = "") {
  std::printf("[%s] %s%s%s\n", ok ? "ok" : "!!", what.c_str(), detail.empty() ? "" : " - ", detail.c_str());
  if (!ok) ++g_failures;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && !std::strcmp(argv[1], "--child")) {   // the program the spawn test starts
    std::printf("child says hello\n");
    std::fprintf(stderr, "child on stderr\n");
    return argc > 2 ? std::atoi(argv[2]) : 0;
  }
  gh2os::InitProcess();

  Check(!gh2os::ExePath().empty() && !gh2os::ExeDir().empty(), "executable path", gh2os::ExePath());
  Check(gh2os::SameFile(gh2os::ExePath(), gh2os::ExePath()), "SameFile on the executable");
  Check(!gh2os::HomeDir().empty(), "home directory", gh2os::HomeDir());
  Check(!gh2os::DataDir().empty() && !gh2os::CacheDir().empty() && !gh2os::ConfigDir().empty() && !gh2os::RuntimeDir().empty(), "data / cache / config / runtime directories",
        gh2os::DataDir() + " | " + gh2os::CacheDir() + " | " + gh2os::ConfigDir() + " | " + gh2os::RuntimeDir());
  Check(gh2os::CommandLine().find("selftest") != std::string::npos || gh2os::CommandLine().find("os_") != std::string::npos, "command line", gh2os::CommandLine());

  gh2os::ProcessStats st;
  const bool have_stats = gh2os::QueryProcessStats(&st);   // (before the Check call: its arguments are evaluated in no fixed order)
  Check(have_stats && st.resident_mb > 0.5 && st.threads >= 1, "process stats", std::to_string(st.resident_mb).substr(0, 6) + " MB resident, " + std::to_string(st.threads) + " threads");

  gh2os::SetCurrentThreadName("gh2-selftest");
  Check(gh2os::CurrentThreadId() != 0, "thread id");
  volatile double sink = 0;
  for (int i = 0; i < 20'000'000; ++i) sink += i * 0.5;
  Check(gh2os::CurrentThreadCpuNs() > 0, "thread CPU time", std::to_string(gh2os::CurrentThreadCpuNs() / 1000000) + " ms");

  // sleeps: 1 ms must really take about a millisecond, not 15
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 20; ++i) gh2os::SleepNs(1'000'000);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  Check(ms < 20 * 4.0, "20 x SleepNs(1 ms) takes well under 80 ms", std::to_string(ms).substr(0, 6) + " ms");

  // the word: a waiter sleeps until it is woken; a timeout returns too
  std::atomic<uint32_t> word{0};
  const auto w0 = std::chrono::steady_clock::now();
  gh2os::WaitOnWord(&word, 0, std::chrono::milliseconds(30));
  const double waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0).count();
  Check(waited >= 20 && waited < 200, "WaitOnWord times out", std::to_string(waited).substr(0, 6) + " ms");
  std::thread waker([&] {
    gh2os::SleepNs(20'000'000);
    word.store(1);
    gh2os::WakeWord(&word, 1);
  });
  const auto w1 = std::chrono::steady_clock::now();
  while (word.load() == 0) gh2os::WaitOnWord(&word, 0, std::chrono::seconds(5));
  const double woke = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w1).count();
  waker.join();
  Check(woke < 1000, "WaitOnWord is woken by WakeWord", std::to_string(woke).substr(0, 6) + " ms");

  // a child: output of both streams comes through, the exit code is reported, the environment is edited
  for (int code : {0, 7}) {
    gh2os::SpawnOptions o;
    o.argv = {gh2os::ExePath(), "--child", std::to_string(code)};
    o.env_set = {{"GH2_OS_TEST", "1"}};
    gh2os::Process p;
    std::string error;
    Check(p.Start(o, &error), "spawn a child", error);
    std::string out;
    char buf[256];
    for (int n; (n = p.ReadOutput(buf, sizeof buf, 5000)) >= 0;) out.append(buf, static_cast<size_t>(n));
    int exit_code = -1000;
    std::string why;
    for (int i = 0; i < 200 && !p.Poll(&exit_code, &why); ++i) gh2os::SleepNs(10'000'000);
    Check(out.find("child says hello") != std::string::npos && out.find("child on stderr") != std::string::npos, "child's stdout and stderr are read", out);
    Check(exit_code == code, "child's exit code", why);
  }
  {
    gh2os::SpawnOptions o;
    o.argv = {"this-program-does-not-exist-gh2"};
    gh2os::Process p;
    std::string error;
    Check(!p.Start(o, &error) && !error.empty(), "spawning a missing program fails with a message", error);
  }

  std::printf("%s\n", g_failures == 0 ? "PASS" : "FAIL");
  return g_failures == 0 ? 0 : 1;
}
