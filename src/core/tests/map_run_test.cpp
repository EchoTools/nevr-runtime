#include "core/map_run.h"

#include <gtest/gtest.h>

#include <cstring>
#include <nlohmann/json.hpp>

#include "core/base64.h"
#include "core/map_index.h"
#include "core/tests/fake_map_env.h"
#include "monocypher-ed25519.h"
#include "monocypher.h"
#include "nevr/symbol_hash.h"

namespace {

using namespace nevr_maps;
using namespace nevr_maps_test;
using json = nlohmann::json;

constexpr const char* kDir = "C:/nevr/cache/maps";
constexpr const char* kBase = "https://maps.test/";
constexpr const char* kNow = "2026-11-11T00:00:00Z";
constexpr const char* kSetPath = "set/windows-x86_64.nevrres";
constexpr std::uint32_t kFrameBytes = 64, kChunkBytes = 100;

void PutU32(std::string* s, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) s->push_back(static_cast<char>(v >> (8 * i)));
}
void PutU64(std::string* s, std::uint64_t v) {
  PutU32(s, static_cast<std::uint32_t>(v));
  PutU32(s, static_cast<std::uint32_t>(v >> 32));
}

std::size_t FramesStart(int chunks) { return 48 + 48 * static_cast<std::size_t>(chunks); }

// A resource set of `chunks` records, one per chunk, each chunk a 64-byte zstd frame whose header states 100 bytes.
std::string MakeSet(int chunks, char salt) {
  std::string s("NEVRRES1", 8);
  const std::uint64_t records = 48, table = records + 32ull * chunks, overrides = table + 16ull * chunks;
  PutU32(&s, 48);
  PutU32(&s, chunks);
  PutU32(&s, chunks);
  PutU32(&s, 0);
  PutU64(&s, records);
  PutU64(&s, table);
  PutU64(&s, overrides);
  for (int i = 0; i < chunks; ++i) {
    PutU64(&s, 0x1111);
    PutU64(&s, static_cast<std::uint64_t>(i + 1));
    PutU32(&s, i);
    PutU32(&s, 0);
    PutU32(&s, kChunkBytes);
    PutU32(&s, 16);
  }
  for (int i = 0; i < chunks; ++i) {
    PutU32(&s, 0);
    PutU32(&s, static_cast<std::uint32_t>(FramesStart(chunks) + kFrameBytes * i));
    PutU32(&s, kFrameBytes);
    PutU32(&s, kChunkBytes);
  }
  for (int i = 0; i < chunks; ++i) {
    PutU32(&s, 0xFD2FB528u);
    s.push_back(static_cast<char>(0xA0));  // single segment, a 4-byte content size
    PutU32(&s, kChunkBytes);
    s.append(kFrameBytes - 9, static_cast<char>(salt + i));
  }
  return s;
}

std::string ScriptName(std::uint64_t hash) {
  char name[32];
  std::snprintf(name, sizeof name, "scripts/%016llx.dll", static_cast<unsigned long long>(hash));
  return name;
}

std::string Hex64(std::uint64_t value) {
  char text[19];
  std::snprintf(text, sizeof text, "0x%016llx", static_cast<unsigned long long>(value));
  return text;
}

// A fixed-seed minisign key (the PC4 test's TestKey, the "ED" form only).
struct TestKey {
  std::uint8_t secret[64];
  PinnedKey pin;

  explicit TestKey(std::uint8_t seed_byte) {
    std::uint8_t seed[32];
    std::memset(seed, seed_byte, sizeof seed);
    crypto_ed25519_key_pair(secret, pin.key.pk.data(), seed);
    for (std::size_t i = 0; i < 8; ++i) pin.key.key_id[i] = static_cast<std::uint8_t>(0xA0 + seed_byte + i);
  }

  std::string SignedObject(const std::string& body) const {
    std::uint8_t digest[64];
    crypto_blake2b(digest, sizeof digest, reinterpret_cast<const std::uint8_t*>(body.data()), body.size());
    std::vector<std::uint8_t> blob = {'E', 'D'};
    blob.insert(blob.end(), pin.key.key_id.begin(), pin.key.key_id.end());
    std::uint8_t signature[64];
    crypto_ed25519_sign(signature, secret, digest, sizeof digest);
    blob.insert(blob.end(), signature, signature + 64);
    const std::string comment = "timestamp:1762819200";
    std::vector<std::uint8_t> global_message(signature, signature + 64);
    global_message.insert(global_message.end(), comment.begin(), comment.end());
    std::uint8_t global[64];
    crypto_ed25519_sign(global, secret, global_message.data(), global_message.size());
    return "untrusted comment: signature from a test key\n" + base64_encode(blob) + "\ntrusted comment: " + comment +
           "\n" + base64_encode(global, sizeof global) + "\n--\n" + body;
  }
};

json PinJson(const Pin& pin) { return {{"sha256", pin.sha256}, {"size", pin.size}}; }

json EditJson(const FieldEdit& e) {
  return {{"at", e.at}, {"width", e.width}, {"to", Hex64(e.to)}};
}

