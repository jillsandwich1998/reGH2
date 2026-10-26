# gh2k_mem: the guest's 4 GB address space, its heaps and thread stacks (Phase 2.7, K.1). Plain C++17 + POSIX: no SDK, no guest macros.
add_library(gh2k_mem STATIC mem.cpp)
target_include_directories(gh2k_mem PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_options(gh2k_mem PRIVATE -Wall -Wextra)
set_target_properties(gh2k_mem PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_executable(gh2_mem_selftest EXCLUDE_FROM_ALL mem_selftest.cpp)
target_link_libraries(gh2_mem_selftest PRIVATE gh2k_mem)
