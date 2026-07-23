# Game Patches — deployed as BugSplat64.dll (replaces the original crash reporter).
# The game statically imports BugSplat64.dll, so it loads at process startup
# before WinMain — giving us the earliest possible hook point.
# Uses MinHook for function hooking (built from extern/minhook)

set(PATCHES_SOURCES
    "dllmain.cpp"
    "initialize.cpp"
    "boot_log_tee.cpp"
    "cli.cpp"
    "boot.cpp"
    "config.cpp"
    "mode_patches.cpp"
    "crash_recovery.cpp"
    "state_machine.cpp"
    "plugin_loader.cpp"
    "module_loader.cpp"
    "resource_override.cpp"
    "winhttp_stub.cpp"
    "ws_bridge.cpp"
    "token_auth.cpp"
    "xpid_patch.cpp"
    "server_timing.cpp"
    "wave0_instrumentation.cpp"
    "pnsrad_enabler.cpp"
    "builtin_log_filter.cpp"
    "broadcaster_guard.cpp"
    "dll_load_hook.cpp"
    "asset_cdn.cpp"
    "headless_graphics.cpp"
    "symbol_corpus.cpp"
    "gameserver/gameserver.cpp"
    "gameserver/messages.cpp"
    "gameserver/server_context.cpp"
    "gameserver/websocket_client.cpp"
    "gameserver/telemetry_streamer.cpp"
    "gameserver/upnp.cpp"
)

set(PATCHES_HEADERS
    "patches.h"
    "gamepatches_internal.h"
    "initialize.h"
    "boot_log_tee.h"
    "cli.h"
    "boot.h"
    "config.h"
    "mode_patches.h"
    "crash_recovery.h"
    "platform_compat.h"
    "resource_override.h"
    "state_machine.h"
    "plugin_loader.h"
    "module_loader.h"
    "winhttp_stub.h"
    "asset_cdn.h"
    "process_mem.h"
    "patch_addresses.h"
    "ws_bridge.h"
    "xpid_patch.h"
    "server_timing.h"
    "wave0_instrumentation.h"
    "pnsrad_enabler.h"
    "builtin_log_filter.h"
    "broadcaster_guard.h"
    "dll_load_hook.h"
    "headless_graphics.h"
    "symbol_corpus.h"
    "gameserver/gameserver.h"
    "gameserver/server_context.h"
    "gameserver/websocket_client.h"
    "gameserver/telemetry_streamer.h"
    "gameserver/messages.h"
    "gameserver/upnp.h"
    "gameserver/constants.h"
    "gameserver/event_ring_buffer.h"
    "gameserver/telemetry_snapshot.h"
)

add_library(gamepatches SHARED ${PATCHES_SOURCES} ${PATCHES_HEADERS}
                               "exports.def")

target_compile_definitions(gamepatches PRIVATE USE_MINHOOK PROTOBUF_STATIC_LIB)

target_precompile_headers(gamepatches PRIVATE "${CMAKE_SOURCE_DIR}/src/common/pch.h")
target_include_directories(gamepatches PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/gameserver
  ${CMAKE_SOURCE_DIR}/src/common
  ${CMAKE_SOURCE_DIR}/plugins/common/include
  ${CMAKE_BINARY_DIR}
)

find_package(CURL REQUIRED)

find_package(ixwebsocket CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(miniupnpc CONFIG REQUIRED)

target_link_libraries(gamepatches PRIVATE minhook common version CURL::libcurl ixwebsocket::ixwebsocket nlohmann_json::nlohmann_json miniupnpc::miniupnpc nevr_api ws2_32)
target_link_directories(gamepatches PRIVATE common)

# Disable PCH for files using ixwebsocket/nlohmann-json — headers conflict with Windows.h macros
set_source_files_properties(ws_bridge.cpp token_auth.cpp server_timing.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
set_source_files_properties(gameserver/websocket_client.cpp gameserver/telemetry_streamer.cpp gameserver/upnp.cpp gameserver/gameserver.cpp gameserver/messages.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

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

if(MINGW)
  add_library(dbghelp_stubs_gp STATIC gameserver/dbghelp_stubs.cpp)
  target_compile_definitions(dbghelp_stubs_gp PRIVATE PROTOBUF_STATIC_LIB)
  set_target_properties(dbghelp_stubs_gp PROPERTIES
    OUTPUT_NAME "dbghelp"
    PREFIX "lib"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/stubs"
  )
  target_link_directories(gamepatches BEFORE PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/stubs")
  add_dependencies(gamepatches dbghelp_stubs_gp)
endif()

if(MINGW)
  target_link_options(gamepatches PRIVATE -Wl,--allow-multiple-definition)
endif()

# XPID patch unit tests — validates byte constants and address sanity.
if(BUILD_TESTING)
  find_package(GTest QUIET)
  if(GTest_FOUND)
    include(GoogleTest)
    add_executable(test_xpid_patch tests/test_xpid_patch.cpp)
    target_include_directories(test_xpid_patch PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${CMAKE_SOURCE_DIR}/src/common
    )
    target_link_libraries(test_xpid_patch PRIVATE GTest::gtest GTest::gtest_main)
    # DISCOVERY_MODE PRE_TEST: defer test discovery to ctest-invocation time instead
    # of running the cross-compiled .exe as a POST_BUILD step. The default POST_BUILD
    # discovery executes the Windows binary on the Linux build host under Wine with a
    # 5s timeout, which the Wine cold-start exceeds — failing the build even though the
    # tests themselves pass. PRE_TEST keeps `cmake --build` from launching Wine.
    gtest_discover_tests(test_xpid_patch DISCOVERY_MODE PRE_TEST)

    # ParseEndpoint unit tests — validates N1 fix (strtoul + errno vs std::stoul throw)
    add_executable(test_parse_endpoint tests/test_parse_endpoint.cpp)
    target_include_directories(test_parse_endpoint PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(test_parse_endpoint PRIVATE GTest::gtest GTest::gtest_main ws2_32)
    gtest_discover_tests(test_parse_endpoint DISCOVERY_MODE PRE_TEST)
  endif()
endif()
