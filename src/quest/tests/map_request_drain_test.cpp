// The session request pass the Quest runs (core/map_request_serve.h, K6, K1b): a queued group is fetched on demand, then listed for the
// session pin, and a requested map is fetched with the one id asked for. Driven over the shared fake Env: no network, no mount.
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
constexpr auto kNoMount = [](const std::string&, const std::vector<std::string>&) {};

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

}  // namespace

int main() {
  AServedRequestMountsItsGroupEvenWhenItStoredNothingNew();
  AFailedRequestFetchMountsNothing();
  AGroupRequestIsFetchedOnDemandThenListed();
  AMapRequestFetchesOnlyTheMapItAsksFor();
  AStoppingPassServesNoRequest();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "map_request_drain_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("map_request_drain_test: all checks passed\n");
  return 0;
}
