# Combat behaviour the Quest sentinel changes in the game (#1737 plan 6a Q3): one file per behaviour, namespace
# nevr_quest::combat::<module>, log events combat_<module>_*, host tests in host_tests/groups/combat.cmake (`just test-quest-combat`). Included from
# src/quest/CMakeLists.txt after the sentinel, whose GOT backend it hooks through (nevr_quest_got_hook).
#
# Every file here runs on the hook path: it includes sentinel/callback_thunk.h, so it is built without exceptions (the contract
# in that header), with hidden visibility (nothing here may become an export, tests/quest TestExportAllowlist) and the
# warnings-as-errors flags its siblings use. The sentinel links this target; --exclude-libs keeps it out of the export set.
add_library(nevr_quest_combat STATIC
  aibots.cpp
  bvh.cpp
  combat_active.cpp
  combat_install.cpp
  combat_level.cpp
  combat_tick.cpp
  combat_level.cpp
  combat_tick.cpp
  decals.cpp
  dronekill.cpp
  dronemuzzle.cpp
  ec.cpp
  ecfx.cpp
  equip_refire.cpp
  gamespace.cpp
  gunconnect.cpp
  headset_gates.cpp
  heaps.cpp
  impactsnd.cpp
  instmodel.cpp
  match_tags.cpp
  memsys.cpp
  meshstep.cpp
  music.cpp
  pfx.cpp
  pfxtev.cpp
  powerboost.cpp
  refresh_rate.cpp
  sendguard.cpp
  sensorwd.cpp
  shaderstep.cpp
  table_step.cpp
  tablestep.cpp
  wallfire.cpp
  weapon_arm.cpp)
target_include_directories(nevr_quest_combat PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_compile_options(nevr_quest_combat PRIVATE
  -Wall -Wextra -Werror -fno-exceptions -fvisibility=hidden -fvisibility-inlines-hidden)
target_link_libraries(nevr_quest_combat PUBLIC nevr_quest_got_hook nevr::plugin_sdk nlohmann_json::nlohmann_json)  # nlohmann: runtime/compat/server_features.h (#2333)
