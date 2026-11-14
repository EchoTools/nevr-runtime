// The map channel's fetch and mount (core/map_run.h; docs/adr/0048-custom-maps-channel.md) over the shared fake Env and its MapFs
// (quest/tests/cargo_fake_env.h, map_fake_fs.h), a scripted server, a recording Mounter and Deriver, and the real parsers fed
// group manifests this test signs with fixed-seed keys. No network, no Win32, no game file. Log assertions read each record's
// fields.
#include "core/map_run.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/base64.h"
#include "core/cargo_limits.h"
#include "core/map_fetch_queue.h"
#include "core/map_index.h"
#include "core/map_set_tables.h"
#include "monocypher-ed25519.h"
#include "monocypher.h"
#include "nevr/resource_set.h"
#include "nevr/symbol_hash.h"
#include "quest/tests/cargo_fake_env.h"
#include "quest/tests/map_fake_fs.h"

#include "quest/tests/map_run_fixture.h"

namespace {

using namespace map_run_fixture;

// The GTest fixture: the shared fixture (map_run_fixture::Fixture), with the assertions a test makes on its own reads.
class MapRunTest : public ::testing::Test, public Fixture {
 protected:
  // The index a test asserts on: its read's status is checked here, so a failed read fails the test.
  CacheIndex Index() {
    CacheIndex index;
    EXPECT_EQ(ReadCacheIndex(env, kDir, &index), CacheIndexStatus::kOk);
    return index;
  }
  // Every handle the mount opened is the mounter's, or closed.
  void ExpectNoLeak() { EXPECT_EQ(env.open_blobs(), mounter.Owned()); }
};


TEST_F(MapRunTest, AnOfflineMountMountsFromTheCacheWithoutNetworkOrMemberHashing) {
  const std::string set_bytes = MakeSet(6, 'a');
  channel.Publish("alpha", "1.0.0", set_bytes);
  channel.Sign(1);
  const FetchReport fetched = Fetch();
  EXPECT_EQ(fetched.reason, "");
  EXPECT_EQ(fetched.outcome, FetchOutcome::kFetched);
  EXPECT_EQ(fetched.maps_updated, 1u);
  EXPECT_EQ(fetched.pieces_fetched, 7u);

  env.server.clear();
  const std::size_t gets = env.requests.size();
  env.hashed_bytes = 0;
  const MountReport boot = Boot();
  EXPECT_EQ(env.requests.size(), gets);
  EXPECT_LT(env.hashed_bytes, set_bytes.size());
  ASSERT_TRUE(Mounted(boot, "alpha"));
  EXPECT_TRUE(mounter.verified["map:alpha"]);
  EXPECT_EQ(mounter.labels["map:alpha"], "map:alpha@1.0.0");
  EXPECT_EQ(mounter.set_sha["map:alpha"], Sha(set_bytes));
  ExpectNoLeak();
  const Record* summary = env.FindRecord("map_summary");
  ASSERT_NE(summary, nullptr);
  EXPECT_EQ(summary->level, nevr_cargo::EventLevel::kInfo);
  EXPECT_EQ(summary->Get("mounted"), "1");
}

// Each byte is hashed once, as it arrives: the fetch hashes the pieces and the package index, and never a stored file.
TEST_F(MapRunTest, TheFetchHashesEachByteOnceWhileItArrives) {
  const Package& p = channel.Publish("alpha", "1.0.0", MakeSet(6, 'a'));
  const std::string index = env.server[std::string(kBase) + "blobs/sha256/" + channel.IndexSha("alpha")].body;
  channel.Sign(1);
  env.hashed_bytes = 0;
  ASSERT_EQ(Fetch().outcome, FetchOutcome::kFetched);
  EXPECT_EQ(env.hashed_bytes, p.members[0].size + index.size());
}

TEST_F(MapRunTest, AScriptRefusedSecondOfThreeWithdrawsAndRegistersNoSet) {
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'), {{1, "dll one"}, {2, "dll two"}, {3, "dll three"}});
  channel.Sign(1);
  Fetch();
  mounter.refuse_scripts = {2};
  const MountReport boot = Boot();
  EXPECT_FALSE(Mounted(boot, "alpha"));
  EXPECT_EQ(mounter.calls, (std::vector<std::string>{"script map:alpha 1", "script map:alpha 2", "withdraw map:alpha"}));
  EXPECT_EQ(TokenOf(boot, "alpha"), "script:pe_rules");
  ExpectNoLeak();
  EXPECT_EQ(env.open_blobs(), 0u);
}

TEST_F(MapRunTest, ASetRefusedAtItsGrantWithdrawsTheScripts) {
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'), {{1, "dll one"}});
  channel.Sign(1);
  Fetch();
  mounter.refuse_sets = {"map:alpha"};
  const MountReport boot = Boot();
  EXPECT_FALSE(Mounted(boot, "alpha"));
  EXPECT_TRUE(mounter.Called("withdraw map:alpha"));
  EXPECT_EQ(TokenOf(boot, "alpha"), "collides");
  EXPECT_EQ(env.open_blobs(), 0u);
}

TEST_F(MapRunTest, TwoMapsSharingAScriptHashTheLowerIdWins) {
  channel.Publish("beta", "1.0.0", MakeSet(2, 'b'), {{9, "shared dll"}});
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'), {{9, "shared dll"}});
  channel.Sign(1);
  Fetch();
  const MountReport boot = Boot();
  EXPECT_TRUE(Mounted(boot, "alpha"));
  EXPECT_FALSE(Mounted(boot, "beta"));
  EXPECT_EQ(TokenOf(boot, "beta"), "script:duplicate");
  const Record* line = env.FindRecord("map_mounted", "id", "beta");
  ASSERT_NE(line, nullptr);
  EXPECT_EQ(line->Get("holder"), "map:alpha");
  EXPECT_FALSE(mounter.Called("script map:beta 9"));
  ExpectNoLeak();
}

TEST_F(MapRunTest, ALocalMapIsSkippedWithNoGrant) {
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'), {{1, "dll"}});
  channel.Sign(1);
  Fetch();
  config.local_maps = {"alpha"};
  const MountReport boot = Boot();
  EXPECT_EQ(TokenOf(boot, "alpha"), "local_map");
  EXPECT_TRUE(mounter.calls.empty());
  EXPECT_EQ(env.open_blobs(), 0u);
}

TEST_F(MapRunTest, ACrashBeforeTheIndexLeavesTheOldVersionMountedAndTheNextGcRemovesTheNewDirectory) {
  channel.Publish("alpha", "1.0.0", MakeSet(4, 'a'));
  channel.Sign(1);
  Fetch();
  const std::string v1_dir = channel.VersionDir("alpha");
  channel.Publish("alpha", "1.1.0", MakeSet(4, 'b'));
  channel.Sign(2);
  const std::string v2_dir = channel.VersionDir("alpha");
  env.fail_writes = {std::string(kDir) + "/index.json"};
  const FetchReport crashed = Fetch();
  EXPECT_EQ(crashed.reason, "index_write");
  EXPECT_EQ(crashed.outcome, FetchOutcome::kFailed);
  EXPECT_TRUE(fs.HasDir(v2_dir));

  const MountReport boot = Boot();
  ASSERT_EQ(boot.staged.size(), 1u);
  EXPECT_EQ(boot.staged[0].version, "1.0.0");

  env.fail_writes.clear();
  env.server.clear();  // offline: the GC runs, the fetch then fails at the manifest
  Fetch();
  EXPECT_FALSE(fs.HasDir(v2_dir));
  EXPECT_TRUE(fs.HasDir(v1_dir));
}

// ADR 0048 decision 7: under -require-signed-plugins a map's scripts count only when an official key signed its manifest; a
// script-free map still mounts.
TEST_F(MapRunTest, UnderSignedOnlyOnlyAnOfficialManifestsScriptsMount) {
  channel.Publish("plain", "1.0.0", MakeSet(2, 'a'));
  channel.Publish("scripted", "1.0.0", MakeSet(2, 'b'), {{1, "dll"}});
  channel.Sign(1);
  Fetch();
  config.signed_only = true;
  MountReport boot = Boot();
  EXPECT_TRUE(Mounted(boot, "plain"));
  EXPECT_EQ(TokenOf(boot, "scripted"), "signed_only");

  TestKey official{1, true};
  config.keys = {official.pin};
  mounter.calls.clear();
  boot = Boot();
  EXPECT_TRUE(Mounted(boot, "scripted"));
}

// A cached version no verified manifest pins any longer (its newer version is not stored yet) still mounts, but an official
// manifest that pins another version does not vouch for its scripts.
TEST_F(MapRunTest, AnOfficialManifestDoesNotVouchForAVersionItDoesNotPin) {
  TestKey official{1, true};
  config.keys = {official.pin};
  channel.Publish("scripted", "1.0.0", MakeSet(2, 'a'), {{1, "dll"}});
  channel.Sign(1);
  Fetch();
  channel.Publish("scripted", "1.1.0", MakeSet(2, 'b'), {{1, "dll two"}});
  channel.Sign(2);
  env.fail_writes = {std::string(kDir) + "/index.json"};  // the new manifest is stored, the new version is not
  Fetch();
  config.signed_only = true;
  EXPECT_EQ(TokenOf(Boot(), "scripted"), "signed_only");
  config.signed_only = false;
  const MountReport boot = Boot();
  ASSERT_EQ(boot.staged.size(), 1u);
  EXPECT_EQ(boot.staged[0].version, "1.0.0");
}

TEST_F(MapRunTest, TheSummaryListsEveryRefusalAndRefusedGroup) {
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'), {{1, "dll"}});
  channel.Publish("beta", "1.0.0", MakeSet(2, 'b'));
  channel.Publish("gamma", "1.0.0", MakeSet(2, 'c'), {}, [](Package& p) { AddAlias(p); });
  channel.Sign(1);
  Fetch();
  mounter.refuse_scripts = {1};
  mounter.refuse_sets = {"map:beta"};
  mounter.alias["map:gamma"] = Grant{false, "alias_partial"};
  const MountReport boot = Boot(nullptr, kNow, {kGroup, kOtherGroup});
  EXPECT_FALSE(Mounted(boot, "gamma"));
  const Record* summary = env.FindRecord("map_summary");
  ASSERT_NE(summary, nullptr);
  EXPECT_EQ(summary->level, nevr_cargo::EventLevel::kWarn);
  EXPECT_EQ(summary->Get("mounted"), "0");
  EXPECT_EQ(summary->Get("refused"), "alpha:script:pe_rules,beta:collides,gamma:alias_partial");
  EXPECT_EQ(summary->Get("groups_refused"), std::string(kOtherGroup) + ":no_manifest");
}

// Decision 5, both platforms: a map with two aliases, one of them refused, keeps nothing it registered (its set and its script
// are withdrawn and their files closed), is refused once as alias_partial, and frees its script hash, so a later map with the
// same script still mounts. The mounter only stages; the core withdraws.
TEST_F(MapRunTest, AMapWithARefusedAliasIsWithdrawnWholeAndFreesItsScripts) {
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'), {{9, "shared dll"}}, [](Package& p) {
    AddAlias(p);
    ConfigAlias refused;
    refused.key_name = "mp_combat_rules";
    refused.alias_name = p.id + "_combat_rules";
    refused.key = EchoVR::CSymbol64Hash(refused.key_name.c_str());
    refused.alias = EchoVR::CSymbol64Hash(refused.alias_name.c_str());
    p.config_aliases.push_back(refused);
  });
  channel.Publish("beta", "1.0.0", MakeSet(2, 'b'), {{9, "shared dll"}});
  channel.Sign(1);
  Fetch();
  mounter.alias["map:alpha"] = Grant{false, ""};
  const MountReport boot = Boot();
  EXPECT_FALSE(Mounted(boot, "alpha"));
  EXPECT_EQ(TokenOf(boot, "alpha"), "alias_partial");
  EXPECT_TRUE(Mounted(boot, "beta"));
  EXPECT_TRUE(mounter.Called("set map:alpha"));
  EXPECT_TRUE(mounter.Called("withdraw map:alpha"));
  EXPECT_EQ(mounter.Owned("map:alpha"), 0u);
  EXPECT_EQ(std::count_if(boot.not_mounted.begin(), boot.not_mounted.end(),
                          [](const auto& r) { return r.first == "alpha" && r.second == "alias_partial"; }),
            1);
  EXPECT_EQ(std::count_if(boot.refused.begin(), boot.refused.end(), [](const auto& r) { return r.first == "alpha"; }), 1);
  ASSERT_EQ(boot.staged.size(), 1u);
  EXPECT_EQ(boot.staged[0].id, "beta");
  const Record* summary = env.FindRecord("map_summary");
  ASSERT_NE(summary, nullptr);
  EXPECT_EQ(summary->Get("mounted"), "1");
  EXPECT_EQ(summary->Get("refused"), "alpha:alias_partial");
}

