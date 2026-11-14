# test-quest-data and test-quest-data-net: the first-run data step's core (src/quest/data) on the host, and the fetch over real
# HTTP (docs/adr/0011). zlib is the host tree's libz.a.

# ── data ────────────────────────────────────────────────────────────────────────────────────────────────────────────────
nevr_ht_group(data)
nevr_ht_obj(o_cdn_device_memory GROUP data NOEXC HOST SRC src/core/device_memory.cpp)
foreach(f sha256 zip staging fetch data_step pack boot_stub)
  nevr_ht_obj(o_data_${f} GROUP data NOEXC HOST SRC src/quest/data/${f}.cpp)
endforeach()
# The tests' scratch directories are made in the build tree (the recipes run from different directories).
set(_data_env NEVR_TEST_TMP=${CMAKE_BINARY_DIR}/data)
nevr_ht_test(data_test GROUP data HOST TIMEOUT 300 ENV ${_data_env}
  SRC src/quest/tests/data_test.cpp OBJS o_data_sha256 o_data_zip o_data_staging LIBS ${NEVR_ZLIB_LIB})
nevr_ht_test(fetch_test GROUP data HOST TIMEOUT 300 ENV ${_data_env}
  SRC src/quest/tests/fetch_test.cpp OBJS o_data_sha256 o_data_zip o_data_staging o_data_fetch LIBS ${NEVR_ZLIB_LIB})
nevr_ht_test(data_step_test GROUP data HOST TIMEOUT 300 ENV ${_data_env}
  SRC src/quest/tests/data_step_test.cpp OBJS o_data_sha256 o_data_zip o_data_staging o_data_fetch o_data_data_step o_data_boot_stub o_cdn_device_memory LIBS ${NEVR_ZLIB_LIB})
# The zero-package stub manifests (src/quest/data/boot_stub.h), read back by the PC server's own manifest reader and by libzstd.
nevr_ht_obj(o_data_installed_levels GROUP data HOST INCS sdk/include SRC src/runtime/server/installed_levels.cpp)
nevr_ht_test(boot_stub_test GROUP data HOST INCS sdk/include TIMEOUT 300 ENV ${_data_env}
  SRC src/quest/tests/boot_stub_test.cpp
  OBJS o_data_sha256 o_data_zip o_data_staging o_data_fetch o_data_data_step o_data_boot_stub o_cdn_device_memory o_data_installed_levels
  LIBS ${NEVR_ZLIB_LIB} ${NEVR_ZSTD_LIB})
nevr_ht_test(pack_test GROUP data HOST TIMEOUT 300 ENV ${_data_env}
  SRC src/quest/tests/pack_test.cpp OBJS o_data_sha256 o_data_zip o_data_staging o_data_fetch o_data_pack LIBS ${NEVR_ZLIB_LIB})
# The CDN texture delivery (docs/adr/0045-cdn-texture-delivery.md): the shared pure core in src/cdn (and the shadow builder in src/core), built here as the sentinel builds
# it (no exceptions; nlohmann::json is used through its non-throwing calls only).
nevr_ht_obj(o_cdn_resource_shadow GROUP data NOEXC HOST SRC src/core/resource_shadow.cpp)
foreach(f cdn_limits cdn_validate cdn_manifest cdn_plan cdn_store cdn_run cdn_resolver cdn_service)
  nevr_ht_obj(o_cdn_${f} GROUP data NOEXC HOST SRC src/cdn/${f}.cpp)
endforeach()
nevr_ht_test(resource_shadow_test GROUP data HOST NOEXC TIMEOUT 120
  SRC src/quest/tests/resource_shadow_test.cpp OBJS o_cdn_resource_shadow)
# The first-run prefetch (docs/adr/0045-cdn-texture-delivery.md, the first-run hold): every object of a manifest, before the engine. cdn_run.cpp
# drives it, so every target that links cdn_run links it.
nevr_ht_obj(o_cdn_cdn_prefetch GROUP data NOEXC HOST SRC src/cdn/cdn_prefetch.cpp)
set(_cdn_objs o_cdn_device_memory o_cdn_resource_shadow o_cdn_cdn_limits o_cdn_cdn_validate o_cdn_cdn_manifest o_cdn_cdn_plan o_cdn_cdn_store o_cdn_cdn_run o_cdn_cdn_prefetch o_data_sha256)
nevr_ht_test(device_memory_test GROUP data HOST TIMEOUT 120
  SRC src/quest/tests/device_memory_test.cpp OBJS o_cdn_device_memory)
