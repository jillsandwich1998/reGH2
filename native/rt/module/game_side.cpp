// The game module's side of the handshake (gh2module/game_module.h): compiled into gh2game.dll together with the generated game code and
// the generated stubs (gh2game_stubs.gen.cpp, native/tools/gen_module_stubs.py). Shim headers only; built with llvm-mingw on Windows, with the bundled clang as libgh2game.so on Linux.
//
// The generated code calls the runtime through the C functions of gh2abi/abi.h (gh2_resolve_indirect, ...) and the kernel through
// `__imp__<Name>`: here the first are forwarded to the host's services, the second (in the stubs) jump through the host's import table.
#include <gh2abi/abi.h>
#include <gh2module/context_layout.h>
#include <gh2module/game_module.h>
#include <rex/image_info.h>

#include <cstring>

extern const rex::PPCImageInfo PPCImageConfig;   // generated (gh2_init.cpp)

// generated (gh2game_stubs.gen.cpp)
extern "C" {
extern Gh2PPCFunc* gh2m_imports[];
extern Gh2PPCFunc* gh2m_overrides[];
extern Gh2PPCFunc* const gh2m_originals[];
extern const uint32_t gh2m_import_count;
extern const uint32_t gh2m_override_count;
extern const uint64_t gh2m_imports_hash;
extern const uint64_t gh2m_overrides_hash;
}

namespace {
Gh2HostTable g_host{};   // a copy: one memory load per service call, no pointer chase
Gh2GameModule g_module{};
}  // namespace

// ---- the C boundary of gh2abi/abi.h, served by the host
extern "C" PPCFunc* gh2_resolve_indirect(uint32_t guest_address) { return g_host.resolve_indirect(guest_address); }
extern "C" uint64_t gh2_guest_tick_count() { return g_host.guest_tick_count(); }
extern "C" void gh2_global_lock() { g_host.global_lock(); }
extern "C" void gh2_global_unlock() { g_host.global_unlock(); }
extern "C" bool gh2_global_try_lock() { return g_host.global_try_lock(); }
extern "C" bool gh2_mmio_load(uint32_t guest_address, uint32_t* out_value) { return g_host.mmio_load(guest_address, out_value); }
extern "C" bool gh2_mmio_store(uint32_t guest_address, uint32_t value) { return g_host.mmio_store(guest_address, value); }
extern "C" void gh2_log(int level, const char* text, size_t length) { g_host.log(level, text, length); }
extern "C" void gh2_spin_hint() { g_host.spin_hint(); }

extern "C" GH2GAME_EXPORT const Gh2GameModule* gh2game_open(const Gh2HostTable* host) {
  g_module.abi_version = GH2GAME_ABI_VERSION;
  g_module.struct_size = sizeof(Gh2GameModule);
  g_module.context_layout = gh2module::ContextLayout();
  g_module.imports_hash = gh2m_imports_hash;
  g_module.overrides_hash = gh2m_overrides_hash;
  g_module.import_count = gh2m_import_count;
  g_module.override_count = gh2m_override_count;
  g_module.image_info = &PPCImageConfig;
  g_module.originals = gh2m_originals;
  g_module.error = nullptr;

  if (!host) {
    g_module.error = "no host table";
  } else if (host->abi_version != GH2GAME_ABI_VERSION || host->struct_size != sizeof(Gh2HostTable)) {
    g_module.error = "the host speaks another version of the module contract";
  } else if (host->context_layout != g_module.context_layout) {
    g_module.error = "PPCContext has another layout in the host";
  } else if (host->imports_hash != gh2m_imports_hash || host->import_count != gh2m_import_count) {
    g_module.error = "the host has another list of kernel imports";
  } else if (host->overrides_hash != gh2m_overrides_hash || host->override_count != gh2m_override_count) {
    g_module.error = "the host overrides another set of game functions";
  } else if (!host->resolve_indirect || !host->guest_tick_count || !host->global_lock || !host->global_unlock || !host->global_try_lock ||
             !host->mmio_load || !host->mmio_store || !host->log || !host->spin_hint || !host->imports || !host->overrides) {
    g_module.error = "the host table is incomplete";
  }
  if (g_module.error) return &g_module;

  g_host = *host;
  std::memcpy(gh2m_imports, host->imports, sizeof(Gh2PPCFunc*) * gh2m_import_count);
  std::memcpy(gh2m_overrides, host->overrides, sizeof(Gh2PPCFunc*) * gh2m_override_count);
  return &g_module;
}
