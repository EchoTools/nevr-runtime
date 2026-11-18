// The custom-map channel's start (core/map_channel.h; docs/adr/0048-custom-maps-channel.md, decisions 3, 7, 8, 12): the maps.* settings
// read once, the start decision, the worker's group plan, the source ordering, the boot that mounts with no network and no hashing,
// its idempotence, the fetch that runs only on the worker, and the worker's request gap. Over the shared fake Env and MapFs
// (quest/tests/cdn_fake_env.h, map_fake_fs.h). Framework-free: the Quest host group runs it. No network, no Win32, no game file.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/map_channel.h"
#include "core/map_groups.h"
#include "core/map_run.h"
#include "quest/tests/cdn_fake_env.h"
#include "quest/tests/map_fake_fs.h"
#include "quest/tests/test_check.h"

namespace nevr_config_alias {
// The Quest sentinel's warning line (quest/sentinel/config_alias_levels.cpp), which this host test does not link. The alias table's
// warnings go nowhere here; the checks read the channel's own records.
void PlatformWarn(const char*) noexcept {}
}  // namespace nevr_config_alias

namespace {

namespace mc = nevr_map_channel;
using cdn_test::FakeEnv;
using cdn_test::FakeMapFs;

// A public minisign key (tests/vectors/maps/minisign/test_key.pub): a valid key that signs nothing a test relies on.
constexpr const char* kTestPub =
    "untrusted comment: minisign public key 12CF26D38A5D6FFB\nRWT7b12K0ybPErgOPJNseA689+mTi7fFLcTuzrBCVDwzdi7wbqXFzF5l\n";
constexpr const char* kBase = "https://maps.test/";
constexpr const char* kGroup = "0b6e4c1a-7f3d-4e2a-9c5b-8d1e2f3a4b5c";
constexpr const char* kNow = "2026-11-11T00:00:00Z";
constexpr const char* kCacheDir = "C:/nevr/cache/maps";

using Values = std::map<std::string, std::string>;
using Lists = std::map<std::string, std::vector<std::string>>;

mc::ScalarLookup Scalars(const Values& values) {
  return [values](const char* key) {
    const auto it = values.find(key);
    return it == values.end() ? std::string() : it->second;
  };
}

mc::ListLookup Items(const Lists& lists) {
  return [lists](const char* key) {
    const auto it = lists.find(key);
    return it == lists.end() ? std::vector<std::string>() : it->second;
  };
}

// A mounter that counts every grant asked for and grants nothing.
class CountingMounter : public nevr_maps::Mounter {
 public:
  int calls = 0;
  nevr_maps::Grant Script(const std::string&, std::uint64_t, std::int64_t, const std::string&, bool) override {
    ++calls;
    return nevr_maps::Grant{};
  }
  nevr_maps::Grant Rekey(const std::string&, std::uint64_t, std::uint64_t, const std::string&, const std::string&) override {
    ++calls;
    return nevr_maps::Grant{};
  }
  nevr_maps::Grant Set(const std::string&, std::int64_t, const std::string&, const std::string&, bool) override {
    ++calls;
    return nevr_maps::Grant{};
  }
  nevr_maps::Grant StageAliases(const std::string&, const std::string&, std::uint64_t,
                                const std::vector<nevr_maps::ConfigAlias>&) override {
    ++calls;
    return nevr_maps::Grant{};
  }
  nevr_maps::Grant DerivedScript(const std::string&, std::uint64_t, std::int64_t, const std::string&, const std::string&,
                                 bool) override {
    ++calls;
    return nevr_maps::Grant{};
  }
  void Withdraw(const std::string&) override { ++calls; }
};

nevr_maps::ChannelConfig PlatformConfig() {
  nevr_maps::ChannelConfig c;
  c.dir = kCacheDir;
  c.platform = nevr_maps::kPlatformQuest;
  c.game_build = "test-build";
  return c;
}

// The settings of a configured channel: base URL, one configured official key.
Values ConfiguredValues() {
  Values v;
  v["maps_base_url"] = kBase;
  return v;
}

const cdn_test::Record* FirstEvent(const FakeEnv& env, const char* name) {
  for (const cdn_test::Record& r : env.records) {
    if (r.name == name) return &r;
  }
  return nullptr;
}

// ---- The settings (ADR 0048 decisions 3, 7, 12) ----------------------------------------------------------------------------------

void SettingsWithoutBaseUrlNeverParseAKey() {
  Lists lists{{"maps_official_keys", {"not a key"}}, {"maps_trusted_keys", {"not a key either"}}};
  const mc::Settings s = mc::ReadSettings(Scalars({}), Items(lists), mc::CompiledOfficialKeys());
  QCHECK(s.reason == "not_configured");
  QCHECK(s.keys.empty());
  QCHECK(mc::DecideStart(s) == mc::Start::kOff);
}

void SettingsReadTheBaseUrlWithItsSlashTheChannelAndTheBudget() {
  const Values v{{"maps_base_url", "https://maps.test"}, {"maps_channel", "echo"}, {"maps_boot_budget_ms", "2500"}};
  const mc::Settings s = mc::ReadSettings(Scalars(v), Items({}), mc::CompiledOfficialKeys());
  QCHECK(s.reason.empty());
  QCHECK(s.base_url == kBase);
  QCHECK(s.channel == "echo");
  QCHECK(s.boot_budget_ms == 2500);
  QCHECK(mc::DecideStart(s) == mc::Start::kRun);
}

// Decision 12: no official key is compiled in, so the compiled list is empty and only maps.official_keys adds one.
void NoOfficialKeyIsCompiledIn() { QCHECK(mc::CompiledOfficialKeys().empty()); }

void AConfiguredOfficialKeyIsOfficialButNeverCompiled() {
  Lists lists{{"maps_official_keys", {kTestPub}}};
  const mc::Settings s = mc::ReadSettings(Scalars(ConfiguredValues()), Items(lists), mc::CompiledOfficialKeys());
  QCHECK(s.reason.empty());
  QCHECK(s.keys.size() == 1);
  if (s.keys.size() == 1) {
    QCHECK(s.keys[0].official);
    QCHECK(!s.keys[0].compiled);  // ADR 0048 decision 7: a key from maps.official_keys never satisfies -require-signed-plugins
  }
}

void ACompiledKeyIsOfficialAndCompiled() {
  nevr_maps::PinnedKey compiled;
  QCHECK(nevr_minisign::ParsePublicKeyText(kTestPub, &compiled.key));
  const std::vector<nevr_maps::PinnedKey> built_in{compiled};
  const mc::Settings s = mc::ReadSettings(Scalars(ConfiguredValues()), Items({}), built_in);
  QCHECK(s.reason.empty());
  QCHECK(s.keys.size() == 1);
  if (s.keys.size() == 1) {
    QCHECK(s.keys[0].official);
    QCHECK(s.keys[0].compiled);
  }
}

void AnAuthorKeyIsNotOfficial() {
  Lists lists{{"maps_trusted_keys", {kTestPub}}};
  const mc::Settings s = mc::ReadSettings(Scalars(ConfiguredValues()), Items(lists), mc::CompiledOfficialKeys());
  QCHECK(s.reason.empty());
  QCHECK(s.keys.size() == 1);
  if (s.keys.size() == 1) {
    QCHECK(!s.keys[0].official);
    QCHECK(!s.keys[0].compiled);
  }
}

// Decision 3: official and author keys are never trusted for one base_url.
void OfficialAndAuthorKeysAreMixed() {
  Lists configured{{"maps_official_keys", {kTestPub}}, {"maps_trusted_keys", {kTestPub}}};
  const mc::Settings s = mc::ReadSettings(Scalars(ConfiguredValues()), Items(configured), mc::CompiledOfficialKeys());
  QCHECK(s.reason == "mixed_keys");
  QCHECK(mc::DecideStart(s) == mc::Start::kRefused);

  nevr_maps::PinnedKey compiled;
  QCHECK(nevr_minisign::ParsePublicKeyText(kTestPub, &compiled.key));
  Lists author_only{{"maps_trusted_keys", {kTestPub}}};
  const mc::Settings with_built_in = mc::ReadSettings(Scalars(ConfiguredValues()), Items(author_only), {compiled});
  QCHECK(with_built_in.reason == "mixed_keys");
}

void AKeyThatDoesNotParseIsRefusedByItsConfigKeyAndIndex() {
  Lists official{{"maps_official_keys", {"not a key"}}};
  const mc::Settings a = mc::ReadSettings(Scalars(ConfiguredValues()), Items(official), mc::CompiledOfficialKeys());
  QCHECK(a.reason == "bad_key");
  QCHECK(a.detail == "maps.official_keys[0]");
  QCHECK(mc::DecideStart(a) == mc::Start::kRefused);

  Lists trusted{{"maps_trusted_keys", {kTestPub, "not a key"}}};
  const mc::Settings b = mc::ReadSettings(Scalars(ConfiguredValues()), Items(trusted), mc::CompiledOfficialKeys());
  QCHECK(b.reason == "bad_key");
  QCHECK(b.detail == "maps.trusted_keys[1]");
}

// ---- The plan: groups, source ordering --------------------------------------------------------------------------------------------

void FetchGroupsAreThePathSafeIdsOnceLatestFirst() {
  const std::vector<std::string> recorded{kGroup, "../escape", "", "Upper", kGroup, "123"};
  const std::vector<std::string> plan = mc::FetchGroups(recorded);
  QCHECK(plan.size() == 2);
  if (plan.size() == 2) {
    QCHECK(plan[0] == kGroup);
    QCHECK(plan[1] == "123");
  }
}

void LocalShadowIdsAreSortedAndUnique() {
  const std::vector<std::string> ids = mc::LocalShadowIds({"b", "a", "b"});
  QCHECK(ids.size() == 2);
  if (ids.size() == 2) {
    QCHECK(ids[0] == "a");
    QCHECK(ids[1] == "b");
  }
}

// ---- The boot ----------------------------------------------------------------------------------------------------------------------

// No maps.base_url: no call to the Env beyond one Info record, no directory, no request, no hashing, no grant.
void WithoutBaseUrlTheChannelMakesNoCallAndWritesOneInfoRecord() {
  FakeEnv env;
  FakeMapFs fs(env);
  CountingMounter mounter;
  const mc::Settings s = mc::ReadSettings(Scalars({}), Items({}), mc::CompiledOfficialKeys());
  mc::Channel channel(PlatformConfig(), s);
  const mc::BootResult& result = channel.Boot(env, fs, mounter, nullptr, kNow, {});
  QCHECK(result.reason == "not_configured");
  QCHECK(channel.start() == mc::Start::kOff);
  QCHECK(env.records.size() == 1);
  const cdn_test::Record* record = FirstEvent(env, "map_start");
  QCHECK(record != nullptr);
  if (record != nullptr) {
    QCHECK(record->level == nevr_cdn::EventLevel::kInfo);
    QCHECK(record->Get("reason") == "not_configured");
  }
  QCHECK(env.requests.empty());
  QCHECK(env.fs_->dirs.empty());
  QCHECK(env.hashed_bytes == 0);
  QCHECK(mounter.calls == 0);
  QCHECK(channel.FetchPass(env, fs, nullptr, {kGroup}, kNow, [] { return true; }).empty());
  QCHECK(env.requests.empty());
}

// A refused configuration mounts nothing and writes one Warning naming the reason.
void ARefusedConfigurationMountsNothing() {
  FakeEnv env;
  FakeMapFs fs(env);
  CountingMounter mounter;
  Lists configured{{"maps_official_keys", {kTestPub}}, {"maps_trusted_keys", {kTestPub}}};
  const mc::Settings s = mc::ReadSettings(Scalars(ConfiguredValues()), Items(configured), mc::CompiledOfficialKeys());
  mc::Channel channel(PlatformConfig(), s);
  const mc::BootResult& result = channel.Boot(env, fs, mounter, nullptr, kNow, {});
  QCHECK(result.reason == "mixed_keys");
  const cdn_test::Record* record = FirstEvent(env, "map_start");
  QCHECK(record != nullptr);
  if (record != nullptr) {
    QCHECK(record->level == nevr_cdn::EventLevel::kWarn);
    QCHECK(record->Get("reason") == "mixed_keys");
  }
  QCHECK(env.requests.empty());
  QCHECK(mounter.calls == 0);
}

// The boot of a configured channel over an empty cache: it reads the cache index, reaches no network and hashes nothing.
void ABootOverAnEmptyCacheReachesNoNetworkAndHashesNothing() {
  FakeEnv env;
  FakeMapFs fs(env);
  CountingMounter mounter;
  const mc::Settings s = mc::ReadSettings(Scalars(ConfiguredValues()), Items({}), mc::CompiledOfficialKeys());
  mc::Channel channel(PlatformConfig(), s);
  const mc::BootResult& result = channel.Boot(env, fs, mounter, nullptr, kNow, {});
  QCHECK(result.reason == "ok");
  QCHECK(channel.booted());
  QCHECK(env.requests.empty());  // the boot thread makes no GET (the fetch is the worker's)
  QCHECK(env.hashed_bytes == 0);  // and hashes no byte
  QCHECK(mounter.calls == 0);
  const cdn_test::Record* record = FirstEvent(env, "map_start");
  QCHECK(record != nullptr);
  if (record != nullptr) {
    QCHECK(record->level == nevr_cdn::EventLevel::kInfo);
    QCHECK(record->Get("reason") == "ok");
  }
}

// A second Boot answers the first: the same result, no record, no call.
void ASecondBootIsIdempotent() {
  FakeEnv env;
  FakeMapFs fs(env);
  CountingMounter mounter;
  const mc::Settings s = mc::ReadSettings(Scalars(ConfiguredValues()), Items({}), mc::CompiledOfficialKeys());
  mc::Channel channel(PlatformConfig(), s);
  const mc::BootResult& first = channel.Boot(env, fs, mounter, nullptr, kNow, {"local"});
  const std::size_t records = env.records.size();
  const std::size_t requests = env.requests.size();
  const int calls = mounter.calls;
  const mc::BootResult& second = channel.Boot(env, fs, mounter, nullptr, kNow, {});
  QCHECK(&first == &second);
  QCHECK(env.records.size() == records);
  QCHECK(env.requests.size() == requests);
  QCHECK(mounter.calls == calls);
  QCHECK(channel.config().local_maps.size() == 1);  // the first boot's local ids stand
}

// The fetch is the worker's: nothing before Boot, nothing on the boot thread, one GET for the group's manifest on the worker.
void TheFetchRunsOnlyOnTheWorkerAfterBoot() {
  FakeEnv env;
  FakeMapFs fs(env);
  CountingMounter mounter;
  const mc::Settings s = mc::ReadSettings(Scalars(ConfiguredValues()), Items({}), mc::CompiledOfficialKeys());
  mc::Channel channel(PlatformConfig(), s);
  QCHECK(channel.FetchPass(env, fs, nullptr, {kGroup}, kNow, [] { return true; }).empty());  // before Boot: nothing
  QCHECK(env.requests.empty());
  channel.Boot(env, fs, mounter, nullptr, kNow, {});
  QCHECK(env.requests.empty());
  const std::vector<nevr_maps::FetchReport> reports =
      channel.FetchPass(env, fs, nullptr, {kGroup}, kNow, [] { return true; });
  QCHECK(reports.size() == 1);
  QCHECK(env.requests.size() == 1);
  if (env.requests.size() == 1) QCHECK(env.requests[0] == nevr_maps::GroupManifestUrl(kBase, kGroup));
}

// A server mounts the group it fetched at once, through the boot's own mount: MountGroups answers nothing before Boot ran the
// channel, and after Boot with nothing cached it mounts nothing and reaches no network.
void MountGroupsIsEmptyUntilBootAndMountsNothingUncached() {
  FakeEnv env;
  FakeMapFs fs(env);
  CountingMounter mounter;
  const mc::Settings s = mc::ReadSettings(Scalars(ConfiguredValues()), Items({}), mc::CompiledOfficialKeys());
  mc::Channel channel(PlatformConfig(), s);
  QCHECK(channel.MountGroups({kGroup}, kNow).mounted.empty());  // before Boot: no mount object exists
  QCHECK(env.requests.empty());
  channel.Boot(env, fs, mounter, nullptr, kNow, {});
  QCHECK(channel.MountGroups({kGroup}, kNow).mounted.empty());  // after Boot: the group is not cached, so nothing mounts
  QCHECK(env.requests.empty());
}

// A listing (ListGroups) names every map it reads and grants none: after Boot over an empty cache it lists nothing, mounts nothing,
// registers nothing and reaches no network. Before Boot there is no run to fold into, so the answer is empty.
void ListGroupsGrantsNothingAndListsNothingWithoutACache() {
  FakeEnv env;
  FakeMapFs fs(env);
  CountingMounter mounter;
  const mc::Settings s = mc::ReadSettings(Scalars(ConfiguredValues()), Items({}), mc::CompiledOfficialKeys());
  mc::Channel channel(PlatformConfig(), s);
  QCHECK(channel.ListGroups({kGroup}, kNow).listed.empty());  // before Boot: no mount object exists
  channel.Boot(env, fs, mounter, nullptr, kNow, {});
  const nevr_maps::MountReport listed = channel.ListGroups({kGroup}, kNow);
  QCHECK(listed.listed.empty());
  QCHECK(listed.mounted.empty());
  QCHECK(mounter.calls == 0);  // a listing never calls the mounter: nothing is granted
  QCHECK(env.requests.empty());
}

// The fold a listing applies to the run's report: an id the listing names replaces its earlier row, the rows of other ids stay, and
// a map this run has mounted never gains a not_mounted row (the listing is what plays after the next start, not a claim about a held map).
void FoldListingReplacesTheListedIdsAndKeepsTheRestOfTheRun() {
  nevr_maps::MountReport run;
  run.mounted = {"a"};
  run.listed = {{1, "a"}, {2, "b"}};
  run.not_mounted = {{"b", "not_asked"}, {"z", "not_cached"}};
  nevr_maps::MountReport listing;
  listing.listed = {{2, "b"}, {3, "c"}};
  listing.not_mounted = {{"b", "stale"}, {"c", "not_asked"}, {"a", "not_asked"}};
  mc::FoldListing(&run, listing);
  QCHECK(run.mounted == std::vector<std::string>{"a"});  // the run's mounts are not changed by a listing
  QCHECK(run.listed.size() == 3);                        // a's row, b's row once, c's row
  QCHECK(run.not_mounted.size() == 3);                   // z's old row, b's new row, c's row; a (mounted) gets none
  bool b_stale = false, z_kept = false, a_listed = false;
  for (const auto& row : run.not_mounted) {
    if (row.first == "b" && row.second == "stale") b_stale = true;
    if (row.first == "z" && row.second == "not_cached") z_kept = true;
    if (row.first == "a") a_listed = true;
  }
  QCHECK(b_stale);   // the listing's token for b replaced the earlier one
  QCHECK(z_kept);    // an id the listing does not name keeps its row
  QCHECK(!a_listed);  // a mounted map is never listed as not mounted
}

// ---- The mount observer's decision (both platforms call it) ------------------------------------------------------------------

// An expected owner no mounted set belongs to is withdrawn; a mounted owner is kept and not withdrawn.
void TheMountDecisionWithdrawsTheUnmountedAndKeepsTheRest() {
  const std::vector<std::string> expected{"map:a", "map:b"};
  const nevr_config_alias::OwnerLevels mounted{{"map:a", {1}}};
  std::vector<std::string> withdrawn;
  const mc::MountDecision d = mc::DecideMount(
      expected, mounted, [&withdrawn](const std::string& owner) { withdrawn.push_back(owner); },
      [](const std::string&, std::uint64_t) { return true; });
  QCHECK(d.mounted.size() == 1);
  if (d.mounted.size() == 1) QCHECK(d.mounted[0] == "map:a");
  QCHECK(d.withdrawn.size() == 1);
  if (d.withdrawn.size() == 1) QCHECK(d.withdrawn[0] == "map:b");
  QCHECK(withdrawn.size() == 1);
  if (withdrawn.size() == 1) QCHECK(withdrawn[0] == "map:b");
}

// ---- The worker ---------------------------------------------------------------------------------------------------------------------

// A worker that was never started has no thread: Stop answers at once, and no pass runs.
void AWorkerNeverStartedHasNoThread() {
  int passes = 0;
  mc::FetchWorker worker([&passes](const std::function<bool()>&) { ++passes; }, [] { return std::uint64_t{0}; });
  QCHECK(!worker.started());
  QCHECK(worker.Stop(std::chrono::milliseconds(0)));
  QCHECK(!worker.Request());
  QCHECK(passes == 0);
}

// A request inside the gap is refused; after the gap it is taken. The first request is taken at once.
void AWorkerRefusesARequestInsideTheGap() {
  std::uint64_t now = 0;
  std::atomic<int> passes{0};
  mc::FetchWorker worker([&passes](const std::function<bool()>&) { passes.fetch_add(1); }, [&now] { return now; });
  worker.Start();
  QCHECK(worker.started());
  QCHECK(worker.Request());
  QCHECK(!worker.Request());  // within kExtraFetchGapMs of the last extra pass
  now += mc::kExtraFetchGapMs;
  QCHECK(worker.Request());
  QCHECK(worker.Stop(std::chrono::milliseconds(5000)));
  // At most the initial pass and the two accepted requests run: the thread may stop before its first pass starts (Stop follows
  // Start at once), so the count is bounded above, not below.
  QCHECK(passes.load() <= 3);
}

// ---- The session pin's group requests and the recorded groups (ADR 0048 decisions 15 and 16) -------------------------------------------

// The armed request queues each group once, latest first, drops a path-unsafe id, and keeps the latest kRequestedGroupSlots (8).
// This is the only test that queues a group, so the queue is read from its empty start.
void TheGroupRequestsAreQueuedLatestFirstOnceAndBounded() {
  constexpr const char* kGameGroup = "44332211-6655-8877-99AA-BBCCDDEEFFB0";  // the game's text; the queued id is its lower case
  constexpr const char* kGameGroupId = "44332211-6655-8877-99aa-bbccddeeffb0";
  mc::ArmSessionGroupRequest();
  std::string id;
  QCHECK(nevr_maps::RequestSessionGroup(kGameGroup, &id) == nevr_maps::SessionGroupRequest::kRequested);
  QCHECK(id == kGameGroupId);
  QCHECK(mc::RequestedGroups() == std::vector<std::string>{kGameGroupId});
  mc::RecordRequestedGroup("../escape");  // not path safe: not queued
  QCHECK(mc::RequestedGroups() == std::vector<std::string>{kGameGroupId});
  mc::RecordRequestedGroup("a");
  mc::RecordRequestedGroup(kGameGroupId);  // already queued: it moves to the front, once
  QCHECK((mc::RequestedGroups() == std::vector<std::string>{kGameGroupId, "a"}));
  std::vector<std::string> expected;
  for (int i = 0; i < 8; ++i) {
    const std::string group = "n" + std::to_string(i);
    mc::RecordRequestedGroup(group);
    expected.insert(expected.begin(), group);
  }
  // Eight new groups: the list keeps the latest eight, and the older game group and "a" are dropped.
  QCHECK(mc::RequestedGroups() == expected);
  QCHECK(mc::RequestedGroups().size() == 8);
}

// The recorded groups are the requests in their order, then the cached groups none of them names.
void TheRecordedGroupsAreTheRequestsThenTheCachedOnes() {
  const std::vector<std::string> merged = mc::MergeRecordedGroups({"b", "a"}, {"c", "a", "b", "d"});
  QCHECK((merged == std::vector<std::string>{"b", "a", "c", "d"}));
  QCHECK(mc::MergeRecordedGroups({}, {}).empty());
  QCHECK((mc::MergeRecordedGroups({}, {"x"}) == std::vector<std::string>{"x"}));
}

// The seam answers this run's requests with an empty cache index (no group is cached in an empty directory), and it reads the
// requests when it is asked, not when it is made.
void TheRecordedGroupSeamAnswersTheRequestsAsTheyAreAskedFor() {
  cdn_test::FakeEnv env;
  const mc::Seams seams = mc::RecordedGroupSeams(env, "/cache/maps");
  QCHECK(seams.recorded_groups != nullptr);
  QCHECK(seams.recorded_groups() == mc::RequestedGroups());
  mc::RecordRequestedGroup("late-group");
  QCHECK(seams.recorded_groups().front() == "late-group");
  QCHECK(!static_cast<bool>(seams.local_map_ids));  // no local copy is listed on either platform
}

}  // namespace

