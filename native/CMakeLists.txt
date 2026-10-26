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
file(REMOVE "${GH2_SIMDE_ONLY_DIR}/simde")
file(CREATE_LINK "${GH2_SIMDE_DIR}/simde" "${GH2_SIMDE_ONLY_DIR}/simde" SYMBOLIC)

add_library(gh2abi_contract INTERFACE)
target_include_directories(gh2abi_contract INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/rt/abi/include")

add_library(gh2abi_headers INTERFACE)
target_link_libraries(gh2abi_headers INTERFACE gh2abi_contract)
target_include_directories(gh2abi_headers INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/rt/abi/shim")
target_include_directories(gh2abi_headers SYSTEM INTERFACE "${GH2_SIMDE_ONLY_DIR}")

add_library(gh2_cpu_flags INTERFACE)
target_compile_options(gh2_cpu_flags INTERFACE -mcmodel=large -msse4.1)

# ---- link flags: one --wrap per kernel import (GH2_TRACE_WRAP_FLAGS = generated tracing wrappers, GH2_HOOK_WRAP_FLAGS = the
# hand-written hooks), both generated from contract/imports.txt and hooks/hooked_imports.txt by tools/gen_import_tracer.py.
include("${CMAKE_CURRENT_SOURCE_DIR}/rt/trace/trace_wraps.cmake")
set(GH2_TRACE_WRAP_FLAGS ${GH2_TRACE_WRAP_FLAGS} PARENT_SCOPE)
set(GH2_HOOK_WRAP_FLAGS ${GH2_HOOK_WRAP_FLAGS} PARENT_SCOPE)

# ---- boundary tracer
add_library(gh2rt_trace OBJECT rt/trace/boundary_trace.cpp rt/trace/import_trace.gen.cpp)
target_include_directories(gh2rt_trace PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/rt/trace/include")
target_link_libraries(gh2rt_trace PRIVATE gh2abi_headers gh2_cpu_flags)

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
target_link_libraries(gh2rt_hooks PRIVATE gh2abi_headers gh2_cpu_flags gh2rt_trace gh2k_vfs gh2k_sync gh2k_thread gh2plat_audio gh2gpu gh2render)
add_dependencies(gh2rt_hooks gh2_codegen)

# ---- the generated game code against the shim (used when GH2_ISOLATED_CPU=ON)
add_library(gh2_cpu STATIC EXCLUDE_FROM_ALL ${REXGLUE_ENTRYPOINT_GENERATED_SOURCES})
target_include_directories(gh2_cpu PRIVATE "${REXGLUE_ENTRYPOINT_INCLUDE_DIR}")
target_link_libraries(gh2_cpu PRIVATE gh2abi_headers gh2_cpu_flags)
rexglue_apply_recomp_settings(gh2_cpu "${REXGLUE_ENTRYPOINT_INCLUDE_DIR}/gh2_pch.h")
add_dependencies(gh2_cpu gh2_codegen)

# ---- the contract on top of the SDK (the interim backend)
add_library(gh2rt_backend_rex STATIC EXCLUDE_FROM_ALL rt/backend_rex/backend_rex.cpp)
target_include_directories(gh2rt_backend_rex PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/rt/trace/include")
target_link_libraries(gh2rt_backend_rex PRIVATE gh2abi_contract rex::runtime)
rexglue_apply_target_settings(gh2rt_backend_rex)

# ---- platform: SDL3 input (dedicated polling thread, guitar classifier) and audio (frame queue + sink); the self-test tools are built on request
add_subdirectory(platform/input_sdl3 EXCLUDE_FROM_ALL)
add_subdirectory(platform/audio_sdl3 EXCLUDE_FROM_ALL)

# ---- kernel: the game's file system and the ARK reader (K.3)
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
    target_link_libraries(gh2gfx_guest_d3d PUBLIC gh2gfx_shader_cache)
    target_compile_options(gh2gfx_guest_d3d PRIVATE -Wall -Wextra)
    add_executable(gh2_d3d_observer_selftest EXCLUDE_FROM_ALL gfx/guest_d3d/observer_selftest.cpp)
    target_link_libraries(gh2_d3d_observer_selftest PRIVATE gh2gfx_guest_d3d)
    target_link_libraries(gh2rt_hooks PRIVATE gh2gfx_guest_d3d)   # gpu_renderer.cpp: the renderer's observer (shader hashes) and the shader cache's container helpers
    add_subdirectory(gfx/render)   # the native renderer needs the shader cache and the fetch scan above
endif()