TEST_F(MapRunTest, ANullDeriverRefusesARecipeMapAndARecipeFreeMapStillMounts) {
  channel.Publish("plain", "1.0.0", MakeSet(2, 'a'));
  channel.Publish("recipe", "1.0.0", MakeSet(2, 'b'), {}, [](Package& p) { AddRecipe(p); });
  channel.Sign(1);
  Fetch();
  EXPECT_NE(env.FindRecord("map_derive", "reason", "derived_unsupported"), nullptr);
  const MountReport boot = Boot();
  EXPECT_TRUE(Mounted(boot, "plain"));
  EXPECT_EQ(TokenOf(boot, "recipe"), "not_cached");
}

TEST_F(MapRunTest, ARecipeMapMountsItsMergedSetAndItsRebuiltScript) {
  constexpr std::uint64_t kRebuiltHash = 0x77, kStockScript = 0x55;
  constexpr std::size_t kScriptBytes = 128, kEditAt = 16;
  deriver.rebuilt_scripts[kRebuiltHash] = std::string(kScriptBytes, 'R');
  channel.Publish("recipe", "1.0.0", MakeSet(3, 'r'), {}, [&](Package& p) {
    AddRecipe(p);
    RekeyPin pin;
    pin.hash = kRebuiltHash;
    pin.stock_source = kStockScript;
    pin.stock_sha256 = Sha("stock dll");
    pin.edits.push_back(FieldEdit{kEditAt, sizeof(std::uint64_t), p.level});
    pin.result_sha256 = Sha(std::string(kScriptBytes, 'R'));
    pin.result_size = kScriptBytes;
    p.rekeyed.push_back(pin);
  });
  channel.Sign(1);
  EXPECT_EQ(Fetch(&deriver).maps_updated, 1u);
  EXPECT_EQ(deriver.derive_calls, 1);
  const CacheIndex index = Index();
  ASSERT_EQ(index.maps.count("recipe/windows-x86_64"), 1u);
  EXPECT_EQ(index.maps.at("recipe/windows-x86_64").stock_stamps.size(), 1u);  // one per distinct path

  const MountReport boot = Boot(&deriver);
  ASSERT_TRUE(Mounted(boot, "recipe"));
  EXPECT_TRUE(mounter.Called("derived map:recipe 119"));
  EXPECT_TRUE(mounter.verified["map:recipe"]);
  EXPECT_EQ(deriver.stock_checked.size(), 1u);
  ExpectNoLeak();
}

TEST_F(MapRunTest, StockCurrentRunsOncePerDistinctStockFileAcrossMaps) {
  channel.Publish("one", "1.0.0", MakeSet(2, 'a'), {}, [](Package& p) { AddRecipe(p); });
  channel.Publish("two", "1.0.0", MakeSet(2, 'b'), {}, [](Package& p) { AddRecipe(p); });
  channel.Sign(1);
  Fetch(&deriver);
  const MountReport boot = Boot(&deriver);
  EXPECT_EQ(boot.mounted.size(), 2u);
  EXPECT_EQ(deriver.stock_checked, (std::vector<std::string>{"C:/game/_data/packages/stock0"}));
}

