# The map scripts on Quest (docs/adr/0048 decision 6): the ELF rules, the map-script table, and Register against a temporary
# library directory, and that the stock redirect's answers do not change. The table's claim order is the shared one
# (src/core/map_script_check.h), which the PC provider applies too.
set(_maps_sent src/quest/sentinel)

nevr_ht_test(map_scripts_test GROUP shared NOEXC PTHREAD DL INCS ${_maps_sent}
  SRC src/quest/tests/map_scripts_test.cpp src/quest/plugin/script_redirect.cpp src/quest/plugin/map_scripts.cpp
      src/core/script_image_rules_elf.cpp src/quest/sentinel/hook_log.cpp src/quest/data/sha256.cpp)

# The map channel's mount and fetch rules (docs/adr/0048 decisions 3, 8 and 10) on the Quest host: the shared core the PC compiles
# (core/map_run.cpp and the format, index, verifier and derive sources it calls), driven through CacheMount and FetchGroup by the
# shared fixture (quest/tests/map_run_fixture.h) over the fake env and map file system. The vendored monocypher C files are an
# object of their own with -w, as maps.cmake builds them for nevr_core: their warnings are upstream's.
set(_maps_mount_core
  src/core/map_run.cpp
  src/core/map_local_precedence.cpp
  src/core/map_manifest.cpp
  src/core/map_index.cpp
  src/core/minisign_verify.cpp
  src/core/map_set_tables.cpp
  src/core/map_derive.cpp
  src/core/script_image_rules.cpp
  src/core/base64.cpp
  src/cdn/cdn_validate.cpp
  src/cdn/cdn_limits.cpp
  src/core/device_memory.cpp
  src/quest/data/sha256.cpp)
nevr_ht_obj(o_map_monocypher GROUP shared COPTS -w
  SRC third_party/monocypher/src/monocypher.c third_party/monocypher/src/monocypher-ed25519.c)
nevr_ht_test(map_mount_rules_test GROUP shared HOST PTHREAD
  INCS src/quest/tests third_party/monocypher/src OBJS o_map_monocypher
  SRC src/quest/tests/map_mount_rules_test.cpp ${_maps_mount_core})

# The dedicated server's maps hold, pure (core/map_server_hold.h; ADR 0048 decisions 13, 17 and 18): the timeout's size, rate and
# bounds, the one group fetched, the hold lifted by the mount or the deadline, the join composition and the signed-only rule.
nevr_ht_test(map_server_hold_test GROUP shared NOEXC INCS src
  SRC src/core/tests/map_server_hold_test.cpp src/core/map_server_hold.cpp)

# The custom-map channel's start (core/map_channel.h; ADR 0048 decisions 3, 7, 8, 12): the settings, the start decision, the boot's
# no-network and no-hash rule, its idempotence, the worker's group plan and the worker's request gap. It links the core fetch and mount
# (core/map_run.cpp) and their formats, over the shared fake Env and MapFs. Monocypher is the vendored one (UPSTREAM.txt): its warnings
# are upstream's, so -w for those two C files only.
set(_maps_channel_monocypher "${NEVR_REPO_ROOT}/third_party/monocypher/src")
set_source_files_properties("${_maps_channel_monocypher}/monocypher.c" "${_maps_channel_monocypher}/monocypher-ed25519.c"
  PROPERTIES COMPILE_OPTIONS "-w")
nevr_ht_test(map_channel_test GROUP shared NOEXC PTHREAD INCS sdk/include src third_party/monocypher/src
  SRC src/core/tests/map_channel_test.cpp src/core/map_channel.cpp src/core/map_request.cpp src/core/map_fetch_queue.cpp src/core/map_session_pin.cpp src/core/map_run.cpp src/core/map_local_precedence.cpp src/core/map_manifest.cpp
      src/core/map_index.cpp src/core/minisign_verify.cpp src/core/map_derive.cpp src/core/map_set_tables.cpp
      src/cdn/cdn_validate.cpp src/cdn/cdn_limits.cpp src/core/device_memory.cpp src/quest/data/sha256.cpp
      src/core/script_image_rules.cpp src/core/script_image_rules_elf.cpp src/core/config_alias.cpp
      third_party/monocypher/src/monocypher.c third_party/monocypher/src/monocypher-ed25519.c)

# The session request pass the Quest runs (core/map_request_serve.h, shared with the PC; ADR 0048 decision 16): a queued group is fetched on demand and then
# listed, a requested map is fetched alone, and a stopping worker serves nothing. Over the core's request queue and the fake Env.
nevr_ht_test(map_request_drain_test GROUP shared NOEXC PTHREAD INCS src third_party/monocypher/src OBJS o_map_monocypher
  SRC src/quest/tests/map_request_drain_test.cpp src/core/map_request.cpp src/core/map_fetch_queue.cpp src/core/map_session_pin.cpp
      ${_maps_mount_core})

# The channel's listing (core/map_channel.h ListGroups; K6): a map a request fetched after the boot is listed as not_asked and is never
# granted to the mounter. Over the map run fixture, which signs the group and fakes the cache.
nevr_ht_test(map_list_groups_test GROUP shared NOEXC PTHREAD INCS src third_party/monocypher/src OBJS o_map_monocypher
  SRC src/quest/tests/map_list_groups_test.cpp src/core/map_channel.cpp src/core/map_local_precedence.cpp src/core/config_alias.cpp
      ${_maps_mount_core})
