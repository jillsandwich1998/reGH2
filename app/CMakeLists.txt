# gh2 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(gh2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(GH2_SOURCES
    src/main.cpp
    src/gh2_host.cpp
    src/graphics_menu.cpp
    src/controllers.cpp
    src/keyboard.cpp
    src/fps_meter.cpp
    src/problem_report.cpp
    src/png_writer.cpp
)

if(WIN32)
    add_executable(gh2 WIN32 ${GH2_SOURCES})
else()
    add_executable(gh2 ${GH2_SOURCES})
endif()

rexglue_setup_target(gh2 GPU_PLUGINS xenos)

target_link_options(gh2 PRIVATE
    -Wl,--wrap=__imp__XamInputGetState
    -Wl,--wrap=__imp__XamInputGetCapabilities
    -Wl,--wrap=__imp__MmAllocatePhysicalMemoryEx
    -Wl,--wrap=__imp__VdSwap
    -Wl,--wrap=__imp__NtCreateFile
    -Wl,--wrap=__imp__NtClose
    -Wl,--wrap=__imp__XamContentCreateEnumerator
    -Wl,--wrap=__imp__XamContentCreateEx
)

# Must match the SDK build (patched): the macro changes the layout of rex::ui::Presenter.
target_compile_definitions(gh2 PRIVATE REX_HAS_FIDELITYFX_SDK=1)