// The nevr-map-package document for `p` (fields recipes only, which is all these tests use).
std::string IndexJson(const Package& p) {
  json members = json::array();
  for (const Member& m : p.members) {
    json pieces = json::array();
    for (const Pin& piece : m.pieces) pieces.push_back(PinJson(piece));
    json member = {{"path", m.path},
                   {"role", m.role == Role::kSet ? "set" : "script"},
                   {"size", m.size},
                   {"sha256", m.sha256},
                   {"pieces", pieces}};
    if (m.role == Role::kScript) member["hash"] = Hex64(m.script_hash);
    members.push_back(member);
  }
  json rekeyed = json::array();
  for (const RekeyPin& r : p.rekeyed) {
    json edits = json::array();
    for (const FieldEdit& e : r.edits) edits.push_back(EditJson(e));
    rekeyed.push_back({{"hash", Hex64(r.hash)},
                       {"stock_source", Hex64(r.stock_source)},
                       {"stock_sha256", r.stock_sha256},
                       {"edits", edits},
                       {"result_sha256", r.result_sha256},
                       {"result_size", r.result_size}});
  }
  json derived = json::array();
  for (const Recipe& r : p.derived) {
    json ops = json::array();
    for (const FieldEdit& e : r.fields) ops.push_back(EditJson(e));
    derived.push_back({{"type", Hex64(r.type)},
                       {"name", Hex64(r.name)},
                       {"stock",
                        {{"type", Hex64(r.stock_type)},
                         {"name", Hex64(r.stock_name)},
                         {"sha256", r.stock_sha256},
                         {"size", r.stock_size}}},
                       {"result", {{"sha256", r.result_sha256}, {"size", r.result_size}}},
                       {"edits", {{"kind", "fields"}, {"ops", ops}}}});
  }
  json aliases = json::array();
  for (const ConfigAlias& a : p.config_aliases) {
    aliases.push_back(
        {{"key", Hex64(a.key)}, {"key_name", a.key_name}, {"alias", Hex64(a.alias)}, {"alias_name", a.alias_name}});
  }
  const json doc = {{"schema", "nevr-map-package"},
                    {"schema_version", 1},
                    {"id", p.id},
                    {"version", p.version},
                    {"platform", p.platform},
                    {"level", Hex64(p.level)},
                    {"level_name", p.level_name},
                    {"parent", "0x3f9915d3001dc28e"},
                    {"game_build", "echovr.exe test"},
                    {"requires_stock_manifest_sha256", json::array()},
                    {"stock_data_version", "1"},
                    {"source_sha256", p.source_sha256},
                    {"members", members},
                    {"rekeyed", rekeyed},
                    {"derived", derived},
                    {"standins", json::array()},
                    {"config_aliases", aliases}};
  return doc.dump();
}

void AddRecipe(Package& p) {
  Recipe r;
  r.type = 0x10;
  r.name = 0x20;
  r.stock_type = 0x10;
  r.stock_name = 0x21;
  r.stock_size = r.result_size = 4096;
  r.stock_sha256 = FakeSha("stock bytes");
  r.result_sha256 = FakeSha("result bytes");
  r.fields.push_back(FieldEdit{128, 8, 2});
  p.derived.push_back(r);
}

void AddAlias(Package& p) {
  ConfigAlias a;
  a.key_name = "mp_arena_rules";
  a.alias_name = p.id + "_arena_rules";
  a.key = EchoVR::CSymbol64Hash(a.key_name.c_str());
  a.alias = EchoVR::CSymbol64Hash(a.alias_name.c_str());
  p.config_aliases.push_back(a);
}

// The bucket: blobs, package indexes and the signed manifest, served by the fake env.
struct Channel {
  FakeEnv& env;
  const TestKey& key;
  std::string name = "echo-test";
  json maps = json::object();
  std::map<std::string, Package> packages{};

  Pin Blob(const std::string& bytes) {
    Pin pin;
    pin.sha256 = FakeSha(bytes);
    pin.size = bytes.size();
    env.server[std::string(kBase) + "blobs/sha256/" + pin.sha256].body = bytes;
    return pin;
  }

  // Cut at `cuts` (piece sizes; the rest is the last piece).
  Member MakeMember(const std::string& path, Role role, const std::string& bytes,
                    const std::vector<std::size_t>& cuts) {
    Member m;
    m.path = path;
    m.role = role;
    m.size = bytes.size();
    m.sha256 = FakeSha(bytes);
    std::size_t at = 0;
    for (std::size_t cut : cuts) {
      m.pieces.push_back(Blob(bytes.substr(at, cut)));
      at += cut;
    }
    if (at < bytes.size()) m.pieces.push_back(Blob(bytes.substr(at)));
    return m;
  }

  const Package& Publish(const std::string& id, const std::string& version, const std::string& set,
                         const std::map<std::uint64_t, std::string>& scripts = {},
                         const std::function<void(Package&)>& edit = nullptr) {
    Package p;
    p.id = id;
    p.version = version;
    p.platform = kPlatformPc;
    p.level_name = id;
    p.level = EchoVR::CSymbol64Hash(id.c_str());
    p.source_sha256 = FakeSha("source " + id);
    // The cut rule in miniature: the header and tables, then one piece per frame.
    const int chunks = static_cast<int>((set.size() - 48) / (48 + kFrameBytes));
    std::vector<std::size_t> cuts{FramesStart(chunks)};
    cuts.resize(static_cast<std::size_t>(chunks), kFrameBytes);
    p.members.push_back(MakeMember(kSetPath, Role::kSet, set, cuts));
    for (const auto& [hash, bytes] : scripts) {
      Member m = MakeMember(ScriptName(hash), Role::kScript, bytes, {});
      m.script_hash = hash;
      p.members.push_back(m);
    }
    if (edit) edit(p);
    std::uint64_t total = 0;
    for (const Member& m : p.members) total += m.size;
    maps[id] = {{"title", "Title " + id}, {"author", "tester"}, {"thumbnail", PinJson(Blob("thumbnail " + id))}};
    maps[id]["platforms"][kPlatformPc] = {{"version", version},
                                          {"index", PinJson(Blob(IndexJson(p)))},
                                          {"level", Hex64(p.level)},
                                          {"level_name", id},
                                          {"game_build", "echovr.exe test"},
                                          {"bytes", total},
                                          {"scripts", scripts.size() + p.rekeyed.size()}};
    return packages[id] = p;
  }

