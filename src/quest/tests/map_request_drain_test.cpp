// The Quest's session request pass (quest/cdn/map_request_drain.h, K6): a queued group is fetched on demand, then listed for the
// session pin, and a requested map is fetched with the one id asked for. Driven over the shared fake Env: no network, no mount.
#include <string>
#include <vector>

#include "core/map_request.h"
#include "quest/cdn/map_request_drain.h"
#include "quest/tests/cdn_fake_env.h"
#include "quest/tests/test_check.h"

namespace {

constexpr const char* kGroup = "0b6e4c1a-7f3d-4e2a-9c5b-8d1e2f3a4b5c";
constexpr const char* kMap = "arena_c";
constexpr auto kAlways = [] { return true; };

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
  nevr_quest::maps::ServeSessionRequests(
      requests, [&trace](const std::string& group, const nevr_maps::FetchScope& scope) { return Fetched(&trace, group, scope); },
      [&trace](const std::string& group) { trace.events.push_back("list:" + group); }, kAlways);
  QCHECK(trace.events.empty());  // nothing is waiting: no fetch and no listing
  std::string reason;
  QCHECK(requests.RequestGroup(kGroup, &reason));
  nevr_quest::maps::ServeSessionRequests(
      requests, [&trace](const std::string& group, const nevr_maps::FetchScope& scope) { return Fetched(&trace, group, scope); },
      [&trace](const std::string& group) { trace.events.push_back("list:" + group); }, kAlways);
  QCHECK((trace.events == std::vector<std::string>{std::string("fetch:") + kGroup, std::string("list:") + kGroup}));
}

// The session pin's map request fetches only that map, under the session's group, and is listed after it.
void AMapRequestFetchesOnlyTheMapItAsksFor() {
  cdn_test::FakeEnv env;
  nevr_maps::MapRequests requests(env);
  Trace trace;
  std::string reason;
  QCHECK(requests.RequestGroup(kGroup, &reason));
  nevr_quest::maps::ServeSessionRequests(
      requests, [&trace](const std::string& group, const nevr_maps::FetchScope& scope) { return Fetched(&trace, group, scope); },
      [&trace](const std::string& group) { trace.events.push_back("list:" + group); }, kAlways);
  trace.events.clear();
  QCHECK(requests.RequestMap(kMap, &reason));
  nevr_quest::maps::ServeSessionRequests(
      requests, [&trace](const std::string& group, const nevr_maps::FetchScope& scope) { return Fetched(&trace, group, scope); },
      [&trace](const std::string& group) { trace.events.push_back("list:" + group); }, kAlways);
  QCHECK((trace.events == std::vector<std::string>{std::string("fetch:") + kGroup + ":" + kMap, std::string("list:") + kGroup}));
}

// A stopping worker serves no request: the pass asks keep_going before each one and stops at once.
void AStoppingPassServesNoRequest() {
  cdn_test::FakeEnv env;
  nevr_maps::MapRequests requests(env);
  Trace trace;
  std::string reason;
  QCHECK(requests.RequestGroup(kGroup, &reason));
  nevr_quest::maps::ServeSessionRequests(
      requests, [&trace](const std::string& group, const nevr_maps::FetchScope& scope) { return Fetched(&trace, group, scope); },
      [&trace](const std::string& group) { trace.events.push_back("list:" + group); }, [] { return false; });
  QCHECK(trace.events.empty());
  QCHECK(requests.Waiting() == 1);  // the request is still waiting for a pass that runs
}

}  // namespace

int main() {
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
