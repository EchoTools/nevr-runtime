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
    "ws_bridge.cpp"
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
    "ws_bridge.h"
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
find_package(ixwebsocket CONFIG REQUIRED)

target_link_libraries(gamepatches PRIVATE minhook common version CURL::libcurl ixwebsocket::ixwebsocket)
target_link_directories(gamepatches PRIVATE common)

# Disable PCH for ws_bridge.cpp — ixwebsocket headers conflict with Windows.h macros
set_source_files_properties(ws_bridge.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

set_target_properties(gamepatches PROPERTIES
  OUTPUT_NAME "BugSplat64"
  PREFIX ""
)

# Disable specific warnings (MSVC only)
if(MSVC)
  target_compile_options(gamepatches PRIVATE /wd4251)
endif()

# ixwebsocket include path workaround: vcpkg adds include/ixwebsocket but
# headers use #include <ixwebsocket/...>, so we need the parent directory.
get_target_property(_ixws_includes ixwebsocket::ixwebsocket INTERFACE_INCLUDE_DIRECTORIES)
foreach(_dir ${_ixws_includes})
  get_filename_component(_parent_dir "${_dir}" DIRECTORY)
  target_include_directories(gamepatches SYSTEM PRIVATE "${_parent_dir}")
endforeach()

# MinGW: mbedTLS (ixwebsocket TLS backend) needs BCryptGenRandom from bcrypt.dll.
# Build a minimal import lib to avoid pulling in all bcrypt exports.
if(MINGW)
  add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/stubs/libbcrypt.a"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/stubs"
    COMMAND x86_64-w64-mingw32-dlltool -d "${CMAKE_CURRENT_SOURCE_DIR}/bcrypt_minimal.def"
            -l "${CMAKE_CURRENT_BINARY_DIR}/stubs/libbcrypt.a"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/bcrypt_minimal.def"
    COMMENT "Generating minimal bcrypt import library for gamepatches"
  )
  add_custom_target(gamepatches_bcrypt_minimal DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/stubs/libbcrypt.a")
  add_dependencies(gamepatches gamepatches_bcrypt_minimal)
  target_link_libraries(gamepatches PRIVATE
    $<LINK_LIBRARY:WHOLE_ARCHIVE,${CMAKE_CURRENT_BINARY_DIR}/stubs/libbcrypt.a>
  )
endif()