  std::string Sign(std::uint64_t serial, const std::string& expires = "2027-01-01T00:00:00Z",
                   const std::vector<std::string>& revoked_ids = {}, const std::vector<std::string>& revoked_sha = {}) {
    const json body = {{"schema", "nevr-maps-manifest"},
                       {"schema_version", 1},
                       {"channel", name},
                       {"serial", serial},
                       {"expires", expires},
                       {"retired", json::object()},
                       {"revoked_ids", revoked_ids},
                       {"revoked_sha256", revoked_sha},
                       {"maps", maps}};
    const std::string signed_object = key.SignedObject(body.dump());
    env.server[std::string(kBase) + "v1/maps/manifest.signed"].body = signed_object;
    return signed_object;
  }

  std::string IndexSha(const std::string& id) const {
    return maps.at(id).at("platforms").at(kPlatformPc).at("index").at("sha256").get<std::string>();
  }
  std::string VersionDir(const std::string& id) const {
    return std::string(kDir) + "/" + id + "/" + kPlatformPc + "/" + VersionDirName(IndexSha(id));
  }
};

class MapRunTest : public ::testing::Test {
 protected:
  void SetUp() override {
    config.keys = {key.pin};
    config.dir = kDir;
    config.base_url = kBase;
    env.MakeDirs(kDir);
  }

  FetchReport Fetch(Deriver* d = nullptr, const std::vector<std::string>& deferred = {}) {
    return FetchAll(env, env, config, deferred, d, kNow, keep_going);
  }
  BootReport Boot(Deriver* d = nullptr, const std::string& now = kNow) {
    return BootMount(env, env, config, mounter, d, now);
  }
  static std::string TokenOf(const BootReport& r, const std::string& id) {
    for (const auto& [mid, token] : r.not_mounted) {
      if (mid == id) return token;
    }
    return std::string();
  }
  static bool Mounted(const BootReport& r, const std::string& id) {
    return std::find(r.mounted.begin(), r.mounted.end(), id) != r.mounted.end();
  }
  CacheIndex Index() {
    CacheIndex index;
    EXPECT_EQ(ReadCacheIndex(env, kDir, &index), CacheIndexStatus::kOk);
    return index;
  }
  // Every handle the boot opened is the mounter's, or closed.
  void ExpectNoLeak() { EXPECT_EQ(env.OpenHandles(), mounter.Owned()); }

  FakeEnv env;
  FakeMounter mounter{env};
  FakeDeriver deriver{env};
  TestKey key{1};
  Channel channel{env, key};
  ChannelConfig config;
  bool keep = true;
  std::function<bool()> keep_going = [this] { return keep; };
};

TEST_F(MapRunTest, OfflineBootMountsFromTheCacheWithoutNetworkOrMemberHashing) {
  config.subscribed = {"alpha"};
  const std::string set = MakeSet(6, 'a');
  channel.Publish("alpha", "1.0.0", set);
  channel.Sign(1);
  const FetchReport fetched = Fetch();
  EXPECT_EQ(fetched.reason, "");
  EXPECT_EQ(fetched.maps_updated, 1u);
  EXPECT_EQ(fetched.pieces_fetched, 7u);

  env.server.clear();
  const int gets = env.GetsUnder("");
  env.hashed_bytes = 0;
  const BootReport boot = Boot();
  EXPECT_EQ(env.GetsUnder(""), gets);
  EXPECT_LT(env.hashed_bytes, set.size());
  ASSERT_TRUE(Mounted(boot, "alpha"));
  EXPECT_TRUE(mounter.verified["map:alpha"]);
  EXPECT_EQ(mounter.labels["map:alpha"], "map:alpha@1.0.0");
  EXPECT_EQ(mounter.set_sha["map:alpha"], FakeSha(set));
  ExpectNoLeak();
  const FakeEnv::Logged* summary = env.Find("map_summary");
  ASSERT_NE(summary, nullptr);
  EXPECT_EQ(summary->level, nevr_cargo::EventLevel::kInfo);
  EXPECT_EQ(summary->Get("mounted"), "1");
}

TEST_F(MapRunTest, AScriptRefusedSecondOfThreeWithdrawsAndRegistersNoSet) {
  config.subscribed = {"alpha"};
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'), {{1, "dll one"}, {2, "dll two"}, {3, "dll three"}});
  channel.Sign(1);
  Fetch();
  mounter.refuse_scripts = {2};
  const BootReport boot = Boot();
  EXPECT_FALSE(Mounted(boot, "alpha"));
  EXPECT_EQ(mounter.calls,
            (std::vector<std::string>{"script map:alpha 1", "script map:alpha 2", "withdraw map:alpha"}));
  EXPECT_EQ(TokenOf(boot, "alpha"), "script:pe_rules");
  ExpectNoLeak();
  EXPECT_EQ(env.OpenHandles(), 0u);
}

TEST_F(MapRunTest, ASetRefusedAtItsGrantWithdrawsTheScripts) {
  config.subscribed = {"alpha"};
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'), {{1, "dll one"}});
  channel.Sign(1);
  Fetch();
  mounter.refuse_sets = {"map:alpha"};
  const BootReport boot = Boot();
  EXPECT_FALSE(Mounted(boot, "alpha"));
  EXPECT_TRUE(mounter.Called("withdraw map:alpha"));
  EXPECT_EQ(TokenOf(boot, "alpha"), "collides");
  EXPECT_EQ(env.OpenHandles(), 0u);
}

