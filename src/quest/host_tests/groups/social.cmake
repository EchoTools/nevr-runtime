# test-quest-social: the Quest social provider on the host (docs/adr/0003, "Social provider"): the ABI pins against the recorded
# vtable, the facade driven through its vtable, the hook decision through the real callback thunk, and the exception contract. The
# frames that sit under a call into the game (social_game_calls.cpp, social_install.cpp) and everything that includes
# callback_thunk.h are built with -fno-exceptions, and tools/check_quest_social_frames.sh pins that their objects carry no
# personality and no LSDA (with a negative control on the object that does). No NDK, no Android, no APK.
# test-quest-social-pinned: the pins against the real libr15.so and libpnsovr.so.

nevr_ht_group(social)
set(_sent src/quest/sentinel)
set(_on HOST INCS ${_sent})
set(_off HOST NOEXC INCS ${_sent})

nevr_ht_test(social_abi_test GROUP social ${_on} ARGS src/quest/tests/fixtures/cnsovrsocial_vtable.txt
  SRC src/quest/tests/social_abi_test.cpp)
nevr_ht_obj(o_social_facade GROUP social ${_on} SRC src/quest/social/social_facade.cpp)
nevr_ht_obj(o_social_frames GROUP social ${_on} SRC src/quest/social/social_frames.cpp)
nevr_ht_obj(o_social_hook_log GROUP social ${_on} SRC src/quest/sentinel/hook_log.cpp)
nevr_ht_obj(o_social_got_hook GROUP social ${_off} SRC src/quest/sentinel/got_hook.cpp)
nevr_ht_obj(o_social_hook_report GROUP social ${_off} SRC src/quest/sentinel/hook_report.cpp)
nevr_ht_obj(o_social_game_calls GROUP social ${_off} SRC src/quest/social/social_game_calls.cpp)
nevr_ht_obj(o_social_install GROUP social ${_off} SRC src/quest/social/social_install.cpp)
nevr_ht_obj(o_social_invite_gate GROUP social ${_off} SRC src/quest/social/social_invite_gate.cpp)
# The patched libr15's identity (sentinel::pinned::LibR15PatchedBuildId, docs/adr/0021 section 7), which the hook translation units
# above call through the pinned_targets.h factories.
nevr_ht_obj(o_social_id_libr15_identity GROUP social ${_off} SRC src/quest/sentinel/libr15_identity.cpp)
nevr_ht_obj(o_social_id_libr15_patch_identity GROUP social ${_off} SRC src/quest/patch/libr15_patch_identity.cpp)
nevr_ht_obj(o_social_id_sha256 GROUP social ${_off} SRC src/quest/data/sha256.cpp)
set(_ident o_social_id_libr15_identity o_social_id_libr15_patch_identity o_social_id_sha256)
# The friend-name decoder, as the Quest build compiles it: no static registration (the sentinel carries no dynamic initializer;
# InstallSocialHook registers it).
nevr_ht_obj(o_social_names GROUP social ${_on} DEFS NEVR_SOCIAL_NAMES_NO_STATIC_REGISTRATION SRC src/runtime/compat/social_names.cpp)
# The frames live across a call into the game carry no exception machinery; the negative control is an object that does.
nevr_ht_check(social_frames_carry_no_personality GROUP social TIMEOUT 60
  SRC ${NEVR_REPO_ROOT}/tools/check_quest_social_frames.sh nm readelf
  $<TARGET_OBJECTS:o_social_game_calls>=SlotUpdateEntry $<TARGET_OBJECTS:o_social_game_calls>=SlotResetEntry
  $<TARGET_OBJECTS:o_social_install>=OnSocial $<TARGET_OBJECTS:o_social_install>=OnPresence
  $<TARGET_OBJECTS:o_social_invite_gate>=OnBoolean)
nevr_ht_check(social_frame_checker_is_not_blind GROUP social TIMEOUT 60
  SRC ${CMAKE_COMMAND} -DCMD=${NEVR_REPO_ROOT}/tools/check_quest_social_frames.sh
  "-DCMD_ARGS=nm|readelf|$<TARGET_OBJECTS:o_social_facade>=Facade" -DEXPECT_RC=nonzero -DEXPECT_OUT=personality
  -P ${NEVR_HT_DIR}/scripts/expect_exit.cmake)
nevr_ht_test(social_facade_test GROUP social ${_on} DL PTHREAD
  OBJS o_social_facade o_social_frames o_social_game_calls o_social_hook_log SRC src/quest/tests/social_facade_test.cpp)
nevr_ht_test(social_names_test GROUP social ${_on} DL PTHREAD LIBS ${NEVR_ZSTD_LIB}
  OBJS o_social_facade o_social_frames o_social_game_calls o_social_hook_log o_social_names
  SRC src/quest/tests/social_names_test.cpp)
# A hang is a failure, not a stuck gate.
nevr_ht_test(social_install_test GROUP social ${_off} DL PTHREAD TIMEOUT 300 LIBS ${NEVR_ZSTD_LIB}
  OBJS o_social_install o_social_facade o_social_game_calls o_social_got_hook o_social_hook_log o_social_hook_report o_social_names
       o_social_invite_gate ${_ident}
  SRC src/quest/tests/social_install_test.cpp)

# ── pinned ──────────────────────────────────────────────────────────────────────────────────────────────────────────────
# social_pinned_test runs on the store APK's libr15.so and libpnsovr.so, which the recipe extracts: it is built here and the recipe
# runs it by name.
nevr_ht_group(social-pinned)
nevr_ht_obj(o_socialp_facade GROUP social-pinned INCS ${_sent} SRC src/quest/social/social_facade.cpp)
nevr_ht_obj(o_socialp_names GROUP social-pinned DEFS NEVR_SOCIAL_NAMES_NO_STATIC_REGISTRATION SRC src/runtime/compat/social_names.cpp)
nevr_ht_test(social_pinned_test GROUP social-pinned NOEXC DL PTHREAD NO_TEST INCS ${_sent} LIBS ${NEVR_ZSTD_LIB}
  OBJS o_socialp_facade o_socialp_names
  SRC src/quest/tests/social_pinned_test.cpp src/quest/social/social_install.cpp src/quest/social/social_invite_gate.cpp
      src/quest/social/social_game_calls.cpp src/quest/sentinel/got_hook.cpp src/quest/sentinel/hook_log.cpp
      src/quest/sentinel/hook_report.cpp src/quest/sentinel/libr15_identity.cpp src/quest/patch/libr15_patch_identity.cpp
      src/quest/data/sha256.cpp)
