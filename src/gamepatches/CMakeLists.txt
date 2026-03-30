# Game Patches — modular DLL for runtime hooks and memory patches
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
    "winhttp_stub.cpp"
    "asset_cdn.cpp"
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
    "state_machine.h"
    "plugin_loader.h"
    "winhttp_stub.h"
    "asset_cdn.h"
    "process_mem.h"
    "patch_addresses.h"
)

add_library(gamepatches SHARED ${PATCHES_SOURCES} ${PATCHES_HEADERS}
                               "exports.def")

target_compile_definitions(gamepatches PRIVATE USE_MINHOOK)

target_precompile_headers(gamepatches PRIVATE "${CMAKE_SOURCE_DIR}/src/common/pch.h")
target_include_directories(gamepatches PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

find_package(CURL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(gamepatches PRIVATE minhook common version CURL::libcurl nlohmann_json::nlohmann_json)
target_link_directories(gamepatches PRIVATE common)

# Set output name to remove 'lib' prefix on MinGW
set_target_properties(gamepatches PROPERTIES
  OUTPUT_NAME "gamepatches"
  PREFIX ""
)

# Disable specific warnings (MSVC only)
if(MSVC)
  target_compile_options(gamepatches PRIVATE /wd4251)
endif()