TEST_F(MapRunTest, TwoMapsSharingAScriptHashTheLowerIdWinsWhateverTheSubscriptionOrder) {
  config.subscribed = {"beta", "alpha", "beta"};
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'), {{9, "shared dll"}});
  channel.Publish("beta", "1.0.0", MakeSet(2, 'b'), {{9, "shared dll"}});
  channel.Sign(1);
  Fetch();
  const BootReport boot = Boot();
  EXPECT_TRUE(Mounted(boot, "alpha"));
  EXPECT_FALSE(Mounted(boot, "beta"));
  EXPECT_EQ(TokenOf(boot, "beta"), "script:duplicate");
  const FakeEnv::Logged* line = env.Find("map_mounted", "id", "beta");
  ASSERT_NE(line, nullptr);
  EXPECT_EQ(line->Get("holder"), "map:alpha");
  EXPECT_FALSE(mounter.Called("script map:beta 9"));
  ExpectNoLeak();
}

TEST_F(MapRunTest, ALocalMapIsSkippedWithNoGrant) {
  config.subscribed = {"alpha"};
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'), {{1, "dll"}});
  channel.Sign(1);
  Fetch();
  config.local_maps = {"alpha"};
  const BootReport boot = Boot();
  EXPECT_EQ(TokenOf(boot, "alpha"), "local_map");
  EXPECT_TRUE(mounter.calls.empty());
  EXPECT_EQ(env.OpenHandles(), 0u);
}

TEST_F(MapRunTest, ACrashBeforeTheIndexLeavesTheOldVersionMountedAndTheNextGcRemovesTheNewDirectory) {
  config.subscribed = {"alpha"};
  channel.Publish("alpha", "1.0.0", MakeSet(4, 'a'));
  channel.Sign(1);
  Fetch();
  const std::string v1_dir = channel.VersionDir("alpha");
  channel.Publish("alpha", "1.1.0", MakeSet(4, 'b'));
  channel.Sign(2);
  const std::string v2_dir = channel.VersionDir("alpha");
  env.fail_writes = {std::string(kDir) + "/index.json"};
  EXPECT_EQ(Fetch().reason, "index_write");
  EXPECT_TRUE(env.HasDir(v2_dir));

  const BootReport boot = Boot();
  ASSERT_EQ(boot.staged.size(), 1u);
  EXPECT_EQ(boot.staged[0].version, "1.0.0");

  env.fail_writes.clear();
  env.server.clear();  // offline: the GC runs, the fetch then fails at the manifest
  Fetch();
  EXPECT_FALSE(env.HasDir(v2_dir));
  EXPECT_TRUE(env.HasDir(v1_dir));
}

TEST_F(MapRunTest, ServerAllowWithAnIndexShaRefusesAnotherSha) {
  config.subscribed = {"alpha"};
  config.is_server = true;
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'), {{1, "dll"}});
  channel.Sign(1);
  config.server_allow = {"alpha@" + channel.IndexSha("alpha")};
  Fetch();
  EXPECT_TRUE(Mounted(Boot(), "alpha"));

  config.server_allow = {"alpha@" + std::string(64, '0')};
  mounter.calls.clear();
  const BootReport boot = Boot();
  EXPECT_EQ(TokenOf(boot, "alpha"), "server_not_allowed");
  EXPECT_FALSE(mounter.Called("set map:alpha"));
}

TEST_F(MapRunTest, TheSummaryListsEveryRefusalAndSkip) {
  config.subscribed = {"alpha", "beta", "gamma", "delta"};
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'), {{1, "dll"}});
  channel.Publish("beta", "1.0.0", MakeSet(2, 'b'));
  channel.Publish("gamma", "1.0.0", MakeSet(2, 'c'), {}, [](Package& p) { AddAlias(p); });
  channel.Sign(1);
  Fetch();
  mounter.refuse_scripts = {1};
  mounter.refuse_sets = {"map:beta"};
  mounter.alias["map:gamma"] = Grant{false, "alias_partial"};
  const BootReport boot = Boot();
  EXPECT_FALSE(Mounted(boot, "gamma"));
  const FakeEnv::Logged* summary = env.Find("map_summary");
  ASSERT_NE(summary, nullptr);
  EXPECT_EQ(summary->level, nevr_cargo::EventLevel::kWarn);
  EXPECT_EQ(summary->Get("mounted"), "0");
  EXPECT_EQ(summary->Get("refused"), "alpha:script:pe_rules,beta:collides,gamma:alias_partial");
  EXPECT_EQ(summary->Get("skipped"), "delta:not_listed");
}

// D8 (OI-4), both platforms: a map with two aliases, one of them refused, keeps nothing it registered (its set and its
// script are withdrawn and their files closed), is refused once as alias_partial, and frees its script hash, so a later
// map with the same script still mounts. The mounter only stages; the core withdraws.
TEST_F(MapRunTest, AMapWithARefusedAliasIsWithdrawnWholeAndFreesItsScripts) {
  config.subscribed = {"alpha", "beta"};
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
  const BootReport boot = Boot();
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
  const FakeEnv::Logged* summary = env.Find("map_summary");
  ASSERT_NE(summary, nullptr);
  EXPECT_EQ(summary->Get("mounted"), "1");
  EXPECT_EQ(summary->Get("refused"), "alpha:alias_partial");
}