TEST_F(MapRunTest, AStockChangeDefersTheMapAndOneFetchRebuildsItForTheNextMount) {
  channel.Publish("recipe", "1.0.0", MakeSet(2, 'r'), {}, [](Package& p) { AddRecipe(p); });
  channel.Sign(1);
  Fetch(&deriver);
  deriver.current = false;
  const MountReport deferred = Boot(&deriver);
  EXPECT_EQ(TokenOf(deferred, "recipe"), "stock_changed");
  EXPECT_EQ(deferred.deferred, (std::vector<std::string>{"recipe"}));

  const std::size_t blob_gets = GetsUnder(std::string(kBase) + "blobs/");
  Fetch(&deriver, deferred.deferred);  // the index sha is unchanged; the fetch must rebuild anyway
  EXPECT_EQ(deriver.derive_calls, 2);
  EXPECT_EQ(GetsUnder(std::string(kBase) + "blobs/"), blob_gets);
  EXPECT_NE(env.FindRecord("map_rehash", "result", "rebuilt"), nullptr);

  deriver.current = true;
  mounter.calls.clear();
  EXPECT_TRUE(Mounted(Boot(&deriver), "recipe"));
}

TEST_F(MapRunTest, ABadFrameRecordsFramesCheckedFalseAndTheSetNeverTakesTheVerifiedPath) {
  std::string set_bytes = MakeSet(3, 'a');
  set_bytes[FramesStart(3) + kFrameBytes] = 0;  // frame 1 loses its zstd magic
  channel.Publish("alpha", "1.0.0", set_bytes);
  channel.Sign(1);
  Fetch();
  EXPECT_FALSE(Index().maps.at("alpha/windows-x86_64").files[0].frames_checked);
  const Record* line = env.FindRecord("map_member", "frames", "frame_magic");
  ASSERT_NE(line, nullptr);
  EXPECT_EQ(line->Get("index"), "1");
  EXPECT_TRUE(Mounted(Boot(), "alpha"));  // the registry's own frame check still runs at the mount
  EXPECT_FALSE(mounter.verified["map:alpha"]);
}

// The set file's tables and every frame header, judged by the cargo validator (core/map_set_tables.h).
TEST_F(MapRunTest, TheSetCheckJudgesEveryFrameHeader) {
  const std::uint64_t table_bytes = std::uint64_t{1} << 20;
  fs.Put("C:/t/good", MakeSet(5, 'x'));
  EXPECT_EQ(CheckSetFile(env, "C:/t/good", table_bytes).reason, "");

  std::string size = MakeSet(5, 'x');
  size[FramesStart(5) + 2 * kFrameBytes + 5] = 99;  // frame 2 states 99 bytes, the chunk 100
  fs.Put("C:/t/size", size);
  SetProblem problem = CheckSetFile(env, "C:/t/size", table_bytes);
  EXPECT_EQ(problem.reason, "content_size_mismatch");
  EXPECT_EQ(problem.index, 2u);

  std::string range = MakeSet(2, 'x');
  range.resize(range.size() - 1);  // the last frame runs past the end
  fs.Put("C:/t/range", range);
  problem = CheckSetFile(env, "C:/t/range", table_bytes);
  EXPECT_EQ(problem.reason, "chunk_range");
  EXPECT_EQ(problem.index, 1u);

  fs.Put("C:/t/short", "NEVRRES1");
  EXPECT_EQ(CheckSetFile(env, "C:/t/short", table_bytes).reason, "too_small");
  // Tables the memory share cannot hold are not read.
  EXPECT_EQ(CheckSetFile(env, "C:/t/good", SetTableMemoryBytes(5, 5) - 1).reason, "no_memory");
  EXPECT_EQ(env.open_blobs(), 0u);
}

TEST_F(MapRunTest, TheBootBudgetSkipsTheLaterIds) {
  for (const char* id : {"alpha", "beta", "gamma"}) channel.Publish(id, "1.0.0", MakeSet(2, id[0]));
  channel.Sign(1);
  Fetch();
  config.boot_budget_ms = 100;
  mounter.set_cost_ms = 60;
  const MountReport boot = Boot();
  EXPECT_EQ(boot.mounted, (std::vector<std::string>{"alpha", "beta"}));
  EXPECT_EQ(TokenOf(boot, "gamma"), "boot_budget");
  EXPECT_NE(env.FindRecord("map_summary", "skipped", "gamma:boot_budget"), nullptr);
}

TEST_F(MapRunTest, StagedListedAndNotMountedAreFilled) {
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  channel.Publish("beta", "1.0.0", MakeSet(2, 'b'));
  channel.Publish("other", "1.0.0", MakeSet(2, 'o'));
  channel.Sign(1);
  Fetch();
  config.local_maps = {"beta"};
  const MountReport boot = Boot();
  ASSERT_EQ(boot.staged.size(), 2u);
  const MountedMap& staged = boot.staged[0];
  EXPECT_EQ(staged.id, "alpha");
  EXPECT_EQ(staged.version, "1.0.0");
  EXPECT_EQ(staged.index_sha256, channel.IndexSha("alpha"));
  EXPECT_EQ(staged.platform, kPlatformPc);
  EXPECT_EQ(staged.source_sha256, Sha("source alpha"));
  EXPECT_EQ(staged.title, "Title alpha");
  EXPECT_EQ(staged.group, kGroup);
  EXPECT_EQ(staged.level, EchoVR::CSymbol64Hash("alpha"));
  ASSERT_EQ(boot.listed.size(), 3u);
  EXPECT_EQ(boot.listed[2], std::make_pair(EchoVR::CSymbol64Hash("other"), std::string("other")));
  EXPECT_EQ(boot.not_mounted, (std::vector<std::pair<std::string, std::string>>{{"beta", "local_map"}}));
}

TEST_F(MapRunTest, AChannelSwitchKeepsBothFloors) {
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  channel.Sign(42);
  Fetch();
  channel.name = "echo-other";
  channel.Sign(7);
  config.channel = "echo-other";
  EXPECT_EQ(Fetch().reason, "");
  const CacheIndex index = Index();
  EXPECT_EQ(index.floors.channels.at("echo-test").at(kGroup).at(kKeyId).serial, 42u);
  EXPECT_EQ(index.floors.channels.at("echo-other").at(kGroup).at(kKeyId).serial, 7u);

  config.channel.clear();  // bound on first use: base_url now names echo-other, so echo-test's manifest is refused
  channel.name = "echo-test";
  channel.Sign(43);
  EXPECT_EQ(Fetch().reason, "wrong_channel");
}

TEST_F(MapRunTest, RevokedByIdByIndexShaAndByPieceShaAreNotMounted) {
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  channel.Publish("beta", "1.0.0", MakeSet(2, 'b'));
  const Package gamma = channel.Publish("gamma", "1.0.0", MakeSet(2, 'c'));
  channel.Publish("plain", "1.0.0", MakeSet(2, 'p'));
  channel.Sign(1);
  Fetch();
  // Revoked in the stored manifest (written by a fetch whose plan the revocation also reaches).
  channel.Sign(2, "2027-01-01T00:00:00Z", {"alpha"}, {channel.IndexSha("beta"), gamma.members[0].pieces[1].sha256});
  Fetch();
  const MountReport boot = Boot();
  EXPECT_EQ(boot.mounted, (std::vector<std::string>{"plain"}));
  EXPECT_EQ(TokenOf(boot, "alpha"), "revoked");
  EXPECT_EQ(TokenOf(boot, "beta"), "revoked");
  EXPECT_EQ(TokenOf(boot, "gamma"), "revoked");
}

