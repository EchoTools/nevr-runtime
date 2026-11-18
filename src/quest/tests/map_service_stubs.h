// The seams of the Quest's map service that only the headset has (quest/cdn/map_service.cpp calls them): the libr15 half of the session
// pin and the resource request hook's view of the game's manifest. The host tests link these stand-ins instead of the real files, which
// need the sentinel's libr15 image; each stand-in counts its calls so a test sees what the service asked for and in which order.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace map_service_stubs {

// What the stand-ins were asked, and what they answer.
struct State {
  std::atomic<int> install_tick_calls{0};
  std::atomic<std::uint64_t> game_manifest_records{0};  // GameManifestRecords's answer: 0 until the game's manifest is built
  std::atomic<int> manifest_records_calls{0};           // how often the service looked for the manifest
  std::atomic<int> manifest_has_calls{0};
  std::atomic<bool> manifest_holds{false};  // GameManifestHolds's answer for every key
  std::atomic<int> alias_install_calls{0};  // InstallIfStaged calls: the alias lookup hook's install, which only the headset has
  std::atomic<int> published_shadows{0};    // PublishMapSets calls with a shadow
  // Called from InstallTick with no arguments: a test records what had happened before the pin was installed.
  std::function<void()> on_install_tick;
};

State& Get();

}  // namespace map_service_stubs
