// Host test for the dedicated server's demand resolve (src/cdn/cdn_demand.*; docs/adr/0045 decision 9, PLAN-v7 R6-a-1): the set a map
// and a game mode need is the level's archive closure plus the mode's labels, every object comes from the store, the installed
// files or the CDN in that order, a set is published only when every object is had, and a failed resolve publishes nothing and saves
// no manifest. A scripted server serves the manifest and the objects through the fake Env (quest/tests/cdn_fake_env.h).
//
// Built and run by `just test-quest-data` (cdn_demand_test).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "cdn/cdn_demand.h"
#include "cdn/cdn_run.h"
#include "core/combat_level_class.h"
#include "core/resource_shadow.h"
#include "quest/tests/cdn_fake_env.h"
#include "quest/tests/cdn_zstd_support.h"
#include "quest/tests/test_check.h"
#include "runtime/server/game_type_table.h"

namespace {

namespace nc = nevr_cdn;
using cdn_test::FakeEnv;
using cdn_test::MakeFrame;
using cdn_test::Response;
using cdn_test::Sha256Of;
using json = nlohmann::ordered_json;

constexpr const char* kBase = "https://cargo.echo.taxi/";
constexpr const char* kManifestPath = "v2/manifest.json.zst";
constexpr const char* kDir = "/cache";
constexpr const char* kBlobsPrefix = "blobs/sha256/";
constexpr const char* kGame = "game";
constexpr const char* kCombat = "combat_content";
constexpr const char* kEchoArena = "echo_arena";
constexpr const char* kEchoCombat = "echo_combat";
constexpr const char* kEchoDemo = "echo_demo";  // a game type the service does not place
constexpr const char* kArenaPrivate = "echo_arena_private";
constexpr const char* kSocial = "social_2.0";
constexpr const char* kSchemaName = "nevr-cargo-manifest";
constexpr const char* kZstdFrameFormat = "zstd-frame";
constexpr const char* kStatusHttpFailed = "http_failed";
constexpr const char* kStatusHashMismatch = "hash_mismatch";
constexpr const char* kStatusOverStorage = "over_storage";
constexpr const char* kStatusDeadline = "deadline";
constexpr const char* kStatusBadArchive = "bad_archive";
constexpr const char* kStatusNoMode = "no_mode";
constexpr const char* kStatusCancelled = "cancelled";
constexpr const char* kStatusHeldSizeMismatch = "held_size_mismatch";  // PrefetchStatusName(kHeldWrongSize), cdn_prefetch.cpp
constexpr const char* kRoleServer = "server";

constexpr char kOtherFill = 'X';
constexpr std::size_t kShortFrameBytes = 40;  // a held frame shorter than the manifest's, with the same name
constexpr std::uint64_t kLevelA = 0x1111;
constexpr std::uint64_t kLevelB = 0x2222;
constexpr std::uint64_t kLevelC = 0x4444;
constexpr std::uint64_t kLevelZero = 0x0;
constexpr std::uint64_t kAssetType = 0x7e57c0de00000001ULL;  // an ordinary asset type (not a descriptor or payload pair)
constexpr std::uint64_t kName10 = 0x10;
constexpr std::uint64_t kName11 = 0x11;
constexpr std::uint64_t kName12 = 0x12;
constexpr std::uint64_t kName13 = 0x13;
constexpr std::uint64_t kName30 = 0x30;
constexpr std::uint64_t kName31 = 0x31;
constexpr std::uint64_t kName40 = 0x40;
constexpr std::uint64_t kName41 = 0x41;
constexpr std::uint64_t kName42 = 0x42;
constexpr std::uint64_t kName50 = 0x50;  // kName50 + k: the asset of combat map k
constexpr std::uint64_t kEchoArenaLevel0 = nevr_combat_level::kEchoArenaLevels[0];  // mpl_arena_a
constexpr std::uint64_t kEchoArenaLevel1 = nevr_combat_level::kEchoArenaLevels[1];  // mpl_arena_a_lowspec
constexpr std::uint64_t kLobbyLevel0 = nevr_combat_level::kLobbyLevels[0];          // mpl_lobby_b
constexpr std::uint64_t kName99 = 0x99;
constexpr std::uint64_t kAssetBytes = 300;     // decompressed bytes of an asset
constexpr std::size_t kAssetFrameBytes = 120;  // stored bytes of an asset's frame
constexpr std::uint32_t kAlign = 16;
constexpr long kHttpOk = 200;
constexpr std::uint64_t kBitsPerByte = 8;
constexpr std::uint64_t kByteMask = 0xff;
constexpr std::size_t kU64Bytes = 8;
constexpr std::uint64_t kHugeCount = 0xffff;   // a 16-bit count far above the bytes any fixture body holds
constexpr std::uint64_t kHugeDependents = 0xffffffffULL;
constexpr std::uint8_t kTrailingByte = 0x7a;
constexpr std::size_t kNoEntries = 0;
constexpr std::size_t kOne = 1;
constexpr std::size_t kTwo = 2;
constexpr std::size_t kThree = 3;
constexpr std::size_t kFour = 4;
constexpr std::size_t kFive = 5;
constexpr std::size_t kSeven = 7;

// ── archive bodies ──────────────────────────────────────────────────────────────────────────────────────────────────────────

void PutU32(std::string* out, std::uint64_t v) {
  for (std::size_t i = 0; i < nc::kU32Bytes; ++i) out->push_back(static_cast<char>((v >> (i * kBitsPerByte)) & kByteMask));
}
void PutU64(std::string* out, std::uint64_t v) {
  for (std::size_t i = 0; i < kU64Bytes; ++i) out->push_back(static_cast<char>((v >> (i * kBitsPerByte)) & kByteMask));
}

// The body of docs/engine/asset-system.md: D dependents, N {type, name} records, M span records (none here), then the final u32 0.
std::string ArchiveBody(const std::vector<std::uint64_t>& deps, const std::vector<std::pair<std::uint64_t, std::uint64_t>>& assets) {
  std::string body;
  PutU32(&body, deps.size());
  for (const std::uint64_t d : deps) PutU64(&body, d);
  PutU32(&body, assets.size());
  for (const auto& a : assets) {
    PutU64(&body, a.first);
    PutU64(&body, a.second);
  }
  PutU32(&body, kNoEntries);  // M
  PutU32(&body, kNoEntries);  // the final u32 0
  return body;
}

std::vector<std::uint8_t> Bytes(const std::string& text) {
  std::vector<std::uint8_t> out;
  out.reserve(text.size());
  for (const char c : text) out.push_back(static_cast<std::uint8_t>(c));
  return out;
}

// ── the fixture: one manifest, served by the fake server ───────────────────────────────────────────────────────────────────

struct Item {
  std::string label;
  std::uint64_t type = 0;
  std::uint64_t name = 0;
  std::string frame;
  std::uint64_t bytes = 0;
  std::string sha;
  std::string body;  // an archive's decompressed body (empty for an asset)
};

void Seal(Item* it) { it->sha = Sha256Of(it->frame); }

Item AssetItem(const std::string& label, std::uint64_t name, char fill) {
  Item it;
  it.label = label;
  it.type = kAssetType;
  it.name = name;
  it.frame = MakeFrame(kAssetBytes, kAssetFrameBytes, fill);
  it.bytes = kAssetBytes;
  Seal(&it);
  return it;
}

// An archive is a zstd frame of its body (the demand resolve decodes it), and its entry's size and bytes are the frame's and the body's.
Item ArchiveItem(std::uint64_t name, const std::string& body) {
  Item it;
  it.label = kGame;
  it.type = nc::kArchiveType;
  it.name = name;
  it.body = body;
  it.frame = cdn_test::ZstdFrameOf(body);
  it.bytes = body.size();
  Seal(&it);
  return it;
}

// The default device's limits: what a manifest is read against in these tests.
nc::Limits DefaultLimits() {
  nc::DeviceFacts facts;
  facts.have_free = true;
  facts.free_bytes = FakeEnv::DefaultDevice().free_bytes;
  facts.have_fds = true;
  facts.fd_soft_limit = FakeEnv::DefaultDevice().fd_soft_limit;
  facts.fds_open = FakeEnv::DefaultDevice().fds_open;
  facts.have_memory = true;
  facts.memory_available = FakeEnv::DefaultDevice().memory_available;
  return nc::ComputeLimits(facts, nc::ManifestEntryMemoryBytes());
}

struct Set {
  std::vector<Item> items;  // manifest order
  nc::RunConfig config;