TEST_F(MapRunTest, ExpiredTheScriptFreeMapMountsAndTheScriptedOneDoesNot) {
  channel.Publish("plain", "1.0.0", MakeSet(2, 'a'));
  channel.Publish("scripted", "1.0.0", MakeSet(2, 'b'), {{1, "dll"}});
  channel.Sign(1, "2026-12-01T00:00:00Z");
  Fetch();
  const MountReport boot = Boot(nullptr, "2026-12-01T00:00:00Z");
  EXPECT_TRUE(Mounted(boot, "plain"));
  EXPECT_EQ(TokenOf(boot, "scripted"), "expired");
}

TEST_F(MapRunTest, AReplayedOldSerialIsRefusedAndTheStoredManifestIsKept) {
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  const std::string old_manifest = channel.Sign(4);
  const std::string current = channel.Sign(5);
  Fetch();
  env.server[channel.ManifestUrl()].body = old_manifest;
  const FetchReport replay = Fetch();
  EXPECT_EQ(replay.reason, "serial_down");
  EXPECT_EQ(replay.outcome, FetchOutcome::kFailed);
  std::string stored;
  ASSERT_TRUE(env.ReadSmallFile(StoredGroupManifestPath(kDir, kGroup), &stored, 1 << 20));
  EXPECT_EQ(stored, current);
  EXPECT_TRUE(Mounted(Boot(), "alpha"));
}

TEST_F(MapRunTest, AnUpdateThatChangesOneFrameFetchesOnePieceAndReusesTheRest) {
  const std::string v1 = MakeSet(6, 'a');
  channel.Publish("alpha", "1.0.0", v1);
  channel.Sign(1);
  Fetch();
  std::string v2 = v1;
  v2[FramesStart(6) + 3 * kFrameBytes + 20] ^= 1;  // a byte of frame 3's payload
  channel.Publish("alpha", "1.1.0", v2);
  channel.Sign(2);
  env.hashed_bytes = 0;
  const FetchReport update = Fetch();
  EXPECT_EQ(update.maps_updated, 1u);
  EXPECT_EQ(update.pieces_fetched, 1u);
  EXPECT_EQ(update.pieces_reused, 6u);
  // A reused piece is copied, not hashed again: only the fetched piece and the package index are hashed.
  EXPECT_EQ(env.hashed_bytes, kFrameBytes + env.server[std::string(kBase) + "blobs/sha256/" + channel.IndexSha("alpha")].body.size());
  EXPECT_EQ(Boot().staged.at(0).version, "1.1.0");
}

// A cached file whose stamp moved since its bytes were verified is not trusted for any piece it holds: they are fetched.
TEST_F(MapRunTest, APieceInACachedFileWhoseStampMovedIsFetched) {
  const std::string v1 = MakeSet(6, 'a');
  channel.Publish("alpha", "1.0.0", v1);
  channel.Sign(1);
  Fetch();
  const std::string set_path = channel.VersionDir("alpha") + "/" + kSetPath;
  std::string altered = v1;
  altered[FramesStart(6) + 30] ^= 1;  // frame 0 on disk
  fs.Put(set_path, altered);
  std::string v2 = v1;
  v2[FramesStart(6) + 5 * kFrameBytes + 30] ^= 1;  // the update changes frame 5
  channel.Publish("alpha", "1.1.0", v2);
  channel.Sign(2);
  const FetchReport update = Fetch();
  EXPECT_EQ(update.pieces_fetched, 7u);
  EXPECT_EQ(update.pieces_reused, 0u);
  EXPECT_EQ(Boot().staged.at(0).version, "1.1.0");
}

TEST_F(MapRunTest, AStampMismatchIsDeferredNotHashedAndTheRehashRefreshesIt) {
  const std::string set_bytes = MakeSet(6, 'a');
  channel.Publish("alpha", "1.0.0", set_bytes);
  channel.Sign(1);
  Fetch();
  fs.Touch(channel.VersionDir("alpha") + "/" + kSetPath);
  env.hashed_bytes = 0;
  const MountReport boot = Boot();
  EXPECT_EQ(TokenOf(boot, "alpha"), "stamp");
  EXPECT_LT(env.hashed_bytes, set_bytes.size());
  EXPECT_TRUE(mounter.calls.empty());
  EXPECT_EQ(env.open_blobs(), 0u);

  Fetch(nullptr, boot.deferred);
  EXPECT_NE(env.FindRecord("map_rehash", "result", "refreshed"), nullptr);
  EXPECT_TRUE(Mounted(Boot(), "alpha"));
}

TEST_F(MapRunTest, ARehashThatFindsOtherBytesDropsTheVersionAndRefetchesIt) {
  const std::string set_bytes = MakeSet(3, 'a');
  channel.Publish("alpha", "1.0.0", set_bytes);
  channel.Sign(1);
  Fetch();
  std::string altered = set_bytes;
  altered[FramesStart(3) + 40] ^= 1;
  fs.Put(channel.VersionDir("alpha") + "/" + kSetPath, altered);
  const MountReport boot = Boot();
  EXPECT_EQ(TokenOf(boot, "alpha"), "stamp");
  const FetchReport repair = Fetch(nullptr, boot.deferred);
  EXPECT_NE(env.FindRecord("map_rehash", "result", "dropped"), nullptr);
  EXPECT_EQ(repair.maps_updated, 1u);
  EXPECT_EQ(repair.pieces_fetched, 4u);  // the altered file's stamp moved, so none of its pieces is copied
  mounter.calls.clear();
  EXPECT_TRUE(Mounted(Boot(), "alpha"));
}

// ADR 0048 decision 4, ADR 0045 decision 7: the mount holds a package index to its pin by the stamp recorded when the fetch hashed it,
// and hashes nothing. Bytes replaced under an unchanged stamp are read as the ones verified: a mount that hashed the index would
// find index_pin and defer the map instead. No hasher the env makes is fed a byte.
TEST_F(MapRunTest, TheMountHoldsThePackageIndexToItsPinByItsStampAndHashesNothing) {
  channel.Publish("alpha", "1.0.0", MakeSet(3, 'a'));
  channel.Sign(1);
  Fetch();
  const std::string package = channel.VersionDir("alpha") + "/package.json";
  std::string& bytes = env.fs_->files.at(package);
  const std::string key = "\"source_sha256\":\"";
  const std::size_t at = bytes.find(key);
  ASSERT_NE(at, std::string::npos);
  const std::size_t last = at + key.size() + 63;  // the source sha's last hex digit; the same size, so the same stamp
  bytes[last] = bytes[last] == '0' ? '1' : '0';
  const std::string replaced_source = bytes.substr(at + key.size(), 64);
  env.hashed_bytes = 0;
  const MountReport boot = Boot();
  ASSERT_TRUE(Mounted(boot, "alpha")) << TokenOf(boot, "alpha");
  EXPECT_EQ(boot.staged.at(0).source_sha256, replaced_source) << "the mount read the bytes on disk, held to the pin by the stamp";
  EXPECT_EQ(env.hashed_bytes, 0u);
  EXPECT_EQ(env.open_blobs(), Index().maps.at("alpha/windows-x86_64").files.size()) << "the index's handle is closed again";
}

// A package index whose stamp moved defers the map to the worker, which hashes the index again: equal bytes record the new stamp and
// the next mount takes the map.
TEST_F(MapRunTest, AMovedPackageIndexStampDefersTheMapAndTheRehashSettlesIt) {
  channel.Publish("alpha", "1.0.0", MakeSet(3, 'a'));
  channel.Sign(1);
  Fetch();
  const std::string package = channel.VersionDir("alpha") + "/package.json";
  fs.Touch(package);
  const MountReport boot = Boot();
  EXPECT_EQ(TokenOf(boot, "alpha"), "package_stamp");
  EXPECT_TRUE(mounter.calls.empty());
  Fetch(nullptr, boot.deferred);
  EXPECT_NE(env.FindRecord("map_rehash", "result", "refreshed"), nullptr);
  EXPECT_TRUE(Mounted(Boot(), "alpha"));
}

