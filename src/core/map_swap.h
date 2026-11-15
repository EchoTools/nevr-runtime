/* Same-owner replace of a mounted map mid-run (owner: "Is there not a way we can swap out which map is mounted?").
 *
 * A map's mounted set is replaced by a newer version's set only when the engine holds nothing of the old one: no level of the map
 * is loaded, and none of the keys the old set serves reads loaded (the engine's own predicate, patch/plugin_assets.cpp
 * EngineHasLoaded). A resident texture of the old set would otherwise stream the new version's mips over the old base (ADR 0018).
 * While a level of the map is loaded the swap waits for the unload. Once no level is loaded, a key still loaded means the engine
 * keeps it for the run, so the swap is refused and the player is told to restart the game to play the update.
 *
 * After a swap the new set serves the map's keys, the old set serves nothing, and every other map is untouched. A key only the old
 * set had is served by no map. This is the decision and the serving table, platform-free; every record goes through the Env.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "cdn/cdn_env.h"

namespace nevr_maps {

using AssetKey = std::pair<std::uint64_t, std::uint64_t>;  // (type, name)

// One version of a map as it is mounted: its version and the keys its set serves.
struct MapSet {
  std::string version;
  std::set<AssetKey> keys;
};

// What the engine holds of one map now.
struct SwapView {
  bool level_loaded = false;    // a level of the map is loaded (the config_alias loaded-level tables)
  std::size_t keys_loaded = 0;  // keys of the mounted set that EngineHasLoaded reads as loaded
};

enum class SwapStep { kSwap, kDefer, kRestart };

//   a level of the map is loaded            -> defer (wait for the unload)
//   no level loaded, some key still loaded  -> restart (the engine keeps it for the run)
//   nothing loaded                          -> swap
SwapStep DecideSwap(const SwapView& view);

// The notice for a refused swap.
std::string SwapRestartLine(const std::string& id);

class MapSwaps {
 public:
  using ViewFn = std::function<SwapView(const std::string& id, const MapSet& mounted)>;

  explicit MapSwaps(nevr_cdn::Env& env) : env_(env) {}

  // A map's set is mounted (boot or a later mount): its keys are served as its own. A key another map already serves stays that
  // map's (the registry refuses such a set before it gets here).
  void Mounted(const std::string& id, MapSet set);

  // The worker stored a newer version of a mounted map: the swap waits for the next Step. A newer Stored replaces a waiting one.
  // False, with nothing waiting, when the map is not mounted or the version is the mounted one.
  bool Stored(const std::string& id, MapSet newer);

  // Each frame: every waiting swap is decided over `view`. Swapped maps are returned; a refused one stops waiting, with one
  // map_swap record and the restart notice in RestartNotices(). A deferred one writes its record once.
  std::vector<std::string> Step(const ViewFn& view);

  // The map whose mounted set serves `key`, or "" when none does.
  std::string ServedBy(const AssetKey& key) const;
  const MapSet* MountedSet(const std::string& id) const;
  bool Waiting(const std::string& id) const { return waiting_.count(id) != 0; }
  const std::vector<std::string>& RestartNotices() const { return restart_notices_; }

 private:
  struct Pending {
    MapSet newer;
    bool defer_said = false;
  };
  void Serve(const std::string& id, const MapSet& set);
  void StopServing(const std::string& id, const MapSet& set);

  nevr_cdn::Env& env_;
  std::map<std::string, MapSet> mounted_;
  std::map<std::string, Pending> waiting_;
  std::map<AssetKey, std::string> served_by_;
  std::vector<std::string> restart_notices_;
};

}  // namespace nevr_maps
