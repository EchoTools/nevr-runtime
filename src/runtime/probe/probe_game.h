#pragma once
// The map probes' game half. Compiled only into a probe build (CMake option NEVR_MAP_PROBES, src/runtime/CMakeLists.txt); every
// probe stays off until maps_probe.yaml in the nEVR folder turns it on, and none runs unless the game image is the supported
// build (nevr_game_image_guard). The probes observe the custom-map channel (docs/reference/maps-channel.md) and write the
// `NEVR.PROBE` tag (docs/standards/logging.md); none changes game state except P-SKEW (the probe server sets the owner JSON's
// nevr_map_pin, the probe client may leave a session). Run them with the channel not configured: the channel's own session pin
// (runtime/content/map_session_pin_game.cpp) writes the same owner-JSON key once a map manifest is set.

#include <cstdint>
#include <string>
#include <vector>

#include "runtime/probe/probe_logic.h"

namespace nevr_map_probe {

// Boot (lifecycle/boot.cpp): registers the probe module's frame callback. The file is read on the first game-thread frame.
void Register();

struct LevelEvent {
  bool enable = false;
  bool ok = false;    // the call returned 0
  bool noop = false;  // the resource already read enabled (or disabled) before the call
  std::uint64_t level = 0;
  std::uint64_t frame = 0;  // ProbeFrame() when the call returned
};

// The log-only CGameLevelResource::Enable/Disable detours shared by P-SUBLEVEL and P-SKEW. Null when installed, otherwise the
// refusal token. Only the first call installs; later calls return its result.
const char* InstallLevelEvents();
void DrainLevelEvents(std::vector<LevelEvent>* out);
std::uint32_t DroppedLevelEvents();

std::uint64_t ProbeFrame();
void LogLines(const std::vector<std::string>& lines);
void* NetGame();
std::int32_t NetState();
std::uint64_t MainLevel();

void RulesStart();
void RulesTick(std::uint64_t frame);

void SublevelStart(const char* hook_refusal);
void SublevelTick(std::uint64_t frame, const std::vector<LevelEvent>& events);
void SublevelFinish();

void SkewStart(const ProbeConfig& config, const char* hook_refusal);
void SkewTick(std::uint64_t frame, const std::vector<LevelEvent>& events);

}  // namespace nevr_map_probe
