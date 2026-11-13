// reGH2.exe - what the Start menu, the desktop icon and the user start: it opens the Tk launcher with the Python the package carries
// (<dir>\python\pythonw.exe <dir>\launcher\gh2_launcher.py <arguments>) and ends. A tiny GUI-subsystem program of its own, so that the shortcut,
// the task bar and the task manager show "reGH2" with its icon instead of a generic Python. Statically linked: nothing to install for it.
#include <windows.h>

#include <string>

namespace {

std::wstring Dir() {
  wchar_t path[MAX_PATH * 4];
  const DWORD n = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
  std::wstring p(path, n);
  const size_t slash = p.find_last_of(L"\\/");
  return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR args, int) {
  const std::wstring dir = Dir();
  const std::wstring python = dir + L"\\python\\pythonw.exe";
  const std::wstring script = dir + L"\\launcher\\gh2_launcher.py";
  if (!Exists(python) || !Exists(script)) {
    MessageBoxW(nullptr, (L"reGH2 is not complete: " + std::wstring(Exists(python) ? script : python) + L" is missing.\nReinstall it with Setup.exe.").c_str(), L"reGH2",
                MB_ICONERROR | MB_OK);
    return 1;
  }
  // -I: the interpreter ignores PYTHON* variables and the user's site-packages (the launcher puts its own folders on sys.path); -X utf8: UTF-8 whatever the console code page
  std::wstring cmd = L"\"" + python + L"\" -I -X utf8 \"" + script + L"\"";
  if (args && *args) cmd += std::wstring(L" ") + args;
  STARTUPINFOW si = {sizeof(si)};
  PROCESS_INFORMATION pi = {};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
    MessageBoxW(nullptr, L"The launcher could not be started.", L"reGH2", MB_ICONERROR | MB_OK);
    return 1;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return 0;
}