  Set() {
    config.dir = kDir;
    config.base_url = kBase;
    config.manifest_path = kManifestPath;
    config.platform = nc::kWindowsX64;
    config.policy.role = nc::Role::kServer;
    // Level A lists assets 10 and 11 and depends on level B, which lists asset 12; level C lists asset 13.
    items.push_back(ArchiveItem(kLevelA, ArchiveBody({kLevelB}, {{kAssetType, kName10}, {kAssetType, kName11}})));
    items.push_back(ArchiveItem(kLevelB, ArchiveBody({}, {{kAssetType, kName12}})));
    items.push_back(AssetItem(kGame, kName10, 'a'));
    items.push_back(AssetItem(kGame, kName11, 'b'));
    items.push_back(AssetItem(kGame, kName12, 'c'));
    items.push_back(AssetItem(kGame, kName99, 'd'));  // listed by no archive
    items.push_back(AssetItem(kCombat, kName30, 'e'));
    items.push_back(AssetItem(kCombat, kName31, 'f'));
    items.push_back(ArchiveItem(kLevelC, ArchiveBody({}, {{kAssetType, kName13}})));
    items.push_back(AssetItem(kGame, kName13, 'g'));
  }

  Item* Find(std::uint64_t type, std::uint64_t name) {
    for (Item& it : items) {
      if (it.type == type && it.name == name) return &it;
    }
    return nullptr;
  }
  const Item* Find(std::uint64_t type, std::uint64_t name) const {
    for (const Item& it : items) {
      if (it.type == type && it.name == name) return &it;
    }
    return nullptr;
  }
  Item* Level(std::uint64_t name) { return Find(nc::kArchiveType, name); }
  const Item* Level(std::uint64_t name) const { return Find(nc::kArchiveType, name); }
  const Item* BySha(const std::string& sha) const {
    for (const Item& it : items) {
      if (it.sha == sha) return &it;
    }
    return nullptr;
  }
  void Drop(std::uint64_t type, std::uint64_t name) {
    for (auto it = items.begin(); it != items.end(); ++it) {
      if (it->type == type && it->name == name) {
        items.erase(it);
        return;
      }
    }
  }
  std::string ManifestText() const {
    json labels = json::object();
    for (const Item& it : items) {
      if (!labels.contains(it.label)) labels[it.label] = json{{"entries", json::array()}};
      labels[it.label]["entries"].push_back(json{{"platform", nc::kWindowsX64.name},
                                                 {"type", nc::FormatHex64(it.type)},
                                                 {"name", nc::FormatHex64(it.name)},
                                                 {"format", kZstdFrameFormat},
                                                 {"sha256", it.sha},
                                                 {"size", it.frame.size()},
                                                 {"bytes", it.bytes},
                                                 {"align", kAlign}});
    }
    return json{{"schema", kSchemaName}, {"schema_version", 1}, {"labels", labels}, {"empty", json::array()}}.dump();
  }
  // The manifest as the resolve reads it: parsed from the text the server serves.
  nc::Manifest Parsed() const {
    nc::Manifest m;
    const nc::ParseResult r = nc::ParseManifest(ManifestText(), DefaultLimits(), &m);
    QCHECK(r.status == nc::ParseStatus::kOk);
    return m;
  }
  std::string BlobUrl(const std::string& sha) const { return std::string(kBase) + kBlobsPrefix + sha; }
  void Serve(FakeEnv& env) const {
    env.server[std::string(kBase) + kManifestPath] = Response{kHttpOk, cdn_test::ZstdFrameOf(ManifestText()), std::string::npos, ""};
    for (const Item& it : items) env.server[BlobUrl(it.sha)] = Response{kHttpOk, it.frame, std::string::npos, ""};
  }
};

// The sets a resolve published, kept alive until the test ends (the worker never frees one, cdn_run.h).
struct Captured {
  std::vector<nc::Mounted*> sets;
  nc::Publish Sink() {
    return [this](std::unique_ptr<nc::Mounted> m) { sets.push_back(m.release()); };
  }
  ~Captured() {
    for (nc::Mounted* m : sets) delete m;
  }
};

// An installed source the test describes: the objects it verifies and the archives it reads back, by sha.
class FakeInstalled : public nc::InstalledSource {
 public:
  std::set<std::string> verified;
  const Set* set = nullptr;
  bool ReadVerified(const nc::Entry& e) override { return verified.count(e.sha256) != 0; }
  bool ReadBody(const nc::Entry& archive, std::vector<std::uint8_t>* body) override {
    if (verified.count(archive.sha256) == 0) return false;
    const Item* it = set->BySha(archive.sha256);
    if (it == nullptr || it->body.empty()) return false;
    *body = Bytes(it->body);
    return true;
  }
};

// The archive bodies the closure reads, from the set, by sha.
nc::ReadArchiveFn ReadFrom(const Set& s) {
  return [&s](const nc::Entry& e, std::vector<std::uint8_t>* body) {
    const Item* it = s.BySha(e.sha256);
    if (it == nullptr || it->body.empty()) return false;
    *body = Bytes(it->body);
    return true;
  };
}

nc::DemandInput Input(std::uint64_t level, const char* game_type) {
  nc::DemandInput in;
  in.level = level;
  in.game_type = game_type;
  return in;
}

nc::Policy ServerPolicy() {
  nc::Policy p;
  p.role = nc::Role::kServer;
  return p;
}

bool Holds(const nc::Needed& needed, std::uint64_t type, std::uint64_t name) {
  for (const nc::Entry& e : needed.entries) {
    if (e.type == type && e.name == name) return true;
  }
  return false;
}

std::size_t CountNamed(const FakeEnv& env, const char* name) {
  std::size_t n = 0;
  for (const cdn_test::Record& r : env.records) {
    if (r.name == name) ++n;
  }
  return n;
}

std::size_t GetsOfBlobs(const FakeEnv& env) {
  std::size_t n = 0;
  for (const std::string& r : env.requests) {
    if (r.find(kBlobsPrefix) != std::string::npos) ++n;
  }
  return n;
}

// Holds an object under `sha` as an earlier resolve would have left it: `frame` is its stored bytes, `bytes` its decompressed size.
void HoldObjectAs(FakeEnv& env, const nc::RunConfig& config, const std::string& sha, const std::string& frame, std::uint64_t bytes) {
  nc::StoreOpenInfo info;
  std::unique_ptr<nc::Store> store = nc::Store::Open(env, config.dir, DefaultLimits(), &info);
  const char* why = "";
  std::unique_ptr<nc::Store::Writer> w = store->Begin(frame.size(), &why);
  w->Write(frame.data(), frame.size());
  w->Commit(sha, bytes);
  store->EndWrites();
}

void HoldObject(FakeEnv& env, const nc::RunConfig& config, const Item& it) { HoldObjectAs(env, config, it.sha, it.frame, it.bytes); }

// ── the closure and the labels (ComputeNeeded) ─────────────────────────────────────────────────────────────────────────────

// The closure of level A is A, B and the assets they list (10, 11, 12): five entries, not asset 99 and no combat entry.
void TheNeededSetIsTheClosureOfTheLevelsArchive() {
  const Set s;
  nc::Needed needed;
  const char* reason = "";
  QCHECK(nc::ComputeNeeded(s.Parsed(), nc::kWindowsX64, ServerPolicy(), Input(kLevelA, kEchoArena), ReadFrom(s), &needed, &reason));
  QCHECK(needed.entries.size() == kFive);
  QCHECK(Holds(needed, nc::kArchiveType, kLevelA) && Holds(needed, nc::kArchiveType, kLevelB));
  QCHECK(Holds(needed, kAssetType, kName10) && Holds(needed, kAssetType, kName11) && Holds(needed, kAssetType, kName12));
  QCHECK(!Holds(needed, kAssetType, kName99));
  QCHECK(needed.labels.size() == kOne && needed.labels[0] == kGame);
}

// A combat mode takes the whole combat label too: seven entries and both labels. The other modes take no mode label.
void ACombatModeTakesTheWholeCombatLabelToo() {
  const Set s;
  const nc::Manifest m = s.Parsed();
  nc::Needed needed;
  const char* reason = "";
  QCHECK(nc::ComputeNeeded(m, nc::kWindowsX64, ServerPolicy(), Input(kLevelA, kEchoCombat), ReadFrom(s), &needed, &reason));
  QCHECK(needed.entries.size() == kSeven);
  QCHECK(Holds(needed, kAssetType, kName30) && Holds(needed, kAssetType, kName31));
  QCHECK(needed.labels.size() == kTwo && needed.labels[0] == kCombat && needed.labels[1] == kGame);
  for (const char* mode : {kArenaPrivate, kSocial}) {
    nc::Needed plain;
    QCHECK(nc::ComputeNeeded(m, nc::kWindowsX64, ServerPolicy(), Input(kLevelA, mode), ReadFrom(s), &plain, &reason));
    QCHECK(plain.entries.size() == kFive);
  }
}

// An archive the manifest lacks is not expanded: its assets are not needed, the rest are.
void AnArchiveTheManifestLacksIsNotExpanded() {
  Set s;
  s.Drop(nc::kArchiveType, kLevelB);
  nc::Needed needed;
  const char* reason = "";
  QCHECK(nc::ComputeNeeded(s.Parsed(), nc::kWindowsX64, ServerPolicy(), Input(kLevelA, kEchoArena), ReadFrom(s), &needed, &reason));
  QCHECK(Holds(needed, nc::kArchiveType, kLevelA) && Holds(needed, kAssetType, kName10) && Holds(needed, kAssetType, kName11));
  QCHECK(!Holds(needed, kAssetType, kName12));
  QCHECK(needed.entries.size() == kThree);
}

// A label that is off contributes nothing, and the resolve is complete with nothing needed.
void ALabelThatIsOffContributesNothing() {
  const Set s;
  nc::Policy p = ServerPolicy();
  p.overrides[kGame] = nc::LabelState::kOff;
  nc::Needed needed;
  const char* reason = "";
  QCHECK(nc::ComputeNeeded(s.Parsed(), nc::kWindowsX64, p, Input(kLevelA, kEchoArena), ReadFrom(s), &needed, &reason));
  QCHECK(needed.entries.empty());
  nc::RunConfig config = s.config;
  config.policy = p;
  FakeEnv env;
  s.Serve(env);
  Captured got;
  const nc::DemandResult r = nc::ResolveDemand(env, config, Input(kLevelA, kEchoArena), nullptr, got.Sink(), [] { return true; });
  QCHECK(r.ok && r.needed == kNoEntries);
  QCHECK(got.sets.empty());
  const cdn_test::Record* rec = env.FindRecord("cdn_demand");
  QCHECK(rec != nullptr && rec->Get("needed") == "0" && rec->Get("role") == kRoleServer);
}

// A game type no level and no mode places is a named failure, found before any object or the manifest is asked for.
void NoModeIsANamedFailure() {
  const Set s;
  nc::Needed needed;
  const char* reason = "";
  QCHECK(!nc::ComputeNeeded(s.Parsed(), nc::kWindowsX64, ServerPolicy(), Input(kLevelZero, kEchoDemo), ReadFrom(s), &needed, &reason));
  QCHECK(std::string(reason) == kStatusNoMode);
  FakeEnv env;
  s.Serve(env);
  Captured got;
  const nc::DemandResult r = nc::ResolveDemand(env, s.config, Input(kLevelZero, kEchoDemo), nullptr, got.Sink(), [] { return true; });
  QCHECK(!r.ok && std::string(r.reason) == kStatusNoMode);
  QCHECK(got.sets.empty());
  QCHECK(env.requests.empty());
}

// The label of a level-0 session is the union of its mode's levels: two arena levels and one lobby, each archive with its asset.
void NoLevelResolvesTheUnionOfTheModesLevels() {
  Set s;
  s.items.push_back(ArchiveItem(kEchoArenaLevel0, ArchiveBody({}, {{kAssetType, kName40}})));
  s.items.push_back(AssetItem(kGame, kName40, 'h'));
  s.items.push_back(ArchiveItem(kEchoArenaLevel1, ArchiveBody({}, {{kAssetType, kName41}})));
  s.items.push_back(AssetItem(kGame, kName41, 'i'));
  s.items.push_back(ArchiveItem(kLobbyLevel0, ArchiveBody({}, {{kAssetType, kName42}})));
  s.items.push_back(AssetItem(kGame, kName42, 'j'));
  nc::Needed needed;
  const char* reason = "";
  QCHECK(nc::ComputeNeeded(s.Parsed(), nc::kWindowsX64, ServerPolicy(), Input(kLevelZero, kEchoArena), ReadFrom(s), &needed, &reason));
  QCHECK(Holds(needed, nc::kArchiveType, kEchoArenaLevel0) && Holds(needed, nc::kArchiveType, kEchoArenaLevel1));
  QCHECK(Holds(needed, nc::kArchiveType, kLobbyLevel0));
  QCHECK(Holds(needed, kAssetType, kName40) && Holds(needed, kAssetType, kName41) && Holds(needed, kAssetType, kName42));
  FakeEnv env;
  s.Serve(env);
  Captured got;
  const nc::DemandResult r = nc::ResolveDemand(env, s.config, Input(kLevelZero, kEchoArena), nullptr, got.Sink(), [] { return true; });
  QCHECK(r.ok && got.sets.size() == kOne);
}

// A combat session with one map still needs all four combat maps: the closure starts from each mode level.
void ACombatSessionWithALevelStillNeedsAllFourMaps() {
  Set s;
  for (std::size_t k = 0; k < nevr_combat_level::kCombatMapCount; ++k) {
    const std::uint64_t name = kName50 + k;
    s.items.push_back(ArchiveItem(nevr_combat_level::kCombatMaps[k], ArchiveBody({}, {{kAssetType, name}})));
    s.items.push_back(AssetItem(kGame, name, static_cast<char>('k' + k)));
  }
  nc::Needed needed;
  const char* reason = "";
  QCHECK(nc::ComputeNeeded(s.Parsed(), nc::kWindowsX64, ServerPolicy(), Input(nevr_combat_level::kCombatMaps[0], kEchoCombat), ReadFrom(s),
                           &needed, &reason));
  for (const std::uint64_t map : nevr_combat_level::kCombatMaps) QCHECK(Holds(needed, nc::kArchiveType, map));
  QCHECK(Holds(needed, kAssetType, kName30) && Holds(needed, kAssetType, kName31));
}

// A level the mode does not name resolves that level's closure alone, the mode placing nothing.
void AnUnknownModeWithALevelResolvesOnlyThatClosure() {
  const Set s;
  nc::Needed needed;
  const char* reason = "";
  QCHECK(nc::ComputeNeeded(s.Parsed(), nc::kWindowsX64, ServerPolicy(), Input(kLevelA, kEchoDemo), ReadFrom(s), &needed, &reason));
  QCHECK(needed.entries.size() == kFive);
  QCHECK(!Holds(needed, nc::kArchiveType, kLevelC));
}

// A failed resolve is an Error record naming the object, and a cancel is a Warning.
void AFailedResolveIsAnErrorRecordAndACancelIsAWarning() {
  const Set s;
  const Item* asset = s.Find(kAssetType, kName12);
  FakeEnv env;
  s.Serve(env);
  env.server.erase(s.BlobUrl(asset->sha));
  Captured got;
  const nc::DemandResult r = nc::ResolveDemand(env, s.config, Input(kLevelA, kEchoArena), nullptr, got.Sink(), [] { return true; });
  QCHECK(!r.ok);
  const cdn_test::Record* rec = env.FindRecord("cdn_demand", "result", "failed");
  QCHECK(rec != nullptr && rec->level == nc::EventLevel::kError);
  QCHECK(rec != nullptr && rec->Get("label") == kGame && rec->Get("name") == nc::FormatHex64(kName12));
  QCHECK(rec != nullptr && rec->Get("sha256") == asset->sha && rec->Get("reason") == kStatusHttpFailed);
  FakeEnv cancelled;
  s.Serve(cancelled);
  const nc::DemandResult c = nc::ResolveDemand(cancelled, s.config, Input(kLevelA, kEchoArena), nullptr, got.Sink(), [] { return false; });
  QCHECK(!c.ok && std::string(c.reason) == kStatusCancelled);
  const cdn_test::Record* crec = cancelled.FindRecord("cdn_demand", "result", "failed");
  QCHECK(crec != nullptr && crec->level == nc::EventLevel::kWarn);
}

// An archive whose body has a trailing byte fails the resolve, names the archive, and publishes nothing.
void ABadArchiveFailsTheResolve() {
  Set s;
  Item* a = s.Level(kLevelA);
  a->body.push_back(static_cast<char>(kTrailingByte));
  a->frame = cdn_test::ZstdFrameOf(a->body);
  a->bytes = a->body.size();
  Seal(a);
  FakeEnv env;
  s.Serve(env);
  Captured got;
  const nc::DemandResult r = nc::ResolveDemand(env, s.config, Input(kLevelA, kEchoArena), nullptr, got.Sink(), [] { return true; });
  QCHECK(!r.ok && std::string(r.reason) == kStatusBadArchive);
  QCHECK(r.label == kGame && r.name == nc::FormatHex64(kLevelA));
  QCHECK(got.sets.empty());
}

// ── the resolve (ResolveDemand) ────────────────────────────────────────────────────────────────────────────────────────────

// A cache that holds everything fetches nothing the second time, and each resolve publishes one set. The store's writes are ended
// before a mount (the PC's share modes refuse a mount of a file with an append handle still open: windows_sharing).
void ACacheThatHoldsEverythingFetchesNothing() {
  const Set s;
  FakeEnv env;
  env.windows_sharing = true;
  s.Serve(env);
  Captured got;
  const nc::DemandResult first = nc::ResolveDemand(env, s.config, Input(kLevelA, kEchoArena), nullptr, got.Sink(), [] { return true; });
  QCHECK(first.ok && first.fetched == kFive && first.from_cache == kNoEntries);
  QCHECK(got.sets.size() == kOne);
  env.requests.clear();
  const nc::DemandResult second = nc::ResolveDemand(env, s.config, Input(kLevelA, kEchoArena), nullptr, got.Sink(), [] { return true; });
  QCHECK(second.ok && second.from_cache == kFive && second.fetched == kNoEntries);
  QCHECK(GetsOfBlobs(env) == kNoEntries);
  QCHECK(env.requests.size() == kOne);  // the manifest only
  QCHECK(got.sets.size() == kTwo);
  QCHECK(got.sets[1]->names == kFive && got.sets[1]->missing == kNoEntries);
}

// Only what the cache lacks is fetched: the cache holds level A and asset 10, so B, 11 and 12 come from the CDN.
void OnlyWhatTheCacheLacksIsFetched() {
  const Set s;
  FakeEnv env;
  s.Serve(env);
  HoldObject(env, s.config, *s.Level(kLevelA));
  HoldObject(env, s.config, *s.Find(kAssetType, kName10));
  env.requests.clear();
  Captured got;
  const nc::DemandResult r = nc::ResolveDemand(env, s.config, Input(kLevelA, kEchoArena), nullptr, got.Sink(), [] { return true; });
  QCHECK(r.ok && r.from_cache == kTwo && r.fetched == kThree);
  QCHECK(GetsOfBlobs(env) == kThree);
  QCHECK(got.sets.size() == kOne);
}

// Q2: a needed object that cannot be had fails the whole resolve. Nothing is published and no manifest is saved; the objects the
// store took before the failure stay, and a later resolve with the object back fetches only that one.
void AnObjectThatCannotBeHadFailsTheWholeResolve() {
  const Set s;
  FakeEnv env;
  s.Serve(env);
  const std::string url = s.BlobUrl(s.Find(kAssetType, kName12)->sha);
  env.server.erase(url);  // a 404
  Captured got;
  const nc::DemandResult r = nc::ResolveDemand(env, s.config, Input(kLevelA, kEchoArena), nullptr, got.Sink(), [] { return true; });
  QCHECK(!r.ok && std::string(r.reason) == kStatusHttpFailed);
  QCHECK(r.name == nc::FormatHex64(kName12));
  QCHECK(got.sets.empty());
  QCHECK(env.fs_->files.count(nc::ManifestCopyPath(kDir)) == kNoEntries);
  env.server[url] = Response{kHttpOk, s.Find(kAssetType, kName12)->frame, std::string::npos, ""};
  env.requests.clear();
  const nc::DemandResult again = nc::ResolveDemand(env, s.config, Input(kLevelA, kEchoArena), nullptr, got.Sink(), [] { return true; });
  QCHECK(again.ok && again.fetched == kOne && again.from_cache == kFour);
  QCHECK(GetsOfBlobs(env) == kOne);
  QCHECK(got.sets.size() == kOne);
}

// An object whose bytes are not the manifest's is refused by name and never stored.
void AHashThatIsNotTheManifestsIsRefused() {
  const Set s;
  FakeEnv env;
  s.Serve(env);
  const Item* bad = s.Find(kAssetType, kName11);
  // Other bytes under the same header: the frame's magic and sizes stay the manifest's, so the SHA-256 is what differs.
  env.server[s.BlobUrl(bad->sha)] = Response{kHttpOk, cdn_test::MakeFrame(bad->bytes, bad->frame.size(), kOtherFill), std::string::npos, ""};
  Captured got;
  const nc::DemandResult r = nc::ResolveDemand(env, s.config, Input(kLevelA, kEchoArena), nullptr, got.Sink(), [] { return true; });
  QCHECK(!r.ok && std::string(r.reason) == kStatusHashMismatch);
  QCHECK(r.name == nc::FormatHex64(kName11));
  QCHECK(got.sets.empty());
  nc::StoreOpenInfo info;
  std::unique_ptr<nc::Store> store = nc::Store::Open(env, s.config.dir, DefaultLimits(), &info);
  QCHECK(!store->Has(bad->sha));
}

// The named wait ends the resolve: the clock passes the demand's limit on the first transfer, the transfer is cancelled and the reason
// is the deadline. Nothing is published, and one Warning record says so.
void TheWaitIsNamedAndEndsTheResolve() {
  const Set s;
  FakeEnv env;
  s.Serve(env);
  bool advanced = false;
  env.allow_get = [&env, &advanced]() {
    if (!advanced && env.requests.back().find(kBlobsPrefix) != std::string::npos) {
      advanced = true;
      env.clock += nc::kServerDemandMs;
    }
    return true;
  };
  Captured got;
  const nc::DemandResult r = nc::ResolveDemand(env, s.config, Input(kLevelA, kEchoArena), nullptr, got.Sink(), [] { return true; });
  QCHECK(!r.ok && std::string(r.reason) == kStatusDeadline);
  QCHECK(got.sets.empty());
  const cdn_test::Record* rec = env.FindRecord("cdn_demand", "result", "failed");
  QCHECK(rec != nullptr && rec->level == nc::EventLevel::kError);
  QCHECK(CountNamed(env, "cdn_demand") == kOne);
}

// Storage: a resolve whose download would not fit the budget fails before the CDN is asked for anything. The fake device's free space
// is set to the archive's frame size, which leaves a budget smaller than that frame.
void OverStorageFailsBeforeTheFirstRequest() {
  const Set s;
  FakeEnv env;
  s.Serve(env);
  env.device.free_bytes = s.Level(kLevelA)->frame.size();
  Captured got;
  const nc::DemandResult r = nc::ResolveDemand(env, s.config, Input(kLevelA, kEchoArena), nullptr, got.Sink(), [] { return true; });
  QCHECK(!r.ok && std::string(r.reason) == kStatusOverStorage);
  QCHECK(GetsOfBlobs(env) == kNoEntries);
  QCHECK(got.sets.empty());
}

// An object the installed files hold verified is neither fetched nor mounted; the rest of the set is.
void AnInstalledObjectIsNotFetchedOrMounted() {
  const Set s;
  FakeEnv env;
  s.Serve(env);
  FakeInstalled installed;
  installed.set = &s;
  installed.verified.insert(s.Find(kAssetType, kName10)->sha);
  Captured got;
  const nc::DemandResult r = nc::ResolveDemand(env, s.config, Input(kLevelA, kEchoArena), &installed, got.Sink(), [] { return true; });
  QCHECK(r.ok && r.from_installed == kOne);
  for (const std::string& req : env.requests) {
    QCHECK(req != s.BlobUrl(s.Find(kAssetType, kName10)->sha));
  }
  QCHECK(got.sets.size() == kOne);
  nevr_shadow::Found found;
  QCHECK(!got.sets[0]->shadow->Find(kAssetType, kName10, &found));
  QCHECK(got.sets[0]->shadow->Find(kAssetType, kName11, &found));
  QCHECK(got.sets[0]->names == kFour);
}

// An installed source that does not verify its object leaves the object to the CDN.
void AnInstalledSourceThatDoesNotVerifyFallsThroughToTheCdn() {
  const Set s;
  FakeEnv env;
  s.Serve(env);
  FakeInstalled installed;
  installed.set = &s;  // verifies nothing
  Captured got;
  const nc::DemandResult r = nc::ResolveDemand(env, s.config, Input(kLevelA, kEchoArena), &installed, got.Sink(), [] { return true; });
  QCHECK(r.ok && r.from_installed == kNoEntries);
  QCHECK(r.fetched == kFive);
  QCHECK(GetsOfBlobs(env) == kFive);
}

// The demand resolve never saves the manifest: a cold store gets none, and a saved one keeps its bytes.
void TheDemandResolveNeverSavesTheManifest() {
  const Set s;
  FakeEnv env;
  s.Serve(env);
  Captured got;
  const nc::DemandResult cold = nc::ResolveDemand(env, s.config, Input(kLevelA, kEchoArena), nullptr, got.Sink(), [] { return true; });
  QCHECK(cold.ok);
  QCHECK(env.fs_->files.count(nc::ManifestCopyPath(kDir)) == kNoEntries);
  const std::string saved = "saved manifest of an earlier start";
  env.fs_->files[nc::ManifestCopyPath(kDir)] = saved;
  const nc::DemandResult warm = nc::ResolveDemand(env, s.config, Input(kLevelA, kEchoArena), nullptr, got.Sink(), [] { return true; });
  QCHECK(warm.ok);
  QCHECK(env.fs_->files[nc::ManifestCopyPath(kDir)] == saved);
}

// A store that holds a manifest name at another size refuses it by name: nothing is served, nothing is published.
void AHeldObjectAtAnotherSizeIsRefusedNotServed() {
  const Set s;
  FakeEnv env;
  s.Serve(env);
  const Item* asset = s.Find(kAssetType, kName10);
  HoldObjectAs(env, s.config, asset->sha, cdn_test::MakeFrame(asset->bytes, kShortFrameBytes, 'q'), asset->bytes);
  Captured got;
  const nc::DemandResult r = nc::ResolveDemand(env, s.config, Input(kLevelA, kEchoArena), nullptr, got.Sink(), [] { return true; });
  QCHECK(!r.ok && std::string(r.reason) == kStatusHeldSizeMismatch);
  QCHECK(r.name == nc::FormatHex64(kName10));
  QCHECK(got.sets.empty());
}

// The store answers no frame for a name it does not hold: the read fails and writes nothing.
void ReadFrameOfAnUnheldObjectIsRefused() {
  const Set s;
  FakeEnv env;
  nc::StoreOpenInfo info;
  std::unique_ptr<nc::Store> store = nc::Store::Open(env, s.config.dir, DefaultLimits(), &info);
  std::string frame;
  QCHECK(!store->ReadFrame(s.Find(kAssetType, kName10)->sha, &frame));
  QCHECK(frame.empty());
}

// Two sessions with two levels: each publishes its own set, and the first set stays valid after the second.
void TheSecondSessionPublishesItsOwnSet() {
  const Set s;
  FakeEnv env;
  s.Serve(env);
  Captured got;
  const nc::DemandResult a = nc::ResolveDemand(env, s.config, Input(kLevelA, kEchoArena), nullptr, got.Sink(), [] { return true; });
  QCHECK(a.ok && got.sets.size() == kOne);
  const nc::DemandResult c = nc::ResolveDemand(env, s.config, Input(kLevelC, kEchoArena), nullptr, got.Sink(), [] { return true; });
  QCHECK(c.ok && got.sets.size() == kTwo);
  nevr_shadow::Found found;
  QCHECK(got.sets[1]->shadow->Find(kAssetType, kName13, &found));
  QCHECK(!got.sets[1]->shadow->Find(kAssetType, kName10, &found));
  QCHECK(got.sets[0]->shadow->Find(kAssetType, kName10, &found));
}

// ── the modes' levels (LevelsOfMode, RootLevels) ───────────────────────────────────────────────────────────────────────────

bool Contains(const std::vector<std::uint64_t>& v, std::uint64_t x) {
  for (const std::uint64_t e : v) {
    if (e == x) return true;
  }
  return false;
}

// Combat is the four maps and the lobbies; the combat lobby is one of the six lobbies, so it is counted once: 4 + 6 = 10.
void LevelsOfModeCombatIsFourMapsLobbyAndLobbies() {
  const std::vector<std::uint64_t> levels = nc::LevelsOfMode(kEchoCombat);
  QCHECK(levels.size() == nevr_combat_level::kCombatMapCount + nevr_combat_level::kLobbyLevelCount);
  for (const std::uint64_t map : nevr_combat_level::kCombatMaps) QCHECK(Contains(levels, map));
  QCHECK(Contains(levels, nevr_combat_level::kCombatLobby));
  for (const std::uint64_t arena : nevr_combat_level::kEchoArenaLevels) QCHECK(!Contains(levels, arena));
}

void LevelsOfModeArenaHasArenaLevelsAndLobbies() {
  const std::vector<std::uint64_t> levels = nc::LevelsOfMode(kEchoArena);
  QCHECK(Contains(levels, kEchoArenaLevel0));
  QCHECK(Contains(levels, nevr_combat_level::kLobbyLevels[3]));
  for (const std::uint64_t map : nevr_combat_level::kCombatMaps) QCHECK(!Contains(levels, map));
}

void LevelsOfModeSocialIsLobbiesOnlyAndNpeAddsTheTutorialLobby() {
  const std::vector<std::uint64_t> social = nc::LevelsOfMode(kSocial);
  QCHECK(social.size() == nevr_combat_level::kLobbyLevelCount);
  QCHECK(!Contains(social, nevr_combat_level::kTutorialLobbyLevel));
  const std::vector<std::uint64_t> npe = nc::LevelsOfMode("social_2.0_npe");
  QCHECK(Contains(npe, nevr_combat_level::kTutorialLobbyLevel));
  QCHECK(npe.size() == social.size() + 1);
}

// Every service game type is placed: an unplaced one would resolve no mode roots.
void EveryServiceGameTypeIsPlaced() {
  for (const nevr_game_type_table::GameType& t : nevr_game_type_table::kGameTypes) {
    QCHECK(!nc::LevelsOfMode(t.name).empty());
  }
}

void AnUnplacedModeHasNoRoots() {
  QCHECK(nc::LevelsOfMode(kEchoDemo).empty());
  QCHECK(nc::RootLevels(Input(kLevelZero, kEchoDemo)).empty());
  const std::vector<std::uint64_t> one = nc::RootLevels(Input(kLevelA, kEchoDemo));
  QCHECK(one.size() == kOne && one[0] == kLevelA);
}

// ── the archive body (ParseArchiveBody) ────────────────────────────────────────────────────────────────────────────────────

void ParseArchiveBodyReadsTheFormatExactly() {
  nc::ArchiveBody parsed;
  const std::string good = ArchiveBody({kLevelB}, {{kAssetType, kName10}, {kAssetType, kName11}});
  QCHECK(nc::ParseArchiveBody(Bytes(good), &parsed));
  QCHECK(parsed.dependents.size() == kOne && parsed.dependents[0] == kLevelB);
  QCHECK(parsed.assets.size() == kTwo);
  // One byte short: the final u32 0 is cut.
  const std::string shortBody = good.substr(0, good.size() - kOne);
  QCHECK(!nc::ParseArchiveBody(Bytes(shortBody), &parsed));
  // N, the asset count, at the offset after the one dependent, claims more records than the bytes left hold.
  std::string bigN = good;
  const std::size_t countAt = nc::kU32Bytes + kU64Bytes;
  bigN[countAt] = static_cast<char>(kHugeCount & kByteMask);
  bigN[countAt + kOne] = static_cast<char>((kHugeCount >> kBitsPerByte) & kByteMask);
  QCHECK(!nc::ParseArchiveBody(Bytes(bigN), &parsed));
  // A byte after the final u32 0.
  std::string trailing = good;
  trailing.push_back(static_cast<char>(kTrailingByte));
  QCHECK(!nc::ParseArchiveBody(Bytes(trailing), &parsed));
  // D huge: a dependent count with no bytes behind it.
  std::string hugeD;
  PutU32(&hugeD, kHugeDependents);
  QCHECK(!nc::ParseArchiveBody(Bytes(hugeD), &parsed));
}

}  // namespace

