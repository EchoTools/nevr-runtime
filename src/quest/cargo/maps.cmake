# CDN_DESIGN Q1 and Q2: the downloaded maps on the Quest. nevr_quest_maps holds the shared registry (core/resource_set_registry.cpp),
# the POSIX set registration (sentinel/map_sets.cpp), PC4/PC5a's channel core, PC10's recipe rebuild over the Quest's stock source,
# the session pin check and the Quest's service over them. It is linked into libovrplatformloader.so: production_steps.cpp calls
# nevr_quest::maps::StartFromConfig, which returns at once and does nothing unless the Quest config gives maps keys.
# map_mount.cpp calls route B's map_scripts (nevr_quest_plugin) and the ConfigAlias core and hook (q5.cmake's ovrplatformloader
# sources): both reach the loader's link, the plugin through nevr_quest_integration.
set(_qmaps_monocypher "${NEVR_REPO_ROOT}/third_party/monocypher/src")
set(_qmaps_monocypher_srcs "${_qmaps_monocypher}/monocypher.c" "${_qmaps_monocypher}/monocypher-ed25519.c")
set_source_files_properties(${_qmaps_monocypher_srcs} PROPERTIES COMPILE_OPTIONS "-w")
add_library(nevr_quest_maps STATIC
  ../../core/resource_set_registry.cpp
  map_sets.cpp
  ../../core/map_manifest.cpp
  ../../core/map_index.cpp
  ../../core/map_run.cpp
  ../../core/minisign_verify.cpp
  ../cargo/posix_map_fs.cpp
  ../cargo/map_service.cpp
  ../cargo/map_mount.cpp
  ../../core/map_derive.cpp
  ../cargo/stock_source_posix.cpp
  ../cargo/map_pin.cpp
  ../cargo/map_pin_game.cpp
  ../cargo/map_start.cpp
  ${_qmaps_monocypher_srcs})
target_include_directories(nevr_quest_maps PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../..
  ${CMAKE_BINARY_DIR}
  ${_qmaps_monocypher})
target_compile_options(nevr_quest_maps PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Werror -fno-exceptions -fvisibility=hidden -fvisibility-inlines-hidden>)
target_link_libraries(nevr_quest_maps PUBLIC nevr_quest_got_hook nevr_quest_cargo nevr_quest_data nevr_quest_data_net
  nevr::plugin_sdk nlohmann_json::nlohmann_json PRIVATE zstd::libzstd)
target_link_libraries(ovrplatformloader PRIVATE nevr_quest_maps)
