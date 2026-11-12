# CDN_DESIGN Q3: the ELF rules, the map-script table and Register (map_scripts_test.cpp). Q5's hook needs the GOT hook and
# the frame dispatcher; its checks run at integration (Q3Q5.md).
nevr_ht_test(map_scripts_test GROUP hooks NOEXC PTHREAD DL INCS ${_sent} TIMEOUT 120
  SRC src/quest/tests/map_scripts_test.cpp src/quest/plugin/script_redirect.cpp src/quest/plugin/map_scripts.cpp
      src/core/script_image_rules_elf.cpp src/quest/sentinel/hook_log.cpp src/quest/data/sha256.cpp)
# CDN_DESIGN Q2d: a Quest map's scripts, set and aliases granted together or not at all, and the mount observer's withdraw and arm.
nevr_ht_test(map_mount_test GROUP data HOST NOEXC PTHREAD DL INCS ${_sent} sdk/include TIMEOUT 120 ENV ${_data_env}
  SRC src/quest/tests/map_mount_test.cpp src/quest/cargo/map_mount.cpp src/quest/plugin/script_redirect.cpp
      src/quest/plugin/map_scripts.cpp src/core/script_image_rules_elf.cpp src/core/config_alias.cpp
  OBJS o_maps_registry o_maps_map_sets o_cargo_resource_shadow o_cargo_hook_log o_data_sha256 LIBS ${NEVR_ZSTD_LIB})
