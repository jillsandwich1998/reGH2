// gh2os - the few operating-system services the native runtime needs, behind one API (Linux/POSIX and Windows). No SDK, no guest macros.
//
// Everything that used to be a /proc read, an XDG variable, a futex, a pthread call or a process spawn in the runtime goes through here, so the rest of the code is the same on
// both systems. Each function says what it does on the two platforms; nothing here changes what the Linux build does.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace gh2os {

// Once, early in main(): Windows asks for a 1 ms timer resolution (sleeps are 15 ms coarse otherwise). Nothing on POSIX.
void InitProcess();

// ---- threads ----
void SetCurrentThreadName(const char* name);   // shown by debuggers and profilers (POSIX: at most 15 characters are kept)
uint32_t CurrentThreadId();                    // the OS thread id (gettid / GetCurrentThreadId)
uint64_t CurrentThreadCpuNs();                 // CPU time the calling thread has used, 0 if unknown
void LowerCurrentThreadPriority();             // background work: the game's own threads come first (nice +10 / THREAD_PRIORITY_BELOW_NORMAL)
// Real-time-ish priority for the audio mixer; returns a short description of what was achieved ("" if nothing could be changed).
std::string RaiseCurrentThreadForAudio();
void SleepNs(int64_t ns);
void YieldThread();

// ---- a futex-like word: sleep while *addr == expected, up to `timeout`; Wake releases up to n waiters ----
void WaitOnWord(std::atomic<uint32_t>* addr, uint32_t expected, std::chrono::nanoseconds timeout);
void WakeWord(std::atomic<uint32_t>* addr, int n);

// ---- places (no trailing separator). Empty when the home/profile directory is unknown ----
std::string HomeDir();     // $HOME / %USERPROFILE%
std::string DataDir();     // $XDG_DATA_HOME/gh2 or ~/.local/share/gh2  |  %LOCALAPPDATA%\gh2     : saves, DLC, shaders
std::string CacheDir();    // $XDG_CACHE_HOME/gh2 or ~/.cache/gh2        |  %LOCALAPPDATA%\gh2\cache : pipeline cache
std::string ConfigDir();   // $XDG_CONFIG_HOME/gh2 or ~/.config/gh2      |  %APPDATA%\gh2
std::string RuntimeDir();  // $XDG_RUNTIME_DIR or /tmp                   |  the temp directory
std::string ExePath();     // the running executable (/proc/self/exe | GetModuleFileName), "" if unknown
std::string ExeDir();      // its directory

// ---- the process ----
struct ProcessStats {
  double resident_mb = 0, virtual_mb = 0;
  int threads = 0, open_files = 0;
};
bool QueryProcessStats(ProcessStats* out);
uint32_t CurrentProcessId();
void SetEnv(const char* name, const char* value, bool overwrite);   // setenv / _putenv_s
std::string CommandLine();
// The "System" lines of a problem report: OS, kernel/build, desktop session, CPU, RAM (one "Key: value" per line, ending in a newline).
std::string DescribeSystem();   // the process's whole command line, arguments separated by spaces (for "was --vsync passed")
bool SameFile(const std::string& a, const std::string& b);

// Opens a file, folder or URL with the desktop's default application (xdg-open / ShellExecute). Fire and forget.
bool OpenWithDesktop(const std::string& target);

// ---- child processes (the shader build) ----
struct SpawnOptions {
  std::vector<std::string> argv;                       // argv[0] is the program (looked up in PATH when it has no separator)
  std::vector<std::pair<std::string, std::string>> env_set;   // added to / replaced in the environment
  std::vector<std::string> env_unset;                  // removed from the environment (POSIX: e.g. LD_LIBRARY_PATH)
  std::string working_dir;                             // "" = inherit
  bool new_process_group = true;                       // so that Terminate() can end the whole tree
};
class Process {
 public:
  Process();
  ~Process();
  Process(const Process&) = delete;
  Process& operator=(const Process&) = delete;
  // Starts the program with stdout+stderr joined into a pipe the caller reads with ReadOutput. Returns false and fills `error` on failure.
  bool Start(const SpawnOptions& options, std::string* error);
  // Reads available output, waiting up to timeout_ms. Returns the bytes read (0 = nothing yet), -1 = the pipe is closed (the process ended or closed its output).
  int ReadOutput(char* buffer, int size, int timeout_ms);
  // Non-blocking: has it ended? On true, *exit_code is the exit status, or -signal / the Windows exception code negated for an abnormal end, and *description says how.
  bool Poll(int* exit_code, std::string* description);
  void Terminate();   // ask the whole process tree to stop
  bool running() const;

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace gh2os