int main() {
  ParseArchiveBodyReadsTheFormatExactly();
  TheNeededSetIsTheClosureOfTheLevelsArchive();
  ACombatModeTakesTheWholeCombatLabelToo();
  AnArchiveTheManifestLacksIsNotExpanded();
  ALabelThatIsOffContributesNothing();
  NoModeIsANamedFailure();
  NoLevelResolvesTheUnionOfTheModesLevels();
  ACombatSessionWithALevelStillNeedsAllFourMaps();
  AnUnknownModeWithALevelResolvesOnlyThatClosure();
  AFailedResolveIsAnErrorRecordAndACancelIsAWarning();
  LevelsOfModeCombatIsFourMapsLobbyAndLobbies();
  LevelsOfModeArenaHasArenaLevelsAndLobbies();
  LevelsOfModeSocialIsLobbiesOnlyAndNpeAddsTheTutorialLobby();
  EveryServiceGameTypeIsPlaced();
  AnUnplacedModeHasNoRoots();
  ABadArchiveFailsTheResolve();
  ACacheThatHoldsEverythingFetchesNothing();
  OnlyWhatTheCacheLacksIsFetched();
  AnObjectThatCannotBeHadFailsTheWholeResolve();
  AHashThatIsNotTheManifestsIsRefused();
  TheWaitIsNamedAndEndsTheResolve();
  OverStorageFailsBeforeTheFirstRequest();
  AnInstalledObjectIsNotFetchedOrMounted();
  AnInstalledSourceThatDoesNotVerifyFallsThroughToTheCdn();
  TheDemandResolveNeverSavesTheManifest();
  TheSecondSessionPublishesItsOwnSet();
  AHeldObjectAtAnotherSizeIsRefusedNotServed();
  ReadFrameOfAnUnheldObjectIsRefused();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "cdn_demand_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("cdn_demand_test: all checks passed\n");
  return 0;
}
