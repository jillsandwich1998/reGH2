// The host's side of the game module handshake (gh2module/game_module.h): loads gh2game.dll from the executable's directory (or the path in
// GH2_GAME_MODULE), hands it the runtime services, the kernel imports and the hooks, and takes the game's original code of every overridden
// function and the image info that ReXApp is constructed with. Shim headers only (as the hooks).
#include <gh2abi/abi.h>
#include <gh2module/context_layout.h>
#include <gh2module/game_module.h>
#include <gh2module/host.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// generated (gh2host_stubs.gen.cpp, native/tools/gen_module_stubs.py)
extern "C" {
extern Gh2PPCFunc* const gh2h_imports[];
extern Gh2PPCFunc* const gh2h_overrides[];
extern Gh2PPCFunc* gh2h_originals[];
extern const uint32_t gh2h_import_count;
extern const uint32_t gh2h_override_count;
extern const uint64_t gh2h_imports_hash;
extern const uint64_t gh2h_overrides_hash;
}

namespace gh2module {

namespace {

[[noreturn]] void Fail(const std::string& what) {
  const std::string text = "The game module could not be loaded:\n\n" + what +
                           "\n\nRebuild the game from the launcher (Update game), then start it again.";
  std::fprintf(stderr, "MODULE: %s\n", what.c_str());
#ifdef _WIN32
  const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
  std::wstring wide(n > 0 ? size_t(n) : 0, L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), n);
  MessageBoxW(nullptr, wide.c_str(), L"Guitar Hero II", MB_OK | MB_ICONERROR);
#endif
  std::exit(1);
}

#ifdef _WIN32
std::string Utf8(const std::wstring& w) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? size_t(n) : 0, '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
  if (!s.empty()) s.pop_back();
  return s;
}

Gh2GameOpenFunc LoadModule(std::string* where) {
  std::wstring path;
  if (const wchar_t* env = _wgetenv(L"GH2_GAME_MODULE"); env && *env) {
    path = env;
  } else {
    std::wstring exe(32768, L'\0');
    const DWORD len = GetModuleFileNameW(nullptr, exe.data(), DWORD(exe.size()));
    exe.resize(len);
    const size_t slash = exe.find_last_of(L"\\/");
    path = (slash == std::wstring::npos ? std::wstring() : exe.substr(0, slash + 1)) + L"" GH2GAME_MODULE_FILE;
  }
  *where = Utf8(path);
  HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!module) Fail(*where + ": LoadLibrary failed (error " + std::to_string(GetLastError()) + ")");
  auto open = reinterpret_cast<Gh2GameOpenFunc>(reinterpret_cast<void*>(GetProcAddress(module, GH2GAME_OPEN_SYMBOL)));
  if (!open) Fail(*where + ": it has no " GH2GAME_OPEN_SYMBOL " (not a game module)");
  return open;
}
#else
Gh2GameOpenFunc LoadModule(std::string* where) {
  const char* env = std::getenv("GH2_GAME_MODULE");
  *where = env && *env ? env : "./libgh2game.so";
  void* module = dlopen(where->c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!module) Fail(*where + ": " + dlerror());
  auto open = reinterpret_cast<Gh2GameOpenFunc>(dlsym(module, GH2GAME_OPEN_SYMBOL));
  if (!open) Fail(*where + ": it has no " GH2GAME_OPEN_SYMBOL " (not a game module)");
  return open;
}
#endif

}  // namespace

const void* OpenGameModule() {
  static const void* image = [] {
    std::string where;
    const Gh2GameOpenFunc open = LoadModule(&where);

    static Gh2HostTable table{};
    table.abi_version = GH2GAME_ABI_VERSION;
    table.struct_size = sizeof(Gh2HostTable);
    table.context_layout = ContextLayout();
    table.imports_hash = gh2h_imports_hash;
    table.overrides_hash = gh2h_overrides_hash;
    table.import_count = gh2h_import_count;
    table.override_count = gh2h_override_count;
    table.resolve_indirect = gh2_resolve_indirect;
    table.guest_tick_count = gh2_guest_tick_count;
    table.global_lock = gh2_global_lock;
    table.global_unlock = gh2_global_unlock;
    table.global_try_lock = gh2_global_try_lock;
    table.mmio_load = gh2_mmio_load;
    table.mmio_store = gh2_mmio_store;
    table.log = gh2_log;
    table.spin_hint = gh2_spin_hint;
    table.imports = gh2h_imports;
    table.overrides = gh2h_overrides;

    const Gh2GameModule* module = open(&table);
    if (!module) Fail(where + ": " GH2GAME_OPEN_SYMBOL " returned nothing");
    if (module->abi_version != GH2GAME_ABI_VERSION || module->struct_size != sizeof(Gh2GameModule))
      Fail(where + ": it was built for another version of the module contract");
    if (module->error) Fail(where + ": " + module->error);
    if (module->context_layout != table.context_layout || module->imports_hash != table.imports_hash || module->overrides_hash != table.overrides_hash ||
        module->import_count != table.import_count || module->override_count != table.override_count || !module->image_info || !module->originals)
      Fail(where + ": it does not match this executable");

    for (uint32_t i = 0; i < gh2h_override_count; ++i) {
      if (!module->originals[i]) Fail(where + ": original code #" + std::to_string(i) + " is missing");
      gh2h_originals[i] = module->originals[i];
    }
    std::fprintf(stderr, "MODULE: %s: contract v%u, %u kernel imports, %u overridden game functions\n", where.c_str(), module->abi_version,
                 module->import_count, module->override_count);
    return module->image_info;
  }();
  return image;
}

}  // namespace gh2module
