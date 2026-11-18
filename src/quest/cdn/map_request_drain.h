#pragma once

// The Quest's session request pass (ADR 0048 decision 16; the Quest twin of the PC's DrainRequests): every request waiting on the
// worker is served, one group at a time. Each group is fetched on demand (the scope its requests asked for), then `after` lists it
// for the session pin, so the pin's view names what this run lacks and what plays after the next start. Pure over its callables, so
// the host test drives it over the shared fake Env (src/quest/tests/map_request_drain_test.cpp).

#include <functional>
#include <string>

#include "core/map_request.h"

namespace nevr_quest::maps {

using GroupFetch = std::function<nevr_maps::FetchReport(const std::string& group, const nevr_maps::FetchScope& scope)>;
using GroupAfter = std::function<void(const std::string& group)>;

// Serves the waiting requests until none is left or keep_going turns false. A stopping worker serves nothing further.
inline void ServeSessionRequests(nevr_maps::MapRequests& requests, const GroupFetch& fetch, const GroupAfter& after,
                                 const std::function<bool()>& keep_going) {
  const auto served = [&fetch, &after](const std::string& group, const nevr_maps::FetchScope& scope) {
    nevr_maps::FetchReport report = fetch(group, scope);
    after(group);
    return report;
  };
  while (keep_going() && requests.RunNext(served)) {
  }
}

}  // namespace nevr_quest::maps