TEST_F(MapRunTest, ANullDeriverRefusesARecipeMapAndARecipeFreeMapStillMounts) {
  config.subscribed = {"plain", "recipe"};
  channel.Publish("plain", "1.0.0", MakeSet(2, 'a'));
  channel.Publish("recipe", "1.0.0", MakeSet(2, 'b'), {}, [](Package& p) { AddRecipe(p); });
  channel.Sign(1);
  Fetch();
  EXPECT_NE(env.Find("map_derive", "reason", "derived_unsupported"), nullptr);
  const BootReport boot = Boot();
  EXPECT_TRUE(Mounted(boot, "plain"));
  EXPECT_EQ(TokenOf(boot, "recipe"), "not_cached");
}

TEST_F(MapRunTest, ARecipeMapMountsItsMergedSetAndItsRebuiltScript) {
  config.subscribed = {"recipe"};
  deriver.rebuilt_scripts[0x77] = std::string(128, 'R');
  channel.Publish("recipe", "1.0.0", MakeSet(3, 'r'), {}, [](Package& p) {
    AddRecipe(p);
    RekeyPin pin;
    pin.hash = 0x77;
    pin.stock_source = 0x55;
    pin.stock_sha256 = FakeSha("stock dll");
    pin.edits.push_back(FieldEdit{16, 8, p.level});
    pin.result_sha256 = FakeSha(std::string(128, 'R'));
    pin.result_size = 128;
    p.rekeyed.push_back(pin);
  });
  channel.Sign(1);
  EXPECT_EQ(Fetch(&deriver).maps_updated, 1u);
  EXPECT_EQ(deriver.derive_calls, 1);
  const CacheIndex index = Index();
  ASSERT_EQ(index.maps.count("recipe/windows-x86_64"), 1u);
  EXPECT_EQ(index.maps.at("recipe/windows-x86_64").stock_stamps.size(), 1u);  // one per distinct path

  const BootReport boot = Boot(&deriver);
  ASSERT_TRUE(Mounted(boot, "recipe"));
  EXPECT_TRUE(mounter.Called("derived map:recipe 119"));
  EXPECT_TRUE(mounter.verified["map:recipe"]);
  EXPECT_EQ(deriver.stock_checked.size(), 1u);
  ExpectNoLeak();
}

TEST_F(MapRunTest, StockCurrentRunsOncePerDistinctStockFileAcrossMaps) {
  config.subscribed = {"one", "two"};
  channel.Publish("one", "1.0.0", MakeSet(2, 'a'), {}, [](Package& p) { AddRecipe(p); });
  channel.Publish("two", "1.0.0", MakeSet(2, 'b'), {}, [](Package& p) { AddRecipe(p); });
  channel.Sign(1);
  Fetch(&deriver);
  const BootReport boot = Boot(&deriver);
  EXPECT_EQ(boot.mounted.size(), 2u);
  EXPECT_EQ(deriver.stock_checked, (std::vector<std::string>{"C:/game/_data/packages/stock0"}));
}

TEST_F(MapRunTest, AStockChangeDefersTheMapAndOneFetchRebuildsItForTheNextBoot) {
  config.subscribed = {"recipe"};
  channel.Publish("recipe", "1.0.0", MakeSet(2, 'r'), {}, [](Package& p) { AddRecipe(p); });
  channel.Sign(1);
  Fetch(&deriver);
  deriver.current = false;
  const BootReport deferred = Boot(&deriver);
  EXPECT_EQ(TokenOf(deferred, "recipe"), "stock_changed");
  EXPECT_EQ(deferred.deferred, (std::vector<std::string>{"recipe"}));

  const int index_gets = env.GetsUnder(std::string(kBase) + "blobs/");
  Fetch(&deriver, deferred.deferred);  // the index sha is unchanged; the fetch must rebuild anyway
  EXPECT_EQ(deriver.derive_calls, 2);
  EXPECT_EQ(env.GetsUnder(std::string(kBase) + "blobs/"), index_gets);
  EXPECT_NE(env.Find("map_rehash", "result", "rebuilt"), nullptr);

  deriver.current = true;
  mounter.calls.clear();
  EXPECT_TRUE(Mounted(Boot(&deriver), "recipe"));
}

TEST_F(MapRunTest, ABadFrameRecordsFramesCheckedFalseAndTheSetNeverTakesTheVerifiedPath) {
  config.subscribed = {"alpha"};
  std::string set = MakeSet(3, 'a');
  set[FramesStart(3) + kFrameBytes] = 0;  // frame 1 loses its zstd magic
  channel.Publish("alpha", "1.0.0", set);
  channel.Sign(1);
  Fetch();
  EXPECT_FALSE(Index().maps.at("alpha/windows-x86_64").files[0].frames_checked);
  EXPECT_TRUE(Mounted(Boot(), "alpha"));  // the registry's own frame check still runs at the mount
  EXPECT_FALSE(mounter.verified["map:alpha"]);
}

TEST_F(MapRunTest, TheFrameWalkChecksEveryFrameHeader) {
  env.MakeDirs("C:/t");
  std::string problem;
  env.Put("C:/t/good", MakeSet(5, 'x'));
  EXPECT_TRUE(WalkSetFrames(env, "C:/t/good", &problem)) << problem;

  std::string size = MakeSet(5, 'x');
  size[FramesStart(5) + 2 * kFrameBytes + 5] = 99;  // frame 2 states 99 bytes, the chunk 100
  env.Put("C:/t/size", size);
  EXPECT_FALSE(WalkSetFrames(env, "C:/t/size", &problem));
  EXPECT_EQ(problem, "frame_size chunk 2");

  std::string range = MakeSet(2, 'x');
  range.resize(range.size() - 1);  // the last frame runs past the end
  env.Put("C:/t/range", range);
  EXPECT_FALSE(WalkSetFrames(env, "C:/t/range", &problem));
  EXPECT_EQ(problem, "chunk_range chunk 1");

  env.Put("C:/t/short", "NEVRRES1");
  EXPECT_FALSE(WalkSetFrames(env, "C:/t/short", &problem));
  EXPECT_EQ(problem, "too_small");
  EXPECT_EQ(env.OpenHandles(), 0u);
}

