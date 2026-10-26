# The SDK's SDL3 for the gh2plat modules: target gh2plat_sdl3 (headers + symbols) and GH2_SDK_LIB_DIR (rpath for stand-alone tools).
# SDL3 is the SDK's: its headers and its symbols, which librexruntime.so exports (SDL3 is linked statically into it). A different
# SDL3 must never end up in the same process. Nothing under native/platform may include a rex/ header or use the rex:: namespace
# (native/tools/check_isolation.sh).
if(TARGET rex::runtime)
    # inside the app build: the SDK is already found, its SDL3 headers and librexruntime.so come with the imported target
    add_library(gh2plat_sdl3 INTERFACE)
    target_link_libraries(gh2plat_sdl3 INTERFACE rex::runtime)
    set(GH2_SDK_LIB_DIR "")
else()
    find_path(GH2_SDK_INCLUDE SDL3/SDL.h HINTS ${CMAKE_PREFIX_PATH} PATH_SUFFIXES include NO_DEFAULT_PATH)
    find_library(GH2_SDK_RUNTIME rexruntime HINTS ${CMAKE_PREFIX_PATH} PATH_SUFFIXES lib NO_DEFAULT_PATH)
    if(NOT GH2_SDK_INCLUDE OR NOT GH2_SDK_RUNTIME)
        message(FATAL_ERROR "SDK's SDL3 not found: set CMAKE_PREFIX_PATH to the ReXGlue SDK prefix (needs include/SDL3 and lib/librexruntime.so)")
    endif()
    set(GH2_SDL3_ONLY_DIR "${CMAKE_CURRENT_BINARY_DIR}/sdl3_only")
    file(MAKE_DIRECTORY "${GH2_SDL3_ONLY_DIR}")
    file(REMOVE "${GH2_SDL3_ONLY_DIR}/SDL3")
    file(CREATE_LINK "${GH2_SDK_INCLUDE}/SDL3" "${GH2_SDL3_ONLY_DIR}/SDL3" SYMBOLIC)
    get_filename_component(GH2_SDK_LIB_DIR "${GH2_SDK_RUNTIME}" DIRECTORY)
    add_library(gh2plat_sdl3 INTERFACE)
    target_include_directories(gh2plat_sdl3 SYSTEM INTERFACE "${GH2_SDL3_ONLY_DIR}")
    target_link_libraries(gh2plat_sdl3 INTERFACE "${GH2_SDK_RUNTIME}")
endif()