// An index entry whose package stamp was never taken (all zero) holds nothing to: the mount defers rather than trust the bytes.
TEST_F(MapRunTest, ANeverStampedPackageIndexDefersTheMap) {
  channel.Publish("alpha", "1.0.0", MakeSet(3, 'a'));
  channel.Sign(1);
  Fetch();
  CacheIndex index = Index();
  index.maps.at("alpha/windows-x86_64").index_stamp = FileStamp();
  ASSERT_TRUE(WriteCacheIndex(env, kDir, index));
  EXPECT_EQ(TokenOf(Boot(), "alpha"), "package_stamp");
}

TEST_F(MapRunTest, APackageIndexWithOtherBytesIsDroppedByTheRehashAndFetchedAgain) {
  channel.Publish("alpha", "1.0.0", MakeSet(3, 'a'));
  channel.Sign(1);
  Fetch();
  const std::string package = channel.VersionDir("alpha") + "/package.json";
  std::string other = env.fs_->files.at(package);
  other += " ";
  fs.Put(package, other);
  const MountReport boot = Boot();
  EXPECT_EQ(TokenOf(boot, "alpha"), "package_stamp");  // other bytes move the stamp; the mount defers, it does not hash
  const FetchReport repair = Fetch(nullptr, boot.deferred);
  EXPECT_NE(env.FindRecord("map_rehash", "reason", "index_hash"), nullptr);
  EXPECT_EQ(repair.maps_updated, 1u);
  mounter.calls.clear();
  EXPECT_TRUE(Mounted(Boot(), "alpha"));
}

// ADR 0045 decision 8: what a fetch adds to the disk is a share of the free space the device reports, never a constant.
TEST_F(MapRunTest, AMapOverTheStorageShareIsSkippedAndTheOthersContinue) {
  channel.Publish("alpha", "1.0.0", MakeSet(10, 'a'));
  channel.Publish("beta", "1.0.0", MakeSet(2, 'b'));
  channel.Sign(1);
  const std::uint64_t beta = MakeSet(2, 'b').size();
  std::uint64_t free_bytes = beta;
  while (free_bytes - free_bytes / nevr_cargo::kStorageShareKept < beta) ++free_bytes;
  env.device.free_bytes = free_bytes;
  const FetchReport fetched = Fetch();
  EXPECT_NE(env.FindRecord("map_plan", "reason", "over_budget"), nullptr);
  EXPECT_EQ(fetched.outcome, FetchOutcome::kFailed);
  EXPECT_EQ(fetched.reason, "over_budget");
  EXPECT_EQ(fetched.maps_updated, 1u);
  const MountReport boot = Boot();
  EXPECT_EQ(boot.mounted, (std::vector<std::string>{"beta"}));
}

// A fact the device did not give stops the fetch with its name, before any request.
TEST_F(MapRunTest, AMissingDeviceFactStopsTheFetchWithItsName) {
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  channel.Sign(1);
  env.device.have_free = false;
  const FetchReport fetched = Fetch();
  EXPECT_EQ(fetched.outcome, FetchOutcome::kFailed);
  EXPECT_EQ(fetched.reason, "no_free_space_fact");
  EXPECT_TRUE(env.requests.empty());
  EXPECT_NE(env.FindRecord("map_limits", "reason", "no_free_space_fact"), nullptr);
}

TEST_F(MapRunTest, MembersOverTheEntrysBytesAreRefusedBeforeAnyFetch) {
  channel.Publish("alpha", "1.0.0", MakeSet(10, 'a'));
  channel.maps["alpha"]["platforms"][kPlatformPc]["bytes"] = 1;
  channel.Sign(1);
  Fetch();
  EXPECT_NE(env.FindRecord("map_index", "reason", "bytes_mismatch"), nullptr);
  EXPECT_TRUE(Boot().mounted.empty());
}

TEST_F(MapRunTest, AMissingIndexMountsNothing) {
  const MountReport boot = Boot();
  EXPECT_TRUE(boot.mounted.empty());
  EXPECT_NE(env.FindRecord("map_mount_group", "reason", "no_index"), nullptr);
  EXPECT_TRUE(mounter.calls.empty());
}

TEST_F(MapRunTest, AMalformedIndexIsKeptAsBadAndTreatedAsEmpty) {
  fs.Put(std::string(kDir) + "/index.json", "{not json");
  EXPECT_TRUE(Boot().mounted.empty());
  EXPECT_NE(env.FindRecord("map_mount_group", "index", "invalid"), nullptr);
  EXPECT_TRUE(fs.Has(std::string(kDir) + "/index.json.bad"));
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  channel.Sign(1);
  EXPECT_EQ(Fetch().maps_updated, 1u);
  EXPECT_TRUE(Mounted(Boot(), "alpha"));
}

TEST_F(MapRunTest, NoFileIdDefersWithStampUnsupported) {
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  channel.Sign(1);
  Fetch();
  fs.stamps_supported = false;
  EXPECT_EQ(TokenOf(Boot(), "alpha"), "stamp_unsupported");
  EXPECT_EQ(env.open_blobs(), 0u);
}

TEST_F(MapRunTest, AShutdownMidDownloadKeepsItsPiecesAndTheNextPassResumes) {
  const std::size_t pieces = PieceCount(channel.Publish("alpha", "1.0.0", MakeSet(6, 'a')));
  channel.Sign(1);
  int gets = 0;
  env.allow_get = [&] {
    if (++gets > 4) keep = false;  // shutdown after the manifest, the index and two pieces
    return true;
  };
  EXPECT_EQ(Fetch().reason, "cancelled");
  const std::string staging = channel.VersionDir("alpha") + ".tmp";
  EXPECT_TRUE(fs.HasDir(staging));
  EXPECT_TRUE(fs.Has(staging + "/resume/state.json"));
  EXPECT_EQ(Boot().mounted.size(), 0u);

  env.allow_get = nullptr;
  keep = true;
  const FetchReport resumed = Fetch();
  EXPECT_EQ(resumed.maps_updated, 1u);
  EXPECT_EQ(resumed.pieces_reused, 2u);
  EXPECT_EQ(resumed.pieces_fetched, pieces - 2);
  EXPECT_FALSE(fs.HasDir(staging));
  EXPECT_EQ(Boot().staged.at(0).version, "1.0.0");
}

// Quest asks every GET with a range from byte 0, which a CDN answers 206 with the whole object: it is accepted as 200 is.
TEST_F(MapRunTest, APartialContentAnswerCarryingTheWholeObjectIsAccepted) {
  channel.Publish("alpha", "1.0.0", MakeSet(3, 'a'));
  channel.Sign(1);
  for (auto& served : env.server) served.second.status = nevr_cargo::kHttpPartialContent;
  const FetchReport fetched = Fetch();
  EXPECT_EQ(fetched.outcome, FetchOutcome::kFetched);
  EXPECT_TRUE(Mounted(Boot(), "alpha"));
}

TEST_F(MapRunTest, APieceLostInTransportKeepsTheOthersAndTheNextPassFetchesOnlyWhatIsMissing) {
  const Package& p = channel.Publish("alpha", "1.0.0", MakeSet(6, 'a'));
  const std::size_t pieces = PieceCount(p);
  ASSERT_GE(p.members[0].pieces.size(), 3u);
  const std::string url = std::string(kBase) + "blobs/sha256/" + p.members[0].pieces[2].sha256;
  channel.Sign(1);
  env.server[url].status = 503;
  const FetchReport lost = Fetch();
  EXPECT_EQ(lost.maps_updated, 0u);
  EXPECT_EQ(lost.outcome, FetchOutcome::kFailed);
  EXPECT_EQ(lost.reason, "http_503");
  EXPECT_NE(env.FindRecord("map_piece", "reason", "http_503"), nullptr);
  const std::string staging = channel.VersionDir("alpha") + ".tmp";
  EXPECT_TRUE(fs.HasDir(staging));

  env.server[url].status = 200;
  const FetchReport resumed = Fetch();
  EXPECT_EQ(resumed.maps_updated, 1u);
  EXPECT_EQ(resumed.pieces_reused, 2u);
  EXPECT_EQ(resumed.pieces_fetched, pieces - 2);
  EXPECT_FALSE(fs.HasDir(staging));
}

