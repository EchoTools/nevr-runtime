# The map scripts on Quest (docs/adr/0048 decision 6): the ELF rules, the map-script table, and Register against a temporary
# library directory, and that the stock redirect's answers do not change. The table's claim order is the shared one
# (src/core/map_script_check.h), which the PC provider applies too.
set(_maps_sent src/quest/sentinel)

nevr_ht_test(map_scripts_test GROUP shared NOEXC PTHREAD DL INCS ${_maps_sent} TIMEOUT 120
  SRC src/quest/tests/map_scripts_test.cpp src/quest/plugin/script_redirect.cpp src/quest/plugin/map_scripts.cpp
      src/core/script_image_rules_elf.cpp src/quest/sentinel/hook_log.cpp src/quest/data/sha256.cpp)