int main() {
  SettingsWithoutBaseUrlNeverParseAKey();
  SettingsReadTheBaseUrlWithItsSlashTheChannelAndTheBudget();
  NoOfficialKeyIsCompiledIn();
  AConfiguredOfficialKeyIsOfficialButNeverCompiled();
  ACompiledKeyIsOfficialAndCompiled();
  AnAuthorKeyIsNotOfficial();
  OfficialAndAuthorKeysAreMixed();
  AKeyThatDoesNotParseIsRefusedByItsConfigKeyAndIndex();
  FetchGroupsAreThePathSafeIdsOnceLatestFirst();
  LocalShadowIdsAreSortedAndUnique();
  WithoutBaseUrlTheChannelMakesNoCallAndWritesOneInfoRecord();
  ARefusedConfigurationMountsNothing();
  ABootOverAnEmptyCacheReachesNoNetworkAndHashesNothing();
  ASecondBootIsIdempotent();
  TheFetchRunsOnlyOnTheWorkerAfterBoot();
  MountGroupsIsEmptyUntilBootAndMountsNothingUncached();
  ListGroupsGrantsNothingAndListsNothingWithoutACache();
  FoldListingReplacesTheListedIdsAndKeepsTheRestOfTheRun();
  AWorkerNeverStartedHasNoThread();
  AWorkerRefusesARequestInsideTheGap();
  TheMountDecisionWithdrawsTheUnmountedAndKeepsTheRest();
  TheGroupRequestsAreQueuedLatestFirstOnceAndBounded();
  TheRecordedGroupsAreTheRequestsThenTheCachedOnes();
  TheRecordedGroupSeamAnswersTheRequestsAsTheyAreAskedFor();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "map_channel_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("map_channel_test: all checks passed\n");
  return 0;
}
