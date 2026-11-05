# test-quest-integration: the Quest sentinel integration on the host (docs/adr/0003, "Integration"): the constructor sequence with
# fakes (order, gating by feature, counters before the single reporter start, one failing piece leaves the others running), the
# post-load policy, the dlopen handler and the pinned dlopen target, the identity source, the social switch, the frame tap, the
# counter budget over the real hook libraries, and the bridge end to end through the real loopback server and router with a fake
# connector. The hook translation units are built -fno-exceptions as on the device. No NDK, no Android.

nevr_ht_group(integration)
set(_sent src/quest/sentinel)
set(_on HOST INCS ${_sent})
set(_off HOST NOEXC INCS ${_sent})

# 1. sequence, post-load policy, identity, social switch, frame tap
nevr_ht_test(integration_sequence_test GROUP integration ${_on} PTHREAD TIMEOUT 300
  SRC src/quest/tests/integration_sequence_test.cpp
      src/quest/integration/ctor_sequence.cpp src/quest/integration/post_load.cpp
      src/quest/integration/identity_source.cpp src/quest/integration/stage_log.cpp src/quest/integration/bridge_uri.cpp
      src/quest/net/frame_tap.cpp src/quest/sentinel/hook_log.cpp src/runtime/compat/evr_codec.cpp
      src/quest/integration/self_check_wiring.cpp src/runtime/compat/self_check.cpp)

# 2. the hook translation units and the counter budget
foreach(f got_hook hook_report)
  nevr_ht_obj(o_integ_${f} GROUP integration ${_off} SRC src/quest/sentinel/${f}.cpp)
endforeach()
nevr_ht_obj(o_integ_tstring_thunks GROUP integration ${_off} SRC src/quest/redirect/tstring_thunks.cpp)
nevr_ht_obj(o_integ_dlopen_hook GROUP integration ${_off} SRC src/quest/integration/dlopen_hook.cpp)
# The dlopen handler calls the script redirect (#1737), built without exceptions as on the device.
nevr_ht_obj(o_integ_script_redirect GROUP integration ${_off} SRC src/quest/plugin/script_redirect.cpp)
nevr_ht_obj(o_integ_social_shim GROUP integration ${_off} SRC src/quest/integration/social_shim.cpp)
nevr_ht_obj(o_integ_login_prompt_hook GROUP integration ${_off} SRC src/quest/sentinel/login_prompt_hook.cpp)
nevr_ht_obj(o_integ_frame_dispatch GROUP integration ${_off} SRC src/quest/invite/frame_dispatch.cpp)
nevr_ht_obj(o_integ_login_title_hook GROUP integration ${_off} SRC src/quest/sentinel/login_title_hook.cpp)
nevr_ht_obj(o_integ_obb_skip_hook GROUP integration ${_off} SRC src/quest/sentinel/obb_skip_hook.cpp)
nevr_ht_obj(o_integ_texture_quality_hook GROUP integration ${_off} SRC src/quest/sentinel/texture_quality_hook.cpp)
nevr_ht_obj(o_integ_texture_menu_hook GROUP integration ${_off} SRC src/quest/sentinel/texture_menu_hook.cpp)
nevr_ht_obj(o_integ_texture_menu GROUP integration ${_on} SRC src/quest/sentinel/texture_menu.cpp)
# quit_hook.cpp is compiled here as a check and is not in the link of the hooks test.
nevr_ht_obj(o_integ_quit_hook GROUP integration ${_off} SRC src/quest/sentinel/quit_hook.cpp)
nevr_ht_obj(o_integ_resource_request_hook GROUP integration ${_off} SRC src/quest/sentinel/resource_request_hook.cpp)
# The shared core the hook's cargo decision links (docs/adr/0045-cdn-texture-delivery.md).
nevr_ht_obj(o_integ_cargo_shadow GROUP integration ${_off} SRC src/core/resource_shadow.cpp)
nevr_ht_obj(o_integ_cargo_resolver GROUP integration ${_off} SRC src/core/cargo_resolver.cpp)
nevr_ht_obj(o_integ_clock_levels_hook GROUP integration ${_off} SRC src/quest/sentinel/clock_levels_hook.cpp)
nevr_ht_obj(o_integ_login_counters GROUP integration ${_off} SRC src/quest/login/login_counters.cpp)
# login_counters.cpp reads the local-answer counters of login_prerequisites.cpp (#411).
nevr_ht_obj(o_integ_login_prerequisites GROUP integration ${_off} SRC src/quest/login/login_prerequisites.cpp)
nevr_ht_obj(o_integ_login_standin GROUP integration ${_off} SRC src/quest/login/login_standin.cpp)
nevr_ht_obj(o_integ_prompt_board GROUP integration ${_off} SRC src/quest/auth/prompt_board.cpp)
nevr_ht_obj(o_integ_social_game_calls GROUP integration ${_off} SRC src/quest/social/social_game_calls.cpp)
nevr_ht_obj(o_integ_social_install GROUP integration ${_off} SRC src/quest/social/social_install.cpp)
nevr_ht_obj(o_integ_social_invite_gate GROUP integration ${_off} SRC src/quest/social/social_invite_gate.cpp)
# The patched libr15's identity (sentinel::pinned::LibR15PatchedBuildId, docs/adr/0021 section 7), which the hook translation units
# above call through the pinned_targets.h factories.
nevr_ht_obj(o_integ_id_libr15_identity GROUP integration ${_off} SRC src/quest/sentinel/libr15_identity.cpp)
nevr_ht_obj(o_integ_id_libr15_patch_identity GROUP integration ${_off} SRC src/quest/patch/libr15_patch_identity.cpp)
nevr_ht_obj(o_integ_id_sha256 GROUP integration ${_off} SRC src/quest/data/sha256.cpp)
nevr_ht_obj(o_integ_social_facade GROUP integration ${_on} SRC src/quest/social/social_facade.cpp)
nevr_ht_obj(o_integ_hook_log GROUP integration ${_on} SRC src/quest/sentinel/hook_log.cpp)
nevr_ht_obj(o_integ_social_names GROUP integration ${_on} DEFS NEVR_SOCIAL_NAMES_NO_STATIC_REGISTRATION
  SRC src/runtime/compat/social_names.cpp)
