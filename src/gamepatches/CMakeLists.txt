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
    "resource_override.cpp"
    "winhttp_stub.cpp"
    "builtin_log_filter.cpp"
    "builtin_server_timing.cpp"
    # builtin_token_auth.cpp excluded: requires curl which breaks dbgcore.dll loading
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
    "builtin_log_filter.h"
    "builtin_server_timing.h"
)

add_library(gamepatches SHARED ${PATCHES_SOURCES} ${PATCHES_HEADERS}
                               "exports.def")

# Build-time embedded keys (set via cmake -D or cmake/local.cmake)
set(NEVR_SERVER_KEY "" CACHE STRING "Nakama server key (embedded at build time)")
set(NEVR_HTTP_KEY "" CACHE STRING "Nakama HTTP key (embedded at build time)")
set(NEVR_API_URL "https://api.echovrce.com" CACHE STRING "Nakama API base URL")

target_compile_definitions(gamepatches PRIVATE
  USE_MINHOOK
  NEVR_SERVER_KEY="${NEVR_SERVER_KEY}"
  NEVR_HTTP_KEY="${NEVR_HTTP_KEY}"
  NEVR_API_URL="${NEVR_API_URL}"
)

target_precompile_headers(gamepatches PRIVATE "${CMAKE_SOURCE_DIR}/src/common/pch.h")
target_include_directories(gamepatches PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/src/common
  ${CMAKE_SOURCE_DIR}/plugins/common/include
)

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
