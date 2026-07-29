# Game Patches — deployed as BugSplat64.dll (replaces the original crash reporter).
# The game statically imports BugSplat64.dll, so it loads at process startup
# before WinMain — giving us the earliest possible hook point.
# Uses MinHook for function hooking (built from extern/minhook)

set(PATCHES_SOURCES
    "lifecycle/dllmain.cpp"
    "lifecycle/initialize.cpp"
    "log/boot_log_tee.cpp"
    "lifecycle/cli.cpp"
    "lifecycle/boot.cpp"
    "lifecycle/config.cpp"
    "patch/mode_patches.cpp"
    "lifecycle/crash_recovery.cpp"
    "hook/hook_guard.cpp"
    "hook/hook_liveness.cpp"
    "lifecycle/state_machine.cpp"
    "ext/plugin_loader.cpp"
    "ext/module_loader.cpp"
    "patch/resource_override.cpp"
    "compat/winhttp_stub.cpp"
    "compat/ws_bridge.cpp"
    "patch/xpid_patch.cpp"
    "frame/tick.cpp"
    "patch/binary_bug_fixes.cpp"
    "patch/pnsrad_enabler.cpp"
    "log/builtin_filter.cpp"
    "patch/broadcaster_guard.cpp"
    "hook/dll_load_hook.cpp"
    "patch/asset_cdn.cpp"
    "patch/headless_graphics.cpp"
    "hook/symbol_corpus.cpp"
    "server/gameserver.cpp"
    "server/messages.cpp"
    "server/server_context.cpp"
    "server/websocket_client.cpp"
    "server/telemetry_streamer.cpp"
    "server/upnp.cpp"
)

set(PATCHES_HEADERS
    "hook/patching.h"
    "lifecycle/initialize.h"
    "log/boot_log_tee.h"
    "lifecycle/cli.h"
    "lifecycle/boot.h"
    "lifecycle/config.h"
    "patch/mode_patches.h"
    "lifecycle/crash_recovery.h"
    "patch/resource_override.h"
    "lifecycle/state_machine.h"
    "ext/plugin_loader.h"
    "ext/module_loader.h"
    "compat/winhttp_stub.h"
    "patch/asset_cdn.h"
    "hook/process_memory.h"
    "hook/addresses.h"
    "compat/ws_bridge.h"
    "patch/xpid_patch.h"
    "frame/tick.h"
    "patch/binary_bug_fixes.h"
    "patch/pnsrad_enabler.h"
    "log/builtin_filter.h"
    "patch/broadcaster_guard.h"
    "hook/dll_load_hook.h"
    "patch/headless_graphics.h"
    "hook/symbol_corpus.h"
    "server/gameserver.h"
    "server/server_context.h"
    "server/websocket_client.h"
    "server/telemetry_streamer.h"
    "server/messages.h"
    "server/upnp.h"
    "server/constants.h"
    "server/event_ring_buffer.h"
    "server/telemetry_snapshot.h"
)

add_library(nevr_runtime SHARED ${PATCHES_SOURCES} ${PATCHES_HEADERS}
                               "exports.def")

target_compile_definitions(nevr_runtime PRIVATE USE_MINHOOK PROTOBUF_STATIC_LIB)

target_precompile_headers(nevr_runtime PRIVATE "${CMAKE_SOURCE_DIR}/src/core/pch.h")
# NOTE: neither ${CMAKE_CURRENT_SOURCE_DIR} nor any subdirectory of it is on
# this list, and that is the point. Those two entries are what let 44 files sit
# flat in one directory and include each other by bare basename, which is how
# the tree got this way. Every include is now path-qualified from src/ —
# "runtime/hook/addresses.h" — so the spelling states which area it reaches
# into. Re-adding a directory here silently re-enables the flat namespace.
target_include_directories(nevr_runtime PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/plugins/common/include
  ${CMAKE_BINARY_DIR}
)

find_package(CURL REQUIRED)

