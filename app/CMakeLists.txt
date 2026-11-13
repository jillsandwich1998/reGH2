# gh2 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake. The hooks, the boundary tracer and the shim that isolates the generated code
# from the runtime live in native/ (Phase 2, see docs/abi_recomp_runtime.md).

cmake_minimum_required(VERSION 3.25)
project(gh2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# GH2_ISOLATED_CPU=ON compiles the generated game code against native/rt/abi only (no SDK header, no `rex::` symbol); the runtime
# services it needs come from native/rt/backend_rex. OFF (default for now) compiles it against the SDK headers as before.
# Hooks and tracer are always built against native/rt/abi only.
option(GH2_ISOLATED_CPU "Compile the generated code against the gh2abi shim instead of the SDK headers (the hooks need the runtime services of native/rt/backend_rex: always ON for a real build)" ON)
# GH2_GAME_MODULE=ON (Windows default) builds the generated code as a separate module, gh2game.dll, with llvm-mingw (redistributable: the
# player's PC needs no Microsoft build tools for it) and loads it at start; OFF links it into the executable as before (the Linux default).
if(WIN32)
    option(GH2_GAME_MODULE "Build the generated game code as gh2game.dll with llvm-mingw (needs GH2_ISOLATED_CPU)" ON)
else()
    option(GH2_GAME_MODULE "Build the generated game code as a separate module (Windows only for now)" OFF)
endif()
if(GH2_GAME_MODULE AND NOT GH2_ISOLATED_CPU)
    message(FATAL_ERROR "GH2_GAME_MODULE needs GH2_ISOLATED_CPU=ON")
endif()
set(GH2_NATIVE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/native" CACHE PATH "Phase 2 runtime pieces (native/)")
add_subdirectory("${GH2_NATIVE_DIR}" native)

# Sources
set(GH2_SOURCES
    src/main.cpp
    src/gh2_host_services.cpp
    src/graphics_menu.cpp
    src/controllers.cpp
    src/keyboard.cpp
    src/native_input.cpp
    src/native_arena.cpp
    src/native_audio.cpp
    src/native_window.cpp
    src/native_overlay.cpp
    src/menu_pad.cpp
    src/shader_bootstrap.cpp
    src/fps_meter.cpp
    src/problem_report.cpp
    src/png_writer.cpp
    src/zip_writer.cpp
    $<TARGET_OBJECTS:gh2rt_hooks>
    $<TARGET_OBJECTS:gh2rt_trace>
)

if(WIN32)
    add_executable(gh2 WIN32 ${GH2_SOURCES})
else()
    add_executable(gh2 ${GH2_SOURCES})
endif()

if(GH2_ISOLATED_CPU)
    # rexglue_setup_target() would build the generated sources against the SDK headers; native/ builds them (gh2_cpu).
    unset(REXGLUE_ENTRYPOINT_GENERATED_SOURCES)
endif()
rexglue_setup_target(gh2 GPU_PLUGINS xenos)

target_include_directories(gh2 PRIVATE "${GH2_NATIVE_DIR}/rt/hooks/include")
target_compile_definitions(gh2 PRIVATE GH2_NATIVE_DIR="${GH2_NATIVE_DIR}")   # where the shader tools are when run from a build tree
target_link_libraries(gh2 PRIVATE gh2plat_input gh2plat_audio gh2k_vfs gh2k_sync gh2k_mem gh2k_thread gh2gpu gh2render gh2vk gh2os)
if(TARGET gh2gfx_guest_d3d)
    target_link_libraries(gh2 PRIVATE gh2gfx_guest_d3d)
    target_compile_definitions(gh2 PRIVATE GH2_HAVE_D3D_OBSERVER=1)
endif()
if(GH2_GAME_MODULE)
    # the game code comes from gh2game.dll at start (src/gh2_app.h -> gh2module::OpenGameModule)
    target_sources(gh2 PRIVATE $<TARGET_OBJECTS:gh2rt_module_host>)
    target_include_directories(gh2 PRIVATE "${GH2_NATIVE_DIR}/rt/module/include")
    target_compile_definitions(gh2 PRIVATE GH2_GAME_MODULE=1)
    target_link_libraries(gh2 PRIVATE gh2rt_backend_rex)
    add_dependencies(gh2 gh2game)
elseif(GH2_ISOLATED_CPU)
    target_link_libraries(gh2 PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,gh2_cpu>" gh2rt_backend_rex)
endif()

# Every kernel import the guest calls is wrapped: the generated tracing wrappers (GH2_TRACE_WRAP_FLAGS) and the hand-written hooks
# (GH2_HOOK_WRAP_FLAGS: XamInputGetState/GetCapabilities, MmAllocatePhysicalMemoryEx, VdSwap, NtCreateFile, NtClose,
# XamContentCreateEnumerator/Ex). Both lists come from native/rt/abi/contract/imports.txt (native/tools/gen_import_tracer.py).
target_link_options(gh2 PRIVATE ${GH2_TRACE_WRAP_FLAGS} ${GH2_HOOK_WRAP_FLAGS})

if(WIN32)
    target_link_options(gh2 PRIVATE -Wl,/DEBUG)   # a PDB with the symbol names (no debug info in the code): crash dumps and scripts/windows/sample-threads.ps1 name the guest functions
    # __builtin_cpu_supports (native/gfx/render/tex_hash.cpp) needs __cpu_model from compiler-rt's builtins, which the SDK's -nostdlib link line leaves out
    execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -rtlib=compiler-rt -print-libgcc-file-name OUTPUT_VARIABLE GH2_CLANG_BUILTINS OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(EXISTS "${GH2_CLANG_BUILTINS}")
        target_link_libraries(gh2 PRIVATE "${GH2_CLANG_BUILTINS}")
    else()
        message(WARNING "clang_rt.builtins not found (${GH2_CLANG_BUILTINS}): __builtin_cpu_supports will not link")
    endif()
endif()

# Must match the SDK build (patched): the macro changes the layout of rex::ui::Presenter.
target_compile_definitions(gh2 PRIVATE REX_HAS_FIDELITYFX_SDK=1)