nevr_ht_test(cdn_limits_test GROUP data HOST TIMEOUT 120
  SRC src/quest/tests/cdn_limits_test.cpp OBJS ${_cdn_objs} LIBS ${NEVR_ZSTD_LIB})
nevr_ht_test(cdn_validate_test GROUP data HOST TIMEOUT 120
  SRC src/quest/tests/cdn_validate_test.cpp OBJS ${_cdn_objs} LIBS ${NEVR_ZSTD_LIB})
nevr_ht_test(cdn_manifest_test GROUP data HOST TIMEOUT 120
  SRC src/quest/tests/cdn_manifest_test.cpp OBJS ${_cdn_objs} LIBS ${NEVR_ZSTD_LIB})
nevr_ht_test(sha256_backend_test GROUP data HOST TIMEOUT 120
  SRC src/quest/tests/sha256_backend_test.cpp OBJS o_data_sha256)
nevr_ht_test(cdn_store_test GROUP data HOST TIMEOUT 120
  SRC src/quest/tests/cdn_store_test.cpp OBJS ${_cdn_objs} LIBS ${NEVR_ZSTD_LIB})
nevr_ht_test(cdn_run_test GROUP data HOST TIMEOUT 300
  SRC src/quest/tests/cdn_run_test.cpp OBJS ${_cdn_objs} LIBS ${NEVR_ZSTD_LIB})
nevr_ht_test(cdn_scale_test GROUP data HOST TIMEOUT 600
  SRC src/quest/tests/cdn_scale_test.cpp OBJS ${_cdn_objs} LIBS ${NEVR_ZSTD_LIB})
nevr_ht_test(cdn_prefetch_test GROUP data HOST TIMEOUT 120
  SRC src/quest/tests/cdn_prefetch_test.cpp OBJS ${_cdn_objs} LIBS ${NEVR_ZSTD_LIB})
# The Quest side: the POSIX env on real files, the settings, and the worker end to end over a scripted transport. quest_config.h is read
# for the settings struct only (nothing of quest_config.cpp is linked).
nevr_ht_obj(o_cdn_env_posix GROUP data NOEXC HOST INCS src/quest/sentinel SRC src/cdn/env_posix.cpp)
nevr_ht_obj(o_cdn_worker GROUP data NOEXC HOST INCS src/quest/sentinel SRC src/quest/cdn/cdn_worker.cpp)
nevr_ht_obj(o_cdn_hook_log GROUP data NOEXC HOST INCS src/quest/sentinel SRC src/quest/sentinel/hook_log.cpp)
nevr_ht_obj(o_cdn_atomic_write GROUP data NOEXC HOST SRC src/quest/auth/atomic_write.cpp)
nevr_ht_test(cdn_service_test GROUP data HOST INCS src/quest/sentinel TIMEOUT 120 ENV ${_data_env}
  SRC src/quest/tests/cdn_service_test.cpp
  OBJS ${_cdn_objs} o_cdn_env_posix o_cdn_cdn_service o_cdn_worker o_cdn_hook_log o_cdn_atomic_write o_data_staging
  LIBS ${NEVR_ZLIB_LIB} ${NEVR_ZSTD_LIB})
# The whole-game launch (#2398): the data step in its all-assets mode, with the worker the sentinel sets, over a scripted transport.
nevr_ht_test(whole_game_launch_test GROUP data HOST INCS src/quest/sentinel TIMEOUT 300 ENV ${_data_env}
  SRC src/quest/tests/whole_game_launch_test.cpp
  OBJS ${_cdn_objs} o_cdn_env_posix o_cdn_cdn_service o_cdn_worker o_cdn_hook_log o_cdn_atomic_write o_data_staging o_data_zip o_data_fetch o_data_data_step o_data_boot_stub
  LIBS ${NEVR_ZLIB_LIB} ${NEVR_ZSTD_LIB})
nevr_ht_test(cdn_resolver_test GROUP data HOST PTHREAD TIMEOUT 120
  SRC src/quest/tests/cdn_resolver_test.cpp OBJS o_cdn_resource_shadow o_cdn_cdn_resolver)

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
      src/quest/data/data_step.cpp src/quest/data/boot_stub.cpp src/core/device_memory.cpp src/quest/data/pack.cpp src/quest/data/curl_transport.cpp src/quest/data/data_host_roots.cpp
  LIBS ${NEVR_ZLIB_LIB} ssl crypto)
