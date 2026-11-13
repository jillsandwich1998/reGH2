// gh2os on Windows. See os.h for what each function promises.
#include "gh2os/os.h"

#include <windows.h>
#include <knownfolders.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <thread>
#include <string>

namespace gh2os {

namespace {

std::wstring Wide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::string Utf8(const wchar_t* w, size_t len) {
  if (!len) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(len), s.data(), n, nullptr, nullptr);
  return s;
}

std::string KnownFolder(const KNOWNFOLDERID& id) {
  PWSTR path = nullptr;
  std::string out;
  if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &path)) && path) out = Utf8(path, wcslen(path));
  CoTaskMemFree(path);
  return out;
}

}  // namespace

void InitProcess() {
  // 1 ms timer resolution for the whole process: sleep_for(1ms) and the audio/vsync pacing threads need it (the default is 15.6 ms).
  using Fn = LONG(NTAPI*)(ULONG, BOOLEAN, PULONG);
  if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
    if (auto set = reinterpret_cast<Fn>(GetProcAddress(nt, "NtSetTimerResolution"))) {
      ULONG actual = 0;
      set(10000, TRUE, &actual);   // 100 ns units: 1 ms
    }
}

// ---- threads ----
void SetCurrentThreadName(const char* name) { SetThreadDescription(GetCurrentThread(), Wide(name).c_str()); }

uint32_t CurrentThreadId() { return GetCurrentThreadId(); }

uint64_t CurrentThreadCpuNs() {
  FILETIME created, exited, kernel, user;
  if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0;
  auto to_u64 = [](const FILETIME& f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
  return (to_u64(kernel) + to_u64(user)) * 100ull;   // 100 ns units
}

void LowerCurrentThreadPriority() { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); }

std::string RaiseCurrentThreadForAudio() {
  if (SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST)) return "THREAD_PRIORITY_HIGHEST";
  return "";
}

void SleepNs(int64_t ns) {
  // A high-resolution waitable timer sleeps to well under a millisecond; Sleep() cannot. One timer per thread.
  thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!timer) {
    Sleep(static_cast<DWORD>((ns + 999'999) / 1'000'000));
    return;
  }
  LARGE_INTEGER due;
  due.QuadPart = -(ns / 100 > 0 ? ns / 100 : 1);   // relative, 100 ns units
  if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, INFINITE);
}

void YieldThread() { SwitchToThread(); }

void WaitOnWord(std::atomic<uint32_t>* addr, uint32_t expected, std::chrono::nanoseconds timeout) {
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout + std::chrono::nanoseconds(999'999)).count();
  WaitOnAddress(reinterpret_cast<volatile VOID*>(addr), &expected, sizeof expected, static_cast<DWORD>(ms < 0 ? 0 : ms));
}

void WakeWord(std::atomic<uint32_t>* addr, int n) {
  if (n == 1) WakeByAddressSingle(reinterpret_cast<PVOID>(addr));
  else WakeByAddressAll(reinterpret_cast<PVOID>(addr));
}

// ---- places ----
std::string HomeDir() { return KnownFolder(FOLDERID_Profile); }

std::string DataDir() {
  const std::string base = KnownFolder(FOLDERID_LocalAppData);
  return base.empty() ? std::string() : base + "\\gh2";
}

std::string CacheDir() {
  const std::string d = DataDir();
  return d.empty() ? std::string() : d + "\\cache";
}

std::string ConfigDir() {
  const std::string base = KnownFolder(FOLDERID_RoamingAppData);
  return base.empty() ? std::string() : base + "\\gh2";
}

std::string RuntimeDir() {
  wchar_t buf[MAX_PATH + 2];
  const DWORD n = GetTempPathW(MAX_PATH + 1, buf);
  std::string s = Utf8(buf, n);
  while (!s.empty() && (s.back() == '\\' || s.back() == '/')) s.pop_back();
  return s;
}

std::string ExePath() {
  wchar_t buf[32768];
  const DWORD n = GetModuleFileNameW(nullptr, buf, 32768);
  return n ? Utf8(buf, n) : std::string();
}

std::string ExeDir() {
  const std::string p = ExePath();
  const size_t slash = p.find_last_of("\\/");
  return slash == std::string::npos ? std::string() : p.substr(0, slash);
}

// ---- the process ----
bool QueryProcessStats(ProcessStats* out) {
  PROCESS_MEMORY_COUNTERS_EX pmc = {};
  pmc.cb = sizeof pmc;
  if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc)) return false;
  out->resident_mb = static_cast<double>(pmc.WorkingSetSize) / 1048576.0;
  out->virtual_mb = static_cast<double>(pmc.PrivateUsage) / 1048576.0;   // committed private memory (the 4.5 GB guest section is shared, not private)
  int threads = 0;
  if (HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); snap != INVALID_HANDLE_VALUE) {
    THREADENTRY32 te = {};
    te.dwSize = sizeof te;
    const DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
      if (te.th32OwnerProcessID == pid) ++threads;
    CloseHandle(snap);
  }
  out->threads = threads;
  DWORD handles = 0;
  GetProcessHandleCount(GetCurrentProcess(), &handles);
  out->open_files = static_cast<int>(handles);
  return true;
}