TEST_F(MapRunTest, TheBootBudgetSkipsTheLaterIds) {
  config.subscribed = {"alpha", "beta", "gamma"};
  for (const char* id : {"alpha", "beta", "gamma"}) channel.Publish(id, "1.0.0", MakeSet(2, id[0]));
  channel.Sign(1);
  Fetch();
  config.boot_budget_ms = 100;
  mounter.set_cost_ms = 60;
  const BootReport boot = Boot();
  EXPECT_EQ(boot.mounted, (std::vector<std::string>{"alpha", "beta"}));
  EXPECT_EQ(TokenOf(boot, "gamma"), "boot_budget");
  EXPECT_NE(env.Find("map_summary", "skipped", "gamma:boot_budget"), nullptr);
}

TEST_F(MapRunTest, StagedListedAndNotMountedAreFilled) {
  config.subscribed = {"alpha", "beta"};
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  channel.Publish("beta", "1.0.0", MakeSet(2, 'b'));
  channel.Publish("other", "1.0.0", MakeSet(2, 'o'));
  channel.Sign(1);
  Fetch();
  config.local_maps = {"beta"};
  const BootReport boot = Boot();
  ASSERT_EQ(boot.staged.size(), 1u);
  const MountedMap& staged = boot.staged[0];
  EXPECT_EQ(staged.id, "alpha");
  EXPECT_EQ(staged.version, "1.0.0");
  EXPECT_EQ(staged.index_sha256, channel.IndexSha("alpha"));
  EXPECT_EQ(staged.platform, kPlatformPc);
  EXPECT_EQ(staged.source_sha256, FakeSha("source alpha"));
  EXPECT_EQ(staged.title, "Title alpha");
  EXPECT_EQ(staged.level, EchoVR::CSymbol64Hash("alpha"));
  ASSERT_EQ(boot.listed.size(), 3u);
  EXPECT_EQ(boot.listed[2], std::make_pair(EchoVR::CSymbol64Hash("other"), std::string("other")));
  EXPECT_EQ(boot.not_mounted, (std::vector<std::pair<std::string, std::string>>{{"beta", "local_map"}}));
}

TEST_F(MapRunTest, AChannelSwitchKeepsBothFloors) {
  config.subscribed = {"alpha"};
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  channel.Sign(42);
  Fetch();
  channel.name = "echo-other";
  channel.Sign(7);
  config.channel = "echo-other";
  EXPECT_EQ(Fetch().reason, "");
  const CacheIndex index = Index();
  EXPECT_EQ(index.floors.channels.at("echo-test").serial, 42u);
  EXPECT_EQ(index.floors.channels.at("echo-other").serial, 7u);

  config.channel.clear();  // bound on first use: base_url now names echo-other, so echo-test's manifest is refused
  channel.name = "echo-test";
  channel.Sign(43);
  EXPECT_EQ(Fetch().reason, "wrong_channel");
}

TEST_F(MapRunTest, RevokedByIdByIndexShaAndByPieceShaAreNotMounted) {
  config.subscribed = {"alpha", "beta", "gamma", "plain"};
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  channel.Publish("beta", "1.0.0", MakeSet(2, 'b'));
  const Package gamma = channel.Publish("gamma", "1.0.0", MakeSet(2, 'c'));
  channel.Publish("plain", "1.0.0", MakeSet(2, 'p'));
  channel.Sign(1);
  Fetch();
  // Revoked in the stored manifest (written by a fetch whose plan the revocation also reaches).
  channel.Sign(2, "2027-01-01T00:00:00Z", {"alpha"}, {channel.IndexSha("beta"), gamma.members[0].pieces[1].sha256});
  Fetch();
  const BootReport boot = Boot();
  EXPECT_EQ(boot.mounted, (std::vector<std::string>{"plain"}));
  EXPECT_EQ(TokenOf(boot, "alpha"), "revoked");
  EXPECT_EQ(TokenOf(boot, "beta"), "revoked");
  EXPECT_EQ(TokenOf(boot, "gamma"), "revoked");
}

TEST_F(MapRunTest, ExpiredTheScriptFreeMapMountsAndTheScriptedOneDoesNot) {
  config.subscribed = {"plain", "scripted"};
  channel.Publish("plain", "1.0.0", MakeSet(2, 'a'));
  channel.Publish("scripted", "1.0.0", MakeSet(2, 'b'), {{1, "dll"}});
  channel.Sign(1, "2026-12-01T00:00:00Z");
  Fetch();
  const BootReport boot = Boot(nullptr, "2026-12-01T00:00:00Z");
  EXPECT_TRUE(Mounted(boot, "plain"));
  EXPECT_EQ(TokenOf(boot, "scripted"), "expired");
}

