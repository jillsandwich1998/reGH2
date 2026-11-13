// gh2module/game_module.h - the contract between the host (gh2.exe: runtime, hooks, renderer) and the game module (gh2game.dll: the
// recompiled game code alone).
//
// On Windows the game module is compiled on the player's PC with llvm-mingw (clang, lld, libc++, mingw-w64: all redistributable), the host
// on ours with the MSVC ABI. The two sides have different C++ ABIs and standard libraries, so everything that crosses is plain C: function
// pointers of type PPCFunc (`void(PPCContext&, uint8_t*)`, the same calling convention on both), integers and POD tables. No C++ name,
// vtable, exception or STL object crosses, with one exception documented in hooks_kthreads.cpp (ExTerminateThread unwinds the module's
// frames, which have no cleanups and only the unwind data every x64 function has).
//
// Handshake: the host loads the module, fills Gh2HostTable and calls the one export, gh2game_open(). The module copies what it needs and
// returns Gh2GameModule. Both carry the ABI version, the PPCContext layout and a hash of the two name lists the stubs were generated from
// (contract/imports.txt and the override list taken from the hooks' objects): any mismatch means the module was built for another host,
// and the launcher rebuilds it.
//
//   module -> host : kernel imports (`__imp__<Name>`, 179)       the module's stub jumps through imports[i]
//                    runtime services (gh2_resolve_indirect ...) the module's gh2_* functions call these
//   host -> module : overrides (`sub_<addr>` / `xstart`, ~916)  the module's strong `sub_X` stub jumps to the host's hook overrides[i]
//                    originals (`__imp__sub_<addr>`)            the host's stub jumps to the module's original code originals[i]
//                    image info (rex::PPCImageInfo)             what ReXApp is constructed with (code range, function table)
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
struct PPCContext;
using Gh2PPCFunc = void(PPCContext& ctx, uint8_t* base);
extern "C" {
#else
#include <stdbool.h>
struct PPCContext;
typedef void Gh2PPCFunc(struct PPCContext* ctx, uint8_t* base);
#endif

#define GH2GAME_ABI_VERSION 1u
#define GH2GAME_OPEN_SYMBOL "gh2game_open"
#define GH2GAME_MODULE_FILE "gh2game.dll"

typedef struct Gh2HostTable {
  uint32_t abi_version;          // GH2GAME_ABI_VERSION
  uint32_t struct_size;          // sizeof(Gh2HostTable)
  uint64_t context_layout;       // Gh2ContextLayout() of the host (gh2module/context_layout.h)
  uint64_t imports_hash;         // FNV-1a of the import list the host's tables were generated from
  uint64_t overrides_hash;       // FNV-1a of the override list
  uint32_t import_count;
  uint32_t override_count;

  // runtime services: the C boundary of gh2abi/abi.h
  Gh2PPCFunc* (*resolve_indirect)(uint32_t guest_address);
  uint64_t (*guest_tick_count)(void);
  void (*global_lock)(void);
  void (*global_unlock)(void);
  bool (*global_try_lock)(void);
  bool (*mmio_load)(uint32_t guest_address, uint32_t* out_value);
  bool (*mmio_store)(uint32_t guest_address, uint32_t value);
  void (*log)(int level, const char* text, size_t length);
  void (*spin_hint)(void);

  Gh2PPCFunc* const* imports;    // [import_count]: what the game's `__imp__<Name>` calls reach (the host's wrapped imports)
  Gh2PPCFunc* const* overrides;  // [override_count]: the host's hooks that replace game functions
} Gh2HostTable;

typedef struct Gh2GameModule {
  uint32_t abi_version;
  uint32_t struct_size;          // sizeof(Gh2GameModule)
  uint64_t context_layout;
  uint64_t imports_hash;
  uint64_t overrides_hash;
  uint32_t import_count;
  uint32_t override_count;
  const void* image_info;        // const rex::PPCImageInfo* (the generated PPCImageConfig)
  Gh2PPCFunc* const* originals;  // [override_count]: the game's own code of every overridden function (`__imp__sub_<addr>`)
  const char* error;             // null when the module accepted the host table; otherwise why not (the other fields may be partial)
} Gh2GameModule;

// The one export of the game module. Called once, before any guest code runs; the table must stay valid for the life of the process.
typedef const Gh2GameModule* (*Gh2GameOpenFunc)(const Gh2HostTable* host);

#ifdef __cplusplus
}  // extern "C"
#endif