find_package(ixwebsocket CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(miniupnpc CONFIG REQUIRED)

target_link_libraries(nevr_runtime PRIVATE minhook nevr_core version CURL::libcurl ixwebsocket::ixwebsocket nlohmann_json::nlohmann_json miniupnpc::miniupnpc nevr_api ws2_32)

# Disable PCH for files using ixwebsocket/nlohmann-json — headers conflict with Windows.h macros
set_source_files_properties(ws_bridge.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
set_source_files_properties(server/websocket_client.cpp server/telemetry_streamer.cpp server/upnp.cpp server/gameserver.cpp server/messages.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

set_target_properties(nevr_runtime PROPERTIES
  OUTPUT_NAME "BugSplat64"
  PREFIX ""
)

# Disable specific warnings (MSVC only)
if(MSVC)
  target_compile_options(nevr_runtime PRIVATE /wd4251)
endif()

# ixwebsocket include path workaround: vcpkg adds include/ixwebsocket but
# headers use #include <ixwebsocket/...>, so we need the parent directory.
get_target_property(_ixws_includes ixwebsocket::ixwebsocket INTERFACE_INCLUDE_DIRECTORIES)
foreach(_dir ${_ixws_includes})
  get_filename_component(_parent_dir "${_dir}" DIRECTORY)
  target_include_directories(nevr_runtime SYSTEM PRIVATE "${_parent_dir}")
endforeach()

# MinGW: mbedTLS (ixwebsocket TLS backend) needs BCryptGenRandom from bcrypt.dll.
# Build a minimal import lib to avoid pulling in all bcrypt exports.
if(MINGW)
  add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/stubs/libbcrypt.a"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/stubs"
    COMMAND x86_64-w64-mingw32-dlltool -d "${CMAKE_CURRENT_SOURCE_DIR}/link/bcrypt_minimal.def"
            -l "${CMAKE_CURRENT_BINARY_DIR}/stubs/libbcrypt.a"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/link/bcrypt_minimal.def"
    COMMENT "Generating minimal bcrypt import library for nevr_runtime"
  )
  add_custom_target(nevr_runtime_bcrypt_minimal DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/stubs/libbcrypt.a")
  add_dependencies(nevr_runtime nevr_runtime_bcrypt_minimal)
  target_link_libraries(nevr_runtime PRIVATE
    $<LINK_LIBRARY:WHOLE_ARCHIVE,${CMAKE_CURRENT_BINARY_DIR}/stubs/libbcrypt.a>
  )
endif()

if(MINGW)
  add_library(dbghelp_stubs_gp STATIC link/dbghelp_stubs.cpp)
  target_compile_definitions(dbghelp_stubs_gp PRIVATE PROTOBUF_STATIC_LIB)
  set_target_properties(dbghelp_stubs_gp PROPERTIES
    OUTPUT_NAME "dbghelp"
    PREFIX "lib"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/stubs"
  )
  target_link_directories(nevr_runtime BEFORE PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/stubs")
  add_dependencies(nevr_runtime dbghelp_stubs_gp)
endif()

if(MINGW)
  target_link_options(nevr_runtime PRIVATE -Wl,--allow-multiple-definition)
endif()

# XPID patch unit tests — validates byte constants and address sanity.
if(BUILD_TESTING)
  find_package(GTest QUIET)
  if(GTest_FOUND)
    include(GoogleTest)
    add_executable(test_xpid_patch tests/test_xpid_patch.cpp)
    target_include_directories(test_xpid_patch PRIVATE
      ${CMAKE_SOURCE_DIR}/src
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
    )
    target_link_libraries(test_parse_endpoint PRIVATE GTest::gtest GTest::gtest_main ws2_32)
    gtest_discover_tests(test_parse_endpoint DISCOVERY_MODE PRE_TEST)

    # Behavioral tests — N68 (plugin/module tick dispatch) + N61 (callback
    # lifecycle). Links plugin_loader.cpp, module_loader.cpp, and ws_bridge.cpp
    # compiled with NEVR_TEST_HOOKS to enable mock-callback injection into the
    # static registries. The test file provides stubs for game globals (Log,
    # FatalError, EchoVR function pointers).
    #
    # ws_bridge.cpp is linked for N61: the test drives the REAL Close handler
    # through test hooks, using real ix::WebSocket objects (never connected).
    # This is the N68 pattern applied to ws_bridge — the production code IS
    # the code under test.
    add_executable(test_behavioral
      tests/test_behavioral.cpp
      # ws_bridge.cpp calls SystemInfo::Get(). Linked as a source rather than
      # via nevr_core, because this test deliberately does NOT link core — it
      # provides its own stubs for Log/FatalError and linking the real ones
      # would collide. system_info.cpp depends on nothing but Win32, so it
      # drops in cleanly and the test drives the REAL measurement code.
      "${CMAKE_SOURCE_DIR}/src/core/system_info.cpp"
      ext/plugin_loader.cpp
      ext/module_loader.cpp
      compat/ws_bridge.cpp
      hook/symbol_corpus.cpp
      # N84: plugin_loader calls HookGuard::VerifyAll after each plugin init.
      # Linked as production source (not a stub) so the test drives the real
      # guard — a stub here would make the N84 tests tautological.
      hook/hook_guard.cpp
    )
    target_compile_definitions(test_behavioral PRIVATE NEVR_TEST_HOOKS)
    # Disable PCH — the test has its own stub headers and must not depend on
    # the nevr_runtime precompiled header (which pulls in MinHook, etc.).
    set_source_files_properties(
      "${CMAKE_SOURCE_DIR}/src/core/system_info.cpp"
      tests/test_behavioral.cpp
      ext/plugin_loader.cpp
      ext/module_loader.cpp
      compat/ws_bridge.cpp
      hook/symbol_corpus.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON
    )
    target_include_directories(test_behavioral PRIVATE
      ${CMAKE_SOURCE_DIR}/src
      ${CMAKE_SOURCE_DIR}/plugins/common/include
      ${CMAKE_BINARY_DIR}
    )
    # ws_bridge.cpp uses ixwebsocket and nlohmann-json. The test creates
    # real ix::WebSocket objects (never connected) as opaque handles.
    find_package(ixwebsocket CONFIG REQUIRED)
    find_package(nlohmann_json CONFIG REQUIRED)
    # ixwebsocket include workaround — same as the main nevr_runtime target.
    get_target_property(_ixws_includes_test ixwebsocket::ixwebsocket INTERFACE_INCLUDE_DIRECTORIES)
    foreach(_dir ${_ixws_includes_test})
      get_filename_component(_parent_dir "${_dir}" DIRECTORY)
      target_include_directories(test_behavioral SYSTEM PRIVATE "${_parent_dir}")
    endforeach()
    # Use the same minimal bcrypt import lib as the main nevr_runtime target.
    # mbedTLS (ixwebsocket TLS backend) needs BCryptGenRandom.
    if(MINGW)
      add_dependencies(test_behavioral nevr_runtime_bcrypt_minimal)
      target_link_libraries(test_behavioral PRIVATE
        $<LINK_LIBRARY:WHOLE_ARCHIVE,${CMAKE_CURRENT_BINARY_DIR}/stubs/libbcrypt.a>
      )
    endif()
    target_link_libraries(test_behavioral PRIVATE
      GTest::gtest GTest::gtest_main
      ixwebsocket::ixwebsocket nlohmann_json::nlohmann_json
      ws2_32
    )
    gtest_discover_tests(test_behavioral DISCOVERY_MODE PRE_TEST)
  endif()
endif()