void SetEnv(const char* name, const char* value, bool overwrite) {
  if (!overwrite && std::getenv(name)) return;
  _putenv_s(name, value);
}

uint32_t CurrentProcessId() { return GetCurrentProcessId(); }

namespace {
// A REG_SZ value of HKLM, "" if absent.
std::string RegistryString(const wchar_t* subkey, const wchar_t* name) {
  wchar_t buf[256];
  DWORD size = sizeof buf;
  if (RegGetValueW(HKEY_LOCAL_MACHINE, subkey, name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
  return Utf8(buf, wcslen(buf));
}
}  // namespace

std::string DescribeSystem() {
  const wchar_t* nt = L"SOFTWARE\Microsoft\Windows NT\CurrentVersion";
  const std::string product = RegistryString(nt, L"ProductName"), display = RegistryString(nt, L"DisplayVersion"), build = RegistryString(nt, L"CurrentBuildNumber");
  const std::string cpu = RegistryString(L"HARDWARE\DESCRIPTION\System\CentralProcessor\0", L"ProcessorNameString");
  MEMORYSTATUSEX ms = {};
  ms.dwLength = sizeof ms;
  GlobalMemoryStatusEx(&ms);
  auto kb = [](unsigned long long bytes) { return std::to_string(bytes / 1024) + " kB"; };
  std::ostringstream r;
  r << "OS: " << (product.empty() ? "Windows" : product) << (display.empty() ? "" : " " + display) << "\n";
  r << "Kernel: Windows build " << (build.empty() ? "?" : build) << " x86_64\n";
  r << "Session: Windows desktop\n";
  r << "CPU: " << (cpu.empty() ? "?" : cpu) << " (" << std::thread::hardware_concurrency() << " threads)\n";
  r << "RAM: total " << kb(ms.ullTotalPhys) << ", available " << kb(ms.ullAvailPhys) << "\n";
  return r.str();
}

std::string CommandLine() {
  const wchar_t* c = GetCommandLineW();
  return Utf8(c, wcslen(c));
}

bool SameFile(const std::string& a, const std::string& b) {
  auto info = [](const std::string& path, BY_HANDLE_FILE_INFORMATION* out) {
    HANDLE h = CreateFileW(Wide(path).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const bool ok = GetFileInformationByHandle(h, out) != 0;
    CloseHandle(h);
    return ok;
  };
  BY_HANDLE_FILE_INFORMATION ia, ib;
  return info(a, &ia) && info(b, &ib) && ia.dwVolumeSerialNumber == ib.dwVolumeSerialNumber && ia.nFileIndexHigh == ib.nFileIndexHigh && ia.nFileIndexLow == ib.nFileIndexLow;
}

bool OpenWithDesktop(const std::string& target) {
  return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", Wide(target).c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

// ---- child processes ----
namespace {

// CommandLineToArgvW's inverse: the quoting rules of the Microsoft C runtime.
void AppendQuoted(std::wstring& cmd, const std::wstring& arg) {
  if (!cmd.empty()) cmd += L' ';
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    cmd += arg;
    return;
  }
  cmd += L'"';
  for (size_t i = 0;; ++i) {
    size_t backslashes = 0;
    while (i < arg.size() && arg[i] == L'\\') ++i, ++backslashes;
    if (i == arg.size()) {
      cmd.append(backslashes * 2, L'\\');
      break;
    }
    if (arg[i] == L'"') cmd.append(backslashes * 2 + 1, L'\\');
    else cmd.append(backslashes, L'\\');
    cmd += arg[i];
  }
  cmd += L'"';
}

std::wstring Upper(std::wstring s) {
  for (wchar_t& c : s) c = static_cast<wchar_t>(towupper(c));
  return s;
}

}  // namespace

struct Process::Impl {
  HANDLE process = nullptr, read = nullptr, job = nullptr;
  bool done = false;
  DWORD code = 0;
};

Process::Process() : impl_(new Impl) {}

Process::~Process() {
  if (impl_->read) CloseHandle(impl_->read);
  if (impl_->process) CloseHandle(impl_->process);
  if (impl_->job) CloseHandle(impl_->job);   // KILL_ON_JOB_CLOSE: whatever the child still runs ends with us
  delete impl_;
}

bool Process::Start(const SpawnOptions& options, std::string* error) {
  auto fail = [&](const char* what) {
    if (error) *error = std::string(what) + " (error " + std::to_string(GetLastError()) + ")";
    return false;
  };
  SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
  HANDLE read = nullptr, write = nullptr;
  if (!CreatePipe(&read, &write, &sa, 0)) return fail("cannot make a pipe");
  SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
  HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

  // the environment: ours, minus env_unset, plus env_set (names compare case-insensitively on Windows)
  std::map<std::wstring, std::pair<std::wstring, std::wstring>> env;   // upper-case name -> (name, value)
  if (wchar_t* block = GetEnvironmentStringsW()) {
    for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
      const wchar_t* eq = wcschr(p + 1, L'=');   // (p + 1: names like "=C:" start with '=')
      if (!eq) continue;
      std::wstring name(p, eq), value(eq + 1);
      env[Upper(name)] = {name, value};
    }
    FreeEnvironmentStringsW(block);
  }
  for (const std::string& u : options.env_unset) env.erase(Upper(Wide(u)));
  for (const auto& kv : options.env_set) env[Upper(Wide(kv.first))] = {Wide(kv.first), Wide(kv.second)};
  std::wstring env_block;
  for (const auto& e : env) env_block += e.second.first + L"=" + e.second.second + L'\0';
  env_block += L'\0';

  std::wstring cmd;
  for (const std::string& a : options.argv) AppendQuoted(cmd, Wide(a));

  // Inherit exactly the two handles the child needs.
  SIZE_T attr_size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
  std::string attr_storage(attr_size, '\0');
  auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_storage.data());
  InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size);
  HANDLE inherit[2] = {write, nul};
  const DWORD inherit_count = nul != INVALID_HANDLE_VALUE ? 2 : 1;
  UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, inherit_count * sizeof(HANDLE), nullptr, nullptr);

  STARTUPINFOEXW si = {};
  si.StartupInfo.cb = sizeof si;
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdOutput = write;
  si.StartupInfo.hStdError = write;
  si.StartupInfo.hStdInput = nul != INVALID_HANDLE_VALUE ? nul : nullptr;
  si.lpAttributeList = attrs;
  PROCESS_INFORMATION pi = {};
  const std::wstring dir = Wide(options.working_dir);
  const DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT | (options.new_process_group ? CREATE_NEW_PROCESS_GROUP : 0);
  const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, flags, env_block.data(), dir.empty() ? nullptr : dir.c_str(), &si.StartupInfo, &pi);
  const DWORD created_error = GetLastError();
  DeleteProcThreadAttributeList(attrs);
  CloseHandle(write);
  if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
  if (!ok) {
    CloseHandle(read);
    SetLastError(created_error);
    return fail("cannot start the program");
  }
  // A job object that kills the whole tree when it is closed (or on Terminate): the child must not outlive the game.
  impl_->job = CreateJobObjectW(nullptr, nullptr);
  if (impl_->job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(impl_->job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
    AssignProcessToJobObject(impl_->job, pi.hProcess);
  }
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);
  impl_->process = pi.hProcess;
  impl_->read = read;
  return true;
}

