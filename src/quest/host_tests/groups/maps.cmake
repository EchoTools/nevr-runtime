# The map scripts on Quest (docs/adr/0048 decision 6): the ELF rules, the map-script table, and Register against a temporary
# library directory, and that the stock redirect's answers do not change. The table's claim order is the shared one
# (src/core/map_script_check.h), which the PC provider applies too.
set(_maps_sent src/quest/sentinel)

nevr_ht_test(map_scripts_test GROUP shared NOEXC PTHREAD DL INCS ${_maps_sent} TIMEOUT 120
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
  src/core/cargo_validate.cpp
  src/core/cargo_limits.cpp
  src/core/device_memory.cpp
  src/quest/data/sha256.cpp)
nevr_ht_obj(o_map_monocypher GROUP shared COPTS -w
  SRC third_party/monocypher/src/monocypher.c third_party/monocypher/src/monocypher-ed25519.c)
nevr_ht_test(map_mount_rules_test GROUP shared HOST PTHREAD TIMEOUT 120
  INCS src/quest/tests third_party/monocypher/src OBJS o_map_monocypher
  SRC src/quest/tests/map_mount_rules_test.cpp ${_maps_mount_core})