// The kept bytes are trusted only while the staging file's stamp is the one recorded when the transfer stopped.
TEST_F(MapRunTest, AStagedMemberAlteredOnDiskIsFetchedAgainFromItsStart) {
  const Package& p = channel.Publish("alpha", "1.0.0", MakeSet(6, 'a'));
  const std::size_t pieces = PieceCount(p);
  channel.Sign(1);
  int gets = 0;
  env.allow_get = [&] {
    if (++gets > 4) keep = false;
    return true;
  };
  EXPECT_EQ(Fetch().reason, "cancelled");
  const std::string staged = channel.VersionDir("alpha") + ".tmp/" + kSetPath;
  ASSERT_TRUE(fs.Has(staged));
  std::string bytes = env.fs_->files[staged];
  bytes[0] ^= 1;
  fs.Put(staged, bytes);

  env.allow_get = nullptr;
  keep = true;
  const FetchReport resumed = Fetch();
  EXPECT_EQ(resumed.maps_updated, 1u);
  EXPECT_EQ(resumed.pieces_reused, 0u);
  EXPECT_EQ(resumed.pieces_fetched, pieces);
  EXPECT_TRUE(Mounted(Boot(), "alpha"));
}

TEST_F(MapRunTest, AnUnfinishedDownloadOfASupersededVersionIsRemoved) {
  const std::string v1 = MakeSet(6, 'a');
  channel.Publish("alpha", "1.0.0", v1);
  channel.Sign(1);
  int gets = 0;
  env.allow_get = [&] {
    if (++gets > 4) keep = false;
    return true;
  };
  EXPECT_EQ(Fetch().reason, "cancelled");
  const std::string old_staging = channel.VersionDir("alpha") + ".tmp";
  ASSERT_TRUE(fs.HasDir(old_staging));

  std::string v2 = v1;
  v2[FramesStart(6) + 3 * kFrameBytes + 20] ^= 1;
  channel.Publish("alpha", "1.1.0", v2);
  channel.Sign(2);
  env.allow_get = nullptr;
  keep = true;
  EXPECT_EQ(Fetch().maps_updated, 1u);
  EXPECT_FALSE(fs.HasDir(old_staging));
  EXPECT_NE(env.FindRecord("map_gc", "reason", "superseded"), nullptr);
  EXPECT_EQ(Boot().staged.at(0).version, "1.1.0");
}

TEST_F(MapRunTest, APieceHashMismatchIsRetriedThreeTimesThenFailsTheMap) {
  const Package& p = channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  channel.Sign(1);
  const std::string url = std::string(kBase) + "blobs/sha256/" + p.members[0].pieces[1].sha256;
  env.corrupt_times[url] = 3;
  EXPECT_EQ(Fetch().maps_updated, 1u);
  EXPECT_EQ(GetsOf(url), 4u);

  const Package& q = channel.Publish("beta", "1.0.0", MakeSet(2, 'q'));  // frames alpha does not hold
  channel.Sign(2);
  const std::string bad = std::string(kBase) + "blobs/sha256/" + q.members[0].pieces[1].sha256;
  env.corrupt_times[bad] = 4;
  const FetchReport failed = Fetch();
  EXPECT_EQ(failed.maps_updated, 0u);
  EXPECT_EQ(failed.reason, "piece_hash");
  EXPECT_NE(env.FindRecord("map_piece", "reason", "piece_hash"), nullptr);
  EXPECT_FALSE(fs.HasDir(channel.VersionDir("beta") + ".tmp"));
}

TEST_F(MapRunTest, AMapTheGroupNoLongerListsIsRemovedByTheNextGc) {
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  channel.Sign(1);
  Fetch();
  channel.Unpublish("alpha");
  channel.Sign(2);
  Fetch();  // the group's list loses alpha at the end of this pass
  Fetch();  // and this pass's GC removes it
  EXPECT_FALSE(fs.HasDir(std::string(kDir) + "/alpha"));
  EXPECT_TRUE(Index().maps.empty());
}

// The audit's scenario: testers trusted a test key on channel echo-test, then switch to the official key, whose manifest carries
// the same channel name with a lower serial and a lower map version.
TEST_F(MapRunTest, SwitchingToTheOfficialKeyStartsItsOwnFloorAndSaysSoOnce) {
  channel.Publish("alpha", "1.3.0", MakeSet(2, 'a'));
  channel.Sign(40);
  ASSERT_EQ(Fetch().reason, "");

  TestKey official_key{2, true};
  Channel official{env, official_key};
  official.Publish("alpha", "1.0.0", MakeSet(2, 'b'));
  official.Sign(1);
  config.keys = {official_key.pin};
  EXPECT_EQ(Fetch().reason, "");
  EXPECT_EQ(Index().maps.at(MapKey("alpha", kPlatformPc)).version, "1.0.0");
  const Record* changed = env.FindRecord("map_trust_changed");
  ASSERT_NE(changed, nullptr);
  EXPECT_EQ(changed->Get("untrusted_keys"), kKeyId);
  EXPECT_EQ(changed->Get("key_id"), "A9A8A7A6A5A4A3A2");
  EXPECT_EQ(changed->Get("channel"), "echo-test");
  EXPECT_EQ(changed->Get("group"), kGroup);
  EXPECT_TRUE(Boot().mounted == std::vector<std::string>{"alpha"});

  official.Sign(2);
  EXPECT_EQ(Fetch().reason, "");
  official.Sign(1);
  EXPECT_EQ(Fetch().reason, "serial_down");  // a rollback under the same key
  EXPECT_EQ(env.CountRecords("map_trust_changed"), 1u);
}

// ---- Groups (ADR 0048): one manifest per lobby group, keyed by its opaque id; the cache keeps the union of the groups' maps.

// A fetch keyed by one group requests that group's manifest and the objects of its maps, and nothing of another group.
TEST_F(MapRunTest, AGroupsFetchRequestsOnlyThatGroupsManifestAndObjects) {
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  channel.Sign(1);
  TestKey other_key{1};
  Channel other{env, other_key};
  other.group = kOtherGroup;
  const Package& beta = other.Publish("beta", "1.0.0", MakeSet(2, 'q'));  // frames alpha does not hold
  other.Sign(1);

  const FetchReport fetched = Fetch();
  EXPECT_EQ(fetched.outcome, FetchOutcome::kFetched);
  EXPECT_EQ(fetched.group, kGroup);
  EXPECT_EQ(GetsOf(channel.ManifestUrl()), 1u);
  EXPECT_EQ(GetsOf(other.ManifestUrl()), 0u);
  EXPECT_EQ(GetsUnder(std::string(kBase) + "guilds/" + kOtherGroup), 0u);
  EXPECT_EQ(GetsOf(std::string(kBase) + "blobs/sha256/" + other.IndexSha("beta")), 0u);
  // Every piece but the first: two sets of one size share their header and tables, which alpha's fetch got.
  for (std::size_t i = 1; i < beta.members[0].pieces.size(); ++i) {
    EXPECT_EQ(GetsOf(std::string(kBase) + "blobs/sha256/" + beta.members[0].pieces[i].sha256), 0u);
  }
  EXPECT_EQ(Index().maps.count(MapKey("beta", kPlatformPc)), 0u);
  EXPECT_EQ(CachedGroupIds(env, kDir), (std::vector<std::string>{kGroup}));
}

