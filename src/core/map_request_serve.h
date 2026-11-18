#pragma once

// The session pin's view on the worker, shared by the PC and the Quest (ADR 0048 decisions 9 and 16; KFC-PORT K6, K1b). The boot
// publishes the view of its report. The worker serves each waiting request, one group at a time: the group is fetched on demand
// (the scope its requests asked for), then listed, and the listing is published, so the pin's view names what this run lacks and
// what plays after the next start. Each edge supplies its fetch, its listing and the sink (nevr_map_session_pin::SetChannelView);
// the decisions of when and what to publish are here, and the host test drives them over the shared fake Env
// (src/core/tests/map_run_test.cpp, ServeSessionView). Platform-free: no PCH, no Log().

#include <algorithm>
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
// platform's grant, and its report back (the answer's consequence reads it). Called on the worker thread, after a fetch that did not
// fail (see ServeSessionRequests).
using GroupMount = std::function<nevr_maps::MountReport(const std::string& group, const std::vector<std::string>& asked)>;

// Records on the request's answer what its mount did for the maps the request asked for: the maps it granted in this run, and the maps it
// found mounted at another version. The mount may report its whole group (production's route: MountScope is null, so no id is scoped),
// and a sibling the request did not name is not this answer's to tell, so a named request keeps only its asked ids. A group request
// names none (empty `asked`) and keeps everything its mount did.
inline void RecordMount(nevr_maps::FetchReport* report, const nevr_maps::MountReport& mounted, const std::vector<std::string>& asked) {
  const auto kept = [&asked](const std::string& id) {
    return asked.empty() || std::find(asked.begin(), asked.end(), id) != asked.end();
  };
  for (const std::string& id : mounted.mounted) {
    if (kept(id)) report->mounted_now.push_back(id);
  }
  for (const auto& row : mounted.not_mounted) {
    if (row.second == nevr_maps::kMountedOtherVersionToken && kept(row.first)) report->superseded.push_back(row.first);
  }
}

// The boot's view: the report the boot mount returned, published before any request is served.
inline void PublishBootView(const nevr_maps::MountReport& report, const ViewSink& publish) {
  publish(nevr_map_session_pin::ViewFromMount(report));
}

// The mounts of the maps a request named that the cache holds at the listed version: each group's asked ids are mounted at once, with no
// fetch and no gap, and the group's listing is published for the pin. The worker's light job runs this (FetchWorker::RequestLight), and
// every full pass runs it first, so a request is never left behind. Returns when none is waiting. A map the mount deferred is asked for
// again as a fetching request (MapRequests::RetryAfterRehash).
inline void ServeMountOnly(nevr_maps::MapRequests& requests, const GroupMount& mount, const GroupListing& list, const ViewSink& publish) {
  for (const auto& entry : requests.TakeMountOnly()) {
    const nevr_maps::MountReport mounted = mount(entry.first, entry.second);
    // A cached map whose mount deferred (its file stamp moved, or its stock files changed) is hashed again by a fetching request, which
    // mounts it after: nothing rehashes a map a mount-only request deferred, and the boot of an on-demand client defers nothing.
    for (const std::string& id : entry.second) {
      const bool deferred = std::find(mounted.deferred.begin(), mounted.deferred.end(), id) != mounted.deferred.end();
      std::string reason;
      if (deferred) requests.RetryAfterRehash(entry.first, id, &reason);
    }
    publish(nevr_map_session_pin::ViewFromMount(list(entry.first)));
  }
}

// The worker's request pass: serves the waiting requests until none is left or keep_going turns false. A stopping worker serves
// nothing further. Each served group is fetched, then mounted (a fetch that did not fail: a map a session asks for mounts in this run,
// ADR 0048 decision 16, whether or not the fetch stored anything new, because the worker's post-pass mounts only the groups it stored),
// then listed, then its listing is published as the view.
inline void ServeSessionRequests(nevr_maps::MapRequests& requests, const GroupFetch& fetch, const GroupListing& list,
                                 const ViewSink& publish, const std::function<bool()>& keep_going, const GroupMount& mount) {
  const auto served = [&fetch, &list, &publish, &mount](const std::string& group, const nevr_maps::FetchScope& scope) {
    nevr_maps::FetchReport report = fetch(group, scope);
    if (report.outcome != nevr_maps::FetchOutcome::kFailed) RecordMount(&report, mount(group, scope.ids), scope.ids);
    publish(nevr_map_session_pin::ViewFromMount(list(group)));
    return report;
  };
  ServeMountOnly(requests, mount, list, publish);
  while (keep_going() && requests.RunNext(served)) {
  }
}

// The groups a background pass stored, which mount in this run after the pass (ADR 0048 decision 16): a group whose fetch did not fail
// and that updated at least one map. The PC worker and the Quest worker both mount exactly these, so the decision is here and the host
// test pins it. Under the on-demand rule (ChannelConfig::mount_on_demand) a pass mounts nothing: a map the pass refreshed mounts when a
// session asks for it, through the request's own mount (ServeSessionRequests).
inline std::vector<std::string> StoredGroupsToMount(const std::vector<nevr_maps::FetchReport>& reports, bool mount_on_demand) {
  std::vector<std::string> stored;
  if (mount_on_demand) return stored;
  for (const nevr_maps::FetchReport& report : reports) {
    if (report.outcome != nevr_maps::FetchOutcome::kFailed && report.maps_updated != 0) stored.push_back(report.group);
  }
  return stored;
}

}  // namespace nevr_map_channel
