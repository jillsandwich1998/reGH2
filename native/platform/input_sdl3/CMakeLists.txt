# gh2plat input: the SDL3 side of Phase 2 input (guitar classifier, dedicated polling thread, player assignment) and its tools.
# Standalone (cmake -S native/platform/input_sdl3 -B <dir> -DCMAKE_PREFIX_PATH=<SDK prefix>) or added from native/CMakeLists.txt.
#
# SDL3 is the SDK's: its headers and its symbols, which librexruntime.so exports (SDL3 is linked statically into it). A different
# SDL3 must never end up in the same process. Nothing in this directory may include a rex/ header or use the rex:: namespace
# (native/tools/check_isolation.sh).
cmake_minimum_required(VERSION 3.20)
if(NOT PROJECT_NAME)
    project(gh2_platform_input LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

if(NOT TARGET gh2plat_sdl3)
    include("${CMAKE_CURRENT_LIST_DIR}/../sdl3.cmake")
endif()

add_library(gh2plat_input STATIC guitar_classifier.cpp input_core.cpp)
target_include_directories(gh2plat_input PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(gh2plat_input PUBLIC gh2plat_sdl3)
target_compile_options(gh2plat_input PRIVATE -Wall -Wextra)

add_executable(gh2_sdl_probe sdl_probe.cpp)
target_link_libraries(gh2_sdl_probe PRIVATE gh2plat_sdl3)
add_executable(gh2_input_selftest selftest.cpp uinput_guitar.cpp)
target_link_libraries(gh2_input_selftest PRIVATE gh2plat_input)
if(GH2_SDK_LIB_DIR)
    set_target_properties(gh2_sdl_probe gh2_input_selftest PROPERTIES BUILD_RPATH "${GH2_SDK_LIB_DIR}")
endif()
