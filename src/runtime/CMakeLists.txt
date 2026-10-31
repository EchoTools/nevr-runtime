# Two DLLs (docs/adr/0008-bugsplat-host-and-nevr-runtime.md, docs/adr/0012-static-install-root.md):
#
#   nevr_host     BugSplat64.dll   the thin host that replaces the original crash reporter. The game statically
#                                  imports it, so it loads at process startup before WinMain. It installs the one
#                                  detour on the game's main wrapper, loads nevr-runtime.dll there, and keeps the
#                                  crash capture. Sources: host/, plus the two lifecycle units it needs.
#   nevr_runtime  nevr-runtime.dll everything else: the hooks, the plugin loader, the bridge, the game server.
#
# Uses MinHook for function hooking (built from extern/minhook)

# The units the host DLL and the host's test library compile. They carry the host's behaviour; none reads the
# runtime's globals or calls the NEVR logger. NEVR_HOST_TESTING is defined by nevr_host_core alone
# (tools/verify_host_seams.py).
set(NEVR_HOST_UNITS
    "host/bootstrap_diagnostics.cpp"
    "host/crash_capture.cpp"
    "host/crash_report_host.cpp"
    "host/crash_send_host.cpp"
    "host/crash_send_winhttp.cpp"
    "host/host_entry.cpp"
    "host/host_identity.cpp"
    "host/minidmp_sender_exports.cpp"
    "host/runtime_exports.cpp"
    "host/runtime_load.cpp"
    "host/runtime_rendezvous.cpp"
    "host/stop_line.cpp"
    # The signature verifier and the pins it checks against (ADR 0012: the host verifies nevr-runtime.dll before it loads it).
    "ext/plugin_pins.cpp"
    "ext/plugin_signature.cpp"
    "lifecycle/game_image_guard.cpp"
    # The host's own crash report (#1689): the shared report writer (core/crash_report.cpp; the host links no nevr_core) and the
    # PC pieces the runtime's report uses too (compiled into both DLLs, as game_image_guard is).
    "../core/crash_report.cpp"
    "lifecycle/crash_report_pe.cpp"
    "lifecycle/crash_report_win_common.cpp"
)

set(PATCHES_SOURCES
    "lifecycle/dllmain.cpp"
    "lifecycle/initialize.cpp"
    # Compiled into both DLLs (the host checks the game image at attach, the runtime again before it initializes).
    "lifecycle/game_image_guard.cpp"
    "lifecycle/runtime_entry.cpp"
    "lifecycle/runtime_exports.cpp"
    "log/boot_log_tee.cpp"
    "lifecycle/cli.cpp"
    "lifecycle/boot.cpp"
    "lifecycle/config.cpp"
    "lifecycle/arena_round_time.cpp"
    "lifecycle/arena_rules_hook.cpp"
    "lifecycle/service_map.cpp"
    "lifecycle/service_redirect.cpp"
    "lifecycle/stable_string_pool.cpp"
    "lifecycle/service_config.cpp"
    "lifecycle/nevr_root_move.cpp"
    "lifecycle/cli_reject.cpp"
    "lifecycle/config_yaml_source.cpp"
    "lifecycle/login_redirect_override.cpp"
    "patch/mode_patches.cpp"
    "patch/loading_tips.cpp"
    "patch/coop_ai_trace.cpp"
    "lifecycle/crash_recovery.cpp"
    "lifecycle/return_to_lobby.cpp"
    "lifecycle/crash_dump_format.cpp"
    "lifecycle/crash_first_filter.cpp"
    "lifecycle/crash_first_win.cpp"
    "lifecycle/crash_report_pe.cpp"
    "lifecycle/crash_report_win.cpp"
    "lifecycle/crash_report_win_common.cpp"
    "hook/hook_guard.cpp"
    "hook/hook_liveness.cpp"
    "hook/export_trace_thunk.cpp"
    "hook/export_tracer.cpp"
    "lifecycle/state_machine.cpp"
    "ext/plugin_loader.cpp"
    "ext/plugin_load_plan.cpp"
    "ext/plugin_manifest.cpp"
    "ext/plugin_signature.cpp"
    "ext/plugin_pins.cpp"
    "ext/plugin_tree.cpp"
    "ext/plugin_trust.cpp"
    "ext/self_provenance.cpp"
    "ext/module_loader.cpp"
    "patch/resource_override.cpp"
    "patch/asset_cache.cpp"
    # #938: the nEVR section of the in-game CREDITS page (pure logic in credits_text.h).
    "patch/credits_text.cpp"
    # #1423: AI bots on a dedicated server (pure logic in server_bots.h).
    "patch/server_bots.cpp"
    # #1422: a server's match team size (pure logic in server_team_size.h).
    "patch/server_team_size.cpp"
    "patch/asset_registry.cpp"
    "patch/plugin_assets.cpp"
    "compat/ws_bridge.cpp"
    # #708: the session router Quest ships and the ixwebsocket adapters ws_bridge.cpp hands game sockets to.
    "compat/session_router.cpp"
    "compat/ws_session_transport.cpp"
    "compat/login_profile.cpp"
    "compat/evr_codec.cpp"
    "compat/self_check.cpp"
    "compat/social_names.cpp"
    "patch/xpid_patch.cpp"
    "frame/frame_stats.cpp"
    "frame/tick.cpp"
    "frame/window_watch.cpp"
    "patch/binary_bug_fixes.cpp"
    "patch/pnsrad_enabler.cpp"
    "patch/mic_provider.cpp"
    "patch/party_invite_gate.cpp"
    "patch/invite_choice_hook.cpp"
    "patch/invite_wizard_hook.cpp"
    "patch/invite_transport_curl.cpp"
    "patch/invite_service.cpp"
    "patch/invite_answer_apply.cpp"
    "patch/invite_join_game.cpp"
    # #1139, docs/adr/0022: the render configuration as one log line (pure model, and the two observer hooks).
    "patch/render_config.cpp"
    "patch/render_config_hook.cpp"
    # #1419, docs/adr/0022: the GPU score the auto-selector reads, filled when the game's own rating is zero (pure rule).
    "patch/gpu_score.cpp"
    "patch/early_quit_lockout.cpp"
    "patch/arena_public_clock.cpp"
    "patch/lobby_full_backoff.cpp"
    "patch/menu_version_label.cpp"
    # #906, ADR 0017 D4: the main-menu cards (pure logic in menu_page.h and menu_cards.h).
    "patch/menu_cards.cpp"
    # #1420: the magnum, smg, Chain and rifle bullets get particle effects (pure logic in netgun_bullets.h).
    "patch/netgun_bullets.cpp"
    # #1420: the netgun bullet ids of the four unfinished weapons (pure logic in netgun_bullet_ids.h).
    "patch/netgun_bullet_ids.cpp"
    "patch/splash_skip.cpp"
    # #896, #906: the UI text writer (pure logic in ui_text.h).
    "patch/ui_text.cpp"
    "patch/ui_texture_mips.cpp"
    "patch/social_facade.cpp"
    "patch/social_facade_object.cpp"
    "log/builtin_filter.cpp"
    "log/symcache.cpp"
    "log/symcache_data.cpp"
    "log/url_diagnostics.cpp"
    "patch/broadcaster_guard.cpp"
    "hook/dll_load_hook.cpp"
    "patch/asset_cdn.cpp"
    "patch/evrp_package.cpp"
    "patch/headless_graphics.cpp"
    "hook/symbol_corpus.cpp"
    "server/gameserver.cpp"
    "server/gameserver_callbacks.cpp"
    "server/gameserver_telemetry.cpp"
    "server/gameserver_serverdb.cpp"
    "server/registration_envelope.cpp"
    "server/registration_metadata.cpp"
    "server/installed_levels.cpp"
    "server/telemetry_game_type.cpp"
    "server/game_type_table.h"
    "server/messages.cpp"
    "server/lobby_settings_relay.cpp"
    "server/server_context.cpp"
    "server/websocket_client.cpp"
    "server/websocket_frame.cpp"
    "server/protobuf_transport.cpp"
    "server/session_success_dispatch.cpp"
    "server/callback_unregistration.cpp"
    "server/session_unregister.cpp"
    "server/main_thread_handoff.cpp"
    "server/serverdb_uri.cpp"
    "server/telemetry_streamer.cpp"
    "server/bearer_reconnect_auth.cpp"
    "server/upnp.cpp"
)

set(PATCHES_HEADERS
    "hook/patching.h"
    "lifecycle/initialize.h"
    "lifecycle/game_image_guard.h"
    "lifecycle/runtime_exports.h"
    "lifecycle/export_guard.h"
    "log/boot_lines.h"
    "log/boot_log_tee.h"
    "log/boot_replay.h"
    "lifecycle/cli.h"
    "lifecycle/boot.h"
    "lifecycle/config.h"
    "lifecycle/config_redirect_result.h"
    "lifecycle/service_map.h"
    "lifecycle/service_redirect.h"
    "lifecycle/stable_string_pool.h"
    "lifecycle/service_config.h"
    "lifecycle/login_redirect_override.h"
    "patch/mode_patches.h"
    "patch/loading_tips.h"
    "patch/coop_ai_trace.h"
    "patch/coop_ai_trace_rules.h"
    "patch/arena_public_clock.h"
    "patch/arena_public_clock_rules.h"
    "patch/render_config.h"
    "patch/render_config_hook.h"
    "patch/gpu_score.h"
    "lifecycle/crash_recovery.h"
    "lifecycle/return_to_lobby.h"
    "lifecycle/return_to_lobby_hold.h"
    "lifecycle/server_timestep.h"
    "lifecycle/cmdline_strip.h"
    "lifecycle/timestep_log.h"
    "lifecycle/server_bringup.h"
    "lifecycle/crash_dump_format.h"
    "lifecycle/crash_first_filter.h"
    "lifecycle/crash_first_win.h"
    "lifecycle/readable_memory.h"
    "lifecycle/console_ctrl_policy.h"
    "lifecycle/veh_policy.h"
    "lifecycle/boot_hook_policy.h"
    "lifecycle/crash_recovery_sites.h"
    "patch/resource_override.h"
    "patch/server_bots.h"
    "patch/server_bots_install.h"
    "patch/server_team_size.h"
    "patch/server_team_size_install.h"
    "patch/asset_cache.h"
    "patch/asset_registry.h"
    "patch/plugin_assets.h"
    "patch/splash_texture.h"
    "lifecycle/state_machine.h"
    "ext/plugin_loader.h"
    "ext/plugin_load_plan.h"
    "ext/plugin_load_plan_build.h"
    "ext/plugin_manifest.h"
    "ext/module_loader.h"
    "patch/asset_cdn.h"
    "patch/evrp_package.h"
    "hook/process_memory.h"
    "hook/addresses.h"
    "compat/ws_bridge.h"
    "compat/socket_role.h"
    "patch/invite_wizard.h"
    "patch/invite_worker.h"
    "patch/invite_answer_apply.h"
    "patch/invite_join.h"
    "patch/invite_join_game.h"
    "patch/invite_answer_text.h"
    "patch/invite_transport.h"
    "patch/invite_transport_curl.h"
    "patch/invite_service.h"
    "patch/invite_config.h"
    "patch/menu_page.h"
    "patch/menu_slots.h"
    "patch/menu_cards.h"
    "patch/netgun_bullets.h"
    "patch/netgun_bullet_ids.h"
    "patch/guild_picker.h"
    "compat/evr_codec.h"
    "compat/lobby_frame.h"
    "compat/lobby_join_state.h"
    "patch/party_follow_dedup_rules.h"
    "patch/xpid_patch.h"
    "frame/frame_stats.h"
    "frame/tick.h"
    "patch/binary_bug_fixes.h"
    "patch/pnsrad_enabler.h"
    "patch/pnsrad_load_queue.h"
    "patch/mic_provider.h"
    "patch/party_invite_gate.h"
    "patch/social_facade.h"
    "patch/social_facade_install.h"
    "log/builtin_filter.h"
    "log/bridge_line.h"
    "log/rate_limiter.h"
    "log/symcache.h"
    "log/url_diagnostics.h"
    "log/security_diagnostics.h"
    "patch/broadcaster_guard.h"
    "hook/dll_load_hook.h"
    "hook/export_trace_policy.h"
    "hook/export_trace_thunk.h"
    "hook/export_tracer.h"
    "patch/headless_graphics.h"
    "hook/symbol_corpus.h"
    "server/gameserver.h"
    "server/gameserver_internal.h"
    "server/registration_envelope.h"
    "server/server_context.h"
    "server/websocket_client.h"
    "server/websocket_frame.h"
    "server/protobuf_transport.h"
    "server/session_success_dispatch.h"
    "server/callback_unregistration.h"
    "server/session_unregister.h"
    "server/main_thread_handoff.h"
    "server/serverdb_uri.h"
    "server/telemetry_streamer.h"
    "server/bearer_reconnect_auth.h"
    "server/serialized_mint.h"
    "server/messages.h"
    "server/lobby_settings_relay.h"
    "server/upnp.h"
    "server/constants.h"
    "server/event_ring_buffer.h"
    "server/telemetry_snapshot.h"
)

add_library(nevr_runtime SHARED ${PATCHES_SOURCES} ${PATCHES_HEADERS})

target_compile_definitions(nevr_runtime PRIVATE NEVR_USE_MINHOOK PROTOBUF_STATIC_LIB)

target_precompile_headers(nevr_runtime PRIVATE "${CMAKE_SOURCE_DIR}/src/core/pch.h")
# NOTE: neither ${CMAKE_CURRENT_SOURCE_DIR} nor any subdirectory of it is on
# this list, and that is the point. Those two entries are what let 44 files sit
# flat in one directory and include each other by bare basename, which is how
# the tree got this way. Every include is now path-qualified from src/ —
# "runtime/hook/addresses.h" — so the spelling states which area it reaches
# into. Re-adding a directory here silently re-enables the flat namespace.
target_include_directories(nevr_runtime PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_BINARY_DIR}
)