TEST_F(MapRunTest, AManifestThatNamesAnotherGroupIsRefused) {
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  channel.Sign(1, "2027-01-01T00:00:00Z", {}, {}, kOtherGroup);
  const FetchReport fetched = Fetch();
  EXPECT_EQ(fetched.outcome, FetchOutcome::kFailed);
  EXPECT_EQ(fetched.reason, "guild_mismatch");
  EXPECT_FALSE(fs.Has(StoredGroupManifestPath(kDir, kGroup)));
  EXPECT_TRUE(Index().maps.empty());
  EXPECT_NE(env.FindRecord("map_manifest", "reason", "guild_mismatch"), nullptr);

  // A stored copy under the wrong group is refused by the mount too.
  channel.Sign(1);
  ASSERT_EQ(Fetch().outcome, FetchOutcome::kFetched);
  fs.Put(StoredGroupManifestPath(kDir, kOtherGroup), env.fs_->files[StoredGroupManifestPath(kDir, kGroup)]);
  const MountReport boot = Boot(nullptr, kNow, {kOtherGroup});
  EXPECT_TRUE(boot.mounted.empty());
  EXPECT_EQ(boot.groups_refused, (std::vector<std::pair<std::string, std::string>>{{kOtherGroup, "guild_mismatch"}}));
}

TEST_F(MapRunTest, AGroupIdThatIsNotPathSafeIsRefusedBeforeAnyRequest) {
  const FetchReport fetched = Fetch(nullptr, {}, "../escape");
  EXPECT_EQ(fetched.outcome, FetchOutcome::kFailed);
  EXPECT_EQ(fetched.reason, "bad_group");
  EXPECT_TRUE(env.requests.empty());
}

// A map two groups list at one version is fetched once: the second group's fetch finds it cached.
TEST_F(MapRunTest, AMapTwoGroupsListIsFetchedOnce) {
  const std::string set_bytes = MakeSet(3, 'a');
  channel.Publish("alpha", "1.0.0", set_bytes);
  channel.Sign(1);
  Channel other{env, key};
  other.group = kOtherGroup;
  other.Publish("alpha", "1.0.0", set_bytes);
  other.Sign(1);

  ASSERT_EQ(Fetch().outcome, FetchOutcome::kFetched);
  const std::size_t blob_gets = GetsUnder(std::string(kBase) + "blobs/");
  const FetchReport second = Fetch(nullptr, {}, kOtherGroup);
  EXPECT_EQ(second.outcome, FetchOutcome::kCached);
  EXPECT_EQ(GetsUnder(std::string(kBase) + "blobs/"), blob_gets);
  EXPECT_NE(env.FindRecord("map_plan", "reason", "current"), nullptr);
  const MountReport boot = Boot(nullptr, kNow, {kOtherGroup, kGroup});
  EXPECT_EQ(boot.mounted, (std::vector<std::string>{"alpha"}));
}

// A piece two maps share is fetched once: the second map copies it from the first map's stored file.
TEST_F(MapRunTest, APieceTwoMapsShareIsFetchedOnce) {
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'), {{1, "shared dll"}});
  channel.Publish("beta", "1.0.0", MakeSet(2, 'q'), {{2, "shared dll"}});
  channel.Sign(1);
  const FetchReport fetched = Fetch();
  EXPECT_EQ(fetched.maps_updated, 2u);
  EXPECT_EQ(GetsOf(std::string(kBase) + "blobs/sha256/" + Sha("shared dll")), 1u);
  EXPECT_EQ(fetched.pieces_reused, 2u);  // the script, and the sets' header and tables, which the two sets share
}

// The client's source: the groups it fetched are kept in its cache index, the latest first, and a launch mounts and fetches
// them again from there. No config key names a group.
TEST_F(MapRunTest, TheClientKeepsTheGroupsItFetchedTheLatestFirst) {
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  channel.Sign(1);
  Channel other{env, key};
  other.group = kOtherGroup;
  other.Publish("beta", "1.0.0", MakeSet(2, 'b'));
  other.Sign(1);
  Fetch();
  Fetch(nullptr, {}, kOtherGroup);
  EXPECT_EQ(CachedGroupIds(env, kDir), (std::vector<std::string>{kOtherGroup, kGroup}));
  Fetch();  // played in the first group again: it becomes the latest
  const std::vector<std::string> groups = CachedGroupIds(env, kDir);
  EXPECT_EQ(groups, (std::vector<std::string>{kGroup, kOtherGroup}));

  const MountReport boot = Boot(nullptr, kNow, groups);
  EXPECT_EQ(boot.mounted, (std::vector<std::string>{"alpha", "beta"}));

  env.requests.clear();
  for (const std::string& group : groups) EXPECT_EQ(Fetch(nullptr, {}, group).outcome, FetchOutcome::kCached);
  EXPECT_EQ(env.requests, (std::vector<std::string>{channel.ManifestUrl(), other.ManifestUrl()}));
}

// Two groups list one map at different versions: the cache holds the version of the group fetched last, and a mount, whatever
// the order of its groups, mounts that version under the group whose manifest pins it.
TEST_F(MapRunTest, TheGroupFetchedLastDecidesAMapsVersion) {
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  channel.Sign(1);
  Channel other{env, key};
  other.group = kOtherGroup;
  other.Publish("alpha", "1.1.0", MakeSet(2, 'b'));
  other.Sign(1);
  Fetch();
  Fetch(nullptr, {}, kOtherGroup);
  EXPECT_EQ(Index().maps.at(MapKey("alpha", kPlatformPc)).version, "1.1.0");
  for (const std::vector<std::string>& groups : {std::vector<std::string>{kOtherGroup, kGroup},
                                                 std::vector<std::string>{kGroup, kOtherGroup}}) {
    const MountReport boot = Boot(nullptr, kNow, groups);
    ASSERT_EQ(boot.staged.size(), 1u);
    EXPECT_EQ(boot.staged[0].version, "1.1.0");
    EXPECT_EQ(boot.staged[0].group, kOtherGroup);
  }
}

// How many groups the cache keeps is bounded by the device's storage share, the oldest dropped first; the maps only a dropped
// group listed go with it.
TEST_F(MapRunTest, TheOldestGroupIsDroppedWhenTheStorageShareCannotHoldIt) {
  constexpr int kChunks = 40;  // a set larger than its group's manifest, so the maps decide what a group costs
  channel.Publish("alpha", "1.0.0", MakeSet(kChunks, 'a'));
  channel.Sign(1);
  Channel other{env, key};
  other.group = kOtherGroup;
  other.Publish("beta", "1.0.0", MakeSet(kChunks, 'b'));
  other.Sign(1);
  constexpr const char* kThirdGroup = "third-group";
  Channel third{env, key};
  third.group = kThirdGroup;
  third.Sign(1);  // lists no map
  Fetch();
  Fetch(nullptr, {}, kOtherGroup);

  env.device.free_bytes = 1;  // what the cache holds now, and almost nothing more
  const FetchReport fetched = Fetch(nullptr, {}, kThirdGroup);
  EXPECT_EQ(fetched.outcome, FetchOutcome::kCached);
  EXPECT_EQ(CachedGroupIds(env, kDir), (std::vector<std::string>{kThirdGroup, kOtherGroup}));
  EXPECT_NE(env.FindRecord("map_gc", "group", kGroup), nullptr);
  EXPECT_FALSE(fs.HasDir(std::string(kDir) + "/alpha"));
  EXPECT_TRUE(fs.HasDir(std::string(kDir) + "/beta"));
  EXPECT_FALSE(fs.Has(StoredGroupManifestPath(kDir, kGroup)));
}