int Process::ReadOutput(char* buffer, int size, int timeout_ms) {
  if (!impl_->read) return -1;
  // Anonymous pipes cannot wait with a timeout: look, and sleep a little while there is nothing.
  const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeout_ms < 0 ? 0 : timeout_ms);
  for (;;) {
    DWORD available = 0;
    if (!PeekNamedPipe(impl_->read, nullptr, 0, nullptr, &available, nullptr)) return -1;   // broken: the child (and its children) closed the pipe
    if (available > 0) {
      DWORD got = 0;
      if (!ReadFile(impl_->read, buffer, static_cast<DWORD>(size), &got, nullptr) || got == 0) return -1;
      return static_cast<int>(got);
    }
    if (GetTickCount64() >= deadline) return 0;
    Sleep(5);
  }
}

bool Process::Poll(int* exit_code, std::string* description) {
  if (!impl_->process) return true;
  if (!impl_->done) {
    if (WaitForSingleObject(impl_->process, 0) != WAIT_OBJECT_0) return false;
    GetExitCodeProcess(impl_->process, &impl_->code);
    impl_->done = true;
  }
  *exit_code = static_cast<int>(impl_->code);
  if (description) {
    char buf[64];
    if (impl_->code >= 0xC0000000u) std::snprintf(buf, sizeof buf, "crashed with exception 0x%08lX", impl_->code);
    else std::snprintf(buf, sizeof buf, "exit %d", static_cast<int>(impl_->code));
    *description = buf;
  }
  return true;
}

void Process::Terminate() {
  if (impl_->job && !impl_->done) TerminateJobObject(impl_->job, 1);
}

bool Process::running() const { return impl_->process && !impl_->done; }

}  // namespace gh2os