# The dlopen hook's frames carry no personality (the one helper it calls is annotated and lives in another translation unit).
nevr_ht_check(dlopen_hook_frames GROUP integration TIMEOUT 60
  SRC ${CMAKE_COMMAND} -DREADELF=${NEVR_READELF} -DOBJECT=$<TARGET_OBJECTS:o_integ_dlopen_hook>
  -P ${NEVR_HT_DIR}/scripts/check_frames.cmake)
nevr_ht_test(integration_hooks_test GROUP integration ${_on} DL PTHREAD TIMEOUT 300 LIBS ${NEVR_YAML_LIB} ${NEVR_ZSTD_LIB}
  OBJS o_integ_got_hook o_integ_hook_report o_integ_tstring_thunks o_integ_dlopen_hook o_integ_script_redirect o_integ_social_shim
       o_integ_social_game_calls o_integ_social_install o_integ_social_invite_gate o_integ_social_facade o_integ_hook_log
       o_integ_social_names o_integ_login_prompt_hook o_integ_frame_dispatch o_integ_login_title_hook o_integ_obb_skip_hook
       o_integ_resource_request_hook o_integ_cargo_shadow o_integ_cargo_resolver o_integ_texture_quality_hook o_integ_texture_menu_hook o_integ_texture_menu
       o_integ_clock_levels_hook o_integ_prompt_board o_integ_login_counters o_integ_login_prerequisites o_integ_login_standin
       o_integ_id_libr15_identity o_integ_id_libr15_patch_identity o_integ_id_sha256
  SRC src/quest/tests/integration_hooks_test.cpp src/quest/integration/post_load.cpp src/quest/integration/ctor_sequence.cpp
      src/quest/redirect/service_redirector.cpp src/quest/redirect/hook_adapter.cpp
      src/quest/sentinel/quest_config.cpp src/core/nevr_config.cpp src/runtime/lifecycle/service_redirect.cpp
      src/runtime/lifecycle/stable_string_pool.cpp)
add_dependencies(host-tests-integration o_integ_quit_hook)

# 3. the bridge end to end, with a fake connector (SessionBridge still links the libcurl connector it builds when none is injected)
nevr_ht_test(session_bridge_test GROUP integration ${_on} PTHREAD CURL TIMEOUT 300 LIBS ssl crypto
  SRC src/quest/tests/session_bridge_test.cpp src/quest/net/session_bridge.cpp src/quest/net/frame_tap.cpp
      src/quest/net/curl_ws_connector.cpp src/quest/auth/ca_bundle.cpp
      src/quest/net/ws_wire.cpp src/quest/net/loopback_game_server.cpp src/quest/net/remote_ws.cpp
      src/runtime/compat/session_router.cpp src/runtime/compat/evr_codec.cpp src/runtime/server/serverdb_uri.cpp)

# ── pinned ──────────────────────────────────────────────────────────────────────────────────────────────────────────────
# The integration's pinned dlopen target against the real libr15.so (docs/adr/0003): the recipe extracts it from the store APK,
# checks its SHA-256 and runs the program by name, so the program is built here and is not a ctest test.
nevr_ht_group(integration-pinned)
nevr_ht_obj(o_integp_dlopen_hook GROUP integration-pinned NOEXC INCS ${_sent} SRC src/quest/integration/dlopen_hook.cpp)
nevr_ht_obj(o_integp_post_load GROUP integration-pinned INCS ${_sent} SRC src/quest/integration/post_load.cpp)
nevr_ht_test(integration_pinned_test GROUP integration-pinned NOEXC DL PTHREAD NO_TEST INCS ${_sent}
  OBJS o_integp_dlopen_hook o_integp_post_load
  SRC src/quest/tests/integration_pinned_test.cpp src/quest/plugin/script_redirect.cpp
      src/quest/sentinel/got_hook.cpp src/quest/sentinel/hook_log.cpp src/quest/sentinel/hook_report.cpp
      src/quest/sentinel/libr15_identity.cpp src/quest/patch/libr15_patch_identity.cpp src/quest/data/sha256.cpp)

# ── hooks-pinned ────────────────────────────────────────────────────────────────────────────────────────────────────────
# test-quest-hooks-pinned: the pinned targets resolved in the real libr15.so, libpnsradmatchmaking.so and libpnsovr.so
# (docs/adr/0003); the recipe extracts them and runs the program by name.
nevr_ht_group(hooks-pinned)
nevr_ht_test(got_pinned_test GROUP hooks-pinned NOEXC DL NO_TEST INCS ${_sent}
  SRC src/quest/tests/got_pinned_test.cpp src/quest/sentinel/got_hook.cpp src/quest/sentinel/hook_log.cpp
      src/quest/sentinel/libr15_identity.cpp src/quest/patch/libr15_patch_identity.cpp src/quest/data/sha256.cpp)
