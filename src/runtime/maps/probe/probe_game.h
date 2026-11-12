#pragma once
// The owner probes' game half (D:\b\cdnresearch\OWNER_PROBES.md). Compiled only into a probe build
// (NEVR_MAPS_PROBES=ON, src/runtime/maps/probes.cmake); every probe stays off until maps_probe.yaml in the nevr folder
// turns it on, and none runs unless the game image is the supported 631547 build (game_image_guard).

#include <cstdint>
#include <string>
#include <vector>

#include "runtime/maps/probe/probe_logic.h"

namespace nevr_maps_probe {

// Boot (lifecycle/boot.cpp): registers the probe module's frame callback. The file is read on the first game-thread
// frame.
void Register();

struct LevelEvent {
  bool enable = false;
  bool ok = false;    // the call returned 0
  bool noop = false;  // the resource already read enabled (or disabled) before the call
  std::uint64_t level = 0;
  std::uint64_t frame = 0;  // ProbeFrame() when the call returned
};

// The log-only CGameLevelResource::Enable/Disable detours shared by P-SUBLEVEL and P-SKEW. Null when installed,
// otherwise the refusal token. Only the first call installs; later calls return its result.
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

}  // namespace nevr_maps_probe
