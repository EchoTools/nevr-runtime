// A guild's manifest, the package index and the minisign verifier (docs/adr/0047-custom-maps-channel.md decisions 1 to 3).
// Manifests are signed here with fixed-seed test keys; one signature made by the minisign tool itself, with a key made for the test
// (tests/vectors/maps/minisign), cross-checks the verifier. The index cases are the shared vector file
// tests/vectors/maps/package_index_cases.json.
#include <gtest/gtest.h>

#include <nevr/symbol_hash.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/base64.h"
#include "core/map_manifest.h"
#include "core/minisign_verify.h"
#include "monocypher-ed25519.h"
#include "monocypher.h"
#include "quest/data/sha256.h"

namespace {

using json = nlohmann::json;
using nevr_maps::FloorBook;
using nevr_maps::ParsedManifest;
using nevr_maps::PinnedKey;
using nevr_minisign::Verdict;

constexpr const char* kNow = "2026-11-11T00:00:00Z";
constexpr const char* kUrl = "https://maps.example/";
constexpr const char* kAuthorId = "A8A7A6A5A4A3A2A1";    // TestKey seed 1
constexpr const char* kOfficialId = "A9A8A7A6A5A4A3A2";  // TestKey seed 2
constexpr const char* kGuild = "0b6e4c1a-7f3d-4e2a-9c5b-8d1e2f3a4b5c";
constexpr const char* kOtherGuild = "c4d5e6f7-0a1b-4c2d-8e3f-a1b2c3d4e5f6";
// What the device lets one document take in these tests: above the v1 limits, so those apply unless a test lowers it.
constexpr std::uint64_t kDeviceDocumentBytes = std::uint64_t{64} << 20;
// The minisign tool's vector (tests/vectors/maps/minisign/README.md).
constexpr const char* kVectorKeyLine = "RWT7b12K0ybPErgOPJNseA689+mTi7fFLcTuzrBCVDwzdi7wbqXFzF5l";
constexpr const char* kVectorKeyId = "12CF26D38A5D6FFB";
constexpr const char* kVectorGuild = "3f2c9a5e-8b1d-4c7a-9e2f-1a2b3c4d5e6f";

std::string ReadVector(const std::string& name) {
  std::ifstream in(std::string(NEVR_MAP_VECTORS) + "/" + name, std::ios::binary);
  EXPECT_TRUE(in.good()) << name;
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string Hex64(std::uint64_t value) {
  char text[19];
  std::snprintf(text, sizeof text, "0x%016llx", static_cast<unsigned long long>(value));
  return text;
}

std::string Rad(const char* name) { return Hex64(EchoVR::CSymbol64Hash(name)); }

std::string Sha(char c) { return std::string(64, c); }

// "$rad:<name>" -> the hex of rad_hash(<name>), everywhere in the document.
void Substitute(json& j) {
  if (j.is_string()) {
    const std::string& s = j.get_ref<const std::string&>();
    if (s.rfind("$rad:", 0) == 0) j = Rad(s.c_str() + 5);
  } else if (j.is_structured()) {
    for (auto& child : j) Substitute(child);
  }
}

struct TestKey {
  std::uint8_t secret[64];
  PinnedKey pin;

  TestKey(std::uint8_t seed_byte, bool official) {
    std::uint8_t seed[32];
    std::memset(seed, seed_byte, sizeof seed);
    crypto_ed25519_key_pair(secret, pin.key.pk.data(), seed);
    for (std::size_t i = 0; i < 8; ++i) pin.key.key_id[i] = static_cast<std::uint8_t>(0xA0 + seed_byte + i);
    pin.official = official;
  }

  std::string Sign(const std::string& message, const std::string& comment = "timestamp:1762819200",
                   char algorithm = 'D') const {
    std::uint8_t digest[64];
    crypto_blake2b(digest, sizeof digest, reinterpret_cast<const std::uint8_t*>(message.data()), message.size());
    std::vector<std::uint8_t> blob = {'E', static_cast<std::uint8_t>(algorithm)};
    blob.insert(blob.end(), pin.key.key_id.begin(), pin.key.key_id.end());
    std::uint8_t signature[64];
    if (algorithm == 'D') {
      crypto_ed25519_sign(signature, secret, digest, sizeof digest);
    } else {
      crypto_ed25519_sign(signature, secret, reinterpret_cast<const std::uint8_t*>(message.data()), message.size());
    }
    blob.insert(blob.end(), signature, signature + 64);
    std::vector<std::uint8_t> global_message(signature, signature + 64);
    global_message.insert(global_message.end(), comment.begin(), comment.end());
    std::uint8_t global[64];
    crypto_ed25519_sign(global, secret, global_message.data(), global_message.size());
    return "untrusted comment: signature from a test key\n" + base64_encode(blob) + "\ntrusted comment: " + comment +
           "\n" + base64_encode(global, sizeof global) + "\n";
  }

  std::string SignedObject(const std::string& body) const { return Sign(body) + "--\n" + body; }
};

json PlatformEntryJson(const char* level_name, const char* version, char index_sha) {
  return {{"version", version}, {"index", {{"sha256", Sha(index_sha)}, {"size", 5321}}}, {"level", Rad(level_name)},
          {"level_name", level_name}, {"game_build", "echovr.exe 34.4.631547"}, {"bytes", 412345678}, {"scripts", 3}};
}

json BaseManifest(const std::string& channel = "echo-community", std::uint64_t serial = 42,
                  const std::string& guild = kGuild) {
  json doc = {{"schema", "nevr-maps-manifest"}, {"schema_version", 1}, {"channel", channel}, {"guild", guild}, {"serial", serial},
              {"expires", "2027-02-09T00:00:00Z"}, {"retired", {{"oldmap/windows-x86_64", "1.2.0"}}},
              {"revoked_ids", json::array()}, {"revoked_sha256", json::array()}};
  doc["maps"]["arena"] = {{"title", "Arena"}, {"author", "someone"},
                          {"thumbnail", {{"sha256", Sha('9')}, {"size", 123}}}};
  doc["maps"]["arena"]["platforms"]["windows-x86_64"] = PlatformEntryJson("arena", "1.2.0", '1');
  doc["maps"]["arena"]["platforms"]["android-arm64"] = PlatformEntryJson("arena_lowspec", "1.1.0", '2');
  return doc;
}

struct MapManifest : ::testing::Test {
  TestKey author{1, false};
  TestKey official{2, true};
  TestKey stranger{3, false};
  std::vector<PinnedKey> keys{author.pin, official.pin};

  ParsedManifest Parse(const std::string& object, const FloorBook& floors = FloorBook(),
                       const std::string& expected = "", const char* now = kNow) {
    return nevr_maps::ParseAndVerifyManifest(object, keys, floors, expected, now, kDeviceDocumentBytes);
  }
  ParsedManifest ParseBody(const json& doc, const FloorBook& floors = FloorBook(), const std::string& expected = "") {
    return Parse(author.SignedObject(doc.dump()), floors, expected);
  }
};


TEST_F(MapManifest, AcceptsASignedManifest) {
  const std::string body = BaseManifest().dump();
  const ParsedManifest p = Parse(author.SignedObject(body));
  ASSERT_EQ(p.reason, "");
  EXPECT_EQ(p.channel, "echo-community");
  EXPECT_EQ(p.guild, kGuild);
  EXPECT_EQ(p.manifest.serial, 42u);
  EXPECT_EQ(p.manifest.expires, "2027-02-09T00:00:00Z");
  EXPECT_EQ(p.body_sha256, nevr_quest::data::Sha256Hex(body.data(), body.size()));
  EXPECT_EQ(p.key_id, "A8A7A6A5A4A3A2A1");  // the id bytes A1..A8, printed as minisign prints them
  EXPECT_FALSE(p.official);
  ASSERT_EQ(p.manifest.maps.count("arena"), 1u);
  const auto& arena = p.manifest.maps.at("arena");
  EXPECT_EQ(arena.title, "Arena");
  EXPECT_EQ(arena.thumbnail.size, 123u);
  ASSERT_EQ(arena.platforms.size(), 2u);
  const auto& pc = arena.platforms.at(nevr_maps::kPlatformPc);
  EXPECT_EQ(pc.version, "1.2.0");
  EXPECT_EQ(pc.level, EchoVR::CSymbol64Hash("arena"));
  EXPECT_EQ(pc.index.sha256, Sha('1'));
  EXPECT_EQ(pc.index.size, 5321u);
  EXPECT_EQ(pc.scripts, 3u);
  EXPECT_EQ(arena.platforms.at(nevr_maps::kPlatformQuest).level_name, "arena_lowspec");
  EXPECT_TRUE(p.dropped.empty());
}

// The group id is opaque: a manifest for a group whose id is not a UUID (a snowflake) is accepted and names it.
TEST_F(MapManifest, AGroupIdThatIsNotAUuidIsAccepted) {
  const ParsedManifest p = ParseBody(BaseManifest("echo-community", 42, "123456789012345678"));
  ASSERT_EQ(p.reason, "");
  EXPECT_EQ(p.guild, "123456789012345678");
}

TEST_F(MapManifest, OfficialNamesTheKeyThatVerified) {
  EXPECT_TRUE(Parse(official.SignedObject(BaseManifest().dump())).official);
  // An author key that claims the official key's id does not verify as official.
  TestKey impostor{4, false};
  impostor.pin.key.key_id = official.pin.key.key_id;
  keys.push_back(impostor.pin);
  const ParsedManifest p = Parse(impostor.SignedObject(BaseManifest().dump()));
  EXPECT_EQ(p.reason, "");
  EXPECT_FALSE(p.official);
}

TEST_F(MapManifest, UnknownKeysAndPlatformsAreIgnored) {
  json doc = BaseManifest();
  doc["future"] = {{"x", 1}};
  doc["maps"]["arena"]["platforms"]["linux-riscv"] = {{"anything", true}};
  doc["maps"]["questonly"]["platforms"]["linux-riscv"] = {{"anything", true}};
  const ParsedManifest p = ParseBody(doc);
  ASSERT_EQ(p.reason, "");
  EXPECT_EQ(p.manifest.maps.at("arena").platforms.size(), 2u);
  EXPECT_EQ(p.manifest.maps.count("questonly"), 0u);  // no platform this client knows: not listed
}

TEST_F(MapManifest, AnExpiredManifestIsAcceptedAndExpiryIsPerMap) {
  const ParsedManifest p = Parse(author.SignedObject(BaseManifest().dump()), FloorBook(), "", "2027-03-01T00:00:00Z");
  ASSERT_EQ(p.reason, "");
  EXPECT_TRUE(nevr_maps::IsExpired(p.manifest.expires, "2027-03-01T00:00:00Z"));
  EXPECT_FALSE(nevr_maps::IsExpired(p.manifest.expires, kNow));
}

struct BodyCase {
  const char* name;
  void (*mutate)(json&);
  const char* reason;
};

TEST_F(MapManifest, EveryBodyRejectionHasItsReason) {
  const BodyCase cases[] = {
      {"schema", [](json& d) { d["schema"] = "nevr-maps-manifes"; }, "bad_schema"},
      {"schema_version", [](json& d) { d["schema_version"] = 2; }, "bad_schema"},
      {"channel_missing", [](json& d) { d.erase("channel"); }, "channel_missing"},
      {"channel_upper", [](json& d) { d["channel"] = "Echo"; }, "bad_channel"},
      {"channel_dot_first", [](json& d) { d["channel"] = ".echo"; }, "bad_channel"},
      {"channel_too_long", [](json& d) { d["channel"] = std::string(65, 'a'); }, "bad_channel"},
      {"channel_number", [](json& d) { d["channel"] = 7; }, "bad_channel"},
      {"guild_missing", [](json& d) { d.erase("guild"); }, "guild_missing"},
      {"guild_upper", [](json& d) { d["guild"] = "0B6E4C1A-7F3D-4E2A-9C5B-8D1E2F3A4B5C"; }, "bad_guild"},
      {"guild_braced", [](json& d) { d["guild"] = "{0b6e4c1a-7f3d-4e2a-9c5b-8d1e2f3a4b5c}"; }, "bad_guild"},
      {"guild_slash", [](json& d) { d["guild"] = "0b6e4c1a/7f3d"; }, "bad_guild"},
      {"guild_dot", [](json& d) { d["guild"] = ".."; }, "bad_guild"},
      {"guild_too_long", [](json& d) { d["guild"] = std::string(nevr_maps::kGroupIdMaxBytes + 1, 'a'); }, "bad_guild"},
      {"guild_number", [](json& d) { d["guild"] = 7; }, "bad_guild"},
      {"guild_empty", [](json& d) { d["guild"] = ""; }, "bad_guild"},
      {"serial_zero", [](json& d) { d["serial"] = 0; }, "malformed"},
      {"serial_negative", [](json& d) { d["serial"] = -1; }, "malformed"},
      {"serial_text", [](json& d) { d["serial"] = "42"; }, "malformed"},
      {"serial_float", [](json& d) { d["serial"] = 42.5; }, "malformed"},
      {"expires_missing", [](json& d) { d.erase("expires"); }, "malformed"},
      {"expires_not_a_date", [](json& d) { d["expires"] = "2027-02-30T00:00:00Z"; }, "malformed"},
      {"expires_offset", [](json& d) { d["expires"] = "2027-02-09T00:00:00+01"; }, "malformed"},
      {"maps_missing", [](json& d) { d.erase("maps"); }, "malformed"},
      {"maps_list", [](json& d) { d["maps"] = json::array(); }, "malformed"},
      {"revoked_ids_object", [](json& d) { d["revoked_ids"] = json::object(); }, "malformed"},
      {"map_id_upper", [](json& d) { d["maps"]["Arena"] = d["maps"]["arena"]; }, "bad_id"},
      {"map_id_too_long", [](json& d) { d["maps"][std::string(56, 'a')] = json::object(); }, "bad_id"},
      {"revoked_id_bad", [](json& d) { d["revoked_ids"] = {"9bad"}; }, "bad_id"},
      {"revoked_sha_bad", [](json& d) { d["revoked_sha256"] = {Sha('G')}; }, "bad_hash"},
      {"platforms_missing", [](json& d) { d["maps"]["arena"].erase("platforms"); }, "malformed"},
      {"title_number", [](json& d) { d["maps"]["arena"]["title"] = 1; }, "malformed"},
      {"thumbnail_sha", [](json& d) { d["maps"]["arena"]["thumbnail"]["sha256"] = "x"; }, "bad_hash"},
      {"version_leading_zero",
       [](json& d) { d["maps"]["arena"]["platforms"]["windows-x86_64"]["version"] = "1.02.0"; }, "bad_version"},
      {"version_five_parts",
       [](json& d) { d["maps"]["arena"]["platforms"]["windows-x86_64"]["version"] = "1.2.3.4.5"; }, "bad_version"},
      {"version_text", [](json& d) { d["maps"]["arena"]["platforms"]["windows-x86_64"]["version"] = "1.2.0-beta"; },
       "bad_version"},
      {"level_name_not_id",
       [](json& d) { d["maps"]["arena"]["platforms"]["windows-x86_64"]["level_name"] = "arena2"; }, "bad_level"},
      {"level_not_its_hash", [](json& d) { d["maps"]["arena"]["platforms"]["windows-x86_64"]["level"] = "0x1"; },
       "bad_level"},
      {"quest_level_name_pc_form",
       [](json& d) {
         d["maps"]["arena"]["platforms"]["android-arm64"]["level_name"] = "arena";
         d["maps"]["arena"]["platforms"]["android-arm64"]["level"] = Rad("arena");
       },
       "bad_level"},
      {"index_sha_upper",
       [](json& d) { d["maps"]["arena"]["platforms"]["windows-x86_64"]["index"]["sha256"] = Sha('A'); }, "bad_hash"},
      {"index_size_zero", [](json& d) { d["maps"]["arena"]["platforms"]["windows-x86_64"]["index"]["size"] = 0; },
       "malformed"},
      {"index_over_cap",
       [](json& d) { d["maps"]["arena"]["platforms"]["windows-x86_64"]["index"]["size"] = nevr_maps::kMaxIndexBytes + 1; },
       "malformed"},
      {"scripts_over_u32",
       [](json& d) { d["maps"]["arena"]["platforms"]["windows-x86_64"]["scripts"] = 4294967296ull; }, "malformed"},
      {"level_missing", [](json& d) { d["maps"]["arena"]["platforms"]["windows-x86_64"].erase("level"); },
       "malformed"},
  };
  for (const BodyCase& c : cases) {
    json doc = BaseManifest();
    c.mutate(doc);
    const ParsedManifest p = ParseBody(doc);
    EXPECT_EQ(p.reason, c.reason) << c.name;
    EXPECT_TRUE(p.manifest.maps.empty()) << c.name;
    EXPECT_TRUE(p.channel.empty()) << c.name;
    EXPECT_TRUE(p.guild.empty()) << c.name;
  }
}

TEST_F(MapManifest, SignatureIsCheckedBeforeAnyJsonAndEveryFailureHasItsReason) {
  const std::string body = BaseManifest().dump();
  const std::string good = author.SignedObject(body);

  EXPECT_EQ(Parse(stranger.SignedObject(body)).reason, "unknown_key");
  EXPECT_EQ(Parse(author.Sign(body, "timestamp:1", 'd') + "--\n" + body).reason, "unsupported_signature");

  std::string flipped = good;
  flipped[flipped.size() - 2] ^= 0x01;
  EXPECT_EQ(Parse(flipped).reason, "bad_signature");

  std::string comment_edited = good;
  const std::size_t at = comment_edited.find("timestamp:");
  comment_edited[at] = 'T';
  EXPECT_EQ(Parse(comment_edited).reason, "bad_global_signature");

  // Not JSON, duplicate keys and deep nesting: each is signed, so only the parser can refuse it.
  EXPECT_EQ(Parse(author.SignedObject("{\"schema\":")).reason, "malformed");
  const std::string duplicate = body.substr(0, body.size() - 1) + ",\"serial\":43}";
  EXPECT_EQ(Parse(author.SignedObject(duplicate)).reason, "malformed");
  std::string deep = std::string(40, '[') + std::string(40, ']');
  deep = "{\"schema\":\"nevr-maps-manifest\",\"x\":" + deep + "}";
  EXPECT_EQ(Parse(author.SignedObject(deep)).reason, "malformed");
}

TEST_F(MapManifest, SplitAndSizeCaps) {
  const std::string body = BaseManifest().dump();
  const std::string good = author.SignedObject(body);
  std::string_view minisig, rest;
  ASSERT_TRUE(nevr_maps::SplitSigned(good, &minisig, &rest));
  EXPECT_EQ(rest, body);
  EXPECT_EQ(minisig.back(), '\n');

  EXPECT_FALSE(nevr_maps::SplitSigned(good.substr(0, 60), &minisig, &rest));
  EXPECT_EQ(Parse(good.substr(0, 60)).reason, "malformed");                       // truncated in the minisig
  EXPECT_EQ(Parse(good.substr(0, good.find("--\n") + 3)).reason, "malformed");     // no body
  EXPECT_EQ(Parse(author.Sign(body) + body).reason, "malformed");                  // no separator
  EXPECT_EQ(Parse(author.Sign(body) + "--\n" + body.substr(0, body.size() - 5)).reason, "bad_signature");

  const std::string big(nevr_maps::kMaxManifestBytes + 1, ' ');
  EXPECT_EQ(Parse(author.Sign(big) + "--\n" + big).reason, "too_large");
  const std::string long_comment = "untrusted comment: " + std::string(nevr_maps::kMaxMinisigBytes, 'x') + "\n";
  EXPECT_EQ(Parse(long_comment + author.Sign(body).substr(author.Sign(body).find('\n') + 1) + "--\n" + body).reason,
            "too_large");
  EXPECT_EQ(Parse(std::string(nevr_maps::kMaxMinisigBytes + nevr_maps::kSignedSeparator.size() + nevr_maps::kMaxManifestBytes + 1,
                              'x'))
                .reason,
            "too_large");
  EXPECT_EQ(Parse(good, FloorBook(), "", "").reason, "bad_now");
  EXPECT_EQ(Parse(good, FloorBook(), "", "2026-11-11 00:00:00").reason, "bad_now");
}

TEST_F(MapManifest, SerialFloorRefusesRollbackAndReuseWhicheverKeySigned) {
  FloorBook floors;
  const std::string a42 = author.SignedObject(BaseManifest("echo-community", 42).dump());
  ParsedManifest p = Parse(a42, floors);
  ASSERT_EQ(p.reason, "");
  floors = nevr_maps::AdvanceFloors(floors, kUrl, p);
  EXPECT_EQ(floors.channels.at("echo-community").at(kGuild).at(kAuthorId).serial, 42u);
  EXPECT_EQ(floors.channels.at("echo-community").at(kGuild).at(kAuthorId).serial_sha256, p.body_sha256);

  EXPECT_EQ(Parse(a42, floors).reason, "");  // the same bytes again: no change

  json other = BaseManifest("echo-community", 42);
  other["revoked_ids"] = {"badmap"};
  EXPECT_EQ(Parse(author.SignedObject(other.dump()), floors).reason, "serial_reuse");
  EXPECT_EQ(Parse(official.SignedObject(other.dump()), floors).reason, "serial_reuse");

  // The rotated-key replay: key A at 42, key B at 43, then A's 42 again.
  p = Parse(official.SignedObject(BaseManifest("echo-community", 43).dump()), floors);
  ASSERT_EQ(p.reason, "");
  floors = nevr_maps::AdvanceFloors(floors, kUrl, p);
  EXPECT_EQ(Parse(a42, floors).reason, "serial_down");
  EXPECT_EQ(floors.channels.at("echo-community").at(kGuild).at(kOfficialId).serial, 43u);
  EXPECT_EQ(floors.channels.at("echo-community").at(kGuild).at(kAuthorId).serial, 42u);
}

TEST_F(MapManifest, FloorsAreKeptPerSignedChannel) {
  FloorBook floors;
  const std::string x42 = author.SignedObject(BaseManifest("channel-x", 42).dump());
  ParsedManifest p = Parse(x42, floors, "");  // first use: any channel, and the caller binds it
  ASSERT_EQ(p.reason, "");
  floors = nevr_maps::AdvanceFloors(floors, kUrl, p);
  EXPECT_EQ(floors.bound.at(kUrl), "channel-x");

  // Switching to channel Y meets Y's own (empty) floor, not X's.
  p = Parse(author.SignedObject(BaseManifest("channel-y", 7).dump()), floors, "channel-y");
  ASSERT_EQ(p.reason, "");
  floors = nevr_maps::AdvanceFloors(floors, kUrl, p);
  EXPECT_EQ(floors.channels.at("channel-x").at(kGuild).at(kAuthorId).serial, 42u);
  EXPECT_EQ(floors.channels.at("channel-y").at(kGuild).at(kAuthorId).serial, 7u);
  EXPECT_EQ(floors.bound.at(kUrl), "channel-y");

  // X's manifest replayed on Y's URL.
  EXPECT_EQ(Parse(x42, floors, "channel-y").reason, "wrong_channel");
  // Back on X, X's floor still holds.
  EXPECT_EQ(Parse(author.SignedObject(BaseManifest("channel-x", 41).dump()), floors, "channel-x").reason, "serial_down");
}

// Each guild's manifest has its own serials: one guild's floor never holds another's back, and a rollback stays refused in each.
TEST_F(MapManifest, FloorsAreKeptPerGuild) {
  FloorBook floors;
  ParsedManifest p = Parse(author.SignedObject(BaseManifest("echo-community", 42, kGuild).dump()), floors);
  ASSERT_EQ(p.reason, "");
  floors = nevr_maps::AdvanceFloors(floors, kUrl, p);

  p = Parse(author.SignedObject(BaseManifest("echo-community", 7, kOtherGuild).dump()), floors);
  ASSERT_EQ(p.reason, "");
  EXPECT_EQ(p.guild, kOtherGuild);
  floors = nevr_maps::AdvanceFloors(floors, kUrl, p);
  EXPECT_EQ(floors.channels.at("echo-community").at(kGuild).at(kAuthorId).serial, 42u);
  EXPECT_EQ(floors.channels.at("echo-community").at(kOtherGuild).at(kAuthorId).serial, 7u);

  EXPECT_EQ(Parse(author.SignedObject(BaseManifest("echo-community", 41, kGuild).dump()), floors).reason, "serial_down");
  EXPECT_EQ(Parse(author.SignedObject(BaseManifest("echo-community", 6, kOtherGuild).dump()), floors).reason, "serial_down");
}

// ADR 0045 decision 8: a document is read only within the device's memory share, and a device that gives no memory fact reads none.
TEST_F(MapManifest, TheDeviceBoundsTheDocumentItReads) {
  const std::string body = BaseManifest().dump();
  const std::string good = author.SignedObject(body);
  EXPECT_EQ(nevr_maps::ParseAndVerifyManifest(good, keys, FloorBook(), "", kNow, body.size()).reason, "");
  EXPECT_EQ(nevr_maps::ParseAndVerifyManifest(good, keys, FloorBook(), "", kNow, body.size() - 1).reason, "too_large");
  EXPECT_EQ(nevr_maps::ParseAndVerifyManifest(good, keys, FloorBook(), "", kNow, 0).reason, "no_memory");
}

TEST(MapDocumentBytes, IsAShareOfTheMemoryTheDeviceReports) {
  nevr_cargo::DeviceFacts facts;
  EXPECT_EQ(nevr_maps::MapDocumentBytes(facts), 0u);  // no memory fact: nothing is read
  facts.have_memory = true;
  facts.memory_available = std::uint64_t{3} << 30;
  // The texture cargo's manifest arithmetic (cargo_limits.h): 1/8 of memory for the text, its copy and its parsed form (2 + 6).
  EXPECT_EQ(nevr_maps::MapDocumentBytes(facts), (std::uint64_t{3} << 30) / (8 * (2 + 6)));
  facts.memory_available = std::uint64_t{1} << 20;
  EXPECT_EQ(nevr_maps::MapDocumentBytes(facts), (std::uint64_t{1} << 20) / (8 * (2 + 6)));
}

// Test key to official key on one channel name: the official manifest is not held back by the test key's floors,
// and a rollback under the official key itself is still refused.
TEST_F(MapManifest, AKeyNoLongerTrustedHoldsNobodyBackButARollbackUnderTheSameKeyIsRefused) {
  FloorBook floors;
  json test_doc = BaseManifest("echo-community", 40);
  test_doc["maps"]["arena"]["platforms"]["windows-x86_64"] = PlatformEntryJson("arena", "1.3.0", '1');
  ParsedManifest p = Parse(author.SignedObject(test_doc.dump()), floors);
  ASSERT_EQ(p.reason, "");
  floors = nevr_maps::AdvanceFloors(floors, kUrl, p);

  const std::vector<PinnedKey> official_only{official.pin};
  const auto as_official = [&](std::uint64_t serial, const char* arena_version) {
    json doc = BaseManifest("echo-community", serial);
    doc["maps"]["arena"]["platforms"]["windows-x86_64"] = PlatformEntryJson("arena", arena_version, '1');
    return nevr_maps::ParseAndVerifyManifest(official.SignedObject(doc.dump()), official_only, floors, "echo-community",
                                             kNow, kDeviceDocumentBytes);
  };
  p = as_official(1, "1.0.0");
  ASSERT_EQ(p.reason, "");
  EXPECT_TRUE(p.dropped.empty());
  EXPECT_EQ(p.untrusted_floor_keys, std::vector<std::string>{kAuthorId});
  floors = nevr_maps::AdvanceFloors(floors, kUrl, p);
  EXPECT_EQ(floors.channels.at("echo-community").at(kGuild).at(kOfficialId).serial, 1u);
  EXPECT_EQ(floors.channels.at("echo-community").at(kGuild).at(kAuthorId).serial, 40u);  // kept, in case the key is trusted again

  p = as_official(2, "1.1.0");
  ASSERT_EQ(p.reason, "");
  floors = nevr_maps::AdvanceFloors(floors, kUrl, p);
  EXPECT_EQ(as_official(1, "1.0.0").reason, "serial_down");
  json reused = BaseManifest("echo-community", 2);
  reused["revoked_ids"] = {"badmap"};
  EXPECT_EQ(nevr_maps::ParseAndVerifyManifest(official.SignedObject(reused.dump()), official_only, floors,
                                              "echo-community", kNow, kDeviceDocumentBytes)
                .reason,
            "serial_reuse");
  p = as_official(3, "1.0.5");
  ASSERT_EQ(p.reason, "");
  ASSERT_EQ(p.dropped.size(), 1u);
  EXPECT_EQ(p.dropped[0], std::make_pair(std::string("arena/windows-x86_64"), std::string("version_down")));

  // Both keys trusted again: the highest floor of the two applies.
  EXPECT_EQ(Parse(official.SignedObject(BaseManifest("echo-community", 39).dump()), floors).reason, "serial_down");
  EXPECT_TRUE(Parse(author.SignedObject(test_doc.dump()), floors).untrusted_floor_keys.empty());
}

TEST_F(MapManifest, VersionDownDropsOnlyThatEntryAndRevocationsStillApply) {
  FloorBook floors;
  floors.channels["echo-community"][kGuild][kAuthorId].serial = 41;
  floors.channels["echo-community"][kGuild][kAuthorId].serial_sha256 = Sha('0');
  floors.channels["echo-community"][kGuild][kAuthorId].version["arena/windows-x86_64"] = "1.3.0";
  floors.channels["echo-community"][kGuild][kAuthorId].version["arena/android-arm64"] = "1.1";
  json doc = BaseManifest();
  doc["revoked_ids"] = {"badmap"};
  doc["revoked_sha256"] = {Sha('7')};
  doc["maps"]["solo"]["platforms"]["windows-x86_64"] = PlatformEntryJson("solo", "0.9.0", '3');
  floors.channels["echo-community"][kGuild][kAuthorId].version["solo/windows-x86_64"] = "1.0.0";

  const ParsedManifest p = ParseBody(doc, floors);
  ASSERT_EQ(p.reason, "");
  ASSERT_EQ(p.dropped.size(), 2u);
  EXPECT_EQ(p.dropped[0], std::make_pair(std::string("arena/windows-x86_64"), std::string("version_down")));
  EXPECT_EQ(p.dropped[1], std::make_pair(std::string("solo/windows-x86_64"), std::string("version_down")));
  const auto& arena = p.manifest.maps.at("arena");
  EXPECT_EQ(arena.platforms.count(nevr_maps::kPlatformPc), 0u);
  EXPECT_EQ(arena.platforms.count(nevr_maps::kPlatformQuest), 1u);  // 1.1.0 equals the floor 1.1
  EXPECT_EQ(p.manifest.maps.count("solo"), 0u);                      // every platform dropped: not listed
  EXPECT_EQ(p.manifest.revoked_ids, std::vector<std::string>{"badmap"});
  EXPECT_EQ(p.manifest.revoked_sha256, std::vector<std::string>{Sha('7')});

  const FloorBook next = nevr_maps::AdvanceFloors(floors, kUrl, p);
  EXPECT_EQ(next.channels.at("echo-community").at(kGuild).at(kAuthorId).version.at("arena/windows-x86_64"), "1.3.0");  // never lowered
  EXPECT_EQ(next.channels.at("echo-community").at(kGuild).at(kAuthorId).serial, 42u);
}

TEST_F(MapManifest, AdvanceFloorsRaisesVersionsAndIgnoresARefusal) {
  FloorBook floors;
  floors.channels["echo-community"][kGuild][kAuthorId].version["arena/windows-x86_64"] = "1.1.9";
  ParsedManifest p = ParseBody(BaseManifest(), floors);
  ASSERT_EQ(p.reason, "");
  const FloorBook next = nevr_maps::AdvanceFloors(floors, kUrl, p);
  EXPECT_EQ(next.channels.at("echo-community").at(kGuild).at(kAuthorId).version.at("arena/windows-x86_64"), "1.2.0");
  EXPECT_EQ(next.channels.at("echo-community").at(kGuild).at(kAuthorId).version.at("arena/android-arm64"), "1.1.0");

  p.reason = "serial_down";
  const FloorBook unchanged = nevr_maps::AdvanceFloors(floors, kUrl, p);
  EXPECT_EQ(unchanged.channels.at("echo-community").at(kGuild).at(kAuthorId).version.at("arena/windows-x86_64"), "1.1.9");
  EXPECT_TRUE(unchanged.bound.empty());
}

TEST(MapVersions, CompareAndValidate) {
  EXPECT_EQ(nevr_maps::CompareVersions("1.2.0", "1.2"), 0);
  EXPECT_LT(nevr_maps::CompareVersions("1.9", "1.10"), 0);
  EXPECT_GT(nevr_maps::CompareVersions("2", "1.99.99"), 0);
  EXPECT_LT(nevr_maps::CompareVersions("bad", "0"), 0);
  EXPECT_GT(nevr_maps::CompareVersions("0", ""), 0);
  EXPECT_EQ(nevr_maps::CompareVersions("x", "y"), 0);
  EXPECT_TRUE(nevr_maps::IsValidVersion("999999999.0.0.1"));
  for (const char* bad : {"", ".", "1.", ".1", "01", "1..2", "1.2.3.4.5", "1000000000", "1.a", "-1", "1 .2"}) {
    EXPECT_FALSE(nevr_maps::IsValidVersion(bad)) << bad;
  }
}

TEST(MapTime, ExpiryFailsClosed) {
  EXPECT_FALSE(nevr_maps::IsExpired("2027-02-09T00:00:00Z", "2027-02-08T23:59:59Z"));
  EXPECT_TRUE(nevr_maps::IsExpired("2027-02-09T00:00:00Z", "2027-02-09T00:00:00Z"));
  EXPECT_TRUE(nevr_maps::IsExpired("2027-02-09T00:00:00Z", "2028-01-01T00:00:00Z"));
  EXPECT_TRUE(nevr_maps::IsExpired("2027-02-09", kNow));
  EXPECT_TRUE(nevr_maps::IsExpired("2027-02-09T00:00:00Z", ""));
  EXPECT_TRUE(nevr_maps::IsValidUtc("2028-02-29T23:59:59Z"));
  for (const char* bad : {"2027-02-29T00:00:00Z", "2100-02-29T00:00:00Z", "2027-13-01T00:00:00Z", "2027-00-01T00:00:00Z",
                          "2027-01-01T24:00:00Z", "2027-01-01T00:60:00Z", "2027-01-01T00:00:60Z", "2027-1-01T00:00:00Z",
                          "2027-01-01t00:00:00z", "2027-01-01T00:00:00Z "}) {
    EXPECT_FALSE(nevr_maps::IsValidUtc(bad)) << bad;
  }
}

TEST(MapNames, IdsAndChannels) {
  EXPECT_TRUE(nevr_maps::IsValidMapId("a"));
  EXPECT_TRUE(nevr_maps::IsValidMapId(std::string(55, 'a')));
  EXPECT_FALSE(nevr_maps::IsValidMapId(std::string(56, 'a')));
  EXPECT_FALSE(nevr_maps::IsValidMapId("1a"));
  EXPECT_FALSE(nevr_maps::IsValidMapId("_a"));
  EXPECT_FALSE(nevr_maps::IsValidMapId("a-b"));
  EXPECT_TRUE(nevr_maps::IsValidChannel("0"));
  EXPECT_TRUE(nevr_maps::IsValidChannel("echo-community.v2"));
  EXPECT_TRUE(nevr_maps::IsValidChannel(std::string(64, 'a')));
  EXPECT_FALSE(nevr_maps::IsValidChannel(std::string(65, 'a')));
  EXPECT_FALSE(nevr_maps::IsValidChannel("-echo"));
  EXPECT_FALSE(nevr_maps::IsValidChannel("echo_community"));
  EXPECT_FALSE(nevr_maps::IsValidChannel(""));
}

// A group id is opaque (map_format.h kGroupIdMaxBytes): only path safety is checked, so a UUID and a snowflake both pass and
// nothing that could leave its path segment or its cache directory does.
TEST(MapNames, GroupIdsAreOpaqueAndPathSafe) {
  EXPECT_TRUE(nevr_maps::IsPathSafeGroupId("0b6e4c1a-7f3d-4e2a-9c5b-8d1e2f3a4b5c"));  // a UUID
  EXPECT_TRUE(nevr_maps::IsPathSafeGroupId("12345678901234567890"));                  // a 20-digit snowflake
  EXPECT_TRUE(nevr_maps::IsPathSafeGroupId("0b6e4c1a7f3d"));                          // no UUID shape is required
  EXPECT_TRUE(nevr_maps::IsPathSafeGroupId(std::string(nevr_maps::kGroupIdMaxBytes, 'a')));
  EXPECT_FALSE(nevr_maps::IsPathSafeGroupId(std::string(nevr_maps::kGroupIdMaxBytes + 1, 'a')));
  EXPECT_FALSE(nevr_maps::IsPathSafeGroupId(""));
  EXPECT_FALSE(nevr_maps::IsPathSafeGroupId("0B6E4C1A-7F3D-4E2A-9C5B-8D1E2F3A4B5C"));
  EXPECT_FALSE(nevr_maps::IsPathSafeGroupId("a/b"));
  EXPECT_FALSE(nevr_maps::IsPathSafeGroupId("a\\b"));
  EXPECT_FALSE(nevr_maps::IsPathSafeGroupId("a.b"));
  EXPECT_FALSE(nevr_maps::IsPathSafeGroupId(".."));
  EXPECT_FALSE(nevr_maps::IsPathSafeGroupId("a_b"));
  EXPECT_FALSE(nevr_maps::IsPathSafeGroupId(std::string("a\0b", 3)));
}

// A signature the minisign tool made (tests/vectors/maps/minisign): a manifest-shaped document signed with a key made for the test.
TEST(Minisign, CrossChecksASignatureTheMinisignToolMade) {
  nevr_minisign::PublicKey key;
  ASSERT_TRUE(nevr_minisign::ParsePublicKeyText(ReadVector("minisign/test_key.pub"), &key));
  const std::string message = ReadVector("minisign/test_manifest.json");
  const std::string sig = ReadVector("minisign/test_manifest.json.minisig");
  std::array<std::uint8_t, nevr_maps::kMinisignKeyIdBytes> key_id{};
  std::string comment;
  ASSERT_EQ(nevr_minisign::Verify(message, sig, {key}, &key_id, &comment), Verdict::kOk);
  EXPECT_EQ(key_id, key.key_id);
  EXPECT_EQ(nevr_maps::KeyIdText(key_id), kVectorKeyId);
  EXPECT_EQ(comment, "nevr map channel test vector, signed with a key made for the test");

  std::string flipped = message;
  flipped[message.size() / 2] ^= 0x20;
  EXPECT_EQ(nevr_minisign::Verify(flipped, sig, {key}, nullptr, nullptr), Verdict::kBadSignature);
  std::string edited = sig;
  edited[edited.find("test vector")] = 'T';
  comment.clear();
  EXPECT_EQ(nevr_minisign::Verify(message, edited, {key}, nullptr, &comment), Verdict::kBadGlobalSignature);
  EXPECT_TRUE(comment.empty());
  EXPECT_EQ(nevr_minisign::Verify(message, sig, {}, nullptr, nullptr), Verdict::kUnknownKey);
  EXPECT_EQ(nevr_minisign::Verify(message, sig.substr(0, sig.find("\ntrusted comment") + 1), {key}, nullptr, nullptr),
            Verdict::kMalformed);
  EXPECT_EQ(nevr_minisign::Verify(message, sig + "extra\n", {key}, nullptr, nullptr), Verdict::kMalformed);
  std::string legacy = sig;
  legacy[legacy.find('\n') + 2] = 'W';  // base64 "RU" decodes to "ED", "RW" to the legacy "Ed"
  EXPECT_EQ(nevr_minisign::Verify(message, legacy, {key}, nullptr, nullptr), Verdict::kUnsupported);
}

// One byte of the tool's signature changed: the signature no longer verifies, whichever byte it is.
TEST(Minisign, AFlippedSignatureByteIsRefused) {
  nevr_minisign::PublicKey key;
  ASSERT_TRUE(nevr_minisign::ParsePublicKeyText(ReadVector("minisign/test_key.pub"), &key));
  const std::string message = ReadVector("minisign/test_manifest.json");
  const std::string sig = ReadVector("minisign/test_manifest.json.minisig");
  const std::size_t line = sig.find('\n') + 1;  // the signature line: base64 of "ED", the key id, the signature
  const std::size_t signature_chars = sig.find('\n', line) - line;
  // Past the algorithm and key id (10 bytes, the first 13 base64 characters) and before the padding.
  for (std::size_t at = line + 16; at < line + signature_chars - 4; at += 7) {
    std::string flipped = sig;
    flipped[at] = flipped[at] == 'A' ? 'B' : 'A';
    EXPECT_EQ(nevr_minisign::Verify(message, flipped, {key}, nullptr, nullptr), Verdict::kBadSignature) << at - line;
  }
}

// The tool's signature and its document, joined as a guild's manifest object, parse end to end.
TEST(Minisign, TheToolsSignedDocumentParsesAsAManifest) {
  nevr_maps::PinnedKey pin;
  ASSERT_TRUE(nevr_minisign::ParsePublicKeyText(ReadVector("minisign/test_key.pub"), &pin.key));
  const std::string object =
      ReadVector("minisign/test_manifest.json.minisig") + "--\n" + ReadVector("minisign/test_manifest.json");
  const ParsedManifest p =
      nevr_maps::ParseAndVerifyManifest(object, {pin}, FloorBook(), "", kNow, kDeviceDocumentBytes);
  ASSERT_EQ(p.reason, "");
  EXPECT_EQ(p.key_id, kVectorKeyId);
  EXPECT_EQ(p.channel, "test-channel");
  EXPECT_EQ(p.guild, kVectorGuild);
  EXPECT_EQ(p.manifest.serial, 7u);
  EXPECT_EQ(p.manifest.maps.at("testmap").platforms.at(nevr_maps::kPlatformPc).level, EchoVR::CSymbol64Hash("testmap"));
}

TEST(Minisign, PublicKeyAndBase64AreStrict) {
  nevr_minisign::PublicKey key;
  const std::string good = kVectorKeyLine;
  EXPECT_TRUE(nevr_minisign::ParsePublicKey(good, &key));
  EXPECT_FALSE(nevr_minisign::ParsePublicKey(good + "\n", &key));
  EXPECT_FALSE(nevr_minisign::ParsePublicKey(" " + good, &key));
  EXPECT_FALSE(nevr_minisign::ParsePublicKey(good.substr(0, 52), &key));
  EXPECT_FALSE(nevr_minisign::ParsePublicKey("RU" + good.substr(2), &key));  // "ED" is a signature algorithm, not a key's
  EXPECT_FALSE(nevr_minisign::ParsePublicKey(good.substr(0, 55) + "=", &key));
  EXPECT_FALSE(nevr_minisign::ParsePublicKey("", &key));
}

TEST(Minisign, PublicKeyTextTakesAWholePubFileOrTheBareLine) {
  const std::string good = kVectorKeyLine;
  nevr_minisign::PublicKey bare, key;
  ASSERT_TRUE(nevr_minisign::ParsePublicKey(good, &bare));
  const auto same = [&] { return key.key_id == bare.key_id && key.pk == bare.pk; };

  EXPECT_TRUE(nevr_minisign::ParsePublicKeyText(good, &key) && same());
  key = {};
  EXPECT_TRUE(nevr_minisign::ParsePublicKeyText("untrusted comment: minisign public key 12CF26D38A5D6FFB\n" + good + "\n",
                                                &key) &&
              same());
  key = {};
  EXPECT_TRUE(nevr_minisign::ParsePublicKeyText("\r\n untrusted comment: minisign public key\r\n  " + good + " \r\n", &key) &&
              same());
  EXPECT_FALSE(nevr_minisign::ParsePublicKeyText("untrusted comment: minisign public key\n", &key));
  EXPECT_FALSE(nevr_minisign::ParsePublicKeyText("", &key));
  EXPECT_FALSE(nevr_minisign::ParsePublicKeyText("not a key\n" + good, &key));
}

TEST(MapConstants, EqualTheSharedConstantsFile) {
  const json c = json::parse(ReadVector("constants.json"));
  EXPECT_EQ(c.at("kStockBytesMinSize").get<std::uint64_t>(), nevr_maps::kStockBytesMinSize);
  EXPECT_EQ(c.at("kStockBlockSize").get<std::uint64_t>(), nevr_maps::kStockBlockSize);
  EXPECT_EQ(c.at("kRecipeOpsMax").get<std::uint64_t>(), nevr_maps::kRecipeOpsMax);
  EXPECT_EQ(c.at("kRecipeInsertMax").get<std::uint64_t>(), nevr_maps::kRecipeInsertMax);
  EXPECT_EQ(c.at("kRecipeMapInsertMax").get<std::uint64_t>(), nevr_maps::kRecipeMapInsertMax);
  EXPECT_EQ(c.at("kRecipeResultMax").get<std::uint64_t>(), nevr_maps::kRecipeResultMax);
  EXPECT_EQ(c.at("kRecipeInsertDivisor").get<std::uint64_t>(), nevr_maps::kRecipeInsertDivisor);
  EXPECT_EQ(c.at("kStockFingerprintPrime").get<std::string>(), "0x100000001b3");
  EXPECT_EQ(c.at("kRecipeFormatVersion").get<int>(), nevr_maps::kRecipeFormatVersion);
}

// ---- The package index.

struct IndexVectors {
  json file;
  IndexVectors() : file(json::parse(ReadVector("package_index_cases.json"))) {}
};

nevr_maps::PlatformEntry EntryFor(const std::string& bytes, const char* level_name = "testmap") {
  nevr_maps::PlatformEntry e;
  e.version = "1.2.0";
  e.level = EchoVR::CSymbol64Hash(level_name);
  e.level_name = level_name;
  e.index.sha256 = nevr_quest::data::Sha256Hex(bytes.data(), bytes.size());
  e.index.size = bytes.size();
  return e;
}

nevr_maps::ParsedPackage ParseIndex(const json& doc, const std::string& platform = nevr_maps::kPlatformPc,
                                    const char* level_name = "testmap") {
  const std::string bytes = doc.dump();
  return nevr_maps::ParsePackageIndex(bytes, "testmap", platform, EntryFor(bytes, level_name), kDeviceDocumentBytes);
}

json BaseIndex() {
  json base = IndexVectors().file.at("base");
  Substitute(base);
  return base;
}

TEST(PackageIndex, EverySharedVectorHasItsReason) {
  const IndexVectors v;
  ASSERT_GE(v.file.at("cases").size(), 80u);
  for (const json& c : v.file.at("cases")) {
    json doc = v.file.at("base").patch(c.at("patch"));
    Substitute(doc);
    const nevr_maps::ParsedPackage p = ParseIndex(doc);
    EXPECT_EQ(p.reason, c.at("reason").get<std::string>()) << c.at("name").get<std::string>();
    if (!p.reason.empty()) {
      EXPECT_TRUE(p.package.members.empty()) << c.at("name").get<std::string>();
    }
  }
}

TEST(PackageIndex, TheBaseParsesInFull) {
  const nevr_maps::ParsedPackage p = ParseIndex(BaseIndex());
  ASSERT_EQ(p.reason, "");
  const nevr_maps::Package& pkg = p.package;
  EXPECT_EQ(pkg.id, "testmap");
  EXPECT_EQ(pkg.level, EchoVR::CSymbol64Hash("testmap"));
  EXPECT_EQ(pkg.parent, 0x3f9915d3001dc28eull);
  EXPECT_EQ(pkg.source_sha256, Sha('b'));
  ASSERT_EQ(pkg.members.size(), 3u);
  EXPECT_EQ(pkg.members[0].role, nevr_maps::Role::kSet);
  EXPECT_EQ(pkg.members[0].pieces.size(), 2u);
  EXPECT_EQ(pkg.members[1].role, nevr_maps::Role::kScript);
  EXPECT_EQ(pkg.members[1].script_hash, 0x00aa11bb22cc33ddull);
  EXPECT_EQ(pkg.members[2].role, nevr_maps::Role::kRecipe);
  ASSERT_EQ(pkg.rekeyed.size(), 1u);
  ASSERT_EQ(pkg.rekeyed[0].edits.size(), 1u);
  EXPECT_EQ(pkg.rekeyed[0].edits[0].to, EchoVR::CSymbol64Hash("testmap"));
  EXPECT_EQ(pkg.rekeyed[0].result_size, 90112u);
  ASSERT_EQ(pkg.derived.size(), 2u);
  EXPECT_EQ(pkg.derived[0].kind, nevr_maps::EditKind::kFields);
  ASSERT_EQ(pkg.derived[0].fields.size(), 2u);
  EXPECT_EQ(pkg.derived[0].fields[1].width, 4);
  EXPECT_EQ(pkg.derived[0].stock_name, 0x21u);
  EXPECT_EQ(pkg.derived[1].kind, nevr_maps::EditKind::kDiff);
  ASSERT_EQ(pkg.derived[1].diff.size(), 3u);
  EXPECT_TRUE(pkg.derived[1].diff[1].insert);
  EXPECT_EQ(pkg.derived[1].diff[2].offset, 8192u);
  EXPECT_EQ(pkg.standins, std::vector<std::uint64_t>{0x40});
  ASSERT_EQ(pkg.config_aliases.size(), 1u);
  EXPECT_EQ(pkg.config_aliases[0].key, EchoVR::CSymbol64Hash("mp_arena_rules"));
  EXPECT_EQ(pkg.config_aliases[0].alias_name, "testmap_arena_rules");
}

TEST(PackageIndex, TheManifestPinIsTheAuthority) {
  const std::string bytes = BaseIndex().dump();
  nevr_maps::PlatformEntry e = EntryFor(bytes);
  e.index.sha256[0] = e.index.sha256[0] == '0' ? '1' : '0';
  EXPECT_EQ(nevr_maps::ParsePackageIndex(bytes, "testmap", nevr_maps::kPlatformPc, e, kDeviceDocumentBytes).reason,
            "index_pin");
  e = EntryFor(bytes);
  e.index.size += 1;
  EXPECT_EQ(nevr_maps::ParsePackageIndex(bytes, "testmap", nevr_maps::kPlatformPc, e, kDeviceDocumentBytes).reason,
            "index_pin");

  const std::string big(nevr_maps::kMaxIndexBytes + 1, ' ');
  EXPECT_EQ(nevr_maps::ParsePackageIndex(big, "testmap", nevr_maps::kPlatformPc, EntryFor(big), kDeviceDocumentBytes).reason,
            "too_large");
  const std::string not_json = "{\"schema\":";
  EXPECT_EQ(
      nevr_maps::ParsePackageIndex(not_json, "testmap", nevr_maps::kPlatformPc, EntryFor(not_json), kDeviceDocumentBytes).reason,
      "malformed");
  const std::string duplicate = bytes.substr(0, bytes.size() - 1) + ",\"id\":\"testmap\"}";
  EXPECT_EQ(
      nevr_maps::ParsePackageIndex(duplicate, "testmap", nevr_maps::kPlatformPc, EntryFor(duplicate), kDeviceDocumentBytes).reason,
      "malformed");

  // The device's share bounds an index below the v1 limit too (ADR 0045 decision 8).
  EXPECT_EQ(nevr_maps::ParsePackageIndex(bytes, "testmap", nevr_maps::kPlatformPc, EntryFor(bytes), bytes.size()).reason, "");
  EXPECT_EQ(nevr_maps::ParsePackageIndex(bytes, "testmap", nevr_maps::kPlatformPc, EntryFor(bytes), bytes.size() - 1).reason,
            "too_large");
  EXPECT_EQ(nevr_maps::ParsePackageIndex(bytes, "testmap", nevr_maps::kPlatformPc, EntryFor(bytes), 0).reason, "no_memory");
}

TEST(PackageIndex, QuestPathsAndLevelName) {
  json doc = BaseIndex();
  doc["platform"] = nevr_maps::kPlatformQuest;
  doc["level_name"] = "testmap_lowspec";
  doc["level"] = Rad("testmap_lowspec");
  doc["members"][0]["path"] = "set/android-arm64.nevrres";
  doc["members"][1]["path"] = "scripts/lib00aa11bb22cc33dd.so";
  doc["members"][2]["path"] = "recipe/android-arm64.bin";
  EXPECT_EQ(ParseIndex(doc, nevr_maps::kPlatformQuest, "testmap_lowspec").reason, "");
  doc["members"][1]["path"] = "scripts/00aa11bb22cc33dd.dll";
  EXPECT_EQ(ParseIndex(doc, nevr_maps::kPlatformQuest, "testmap_lowspec").reason, "bad_path");
}

// A `from` is stock bytes, so an edit that carries one is refused whatever its value, never ignored like other unknown keys.
TEST(PackageIndex, AnEditCarryingFromIsRefused) {
  for (const json& value : {json("0x1"), json(nullptr), json("not hex")}) {
    json doc = BaseIndex();
    doc["derived"][0]["edits"]["ops"][0]["from"] = value;
    EXPECT_EQ(ParseIndex(doc).reason, "recipe_from_forbidden") << value.dump();
    doc = BaseIndex();
    doc["rekeyed"][0]["edits"][0]["from"] = value;
    EXPECT_EQ(ParseIndex(doc).reason, "recipe_from_forbidden") << value.dump();
  }
}

TEST(PackageIndex, OpsCapAndMapInsertCap) {
  json doc = BaseIndex();
  json& fields = doc["derived"][0];
  fields["stock"]["size"] = 1u << 20;
  fields["result"]["size"] = 1u << 20;
  json ops = json::array();
  for (std::uint64_t i = 0; i < nevr_maps::kRecipeOpsMax; ++i) {
    ops.push_back({{"at", i * 2}, {"width", 1}, {"to", "0x1"}});
  }
  fields["edits"]["ops"] = ops;
  EXPECT_EQ(ParseIndex(doc).reason, "");  // 65,536 bytes carried, under (1 MiB / 4)
  fields["edits"]["ops"].push_back({{"at", 1u << 19}, {"width", 1}, {"to", "0x1"}});
  EXPECT_EQ(ParseIndex(doc).reason, "recipe_ops");

  // Seventeen recipes that each carry 4 MiB, the per-recipe cap: 68 MiB is over the 64 MiB per map.
  doc = BaseIndex();
  doc["members"][2]["size"] = 4u << 20;
  doc["members"][2]["pieces"] = {{{"sha256", Sha('1')}, {"size", 4u << 20}}};
  json derived = json::array();
  for (int i = 0; i < 17; ++i) {
    json r = BaseIndex()["derived"][1];
    r["name"] = Hex64(0x100 + i);
    r["stock"]["size"] = 12u << 20;
    r["result"]["size"] = 16u << 20;
    r["edits"]["ops"] = {{{"copy", {0, 12u << 20}}}, {{"insert", {0, 4u << 20}}}};
    derived.push_back(r);
  }
  doc["derived"] = derived;
  EXPECT_EQ(ParseIndex(doc).reason, "recipe_cap");
  doc["derived"].erase(16);
  EXPECT_EQ(ParseIndex(doc).reason, "");  // exactly 64 MiB
}

}  // namespace
