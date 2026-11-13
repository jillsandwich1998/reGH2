# gh2k_hostaddr: header-only, where a guest address lives in the host's address space (see include/gh2k/host_addr.h).
add_library(gh2k_hostaddr INTERFACE)
target_include_directories(gh2k_hostaddr INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
