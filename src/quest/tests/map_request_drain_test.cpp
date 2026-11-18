// The session request pass the Quest runs (core/map_request_serve.h, K6, K1b): a queued group is fetched on demand, then listed for the
// session pin, and a requested map is fetched with the one id asked for. Driven over the shared fake Env: no network, no mount.
#include <algorithm>
#include <string>
#include <vector>

#include "core/map_request.h"
#include "core/map_request_serve.h"
#include "quest/tests/cdn_fake_env.h"
#include "quest/tests/test_check.h"

namespace {

constexpr const char* kGroup = "0b6e4c1a-7f3d-4e2a-9c5b-8d1e2f3a4b5c";
constexpr const char* kMap = "arena_c";
constexpr auto kAlways = [] { return true; };
// The edge's mount is not under test in these checks: it does nothing.
constexpr auto kNoMount = [](const std::string&, const std::vector<std::string>&) { return nevr_maps::MountReport{}; };

// One fetch of a request: its group and the ids its scope asked for, in order of calls.
struct Trace {
  std::vector<std::string> events;
};

nevr_maps::FetchReport Fetched(Trace* trace, const std::string& group, const nevr_maps::FetchScope& scope) {
  std::string line = "fetch:" + group;
  for (const std::string& id : scope.ids) line += ":" + id;
  trace->events.push_back(line);
  nevr_maps::FetchReport report;
  report.outcome = nevr_maps::FetchOutcome::kFetched;
  return report;
}

// A mount-only request whose mount defers the map (its file stamp moved: nothing hashes it at an on-demand boot) is asked for again as a
// fetching request, so the next fetch hashes it (Channel::TakeDeferred) and its mount then grants the map. A mount that deferred
// nothing asks for nothing more.
void AMountOnlyRequestThatDefersIsAskedForAgainAsAFetch() {
  cdn_test::FakeEnv env;
  nevr_map_session_pin::ResetForTest();
  nevr_map_session_pin::ChannelView view;
  view.listed = {{nevr_map_session_pin::SessionLevelOf(kMap), kMap}};
  view.not_mounted = {{kMap, nevr_maps::kNotAskedToken}};
  nevr_map_session_pin::SetChannelView(view);
  nevr_maps::MapRequests requests(env);
  std::string reason;
  QCHECK(requests.RequestGroup(kGroup, &reason));
  QCHECK(requests.RequestMap(kMap, &reason));
  Trace trace;
  nevr_map_channel::ServeSessionRequests(
      requests, [&trace](const std::string& group, const nevr_maps::FetchScope& scope) { return Fetched(&trace, group, scope); },
      [](const std::string&) { return nevr_maps::MountReport{}; }, [](nevr_map_session_pin::ChannelView) {}, kAlways,
      [](const std::string&, const std::vector<std::string>& asked) {
        nevr_maps::MountReport report;
        report.deferred = asked;  // the mount deferred every map it was asked for
        return report;
      });
  // The pass served the mount-only request first (deferred), then the retry and the group request: the fetch that hashes it ran.
  QCHECK(std::find(trace.events.begin(), trace.events.end(), std::string("fetch:") + kGroup + ":" + kMap) != trace.events.end());
  // A mount that deferred nothing asks for nothing more.
  nevr_map_session_pin::SetChannelView(view);
  QCHECK(requests.RequestMap(kMap, &reason));
  Trace quiet;
  nevr_map_channel::ServeSessionRequests(
      requests, [&quiet](const std::string& group, const nevr_maps::FetchScope& scope) { return Fetched(&quiet, group, scope); },
      [](const std::string&) { return nevr_maps::MountReport{}; }, [](nevr_map_session_pin::ChannelView) {}, kAlways, kNoMount);
  QCHECK(std::none_of(quiet.events.begin(), quiet.events.end(), [](const std::string& e) { return e.rfind("fetch:", 0) == 0; }));
  nevr_map_session_pin::ResetForTest();
}

// A group request is fetched on demand (no id) and listed once after its fetch; nothing is fetched before the request.
void AGroupRequestIsFetchedOnDemandThenListed() {
  cdn_test::FakeEnv env;
  nevr_maps::MapRequests requests(env);
  Trace trace;
  nevr_map_channel::ServeSessionRequests(
      requests, [&trace](const std::string& group, const nevr_maps::FetchScope& scope) { return Fetched(&trace, group, scope); },
      [&trace](const std::string& group) { trace.events.push_back("list:" + group); return nevr_maps::MountReport{}; },
      [](nevr_map_session_pin::ChannelView) {}, kAlways, kNoMount);
  QCHECK(trace.events.empty());  // nothing is waiting: no fetch and no listing
  std::string reason;
  QCHECK(requests.RequestGroup(kGroup, &reason));
  nevr_map_channel::ServeSessionRequests(
      requests, [&trace](const std::string& group, const nevr_maps::FetchScope& scope) { return Fetched(&trace, group, scope); },
      [&trace](const std::string& group) { trace.events.push_back("list:" + group); return nevr_maps::MountReport{}; },
      [](nevr_map_session_pin::ChannelView) {}, kAlways, kNoMount);
  QCHECK((trace.events == std::vector<std::string>{std::string("fetch:") + kGroup, std::string("list:") + kGroup}));
}

// The session pin's map request fetches only that map, under the session's group, and is listed after it.
void AMapRequestFetchesOnlyTheMapItAsksFor() {
  cdn_test::FakeEnv env;
  nevr_maps::MapRequests requests(env);
  Trace trace;
  std::string reason;
  QCHECK(requests.RequestGroup(kGroup, &reason));
  nevr_map_channel::ServeSessionRequests(
      requests, [&trace](const std::string& group, const nevr_maps::FetchScope& scope) { return Fetched(&trace, group, scope); },
      [&trace](const std::string& group) { trace.events.push_back("list:" + group); return nevr_maps::MountReport{}; },
      [](nevr_map_session_pin::ChannelView) {}, kAlways, kNoMount);
  trace.events.clear();
  QCHECK(requests.RequestMap(kMap, &reason));
  nevr_map_channel::ServeSessionRequests(
      requests, [&trace](const std::string& group, const nevr_maps::FetchScope& scope) { return Fetched(&trace, group, scope); },
      [&trace](const std::string& group) { trace.events.push_back("list:" + group); return nevr_maps::MountReport{}; },
      [](nevr_map_session_pin::ChannelView) {}, kAlways, kNoMount);
  QCHECK((trace.events == std::vector<std::string>{std::string("fetch:") + kGroup + ":" + kMap, std::string("list:") + kGroup}));
}

// A stopping worker serves no request: the pass asks keep_going before each one and stops at once.
void AStoppingPassServesNoRequest() {
  cdn_test::FakeEnv env;
  nevr_maps::MapRequests requests(env);
  Trace trace;
  std::string reason;
  QCHECK(requests.RequestGroup(kGroup, &reason));
  nevr_map_channel::ServeSessionRequests(
      requests, [&trace](const std::string& group, const nevr_maps::FetchScope& scope) { return Fetched(&trace, group, scope); },
      [&trace](const std::string& group) { trace.events.push_back("list:" + group); return nevr_maps::MountReport{}; },
      [](nevr_map_session_pin::ChannelView) {}, [] { return false; }, kNoMount);
  QCHECK(trace.events.empty());
  QCHECK(requests.Waiting() == 1);  // the request is still waiting for a pass that runs
}

// Serves one pass of the waiting requests, each fetch ending with `outcome` and storing nothing new (maps_updated stays 0). Each mount
// the edge is asked for is a line: the group, then the ids its fetch asked for.
void ServePass(nevr_maps::MapRequests& requests, nevr_maps::FetchOutcome outcome, std::vector<std::string>* mounts) {
  nevr_map_channel::ServeSessionRequests(
      requests,
      [outcome](const std::string& group, const nevr_maps::FetchScope&) {
        nevr_maps::FetchReport report;
        report.group = group;
        report.outcome = outcome;
        return report;
      },
      [](const std::string&) { return nevr_maps::MountReport{}; }, [](nevr_map_session_pin::ChannelView) {}, kAlways,
      [mounts](const std::string& group, const std::vector<std::string>& asked) {
        std::string line = group;
        for (const std::string& id : asked) line += ":" + id;
        mounts->push_back(line);
        return nevr_maps::MountReport{};
      });
}

// A request's fetch mounts its group in this run even when it stored nothing new: the post-pass mount sees only the groups that
// stored, so the request's own mount is the one that reaches a map it fetched. The mount gets the ids the fetch's scope asked for (a
// group request asks for none, a map request for its one id). A failed fetch mounts nothing. The decision is the core's
// (ServeSessionRequests), so the PC and the Quest share it.
void AServedRequestMountsItsGroupEvenWhenItStoredNothingNew() {
  cdn_test::FakeEnv env;
  nevr_maps::MapRequests requests(env);
  std::string reason;
  std::vector<std::string> mounts;
  QCHECK(requests.RequestGroup(kGroup, &reason));
  ServePass(requests, nevr_maps::FetchOutcome::kCached, &mounts);
  QCHECK((mounts == std::vector<std::string>{std::string(kGroup)}));  // the group request: one mount, no ids
  QCHECK(requests.RequestMap(kMap, &reason));
  ServePass(requests, nevr_maps::FetchOutcome::kCached, &mounts);
  QCHECK((mounts == std::vector<std::string>{std::string(kGroup), std::string(kGroup) + ":" + kMap}));  // the map: its one id
}

// A failed fetch stores nothing and mounts nothing.
void AFailedRequestFetchMountsNothing() {
  cdn_test::FakeEnv env;
  nevr_maps::MapRequests requests(env);
  std::string reason;
  std::vector<std::string> mounts;
  QCHECK(requests.RequestGroup(kGroup, &reason));
  ServePass(requests, nevr_maps::FetchOutcome::kFailed, &mounts);
  QCHECK(mounts.empty());
}

// The background pass mounts the groups it stored (StoredGroupsToMount, the decision both workers call after a pass): a group whose fetch
// did not fail and that updated a map. A failed fetch and a fetch that stored nothing are not mounted.
void APassMountsOnlyTheGroupsItStored() {
  constexpr const char* kStored = "11111111-2222-3333-4444-555555555555";
  constexpr const char* kStoredLater = "66666666-7777-8888-9999-aaaaaaaaaaaa";
  constexpr const char* kCachedOnly = "bbbbbbbb-cccc-dddd-eeee-ffffffffffff";
  constexpr const char* kFailedGroup = "01234567-89ab-cdef-0123-456789abcdef";
  nevr_maps::FetchReport stored;
  stored.group = kStored;
  stored.outcome = nevr_maps::FetchOutcome::kFetched;
  stored.maps_updated = 2;
  nevr_maps::FetchReport cached;
  cached.group = kCachedOnly;
  cached.outcome = nevr_maps::FetchOutcome::kCached;
  nevr_maps::FetchReport failed;
  failed.group = kFailedGroup;
  failed.outcome = nevr_maps::FetchOutcome::kFailed;
  failed.maps_updated = 3;  // a failed fetch that says it stored maps still mounts nothing
  nevr_maps::FetchReport later;
  later.group = kStoredLater;
  later.outcome = nevr_maps::FetchOutcome::kFetched;
  later.maps_updated = 1;
  const std::vector<std::string> mount = nevr_map_channel::StoredGroupsToMount({stored, cached, failed, later}, /*mount_on_demand=*/false);
  QCHECK((mount == std::vector<std::string>{std::string(kStored), std::string(kStoredLater)}));
  QCHECK(nevr_map_channel::StoredGroupsToMount({}, /*mount_on_demand=*/false).empty());
  // Under the on-demand rule a pass that stored a map mounts none: a map mounts when a session asks for it (the request's own mount).
  QCHECK(nevr_map_channel::StoredGroupsToMount({stored, later}, /*mount_on_demand=*/true).empty());
}

// A request for a map the cache holds at the listed version (the pin's view says not_asked) names no fetch: the mount is queued apart from the
// group's fetch, and the pass mounts it first, with its one id, then lists the group for the pin. A map the cache lacks still fetches.
void ARequestForACachedCurrentMapIsAMountWithNoFetch() {
  cdn_test::FakeEnv env;
  nevr_map_session_pin::ResetForTest();
  nevr_map_session_pin::ChannelView view;
  view.listed = {{nevr_map_session_pin::SessionLevelOf(kMap), kMap}};
  view.not_mounted = {{kMap, nevr_maps::kNotAskedToken}};
  nevr_map_session_pin::SetChannelView(view);
  nevr_maps::MapRequests requests(env);
  std::string reason;
  QCHECK(requests.RequestGroup(kGroup, &reason));
  QCHECK(requests.RequestMap(kMap, &reason));
  QCHECK(env.FindRecord("map_request", "reason", "mount_only") != nullptr);
  std::vector<std::string> events;
  nevr_map_channel::ServeMountOnly(
      requests, [&events](const std::string& group, const std::vector<std::string>& asked) { events.push_back("mount:" + group + ":" + asked[0]); return nevr_maps::MountReport{}; },
      [&events](const std::string& group) { events.push_back("list:" + group); return nevr_maps::MountReport{}; },
      [](nevr_map_session_pin::ChannelView) {});
  QCHECK((events == std::vector<std::string>{std::string("mount:") + kGroup + ":" + kMap, std::string("list:") + kGroup}));
  events.clear();
  nevr_map_channel::ServeMountOnly(
      requests, [&events](const std::string&, const std::vector<std::string>&) { events.push_back("mount"); return nevr_maps::MountReport{}; },
      [&events](const std::string&) { events.push_back("list"); return nevr_maps::MountReport{}; }, [](nevr_map_session_pin::ChannelView) {});
  QCHECK(events.empty());  // taken once
  // The group's own request carried no id: its fetch asks for none.
  Trace trace;
  nevr_map_channel::ServeSessionRequests(
      requests, [&trace](const std::string& group, const nevr_maps::FetchScope& scope) { return Fetched(&trace, group, scope); },
      [](const std::string&) { return nevr_maps::MountReport{}; }, [](nevr_map_session_pin::ChannelView) {}, kAlways, kNoMount);
  QCHECK((trace.events == std::vector<std::string>{std::string("fetch:") + kGroup}));
  // The pass serves a queued mount first too, so a mount the light job has not taken yet is not left behind it.
  nevr_map_session_pin::SetChannelView(view);
  QCHECK(requests.RequestMap(kMap, &reason));
  std::vector<std::string> passMounts;
  nevr_map_channel::ServeSessionRequests(
      requests, [&trace](const std::string& group, const nevr_maps::FetchScope& scope) { return Fetched(&trace, group, scope); },
      [](const std::string&) { return nevr_maps::MountReport{}; }, [](nevr_map_session_pin::ChannelView) {}, kAlways,
      [&passMounts](const std::string& group, const std::vector<std::string>& asked) {
        passMounts.push_back(group + ":" + asked[0]);
        return nevr_maps::MountReport{};
      });
  QCHECK((passMounts == std::vector<std::string>{std::string(kGroup) + ":" + kMap}));
  // A map the cache lacks is still a fetch under the gap.
  nevr_map_session_pin::ChannelView lacking;
  lacking.listed = {{nevr_map_session_pin::SessionLevelOf(kMap), kMap}};
  lacking.not_mounted = {{kMap, nevr_maps::kNotCachedToken}};
  nevr_map_session_pin::SetChannelView(lacking);
  QCHECK(requests.RequestMap(kMap, &reason));
  trace.events.clear();
  nevr_map_channel::ServeSessionRequests(
      requests, [&trace](const std::string& group, const nevr_maps::FetchScope& scope) { return Fetched(&trace, group, scope); },
      [](const std::string&) { return nevr_maps::MountReport{}; }, [](nevr_map_session_pin::ChannelView) {}, kAlways, kNoMount);
  QCHECK((trace.events == std::vector<std::string>{std::string("fetch:") + kGroup + ":" + kMap}));
  nevr_map_session_pin::ResetForTest();
}

}  // namespace

int main() {
  ARequestForACachedCurrentMapIsAMountWithNoFetch();
  AMountOnlyRequestThatDefersIsAskedForAgainAsAFetch();
  AServedRequestMountsItsGroupEvenWhenItStoredNothingNew();
  AFailedRequestFetchMountsNothing();
  AGroupRequestIsFetchedOnDemandThenListed();
  AMapRequestFetchesOnlyTheMapItAsksFor();
  AStoppingPassServesNoRequest();
  APassMountsOnlyTheGroupsItStored();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "map_request_drain_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("map_request_drain_test: all checks passed\n");
  return 0;
}
