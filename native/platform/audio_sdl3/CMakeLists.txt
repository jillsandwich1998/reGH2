# gh2plat audio: the SDL3 audio sink of Phase 2 (lock-free frame queue, 256-sample x 6-channel guest frames) and its self-test.
# Standalone (cmake -S native/platform/audio_sdl3 -B <dir> -DCMAKE_PREFIX_PATH=<SDK prefix>) or added from native/CMakeLists.txt.
cmake_minimum_required(VERSION 3.20)
if(NOT PROJECT_NAME)
    project(gh2_platform_audio LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

if(NOT TARGET gh2plat_sdl3)
    include("${CMAKE_CURRENT_LIST_DIR}/../sdl3.cmake")
endif()

add_library(gh2plat_audio STATIC audio_format.cpp audio_sink.cpp)
target_include_directories(gh2plat_audio PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(gh2plat_audio PUBLIC gh2plat_sdl3)
target_compile_options(gh2plat_audio PRIVATE -Wall -Wextra -ffp-contract=off)  # the stereo fold must round exactly like the SSE code it replaces

add_executable(gh2_audio_selftest audio_selftest.cpp)
target_link_libraries(gh2_audio_selftest PRIVATE gh2plat_audio)
if(GH2_SDK_LIB_DIR)
    set_target_properties(gh2_audio_selftest PROPERTIES BUILD_RPATH "${GH2_SDK_LIB_DIR}")
endif()