# #938: the project section of the in-game CREDITS page is rendered from config/credits.json at build time and
# compiled in (patch/credits_block.h declares it). A name the page cannot show, or a file that does not
# validate, fails this step and so the build; nothing is cut or dropped at run time.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(NEVR_CREDITS_BLOCK_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/credits_block.cpp")
add_custom_command(
  OUTPUT "${NEVR_CREDITS_BLOCK_CPP}"
  COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/render_credits.py"
          --ingame-source "${NEVR_CREDITS_BLOCK_CPP}" --json "${CMAKE_SOURCE_DIR}/config/credits.json"
  DEPENDS "${CMAKE_SOURCE_DIR}/config/credits.json" "${CMAKE_SOURCE_DIR}/tools/render_credits.py"
  COMMENT "Rendering the in-game credits block from config/credits.json"
  VERBATIM)
target_sources(nevr_runtime PRIVATE "${NEVR_CREDITS_BLOCK_CPP}")
set_source_files_properties("${NEVR_CREDITS_BLOCK_CPP}" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

find_package(CURL REQUIRED)

find_package(ixwebsocket CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(miniupnpc CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)  # the friend-name lookup decodes the profile reply (compat/social_names.cpp)

find_package(ZLIB REQUIRED)  # server/lobby_settings_relay.cpp inflates the game service's lobby settings frame (#905)
target_link_libraries(nevr_runtime PRIVATE minhook nevr_core version zstd::libzstd ZLIB::ZLIB CURL::libcurl ixwebsocket::ixwebsocket nlohmann_json::nlohmann_json miniupnpc::miniupnpc nevr_api ws2_32 nevr_platform_compat nevr_token_auth ole32 crypt32 advapi32)

# yaml-cpp backs src/core/nevr_config (compiled into nevr_core as an archive with
# unresolved yaml symbols). The DLL is where those symbols get resolved — this is
# the ONLY production link of yaml-cpp. The module DLLs never reference nevr_config,
# so yaml-cpp does not reach them.
target_link_libraries(nevr_runtime PRIVATE yaml-cpp::yaml-cpp)

# The two files here that read the version (cmake/nevr_build_identity.cmake); a property of this directory, so every
# target below that compiles them (test_behavioral compiles ws_bridge.cpp) gets it too.
nevr_build_identity_define(TARGET nevr_runtime READS VERSION
  SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/lifecycle/initialize.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/compat/ws_bridge.cpp")
# The host states the same version and commit as the runtime in a literal of its own (host/host_identity.cpp):
# tools/package_release.py refuses a pair that disagrees.
nevr_build_identity_define(TARGET nevr_runtime READS VERSION COMMIT_FULL
  SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/host/host_identity.cpp")

# The host units use no precompiled header: the host links no nevr_core, and nevr_host_core and nevr_host build them alike.
set_source_files_properties(${NEVR_HOST_UNITS} host/host_dllmain.cpp host/runtime_exports.cpp lifecycle/system_module_loader.cpp
                            PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
# Disable PCH for files using ixwebsocket/nlohmann-json — headers conflict with Windows.h macros
set_source_files_properties(compat/ws_bridge.cpp compat/session_router.cpp compat/ws_session_transport.cpp compat/social_names.cpp compat/evr_codec.cpp compat/self_check.cpp patch/asset_cache.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
# #20: the export tracer's thunk layer is windows.h plus a global assembly block; the tracer logs through core.
set_source_files_properties(hook/export_trace_thunk.cpp hook/export_tracer.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
# N133 S3: service_map.cpp (pure, yaml-cpp-free but must stay PCH-agnostic so the
# test can link it) and service_config.cpp (defines NOMINMAX before windows.h, as
# the core PCH pulls windows.h WITHOUT NOMINMAX and its min/max macros break
# <optional>/<set>). Same reason nevr_config.cpp skips the PCH.
set_source_files_properties(
  lifecycle/service_map.cpp
  lifecycle/service_redirect.cpp
  lifecycle/stable_string_pool.cpp
  lifecycle/service_config.cpp
  lifecycle/nevr_root_move.cpp
  lifecycle/cli_reject.cpp
  lifecycle/config_yaml_source.cpp
  lifecycle/login_redirect_override.cpp
  PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
# N134 S6: plugin_load_plan.cpp is the pure builder — nevr_config + nlohmann-json,
# no windows.h. Same reason as service_map.cpp: it must stay PCH-agnostic (defines
# NOMINMAX before nlohmann, which pulls <limits>) so the test links it standalone.
set_source_files_properties(ext/plugin_load_plan.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
# The PE reader of the crash report is bytes only (no windows.h), so its test builds it without the PCH too.
set_source_files_properties(lifecycle/crash_report_pe.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
# #60: plugin_manifest.cpp builds the login's nevr_plugins array with nlohmann-json;
# pure and PCH-free for the same reason as plugin_load_plan.cpp.
set_source_files_properties(ext/plugin_manifest.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
# #432: asset_registry.cpp builds the login's nevr_assets array with nlohmann-json; same arrangement.
set_source_files_properties(patch/asset_registry.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
set_source_files_properties(log/url_diagnostics.cpp server/bearer_reconnect_auth.cpp server/websocket_client.cpp server/websocket_frame.cpp server/protobuf_transport.cpp server/session_success_dispatch.cpp server/callback_unregistration.cpp server/session_unregister.cpp server/main_thread_handoff.cpp server/serverdb_uri.cpp server/telemetry_streamer.cpp server/upnp.cpp server/gameserver.cpp server/gameserver_callbacks.cpp server/gameserver_telemetry.cpp server/gameserver_serverdb.cpp server/registration_envelope.cpp server/messages.cpp server/lobby_settings_relay.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

# ws_bridge.cpp has no C-style casts (CPP-MINGW-ADDENDUM hard stop); keep it that way at compile time.
# The property is directory-scoped, so the test target that compiles the same file gets it too.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set_source_files_properties(compat/ws_bridge.cpp PROPERTIES COMPILE_OPTIONS "-Werror=old-style-cast")
endif()

# The signature gate (ADR 0010, #741, #924). Every build checks plugin signatures the same way: a run loads unsigned
# plugins unless it is started with -require-signed-plugins (src/runtime/ext/plugin_trust.cpp). This option marks the
# build as the plugin developers' build: the DLL embeds the NUL-bounded literal "NEVR-PLUGIN-TRUST dev" instead of
# "NEVR-PLUGIN-TRUST release", and tools/package_release.py refuses to package any DLL that does not say release.
# It does not change how a run checks plugins. No config key and no environment variable changes the mode of a run.
# It is also the build whose unsigned nevr-runtime.dll the host loads (ADR 0012), so every preset that makes a test DLL pair
# (mingw-plugin-dev, mingw-scenario) sets it; tools/tests/test_preset_unsigned_runtime.py pins that.
option(NEVR_ALLOW_UNSIGNED_PLUGINS "Mark the build as the plugin developers' build (never ship)" OFF)
if(NEVR_ALLOW_UNSIGNED_PLUGINS)
  set_source_files_properties(ext/plugin_trust.cpp ext/plugin_pins.cpp PROPERTIES COMPILE_DEFINITIONS NEVR_ALLOW_UNSIGNED_PLUGINS=1)
  # The host carries no NEVR-PLUGIN-TRUST literal, so it says the same in a literal of its own (host/host_identity.cpp).
  set_property(SOURCE host/host_identity.cpp APPEND PROPERTY COMPILE_DEFINITIONS NEVR_ALLOW_UNSIGNED_PLUGINS=1)
  message(STATUS "NEVR_ALLOW_UNSIGNED_PLUGINS=ON: BugSplat64.dll is marked as a plugin developer build (never ship)")
endif()
set_source_files_properties(ext/plugin_signature.cpp ext/plugin_tree.cpp ext/plugin_trust.cpp ext/plugin_pins.cpp ext/self_provenance.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

# Scenario-test control endpoint (docs/design/2026-10-01-social-scenario-harness.md). It can inject
# messages into a live session, so it is OFF by default and only the mingw-scenario preset turns it
# on; `just verify` checks the release DLL carries none of its strings.
option(NEVR_SCENARIO_CONTROL "Compile the scenario-test control endpoint into nevr-runtime.dll (test builds only)" OFF)
if(NEVR_SCENARIO_CONTROL)
  target_sources(nevr_runtime PRIVATE scenario/scenario_control.cpp)
  target_compile_definitions(nevr_runtime PRIVATE NEVR_SCENARIO_CONTROL=1)
  set_source_files_properties(scenario/scenario_control.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
  message(STATUS "NEVR_SCENARIO_CONTROL=ON: nevr-runtime.dll carries the scenario control endpoint (test build)")
endif()

set_target_properties(nevr_runtime PROPERTIES
  OUTPUT_NAME "nevr-runtime"
  PREFIX ""
)

# The Details tab of nevr-runtime.dll and BugSplat64.dll (cmake/nevr_version_info.cmake; tools/pe_version_info.py
# reads it back).
nevr_add_version_info(nevr_runtime
  FILE_DESCRIPTION "nEVR runtime for Echo VR"
  ORIGINAL_FILENAME "nevr-runtime.dll"
  TYPE DLL)

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
    # dlltool names the archive's import-name symbol after the -l argument as given, so an absolute
    # path would put the build directory into the DLL: run it in the output directory with a bare name.
    COMMAND x86_64-w64-mingw32-dlltool -d "${CMAKE_CURRENT_SOURCE_DIR}/link/bcrypt_minimal.def" -l libbcrypt.a
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/stubs"
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
  add_library(nevr_dbghelp_stubs STATIC link/dbghelp_stubs.cpp)
  target_compile_definitions(nevr_dbghelp_stubs PRIVATE PROTOBUF_STATIC_LIB)
  set_target_properties(nevr_dbghelp_stubs PROPERTIES
    OUTPUT_NAME "dbghelp"
    PREFIX "lib"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/stubs"
  )
  target_link_directories(nevr_runtime BEFORE PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/stubs")
  add_dependencies(nevr_runtime nevr_dbghelp_stubs)
endif()

if(MINGW)
  # N146 applies to the host (below), which keeps the stock BugSplat64.dll's image base. The runtime has a base of
  # its own, inside the range the one-file DLL occupied, so the loader never has to choose between the two
  # (tools/verify_pe_contract.py RUNTIME_IMAGE_BASE).
  target_link_options(nevr_runtime PRIVATE
    -Wl,--image-base=0x181000000
    -Wl,--allow-multiple-definition
  )
endif()

# ---------------------------------------------------------------------------------------------------------------
# The host: BugSplat64.dll (docs/adr/0008-bugsplat-host-and-nevr-runtime.md, docs/adr/0012-static-install-root.md).
#
# Only what must keep working when nevr-runtime.dll does not, and that rarely changes: the game-facing exports, the
# one detour on the game's main wrapper, the runtime loader, and the crash capture. It links MinHook and
# nlohmann_json and nothing else of the project: no nevr_core, no sockets, no TLS, no plugin loader
# (tools/verify_pe_contract.py pins its imports). It is built WITHOUT NEVR_HOST_TESTING.
# ---------------------------------------------------------------------------------------------------------------
add_library(nevr_host SHARED
  ${NEVR_HOST_UNITS}
  host/host_dllmain.cpp
  lifecycle/system_module_loader.cpp
  host/exports.def)
target_include_directories(nevr_host PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(nevr_host PRIVATE minhook nlohmann_json::nlohmann_json crypt32 advapi32)
set_target_properties(nevr_host PROPERTIES
  OUTPUT_NAME "BugSplat64"
  PREFIX ""
)
nevr_add_version_info(nevr_host
  FILE_DESCRIPTION "nEVR host for Echo VR"
  ORIGINAL_FILENAME "BugSplat64.dll"
  TYPE DLL)
if(MINGW)
  # N146: the stock BugSplat64.dll is linked with /BASE:0x180000000.
  # The game's D3D11/DXVK graphics DLLs fail to load when BugSplat64.dll
  # occupies a different address range.  Setting the same ImageBase fixes
  # the client-mode window-not-rendering bug.
  target_link_options(nevr_host PRIVATE -Wl,--image-base=0x180000000)
endif()
# The runtime is loaded from the host's directory at the rendezvous, never linked: build it with the host so a
# build tree always holds the matched pair.
add_dependencies(nevr_host nevr_runtime)

# XPID patch unit tests — validates byte constants and address sanity.
if(BUILD_TESTING)
  find_package(GTest QUIET)
  if(GTest_FOUND)
    include(GoogleTest)
    add_executable(test_game_image_guard
      tests/test_game_image_guard.cpp
      lifecycle/game_image_guard.cpp)
    set_source_files_properties(tests/test_game_image_guard.cpp lifecycle/game_image_guard.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_game_image_guard PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_game_image_guard PRIVATE GTest::gtest)
    gtest_discover_tests(test_game_image_guard DISCOVERY_MODE PRE_TEST)

    # The host <-> runtime descriptor (core/runtime_abi.h): header-only validators that BugSplat64.dll
    # runs on nevr-runtime.dll's descriptor, and nevr-runtime.dll on the host's context.
    add_executable(test_runtime_abi tests/test_runtime_abi.cpp)
    set_source_files_properties(tests/test_runtime_abi.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_runtime_abi PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_runtime_abi PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_runtime_abi DISCOVERY_MODE PRE_TEST)

    # The guard behind every export entry of nevr-runtime.dll (lifecycle/export_guard.h): header-only.
    add_executable(test_runtime_export_guard tests/test_runtime_export_guard.cpp)
    set_source_files_properties(tests/test_runtime_export_guard.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_compile_options(test_runtime_export_guard PRIVATE -Werror)
    target_include_directories(test_runtime_export_guard PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_runtime_export_guard PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_runtime_export_guard DISCOVERY_MODE PRE_TEST)

    # The code the stable BugSplat64.dll host runs behind its DllMain and its exports
    # (runtime/host/, docs/adr/0008-bugsplat-host-and-nevr-runtime.md): the diagnostic
    # recorder, the minidump capture, the runtime load/validate state machine and the
    # GameMainWrapper rendezvous. It is a static library of its own so the tests
    # drive the same units nevr_host links into BugSplat64.dll, not a copy of them (the same source files, built
    # here with the test seams).
    find_package(nlohmann_json CONFIG REQUIRED)
    add_library(nevr_host_core STATIC ${NEVR_HOST_UNITS})
    target_compile_options(nevr_host_core PRIVATE -Werror)
    target_include_directories(nevr_host_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
    # The host units carry test seams (RuntimeRendezvous::Seams, SetEmitLoadFailureForTest, the emit
    # sink) that exist only under NEVR_HOST_TESTING. This library is built for the tests alone; the
    # target that builds the host into BugSplat64.dll (nevr_host) must not define it
    # (tools/tests/test_host_test_seams.py).
    target_compile_definitions(nevr_host_core PUBLIC NEVR_HOST_TESTING)
    target_link_libraries(nevr_host_core PUBLIC minhook nlohmann_json::nlohmann_json crypt32 advapi32)

    # Attach-time MinHook proof gate for the BugSplat64.dll / nevr-runtime.dll split
    # (docs/adr/0008-bugsplat-host-and-nevr-runtime.md, "Proof gate").
    # The host (the production host units behind a shim, built as BugSplat64.dll) validates the game image and the
    # real GameMainWrapper prologue and installs ONE MinHook detour from
    # DLL_PROCESS_ATTACH; it loads the versioned runtime (core/runtime_abi.h) on
    # the first rendezvous call. test_bootstrap_attach.exe drives twelve
    # dynamic-load child scenarios; test_bootstrap_attach_static.exe imports the
    # host statically, as echovr.exe does, so attach runs before main.
    set(BOOTSTRAP_ATTACH_DIR "${CMAKE_BINARY_DIR}/bin/bootstrap-attach")
    set(BOOTSTRAP_ATTACH_FIXTURE_DIR "${BOOTSTRAP_ATTACH_DIR}/fixtures")

    function(bootstrap_attach_output target directory)
      set_target_properties(${target} PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${directory}"
        RUNTIME_OUTPUT_DIRECTORY "${directory}"
        LIBRARY_OUTPUT_DIRECTORY "${directory}")
      target_compile_options(${target} PRIVATE -Werror)
      target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    endfunction()

    # The host of the gate is the production host units (nevr_host_core) behind a shim DllMain.
    add_library(test_bootstrap_attach_host SHARED tests/bootstrap_attach/host_shim.cpp)
    target_link_libraries(test_bootstrap_attach_host PRIVATE nevr_host_core)
    set_target_properties(test_bootstrap_attach_host PROPERTIES
      PREFIX "" OUTPUT_NAME "BugSplat64")
    bootstrap_attach_output(test_bootstrap_attach_host "${BOOTSTRAP_ATTACH_DIR}")

    # One nevr-runtime.dll per fixture directory; the variant is a compile definition.
    function(add_bootstrap_attach_fixture target fixture_dir)
      add_library(${target} SHARED tests/bootstrap_attach/runtime_probe.cpp)
      target_link_libraries(${target} PRIVATE minhook)
      set_target_properties(${target} PROPERTIES PREFIX "" OUTPUT_NAME "nevr-runtime")
      bootstrap_attach_output(${target} "${BOOTSTRAP_ATTACH_FIXTURE_DIR}/${fixture_dir}")
      if(ARGN)
        target_compile_definitions(${target} PRIVATE ${ARGN})
      endif()
    endfunction()

    add_bootstrap_attach_fixture(test_bootstrap_attach_runtime_good good)
    add_bootstrap_attach_fixture(test_bootstrap_attach_runtime_bad_version bad-version
      BOOTSTRAP_PROBE_BAD_VERSION)
    add_bootstrap_attach_fixture(test_bootstrap_attach_runtime_init_failure init-failure
      BOOTSTRAP_PROBE_INIT_FAILURE)
    add_bootstrap_attach_fixture(test_bootstrap_attach_runtime_reenter_init reenter-init
      BOOTSTRAP_PROBE_REENTER_INIT)
    add_bootstrap_attach_fixture(test_bootstrap_attach_runtime_slow_init slow-init
      BOOTSTRAP_PROBE_SLOW_INIT)

    add_library(test_bootstrap_attach_runtime_missing_entry SHARED
      tests/bootstrap_attach/runtime_missing_entry.cpp)
    set_target_properties(test_bootstrap_attach_runtime_missing_entry PROPERTIES
      PREFIX "" OUTPUT_NAME "nevr-runtime")
    bootstrap_attach_output(test_bootstrap_attach_runtime_missing_entry
      "${BOOTSTRAP_ATTACH_FIXTURE_DIR}/missing-entry")

    set(BOOTSTRAP_BAD_IMAGE
      "${BOOTSTRAP_ATTACH_FIXTURE_DIR}/bad-image/nevr-runtime.dll")
    add_custom_command(OUTPUT "${BOOTSTRAP_BAD_IMAGE}"
      COMMAND ${CMAKE_COMMAND} -E make_directory
        "${BOOTSTRAP_ATTACH_FIXTURE_DIR}/bad-image"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/bootstrap_attach/runtime_bad_image.txt"
        "${BOOTSTRAP_BAD_IMAGE}"
      DEPENDS tests/bootstrap_attach/runtime_bad_image.txt
      VERBATIM)
    add_custom_target(test_bootstrap_attach_bad_image ALL
      DEPENDS "${BOOTSTRAP_BAD_IMAGE}")

    add_executable(test_bootstrap_attach_static tests/bootstrap_attach/attach_test.cpp)
    target_compile_definitions(test_bootstrap_attach_static PRIVATE BOOTSTRAP_PROBE_STATIC_IMPORT)
    target_link_libraries(test_bootstrap_attach_static PRIVATE test_bootstrap_attach_host
      nlohmann_json::nlohmann_json)
    bootstrap_attach_output(test_bootstrap_attach_static "${BOOTSTRAP_ATTACH_DIR}")

    add_executable(test_bootstrap_attach tests/bootstrap_attach/attach_test.cpp)
    target_link_libraries(test_bootstrap_attach PRIVATE nlohmann_json::nlohmann_json)
    bootstrap_attach_output(test_bootstrap_attach "${BOOTSTRAP_ATTACH_DIR}")
    add_dependencies(test_bootstrap_attach
      test_bootstrap_attach_host
      test_bootstrap_attach_static
      test_bootstrap_attach_runtime_good
      test_bootstrap_attach_runtime_bad_version
      test_bootstrap_attach_runtime_init_failure
      test_bootstrap_attach_runtime_reenter_init
      test_bootstrap_attach_runtime_slow_init
      test_bootstrap_attach_runtime_missing_entry
      test_bootstrap_attach_bad_image)

    add_executable(test_host_json tests/test_host_json.cpp)
    set_source_files_properties(tests/test_host_json.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_compile_options(test_host_json PRIVATE -Werror)
    target_link_libraries(test_host_json PRIVATE nevr_host_core GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_host_json DISCOVERY_MODE PRE_TEST)

    # The one console line the host writes when it stops the game or leaves it running without the runtime (#1252).
    add_executable(test_host_stop_line tests/test_host_stop_line.cpp)
    target_compile_options(test_host_stop_line PRIVATE -Werror)
    target_link_libraries(test_host_stop_line PRIVATE nevr_host_core GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_host_stop_line DISCOVERY_MODE PRE_TEST)

    add_executable(test_bootstrap_diagnostics tests/test_bootstrap_diagnostics.cpp)
    target_compile_options(test_bootstrap_diagnostics PRIVATE -Werror)
    target_link_libraries(test_bootstrap_diagnostics PRIVATE nevr_host_core GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_bootstrap_diagnostics DISCOVERY_MODE PRE_TEST)

    add_executable(test_host_crash_capture tests/test_host_crash_capture.cpp)
    target_compile_options(test_host_crash_capture PRIVATE -Werror)
    target_link_libraries(test_host_crash_capture PRIVATE nevr_host_core GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_host_crash_capture DISCOVERY_MODE PRE_TEST)

    add_executable(test_host_runtime_load tests/test_host_runtime_load.cpp)
    target_compile_options(test_host_runtime_load PRIVATE -Werror)
    target_link_libraries(test_host_runtime_load PRIVATE nevr_host_core GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_host_runtime_load DISCOVERY_MODE PRE_TEST)

    add_executable(test_host_rendezvous tests/test_host_rendezvous.cpp)
    target_compile_options(test_host_rendezvous PRIVATE -Werror)
    target_link_libraries(test_host_rendezvous PRIVATE nevr_host_core GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_host_rendezvous DISCOVERY_MODE PRE_TEST)

    # The host's verdict on nevr-runtime.dll (ADR 0012 step 3, #854): the decision table and the detour over real
    # staged files, signed by tools/gen_plugin_signature_fixtures.py (NEVR_SIGFIX_DIR; the rendezvous runtime
    # stand-in is the file it signs).
    add_executable(test_host_runtime_verify tests/test_host_runtime_verify.cpp)
    target_compile_options(test_host_runtime_verify PRIVATE -Werror)
    target_link_libraries(test_host_runtime_verify PRIVATE nevr_host_core GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_host_runtime_verify DISCOVERY_MODE PRE_TEST)

    # The host's sixteen export thunks: the shipped file (host/runtime_exports.cpp) is one of the host units the
    # library holds, and the test pulls it from there.
    add_executable(test_host_exports tests/test_host_exports.cpp)
    set_source_files_properties(tests/test_host_exports.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_compile_options(test_host_exports PRIVATE -Werror)
    target_link_libraries(test_host_exports PRIVATE nevr_host_core GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_host_exports DISCOVERY_MODE PRE_TEST)

    add_executable(test_host_minidump tests/test_host_minidump.cpp)
    target_compile_options(test_host_minidump PRIVATE -Werror)
    target_include_directories(test_host_minidump PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_host_minidump PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_host_minidump DISCOVERY_MODE PRE_TEST)

    # Crash harness: a BugSplat64.dll shim (the production host objects behind the real
    # mangled createReport export, plus runtime-load test seams) and child scenarios that
    # fault through it.
    set(HOST_CRASH_DIR "${CMAKE_BINARY_DIR}/bin/host-crash")
    set(HOST_CRASH_FIXTURE_DIR "${HOST_CRASH_DIR}/fixtures")

    function(host_crash_output target directory)
      set_target_properties(${target} PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${directory}"
        RUNTIME_OUTPUT_DIRECTORY "${directory}"
        LIBRARY_OUTPUT_DIRECTORY "${directory}")
      target_compile_options(${target} PRIVATE -Werror)
      target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    endfunction()

    add_library(test_host_crash_shim SHARED
      tests/host_crash/shim.cpp
      "tests/host_crash/shim.def")
    target_link_libraries(test_host_crash_shim PRIVATE nevr_host_core)
    # BugSplat_createReport is defined in nevr_host_core (host/minidmp_sender_exports.cpp, the shipped file) and
    # only named by shim.def, which does not make the linker take an archive member: ask for it.
    target_link_options(test_host_crash_shim PRIVATE -Wl,--undefined=BugSplat_createReport)
    set_target_properties(test_host_crash_shim PROPERTIES PREFIX "" OUTPUT_NAME "BugSplat64")
    host_crash_output(test_host_crash_shim "${HOST_CRASH_DIR}")

    function(add_host_crash_fixture target fixture_dir)
      add_library(${target} SHARED tests/host_crash/runtime_fixture.cpp)
      set_target_properties(${target} PROPERTIES PREFIX "" OUTPUT_NAME "nevr-runtime")
      host_crash_output(${target} "${HOST_CRASH_FIXTURE_DIR}/${fixture_dir}")
      if(ARGN)
        target_compile_definitions(${target} PRIVATE ${ARGN})
      endif()
    endfunction()

    add_host_crash_fixture(test_host_crash_runtime_good good)
    add_host_crash_fixture(test_host_crash_runtime_bad_version bad-version
      HOST_CRASH_BAD_VERSION)
    add_host_crash_fixture(test_host_crash_runtime_init_failure init-failure
      HOST_CRASH_INIT_FAILURE)

    add_library(test_host_crash_runtime_missing_entry SHARED
      tests/host_crash/runtime_missing_entry.cpp)
    set_target_properties(test_host_crash_runtime_missing_entry PROPERTIES
      PREFIX "" OUTPUT_NAME "nevr-runtime")
    host_crash_output(test_host_crash_runtime_missing_entry
      "${HOST_CRASH_FIXTURE_DIR}/missing-entry")

    add_executable(test_host_crash tests/host_crash/host_crash_test.cpp)
    target_link_libraries(test_host_crash PRIVATE nlohmann_json::nlohmann_json)
    host_crash_output(test_host_crash "${HOST_CRASH_DIR}")
    add_dependencies(test_host_crash
      test_host_crash_shim
      test_host_crash_runtime_good
      test_host_crash_runtime_bad_version
      test_host_crash_runtime_init_failure
      test_host_crash_runtime_missing_entry)

    # Early GameMainWrapper rendezvous harness for the BugSplat64.dll / nevr-runtime.dll
    # split (docs/adr/0008-bugsplat-host-and-nevr-runtime.md, "Decision: the rendezvous is
    # the game's main wrapper"). The host shim (BugSplat64.dll) calls the production host
    # entry points: one detour from DLL_PROCESS_ATTACH, then the sibling nevr-runtime.dll loads on
    # the first call. test_rendezvous.exe drives valid / missing / reenter / bad-prologue
    # child scenarios plus the NEVR_SetGameModule external contract.
    set(RENDEZVOUS_DIR "${CMAKE_BINARY_DIR}/bin/rendezvous")
    set(RENDEZVOUS_FIXTURE_DIR "${RENDEZVOUS_DIR}/fixtures")

    function(rendezvous_output target directory)
      set_target_properties(${target} PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${directory}"
        RUNTIME_OUTPUT_DIRECTORY "${directory}"
        LIBRARY_OUTPUT_DIRECTORY "${directory}")
      target_compile_options(${target} PRIVATE -Werror)
      target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    endfunction()

    add_library(test_rendezvous_host SHARED tests/rendezvous/host_shim.cpp)
    target_link_libraries(test_rendezvous_host PRIVATE nevr_host_core)
    set_target_properties(test_rendezvous_host PROPERTIES PREFIX "" OUTPUT_NAME "BugSplat64")
    rendezvous_output(test_rendezvous_host "${RENDEZVOUS_DIR}")

    # One nevr-runtime.dll per fixture directory; the variant is a compile definition.
    function(add_rendezvous_fixture target fixture_dir)
      add_library(${target} SHARED tests/rendezvous/runtime_fixture.cpp)
      set_target_properties(${target} PROPERTIES PREFIX "" OUTPUT_NAME "nevr-runtime")
      rendezvous_output(${target} "${RENDEZVOUS_FIXTURE_DIR}/${fixture_dir}")
      if(ARGN)
        target_compile_definitions(${target} PRIVATE ${ARGN})
      endif()
    endfunction()

    add_rendezvous_fixture(test_rendezvous_runtime_good good)
    add_rendezvous_fixture(test_rendezvous_runtime_reenter reenter-init
      RENDEZVOUS_PROBE_REENTER_INIT)

    add_executable(test_rendezvous tests/rendezvous/rendezvous_test.cpp)
    target_link_libraries(test_rendezvous PRIVATE nlohmann_json::nlohmann_json)
    rendezvous_output(test_rendezvous "${RENDEZVOUS_DIR}")
    add_dependencies(test_rendezvous
      test_rendezvous_host
      test_rendezvous_runtime_good
      test_rendezvous_runtime_reenter)

    # #20: the export tracer. The thunk is assembly, so this test runs it (under Wine in test-auth-unit):
    # forwarded register, stack and float arguments, results, nesting, exceptions, threads.
    add_executable(test_export_trace
      tests/test_export_trace.cpp
      hook/export_trace_thunk.cpp
      hook/export_tracer.cpp)
    set_source_files_properties(tests/test_export_trace.cpp hook/export_trace_thunk.cpp hook/export_tracer.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_export_trace PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_export_trace PRIVATE GTest::gtest GTest::gtest_main)
    # A stand-in module the test loads: named pnsrad.dll (the tracer identifies a platform DLL by file name),
    # with a code export and a data export, in its own directory next to the test.
    add_library(nevr_export_trace_fixture MODULE tests/export_trace_fixture.cpp)
    set_target_properties(nevr_export_trace_fixture PROPERTIES PREFIX "" OUTPUT_NAME "pnsrad"
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/export-trace-fixture"
      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/export-trace-fixture")
    set_source_files_properties(tests/export_trace_fixture.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    add_dependencies(test_export_trace nevr_export_trace_fixture)
    gtest_discover_tests(test_export_trace DISCOVERY_MODE PRE_TEST)

    add_executable(test_system_module_loader
      tests/test_system_module_loader.cpp
      lifecycle/system_module_loader.cpp)
    set_source_files_properties(tests/test_system_module_loader.cpp lifecycle/system_module_loader.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_system_module_loader PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_system_module_loader PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_system_module_loader DISCOVERY_MODE PRE_TEST)

    add_executable(test_login_redirect_override
      tests/test_login_redirect_override.cpp
      lifecycle/login_redirect_override.cpp)
    set_source_files_properties(tests/test_login_redirect_override.cpp lifecycle/login_redirect_override.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_login_redirect_override PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_login_redirect_override PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_login_redirect_override DISCOVERY_MODE PRE_TEST)

    add_executable(test_websocket_frame
      tests/test_websocket_frame.cpp
      server/websocket_frame.cpp)
    set_source_files_properties(tests/test_websocket_frame.cpp server/websocket_frame.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_websocket_frame PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_websocket_frame PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_websocket_frame DISCOVERY_MODE PRE_TEST)

    # test_registration_roundtrip.cpp (#46): the registration against a local fake ServerDB.
    add_executable(test_protobuf_transport
      tests/test_protobuf_transport.cpp
      tests/test_registration_roundtrip.cpp
      server/registration_envelope.cpp
      server/protobuf_transport.cpp
      server/websocket_frame.cpp
      log/url_diagnostics.cpp
      server/websocket_client.cpp)
    set_source_files_properties(tests/test_protobuf_transport.cpp tests/test_registration_roundtrip.cpp server/registration_envelope.cpp server/installed_levels.cpp server/protobuf_transport.cpp log/url_diagnostics.cpp server/websocket_client.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_compile_definitions(test_protobuf_transport PRIVATE NEVR_TEST_HOOKS)
    target_include_directories(test_protobuf_transport PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_protobuf_transport PRIVATE GTest::gtest GTest::gtest_main nevr_api ixwebsocket::ixwebsocket CURL::libcurl ws2_32)
    if(MINGW)
      add_dependencies(test_protobuf_transport nevr_runtime_bcrypt_minimal)
      target_link_libraries(test_protobuf_transport PRIVATE
        $<LINK_LIBRARY:WHOLE_ARCHIVE,${CMAKE_CURRENT_BINARY_DIR}/stubs/libbcrypt.a>)
    endif()
    gtest_discover_tests(test_protobuf_transport DISCOVERY_MODE PRE_TEST)

    # Issue #39: the bearer header presented on ixwebsocket's automatic reconnect,
    # driven against a scripted loopback upgrade server.
    # #114: BearerReconnectAuth (the telemetry socket's equivalent) rides in this executable.
    add_executable(test_websocket_client_auth
      tests/test_websocket_client_auth.cpp
      server/bearer_reconnect_auth.cpp
      server/websocket_client.cpp
      server/websocket_frame.cpp
      log/url_diagnostics.cpp)
    set_source_files_properties(tests/test_websocket_client_auth.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_compile_definitions(test_websocket_client_auth PRIVATE NEVR_TEST_HOOKS)
    target_include_directories(test_websocket_client_auth PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_websocket_client_auth PRIVATE
      GTest::gtest GTest::gtest_main ixwebsocket::ixwebsocket CURL::libcurl ws2_32)
    if(MINGW)
      add_dependencies(test_websocket_client_auth nevr_runtime_bcrypt_minimal)
      target_link_libraries(test_websocket_client_auth PRIVATE
        $<LINK_LIBRARY:WHOLE_ARCHIVE,${CMAKE_CURRENT_BINARY_DIR}/stubs/libbcrypt.a>)
    endif()
    gtest_discover_tests(test_websocket_client_auth DISCOVERY_MODE PRE_TEST)

    # test_gpu_hung_notice.cpp (#553, header-only policy) rides in this executable, so the existing
    # url-diagnostics loop in `just test-auth-unit` runs it with no recipe change.
    # test_game_data_notice.cpp (#960, header-only policy) rides here too.
    add_executable(test_url_diagnostics
      tests/test_url_diagnostics.cpp
      tests/test_gpu_hung_notice.cpp
      tests/test_game_data_notice.cpp
      log/url_diagnostics.cpp)
    set_source_files_properties(tests/test_url_diagnostics.cpp tests/test_gpu_hung_notice.cpp
      tests/test_game_data_notice.cpp log/url_diagnostics.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_url_diagnostics PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_url_diagnostics PRIVATE GTest::gtest GTest::gtest_main CURL::libcurl)
    gtest_discover_tests(test_url_diagnostics DISCOVERY_MODE PRE_TEST)

    # test_main_thread_handoff.cpp (GH #44) rides in this executable, so the
    # existing `just test-auth-unit` loop runs it with no recipe change.

    # Issue #41: the ServerDB URI builder (percent-encoded query) — pure, curl only.
    # test_failure_detail.cpp (#35, header-only failure_detail.h) rides in this executable.
    # test_boot_replay.cpp (#5, header-only log/boot_lines.h and log/boot_replay.h) rides in this executable.
    add_executable(test_serverdb_uri
      tests/test_serverdb_uri.cpp
      tests/test_failure_detail.cpp
      tests/test_boot_replay.cpp
      tests/test_login_session.cpp
      server/serverdb_uri.cpp
      log/url_diagnostics.cpp)
    set_source_files_properties(tests/test_serverdb_uri.cpp tests/test_failure_detail.cpp tests/test_boot_replay.cpp tests/test_login_session.cpp server/serverdb_uri.cpp log/url_diagnostics.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_serverdb_uri PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_serverdb_uri PRIVATE GTest::gtest GTest::gtest_main CURL::libcurl)
    gtest_discover_tests(test_serverdb_uri DISCOVERY_MODE PRE_TEST)

    # #990: the installed levels read from the package manifests (installed_levels.cpp: std and zstd only).
    add_executable(test_installed_levels
      tests/test_installed_levels.cpp
      server/installed_levels.cpp
      server/registration_envelope.cpp
      patch/asset_registry.cpp)
    set_source_files_properties(tests/test_installed_levels.cpp server/installed_levels.cpp server/registration_envelope.cpp
      patch/asset_registry.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_installed_levels PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_installed_levels PRIVATE nevr::plugin_sdk GTest::gtest GTest::gtest_main zstd::libzstd
      nlohmann_json::nlohmann_json nevr_api)
    gtest_discover_tests(test_installed_levels DISCOVERY_MODE PRE_TEST)

    # #1246: the server launcher's run-log selection and console line (launcher/server_log.h, header-only) and the run ID format
    # it shares with the runtime (core/run_id.h). The launcher's own window is not driven here: this is the pure part.
    add_executable(test_server_log tests/test_server_log.cpp)
    set_source_files_properties(tests/test_server_log.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_server_log PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_server_log PRIVATE GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json)
    gtest_discover_tests(test_server_log DISCOVERY_MODE PRE_TEST)

    # #990, #996: the capture header's game type is the settings JSON's gametype, else the lobby type (telemetry_game_type.cpp).
    add_executable(test_telemetry_game_type tests/test_telemetry_game_type.cpp server/telemetry_game_type.cpp)
    set_source_files_properties(tests/test_telemetry_game_type.cpp server/telemetry_game_type.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_telemetry_game_type PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_telemetry_game_type PRIVATE GTest::gtest GTest::gtest_main nevr_api
      nevr::plugin_sdk nlohmann_json::nlohmann_json)
    gtest_discover_tests(test_telemetry_game_type DISCOVERY_MODE PRE_TEST)

    # #982: the public Arena round default applies only to a stored public Arena session (arena_round_time.cpp).
    add_executable(test_arena_round_time tests/test_arena_round_time.cpp lifecycle/arena_round_time.cpp)
    set_source_files_properties(tests/test_arena_round_time.cpp lifecycle/arena_round_time.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_compile_definitions(test_arena_round_time PRIVATE NEVR_ARENA_ROUND_TIME_TESTING)
    target_include_directories(test_arena_round_time PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_arena_round_time PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_arena_round_time DISCOVERY_MODE PRE_TEST)

    # #982: the CJson_GetFloat detour answers round_time with 480 s for a public Arena session, whatever the order of
    # the game's reads and the session's messages (lifecycle/arena_rules_hook.cpp).
    add_executable(test_arena_rules_hook tests/test_arena_rules_hook.cpp lifecycle/arena_rules_hook.cpp
      lifecycle/arena_round_time.cpp)
    set_source_files_properties(tests/test_arena_rules_hook.cpp lifecycle/arena_rules_hook.cpp
      lifecycle/arena_round_time.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_compile_definitions(test_arena_rules_hook PRIVATE NEVR_ARENA_ROUND_TIME_TESTING)
    target_include_directories(test_arena_rules_hook PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_arena_rules_hook PRIVATE GTest::gtest GTest::gtest_main nevr_core
      nlohmann_json::nlohmann_json nevr::plugin_sdk)
    gtest_discover_tests(test_arena_rules_hook DISCOVERY_MODE PRE_TEST)

    # #1423: AI bots on a dedicated server: the settings rewrite, the request, the schedule and the sites (server_bots.h).
    add_executable(test_server_bots tests/test_server_bots.cpp)
    set_source_files_properties(tests/test_server_bots.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_server_bots PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_server_bots PRIVATE GTest::gtest GTest::gtest_main nevr::plugin_sdk
      nlohmann_json::nlohmann_json)
    gtest_discover_tests(test_server_bots DISCOVERY_MODE PRE_TEST)
    # #1422: a server's match team size, its two checked-patch sites driven on buffers (server_team_size.h).
    add_executable(test_server_team_size tests/test_server_team_size.cpp)
    set_source_files_properties(tests/test_server_team_size.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_server_team_size PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_server_team_size PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_server_team_size DISCOVERY_MODE PRE_TEST)

    # #982: the public Arena match clock keeps counting through goals (patch/arena_public_clock_rules.h).
    add_executable(test_arena_public_clock tests/test_arena_public_clock.cpp lifecycle/arena_round_time.cpp)
    set_source_files_properties(tests/test_arena_public_clock.cpp lifecycle/arena_round_time.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_arena_public_clock PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_arena_public_clock PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_arena_public_clock DISCOVERY_MODE PRE_TEST)

    # #982: the hook on the real stock Arena script DLLs under MinHook (needs NEVR_SCRIPT_DLL_DIR; skips without it).
    add_executable(test_arena_public_clock_hook tests/test_arena_public_clock_hook.cpp patch/arena_public_clock.cpp
      lifecycle/arena_round_time.cpp hook/dll_load_hook.cpp hook/hook_guard.cpp log/boot_log_tee.cpp)
    set_source_files_properties(tests/test_arena_public_clock_hook.cpp patch/arena_public_clock.cpp
      lifecycle/arena_round_time.cpp hook/dll_load_hook.cpp hook/hook_guard.cpp log/boot_log_tee.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_compile_definitions(test_arena_public_clock_hook PRIVATE NEVR_USE_MINHOOK NEVR_DLL_LOAD_HOOK_TESTING
      NEVR_TEST_HOOKS NEVR_ARENA_PUBLIC_CLOCK_TESTING)
    target_include_directories(test_arena_public_clock_hook PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_arena_public_clock_hook PRIVATE GTest::gtest GTest::gtest_main minhook nevr_core)
    gtest_discover_tests(test_arena_public_clock_hook DISCOVERY_MODE PRE_TEST)

    # #1139: the render_config model against a fake game image (patch/render_config.h; no Windows call).
    add_executable(test_render_config tests/test_render_config.cpp patch/render_config.cpp)
    set_source_files_properties(tests/test_render_config.cpp patch/render_config.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_render_config PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_render_config PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_render_config DISCOVERY_MODE PRE_TEST)

    # #1419: the GPU score rule (patch/gpu_score.h; no Windows call).
    add_executable(test_gpu_score tests/test_gpu_score.cpp patch/gpu_score.cpp)
    set_source_files_properties(tests/test_gpu_score.cpp patch/gpu_score.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_gpu_score PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_gpu_score PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_gpu_score DISCOVERY_MODE PRE_TEST)

    # #1139: the two observer hooks and Poll under MinHook, against a fake image with the real prologues at their RVAs.
    add_executable(test_render_config_hook tests/test_render_config_hook.cpp patch/render_config.cpp
      patch/render_config_hook.cpp patch/gpu_score.cpp hook/hook_guard.cpp)
    set_source_files_properties(tests/test_render_config_hook.cpp patch/render_config.cpp
      patch/render_config_hook.cpp patch/gpu_score.cpp hook/hook_guard.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_compile_definitions(test_render_config_hook PRIVATE NEVR_USE_MINHOOK NEVR_TEST_HOOKS
      NEVR_RENDER_CONFIG_TESTING)
    target_include_directories(test_render_config_hook PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_render_config_hook PRIVATE GTest::gtest GTest::gtest_main minhook nevr_core)
    gtest_discover_tests(test_render_config_hook DISCOVERY_MODE PRE_TEST)

    add_executable(test_callback_unregistration
      tests/test_callback_unregistration.cpp
      tests/test_main_thread_handoff.cpp
      server/callback_unregistration.cpp
      server/main_thread_handoff.cpp
      server/server_context.cpp)
    set_source_files_properties(tests/test_callback_unregistration.cpp tests/test_main_thread_handoff.cpp
      server/callback_unregistration.cpp server/main_thread_handoff.cpp server/server_context.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_callback_unregistration PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_callback_unregistration PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_callback_unregistration DISCOVERY_MODE PRE_TEST)

    # Issue #38: entrant accessors read the lobby's live entrant array.
    add_executable(test_server_context
      tests/test_server_context.cpp
      server/server_context.cpp)
    set_source_files_properties(tests/test_server_context.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_server_context PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_server_context PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_server_context DISCOVERY_MODE PRE_TEST)

    add_executable(test_session_unregister
      tests/test_session_unregister.cpp
      server/session_unregister.cpp
      server/server_context.cpp
      server/protobuf_transport.cpp
      server/websocket_client.cpp
      server/websocket_frame.cpp
      log/url_diagnostics.cpp)
    set_source_files_properties(tests/test_session_unregister.cpp server/session_unregister.cpp server/server_context.cpp
      server/protobuf_transport.cpp log/url_diagnostics.cpp server/websocket_client.cpp server/websocket_frame.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_compile_definitions(test_session_unregister PRIVATE NEVR_TEST_HOOKS)
    target_include_directories(test_session_unregister PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_session_unregister PRIVATE
      GTest::gtest GTest::gtest_main nevr_api ixwebsocket::ixwebsocket CURL::libcurl ws2_32)
    if(MINGW)
      add_dependencies(test_session_unregister nevr_runtime_bcrypt_minimal)
      target_link_libraries(test_session_unregister PRIVATE
        $<LINK_LIBRARY:WHOLE_ARCHIVE,${CMAKE_CURRENT_BINARY_DIR}/stubs/libbcrypt.a>)
    endif()
    gtest_discover_tests(test_session_unregister DISCOVERY_MODE PRE_TEST)

    # co-op AI diagnostic (issue #63): the pure decisions behind patch/coop_ai_trace.cpp.
    add_executable(test_coop_ai_trace tests/test_coop_ai_trace.cpp)
    target_include_directories(test_coop_ai_trace PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_coop_ai_trace PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_coop_ai_trace DISCOVERY_MODE PRE_TEST)

    # #954 frame-time probe: the PURE half (frame/frame_stats.h: ring, window math, return-address predicate,
    # ovrPerfStats extraction, log lines). The hook and the log call (frame_stats.cpp) are not linked here.
    add_executable(test_frame_stats tests/test_frame_stats.cpp)
    target_include_directories(test_frame_stats PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_frame_stats PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_frame_stats DISCOVERY_MODE PRE_TEST)

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

    # HTTP session API port scan (#1511): pure logic, no socket.
    add_executable(test_http_port_scan tests/test_http_port_scan.cpp)
    target_include_directories(test_http_port_scan PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_http_port_scan PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_http_port_scan DISCOVERY_MODE PRE_TEST)

    # mic_dsp unit tests (GH#15) — the PURE ring-buffer and downmix/resample
    # logic behind the WASAPI mic provider (runtime/patch/mic_provider.cpp).
    # Compiles core/mic_dsp.cpp directly (the same TU nevr_core links): no
    # windows.h, no capture device, no Wine needed to exercise this math.
    add_executable(test_mic_dsp
      tests/test_mic_dsp.cpp
      ${CMAKE_SOURCE_DIR}/src/core/mic_dsp.cpp)
    set_source_files_properties(tests/test_mic_dsp.cpp ${CMAKE_SOURCE_DIR}/src/core/mic_dsp.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_mic_dsp PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_mic_dsp PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_mic_dsp DISCOVERY_MODE PRE_TEST)

    # test_mic_owner_thread.cpp (GH #51) rides in the same executable, so the
    # existing `just test-auth-unit` loop runs it with no recipe change.
    add_executable(test_mic_lifecycle
      tests/test_mic_lifecycle.cpp
      tests/test_mic_owner_thread.cpp
      ${CMAKE_SOURCE_DIR}/src/core/mic_lifecycle.cpp
      ${CMAKE_SOURCE_DIR}/src/core/mic_capture_drain.cpp
      ${CMAKE_SOURCE_DIR}/src/core/mic_dsp.cpp
      ${CMAKE_SOURCE_DIR}/src/core/mic_owner_thread.cpp)
    set_source_files_properties(tests/test_mic_lifecycle.cpp tests/test_mic_owner_thread.cpp
      ${CMAKE_SOURCE_DIR}/src/core/mic_lifecycle.cpp
      ${CMAKE_SOURCE_DIR}/src/core/mic_capture_drain.cpp
      ${CMAKE_SOURCE_DIR}/src/core/mic_dsp.cpp
      ${CMAKE_SOURCE_DIR}/src/core/mic_owner_thread.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_mic_lifecycle PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_mic_lifecycle PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_mic_lifecycle DISCOVERY_MODE PRE_TEST)

    add_executable(test_telemetry_snapshot_store tests/test_telemetry_snapshot_store.cpp)
    set_source_files_properties(tests/test_telemetry_snapshot_store.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_telemetry_snapshot_store PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_telemetry_snapshot_store PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_telemetry_snapshot_store DISCOVERY_MODE PRE_TEST)

    # Telemetry property keys pinned to the CSymbol64 of their names, and the getters' owner and key (#915);
    # the game's state symbols to the telemetry status enum (#921).
    add_executable(test_telemetry_game_properties tests/test_telemetry_game_properties.cpp)
    set_source_files_properties(tests/test_telemetry_game_properties.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_telemetry_game_properties PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_telemetry_game_properties PRIVATE nevr::plugin_sdk GTest::gtest GTest::gtest_main nevr_api)
    gtest_discover_tests(test_telemetry_game_properties DISCOVERY_MODE PRE_TEST)

    # ParseEndpoint unit tests — validates N1 fix (strtoul + errno vs std::stoul throw)
    add_executable(test_parse_endpoint tests/test_parse_endpoint.cpp)
    target_include_directories(test_parse_endpoint PRIVATE
    )
    target_link_libraries(test_parse_endpoint PRIVATE GTest::gtest GTest::gtest_main ws2_32)
    gtest_discover_tests(test_parse_endpoint DISCOVERY_MODE PRE_TEST)

    # nevr_config unit tests (N133 S2) — parse a sample config.yaml in isolation:
    # nested plugin args, ${VAR:?} secrets interpolation (set and unset->fail),
    # ${VAR:-default}, and unknown-top-level-key rejection. Links the real
    # nevr_config (in nevr_core) + yaml-cpp; no consumer is wired yet.
    add_executable(test_nevr_config tests/test_nevr_config.cpp)
    # The test includes only nevr_config.h (yaml-cpp-free via pImpl), so it needs
    # no yaml headers and no windows/PCH gymnastics — but skip the PCH anyway to
    # match the other test targets and keep it independent of the runtime PCH.
    set_source_files_properties(tests/test_nevr_config.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_nevr_config PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_nevr_config PRIVATE
      GTest::gtest GTest::gtest_main nevr_core yaml-cpp::yaml-cpp)
    gtest_discover_tests(test_nevr_config DISCOVERY_MODE PRE_TEST)

    # service_map behaviour-equivalence tests (N133 S3) — the PURE resolution core
    # of the config.cpp cutover (flat->path map, service-host fallback, scheme
    # redirect). Compiles service_map.cpp and service_redirect.cpp directly (the
    # same TUs nevr_runtime links)
    # + nevr_core (nevr_config) + yaml-cpp. No game stubs: service_map.cpp has no
    # windows/Log/extern dependencies. The impure half (service_config.cpp) is NOT
    # linked here — it is exercised by the differential server run.
    add_executable(test_service_map
      tests/test_service_map.cpp
      tests/test_bridge_policy.cpp
      lifecycle/service_map.cpp
      lifecycle/service_redirect.cpp)
    set_source_files_properties(
      tests/test_service_map.cpp
      tests/test_bridge_policy.cpp
      lifecycle/service_map.cpp
      lifecycle/service_redirect.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_service_map PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_service_map PRIVATE
      GTest::gtest GTest::gtest_main nevr_core yaml-cpp::yaml-cpp nlohmann_json::nlohmann_json)
    gtest_discover_tests(test_service_map DISCOVERY_MODE PRE_TEST)

    # Stable C-string storage and direct config-accessor failure-boundary tests.
    add_executable(test_service_config
      tests/test_service_config.cpp
      tests/test_stable_string_pool.cpp
      tests/test_config_yaml_choice.cpp
      lifecycle/cli_reject.cpp
      lifecycle/config_yaml_source.cpp
      lifecycle/service_config.cpp
      lifecycle/service_map.cpp
      lifecycle/service_redirect.cpp
      lifecycle/stable_string_pool.cpp
      ext/plugin_load_plan.cpp)
    target_compile_definitions(test_service_config PRIVATE NEVR_TEST_HOOKS)
    set_source_files_properties(
      tests/test_service_config.cpp
      tests/test_stable_string_pool.cpp
      tests/test_config_yaml_choice.cpp
      lifecycle/cli_reject.cpp
      lifecycle/config_yaml_source.cpp
      lifecycle/service_config.cpp
      lifecycle/service_map.cpp
      lifecycle/service_redirect.cpp
      lifecycle/stable_string_pool.cpp
      ext/plugin_load_plan.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_service_config PRIVATE
      ${CMAKE_SOURCE_DIR}/src
      ${CMAKE_BINARY_DIR})
    target_link_libraries(test_service_config PRIVATE
      GTest::gtest GTest::gtest_main nevr_core nevr_abi yaml-cpp::yaml-cpp nlohmann_json::nlohmann_json)
    gtest_discover_tests(test_service_config DISCOVERY_MODE PRE_TEST)

    add_library(test_stable_string_pool_fixture SHARED
      tests/stable_string_pool_fixture.cpp
      lifecycle/stable_string_pool.cpp)
    target_include_directories(test_stable_string_pool_fixture PRIVATE ${CMAKE_SOURCE_DIR}/src)
    set_source_files_properties(
      tests/stable_string_pool_fixture.cpp
      lifecycle/stable_string_pool.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    set_target_properties(test_stable_string_pool_fixture PROPERTIES
      PREFIX ""
      OUTPUT_NAME "test_stable_string_pool_fixture"
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

    add_library(test_stable_string_pool_destructible_control SHARED
      tests/stable_string_pool_destructible_control.cpp)
    set_target_properties(test_stable_string_pool_destructible_control PROPERTIES
      PREFIX ""
      OUTPUT_NAME "test_stable_string_pool_destructible_control"
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
    add_dependencies(test_service_config
      test_stable_string_pool_fixture
      test_stable_string_pool_destructible_control)

    # #703: the content-addressed asset cache (patch/asset_cache.cpp), driven for real against a temp directory.
    add_executable(test_asset_cache
      tests/test_asset_cache.cpp
      patch/asset_cache.cpp)
    set_source_files_properties(tests/test_asset_cache.cpp patch/asset_cache.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_asset_cache PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_asset_cache PRIVATE
      GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json advapi32)
    gtest_discover_tests(test_asset_cache DISCOVERY_MODE PRE_TEST)

    # #857: the nevr root's paths (core/nevr_root.h) and the one-time move into it (lifecycle/nevr_root_move.cpp), driven for
    # real against a temp tree.
    add_executable(test_nevr_root
      tests/test_nevr_root.cpp
      lifecycle/nevr_root_move.cpp)
    set_source_files_properties(tests/test_nevr_root.cpp lifecycle/nevr_root_move.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_compile_definitions(test_nevr_root PRIVATE NEVR_TEST_HOOKS)
    target_include_directories(test_nevr_root PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_nevr_root PRIVATE GTest::gtest GTest::gtest_main nevr_core)
    gtest_discover_tests(test_nevr_root DISCOVERY_MODE PRE_TEST)

    # EVR codec (compat/evr_codec.cpp): platform-neutral frame parse/build and login layout, the same
    # source the Quest target compiles. login_profile.cpp supplies the real profile JSON the tests parse.
    # test_lobby_frame.cpp (#929, header-only compat/lobby_frame.h on top of the codec) rides in this executable.
    add_executable(test_evr_codec
      tests/test_evr_codec.cpp
      tests/test_lobby_frame.cpp
      compat/evr_codec.cpp
      compat/login_profile.cpp)
    set_source_files_properties(tests/test_evr_codec.cpp tests/test_lobby_frame.cpp compat/evr_codec.cpp
      compat/login_profile.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_evr_codec PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_evr_codec PRIVATE nevr::plugin_sdk
      GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json)
    gtest_discover_tests(test_evr_codec DISCOVERY_MODE PRE_TEST)

    # Session router (compat/session_router.cpp, #708 S0): the source the Quest target compiles, with its host
    # test (plain main, no framework: the Quest NDK build shares the file), run under Wine here.
    add_executable(test_session_router
      ../quest/tests/session_router_test.cpp
      compat/session_router.cpp
      compat/evr_codec.cpp)
    set_source_files_properties(../quest/tests/session_router_test.cpp compat/session_router.cpp compat/evr_codec.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_session_router PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_session_router PRIVATE nevr::plugin_sdk nlohmann_json::nlohmann_json)

    # Self-check unit (compat/self_check.cpp): the registry that reports a run-card check's result as one
    # remote log string, and the SNSRemoteLogSetv3 frame builder it sends through. Platform neutral, the same
    # sources the Quest target compiles.
    add_executable(test_self_check
      tests/test_self_check.cpp
      compat/self_check.cpp
      compat/evr_codec.cpp)
    set_source_files_properties(tests/test_self_check.cpp compat/self_check.cpp compat/evr_codec.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_self_check PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_self_check PRIVATE nevr::plugin_sdk
      GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json)
    gtest_discover_tests(test_self_check DISCOVERY_MODE PRE_TEST)

    # Crash report send (core/crash_send.h, #1689): the reports a crash left in the crash folder go to nevr-api once,
    # from the BugSplat64.dll host and the Quest sentinel. Header only; the reports it reads come from the real writer
    # (core/crash_report.cpp), so the test links it.
    add_executable(test_crash_send
      tests/test_crash_send.cpp
      ${CMAKE_SOURCE_DIR}/src/core/crash_report.cpp)
    set_source_files_properties(tests/test_crash_send.cpp ${CMAKE_SOURCE_DIR}/src/core/crash_report.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_crash_send PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(test_crash_send PRIVATE -Werror)
    target_link_libraries(test_crash_send PRIVATE GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json)
    gtest_discover_tests(test_crash_send DISCOVERY_MODE PRE_TEST)

    # The host's POST (host/crash_send_winhttp.cpp): WinHTTP loaded by name, against a loopback server. nevr_host_core
    # holds the production unit, built with the host's own flags.
    add_executable(test_host_crash_send tests/test_host_crash_send.cpp)
    set_source_files_properties(tests/test_host_crash_send.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_compile_options(test_host_crash_send PRIVATE -Werror)
    target_link_libraries(test_host_crash_send PRIVATE nevr_host_core GTest::gtest GTest::gtest_main ws2_32)
    gtest_discover_tests(test_host_crash_send DISCOVERY_MODE PRE_TEST)

    # The host's own crash report (host/crash_report_host.cpp, #1689): a crash the runtime does not report leaves a report
    # the sender reads. The unit is the one BugSplat64.dll carries (nevr_host_core).
    add_executable(test_host_crash_report tests/test_host_crash_report.cpp)
    set_source_files_properties(tests/test_host_crash_report.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_compile_options(test_host_crash_report PRIVATE -Werror)
    target_link_libraries(test_host_crash_report PRIVATE nevr_host_core GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_host_crash_report DISCOVERY_MODE PRE_TEST)

    # The #398 self-check (compat/party_share_check.h): each service answer to a party data share is one
    # self-check result. Header only on top of the self-check unit and the party symbol table.
    add_executable(test_party_share_check
      tests/test_party_share_check.cpp
      compat/self_check.cpp
      compat/evr_codec.cpp)
    set_source_files_properties(tests/test_party_share_check.cpp compat/self_check.cpp compat/evr_codec.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_party_share_check PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_party_share_check PRIVATE nevr::plugin_sdk
      GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json)
    gtest_discover_tests(test_party_share_check DISCOVERY_MODE PRE_TEST)

    # The #403 self-check (compat/party_leave_check.h): each SNSPartyLeaveNotify the game client receives is one
    # self-check result. Header only on top of the self-check unit and the party symbol table.
    add_executable(test_party_leave_check
      tests/test_party_leave_check.cpp
      compat/self_check.cpp
      compat/evr_codec.cpp)
    set_source_files_properties(tests/test_party_leave_check.cpp compat/self_check.cpp compat/evr_codec.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_party_leave_check PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_party_leave_check PRIVATE nevr::plugin_sdk
      GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json)
    gtest_discover_tests(test_party_leave_check DISCOVERY_MODE PRE_TEST)

    # The #392 self-check (lifecycle/quit_check.h): the client's game loop returning is one
    # self-check result. Header only on top of the self-check unit .
    add_executable(test_quit_check
      tests/test_quit_check.cpp
      compat/self_check.cpp
      compat/evr_codec.cpp)
    set_source_files_properties(tests/test_quit_check.cpp compat/self_check.cpp compat/evr_codec.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_quit_check PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_quit_check PRIVATE nevr::plugin_sdk
      GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json)
    gtest_discover_tests(test_quit_check DISCOVERY_MODE PRE_TEST)

    # The #405 self-check (compat/friend_request_check.h): each friend request that arrives, repeats or ends is one
    # self-check result. Header only on top of the self-check unit and the party symbol table.
    add_executable(test_friend_request_check
      tests/test_friend_request_check.cpp
      compat/self_check.cpp
      compat/evr_codec.cpp)
    set_source_files_properties(tests/test_friend_request_check.cpp compat/self_check.cpp compat/evr_codec.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_friend_request_check PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_friend_request_check PRIVATE nevr::plugin_sdk
      GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json)
    gtest_discover_tests(test_friend_request_check DISCOVERY_MODE PRE_TEST)

    # The #562 self-check (compat/credential_cache_check.h): what the credential cache did in this process is one
    # self-check result. Header only on top of the self-check unit and the party symbol table.
    add_executable(test_credential_cache_check
      tests/test_credential_cache_check.cpp
      compat/self_check.cpp
      compat/evr_codec.cpp)
    set_source_files_properties(tests/test_credential_cache_check.cpp compat/self_check.cpp compat/evr_codec.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_credential_cache_check PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_credential_cache_check PRIVATE nevr::plugin_sdk
      GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json)
    gtest_discover_tests(test_credential_cache_check DISCOVERY_MODE PRE_TEST)

    # #520: which game socket is config, login or matchmaker (compat/socket_role.h): header only, pure.
    add_executable(test_socket_role tests/test_socket_role.cpp)
    set_source_files_properties(tests/test_socket_role.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_socket_role PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_socket_role PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_socket_role DISCOVERY_MODE PRE_TEST)

    # #1254: what a refused login does (compat/login_refusal.h): header only, pure.
    add_executable(test_login_refusal tests/test_login_refusal.cpp)
    set_source_files_properties(tests/test_login_refusal.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_login_refusal PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_login_refusal PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_login_refusal DISCOVERY_MODE PRE_TEST)

    # #906, docs/adr/0017: the client half of the invite codes (patch/invite_wizard.h): header only, pure.
    # It links the generated symbol cache: the map cards' level names are the cache's, and the test reads them from it.
    add_executable(test_invite_wizard tests/test_invite_wizard.cpp log/symcache_data.cpp)
    set_source_files_properties(tests/test_invite_wizard.cpp log/symcache_data.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_invite_wizard PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_invite_wizard PRIVATE nevr::plugin_sdk GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json)
    gtest_discover_tests(test_invite_wizard DISCOVERY_MODE PRE_TEST)

    # #1156, docs/adr/0017: the request worker (patch/invite_worker.h): header only, pure, a real thread over a fake transport.
    add_executable(test_invite_worker tests/test_invite_worker.cpp)
    set_source_files_properties(tests/test_invite_worker.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_invite_worker PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_invite_worker PRIVATE GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json)
    gtest_discover_tests(test_invite_worker DISCOVERY_MODE PRE_TEST)

    # #1278, docs/adr/0017: what the service's answer becomes in the wizard's text (patch/invite_answer_text.h): header only, pure.
    add_executable(test_invite_answer_text tests/test_invite_answer_text.cpp)
    set_source_files_properties(tests/test_invite_answer_text.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_invite_answer_text PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_invite_answer_text PRIVATE GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json)
    gtest_discover_tests(test_invite_answer_text DISCOVERY_MODE PRE_TEST)

    # #1401, docs/adr/0017 ("Join"): the join of a resolved code (patch/invite_join.h): header only, pure, a fake game seam.
    add_executable(test_invite_join tests/test_invite_join.cpp)
    set_source_files_properties(tests/test_invite_join.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_invite_join PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_invite_join PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_invite_join DISCOVERY_MODE PRE_TEST)

    # #1156, docs/adr/0017: the invite transport (patch/invite_transport.h, patch/invite_transport_curl.cpp): libcurl against a
    # scripted loopback server.
    add_executable(test_invite_transport tests/test_invite_transport.cpp patch/invite_transport_curl.cpp)
    set_source_files_properties(tests/test_invite_transport.cpp patch/invite_transport_curl.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_invite_transport PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_invite_transport PRIVATE GTest::gtest GTest::gtest_main CURL::libcurl ws2_32)
    gtest_discover_tests(test_invite_transport DISCOVERY_MODE PRE_TEST)

    # #1156, docs/adr/0017: the invite service (patch/invite_service.cpp): config key, address check, transport and worker.
    add_executable(test_invite_service tests/test_invite_service.cpp patch/invite_service.cpp patch/invite_transport_curl.cpp)
    set_source_files_properties(tests/test_invite_service.cpp patch/invite_service.cpp patch/invite_transport_curl.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_invite_service PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_invite_service PRIVATE GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json CURL::libcurl ws2_32)
    gtest_discover_tests(test_invite_service DISCOVERY_MODE PRE_TEST)

    # #1415, docs/adr/0017: the per-frame applier (patch/invite_answer_apply.cpp) against the real invite service; the writer,
    # the join and the log are stubs (tests/invite_apply_stubs.cpp).
    add_executable(test_invite_answer_apply tests/test_invite_answer_apply.cpp tests/invite_apply_stubs.cpp patch/invite_answer_apply.cpp patch/invite_service.cpp patch/invite_transport_curl.cpp)
    set_source_files_properties(tests/test_invite_answer_apply.cpp tests/invite_apply_stubs.cpp patch/invite_answer_apply.cpp patch/invite_service.cpp patch/invite_transport_curl.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_invite_answer_apply PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_invite_answer_apply PRIVATE nevr::plugin_sdk GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json CURL::libcurl ws2_32)
    gtest_discover_tests(test_invite_answer_apply DISCOVERY_MODE PRE_TEST)

    # #906, docs/adr/0017: what the wizard's cards chose (patch/invite_choices.h): header only, pure.
    add_executable(test_invite_choices tests/test_invite_choices.cpp)
    set_source_files_properties(tests/test_invite_choices.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_invite_choices PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_invite_choices PRIVATE nevr::plugin_sdk GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json)
    gtest_discover_tests(test_invite_choices DISCOVERY_MODE PRE_TEST)

    # #896, #906, docs/adr/0017: the UI text writer (patch/ui_text.h): header only, pure.
    add_executable(test_ui_text tests/test_ui_text.cpp)
    set_source_files_properties(tests/test_ui_text.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_ui_text PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_ui_text PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_ui_text DISCOVERY_MODE PRE_TEST)

    # #963: the CREATE GAME guild picker (patch/guild_picker.h): header only, pure.
    add_executable(test_guild_picker tests/test_guild_picker.cpp)
    set_source_files_properties(tests/test_guild_picker.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_guild_picker PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_guild_picker PRIVATE GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json)
    gtest_discover_tests(test_guild_picker DISCOVERY_MODE PRE_TEST)

    # The #408 self-check (compat/status_redirect_check.h): the game's API connections counted as redirected or
    # passed through. Header only on top of the self-check unit.
    add_executable(test_status_redirect_check
      tests/test_status_redirect_check.cpp
      compat/self_check.cpp
      compat/evr_codec.cpp)
    set_source_files_properties(tests/test_status_redirect_check.cpp compat/self_check.cpp compat/evr_codec.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_status_redirect_check PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_status_redirect_check PRIVATE nevr::plugin_sdk
      GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json)
    gtest_discover_tests(test_status_redirect_check DISCOVERY_MODE PRE_TEST)

    # Legacy <-> current translator (compat/legacy_codec.cpp): platform neutral like evr_codec, so it builds
    # and runs on its own. It is not part of BugSplat64.dll until the legacy mode wires it in.
    find_package(ZLIB REQUIRED)
    add_executable(test_legacy_codec
      tests/test_legacy_codec.cpp
      compat/legacy_codec.cpp
      compat/evr_codec.cpp)
    set_source_files_properties(tests/test_legacy_codec.cpp compat/legacy_codec.cpp compat/evr_codec.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_legacy_codec PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_legacy_codec PRIVATE nevr::plugin_sdk
      GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json zstd::libzstd ZLIB::ZLIB)
    gtest_discover_tests(test_legacy_codec DISCOVERY_MODE PRE_TEST)

    # CSymbol64Hash with plain `char` unsigned, as under the NDK's AArch64 clang (#657). A change to
    # nevr/symbol_hash.h that zero-extends a byte >= 0x80 passes with the default signed char and fails here.
    add_executable(test_symbol_hash tests/test_symbol_hash.cpp)
    set_source_files_properties(tests/test_symbol_hash.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_compile_options(test_symbol_hash PRIVATE -funsigned-char)
    target_include_directories(test_symbol_hash PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_symbol_hash PRIVATE nevr::plugin_sdk GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_symbol_hash DISCOVERY_MODE PRE_TEST)

    # The per-login legacy session (compat/legacy_session.cpp) on top of the translator; same platform-neutral
    # rules, so it also builds and runs on its own.
    add_executable(test_legacy_session
      tests/test_legacy_session.cpp
      compat/legacy_session.cpp
      compat/legacy_codec.cpp
      compat/evr_codec.cpp)
    set_source_files_properties(tests/test_legacy_session.cpp compat/legacy_session.cpp compat/legacy_codec.cpp
      compat/evr_codec.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_legacy_session PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_legacy_session PRIVATE nevr::plugin_sdk
      GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json zstd::libzstd ZLIB::ZLIB)
    gtest_discover_tests(test_legacy_session DISCOVERY_MODE PRE_TEST)

    # The plugin asset registry (patch/asset_registry.cpp): std + nlohmann-json only, so it builds and runs on
    # its own. The hooks that serve it to the engine (patch/plugin_assets.cpp) are in BugSplat64.dll.
    add_executable(test_asset_registry
      tests/test_asset_registry.cpp
      patch/asset_registry.cpp)
    set_source_files_properties(tests/test_asset_registry.cpp patch/asset_registry.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_asset_registry PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_asset_registry PRIVATE nevr::plugin_sdk
      GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json)
    gtest_discover_tests(test_asset_registry DISCOVERY_MODE PRE_TEST)

    add_executable(test_social_facade
      tests/test_social_facade.cpp
      patch/social_facade_object.cpp
      compat/social_names.cpp)
    target_compile_definitions(test_social_facade PRIVATE NEVR_TEST_HOOKS NEVR_USE_MINHOOK)
    set_source_files_properties(tests/test_social_facade.cpp patch/social_facade_object.cpp compat/social_names.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_social_facade PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_social_facade PRIVATE
      GTest::gtest GTest::gtest_main nevr_core minhook zstd::libzstd nlohmann_json::nlohmann_json)
    gtest_discover_tests(test_social_facade DISCOVERY_MODE PRE_TEST)

    # test_server_timestep.cpp (#511, header-only lifecycle/server_timestep.h) rides in this executable.
    add_executable(test_early_quit_lockout tests/test_early_quit_lockout.cpp tests/test_server_timestep.cpp)
    set_source_files_properties(tests/test_early_quit_lockout.cpp tests/test_server_timestep.cpp
                                PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_early_quit_lockout PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_early_quit_lockout PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_early_quit_lockout DISCOVERY_MODE PRE_TEST)

    # test_lobby_full_backoff.cpp (#1011, header-only patch/lobby_full_backoff_rules.h): the lobby-full retry policy.
    add_executable(test_lobby_full_backoff tests/test_lobby_full_backoff.cpp)
    target_include_directories(test_lobby_full_backoff PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_lobby_full_backoff PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_lobby_full_backoff DISCOVERY_MODE PRE_TEST)

    # test_party_follow_dedup.cpp (#1413): the lobby join state (compat/lobby_join_state.h) and the follow decision
    # (patch/party_follow_dedup_rules.h). Both header-only, pure.
    add_executable(test_party_follow_dedup tests/test_party_follow_dedup.cpp)
    target_include_directories(test_party_follow_dedup PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_party_follow_dedup PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_party_follow_dedup DISCOVERY_MODE PRE_TEST)

    # test_lobby_failure_message.cpp (#1443, header-only patch/lobby_failure_message_rules.h): the service message on the
    # error page for the lobby failure codes the game shows as a literal.
    add_executable(test_lobby_failure_message tests/test_lobby_failure_message.cpp)
    target_include_directories(test_lobby_failure_message PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_lobby_failure_message PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_lobby_failure_message DISCOVERY_MODE PRE_TEST)

    # test_menu_version_label.cpp (#747, header-only patch/menu_version_label_rules.h).
    add_executable(test_menu_version_label tests/test_menu_version_label.cpp)
    set_source_files_properties(tests/test_menu_version_label.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_menu_version_label PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_menu_version_label PRIVATE nevr::plugin_sdk GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_menu_version_label DISCOVERY_MODE PRE_TEST)

    # test_splash_skip.cpp (#962, header-only patch/splash_skip_rules.h).
    add_executable(test_splash_skip tests/test_splash_skip.cpp)
    set_source_files_properties(tests/test_splash_skip.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_splash_skip PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_splash_skip PRIVATE nevr::plugin_sdk GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_splash_skip DISCOVERY_MODE PRE_TEST)

    # test_ui_texture_mips.cpp (#961, header-only patch/ui_texture_mips_rules.h).
    add_executable(test_ui_texture_mips tests/test_ui_texture_mips.cpp)
    set_source_files_properties(tests/test_ui_texture_mips.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_ui_texture_mips PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_ui_texture_mips PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_ui_texture_mips DISCOVERY_MODE PRE_TEST)

    # test_cmdline_strip.cpp (#515, header-only lifecycle/cmdline_strip.h); nevr_core supplies g_headlessTickRateHz.
    add_executable(test_cmdline_strip tests/test_cmdline_strip.cpp tests/test_timestep_log.cpp)
    set_source_files_properties(tests/test_cmdline_strip.cpp tests/test_timestep_log.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_cmdline_strip PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_cmdline_strip PRIVATE GTest::gtest GTest::gtest_main nevr_core)
    gtest_discover_tests(test_cmdline_strip DISCOVERY_MODE PRE_TEST)

    # test_cli_check.cpp (#1253, header-only lifecycle/cli_check.h over core/cli_flags.h and core/cli_option_value.h).
    add_executable(test_cli_check tests/test_cli_check.cpp)
    set_source_files_properties(tests/test_cli_check.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_cli_check PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_cli_check PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_cli_check DISCOVERY_MODE PRE_TEST)

    # test_scenario_ui_text.cpp (#906, the ui_text fire action in runtime/scenario/scenario_protocol.h) and test_scenario_invite.cpp
    # (#1156, the invite fire actions) ride in this executable.
    add_executable(test_scenario_early_quit tests/test_scenario_early_quit.cpp tests/test_scenario_ui_text.cpp tests/test_scenario_invite.cpp)
    set_source_files_properties(tests/test_scenario_early_quit.cpp tests/test_scenario_ui_text.cpp tests/test_scenario_invite.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_scenario_early_quit PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_scenario_early_quit PRIVATE
      GTest::gtest GTest::gtest_main nevr_core nlohmann_json::nlohmann_json)
    gtest_discover_tests(test_scenario_early_quit DISCOVERY_MODE PRE_TEST)

    # test_return_to_lobby_hold.cpp (#58, header-only lifecycle/return_to_lobby_hold.h) and
    # test_server_bringup.cpp (#45, #592, header-only lifecycle/server_bringup.h) and
    # test_return_to_lobby_queue.cpp (#730, header-only lifecycle/return_to_lobby_queue.h) and
    # test_window_watch.cpp (#341, the decision half of frame/window_watch.h) ride in this executable.
    add_executable(test_schannel_cred_guard tests/test_schannel_cred_guard.cpp tests/test_return_to_lobby_hold.cpp
                   tests/test_server_bringup.cpp tests/test_return_to_lobby_queue.cpp tests/test_window_watch.cpp)
    target_include_directories(test_schannel_cred_guard PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_schannel_cred_guard PRIVATE GTest::gtest GTest::gtest_main nevr::plugin_sdk)
    gtest_discover_tests(test_schannel_cred_guard DISCOVERY_MODE PRE_TEST)

    add_executable(test_hooking tests/test_hooking.cpp)
    target_compile_definitions(test_hooking PRIVATE NEVR_USE_MINHOOK)
    target_include_directories(test_hooking PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_hooking PRIVATE GTest::gtest GTest::gtest_main minhook)
    gtest_discover_tests(test_hooking DISCOVERY_MODE PRE_TEST)

    # #1774: the loading-tip node hooks, installed into a hand-assembled image under real MinHook (no game binary).
    add_executable(test_loading_tips tests/test_loading_tips.cpp patch/loading_tips.cpp hook/hook_guard.cpp
      hook/hook_liveness.cpp log/boot_log_tee.cpp)
    set_source_files_properties(tests/test_loading_tips.cpp patch/loading_tips.cpp hook/hook_guard.cpp
      hook/hook_liveness.cpp log/boot_log_tee.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_compile_definitions(test_loading_tips PRIVATE NEVR_USE_MINHOOK)
    target_include_directories(test_loading_tips PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_loading_tips PRIVATE GTest::gtest GTest::gtest_main minhook nevr_core)
    gtest_discover_tests(test_loading_tips DISCOVERY_MODE PRE_TEST)

    # DllLoadHook load-filter test (#361): the real LoadLibrary hooks under MinHook.
    add_executable(test_dll_load_hook tests/test_dll_load_hook.cpp hook/dll_load_hook.cpp hook/hook_guard.cpp log/boot_log_tee.cpp)
    set_source_files_properties(tests/test_dll_load_hook.cpp hook/dll_load_hook.cpp hook/hook_guard.cpp log/boot_log_tee.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_compile_definitions(test_dll_load_hook PRIVATE NEVR_USE_MINHOOK NEVR_DLL_LOAD_HOOK_TESTING NEVR_TEST_HOOKS)
    target_include_directories(test_dll_load_hook PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_dll_load_hook PRIVATE GTest::gtest GTest::gtest_main minhook nevr_core)
    # #504's timing fixture loads the stand-in pnsrad.dll that test_export_trace builds.
    add_dependencies(test_dll_load_hook nevr_export_trace_fixture)
    gtest_discover_tests(test_dll_load_hook DISCOVERY_MODE PRE_TEST)

    # #593: Log() routes by boot phase. Links the real Log() (nevr_core) and the real boot tee.
    add_executable(test_boot_log_routing tests/test_boot_log_routing.cpp log/boot_log_tee.cpp)
    set_source_files_properties(tests/test_boot_log_routing.cpp log/boot_log_tee.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_boot_log_routing PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_boot_log_routing PRIVATE GTest::gtest GTest::gtest_main nevr_core)
    gtest_discover_tests(test_boot_log_routing DISCOVERY_MODE PRE_TEST)

    # plugin_load_plan tests (N134 S6) — the PURE core of the config-driven plugin
    # loader: BuildLoadPlan (ordered selection with enabled flag, file defaulting,
    # required/target carry), ArgsToJson (the v4 args_json contract), and
    # ChoosePluginInit (v4-preferred/v3-fallback). Compiles plugin_load_plan.cpp
    # directly (the same TU nevr_runtime links) + nevr_core (nevr_config) + yaml-cpp
    # + nlohmann-json. No game stubs: the builder has no windows/Log/extern deps.
    # The LoadLibrary-entangled loader (plugin_loader.cpp) is NOT linked here — it
    # is exercised by the differential server run.
    add_executable(test_plugin_load_plan
      tests/test_plugin_load_plan.cpp
      ext/plugin_load_plan.cpp
      ext/plugin_manifest.cpp)
    set_source_files_properties(tests/test_plugin_load_plan.cpp ext/plugin_load_plan.cpp
      ext/plugin_manifest.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_plugin_load_plan PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_plugin_load_plan PRIVATE
      GTest::gtest GTest::gtest_main nevr_core yaml-cpp::yaml-cpp nlohmann_json::nlohmann_json)
    gtest_discover_tests(test_plugin_load_plan DISCOVERY_MODE PRE_TEST)

    # plugin_signature tests (ADR 0010, #741) — the pure verifier over signed fixtures that
    # tools/gen_plugin_signature_fixtures.py writes into $NEVR_SIGFIX_DIR. Links only the
    # verifier TU and crypt32/advapi32: no game stubs, no Log. The verifier comes from nevr_host_core, the one
    # library built with NEVR_HOST_TESTING, because the throwing seam exists only there (verify_host_seams.py).
    add_executable(test_plugin_signature
      tests/test_plugin_signature.cpp)
    set_source_files_properties(tests/test_plugin_signature.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_plugin_signature PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_plugin_signature PRIVATE
      nevr_host_core GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json crypt32 advapi32)
    gtest_discover_tests(test_plugin_signature DISCOVERY_MODE PRE_TEST)

    # plugin_tree tests (ADR 0010, #741): a plugin's imports, delay loads and forwarders from its own directory
    # pass the same check, over fixture directories the same generator writes.
    add_executable(test_plugin_tree
      tests/test_plugin_tree.cpp
      ext/plugin_tree.cpp
      ext/plugin_signature.cpp
      ext/plugin_pins.cpp)
    set_source_files_properties(tests/test_plugin_tree.cpp ext/plugin_tree.cpp ext/plugin_signature.cpp ext/plugin_pins.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_plugin_tree PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_plugin_tree PRIVATE
      GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json crypt32 advapi32)
    gtest_discover_tests(test_plugin_tree DISCOVERY_MODE PRE_TEST)

    # plugin_trust tests (ADR 0010, #741): the pinned root matches its digest, the policy is well formed.
    add_executable(test_plugin_trust
      tests/test_plugin_trust.cpp
      ext/plugin_trust.cpp
      ext/plugin_signature.cpp
      ext/plugin_pins.cpp)
    set_source_files_properties(tests/test_plugin_trust.cpp ext/plugin_trust.cpp ext/plugin_signature.cpp ext/plugin_pins.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_plugin_trust PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_plugin_trust PRIVATE GTest::gtest GTest::gtest_main crypt32 advapi32)
    gtest_discover_tests(test_plugin_trust DISCOVERY_MODE PRE_TEST)

    # self_provenance tests (#842, ADR 0010 section 1): the label for a signed release, an unsigned build and a dev version,
    # and the module's own file read through the verifier. Compiles the decision and the verifier only, no game.
    add_executable(test_self_provenance
      tests/test_self_provenance.cpp
      ext/self_provenance.cpp
      ext/plugin_signature.cpp)
    set_source_files_properties(tests/test_self_provenance.cpp ext/self_provenance.cpp ext/plugin_signature.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_self_provenance PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_self_provenance PRIVATE GTest::gtest GTest::gtest_main crypt32 advapi32)
    gtest_discover_tests(test_self_provenance DISCOVERY_MODE PRE_TEST)

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
      tests/test_resource_override.cpp
      tests/test_evrp_package.cpp
      # evrp_package.cpp is pure parsing; the test drives the production parser.
      patch/evrp_package.cpp
      # #703: resource_override registers the cache's overrides through the production cache.
      patch/asset_cache.cpp
      patch/resource_override.cpp
      # #1420: the netgun bullet transforms, driven through the resource seam.
      patch/netgun_bullets.cpp
      # #479: the XPID patch's failure path (g_bootHookFailed, never ServerFatal) is driven for real.
      patch/xpid_patch.cpp
      # ws_bridge.cpp calls nevr_system_info::Get(). Linked as a source rather than
      # via nevr_core, because this test deliberately does NOT link core — it
      # provides its own stubs for Log/FatalError and linking the real ones
      # would collide. system_info.cpp depends on nothing but Win32, so it
      # drops in cleanly and the test drives the REAL measurement code.
      "${CMAKE_SOURCE_DIR}/src/core/system_info.cpp"
      # N112: build_identity.cpp depends on nothing but preprocessor stringify
      # macros — no Win32, no PCH, no external deps. Drops in cleanly alongside
      # system_info.cpp so the test drives the REAL compile-time constants.
      "${CMAKE_SOURCE_DIR}/src/core/build_identity.cpp"
      ext/plugin_loader.cpp
      # #60: plugin_loader.cpp builds the login's plugin report through this.
      ext/plugin_manifest.cpp
      # #432: plugin_loader.cpp and ws_bridge.cpp read the process-wide plugin asset registry.
      patch/asset_registry.cpp
      # #741: the signature gate StageOne consults, linked as production source.
      ext/plugin_signature.cpp
      ext/plugin_pins.cpp
      ext/plugin_tree.cpp
      ext/plugin_trust.cpp
      ext/module_loader.cpp
      log/url_diagnostics.cpp
      # patching.h's PatchDetour reports through BootLogTee while boot is running (#92).
      log/boot_log_tee.cpp
      compat/ws_bridge.cpp
      compat/session_router.cpp
      compat/ws_session_transport.cpp
      compat/login_profile.cpp
      compat/evr_codec.cpp
      # self-checks: ws_bridge.cpp wires the sender and the login gate through the unit.
      compat/self_check.cpp
      # #41: ws_bridge.cpp builds its URL credentials through the ServerDbUri encoder.
      server/serverdb_uri.cpp
      hook/symbol_corpus.cpp
      log/symcache_data.cpp
      # N84: plugin_loader calls nevr_hook_guard::VerifyAll after each plugin init.
      # Linked as production source (not a stub) so the test drives the real
      # guard — a stub here would make the N84 tests tautological.
      hook/hook_guard.cpp
      hook/hook_liveness.cpp
      # #741: plugin_loader registers its load filter with the hook.
      hook/dll_load_hook.cpp
    )
    target_compile_definitions(test_behavioral PRIVATE NEVR_TEST_HOOKS NEVR_USE_MINHOOK
      CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
    # build_identity.cpp is compiled again here, from src/core (nevr_core's definitions are another directory's).
    nevr_build_identity_define(TARGET test_behavioral READS VERSION COMMIT_HASH COMMIT_FULL DESCRIBE
      SOURCES "${CMAKE_SOURCE_DIR}/src/core/build_identity.cpp")
    # Disable PCH — the test has its own stub headers and must not depend on
    # the nevr_runtime precompiled header (which pulls in MinHook, etc.).
    set_source_files_properties(
      "${CMAKE_SOURCE_DIR}/src/core/system_info.cpp"
      "${CMAKE_SOURCE_DIR}/src/core/build_identity.cpp"
      tests/test_behavioral.cpp
      tests/test_resource_override.cpp
      tests/test_evrp_package.cpp
      patch/evrp_package.cpp
      patch/asset_cache.cpp
      patch/resource_override.cpp
      patch/netgun_bullets.cpp
      patch/xpid_patch.cpp
      ext/plugin_loader.cpp
      ext/plugin_manifest.cpp
      patch/asset_registry.cpp
      ext/plugin_signature.cpp
      ext/plugin_pins.cpp
      ext/plugin_tree.cpp
      ext/plugin_trust.cpp
      ext/module_loader.cpp
      log/url_diagnostics.cpp
      compat/ws_bridge.cpp
      compat/session_router.cpp
      compat/ws_session_transport.cpp
      compat/login_profile.cpp
      compat/evr_codec.cpp
      compat/self_check.cpp
      server/serverdb_uri.cpp
      hook/symbol_corpus.cpp
      log/symcache_data.cpp
      hook/hook_liveness.cpp
      hook/dll_load_hook.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON
    )
    target_include_directories(test_behavioral PRIVATE
      ${CMAKE_SOURCE_DIR}/src
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
      GTest::gtest GTest::gtest_main nevr::plugin_sdk
      ixwebsocket::ixwebsocket nlohmann_json::nlohmann_json minhook CURL::libcurl
      ws2_32 psapi advapi32 crypt32
    )

    # Hermetic DLL fixtures for the real LoadLibraryExA path in
    # test_behavioral's plugin-loader diagnostics. They are placed beside the
    # test executable (under bin/plugins), which is exactly where LoadPlugins
    # resolves configured DLLs when the executable is the host module.
    add_library(test_plugin_future_api SHARED
      tests/plugin_future_api_test_dll.cpp)
    target_compile_definitions(test_plugin_future_api PRIVATE NEVR_PLUGIN_EXPORTS)
    target_link_libraries(test_plugin_future_api PRIVATE nevr::plugin_sdk)
    set_target_properties(test_plugin_future_api PROPERTIES
      PREFIX ""
      OUTPUT_NAME "test_plugin_future_api"
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins")
    add_library(test_plugin_onframe SHARED
      tests/plugin_onframe_test_dll.cpp)
    target_compile_definitions(test_plugin_onframe PRIVATE NEVR_PLUGIN_EXPORTS)
    target_link_libraries(test_plugin_onframe PRIVATE nevr::plugin_sdk)
    set_target_properties(test_plugin_onframe PROPERTIES
      PREFIX ""
      OUTPUT_NAME "test_plugin_onframe"
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins")
    # #598: two builds of one fixture that logs through ctx->log, so a test can tell two plugins' ids apart.
    foreach(_logging_suffix a b)
      add_library(test_plugin_logging_${_logging_suffix} SHARED
        tests/plugin_logging_test_dll.cpp)
      target_compile_definitions(test_plugin_logging_${_logging_suffix} PRIVATE NEVR_PLUGIN_EXPORTS
        TEST_PLUGIN_NAME="test-plugin-logging-${_logging_suffix}")
      target_link_libraries(test_plugin_logging_${_logging_suffix} PRIVATE nevr::plugin_sdk)
      set_target_properties(test_plugin_logging_${_logging_suffix} PROPERTIES
        PREFIX ""
        OUTPUT_NAME "test_plugin_logging_${_logging_suffix}"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins")
    endforeach()
    # #1094: a plugin that reads the main level from its init, per-frame and state-change contexts.
    add_library(test_plugin_level SHARED
      tests/plugin_level_test_dll.cpp)
    target_compile_definitions(test_plugin_level PRIVATE NEVR_PLUGIN_EXPORTS)
    target_link_libraries(test_plugin_level PRIVATE nevr::plugin_sdk)
    set_target_properties(test_plugin_level PROPERTIES
      PREFIX ""
      OUTPUT_NAME "test_plugin_level"
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins")
    add_library(test_plugin_init_fail SHARED
      tests/plugin_init_fail_test_dll.cpp)
    target_compile_definitions(test_plugin_init_fail PRIVATE NEVR_PLUGIN_EXPORTS)
    target_link_libraries(test_plugin_init_fail PRIVATE nevr::plugin_sdk)
    set_target_properties(test_plugin_init_fail PROPERTIES
      PREFIX ""
      OUTPUT_NAME "test_plugin_init_fail"
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins")
    add_library(test_plugin_partial_init_fail SHARED
      tests/plugin_partial_init_fail_test_dll.cpp)
    target_compile_definitions(test_plugin_partial_init_fail PRIVATE NEVR_PLUGIN_EXPORTS)
    target_link_libraries(test_plugin_partial_init_fail PRIVATE nevr::plugin_sdk)
    set_target_properties(test_plugin_partial_init_fail PROPERTIES
      PREFIX ""
      OUTPUT_NAME "test_plugin_partial_init_fail"
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins")
    add_library(test_plugin_info_keeper SHARED
      tests/plugin_info_keeper_test_dll.cpp)
    target_compile_definitions(test_plugin_info_keeper PRIVATE NEVR_PLUGIN_EXPORTS)
    target_link_libraries(test_plugin_info_keeper PRIVATE nevr::plugin_sdk)
    set_target_properties(test_plugin_info_keeper PROPERTIES
      PREFIX ""
      OUTPUT_NAME "test_plugin_info_keeper"
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins")
    add_dependencies(test_behavioral test_plugin_future_api test_plugin_onframe test_plugin_init_fail
      test_plugin_partial_init_fail test_plugin_info_keeper test_plugin_logging_a test_plugin_logging_b
      test_plugin_level)

    gtest_discover_tests(test_behavioral DISCOVERY_MODE PRE_TEST)

    # CachedAuthToken claim parsing is header-only production code. Keep it in
    # a small independent executable so JWT regressions do not require the
    # network-facing token-auth module or a live Nakama service.
    add_executable(test_token_auth
      tests/test_token_auth.cpp)
    set_source_files_properties(tests/test_token_auth.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_compile_definitions(test_token_auth PRIVATE NEVR_TEST_HOOKS)
    target_include_directories(test_token_auth PRIVATE
      ${CMAKE_SOURCE_DIR}/src
      ${CMAKE_SOURCE_DIR}/src/modules/token-auth/src)
    target_link_libraries(test_token_auth PRIVATE
      GTest::gtest GTest::gtest_main nevr_core nlohmann_json::nlohmann_json nevr_token_auth_test_hooks)
    gtest_discover_tests(test_token_auth DISCOVERY_MODE PRE_TEST)

    # test_lobby_settings_relay.cpp (#905, server/lobby_settings_relay.cpp) rides in this executable so the existing
    # `just test-auth-unit` loop runs it with no recipe change.
    add_executable(test_messages tests/test_messages.cpp tests/test_lobby_settings_relay.cpp server/messages.cpp
      server/session_success_dispatch.cpp server/lobby_settings_relay.cpp)
    set_source_files_properties(tests/test_messages.cpp tests/test_lobby_settings_relay.cpp server/messages.cpp
      server/session_success_dispatch.cpp server/lobby_settings_relay.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_messages PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_messages PRIVATE
      GTest::gtest GTest::gtest_main nevr_abi nevr_api nlohmann_json::nlohmann_json ZLIB::ZLIB ws2_32)
    gtest_discover_tests(test_messages DISCOVERY_MODE PRE_TEST)

    # test_plugin_logger.cpp (#34, header-only sdk/include/nevr/plugin_logger.h) and
    # test_plugin_log.cpp (#598, header-only ext/plugin_log.h) and
    # test_rate_limiter.cpp (#894, header-only log/rate_limiter.h) ride in this
    # executable so the existing `just test-auth-unit` loop runs it.
    add_executable(test_crash_recovery
      tests/test_crash_recovery.cpp tests/test_game_line_format.cpp tests/test_bridge_line.cpp tests/test_broadcaster_pointer_guard.cpp tests/test_process_memory.cpp tests/test_pnsrad_diag_queue.cpp tests/test_pnsrad_load_queue.cpp tests/test_json_escape.cpp tests/test_plugin_logger.cpp tests/test_level_gate.cpp tests/test_plugin_log.cpp tests/test_resource_registry.cpp tests/test_bounded_thread.cpp tests/test_reader_gate.cpp tests/test_checked_patch.cpp tests/test_credits_text.cpp tests/test_menu_page.cpp tests/test_netgun_bullets.cpp tests/test_netgun_bullet_ids.cpp tests/test_rate_limiter.cpp tests/test_clock_skew.cpp lifecycle/crash_dump_format.cpp tests/test_crash_report_pe.cpp lifecycle/crash_report_pe.cpp tests/test_crash_first.cpp lifecycle/crash_first_filter.cpp)
    set_source_files_properties(tests/test_crash_recovery.cpp tests/test_game_line_format.cpp tests/test_bridge_line.cpp tests/test_broadcaster_pointer_guard.cpp tests/test_process_memory.cpp tests/test_pnsrad_diag_queue.cpp tests/test_pnsrad_load_queue.cpp tests/test_json_escape.cpp tests/test_plugin_logger.cpp tests/test_level_gate.cpp tests/test_plugin_log.cpp tests/test_resource_registry.cpp tests/test_bounded_thread.cpp tests/test_reader_gate.cpp tests/test_checked_patch.cpp tests/test_credits_text.cpp tests/test_menu_page.cpp tests/test_netgun_bullets.cpp tests/test_netgun_bullet_ids.cpp tests/test_rate_limiter.cpp tests/test_clock_skew.cpp lifecycle/crash_dump_format.cpp tests/test_crash_report_pe.cpp lifecycle/crash_report_pe.cpp tests/test_crash_first.cpp lifecycle/crash_first_filter.cpp
      PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(test_crash_recovery PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_crash_recovery PRIVATE GTest::gtest GTest::gtest_main nevr::plugin_sdk)
    gtest_discover_tests(test_crash_recovery DISCOVERY_MODE PRE_TEST)
  endif()
endif()
