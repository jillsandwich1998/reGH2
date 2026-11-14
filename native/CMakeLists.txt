# Phase 2 native pieces (see native/README.md and docs/abi_recomp_runtime.md). Added from app/CMakeLists.txt with
# add_subdirectory(), after generated/rexglue.cmake has been included (it provides the generated-source variables and the SDK).
#
# Targets
#   gh2abi_headers      INTERFACE  the shim: contract (gh2abi/abi.h) + include/rex/* (vendored CPU headers and one-line aliases) + simde.
#                                  Everything compiled "shim only" below sees exactly these headers and no SDK header.
#   gh2abi_contract     INTERFACE  gh2abi/abi.h alone (what the SDK-backed backend may see)
#   gh2_cpu_flags       INTERFACE  compile flags every piece that touches the guest ABI must share with the generated code
#   gh2rt_trace         OBJECT     boundary tracer + generated import wrappers          (shim only)
#   gh2rt_hooks         OBJECT     hand-written import / function hooks                 (shim only)
#   gh2_cpu             STATIC     the generated game code compiled against the shim    (shim only; EXCLUDE_FROM_ALL)
#   gh2rt_backend_rex   STATIC     the gh2abi contract implemented on the ReXGlue SDK (the only piece here that sees SDK headers)
#   gh2plat_input       STATIC     SDL3 input: guitar classifier + the dedicated polling thread (platform/input_sdl3; no rex:: either)
#   gh2plat_audio       STATIC     SDL3 audio sink: lock-free 256-sample x 6-channel frame queue (platform/audio_sdl3)
#   gh2gfx_shader_cache STATIC     shaders.pak reader + unknown-hash log (gfx/shader_cache; needs third_party/fetch.sh)
#   gh2gfx_guest_d3d    STATIC     G.0: observer that correlates the game's D3D shader binds with the cache (gfx/guest_d3d/observer.cpp); the hooks and the
#                                  discovery probe of the D3D library are in gh2rt_hooks (gfx/guest_d3d/d3d_hooks.cpp, d3d_probe*.cpp)

# ---- simde: header-only SIMD emulation used by the PPC intrinsics. Take the SDK's copy first (same version as the legacy
# build), then the system's. Only the simde/ directory is exposed, never the SDK's rex/ headers.
set(GH2_SIMDE_DIR "" CACHE PATH "Directory that contains simde/ (default: SDK prefix, then the system)")
if(NOT GH2_SIMDE_DIR)
    find_path(GH2_SIMDE_FOUND simde/x86/sse4.1.h PATHS ${CMAKE_PREFIX_PATH} PATH_SUFFIXES include NO_DEFAULT_PATH)
    if(NOT GH2_SIMDE_FOUND)
        find_path(GH2_SIMDE_FOUND simde/x86/sse4.1.h)
    endif()
    set(GH2_SIMDE_DIR "${GH2_SIMDE_FOUND}")
endif()
if(NOT GH2_SIMDE_DIR)
    message(FATAL_ERROR "simde not found: set GH2_SIMDE_DIR to a directory that contains simde/")
endif()
set(GH2_SIMDE_ONLY_DIR "${CMAKE_CURRENT_BINARY_DIR}/simde_only")
file(MAKE_DIRECTORY "${GH2_SIMDE_ONLY_DIR}")
if(WIN32)
    # (a copy: symbolic links need a privilege on Windows; the headers are only read)
    if(NOT EXISTS "${GH2_SIMDE_ONLY_DIR}/simde/x86/sse4.1.h")
        file(COPY "${GH2_SIMDE_DIR}/simde" DESTINATION "${GH2_SIMDE_ONLY_DIR}")
    endif()
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
else()
    file(REMOVE "${GH2_SIMDE_ONLY_DIR}/simde")
    file(CREATE_LINK "${GH2_SIMDE_DIR}/simde" "${GH2_SIMDE_ONLY_DIR}/simde" SYMBOLIC)
endif()

