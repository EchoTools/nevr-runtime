// The map channel's mount and fetch rules on the Quest host, over the shared fixture (quest/tests/map_run_fixture.h), so the
// program runs the code the PC GTest runs (core/map_run.cpp, core/tests/map_run_test.cpp) against the same fakes:
//   - the local-copy precedence (docs/adr/0048 decision 8): a CDN map whose id or level name a local copy carries is skipped
//     whole with `map_skipped reason=local_map`, and the fetch plans it `local_map` and stores nothing for it;
//   - revocation (decision 10): a revoked id, a revoked index sha256 and an id the stored manifest no longer lists;
//   - expiry (decision 3): past `expires` a map with scripts does not mount and a script-free map still does.
// Every rule is checked by the record it writes as well as by the mount's report.
//
// A map's level name is derived from its id (core/map_manifest.cpp, ExpectedLevelName: the PC level is the id, the Quest level is
// the id with its suffix), and the manifest refuses any other, so the tests name a local copy by the level the manifest carries.

#include <set>
#include <string>

#include "core/map_local_precedence.h"
#include "core/map_run.h"
#include "quest/tests/map_run_fixture.h"
#include "quest/tests/mini_test.h"

using namespace map_run_fixture;
using namespace nevr_maps;

namespace {

constexpr const char* kExpiredAt = "2026-12-01T00:00:00Z";  // a manifest's expiry in these tests; the mount's `now` is this too

// The provider seam (ChannelConfig::local_level_names) answering `names`.
LocalLevelNamesFn LocalLevels(std::set<std::string> names) {
  return [names] { return names; };
}

}  // namespace

// A local copy of the level the CDN map carries wins: the map is skipped with no grant, and the record names the local level.
TEST(LocalCopyOfTheSameLevelWinsOverTheCdnMap) {
  Fixture f;
  f.channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  f.channel.Sign(1);
  f.Fetch();
  f.config.local_level_names = LocalLevels({"alpha"});
  const MountReport boot = f.Boot();
  CHECK_EQ(Fixture::TokenOf(boot, "alpha"), std::string("local_map"));
  CHECK(!Fixture::Mounted(boot, "alpha"));
  CHECK(f.mounter.calls.empty());
  CHECK_EQ(f.env.open_blobs(), 0u);
  CHECK(f.env.FindRecord("map_skipped", "reason", "local_map") != nullptr);
  CHECK(f.env.FindRecord("map_skipped", "local_name", "alpha") != nullptr);
}

// The fetch plans the map `local_map` and stores nothing for it, the same rule as the mount.
TEST(ALocalLevelIsPlannedAsLocalAndItsMapIsNotFetched) {
  Fixture f;
  f.channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  f.channel.Sign(1);
  f.config.local_level_names = LocalLevels({"alpha"});
  const FetchReport fetched = f.Fetch();
  CHECK_EQ(fetched.maps_updated, 0u);
  CHECK(f.env.FindRecord("map_plan", "reason", "local_map") != nullptr);
  CHECK(f.Index().maps.count(MapKey("alpha", kPlatformPc)) == 0u);
}

// A local copy of another level does not skip the map.
TEST(ALocalCopyOfAnotherLevelDoesNotSkipTheMap) {
  Fixture f;
  f.channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  f.channel.Sign(1);
  f.Fetch();
  f.config.local_level_names = LocalLevels({"beta"});
  const MountReport boot = f.Boot();
  CHECK(Fixture::Mounted(boot, "alpha"));
  CHECK(f.env.FindRecord("map_skipped", "reason", "local_map") == nullptr);
}

// With no provider (the default) and no local map ids, nothing is local and the map mounts.
TEST(WithNoLocalProviderTheMapMounts) {
  Fixture f;
  f.channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  f.channel.Sign(1);
  f.Fetch();
  const MountReport boot = f.Boot();
  CHECK(Fixture::Mounted(boot, "alpha"));
}

// The local map loader's ids are local names as before: a local map id skips the CDN map of that id.
TEST(ALocalMapIdStillSkipsTheMapOfThatId) {
  Fixture f;
  f.channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  f.channel.Sign(1);
  f.Fetch();
  f.config.local_maps = {"alpha"};
  const MountReport boot = f.Boot();
  CHECK_EQ(Fixture::TokenOf(boot, "alpha"), std::string("local_map"));
  CHECK(f.mounter.calls.empty());
}

// The decision itself, over the names a Quest map carries: its id and its suffixed level name. A local copy of the suffixed level
// skips the map, and the match names the level that matched.
TEST(DecideMatchesAnyCdnNameAgainstTheLocalNames) {
  const LocalPrecedence quest = DecideLocalPrecedence({"alpha", "alpha_quest"}, {"alpha_quest"});
  CHECK(quest.skip);
  CHECK_EQ(quest.matched, std::string("alpha_quest"));
  CHECK(!DecideLocalPrecedence({"alpha", "alpha_quest"}, {"beta"}).skip);
  CHECK(!DecideLocalPrecedence({"alpha", "alpha_quest"}, {}).skip);
  CHECK(!DecideLocalPrecedence({"", "alpha"}, {""}).skip);  // an empty name matches nothing
  CHECK_EQ(std::string(kLocalMapToken), std::string("local_map"));
}

