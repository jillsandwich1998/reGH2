# gh2gfx shader cache: reader of shaders.pak + unknown-hash logging, and a tool that checks a cache directory.
# Standalone (cmake -S native/gfx/shader_cache -B <dir>) or added from native/CMakeLists.txt (EXCLUDE_FROM_ALL).
cmake_minimum_required(VERSION 3.20)
if(NOT PROJECT_NAME)
    project(gh2_shader_cache LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

# xxHash (header-only use): the copy that comes with XenosRecomp (third_party/fetch.sh), or the SDK's / the system's.
find_path(GH2_XXHASH_DIR xxhash.h
    HINTS "${CMAKE_CURRENT_LIST_DIR}/../../third_party/XenosRecomp/thirdparty/xxHash" PATH_SUFFIXES include)
if(NOT GH2_XXHASH_DIR)
    message(FATAL_ERROR "xxhash.h not found: run native/third_party/fetch.sh or install xxHash")
endif()

add_library(gh2gfx_shader_cache STATIC shader_cache.cpp)
target_include_directories(gh2gfx_shader_cache PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_include_directories(gh2gfx_shader_cache SYSTEM PRIVATE "${GH2_XXHASH_DIR}")

add_executable(gh2_shader_cache_tool shader_cache_tool.cpp)
target_link_libraries(gh2_shader_cache_tool PRIVATE gh2gfx_shader_cache)
