#pragma once
// The fixture the map channel's tests share (docs/adr/0048-custom-maps-channel.md): the fetch and the mount (core/map_run.h) over the
// shared fake Env and its MapFs (quest/tests/cargo_fake_env.h, map_fake_fs.h), a scripted server, a recording Mounter and Deriver,
// and the real parsers fed group manifests the fixture signs with fixed-seed keys. No network, no Win32, no game file. It has no
// GTest in it: the PC's GTest fixture and the Quest host's mini test derive from it.
#include "core/map_run.h"

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
namespace map_run_fixture {

using namespace nevr_maps;
using cargo_test::FakeEnv;
using cargo_test::FakeMapFs;
using cargo_test::Record;
using json = nlohmann::json;
namespace set = nevr_resource_set;

constexpr const char* kKeyId = "A8A7A6A5A4A3A2A1";  // TestKey seed 1
constexpr const char* kDir = "C:/nevr/cache/maps";
constexpr const char* kBase = "https://maps.test/";
constexpr const char* kNow = "2026-11-11T00:00:00Z";
constexpr const char* kSetPath = "set/windows-x86_64.nevrres";
// Two lobby groups: a UUID and a snowflake, since a group id is opaque.
constexpr const char* kGroup = "0b6e4c1a-7f3d-4e2a-9c5b-8d1e2f3a4b5c";
constexpr const char* kOtherGroup = "123456789012345678";
constexpr std::uint32_t kFrameBytes = 64, kChunkBytes = 100;
constexpr std::uint32_t kRecordAlignment = 16;
constexpr std::uint64_t kRecordType = 0x1111;
constexpr unsigned kBitsPerByte = 8;
constexpr std::uint8_t kFrameDescriptorFourByteSize = 0xA0;  // single segment, a 4-byte content size (RFC 8878 3.1.1.1)
constexpr std::size_t kFrameHeaderBytes = 9;                  // magic 4, descriptor 1, content size 4

std::string Sha(const std::string& bytes) { return cargo_test::Sha256Of(bytes); }

void PutU32(std::string* s, std::uint32_t v) {
  for (std::size_t i = 0; i < sizeof(v); ++i) s->push_back(static_cast<char>(v >> (kBitsPerByte * i)));
}
void PutU64(std::string* s, std::uint64_t v) {
  PutU32(s, static_cast<std::uint32_t>(v));
  PutU32(s, static_cast<std::uint32_t>(v >> (kBitsPerByte * sizeof(std::uint32_t))));
}

std::size_t FramesStart(int chunks) {
  return set::kHeaderBytes + (set::kRecordBytes + set::kChunkBytes) * static_cast<std::size_t>(chunks);
}

// A resource set of `chunks` records, one per chunk, each chunk a 64-byte zstd frame whose header states 100 bytes.
std::string MakeSet(int chunks, char salt) {
  std::string s(set::kMagic, sizeof(set::kMagic));
  const std::uint64_t records = set::kHeaderBytes, table = records + set::kRecordBytes * std::uint64_t(chunks),
                      overrides = table + set::kChunkBytes * std::uint64_t(chunks);
  PutU32(&s, set::kHeaderBytes);
  PutU32(&s, chunks);
  PutU32(&s, chunks);
  PutU32(&s, 0);
  PutU64(&s, records);
  PutU64(&s, table);
  PutU64(&s, overrides);
  for (int i = 0; i < chunks; ++i) {
    PutU64(&s, kRecordType);
    PutU64(&s, static_cast<std::uint64_t>(i + 1));
    PutU32(&s, i);
    PutU32(&s, 0);
    PutU32(&s, kChunkBytes);
    PutU32(&s, kRecordAlignment);
  }
  for (int i = 0; i < chunks; ++i) {
    PutU32(&s, 0);
    PutU32(&s, static_cast<std::uint32_t>(FramesStart(chunks) + kFrameBytes * i));
    PutU32(&s, kFrameBytes);
    PutU32(&s, kChunkBytes);
  }
  for (int i = 0; i < chunks; ++i) {
    PutU32(&s, set::kZstdMagic);
    s.push_back(static_cast<char>(kFrameDescriptorFourByteSize));
    PutU32(&s, kChunkBytes);
    s.append(kFrameBytes - kFrameHeaderBytes, static_cast<char>(salt + i));
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

// A fixed-seed minisign key, the "ED" form only.
struct TestKey {
  std::uint8_t secret[64];
  PinnedKey pin;

  // `compiled`: the key is one compiled into the build (only those satisfy -require-signed-plugins; ADR 0048 decision 7).
  explicit TestKey(std::uint8_t seed_byte, bool official = false, bool compiled = false) {
    std::uint8_t seed[32];
    std::memset(seed, seed_byte, sizeof seed);
    crypto_ed25519_key_pair(secret, pin.key.pk.data(), seed);
    for (std::size_t i = 0; i < pin.key.key_id.size(); ++i) pin.key.key_id[i] = static_cast<std::uint8_t>(0xA0 + seed_byte + i);
    pin.official = official;
    pin.compiled = compiled;
  }

  std::string SignedObject(const std::string& body) const {
    std::uint8_t digest[64];
    crypto_blake2b(digest, sizeof digest, reinterpret_cast<const std::uint8_t*>(body.data()), body.size());
    std::vector<std::uint8_t> blob = {'E', 'D'};
    blob.insert(blob.end(), pin.key.key_id.begin(), pin.key.key_id.end());
    std::uint8_t signature[64];
    crypto_ed25519_sign(signature, secret, digest, sizeof digest);
    blob.insert(blob.end(), signature, signature + sizeof signature);
    const std::string comment = "timestamp:1762819200";
    std::vector<std::uint8_t> global_message(signature, signature + sizeof signature);
    global_message.insert(global_message.end(), comment.begin(), comment.end());
    std::uint8_t global[64];
    crypto_ed25519_sign(global, secret, global_message.data(), global_message.size());
    return "untrusted comment: signature from a test key\n" + base64_encode(blob) + "\ntrusted comment: " + comment + "\n" +
           base64_encode(global, sizeof global) + "\n--\n" + body;
  }
};

json PinJson(const Pin& pin) { return {{"sha256", pin.sha256}, {"size", pin.size}}; }

json EditJson(const FieldEdit& e) { return {{"at", e.at}, {"width", e.width}, {"to", Hex64(e.to)}}; }

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
    aliases.push_back({{"key", Hex64(a.key)}, {"key_name", a.key_name}, {"alias", Hex64(a.alias)}, {"alias_name", a.alias_name}});
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

constexpr std::uint64_t kRecipeType = 0x10, kRecipeName = 0x20, kRecipeStockName = 0x21, kRecipeBytes = 4096;
constexpr std::uint64_t kRecipeEditAt = 128, kRecipeEditTo = 2;

void AddRecipe(Package& p) {
  Recipe r;
  r.type = kRecipeType;
  r.name = kRecipeName;
  r.stock_type = kRecipeType;
  r.stock_name = kRecipeStockName;
  r.stock_size = r.result_size = kRecipeBytes;
  r.stock_sha256 = Sha("stock bytes");
  r.result_sha256 = Sha("result bytes");
  r.fields.push_back(FieldEdit{kRecipeEditAt, sizeof(std::uint64_t), kRecipeEditTo});
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

// One group's manifest and the blobs it names, served by the fake env.
struct Channel {
  FakeEnv& env;
  const TestKey& key;
  std::string group = kGroup;
  std::string name = "echo-test";
  json maps = json::object();
  std::map<std::string, Package> packages{};

  Pin Blob(const std::string& bytes) {
    Pin pin;
    pin.sha256 = Sha(bytes);
    pin.size = bytes.size();
    env.server[std::string(kBase) + "blobs/sha256/" + pin.sha256].body = bytes;
    return pin;
  }

  // Cut at `cuts` (piece sizes; the rest is the last piece).
  Member MakeMember(const std::string& path, Role role, const std::string& bytes, const std::vector<std::size_t>& cuts) {
    Member m;
    m.path = path;
    m.role = role;
    m.size = bytes.size();
    m.sha256 = Sha(bytes);
    std::size_t at = 0;
    for (std::size_t cut : cuts) {
      m.pieces.push_back(Blob(bytes.substr(at, cut)));
      at += cut;
    }
    if (at < bytes.size()) m.pieces.push_back(Blob(bytes.substr(at)));
    return m;
  }

  const Package& Publish(const std::string& id, const std::string& version, const std::string& set_bytes,
                         const std::map<std::uint64_t, std::string>& scripts = {},
                         const std::function<void(Package&)>& edit = nullptr) {
    Package p;
    p.id = id;
    p.version = version;
    p.platform = kPlatformPc;
    p.level_name = id;
    p.level = EchoVR::CSymbol64Hash(id.c_str());
    p.source_sha256 = Sha("source " + id);
    // The cut rule in miniature: the header and tables, then one piece per frame.
    const int chunks = static_cast<int>((set_bytes.size() - set::kHeaderBytes) / (set::kRecordBytes + set::kChunkBytes + kFrameBytes));
    std::vector<std::size_t> cuts{FramesStart(chunks)};
    cuts.resize(static_cast<std::size_t>(chunks), kFrameBytes);
    p.members.push_back(MakeMember(kSetPath, Role::kSet, set_bytes, cuts));
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

  void Unpublish(const std::string& id) { maps.erase(id); }

  // The signed body names `body_group`, by default the group it is served for.
  std::string Sign(std::uint64_t serial, const std::string& expires = "2027-01-01T00:00:00Z",
                   const std::vector<std::string>& revoked_ids = {}, const std::vector<std::string>& revoked_sha = {},
                   const std::string& body_group = std::string()) {
    const json body = {{"schema", "nevr-maps-manifest"},
                       {"schema_version", 1},
                       {"channel", name},
                       {"guild", body_group.empty() ? group : body_group},
                       {"serial", serial},
                       {"expires", expires},
                       {"retired", json::object()},
                       {"revoked_ids", revoked_ids},
                       {"revoked_sha256", revoked_sha},
                       {"maps", maps}};
    const std::string signed_object = key.SignedObject(body.dump());
    env.server[ManifestUrl()].body = signed_object;
    return signed_object;
  }

  std::string ManifestUrl() const { return GroupManifestUrl(kBase, group); }
  std::string IndexSha(const std::string& id) const {
    return maps.at(id).at("platforms").at(kPlatformPc).at("index").at("sha256").get<std::string>();
  }
  std::string VersionDir(const std::string& id) const {
    return std::string(kDir) + "/" + id + "/" + kPlatformPc + "/" + VersionDirName(IndexSha(id));
  }
};

// Records the grants a mount asks for; a granted handle is the mounter's until Withdraw closes it.
class FakeMounter : public Mounter {
 public:
  explicit FakeMounter(FakeEnv& env) : env_(env) {}

  std::vector<std::string> calls;
  std::set<std::uint64_t> refuse_scripts;
  std::set<std::string> refuse_sets;    // owners whose Set is refused "collides"
  std::map<std::string, Grant> alias;   // owner -> the StageAliases result (default ok)
  std::map<std::string, bool> verified;  // owner -> verified_stamp of its Set
  std::map<std::string, std::string> labels, set_sha;
  std::uint64_t set_cost_ms = 0;  // the clock moves this much per Set (the boot budget)
  std::vector<bool> script_official;  // the `official` each Script call was given, in order

  Grant Script(const std::string& owner, std::uint64_t hash, std::int64_t held, const std::string&, bool official) override {
    calls.push_back("script " + owner + " " + std::to_string(hash));
    script_official.push_back(official);
    return Take(owner, held, !refuse_scripts.count(hash), "pe_rules");
  }
  Grant Rekey(const std::string& owner, std::uint64_t hash, std::uint64_t, const std::string&) override {
    calls.push_back("rekey " + owner + " " + std::to_string(hash));
    return Ok(!refuse_scripts.count(hash), "rekey_mismatch");
  }
  Grant Set(const std::string& owner, std::int64_t held, const std::string& label, const std::string& sha256,
            bool verified_stamp) override {
    calls.push_back("set " + owner);
    env_.clock += set_cost_ms;
    verified[owner] = verified_stamp;
    labels[owner] = label;
    set_sha[owner] = sha256;
    return Take(owner, held, !refuse_sets.count(owner), "collides");
  }
  Grant StageAliases(const std::string& owner, const std::string&, std::uint64_t, const std::vector<ConfigAlias>&) override {
    calls.push_back("aliases " + owner);
    const auto it = alias.find(owner);
    return it == alias.end() ? Ok(true, "") : it->second;
  }
  Grant DerivedScript(const std::string& owner, std::uint64_t hash, std::int64_t held, const std::string&,
                      const std::string&, bool) override {
    calls.push_back("derived " + owner + " " + std::to_string(hash));
    return Take(owner, held, !refuse_scripts.count(hash), "pe_rules");
  }
  void Withdraw(const std::string& owner) override {
    calls.push_back("withdraw " + owner);
    for (std::int64_t handle : owned_[owner]) env_.CloseBlob(handle);
    owned_.erase(owner);
  }
  std::size_t Owned() const {
    std::size_t n = 0;
    for (const auto& [owner, held] : owned_) n += held.size();
    return n;
  }
  std::size_t Owned(const std::string& owner) const {
    const auto it = owned_.find(owner);
    return it == owned_.end() ? 0 : it->second.size();
  }
  bool Called(const std::string& call) const { return std::find(calls.begin(), calls.end(), call) != calls.end(); }

 private:
  static Grant Ok(bool ok, const char* reason) {
    Grant g;
    g.ok = ok;
    if (!ok) g.reason = reason;
    return g;
  }
  Grant Take(const std::string& owner, std::int64_t held, bool ok, const char* reason) {
    if (ok) owned_[owner].push_back(held);
    return Ok(ok, reason);
  }

  FakeEnv& env_;
  std::map<std::string, std::vector<std::int64_t>> owned_;
};

// Rebuilds a recipe map the simple way: the merged set is the published set's bytes, and each rekeyed script with edits is the
// bytes the test registered for its hash. Every rebuild reads the same stock file twice, as a real rebuild's read_from would.
class FakeDeriver : public Deriver {
 public:
  explicit FakeDeriver(FakeMapFs& fs, FakeEnv& env) : fs_(fs), env_(env) {}

  bool current = true;
  std::string reason;
  int derive_calls = 0;
  std::vector<std::string> stock_checked;  // one path per StockCurrent call
  std::map<std::uint64_t, std::string> rebuilt_scripts;

  DeriveOutput Derive(const Package& package, const std::string& version_dir, const std::function<bool()>&) override {
    ++derive_calls;
    DeriveOutput out;
    out.reason = reason;
    if (!reason.empty()) return out;
    StockStamp stock;
    stock.path = "C:/game/_data/packages/stock0";
    stock.stamp.id_lo = 1;
    out.stock_stamps = {stock, stock};
    if (!package.derived.empty()) {
      std::string bytes;
      env_.ReadSmallFile(version_dir + "/" + kSetPath, &bytes, std::size_t{1} << 30);
      out.set_path = version_dir + "/derived/windows-x86_64.nevrres";
      fs_.Put(out.set_path, bytes);
      out.set_sha256 = Sha(bytes);
    }
    for (const RekeyPin& r : package.rekeyed) {
      if (r.edits.empty()) continue;
      const std::string name = "derived/" + ScriptName(r.hash);
      fs_.Put(version_dir + "/" + name, rebuilt_scripts[r.hash]);
      out.scripts.emplace_back(r.hash, name);
    }
    return out;
  }
  bool StockCurrent(const std::vector<StockStamp>& stamps) override {
    for (const StockStamp& s : stamps) stock_checked.push_back(s.path);
    return current;
  }

 private:
  FakeMapFs& fs_;
  FakeEnv& env_;
};

std::size_t PieceCount(const Package& p) {
  std::size_t n = 0;
  for (const Member& m : p.members) n += m.pieces.size();
  return n;
}

// The fixture of one map channel test: a fake env and map file system, a recording mounter and deriver, one signing key and one
// channel of that key's signed manifests. Plain C++: the PC's GTest fixture (core/tests/map_run_test.cpp) and the Quest host's
// test program (quest/tests/map_mount_rules_test.cpp) both derive from it, so both drive the same code over the same fakes.
struct Fixture {
  Fixture() {
    config.keys = {key.pin};
    config.dir = kDir;
    config.base_url = kBase;
    env.MakeDirs(kDir);
  }

  FetchReport Fetch(Deriver* d = nullptr, const std::vector<std::string>& deferred = {}, const std::string& group = kGroup,
                    const FetchProgressFn& progress = nullptr) {
    return FetchGroup(env, fs, config, group, deferred, d, kNow, keep_going, progress);
  }
  // One launch: a fresh mount of the given groups.
  MountReport Boot(Deriver* d = nullptr, const std::string& now = kNow,
                   const std::vector<std::string>& groups = {kGroup}) {
    CacheMount mount(env, fs, config, mounter, d);
    return mount.Mount(groups, now);
  }
  static std::string TokenOf(const MountReport& r, const std::string& id) {
    for (const auto& [mid, token] : r.not_mounted) {
      if (mid == id) return token;
    }
    return std::string();
  }
  static bool Mounted(const MountReport& r, const std::string& id) {
    return std::find(r.mounted.begin(), r.mounted.end(), id) != r.mounted.end();
  }
  // The cache index as it is on disk; a test that asserts on the read checks its status itself.
  CacheIndex Index() {
    CacheIndex index;
    static_cast<void>(ReadCacheIndex(env, kDir, &index));
    return index;
  }
  std::size_t GetsOf(const std::string& url) const {
    return static_cast<std::size_t>(std::count(env.requests.begin(), env.requests.end(), url));
  }
  std::size_t GetsUnder(const std::string& prefix) const {
    return static_cast<std::size_t>(std::count_if(env.requests.begin(), env.requests.end(), [&](const std::string& url) {
      return url.compare(0, prefix.size(), prefix) == 0;
    }));
  }

  FakeEnv env;
  FakeMapFs fs{env};
  FakeMounter mounter{env};
  FakeDeriver deriver{fs, env};
  TestKey key{1};
  Channel channel{env, key};
  ChannelConfig config;
  bool keep = true;
  std::function<bool()> keep_going = [this] { return keep; };
};

}  // namespace map_run_fixture
