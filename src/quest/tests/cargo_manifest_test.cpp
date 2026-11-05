// Host test for the cargo manifest parser and the fetch plan (src/core/cargo_manifest.*, src/core/cargo_plan.*;
// docs/adr/0045-cdn-texture-delivery.md D2, D5, D6): the one document shape, every refusal with its status and the label and index it
// names (never text from the file), the label states that code and config decide and the manifest cannot, the platform split, the
// size and pairing rules, and the cap. The documents are built with nlohmann::json here; the code under test reads them with its
// non-throwing calls only.
//
// Built and run by `just test-quest-data`.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/cargo_manifest.h"
#include "core/cargo_plan.h"
#include "quest/tests/test_check.h"

namespace {

using json = nlohmann::ordered_json;  // document order matters: the cap is spent in it
namespace nc = nevr_cargo;

constexpr const char* kPoster = "0x3fa6d25bc24a7b01";

std::string Sha(char digit) { return std::string(64, digit); }

json EntryJson(const char* platform, const char* type, const char* name, const std::string& sha, std::uint64_t size,
               std::uint64_t bytes, std::uint64_t stock) {
  return json{{"platform", platform}, {"type", type}, {"name", name}, {"format", "zstd-frame"}, {"sha256", sha},
              {"size", size},         {"bytes", bytes}, {"align", 16},  {"stock_size", stock}};
}

json Doc(std::initializer_list<std::pair<std::string, std::vector<json>>> labels) {
  json l = json::object();
  for (const auto& kv : labels) {
    json entries = json::array();
    for (const json& e : kv.second) entries.push_back(e);
    l[kv.first] = json{{"entries", entries}};
  }
  return json{{"schema", "nevr-cargo-manifest"}, {"schema_version", 1}, {"labels", l}};
}

// The poster's two Quest buffers, as T0 measured them: the descriptor 248 bytes, the pixels 8,388,608.
json Poster(const std::string& shaA, const std::string& shaB, std::uint64_t pixelBytes = 8388608) {
  return Doc({{"lobby_posters",
               {EntryJson("android-arm64", "0xe2efe7289d5985b8", kPoster, shaA, 40, 248, 248),
                EntryJson("android-arm64", "0x489bb35d53ca50e9", kPoster, shaB, 2000, pixelBytes, 8388608)}}});
}

nc::ParseResult Parse(const json& doc, nc::Manifest* m = nullptr) {
  nc::Manifest local;
  return nc::ParseManifest(doc.dump(), m != nullptr ? m : &local);
}

void AValidManifestParsesToExactlyItsFields() {
  nc::Manifest m;
  const nc::ParseResult r = Parse(Poster(Sha('a'), Sha('b')), &m);
  QCHECK(r.status == nc::ParseStatus::kOk);
  QCHECK(m.labels.size() == 1 && m.labels[0] == "lobby_posters");
  QCHECK(m.entries.size() == 2);
  if (m.entries.size() != 2) return;
  const nc::Entry& e = m.entries[1];
  QCHECK(e.label == "lobby_posters" && e.platform == "android-arm64");
  QCHECK(e.type == 0x489bb35d53ca50e9ULL && e.name == 0x3fa6d25bc24a7b01ULL);
  QCHECK(e.sha256 == Sha('b') && e.size == 2000 && e.bytes == 8388608 && e.align == 16 && e.stock_size == 8388608);
  QCHECK(m.unknown_fields == 0);
}

void EveryRequiredKeyIsRequired() {
  for (const char* key : {"platform", "type", "name", "format", "sha256", "size", "bytes", "align", "stock_size"}) {
    json doc = Poster(Sha('a'), Sha('b'));
    doc["labels"]["lobby_posters"]["entries"][1].erase(key);
    const nc::ParseResult r = Parse(doc);
    QCHECK(r.status == nc::ParseStatus::kMissingField);
    QCHECK(r.label == "lobby_posters" && r.index == 1);
  }
  for (const char* key : {"schema", "schema_version", "labels"}) {
    json doc = Poster(Sha('a'), Sha('b'));
    doc.erase(key);
    const nc::ParseStatus status = Parse(doc).status;
    QCHECK(status == nc::ParseStatus::kSchema || status == nc::ParseStatus::kVersion || status == nc::ParseStatus::kMissingField);
  }
}

void TheSchemaAndTheVersionAreChecked() {
  json doc = Poster(Sha('a'), Sha('b'));
  doc["schema"] = "something-else";
  QCHECK(Parse(doc).status == nc::ParseStatus::kSchema);
  doc = Poster(Sha('a'), Sha('b'));
  doc["schema_version"] = 2;
  QCHECK(Parse(doc).status == nc::ParseStatus::kVersion);
  doc["schema_version"] = "1";
  QCHECK(Parse(doc).status == nc::ParseStatus::kVersion);
  nc::Manifest m;
  QCHECK(nc::ParseManifest("not json at all", &m).status == nc::ParseStatus::kNotJson);
  QCHECK(nc::ParseManifest("[1,2]", &m).status == nc::ParseStatus::kNotJson);
  QCHECK(nc::ParseManifest("", &m).status == nc::ParseStatus::kNotJson);
}

void ADocumentOverTheCapIsRefusedBeforeItIsRead() {
  nc::Manifest m;
  const std::string big(nc::kMaxManifestBytes + 1, ' ');
  QCHECK(nc::ParseManifest(big, &m).status == nc::ParseStatus::kTooLarge);
  json doc = Poster(Sha('a'), Sha('b'));
  std::string text = doc.dump();
  text.append(nc::kMaxManifestBytes - text.size(), ' ');  // exactly the cap: still read
  QCHECK(nc::ParseManifest(text, &m).status == nc::ParseStatus::kOk);
}

void AMalformedHashOrNumberIsNamedWithItsLabelAndIndex() {
  struct Case {
    const char* key;
    json value;
    nc::ParseStatus want;
  };
  const Case cases[] = {
      {"sha256", std::string(63, 'a'), nc::ParseStatus::kBadHash},
      {"sha256", std::string(64, 'A'), nc::ParseStatus::kBadHash},
      {"sha256", std::string(64, 'g'), nc::ParseStatus::kBadHash},
      {"sha256", 7, nc::ParseStatus::kBadField},
      {"type", "e2efe7289d5985b8", nc::ParseStatus::kBadField},
      {"type", "0x", nc::ParseStatus::kBadField},
      {"type", "0x00000000000000000", nc::ParseStatus::kBadField},
      {"type", "0xzz", nc::ParseStatus::kBadField},
      {"name", 12, nc::ParseStatus::kBadField},
      {"size", "2000", nc::ParseStatus::kBadField},
      {"size", -1, nc::ParseStatus::kBadField},
      {"size", 0, nc::ParseStatus::kTooBig},
      {"size", nc::kMaxBlobBytes + 1, nc::ParseStatus::kTooBig},
      {"bytes", nc::kMaxBlobBytes + 1, nc::ParseStatus::kTooBig},
      {"bytes", 0, nc::ParseStatus::kTooBig},
      {"align", 0, nc::ParseStatus::kBadField},
      {"align", nc::kMaxAlignment + 1, nc::ParseStatus::kBadField},
      {"format", "raw", nc::ParseStatus::kBadFormat},
      {"format", 3, nc::ParseStatus::kBadField},
      {"platform", "", nc::ParseStatus::kBadField},
      {"platform", "Android ARM", nc::ParseStatus::kBadField},
  };
  for (const Case& c : cases) {
    json doc = Poster(Sha('a'), Sha('b'));
    doc["labels"]["lobby_posters"]["entries"][1][c.key] = c.value;
    const nc::ParseResult r = Parse(doc);
    if (r.status != c.want) std::fprintf(stderr, "case %s: got %s\n", c.key, nc::ParseStatusName(r.status));
    QCHECK(r.status == c.want);
    QCHECK(r.label == "lobby_posters" && r.index == 1);
  }
}

void ThePlainMaximaAreAccepted() {
  json doc = Poster(Sha('a'), Sha('b'));
  doc["labels"]["lobby_posters"]["entries"][1]["size"] = nc::kMaxBlobBytes;
  doc["labels"]["lobby_posters"]["entries"][1]["bytes"] = nc::kMaxBlobBytes;
  doc["labels"]["lobby_posters"]["entries"][1]["align"] = nc::kMaxAlignment;
  QCHECK(Parse(doc).status == nc::ParseStatus::kOk);
}

void ADuplicateKeyIsRefusedWithinAndAcrossLabels() {
  json doc = Doc({{"lobby_posters",
                   {EntryJson("android-arm64", "0x489bb35d53ca50e9", kPoster, Sha('a'), 10, 10, 10),
                    EntryJson("android-arm64", "0x489bb35d53ca50e9", kPoster, Sha('b'), 10, 10, 10)}}});
  nc::ParseResult r = Parse(doc);
  QCHECK(r.status == nc::ParseStatus::kDuplicate && r.label == "lobby_posters" && r.index == 1);
  doc = Doc({{"lobby_posters", {EntryJson("android-arm64", "0x489bb35d53ca50e9", kPoster, Sha('a'), 10, 10, 10)}},
             {"other", {EntryJson("android-arm64", "0x489bb35d53ca50e9", kPoster, Sha('b'), 10, 10, 10)}}});
  r = Parse(doc);
  QCHECK(r.status == nc::ParseStatus::kDuplicate && r.label == "other" && r.index == 0);
  // The same (type, name) on another platform is not a duplicate.
  doc = Doc({{"lobby_posters", {EntryJson("android-arm64", "0x489bb35d53ca50e9", kPoster, Sha('a'), 10, 10, 10),
                                EntryJson("windows-x86_64", "0x489bb35d53ca50e9", kPoster, Sha('b'), 10, 10, 10)}}});
  QCHECK(Parse(doc).status == nc::ParseStatus::kOk);
}

void ALabelNameThatIsNotPlainIsRefusedAndNeverEchoed() {
  json doc = Poster(Sha('a'), Sha('b'));
  doc["labels"]["Bad Label \"quote\""] = doc["labels"]["lobby_posters"];
  doc["labels"].erase("lobby_posters");
  const nc::ParseResult r = Parse(doc);
  QCHECK(r.status == nc::ParseStatus::kBadField);
  QCHECK(r.label.empty());
}

void UnknownKeysAreIgnoredCountedAndSetNothing() {
  json doc = Poster(Sha('a'), Sha('b'));
  doc["labels"]["lobby_posters"]["state"] = "required";     // a manifest trying to set a state
  doc["labels"]["lobby_posters"]["entries"][0]["mystery"] = 1;
  doc["something_new"] = true;
  nc::Manifest m;
  QCHECK(Parse(doc, &m).status == nc::ParseStatus::kOk);
  QCHECK(m.unknown_fields == 3);
  nc::Policy none;
  QCHECK(nc::StateOf("lobby_posters", none) == nc::LabelState::kOn);  // the code default, not the manifest's "required"
}

void TooManyEntriesOrLabelsAreRefused() {
  std::vector<json> entries;
  for (std::uint64_t i = 0; i < nc::kMaxEntries + 1; ++i) {
    char name[24];
    std::snprintf(name, sizeof(name), "0x%llx", static_cast<unsigned long long>(i + 1));
    entries.push_back(EntryJson("android-arm64", "0x489bb35d53ca50e9", name, Sha('a'), 10, 10, 10));
  }
  QCHECK(Parse(Doc({{"lobby_posters", entries}})).status == nc::ParseStatus::kTooMany);
  json l = json::object();
  for (std::size_t i = 0; i < nc::kMaxLabels + 1; ++i) l["l" + std::to_string(i)] = json{{"entries", json::array()}};
  json doc = json{{"schema", "nevr-cargo-manifest"}, {"schema_version", 1}, {"labels", l}};
  QCHECK(Parse(doc).status == nc::ParseStatus::kTooMany);
}

void TheHexHelpersTakeOnlyTheirShape() {
  std::uint64_t v = 0;
  QCHECK(nc::ParseHex64("0xffffffffffffffff", &v) && v == ~std::uint64_t{0});
  QCHECK(nc::ParseHex64("0x0", &v) && v == 0);
  QCHECK(!nc::ParseHex64("0X1", &v));
  QCHECK(!nc::ParseHex64("1", &v));
  QCHECK(!nc::ParseHex64("0x1 ", &v));
  QCHECK(nc::IsSha256Hex(Sha('0')) && nc::IsSha256Hex(Sha('f')));
  QCHECK(!nc::IsSha256Hex(Sha('F')) && !nc::IsSha256Hex("") && !nc::IsSha256Hex(Sha('a') + "a"));
}

// ---- the plan -------------------------------------------------------------------------------------------------------------

nc::Manifest Manifested(const json& doc) {
  nc::Manifest m;
  QCHECK(Parse(doc, &m).status == nc::ParseStatus::kOk);
  return m;
}

const std::function<bool(const std::string&)> kNothingCached = [](const std::string&) { return false; };

void NoConfigMeansLobbyPostersOnAndAnyOtherLabelOff() {
  nc::Policy none;
  QCHECK(nc::StateOf("lobby_posters", none) == nc::LabelState::kOn);
  QCHECK(nc::StateOf("brand_new_label", none) == nc::LabelState::kOff);
  QCHECK(nc::kUnlistedLabelState == nc::LabelState::kOff);
  const nc::Manifest m = Manifested(Doc({{"lobby_posters", {EntryJson("android-arm64", "0x489bb35d53ca50e9", kPoster, Sha('a'), 10, 10, 10)}},
                                         {"brand_new_label", {EntryJson("android-arm64", "0x489bb35d53ca50e9", "0x1", Sha('b'), 10, 10, 10)}}}));
  const nc::Plan p = nc::MakePlan(m, nc::kAndroidArm64, none, kNothingCached);
  QCHECK(p.labels.size() == 2);
  if (p.labels.size() != 2) return;
  QCHECK(p.labels[0].label == "lobby_posters" && p.labels[0].skipped == nc::SkipReason::kNone && p.labels[0].items.size() == 1);
  QCHECK(p.labels[1].label == "brand_new_label" && p.labels[1].skipped == nc::SkipReason::kOff && p.labels[1].items.empty());
}

void ConfigOnlyChangesAStateAndRequiredGoesFirst() {
  const nc::Manifest m = Manifested(Doc({{"a_label", {EntryJson("android-arm64", "0x489bb35d53ca50e9", "0x1", Sha('a'), 10, 10, 10)}},
                                         {"lobby_posters", {EntryJson("android-arm64", "0x489bb35d53ca50e9", kPoster, Sha('b'), 10, 10, 10)}},
                                         {"z_label", {EntryJson("android-arm64", "0x489bb35d53ca50e9", "0x2", Sha('c'), 10, 10, 10)}}}));
  nc::Policy p;
  p.overrides["lobby_posters"] = nc::LabelState::kOff;
  p.overrides["a_label"] = nc::LabelState::kOn;
  p.overrides["z_label"] = nc::LabelState::kRequired;
  const nc::Plan plan = nc::MakePlan(m, nc::kAndroidArm64, p, kNothingCached);
  QCHECK(plan.labels.size() == 3);
  if (plan.labels.size() != 3) return;
  QCHECK(plan.labels[0].label == "z_label" && plan.labels[0].state == nc::LabelState::kRequired);  // required first
  QCHECK(plan.labels[1].label == "a_label" && plan.labels[1].state == nc::LabelState::kOn);
  QCHECK(plan.labels[2].label == "lobby_posters" && plan.labels[2].skipped == nc::SkipReason::kOff && plan.labels[2].items.empty());
  // required_only keeps the required label alone.
  p.required_only = true;
  const nc::Plan only = nc::MakePlan(m, nc::kAndroidArm64, p, kNothingCached);
  QCHECK(only.labels[0].skipped == nc::SkipReason::kNone && only.labels[0].items.size() == 1);
  QCHECK(only.labels[1].skipped == nc::SkipReason::kRequiredOnly);
  QCHECK(only.labels[2].skipped == nc::SkipReason::kOff);  // off outranks the bandwidth switch in the report
}

void OnlyThisPlatformsEntriesArePlanned() {
  const nc::Manifest m = Manifested(Doc({{"lobby_posters", {EntryJson("android-arm64", "0x489bb35d53ca50e9", kPoster, Sha('a'), 10, 10, 10),
                                                            EntryJson("windows-x86_64", "0xbeac1969cb7b8861", kPoster, Sha('b'), 10, 10, 10)}}}));
  const nc::Plan q = nc::MakePlan(m, nc::kAndroidArm64, nc::Policy(), kNothingCached);
  QCHECK(q.platform_entries == 1 && q.labels[0].items.size() == 1 && q.labels[0].items[0].entry.platform == "android-arm64");
  const nc::Plan w = nc::MakePlan(m, nc::kWindowsX64, nc::Policy(), kNothingCached);
  QCHECK(w.platform_entries == 1 && w.labels[0].items.size() == 1 && w.labels[0].items[0].entry.platform == "windows-x86_64");
  // A label with none of this platform's entries is reported as such.
  const nc::Manifest none = Manifested(Doc({{"lobby_posters", {EntryJson("windows-x86_64", "0xbeac1969cb7b8861", kPoster, Sha('b'), 10, 10, 10)}}}));
  const nc::Plan q2 = nc::MakePlan(none, nc::kAndroidArm64, nc::Policy(), kNothingCached);
  QCHECK(q2.platform_entries == 0 && q2.labels[0].skipped == nc::SkipReason::kNoEntries);
}

void CachedBlobsAreCountedOnceAndNotFetched() {
  // Two entries share one blob; a third is held.
  const nc::Manifest m = Manifested(Doc({{"lobby_posters", {EntryJson("android-arm64", "0x489bb35d53ca50e9", "0x1", Sha('a'), 100, 10, 10),
                                                            EntryJson("android-arm64", "0x489bb35d53ca50e9", "0x2", Sha('a'), 100, 10, 10),
                                                            EntryJson("android-arm64", "0x489bb35d53ca50e9", "0x3", Sha('b'), 50, 10, 10),
                                                            EntryJson("android-arm64", "0x489bb35d53ca50e9", "0x4", Sha('c'), 7, 10, 10)}}}));
  const auto held = [](const std::string& sha) { return sha == std::string(64, 'c'); };
  const nc::Plan p = nc::MakePlan(m, nc::kAndroidArm64, nc::Policy(), held);
  const nc::PlanLabel& l = p.labels[0];
  QCHECK(l.entries == 4 && l.cached == 1 && l.to_fetch == 2 && l.fetch_bytes == 150);
  QCHECK(l.items[3].cached && !l.items[0].cached);
}

void APayloadOfAnotherSizeNeedsItsDescriptor() {
  // buffer 1 at the stock size needs no descriptor; at another size it does.
  const std::string pixels = "0x489bb35d53ca50e9", descriptor = "0xe2efe7289d5985b8";
  nc::Manifest same = Manifested(Doc({{"lobby_posters", {EntryJson("android-arm64", pixels.c_str(), kPoster, Sha('a'), 10, 8388608, 8388608)}}}));
  nc::Plan p = nc::MakePlan(same, nc::kAndroidArm64, nc::Policy(), kNothingCached);
  QCHECK(p.labels[0].items.size() == 1 && p.labels[0].refused.empty());

  nc::Manifest other = Manifested(Doc({{"lobby_posters",
      {EntryJson("android-arm64", pixels.c_str(), kPoster, Sha('a'), 10, 5592572, 8388608),
       EntryJson("android-arm64", pixels.c_str(), "0x77", Sha('b'), 10, 100, 100)}}}));
  p = nc::MakePlan(other, nc::kAndroidArm64, nc::Policy(), kNothingCached);
  QCHECK(p.labels[0].items.size() == 1);  // only the other name survives
  QCHECK(p.labels[0].items[0].entry.name == 0x77);
  QCHECK(p.labels[0].refused.size() == 1 && p.labels[0].refused[0].name == 0x3fa6d25bc24a7b01ULL);
  QCHECK(std::string(p.labels[0].refused[0].reason) == "unpaired_payload");
  QCHECK(p.labels[0].entries == 1);
  QCHECK(p.labels[0].to_fetch == 1 && p.labels[0].fetch_bytes == 10);

  nc::Manifest paired = Manifested(Doc({{"lobby_posters",
      {EntryJson("android-arm64", pixels.c_str(), kPoster, Sha('a'), 10, 5592572, 8388608),
       EntryJson("android-arm64", descriptor.c_str(), kPoster, Sha('b'), 10, 256, 248)}}}));
  p = nc::MakePlan(paired, nc::kAndroidArm64, nc::Policy(), kNothingCached);
  QCHECK(p.labels[0].items.size() == 2 && p.labels[0].refused.empty());

  // The descriptor must be in the same label.
  nc::Manifest otherLabel = Manifested(Doc({{"lobby_posters", {EntryJson("android-arm64", pixels.c_str(), kPoster, Sha('a'), 10, 5592572, 8388608)}},
                                            {"extra", {EntryJson("android-arm64", descriptor.c_str(), kPoster, Sha('b'), 10, 256, 248)}}}));
  nc::Policy on;
  on.overrides["extra"] = nc::LabelState::kOn;
  p = nc::MakePlan(otherLabel, nc::kAndroidArm64, on, kNothingCached);
  QCHECK(p.labels[0].items.empty() && p.labels[0].refused.size() == 1);
}

void ALabelThatDoesNotFitTheCapIsSkippedWholeAndTheNextOneStillFits() {
  std::vector<json> big;
  for (int i = 0; i < 5; ++i) {  // 5 x 128 MiB = 640 MiB, over the 512 MiB cap
    char name[16];
    std::snprintf(name, sizeof(name), "0x%d", i + 1);
    big.push_back(EntryJson("android-arm64", "0x489bb35d53ca50e9", name, Sha(static_cast<char>('0' + i)), nc::kMaxBlobBytes, 10, 10));
  }
  const nc::Manifest m = Manifested(Doc({{"huge", big},
                                         {"lobby_posters", {EntryJson("android-arm64", "0x489bb35d53ca50e9", kPoster, Sha('a'), 100, 10, 10)}}}));
  nc::Policy p;
  p.overrides["huge"] = nc::LabelState::kOn;
  const nc::Plan plan = nc::MakePlan(m, nc::kAndroidArm64, p, kNothingCached);
  QCHECK(plan.labels[0].label == "huge" && plan.labels[0].skipped == nc::SkipReason::kOverCap && plan.labels[0].items.empty());
  QCHECK(plan.labels[1].label == "lobby_posters" && plan.labels[1].skipped == nc::SkipReason::kNone);
  // Exactly the cap fits.
  std::vector<json> exact;
  for (int i = 0; i < 4; ++i) {
    char name[16];
    std::snprintf(name, sizeof(name), "0x%d", i + 1);
    exact.push_back(EntryJson("android-arm64", "0x489bb35d53ca50e9", name, Sha(static_cast<char>('0' + i)), nc::kMaxBlobBytes, 10, 10));
  }
  const nc::Manifest m2 = Manifested(Doc({{"huge", exact}}));
  const nc::Plan p2 = nc::MakePlan(m2, nc::kAndroidArm64, p, kNothingCached);
  QCHECK(p2.labels[0].skipped == nc::SkipReason::kNone && p2.labels[0].items.size() == 4);
}

void TheStateAndReasonNamesRoundTrip() {
  nc::LabelState s = nc::LabelState::kOff;
  QCHECK(nc::ParseLabelState("on", &s) && s == nc::LabelState::kOn);
  QCHECK(nc::ParseLabelState("off", &s) && s == nc::LabelState::kOff);
  QCHECK(nc::ParseLabelState("required", &s) && s == nc::LabelState::kRequired);
  QCHECK(!nc::ParseLabelState("On", &s) && !nc::ParseLabelState("yes", &s) && !nc::ParseLabelState("", &s));
  QCHECK(std::string(nc::LabelStateName(nc::LabelState::kRequired)) == "required");
  QCHECK(std::string(nc::SkipReasonName(nc::SkipReason::kOverCap)) == "over_cap");
}

}  // namespace

