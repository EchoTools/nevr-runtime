# Game Patches — deployed as BugSplat64.dll (replaces the original crash reporter).
# The game statically imports BugSplat64.dll, so it loads at process startup
# before WinMain — giving us the earliest possible hook point.
# Uses MinHook for function hooking (built from extern/minhook)

set(PATCHES_SOURCES
    "dllmain.cpp"
    "initialize.cpp"
    "cli.cpp"
    "boot.cpp"
    "config.cpp"
    "mode_patches.cpp"
    "crash_recovery.cpp"
    "platform_compat.cpp"
    "state_machine.cpp"
    "plugin_loader.cpp"
    "resource_override.cpp"
    "winhttp_stub.cpp"
)

set(PATCHES_HEADERS
    "patches.h"
    "gamepatches_internal.h"
    "initialize.h"
    "cli.h"
    "boot.h"
    "config.h"
    "mode_patches.h"
    "crash_recovery.h"
    "platform_compat.h"
    "resource_override.h"
    "state_machine.h"
    "plugin_loader.h"
    "winhttp_stub.h"
    "process_mem.h"
    "patch_addresses.h"
)

add_library(gamepatches SHARED ${PATCHES_SOURCES} ${PATCHES_HEADERS}
                               "exports.def")

target_compile_definitions(gamepatches PRIVATE USE_MINHOOK)

target_precompile_headers(gamepatches PRIVATE "${CMAKE_SOURCE_DIR}/src/common/pch.h")
target_include_directories(gamepatches PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/src/common
  ${CMAKE_SOURCE_DIR}/plugins/common/include
)

find_package(CURL REQUIRED)
target_link_libraries(gamepatches PRIVATE minhook common version CURL::libcurl)
target_link_directories(gamepatches PRIVATE common)

set_target_properties(gamepatches PROPERTIES
  OUTPUT_NAME "BugSplat64"
  PREFIX ""
)

# Disable specific warnings (MSVC only)
if(MSVC)
  target_compile_options(gamepatches PRIVATE /wd4251)
endif()
