#include "quest/tests/map_service_stubs.h"

#include "quest/cdn/map_pin_game.h"
#include "quest/sentinel/resource_request_hook.h"

namespace map_service_stubs {

State& Get() {
  static State instance;
  return instance;
}

}  // namespace map_service_stubs

namespace nevr_quest::maps::pin {

bool InstallTick() noexcept {
  map_service_stubs::State& state = map_service_stubs::Get();
  state.install_tick_calls.fetch_add(1);
  if (state.on_install_tick) state.on_install_tick();
  return true;
}

std::uint64_t MainLevelNow() noexcept { return 0; }

}  // namespace nevr_quest::maps::pin

namespace nevr_quest::resource_request {

void PublishMapSets(const nevr_shadow::Shadow* shadow) noexcept {
  if (shadow != nullptr) map_service_stubs::Get().published_shadows.fetch_add(1);
}

std::uint64_t GameManifestRecords() noexcept {
  map_service_stubs::Get().manifest_records_calls.fetch_add(1);
  return map_service_stubs::Get().game_manifest_records.load();
}

bool GameManifestHolds(std::uint64_t, std::uint64_t) noexcept {
  map_service_stubs::Get().manifest_has_calls.fetch_add(1);
  return map_service_stubs::Get().manifest_holds.load();
}

}  // namespace nevr_quest::resource_request

// The alias table's Warning lines go to the sentinel's log on the Quest (quest/sentinel/config_alias_levels.cpp); the service tests read none.
namespace nevr_config_alias {
void PlatformWarn(const char*) noexcept {}

// The lookup hook goes in only when something is staged (quest/sentinel/config_alias_hook.cpp): the stand-in counts the asks.
bool InstallIfStaged() {
  map_service_stubs::Get().alias_install_calls.fetch_add(1);
  return false;
}
}  // namespace nevr_config_alias