TEST_F(MapRunTest, AReplayedOldSerialIsRefusedAndTheStoredManifestIsKept) {
  config.subscribed = {"alpha"};
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  const std::string old_manifest = channel.Sign(4);
  const std::string current = channel.Sign(5);
  Fetch();
  env.server[std::string(kBase) + "v1/maps/manifest.signed"].body = old_manifest;
  EXPECT_EQ(Fetch().reason, "serial_down");
  std::string stored;
  ASSERT_TRUE(env.ReadSmallFile(std::string(kDir) + "/manifest.signed", &stored, 1 << 20));
  EXPECT_EQ(stored, current);
  EXPECT_TRUE(Mounted(Boot(), "alpha"));
}

TEST_F(MapRunTest, AnUpdateThatChangesOneFrameFetchesOnePieceAndReusesTheRest) {
  config.subscribed = {"alpha"};
  const std::string v1 = MakeSet(6, 'a');
  channel.Publish("alpha", "1.0.0", v1);
  channel.Sign(1);
  Fetch();
  std::string v2 = v1;
  v2[FramesStart(6) + 3 * kFrameBytes + 20] ^= 1;  // a byte of frame 3's payload
  channel.Publish("alpha", "1.1.0", v2);
  channel.Sign(2);
  const FetchReport update = Fetch();
  EXPECT_EQ(update.maps_updated, 1u);
  EXPECT_EQ(update.pieces_fetched, 1u);
  EXPECT_EQ(update.pieces_reused, 6u);
  EXPECT_EQ(Boot().staged.at(0).version, "1.1.0");
}

TEST_F(MapRunTest, AReusedPieceAlteredOnDiskIsCaughtAndFetched) {
  config.subscribed = {"alpha"};
  const std::string v1 = MakeSet(6, 'a');
  channel.Publish("alpha", "1.0.0", v1);
  channel.Sign(1);
  Fetch();
  const std::string set_path = channel.VersionDir("alpha") + "/" + kSetPath;
  std::string altered = v1;
  altered[FramesStart(6) + 30] ^= 1;  // frame 0 on disk
  env.Put(set_path, altered);
  std::string v2 = v1;
  v2[FramesStart(6) + 5 * kFrameBytes + 30] ^= 1;  // the update changes frame 5
  channel.Publish("alpha", "1.1.0", v2);
  channel.Sign(2);
  const FetchReport update = Fetch();
  EXPECT_EQ(update.pieces_fetched, 2u);
  EXPECT_EQ(update.pieces_reused, 5u);
}

TEST_F(MapRunTest, AStampMismatchIsDeferredNotHashedAndTheRehashRefreshesIt) {
  config.subscribed = {"alpha"};
  const std::string set = MakeSet(6, 'a');
  channel.Publish("alpha", "1.0.0", set);
  channel.Sign(1);
  Fetch();
  env.Touch(channel.VersionDir("alpha") + "/" + kSetPath);
  env.hashed_bytes = 0;
  const BootReport boot = Boot();
  EXPECT_EQ(TokenOf(boot, "alpha"), "stamp");
  EXPECT_LT(env.hashed_bytes, set.size());
  EXPECT_TRUE(mounter.calls.empty());
  EXPECT_EQ(env.OpenHandles(), 0u);

  Fetch(nullptr, boot.deferred);
  EXPECT_NE(env.Find("map_rehash", "result", "refreshed"), nullptr);
  EXPECT_TRUE(Mounted(Boot(), "alpha"));
}

TEST_F(MapRunTest, ARehashThatFindsOtherBytesDropsTheVersionAndRefetchesIt) {
  config.subscribed = {"alpha"};
  const std::string set = MakeSet(3, 'a');
  channel.Publish("alpha", "1.0.0", set);
  channel.Sign(1);
  Fetch();
  std::string altered = set;
  altered[FramesStart(3) + 40] ^= 1;
  env.Put(channel.VersionDir("alpha") + "/" + kSetPath, altered);
  const BootReport boot = Boot();
  EXPECT_EQ(TokenOf(boot, "alpha"), "stamp");
  const FetchReport repair = Fetch(nullptr, boot.deferred);
  EXPECT_EQ(repair.maps_updated, 1u);
  EXPECT_EQ(repair.pieces_fetched, 1u);
  mounter.calls.clear();
  EXPECT_TRUE(Mounted(Boot(), "alpha"));
}

TEST_F(MapRunTest, OverTheCapTheMapIsSkippedAndTheOthersContinue) {
  config.subscribed = {"alpha", "beta"};
  channel.Publish("alpha", "1.0.0", MakeSet(10, 'a'));
  channel.Publish("beta", "1.0.0", MakeSet(2, 'b'));
  channel.Sign(1);
  config.cap_bytes = MakeSet(2, 'b').size() + 10;
  Fetch();
  EXPECT_NE(env.Find("map_plan", "reason", "over_cap"), nullptr);
  const BootReport boot = Boot();
  EXPECT_EQ(boot.mounted, (std::vector<std::string>{"beta"}));
}

// CODE_AUDIT_NEVR1 m2: the cap is charged with the signed entry's bytes, so members that add up to more are refused.
TEST_F(MapRunTest, MembersOverTheEntrysBytesAreRefusedBeforeAnyFetch) {
  config.subscribed = {"alpha"};
  channel.Publish("alpha", "1.0.0", MakeSet(10, 'a'));
  channel.maps["alpha"]["platforms"][kPlatformPc]["bytes"] = 1;
  channel.Sign(1);
  Fetch();
  EXPECT_NE(env.Find("map_index", "reason", "bytes_mismatch"), nullptr);
  EXPECT_TRUE(Boot().mounted.empty());
}

