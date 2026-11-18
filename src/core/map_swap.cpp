#include "core/map_swap.h"

#include <utility>

#include "core/map_manifest.h"

namespace nevr_maps {

SwapStep DecideSwap(const SwapView& view) {
  if (view.level_loaded) return SwapStep::kDefer;
  if (view.keys_loaded != 0) return SwapStep::kRestart;
  return SwapStep::kSwap;
}

std::string SwapRestartLine(const std::string& id) { return "Restart the game to play the update of " + id + "."; }

void MapSwaps::Serve(const std::string& id, const MapSet& set) {
  for (const AssetKey& key : set.keys) served_by_.emplace(key, id);
}

void MapSwaps::StopServing(const std::string& id, const MapSet& set) {
  for (const AssetKey& key : set.keys) {
    const auto served = served_by_.find(key);
    if (served != served_by_.end() && served->second == id) served_by_.erase(served);
  }
}

void MapSwaps::Mounted(const std::string& id, MapSet set) {
  const auto old = mounted_.find(id);
  if (old != mounted_.end()) StopServing(id, old->second);
  Serve(id, set);
  mounted_[id] = std::move(set);
}

bool MapSwaps::Stored(const std::string& id, MapSet newer) {
  const auto mounted = mounted_.find(id);
  if (mounted == mounted_.end() || CompareVersions(newer.version, mounted->second.version) <= 0) return false;
  const auto waiting = waiting_.find(id);
  if (waiting != waiting_.end() && CompareVersions(newer.version, waiting->second.newer.version) <= 0) return false;
  waiting_[id] = Pending{std::move(newer), false};
  return true;
}

std::vector<std::string> MapSwaps::Step(const ViewFn& view) {
  std::vector<std::string> swapped;
  for (auto it = waiting_.begin(); it != waiting_.end();) {
    const std::string& id = it->first;
    MapSet& mounted = mounted_.at(id);
    const SwapView seen = view(id, mounted);
    const SwapStep step = DecideSwap(seen);
    const std::vector<nevr_cdn::Field> what = {{"id", id}, {"from", mounted.version}, {"to", it->second.newer.version}};
    auto record = [&](nevr_cdn::EventLevel level, const char* decision, const std::string& consequence) {
      std::vector<nevr_cdn::Field> fields = what;
      fields.emplace_back("decision", decision);
      fields.emplace_back("keys_loaded", seen.keys_loaded);
      fields.emplace_back("consequence", consequence);
      env_.Event(level, "map_swap", fields);
    };
    if (step == SwapStep::kDefer) {
      if (!it->second.defer_said) {
        it->second.defer_said = true;
        record(nevr_cdn::EventLevel::kInfo, "defer", "a level of " + id + " is loaded: the update is swapped in after it unloads");
      }
      ++it;
      continue;
    }
    if (step == SwapStep::kRestart) {
      record(nevr_cdn::EventLevel::kWarn, "restart",
             "the game keeps " + id + "'s old assets loaded: the update is played after a restart, never mixed with the old one");
      restart_notices_.push_back(SwapRestartLine(id));
      it = waiting_.erase(it);
      continue;
    }
    record(nevr_cdn::EventLevel::kInfo, "swap", "the next load of " + id + " plays the update");
    StopServing(id, mounted);
    mounted = std::move(it->second.newer);
    Serve(id, mounted);
    swapped.push_back(id);
    it = waiting_.erase(it);
  }
  return swapped;
}

const std::string* MapSwaps::ServedBy(const AssetKey& key) const {
  const auto served = served_by_.find(key);
  return served == served_by_.end() ? nullptr : &served->second;
}

const MapSet* MapSwaps::MountedSet(const std::string& id) const {
  const auto mounted = mounted_.find(id);
  return mounted == mounted_.end() ? nullptr : &mounted->second;
}

}  // namespace nevr_maps