add_library(gh2abi_contract INTERFACE)
target_include_directories(gh2abi_contract INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/rt/abi/include")

add_library(gh2abi_headers INTERFACE)
target_link_libraries(gh2abi_headers INTERFACE gh2abi_contract)
target_include_directories(gh2abi_headers INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/rt/abi/shim")
target_include_directories(gh2abi_headers SYSTEM INTERFACE "${GH2_SIMDE_ONLY_DIR}")

add_library(gh2_cpu_flags INTERFACE)
if(WIN32)
    target_compile_options(gh2_cpu_flags INTERFACE -msse4.1)   # (no -mcmodel=large: it is an ELF option; a COFF image stays under 2 GB)
else()
    target_compile_options(gh2_cpu_flags INTERFACE -mcmodel=large -msse4.1)
endif()

# ---- link flags: one --wrap per kernel import (GH2_TRACE_WRAP_FLAGS = generated tracing wrappers, GH2_HOOK_WRAP_FLAGS = the
# hand-written hooks), both generated from contract/imports.txt and hooks/hooked_imports.txt by tools/gen_import_tracer.py.
include("${CMAKE_CURRENT_SOURCE_DIR}/rt/trace/trace_wraps.cmake")
if(WIN32)
    # lld-link spells GNU ld's -Wl,--wrap=SYM as /wrap:SYM (checked with LLD 23 on __imp__ names: __wrap_X and __real_X resolve as with ld)
    list(TRANSFORM GH2_TRACE_WRAP_FLAGS REPLACE "--wrap=" "/wrap:")
    list(TRANSFORM GH2_HOOK_WRAP_FLAGS REPLACE "--wrap=" "/wrap:")
endif()
set(GH2_TRACE_WRAP_FLAGS ${GH2_TRACE_WRAP_FLAGS} PARENT_SCOPE)
set(GH2_HOOK_WRAP_FLAGS ${GH2_HOOK_WRAP_FLAGS} PARENT_SCOPE)

# ---- boundary tracer
add_library(gh2rt_trace OBJECT rt/trace/boundary_trace.cpp rt/trace/import_trace.gen.cpp)
target_include_directories(gh2rt_trace PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/rt/trace/include")
target_link_libraries(gh2rt_trace PRIVATE gh2abi_headers gh2_cpu_flags gh2os)

# ---- hooks (they use the generated gh2_pch.h for the REX_LOAD_*/REX_STORE_* macros, never an SDK header)
add_library(gh2rt_hooks OBJECT
    rt/hooks/host_services.cpp
    rt/hooks/intro_skip.cpp
    rt/hooks/hooks_input.cpp
    rt/hooks/hooks_video.cpp
    rt/hooks/hooks_file.cpp
    rt/hooks/vfs_bridge.cpp
    rt/hooks/hooks_sync.cpp
    rt/hooks/hooks_threads.cpp
    rt/hooks/hooks_objects.cpp
    rt/hooks/hooks_kthreads.cpp
    rt/hooks/hooks_audio.cpp
    rt/hooks/hooks_gpu.cpp
    rt/hooks/gpu_renderer.cpp
    rt/hooks/hooks_memory.cpp
    rt/hooks/hooks_misc.cpp
    rt/hooks/stall_watchdog.cpp
    gfx/guest_d3d/d3d_hooks.cpp
    gfx/guest_d3d/d3d_probe.cpp
    gfx/guest_d3d/d3d_probe.gen.cpp)
target_include_directories(gh2rt_hooks
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/rt/hooks/include" "${CMAKE_CURRENT_SOURCE_DIR}/gfx/guest_d3d/include"
    PRIVATE "${REXGLUE_ENTRYPOINT_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/rt/hooks")
target_link_libraries(gh2rt_hooks PRIVATE gh2abi_headers gh2_cpu_flags gh2rt_trace gh2k_vfs gh2k_sync gh2k_thread gh2plat_audio gh2gpu gh2render gh2os gh2k_hostaddr)
add_dependencies(gh2rt_hooks gh2_codegen)

# ---- the generated game code against the shim (used when GH2_ISOLATED_CPU=ON)
add_library(gh2_cpu STATIC EXCLUDE_FROM_ALL ${REXGLUE_ENTRYPOINT_GENERATED_SOURCES})
target_include_directories(gh2_cpu PRIVATE "${REXGLUE_ENTRYPOINT_INCLUDE_DIR}")
target_link_libraries(gh2_cpu PRIVATE gh2abi_headers gh2_cpu_flags)
rexglue_apply_recomp_settings(gh2_cpu "${REXGLUE_ENTRYPOINT_INCLUDE_DIR}/gh2_pch.h")
add_dependencies(gh2_cpu gh2_codegen)

# ---- the game module (GH2_GAME_MODULE=ON, the Windows default): the generated code is not linked into the executable but built as
# gh2game.dll with llvm-mingw (Windows) or libgh2game.so with this build's clang (Linux) (native/tools/build_game_module.py), which the host loads at start (native/rt/module, gh2module/game_module.h).
#   overrides.txt              the game functions the hooks replace: every sub_X / xstart the hooks' objects define (also what the module needs)
#   gh2host_stubs.gen.cpp      the host's tables (imports, hooks) and its __imp__sub_X stubs onto the module's original code
#   gh2game                    the module itself, next to the executable
if(GH2_GAME_MODULE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(GH2_MODULE_DIR "${CMAKE_CURRENT_BINARY_DIR}/module")
    set(GH2_MODULE_OVERRIDES "${GH2_MODULE_DIR}/overrides.txt" PARENT_SCOPE)
    set(GH2_MODULE_STUBS_TOOL "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_module_stubs.py")
    set(GH2_IMPORTS_LIST "${CMAKE_CURRENT_SOURCE_DIR}/rt/abi/contract/imports.txt")
    add_custom_command(OUTPUT "${GH2_MODULE_DIR}/overrides.txt"
        COMMAND Python3::Interpreter "${GH2_MODULE_STUBS_TOOL}" overrides --nm "${CMAKE_NM}" --out "${GH2_MODULE_DIR}/overrides.txt" $<TARGET_OBJECTS:gh2rt_hooks>
        DEPENDS $<TARGET_OBJECTS:gh2rt_hooks> "${GH2_MODULE_STUBS_TOOL}"
        COMMENT "Listing the game functions the hooks override"
        COMMAND_EXPAND_LISTS VERBATIM)
    add_custom_command(OUTPUT "${GH2_MODULE_DIR}/gh2host_stubs.gen.cpp"
        COMMAND Python3::Interpreter "${GH2_MODULE_STUBS_TOOL}" host --imports "${GH2_IMPORTS_LIST}" --overrides "${GH2_MODULE_DIR}/overrides.txt"
                --out "${GH2_MODULE_DIR}/gh2host_stubs.gen.cpp"
        DEPENDS "${GH2_MODULE_DIR}/overrides.txt" "${GH2_IMPORTS_LIST}" "${GH2_MODULE_STUBS_TOOL}"
        VERBATIM)
    add_library(gh2rt_module_host OBJECT rt/module/host_side.cpp "${GH2_MODULE_DIR}/gh2host_stubs.gen.cpp")
    target_include_directories(gh2rt_module_host PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/rt/module/include")
    target_link_libraries(gh2rt_module_host PRIVATE gh2abi_headers gh2_cpu_flags)

    if(WIN32)
        set(GH2_LLVM_MINGW "$ENV{GH2_LLVM_MINGW}" CACHE PATH "llvm-mingw (the directory with bin/clang++.exe) that builds the game module")
        if(NOT EXISTS "${GH2_LLVM_MINGW}/bin/clang++.exe")
            message(FATAL_ERROR "GH2_GAME_MODULE needs llvm-mingw: set GH2_LLVM_MINGW (now '${GH2_LLVM_MINGW}')")
        endif()
        set(GH2_MODULE_TOOLCHAIN_ARGS --os windows --toolchain "${GH2_LLVM_MINGW}")
        set(GH2_MODULE_FILE gh2game.dll)
    else()
        # the compiler of this build (and the sysroot of a toolchain file, as the AppImage's bundled one)
        set(_mflags "")
        if(CMAKE_SYSROOT)
            string(APPEND _mflags " --sysroot=${CMAKE_SYSROOT}")
        endif()
        string(APPEND _mflags " ${CMAKE_CXX_FLAGS_INIT}")
        string(STRIP "${_mflags}" _mflags)
        set(GH2_MODULE_TOOLCHAIN_ARGS --os linux --cxx "${CMAKE_CXX_COMPILER}" "--cflags=${_mflags}")
        set(GH2_MODULE_FILE libgh2game.so)
    endif()
    # (always run: the module's own Ninja knows what is out of date and does nothing when nothing is)
    add_custom_target(gh2game ALL
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_game_module.py"
                --generated "${REXGLUE_ENTRYPOINT_INCLUDE_DIR}" --native "${CMAKE_CURRENT_SOURCE_DIR}" --simde "${GH2_SIMDE_ONLY_DIR}"
                --overrides "${GH2_MODULE_DIR}/overrides.txt" ${GH2_MODULE_TOOLCHAIN_ARGS} --ninja "${CMAKE_MAKE_PROGRAM}"
                --build "${GH2_MODULE_DIR}/game" --out "${CMAKE_BINARY_DIR}/${GH2_MODULE_FILE}"
        DEPENDS "${GH2_MODULE_DIR}/overrides.txt"
        BYPRODUCTS "${CMAKE_BINARY_DIR}/${GH2_MODULE_FILE}"
        COMMENT "Building the game module (${GH2_MODULE_FILE})"
        USES_TERMINAL VERBATIM)
    add_dependencies(gh2game gh2_codegen)
endif()

# ---- the contract on top of the SDK (the interim backend)
add_library(gh2rt_backend_rex STATIC EXCLUDE_FROM_ALL rt/backend_rex/backend_rex.cpp)
target_include_directories(gh2rt_backend_rex PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/rt/trace/include")
target_link_libraries(gh2rt_backend_rex PRIVATE gh2abi_contract rex::runtime)
rexglue_apply_target_settings(gh2rt_backend_rex)

# ---- platform: the OS services behind one API (gh2os), SDL3 input (dedicated polling thread, guitar classifier) and audio (frame queue + sink); the self-test tools are built on request
add_subdirectory(platform/os)
add_subdirectory(platform/input_sdl3 EXCLUDE_FROM_ALL)
add_subdirectory(platform/audio_sdl3 EXCLUDE_FROM_ALL)

# ---- kernel: the game's file system and the ARK reader (K.3)
add_subdirectory(kernel/hostaddr)
add_subdirectory(kernel/vfs)
add_subdirectory(kernel/sync)
add_subdirectory(kernel/mem)
add_subdirectory(kernel/thread)
add_subdirectory(gfx/gpu)
add_subdirectory(gfx/vk)

# ---- gfx: the shader cache reader (xxHash comes with the XenosRecomp clone of third_party/fetch.sh); not linked by anything yet
# xxHash: the copy of the XenosRecomp clone (a developer's tree) or the SDK's own (the player's build)
find_path(GH2_XXHASH_PROBE xxhash.h HINTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/XenosRecomp/thirdparty/xxHash" PATH_SUFFIXES include)
if(GH2_XXHASH_PROBE)
    add_subdirectory(gfx/shader_cache EXCLUDE_FROM_ALL)
    # the D3D shader observer (G.0): correlates what the game's D3D library binds with the shader cache; the hooks themselves are in gh2rt_hooks
    add_library(gh2gfx_guest_d3d STATIC EXCLUDE_FROM_ALL gfx/guest_d3d/observer.cpp gfx/guest_d3d/observer_install.cpp gfx/guest_d3d/graphics_state.cpp gfx/guest_d3d/fetch_scan.cpp)
    target_include_directories(gh2gfx_guest_d3d PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/gfx/guest_d3d" "${CMAKE_CURRENT_SOURCE_DIR}/gfx/guest_d3d/include")
    target_link_libraries(gh2gfx_guest_d3d PUBLIC gh2gfx_shader_cache gh2k_hostaddr)
    target_compile_options(gh2gfx_guest_d3d PRIVATE -Wall -Wextra)
    add_executable(gh2_d3d_observer_selftest EXCLUDE_FROM_ALL gfx/guest_d3d/observer_selftest.cpp)
    target_link_libraries(gh2_d3d_observer_selftest PRIVATE gh2gfx_guest_d3d)
    target_link_libraries(gh2rt_hooks PRIVATE gh2gfx_guest_d3d)   # gpu_renderer.cpp: the renderer's observer (shader hashes) and the shader cache's container helpers
    add_subdirectory(gfx/render)   # the native renderer needs the shader cache and the fetch scan above
endif()
