# test-quest-data and test-quest-data-net: the first-run data step's core (src/quest/data) on the host, and the fetch over real
# HTTP (docs/adr/0011). zlib is the host tree's libz.a.

# ── data ────────────────────────────────────────────────────────────────────────────────────────────────────────────────
nevr_ht_group(data)
foreach(f sha256 zip staging fetch data_step pack)
  nevr_ht_obj(o_data_${f} GROUP data NOEXC HOST SRC src/quest/data/${f}.cpp)
endforeach()
# The tests' scratch directories are made in the build tree (the recipes run from different directories).
set(_data_env NEVR_TEST_TMP=${CMAKE_BINARY_DIR}/data)
nevr_ht_test(data_test GROUP data HOST TIMEOUT 300 ENV ${_data_env}
  SRC src/quest/tests/data_test.cpp OBJS o_data_sha256 o_data_zip o_data_staging LIBS ${NEVR_ZLIB_LIB})
nevr_ht_test(fetch_test GROUP data HOST TIMEOUT 300 ENV ${_data_env}
  SRC src/quest/tests/fetch_test.cpp OBJS o_data_sha256 o_data_zip o_data_staging o_data_fetch LIBS ${NEVR_ZLIB_LIB})
nevr_ht_test(data_step_test GROUP data HOST TIMEOUT 300 ENV ${_data_env}
  SRC src/quest/tests/data_step_test.cpp OBJS o_data_sha256 o_data_zip o_data_staging o_data_fetch o_data_data_step LIBS ${NEVR_ZLIB_LIB})
nevr_ht_test(pack_test GROUP data HOST TIMEOUT 300 ENV ${_data_env}
  SRC src/quest/tests/pack_test.cpp OBJS o_data_sha256 o_data_zip o_data_staging o_data_fetch o_data_pack LIBS ${NEVR_ZLIB_LIB})

# ── data-net ────────────────────────────────────────────────────────────────────────────────────────────────────────────
# curl_transport_test needs a throwaway CA and six servers, which the recipe starts; data_wiring_test runs after it. Both are
# built here and run by the recipe by name, so neither is a ctest test.
nevr_ht_group(data-net)
nevr_ht_test(curl_transport_test GROUP data-net HOST CURL PTHREAD NO_TEST
  SRC src/quest/data/sha256.cpp src/quest/data/zip.cpp src/quest/data/staging.cpp src/quest/data/fetch.cpp
      src/quest/data/curl_transport.cpp src/quest/data/data_host_roots.cpp src/quest/tests/curl_transport_test.cpp
  LIBS ${NEVR_ZLIB_LIB} ssl crypto)
# The step as the game reaches it: through the permission-wait handler's thunk entry (src/quest/sentinel/data_wiring.cpp). The
# hooks it links call the pinned factories, which take the patched libr15's identity (docs/adr/0021 section 7).
nevr_ht_test(data_wiring_test GROUP data-net HOST CURL PTHREAD NOEXC DL NO_TEST INCS src/quest/sentinel
  SRC src/quest/tests/data_wiring_test.cpp src/quest/sentinel/data_wiring.cpp
      src/quest/sentinel/obb_skip_hook.cpp src/quest/sentinel/permission_wait_hook.cpp src/quest/sentinel/process_end_hook.cpp
      src/quest/sentinel/got_hook.cpp src/quest/sentinel/hook_log.cpp src/quest/sentinel/hook_report.cpp
      src/quest/sentinel/libr15_identity.cpp src/quest/patch/libr15_patch_identity.cpp
      src/quest/data/sha256.cpp src/quest/data/zip.cpp src/quest/data/staging.cpp src/quest/data/fetch.cpp
      src/quest/data/data_step.cpp src/quest/data/pack.cpp src/quest/data/curl_transport.cpp src/quest/data/data_host_roots.cpp
  LIBS ${NEVR_ZLIB_LIB} ssl crypto)
