// The local-copy precedence of a custom map (docs/adr/0048-custom-maps-channel.md, decision 8): a local copy of the same level wins
// over the CDN copy. The CDN map is skipped whole with `map_skipped reason=local_map`, so the two never mix per key. Pure: the
// caller passes the names, and nothing here reads a file, calls the game or writes a log. CacheMount::Mount and FetchGroup both
// decide through DecideLocalPrecedence, so the rule has one home.
#pragma once

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace nevr_maps {

// The provider seam: the level names of the local copies this run holds (a hand-installed level file the local loader finds).
// The channel supplies the real one on each platform. An empty function is no local level at all, the default
// (ChannelConfig::local_level_names).
using LocalLevelNamesFn = std::function<std::set<std::string>()>;

// The refusal token of a local copy: the `reason` of `map_skipped` (Warning) and of the fetch's `map_plan`.
inline constexpr const char* kLocalMapToken = "local_map";

struct LocalPrecedence {
  bool skip = false;
  std::string matched;  // the name a CDN name and a local name share; set only when skip
};

// `cdn_names` are the map's id and each of its level names. `local_names` is CollectLocalNames' result. The map is skipped when any
// CDN name is a local name; an empty name matches nothing.
LocalPrecedence DecideLocalPrecedence(const std::vector<std::string>& cdn_names, const std::set<std::string>& local_names);

// The local names of one run: the provider's level names and the ids the local map loader loads (ChannelConfig::local_maps). An id
// the loader loads is one more local name, so the two inputs decide through one rule.
std::set<std::string> CollectLocalNames(const LocalLevelNamesFn& provider, const std::vector<std::string>& local_map_ids);

}  // namespace nevr_maps
