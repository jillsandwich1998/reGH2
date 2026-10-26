# gh2gpu: the GPU's command processor, natively (Phase 3): ring buffer, PM4 packets, registers, sync. Plain C++17: no SDK, no guest macros.
add_library(gh2gpu STATIC command_processor.cpp)
target_include_directories(gh2gpu PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_options(gh2gpu PRIVATE -Wall -Wextra)
set_target_properties(gh2gpu PROPERTIES POSITION_INDEPENDENT_CODE ON)
find_package(Threads REQUIRED)
target_link_libraries(gh2gpu PUBLIC Threads::Threads)
add_executable(gh2_gpu_selftest EXCLUDE_FROM_ALL gpu_selftest.cpp)
target_link_libraries(gh2_gpu_selftest PRIVATE gh2gpu)
