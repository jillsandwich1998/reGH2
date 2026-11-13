# gh2k_vfs: the game's file system and the ARK reader, natively (Phase 2.7, K.3). Plain C++17: no SDK, no guest macros.
if(WIN32)
    # Windows has no system zlib: use one found by CMake (vcpkg, GH2_ZLIB_ROOT...), else build the pinned release from source (statically, inside this project).
    find_package(ZLIB QUIET)
    if(NOT ZLIB_FOUND)
        include(FetchContent)
        set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
        set(ZLIB_BUILD_STATIC ON CACHE BOOL "" FORCE)
        set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
        FetchContent_Declare(gh2_zlib
            URL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
            URL_HASH SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_MakeAvailable(gh2_zlib)
        # zlib's own CMake names the target "zlibstatic" and keeps zconf.h in its build directory
        add_library(ZLIB::ZLIB INTERFACE IMPORTED GLOBAL)
        target_link_libraries(ZLIB::ZLIB INTERFACE zlibstatic)
        target_include_directories(ZLIB::ZLIB INTERFACE "${gh2_zlib_SOURCE_DIR}" "${gh2_zlib_BINARY_DIR}")
        set(GH2_ZLIB_FETCHED ON)
    endif()
else()
    find_package(ZLIB REQUIRED)
endif()
add_library(gh2k_vfs STATIC vfs.cpp ark.cpp)
target_include_directories(gh2k_vfs PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(gh2k_vfs PUBLIC ZLIB::ZLIB)
target_compile_options(gh2k_vfs PRIVATE -Wall -Wextra)
set_target_properties(gh2k_vfs PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_executable(gh2_vfs_selftest EXCLUDE_FROM_ALL vfs_selftest.cpp)
target_link_libraries(gh2_vfs_selftest PRIVATE gh2k_vfs)
