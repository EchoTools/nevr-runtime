# CDN_DESIGN Q3 (docs/adr/0047): map scripts, route B. The table lives in plugin/script_redirect.cpp (already in
# nevr_quest_plugin); Register's file checks and the ELF rules are added here. Included from integration/CMakeLists.txt.
target_sources(nevr_quest_plugin PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/../plugin/map_scripts.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/../../core/script_image_rules_elf.cpp")
