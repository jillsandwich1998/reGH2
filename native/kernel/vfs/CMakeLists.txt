# gh2k_vfs: the game's file system and the ARK reader, natively (Phase 2.7, K.3). Plain C++17: no SDK, no guest macros.
find_package(ZLIB REQUIRED)
add_library(gh2k_vfs STATIC vfs.cpp ark.cpp)
target_include_directories(gh2k_vfs PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(gh2k_vfs PUBLIC ZLIB::ZLIB)
target_compile_options(gh2k_vfs PRIVATE -Wall -Wextra)
set_target_properties(gh2k_vfs PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_executable(gh2_vfs_selftest EXCLUDE_FROM_ALL vfs_selftest.cpp)
target_link_libraries(gh2_vfs_selftest PRIVATE gh2k_vfs)
