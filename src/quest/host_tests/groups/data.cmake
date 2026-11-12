# test-quest-data and test-quest-data-net: the first-run data step's core (src/quest/data) on the host, and the fetch over real
# HTTP (docs/adr/0011). zlib is the host tree's libz.a.

# ── data ────────────────────────────────────────────────────────────────────────────────────────────────────────────────
nevr_ht_group(data)
nevr_ht_obj(o_cargo_device_memory GROUP data NOEXC HOST SRC src/core/device_memory.cpp)
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
  SRC src/quest/tests/data_step_test.cpp OBJS o_data_sha256 o_data_zip o_data_staging o_data_fetch o_data_data_step o_cargo_device_memory LIBS ${NEVR_ZLIB_LIB})
nevr_ht_test(pack_test GROUP data HOST TIMEOUT 300 ENV ${_data_env}
  SRC src/quest/tests/pack_test.cpp OBJS o_data_sha256 o_data_zip o_data_staging o_data_fetch o_data_pack LIBS ${NEVR_ZLIB_LIB})
# The CDN texture delivery (docs/adr/0045-cdn-texture-delivery.md): the shared pure core in src/core, built here as the sentinel builds
# it (no exceptions; nlohmann::json is used through its non-throwing calls only).
foreach(f resource_shadow cargo_limits cargo_validate cargo_manifest cargo_plan cargo_store cargo_run cargo_resolver)
  nevr_ht_obj(o_cargo_${f} GROUP data NOEXC HOST SRC src/core/${f}.cpp)
endforeach()
nevr_ht_test(resource_shadow_test GROUP data HOST NOEXC TIMEOUT 120
  SRC src/quest/tests/resource_shadow_test.cpp OBJS o_cargo_resource_shadow)
set(_cargo_objs o_cargo_device_memory o_cargo_resource_shadow o_cargo_cargo_limits o_cargo_cargo_validate o_cargo_cargo_manifest o_cargo_cargo_plan o_cargo_cargo_store o_cargo_cargo_run o_data_sha256)
nevr_ht_test(device_memory_test GROUP data HOST TIMEOUT 120
  SRC src/quest/tests/device_memory_test.cpp OBJS o_cargo_device_memory)
nevr_ht_test(cargo_limits_test GROUP data HOST TIMEOUT 120
  SRC src/quest/tests/cargo_limits_test.cpp OBJS ${_cargo_objs} LIBS ${NEVR_ZSTD_LIB})
nevr_ht_test(cargo_validate_test GROUP data HOST TIMEOUT 120
  SRC src/quest/tests/cargo_validate_test.cpp OBJS ${_cargo_objs} LIBS ${NEVR_ZSTD_LIB})
nevr_ht_test(cargo_manifest_test GROUP data HOST TIMEOUT 120
  SRC src/quest/tests/cargo_manifest_test.cpp OBJS ${_cargo_objs} LIBS ${NEVR_ZSTD_LIB})
nevr_ht_test(sha256_backend_test GROUP data HOST TIMEOUT 120
  SRC src/quest/tests/sha256_backend_test.cpp OBJS o_data_sha256)
nevr_ht_test(cargo_store_test GROUP data HOST TIMEOUT 120
  SRC src/quest/tests/cargo_store_test.cpp OBJS ${_cargo_objs} LIBS ${NEVR_ZSTD_LIB})
nevr_ht_test(cargo_run_test GROUP data HOST TIMEOUT 300
  SRC src/quest/tests/cargo_run_test.cpp OBJS ${_cargo_objs} LIBS ${NEVR_ZSTD_LIB})
nevr_ht_test(cargo_scale_test GROUP data HOST TIMEOUT 600
  SRC src/quest/tests/cargo_scale_test.cpp OBJS ${_cargo_objs} LIBS ${NEVR_ZSTD_LIB})
# The Quest side: the POSIX env on real files, the settings, and the worker end to end over a scripted transport. quest_config.h is read
# for the settings struct only (nothing of quest_config.cpp is linked).
nevr_ht_obj(o_cargo_posix_env GROUP data NOEXC HOST INCS src/quest/sentinel SRC src/quest/cargo/posix_env.cpp)
nevr_ht_obj(o_cargo_service GROUP data NOEXC HOST INCS src/quest/sentinel SRC src/quest/cargo/cargo_service.cpp)
nevr_ht_obj(o_cargo_hook_log GROUP data NOEXC HOST INCS src/quest/sentinel SRC src/quest/sentinel/hook_log.cpp)
nevr_ht_obj(o_cargo_atomic_write GROUP data NOEXC HOST SRC src/quest/auth/atomic_write.cpp)
nevr_ht_test(cargo_service_test GROUP data HOST INCS src/quest/sentinel TIMEOUT 120 ENV ${_data_env}
  SRC src/quest/tests/cargo_service_test.cpp
  OBJS ${_cargo_objs} o_cargo_posix_env o_cargo_service o_cargo_hook_log o_cargo_atomic_write o_data_staging
  LIBS ${NEVR_ZLIB_LIB} ${NEVR_ZSTD_LIB})
nevr_ht_test(cargo_resolver_test GROUP data HOST PTHREAD TIMEOUT 120
  SRC src/quest/tests/cargo_resolver_test.cpp OBJS o_cargo_resource_shadow o_cargo_cargo_resolver)

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
      src/quest/data/data_step.cpp src/core/device_memory.cpp src/quest/data/pack.cpp src/quest/data/curl_transport.cpp src/quest/data/data_host_roots.cpp
  LIBS ${NEVR_ZLIB_LIB} ssl crypto)
