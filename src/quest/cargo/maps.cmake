# The custom-map channel on the Quest (docs/adr/0048-custom-maps-channel.md): the core's start and fetch (core/map_channel.h,
# core/map_run.cpp and their formats), the shared resource-set registry the PC also compiles (core/resource_set_registry.cpp), the
# script image rules, the Quest's map file system and set registration (sentinel/map_sets.cpp), route B's map scripts
# (plugin/map_scripts.cpp), and the Quest's service, mount and production start (cargo/map_*.cpp).
# It is linked into libovrplatformloader.so: production_steps.cpp calls nevr_quest::maps::StartFromConfig, which returns at once and
# writes one Info record unless the Quest config gives maps.base_url. Included from src/quest/sentinel/CMakeLists.txt.
set(_qmaps_monocypher "${NEVR_REPO_ROOT}/third_party/monocypher/src")
# Vendored unmodified (third_party/monocypher/UPSTREAM.txt): its warnings are upstream's, so they are off for these two C files only.
set_source_files_properties("${_qmaps_monocypher}/monocypher.c" "${_qmaps_monocypher}/monocypher-ed25519.c"
  PROPERTIES COMPILE_OPTIONS "-w")
add_library(nevr_quest_maps STATIC
  ../../core/map_channel.cpp
  ../../core/map_run.cpp
  ../../core/map_manifest.cpp
  ../../core/map_index.cpp
  ../../core/map_derive.cpp
  ../../core/map_set_tables.cpp
  ../../core/minisign_verify.cpp
  ../../core/resource_set_registry.cpp
  ../../core/script_image_rules.cpp
  ../../core/script_image_rules_elf.cpp
  ../../core/device_memory.cpp
  ../sentinel/map_sets.cpp
  ../plugin/map_scripts.cpp
  posix_map_fs.cpp
  stock_source.cpp
  map_service.cpp
  map_mount.cpp
  map_start.cpp
  "${_qmaps_monocypher}/monocypher.c"
  "${_qmaps_monocypher}/monocypher-ed25519.c")
target_include_directories(nevr_quest_maps PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../.."
  "${CMAKE_CURRENT_SOURCE_DIR}/../sentinel"
  "${_qmaps_monocypher}")
target_compile_options(nevr_quest_maps PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Werror -fno-exceptions -fvisibility=hidden -fvisibility-inlines-hidden>)
target_link_libraries(nevr_quest_maps PUBLIC nevr_quest_cargo nevr_quest_plugin nevr_quest_config_alias nevr::plugin_sdk
  nlohmann_json::nlohmann_json)
target_link_libraries(ovrplatformloader PRIVATE nevr_quest_maps)