// The local names of a run are the provider's level names and the local map ids, one set; an empty name is not a name.
TEST(CollectLocalNamesUnitesTheProviderAndTheLocalMapIds) {
  const std::set<std::string> names =
      CollectLocalNames(LocalLevels({"arena_a", ""}), {"alpha", ""});
  CHECK_EQ(names.size(), 2u);
  CHECK(names.count("arena_a") == 1u);
  CHECK(names.count("alpha") == 1u);
  CHECK(CollectLocalNames(LocalLevelNamesFn(), {}).empty());  // the default provider is no local level
}

// A revoked id is not mounted, and the stored manifest that revokes it is the one the mount reads (not a fetch, which would drop
// the cached version first). The map the id names stays in the cache, so only the revocation can keep it out.
TEST(ARevokedIdIsNotMounted) {
  Fixture f;
  f.channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  f.channel.Publish("plain", "1.0.0", MakeSet(2, 'p'));
  f.channel.Sign(1);
  f.Fetch();
  const std::string revoking = f.channel.Sign(2, "2027-01-01T00:00:00Z", {"alpha"});
  CHECK(f.env.WriteFileAtomically(StoredGroupManifestPath(kDir, kGroup), revoking));
  const MountReport boot = f.Boot();
  CHECK(Fixture::Mounted(boot, "plain"));
  CHECK(!Fixture::Mounted(boot, "alpha"));
  CHECK_EQ(Fixture::TokenOf(boot, "alpha"), std::string("revoked"));
  CHECK(!f.mounter.Called("set map:alpha"));
}

// A revoked index sha256 is not mounted, whichever map lists it.
TEST(ARevokedIndexSha256IsNotMounted) {
  Fixture f;
  f.channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  f.channel.Publish("plain", "1.0.0", MakeSet(2, 'p'));
  f.channel.Sign(1);
  f.Fetch();
  const std::string revoking = f.channel.Sign(2, "2027-01-01T00:00:00Z", {}, {f.channel.IndexSha("alpha")});
  CHECK(f.env.WriteFileAtomically(StoredGroupManifestPath(kDir, kGroup), revoking));
  const MountReport boot = f.Boot();
  CHECK(Fixture::Mounted(boot, "plain"));
  CHECK_EQ(Fixture::TokenOf(boot, "alpha"), std::string("revoked"));
}

// An id the stored manifest no longer lists is not mounted: the mount iterates only the maps a loaded group lists, so a map the
// cache still holds is dropped at the next mount that reads the manifest.
TEST(AnIdTheStoredManifestNoLongerListsIsNotMounted) {
  Fixture f;
  f.channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  f.channel.Publish("beta", "1.0.0", MakeSet(2, 'b'));
  f.channel.Sign(1);
  f.Fetch();
  f.channel.Unpublish("beta");
  const std::string unlisting = f.channel.Sign(2);
  CHECK(f.env.WriteFileAtomically(StoredGroupManifestPath(kDir, kGroup), unlisting));
  const MountReport boot = f.Boot();
  CHECK(Fixture::Mounted(boot, "alpha"));
  CHECK(!Fixture::Mounted(boot, "beta"));
  CHECK_EQ(Fixture::TokenOf(boot, "beta"), std::string(""));
  CHECK(!f.mounter.Called("set map:beta"));
}

// Past `expires` a map with scripts does not mount, and no script is granted for it.
TEST(AnExpiredMapWithScriptsDoesNotMount) {
  Fixture f;
  f.channel.Publish("scripted", "1.0.0", MakeSet(2, 'b'), {{1, "dll"}});
  f.channel.Sign(1, kExpiredAt);
  f.Fetch();
  const MountReport boot = f.Boot(nullptr, kExpiredAt);
  CHECK(!Fixture::Mounted(boot, "scripted"));
  CHECK_EQ(Fixture::TokenOf(boot, "scripted"), std::string("expired"));
  CHECK(!f.mounter.Called("script map:scripted 1"));
}

// Past `expires` a script-free map still mounts: expiry blocks scripted maps only.
TEST(AnExpiredMapWithoutScriptsStillMounts) {
  Fixture f;
  f.channel.Publish("plain", "1.0.0", MakeSet(2, 'a'));
  f.channel.Sign(1, kExpiredAt);
  f.Fetch();
  const MountReport boot = f.Boot(nullptr, kExpiredAt);
  CHECK(Fixture::Mounted(boot, "plain"));
}

int main(int argc, char** argv) { return mini_test::RunAll(argc, argv); }
