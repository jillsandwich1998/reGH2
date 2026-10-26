# gh2k_thread: the guest-visible state of a thread (KPCR, KTHREAD, TLS block, stack), built natively (Phase 2.7). Plain C++17: no SDK, no guest macros.
add_library(gh2k_thread STATIC thread.cpp)
target_include_directories(gh2k_thread PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(gh2k_thread PUBLIC gh2k_mem)
target_compile_options(gh2k_thread PRIVATE -Wall -Wextra)
set_target_properties(gh2k_thread PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_executable(gh2_thread_selftest EXCLUDE_FROM_ALL thread_selftest.cpp)
target_link_libraries(gh2_thread_selftest PRIVATE gh2k_thread gh2k_sync)