// The mount can run again (after a fetch, off the boot thread): a map it mounted is kept, not granted twice, and a map that
// arrived since is mounted.
TEST_F(MapRunTest, TheMountCanRunAgainAndMountsOnlyWhatArrived) {
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'), {{1, "dll"}});
  channel.Sign(1);
  Fetch();
  CacheMount mount(env, fs, config, mounter, nullptr);
  const MountReport first = mount.Mount({kGroup}, kNow);
  EXPECT_EQ(first.mounted, (std::vector<std::string>{"alpha"}));
  const std::size_t grants = mounter.calls.size();

  MountReport again = mount.Mount({kGroup}, kNow);
  EXPECT_TRUE(again.mounted.empty());
  EXPECT_EQ(again.kept, (std::vector<std::string>{"alpha"}));
  EXPECT_EQ(mounter.calls.size(), grants);
  ExpectNoLeak();

  Channel other{env, key};
  other.group = kOtherGroup;
  other.Publish("beta", "1.0.0", MakeSet(2, 'b'));
  other.Sign(1);
  Fetch(nullptr, {}, kOtherGroup);
  again = mount.Mount({kOtherGroup, kGroup}, kNow);
  EXPECT_EQ(again.mounted, (std::vector<std::string>{"beta"}));
  EXPECT_EQ(again.kept, (std::vector<std::string>{"alpha"}));
  EXPECT_FALSE(mounter.Called("script map:beta 1"));

  // A version that arrived for a mounted map is not swapped in this process.
  channel.Publish("alpha", "1.1.0", MakeSet(2, 'c'), {{1, "dll"}});
  channel.Sign(2);
  Fetch();
  again = mount.Mount({kGroup}, kNow);
  EXPECT_EQ(TokenOf(again, "alpha"), "mounted_other_version");
}

// The mount holds every file for the process: the files it takes are bounded by a share of the descriptors the device reports.
TEST_F(MapRunTest, TheMountHoldsNoMoreFilesThanTheDescriptorShare) {
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'), {{1, "dll one"}, {2, "dll two"}});
  channel.Sign(1);
  Fetch();
  constexpr std::uint64_t kFilesInShare = 2;  // alpha holds 3: its set and two scripts
  env.device.fds_open = env.device.fd_soft_limit - kFilesInShare * kDescriptorShareForMaps;
  const MountReport boot = Boot();
  EXPECT_EQ(TokenOf(boot, "alpha"), "descriptors");
  EXPECT_EQ(env.open_blobs(), 0u);
  env.device.have_fds = false;
  EXPECT_TRUE(Boot().mounted.empty());
  EXPECT_NE(env.FindRecord("map_mount_group", "reason", "no_descriptor_fact"), nullptr);
}

// The size of the set before any piece, and the bytes fetched with the time taken as the fetch goes: what a caller needs to bound
// a wait by the download rate.
TEST_F(MapRunTest, TheFetchReportsTheSetsSizeFirstAndThenItsProgress) {
  const Package& p = channel.Publish("alpha", "1.0.0", MakeSet(4, 'a'));
  channel.Sign(1);
  std::vector<FetchProgress> seen;
  std::size_t piece_gets_at_first = 0;
  const FetchReport fetched = Fetch(nullptr, {}, kGroup, [&](const FetchProgress& progress) {
    if (seen.empty()) piece_gets_at_first = GetsUnder(std::string(kBase) + "blobs/");
    seen.push_back(progress);
  });
  ASSERT_GE(seen.size(), 2u);
  EXPECT_EQ(seen.front().set_bytes, p.members[0].size);
  EXPECT_EQ(seen.front().bytes_to_fetch, p.members[0].size);
  EXPECT_EQ(seen.front().bytes_fetched, 0u);
  EXPECT_EQ(piece_gets_at_first, 0u);  // known from the manifest, before the index or any piece
  EXPECT_EQ(seen.back().bytes_fetched, p.members[0].size);
  for (std::size_t i = 1; i < seen.size(); ++i) {
    EXPECT_GE(seen[i].bytes_fetched, seen[i - 1].bytes_fetched);
    EXPECT_GT(seen[i].elapsed_ms, seen[i - 1].elapsed_ms);
  }
  EXPECT_EQ(fetched.set_bytes, p.members[0].size);
  EXPECT_EQ(fetched.bytes_fetched, p.members[0].size);
  EXPECT_GT(fetched.elapsed_ms, 0u);
}

// ---- The request queue (core/map_fetch_queue.h).

FetchReport Answer(const std::string& group, FetchOutcome outcome) {
  FetchReport r;
  r.group = group;
  r.outcome = outcome;
  return r;
}

TEST(GroupFetchQueue, ARequestIsAnsweredOnTheWorkerWithTheFetchsOutcome) {
  GroupFetchQueue queue;
  std::vector<std::string> fetched;
  std::vector<std::string> heard;
  std::string reason;
  ASSERT_TRUE(queue.Request(kGroup, [&](const FetchReport& r) { heard.push_back(r.group + ":" + FetchOutcomeName(r.outcome)); },
                            &reason));
  EXPECT_TRUE(queue.WaitForRequest(0));
  EXPECT_TRUE(queue.RunNext([&](const std::string& group) {
    fetched.push_back(group);
    return Answer(group, FetchOutcome::kCached);
  }));
  EXPECT_EQ(fetched, (std::vector<std::string>{kGroup}));
  EXPECT_EQ(heard, (std::vector<std::string>{std::string(kGroup) + ":cached"}));
  EXPECT_FALSE(queue.RunNext([&](const std::string& group) { return Answer(group, FetchOutcome::kFailed); }));
  EXPECT_FALSE(queue.WaitForRequest(0));
}

TEST(GroupFetchQueue, TwoRequestsForAWaitingGroupShareOneFetch) {
  GroupFetchQueue queue;
  int fetches = 0, heard = 0;
  std::string reason;
  for (int i = 0; i < 2; ++i) ASSERT_TRUE(queue.Request(kGroup, [&](const FetchReport&) { ++heard; }, &reason));
  ASSERT_TRUE(queue.Request(kOtherGroup, [&](const FetchReport&) { ++heard; }, &reason));
  EXPECT_EQ(queue.Waiting(), 2u);
  while (queue.RunNext([&](const std::string& group) {
    ++fetches;
    return Answer(group, FetchOutcome::kFetched);
  })) {
  }
  EXPECT_EQ(fetches, 2);
  EXPECT_EQ(heard, 3);
}

// A request that arrives while its group is being fetched waits for a fetch of its own: the running one may have read the
// manifest already.
TEST(GroupFetchQueue, ARequestDuringItsGroupsFetchGetsAFetchOfItsOwn) {
  GroupFetchQueue queue;
  int fetches = 0;
  std::string reason;
  ASSERT_TRUE(queue.Request(kGroup, nullptr, &reason));
  queue.RunNext([&](const std::string& group) {
    ++fetches;
    EXPECT_TRUE(queue.Request(group, nullptr, &reason));
    return Answer(group, FetchOutcome::kFetched);
  });
  EXPECT_EQ(queue.Waiting(), 1u);
  queue.RunNext([&](const std::string& group) {
    ++fetches;
    return Answer(group, FetchOutcome::kCached);
  });
  EXPECT_EQ(fetches, 2);
}

TEST(GroupFetchQueue, ABadGroupIsRefusedAndStopAnswersEveryWaitingRequest) {
  GroupFetchQueue queue;
  std::string reason;
  EXPECT_FALSE(queue.Request("Not/Safe", [](const FetchReport&) { FAIL() << "a refused request is never answered"; }, &reason));
  EXPECT_EQ(reason, "bad_group");
  std::vector<std::string> heard;
  ASSERT_TRUE(queue.Request(kGroup, [&](const FetchReport& r) { heard.push_back(FetchOutcomeName(r.outcome) + (":" + r.reason)); },
                            &reason));
  queue.Stop();
  EXPECT_EQ(heard, (std::vector<std::string>{"failed:cancelled"}));
  EXPECT_FALSE(queue.Request(kGroup, nullptr, &reason));
  EXPECT_EQ(reason, "stopped");
  EXPECT_FALSE(queue.WaitForRequest(0));
}

}  // namespace