int main() {
  AValidManifestParsesToExactlyItsFields();
  EveryRequiredKeyIsRequired();
  TheSchemaAndTheVersionAreChecked();
  ADocumentOverTheCapIsRefusedBeforeItIsRead();
  AMalformedHashOrNumberIsNamedWithItsLabelAndIndex();
  ThePlainMaximaAreAccepted();
  ADuplicateKeyIsRefusedWithinAndAcrossLabels();
  ALabelNameThatIsNotPlainIsRefusedAndNeverEchoed();
  UnknownKeysAreIgnoredCountedAndSetNothing();
  TooManyEntriesOrLabelsAreRefused();
  TheHexHelpersTakeOnlyTheirShape();
  NoConfigMeansLobbyPostersOnAndAnyOtherLabelOff();
  ConfigOnlyChangesAStateAndRequiredGoesFirst();
  OnlyThisPlatformsEntriesArePlanned();
  CachedBlobsAreCountedOnceAndNotFetched();
  APayloadOfAnotherSizeNeedsItsDescriptor();
  ALabelThatDoesNotFitTheCapIsSkippedWholeAndTheNextOneStillFits();
  TheStateAndReasonNamesRoundTrip();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "cargo_manifest_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("cargo_manifest_test: all checks passed\n");
  return 0;
}