TEST_F(MapRunTest, AMissingIndexMountsNothing) {
  config.subscribed = {"alpha"};
  const BootReport boot = Boot();
  EXPECT_TRUE(boot.mounted.empty());
  EXPECT_EQ(TokenOf(boot, "alpha"), "no_index");
  EXPECT_TRUE(mounter.calls.empty());
}

TEST_F(MapRunTest, AMalformedIndexIsKeptAsBadAndTreatedAsEmpty) {
  config.subscribed = {"alpha"};
  env.Put(std::string(kDir) + "/index.json", "{not json");
  EXPECT_EQ(TokenOf(Boot(), "alpha"), "no_index");
  EXPECT_TRUE(env.Has(std::string(kDir) + "/index.json.bad"));
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  channel.Sign(1);
  EXPECT_EQ(Fetch().maps_updated, 1u);
  EXPECT_TRUE(Mounted(Boot(), "alpha"));
}

TEST_F(MapRunTest, NoFileIdDefersWithStampUnsupported) {
  config.subscribed = {"alpha"};
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  channel.Sign(1);
  Fetch();
  env.stamps_supported = false;
  EXPECT_EQ(TokenOf(Boot(), "alpha"), "stamp_unsupported");
  EXPECT_EQ(env.OpenHandles(), 0u);
}

TEST_F(MapRunTest, AShutdownMidPieceLeavesTheTmpForTheNextGc) {
  config.subscribed = {"alpha"};
  channel.Publish("alpha", "1.0.0", MakeSet(6, 'a'));
  channel.Sign(1);
  int gets = 0;
  env.allow_get = [&] {
    if (++gets > 4) keep = false;  // shutdown after the manifest, the index and two pieces
    return true;
  };
  EXPECT_EQ(Fetch().reason, "cancelled");
  const std::string tmp = channel.VersionDir("alpha") + ".tmp";
  EXPECT_TRUE(env.HasDir(tmp));
  EXPECT_EQ(Boot().mounted.size(), 0u);

  env.allow_get = [] { return true; };
  keep = true;
  const FetchReport resumed = Fetch();
  EXPECT_NE(env.Find("map_gc", "reason", "unreferenced"), nullptr);
  EXPECT_EQ(resumed.maps_updated, 1u);
  EXPECT_FALSE(env.HasDir(tmp));
}

TEST_F(MapRunTest, APieceHashMismatchIsRetriedThreeTimesThenFailsTheMap) {
  config.subscribed = {"alpha"};
  const Package& p = channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  channel.Sign(1);
  const std::string url = std::string(kBase) + "blobs/sha256/" + p.members[0].pieces[1].sha256;
  env.server[url].corrupt_times = 3;
  EXPECT_EQ(Fetch().maps_updated, 1u);
  EXPECT_EQ(env.gets[url], 4);

  config.subscribed = {"beta"};
  const Package& q = channel.Publish("beta", "1.0.0", MakeSet(2, 'b'));
  channel.Sign(2);
  const std::string bad = std::string(kBase) + "blobs/sha256/" + q.members[0].pieces[1].sha256;
  env.server[bad].corrupt_times = 4;
  EXPECT_EQ(Fetch().maps_updated, 0u);
  EXPECT_NE(env.Find("map_piece", "reason", "piece_hash"), nullptr);
  EXPECT_FALSE(env.HasDir(channel.VersionDir("beta") + ".tmp"));
}

TEST_F(MapRunTest, AnUnsubscribedMapIsRemovedByGc) {
  config.subscribed = {"alpha"};
  channel.Publish("alpha", "1.0.0", MakeSet(2, 'a'));
  channel.Sign(1);
  Fetch();
  config.subscribed.clear();
  env.server.clear();
  Fetch();
  EXPECT_FALSE(env.HasDir(std::string(kDir) + "/alpha"));
  EXPECT_TRUE(Index().maps.empty());
}

TEST_F(MapRunTest, TheCacheIndexRoundTrips) {
  CacheIndex index;
  index.floors.channels["c"].serial = 3;
  index.floors.channels["c"].serial_sha256 = std::string(64, 'a');
  index.floors.channels["c"].version["m/windows-x86_64"] = "1.2.0";
  index.floors.bound["https://x/"] = "c";
  CachedMap m;
  m.id = "m";
  m.platform = kPlatformPc;
  m.version = "1.2.0";
  m.index_sha256 = std::string(64, 'b');
  m.index_size = 99;
  m.level = 0xfedcba9876543210ull;
  m.level_name = "m";
  m.dir = "m/windows-x86_64/bbbbbbbbbbbb";
  m.pinned_serial = 3;
  m.stock_manifest_ok = true;
  CachedFile f;
  f.path = "scripts/00000000000000aa.dll";
  f.role = FileRole::kDerivedScript;
  f.script_hash = 0xaa;
  f.size = 5;
  f.sha256 = std::string(64, 'c');
  f.stamp.last_write = 133000000000000000ull;
  m.files.push_back(f);
  m.stock_stamps.push_back(StockStamp{"C:/g/p", f.stamp});
  index.maps[MapKey("m", kPlatformPc)] = m;
  CacheIndex back;
  ASSERT_TRUE(CacheIndexFromJson(CacheIndexToJson(index), &back));
  EXPECT_EQ(back.maps.at("m/windows-x86_64"), m);
  EXPECT_EQ(back.floors.channels.at("c").version.at("m/windows-x86_64"), "1.2.0");
  EXPECT_EQ(back.floors.bound.at("https://x/"), "c");
  EXPECT_FALSE(CacheIndexFromJson("{\"schema\":\"nevr-maps-index\",\"schema_version\":2}", &back));
}

}  // namespace
