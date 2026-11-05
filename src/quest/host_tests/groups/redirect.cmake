# test-quest-redirect: the Quest config-string redirect on the host. Two fixture shared objects import CJson::TString by its
# mangled name through a PLT slot (src/quest/redirect/tests); redirect_test drives the production ServiceRedirector, the typed
# thunks and GotHook against them with a fake game config. No NDK, no Android.

nevr_ht_group(redirect)
set(_sent src/quest/sentinel)
set(_fx src/quest/redirect/tests)

nevr_ht_so(redirect_fx_provider GROUP redirect HOST INCS ${_sent} OUTPUT_NAME redirfx_provider SRC ${_fx}/tstring_fixture_provider.cpp)
nevr_ht_so(redirect_fx_consumer_a GROUP redirect HOST INCS ${_sent} OUTPUT_NAME redirfx_consumer_a
  LIBS redirect_fx_provider LOPTS -Wl,-z,now,-z,relro BUILD_RPATH "\$ORIGIN" SRC ${_fx}/tstring_fixture_consumer.cpp)
nevr_ht_so(redirect_fx_consumer_b GROUP redirect HOST INCS ${_sent} OUTPUT_NAME redirfx_consumer_b
  LIBS redirect_fx_provider LOPTS -Wl,-z,now,-z,relro BUILD_RPATH "\$ORIGIN" SRC ${_fx}/tstring_fixture_consumer.cpp)
# The thunk translation unit and the hook backend are built without exceptions (as on Android); everything else, including the
# test, with them.
nevr_ht_obj(o_redirect_tstring_thunks GROUP redirect HOST NOEXC INCS ${_sent} SRC src/quest/redirect/tstring_thunks.cpp)
foreach(f got_hook hook_log hook_report)
  nevr_ht_obj(o_redirect_${f} GROUP redirect HOST NOEXC INCS ${_sent} SRC src/quest/sentinel/${f}.cpp)
endforeach()
# The patched libr15's identity (sentinel::pinned::LibR15PatchedBuildId, docs/adr/0021 section 7): tstring_thunks.cpp calls the
# pinned factories, so the link takes the three objects test-quest-hooks takes, built as on the device.
nevr_ht_obj(o_redirect_id_libr15_identity GROUP redirect HOST NOEXC INCS ${_sent} SRC src/quest/sentinel/libr15_identity.cpp)
nevr_ht_obj(o_redirect_id_libr15_patch_identity GROUP redirect HOST NOEXC INCS ${_sent} SRC src/quest/patch/libr15_patch_identity.cpp)
nevr_ht_obj(o_redirect_id_sha256 GROUP redirect HOST NOEXC INCS ${_sent} SRC src/quest/data/sha256.cpp)
# The thunk's frames must sit under the personality-free CIE: no "zPLR" augmentation, at least one "zR".
nevr_ht_check(tstring_thunks_frames GROUP redirect TIMEOUT 60
  SRC ${CMAKE_COMMAND} -DREADELF=${NEVR_READELF} -DOBJECT=$<TARGET_OBJECTS:o_redirect_tstring_thunks>
  -P ${NEVR_HT_DIR}/scripts/check_frames.cmake)

set(_redirect_srcs src/quest/redirect/tests/redirect_test.cpp src/quest/redirect/service_redirector.cpp
    src/quest/redirect/hook_adapter.cpp src/quest/sentinel/quest_config.cpp src/core/nevr_config.cpp
    src/runtime/lifecycle/service_redirect.cpp src/runtime/lifecycle/stable_string_pool.cpp)
set(_redirect_objs o_redirect_tstring_thunks o_redirect_got_hook o_redirect_hook_log o_redirect_hook_report
    o_redirect_id_libr15_identity o_redirect_id_libr15_patch_identity o_redirect_id_sha256)
nevr_ht_test(redirect_test GROUP redirect HOST DL PTHREAD INCS ${_sent} ARGS "${CMAKE_BINARY_DIR}/redirect"
  DEPENDS redirect_fx_consumer_a redirect_fx_consumer_b LIBS ${NEVR_YAML_LIB}
  SRC ${_redirect_srcs} OBJS ${_redirect_objs})
# The same test under ThreadSanitizer at -O1: the cache lock is only observable as a data race, and -O1 also exercises the
# warnings that -O0 hides. The thunk TU and backend stay uninstrumented objects (they hold no shared state beyond atomics).
nevr_ht_test(redirect_test_tsan GROUP redirect HOST DL PTHREAD TSAN INCS ${_sent} ARGS "${CMAKE_BINARY_DIR}/redirect"
  ENV "TSAN_OPTIONS=halt_on_error=1 exitcode=66"
  DEPENDS redirect_fx_consumer_a redirect_fx_consumer_b LIBS ${NEVR_YAML_LIB}
  SRC ${_redirect_srcs} OBJS ${_redirect_objs})
