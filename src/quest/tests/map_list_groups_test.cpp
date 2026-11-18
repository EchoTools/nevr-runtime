// ListGroups (core/map_channel.h, K6): a booted channel lists a group's maps and grants none of them. The map is fetched on demand
// after the boot (the request's fetch), so the listing is the first time this run sees it, and it must be reported as not_asked,
// never mounted or handed to the mounter. Over the map run fixture: a signed group, the fake cache, no network.
#include <cstdio>
#include <string>

#include "core/map_channel.h"
#include "core/map_run.h"
#include "quest/tests/map_run_fixture.h"
#include "quest/tests/test_check.h"

namespace nevr_config_alias {
// The Quest sentinel's warning line (quest/sentinel/config_alias_levels.cpp), which this host test does not link (as map_channel_test).
void PlatformWarn(const char*) noexcept {}
}  // namespace nevr_config_alias

namespace {

namespace mc = nevr_map_channel;
using map_run_fixture::Fixture;

// A group's map that a request fetched after the boot: it is in the cache, the run has not mounted it, and a listing names it.
void AListingOfAMapFetchedAfterBootIsNotMountedOrGranted() {
  Fixture f;
  f.channel.Publish("arena_c", "1.0.0", map_run_fixture::MakeSet(4, 'c'));
  f.channel.Sign(1);
  nevr_maps::ChannelConfig platform = f.config;
  mc::Settings settings;
  settings.base_url = map_run_fixture::kBase;
  settings.keys = {f.key.pin};
  mc::Channel channel(platform, settings);
  channel.Boot(f.env, f.fs, f.mounter, nullptr, map_run_fixture::kNow, {});  // the cache is empty: nothing is mounted at boot
  QCHECK(f.mounter.calls.empty());

  nevr_maps::FetchScope scope;  // what a session's request asks for: this map alone, on demand
  scope.on_demand = true;
  scope.ids = {"arena_c"};
  const nevr_maps::FetchReport fetched = nevr_maps::FetchGroup(f.env, f.fs, platform, map_run_fixture::kGroup, {}, nullptr,
                                                               map_run_fixture::kNow, f.keep_going, nullptr, scope);
  QCHECK(fetched.outcome == nevr_maps::FetchOutcome::kFetched);

  const nevr_maps::MountReport listing = channel.ListGroups({map_run_fixture::kGroup}, map_run_fixture::kNow);
  QCHECK(Fixture::TokenOf(listing, "arena_c") == "not_asked");  // the run names it as waiting for the next start
  QCHECK(!Fixture::Mounted(listing, "arena_c"));
  QCHECK(f.mounter.calls.empty());  // a listing grants nothing: the mounter is not asked for the map
  bool listed = false;
  for (const auto& row : listing.listed) listed = listed || row.second == "arena_c";
  QCHECK(listed);
}

// The listing is folded onto the boot's report: a map an earlier run cached and this boot mounted is still mounted in the listing.
void AListingKeepsTheMapsTheBootMounted() {
  Fixture f;
  f.channel.Publish("alpha", "1.0.0", map_run_fixture::MakeSet(2, 'a'));
  f.channel.Sign(1);
  QCHECK(f.Fetch().outcome == nevr_maps::FetchOutcome::kFetched);  // the cache holds alpha before this boot
  nevr_maps::ChannelConfig platform = f.config;
  mc::Settings settings;
  settings.base_url = map_run_fixture::kBase;
  settings.keys = {f.key.pin};
  mc::Channel channel(platform, settings);
  const mc::BootResult& boot = channel.Boot(f.env, f.fs, f.mounter, nullptr, map_run_fixture::kNow, {});
  QCHECK(Fixture::Mounted(boot.report, "alpha"));
  const nevr_maps::MountReport listing = channel.ListGroups({map_run_fixture::kGroup}, map_run_fixture::kNow);
  QCHECK(Fixture::Mounted(listing, "alpha"));  // the run's own mount is not lost to the listing
}

}  // namespace

int main() {
  AListingOfAMapFetchedAfterBootIsNotMountedOrGranted();
  AListingKeepsTheMapsTheBootMounted();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "map_list_groups_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("map_list_groups_test: all checks passed\n");
  return 0;
}
