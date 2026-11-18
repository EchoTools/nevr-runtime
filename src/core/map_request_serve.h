#pragma once

// The session pin's view on the worker, shared by the PC and the Quest (ADR 0048 decisions 9 and 16; KFC-PORT K6, K1b). The boot
// publishes the view of its report. The worker serves each waiting request, one group at a time: the group is fetched on demand
// (the scope its requests asked for), then listed, and the listing is published, so the pin's view names what this run lacks and
// what plays after the next start. Each edge supplies its fetch, its listing and the sink (nevr_map_session_pin::SetChannelView);
// the decisions of when and what to publish are here, and the host test drives them over the shared fake Env
// (src/core/tests/map_run_test.cpp, ServeSessionView). Platform-free: no PCH, no Log().

#include <functional>
#include <string>
#include <vector>

#include "core/map_request.h"
#include "core/map_session_pin.h"

namespace nevr_map_channel {

// The fetch of one requested group under its scope (MapRequests::Fetch), the listing of that one group for the pin (ListGroups),
// and where the view goes.
using GroupFetch = std::function<nevr_maps::FetchReport(const std::string& group, const nevr_maps::FetchScope& scope)>;
using GroupListing = std::function<nevr_maps::MountReport(const std::string& group)>;
using ViewSink = std::function<void(nevr_map_session_pin::ChannelView)>;
// The mount of a served group, which the edge supplies: MountGroups for the group with the ids its fetch asked for (`asked`), then the
// platform's grant. Called on the worker thread, after a fetch that did not fail (see ServeSessionRequests).
using GroupMount = std::function<void(const std::string& group, const std::vector<std::string>& asked)>;

// The boot's view: the report the boot mount returned, published before any request is served.
inline void PublishBootView(const nevr_maps::MountReport& report, const ViewSink& publish) {
  publish(nevr_map_session_pin::ViewFromMount(report));
}

// The worker's request pass: serves the waiting requests until none is left or keep_going turns false. A stopping worker serves
// nothing further. Each served group is fetched, then mounted (a fetch that did not fail: a map a session asks for mounts in this run,
// ADR 0048 decision 16, whether or not the fetch stored anything new, because the worker's post-pass mounts only the groups it stored),
// then listed, then its listing is published as the view.
inline void ServeSessionRequests(nevr_maps::MapRequests& requests, const GroupFetch& fetch, const GroupListing& list,
                                 const ViewSink& publish, const std::function<bool()>& keep_going, const GroupMount& mount) {
  const auto served = [&fetch, &list, &publish, &mount](const std::string& group, const nevr_maps::FetchScope& scope) {
    nevr_maps::FetchReport report = fetch(group, scope);
    if (report.outcome != nevr_maps::FetchOutcome::kFailed) mount(group, scope.ids);
    publish(nevr_map_session_pin::ViewFromMount(list(group)));
    return report;
  };
  while (keep_going() && requests.RunNext(served)) {
  }
}

// The groups a background pass stored, which mount in this run after the pass (ADR 0048 decision 16): a group whose fetch did not fail
// and that updated at least one map. The PC worker and the Quest worker both mount exactly these, so the decision is here and the host
// test pins it.
inline std::vector<std::string> StoredGroupsToMount(const std::vector<nevr_maps::FetchReport>& reports) {
  std::vector<std::string> stored;
  for (const nevr_maps::FetchReport& report : reports) {
    if (report.outcome != nevr_maps::FetchOutcome::kFailed && report.maps_updated != 0) stored.push_back(report.group);
  }
  return stored;
}

}  // namespace nevr_map_channel
