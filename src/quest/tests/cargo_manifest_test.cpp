// Host test for the cargo manifest parser and the plan (src/core/cargo_manifest.*, cargo_plan.*; docs/adr/0045-cdn-texture-delivery.md):
// any on-disk type, no fixed caps, the validator's refusals listed by rule, the `empty` list, and the plan's budgets from the device.
//
// Built and run by `just test-quest-data`.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/cargo_manifest.h"
#include "core/cargo_plan.h"
#include "quest/tests/cargo_fake_env.h"
#include "quest/tests/cargo_zstd_support.h"
#include "quest/tests/test_check.h"

namespace {

namespace nc = nevr_cargo;
using json = nlohmann::ordered_json;

constexpr const char* kArchiveType = "0x2a41cf1c1d9e5d32";  // CArchiveResource: not a texture
constexpr const char* kDescType = "0xe2efe7289d5985b8";
constexpr const char* kPixType = "0x489bb35d53ca50e9";

nc::Limits Roomy() {
  nc::Limits l;
  l.ok = true;
  l.manifest_text_bytes = std::uint64_t{64} << 20;
  l.max_entries = 1000000;
  l.max_object_bytes = std::uint64_t{1} << 30;
  l.store_file_bytes = nc::kFormatMaxStoreBytes;
  l.max_store_files = 8;
  l.storage_budget = std::uint64_t{50} << 30;
  l.index_bytes = nc::kIndexBytesFor(1000000);
  return l;
}

std::string Sha(char c) { return std::string(64, c); }

json Entry(const char* type, const char* name, char sha, std::uint64_t size = 100, std::uint64_t bytes = 200, std::uint64_t align = 16) {
  return json{{"platform", "android-arm64"}, {"type", type}, {"name", name}, {"format", "zstd-frame"},
              {"sha256", Sha(sha)},          {"size", size}, {"bytes", bytes}, {"align", align}};
}

std::string Doc(const json& entries, const json& empty = json::array(), const char* label = "game") {
  json labels = json::object();
  labels[label] = json{{"entries", entries}};
  return json{{"schema", "nevr-cargo-manifest"}, {"schema_version", 1}, {"labels", labels}, {"empty", empty}}.dump();
}

nc::ParseResult Parse(const std::string& text, nc::Manifest* m, const nc::Limits& limits = Roomy()) {
  return nc::ParseManifest(text, limits, m);
}

void ANonTextureEntryNeedsNoStockSize() {
  nc::Manifest m;
  const nc::ParseResult r = Parse(Doc(json::array({Entry(kArchiveType, "0x1", 'a'), Entry(kDescType, "0x2", 'b')})), &m);
  QCHECK(r.status == nc::ParseStatus::kOk);
  QCHECK(m.entries.size() == 2);
  QCHECK(!m.entries[0].has_stock_size && !m.entries[1].has_stock_size);
  QCHECK(m.entries[0].type == 0x2a41cf1c1d9e5d32ULL);
  json e = Entry(kPixType, "0x3", 'c');
  e["stock_size"] = 8388608;
  QCHECK(Parse(Doc(json::array({e})), &m).status == nc::ParseStatus::kOk);
  QCHECK(m.entries.size() == 1 && m.entries[0].has_stock_size && m.entries[0].stock_size == 8388608);
}

void ThereIsNoEntryCapOfTwoThousandAndFortyEight() {
  json entries = json::array();
  for (int i = 0; i < 5000; ++i) entries.push_back(Entry(kArchiveType, ("0x" + std::to_string(i + 1)).c_str(), 'a' + (i % 6)));
  nc::Manifest m;
  QCHECK(Parse(Doc(entries), &m).status == nc::ParseStatus::kOk);
  QCHECK(m.entries.size() == 5000);
}

void TheEntryCountAndTheTextAreHeldToTheComputedLimits() {
  json entries = json::array();
  for (int i = 0; i < 10; ++i) entries.push_back(Entry(kArchiveType, ("0x" + std::to_string(i + 1)).c_str(), 'a'));
  const std::string text = Doc(entries);
  nc::Limits l = Roomy();
  nc::Manifest m;
  l.max_entries = 10;
  QCHECK(Parse(text, &m, l).status == nc::ParseStatus::kOk);
  l.max_entries = 9;
  nc::ParseResult r = Parse(text, &m, l);
  QCHECK(r.status == nc::ParseStatus::kTooMany && r.label == "game" && r.index == 9);
  l = Roomy();
  l.manifest_text_bytes = text.size();
  QCHECK(Parse(text, &m, l).status == nc::ParseStatus::kOk);
  l.manifest_text_bytes = text.size() - 1;
  QCHECK(Parse(text, &m, l).status == nc::ParseStatus::kTooLarge);
  l = Roomy();
  l.ok = false;
  QCHECK(Parse(text, &m, l).status == nc::ParseStatus::kNoLimits);
  QCHECK(m.entries.empty());
}

void ARecordTheValidatorRefusesIsLeftOutAndListedByRule() {
  json entries = json::array();
  entries.push_back(Entry(kArchiveType, "0x1", 'a'));                  // 0 good
  entries.push_back(Entry(kArchiveType, "0x2", 'b', 100, 200, 3));      // 1 alignment
  entries.push_back(Entry(kArchiveType, "0x3", 'c', 100, 0));           // 2 zero bytes
  entries.push_back(Entry(kArchiveType, "0x4", 'd', 0, 200));           // 3 zero size
  entries.push_back(Entry(kArchiveType, "0x5", 'e', 100, std::uint64_t{5} << 30));  // 4 over the engine's word
  entries.push_back(Entry(kArchiveType, "0x6", 'f'));                  // 5 good
  nc::Manifest m;
  const nc::ParseResult r = Parse(Doc(entries), &m);
  QCHECK(r.status == nc::ParseStatus::kOk);  // one bad record does not refuse the manifest
  QCHECK(m.entries.size() == 2);
  QCHECK(m.refused.size() == 4);
  QCHECK(m.refused[0].index == 1 && m.refused[0].rule == nc::Rule::kAlignment && m.refused[0].label == "game" && m.refused[0].name == 2);
  QCHECK(m.refused[1].index == 2 && m.refused[1].rule == nc::Rule::kZeroBytes);
  QCHECK(m.refused[2].index == 3 && m.refused[2].rule == nc::Rule::kZeroBytes);
  QCHECK(m.refused[3].index == 4 && m.refused[3].rule == nc::Rule::kSizeRange);
  nc::Limits small = Roomy();
  small.max_object_bytes = 250;
  nc::Manifest m2;
  QCHECK(Parse(Doc(json::array({Entry(kArchiveType, "0x1", 'a'), Entry(kArchiveType, "0x2", 'b', 100, 251)})), &m2, small).status ==
         nc::ParseStatus::kOk);
  QCHECK(m2.entries.size() == 1 && m2.refused.size() == 1 && m2.refused[0].rule == nc::Rule::kObjectTooLarge);
}

void BothEntriesOfADuplicateKeyAreRefused() {
  json entries = json::array();
  entries.push_back(Entry(kArchiveType, "0x1", 'a'));
  entries.push_back(Entry(kArchiveType, "0x2", 'b'));
  entries.push_back(Entry(kArchiveType, "0x1", 'c'));  // the same (platform, type, name) as entry 0, other content
  nc::Manifest m;
  QCHECK(Parse(Doc(entries), &m).status == nc::ParseStatus::kOk);
  QCHECK(m.entries.size() == 1 && m.entries[0].name == 2);
  QCHECK(m.refused.size() == 2);
  QCHECK(m.refused[0].rule == nc::Rule::kDuplicateKey && m.refused[0].index == 0);
  QCHECK(m.refused[1].rule == nc::Rule::kDuplicateKey && m.refused[1].index == 2);
}

void AKeySharedByTwoLabelsIsADuplicateToo() {
  json labels = json::object();
  labels["a"] = json{{"entries", json::array({Entry(kArchiveType, "0x1", 'a')})}};
  labels["b"] = json{{"entries", json::array({Entry(kArchiveType, "0x1", 'b')})}};
  const std::string text = json{{"schema", "nevr-cargo-manifest"}, {"schema_version", 1}, {"labels", labels}}.dump();
  nc::Manifest m;
  QCHECK(Parse(text, &m).status == nc::ParseStatus::kOk);
  QCHECK(m.entries.empty() && m.refused.size() == 2);
  QCHECK(m.refused[0].label == "a" && m.refused[1].label == "b");
}

void TheEmptyListIsReadAndItsRecordsAreChecked() {
  json empty = json::array();
  empty.push_back(json{{"type", kArchiveType}, {"name", "0x10"}, {"align", 4}});
  empty.push_back(json{{"type", kArchiveType}, {"name", "0x11"}, {"align", 3}});    // alignment
  empty.push_back(json{{"type", kArchiveType}, {"name", "0x1"}, {"align", 4}});     // the key of an entry
  nc::Manifest m;
  QCHECK(Parse(Doc(json::array({Entry(kArchiveType, "0x1", 'a')}), empty), &m).status == nc::ParseStatus::kOk);
  QCHECK(m.empties.size() == 1 && m.empties[0].name == 0x10 && m.empties[0].align == 4 && m.empties[0].type == 0x2a41cf1c1d9e5d32ULL);
  QCHECK(m.refused.size() == 3);  // the bad alignment, and both holders of the duplicated key
  QCHECK(m.entries.empty());
}

void ADocumentWithNoEmptyListParses() {
  json labels = json::object();
  labels["game"] = json{{"entries", json::array({Entry(kArchiveType, "0x1", 'a')})}};
  nc::Manifest m;
  QCHECK(Parse(json{{"schema", "nevr-cargo-manifest"}, {"schema_version", 1}, {"labels", labels}}.dump(), &m).status == nc::ParseStatus::kOk);
  QCHECK(m.empties.empty());
}

void AShapeThatIsNotTheSchemaIsRefusedWhole() {
  nc::Manifest m;
  QCHECK(Parse("not json", &m).status == nc::ParseStatus::kNotJson);
  QCHECK(Parse("[]", &m).status == nc::ParseStatus::kNotJson);
  QCHECK(Parse(R"({"schema":"other","schema_version":1,"labels":{}})", &m).status == nc::ParseStatus::kSchema);
  QCHECK(Parse(R"({"schema":"nevr-cargo-manifest","schema_version":2,"labels":{}})", &m).status == nc::ParseStatus::kVersion);
  QCHECK(Parse(R"({"schema":"nevr-cargo-manifest","schema_version":1})", &m).status == nc::ParseStatus::kMissingField);
  json e = Entry(kArchiveType, "0x1", 'a');
  e["sha256"] = "XYZ";
  nc::ParseResult r = Parse(Doc(json::array({Entry(kArchiveType, "0x2", 'a'), e})), &m);
  QCHECK(r.status == nc::ParseStatus::kBadHash && r.label == "game" && r.index == 1);
  e = Entry(kArchiveType, "0x1", 'a');
  e["format"] = "raw";
  QCHECK(Parse(Doc(json::array({e})), &m).status == nc::ParseStatus::kBadFormat);
  e = Entry(kArchiveType, "0x1", 'a');
  e["size"] = "100";
  QCHECK(Parse(Doc(json::array({e})), &m).status == nc::ParseStatus::kBadField);
  e = Entry(kArchiveType, "0x1", 'a');
  e.erase("align");
  QCHECK(Parse(Doc(json::array({e})), &m).status == nc::ParseStatus::kMissingField);
  e = Entry("12345", "0x1", 'a');  // a type that is not a 0x string
  QCHECK(Parse(Doc(json::array({e})), &m).status == nc::ParseStatus::kBadField);
  QCHECK(m.entries.empty());  // a refused manifest leaves nothing behind
}

void AnUnknownKeyIsCountedNeverNamed() {
  json e = Entry(kArchiveType, "0x1", 'a');
  e["note"] = "hello";
  json labels = json::object();
  labels["game"] = json{{"entries", json::array({e})}, {"state", "on"}};
  const std::string text = json{{"schema", "nevr-cargo-manifest"}, {"schema_version", 1}, {"labels", labels}, {"sets", json::array()}}.dump();
  nc::Manifest m;
  QCHECK(Parse(text, &m).status == nc::ParseStatus::kOk);
  QCHECK(m.unknown_fields == 3);  // the entry's note, the label's state, the document's sets
}

void TheEntryMemoryCostIsWhatTheRunHolds() {
  QCHECK(nc::ManifestEntryMemoryBytes() >= sizeof(nc::Entry));
}

// ── the manifest as published: one zstd frame ────────────────────────────────────────────────────────────────────────────
nc::ParseResult ParseFrame(const std::string& frame, nc::Manifest* m, const nc::Limits& limits = Roomy()) {
  return nc::ParseManifestFrame(frame, limits, m);
}

void AManifestFrameDecodesAndParsesLikeItsText() {
  json entries = json::array();
  for (int i = 0; i < 300; ++i) entries.push_back(Entry(kArchiveType, ("0x" + std::to_string(i + 1)).c_str(), 'a' + (i % 6), 100 + i));
  const std::string text = Doc(entries);
  const std::string frame = cargo_test::ZstdFrameOf(text);
  QCHECK(frame.size() < text.size() / 3);  // it is the compressed form that is published
  nc::Manifest a, b;
  QCHECK(Parse(text, &a).status == nc::ParseStatus::kOk);
  QCHECK(ParseFrame(frame, &b).status == nc::ParseStatus::kOk);
  QCHECK(a.entries.size() == 300 && b.entries.size() == 300 && a.entries[299].name == b.entries[299].name && a.entries[299].size == b.entries[299].size);
}

void PlainJsonIsNotAManifest() {
  nc::Manifest m;
  const std::string text = Doc(json::array({Entry(kArchiveType, "0x1", 'a')}));
  QCHECK(ParseFrame(text, &m).status == nc::ParseStatus::kNotZstd);
  QCHECK(ParseFrame("", &m).status == nc::ParseStatus::kNotZstd);
  QCHECK(ParseFrame("abc", &m).status == nc::ParseStatus::kNotZstd);
  QCHECK(std::string(nc::ParseStatusName(nc::ParseStatus::kNotZstd)) == "not_zstd");
  QCHECK(m.entries.empty());
}

void AFrameThatStatesNoContentSizeIsRefused() {
  nc::Manifest m;
  const std::string text = Doc(json::array({Entry(kArchiveType, "0x1", 'a')}));
  QCHECK(ParseFrame(cargo_test::ZstdStreamFrameOf(text), &m).status == nc::ParseStatus::kNoContentSize);
  QCHECK(std::string(nc::ParseStatusName(nc::ParseStatus::kNoContentSize)) == "no_content_size");
}

void TheDeclaredSizeIsHeldToTheLimitBeforeAnythingIsDecoded() {
  nc::Manifest m;
  const std::string text = Doc(json::array({Entry(kArchiveType, "0x1", 'a')}));
  const std::string frame = cargo_test::ZstdFrameOf(text);
  nc::Limits l = Roomy();
  l.manifest_text_bytes = text.size();
  QCHECK(ParseFrame(frame, &m, l).status == nc::ParseStatus::kOk);
  l.manifest_text_bytes = text.size() - 1;
  QCHECK(ParseFrame(frame, &m, l).status == nc::ParseStatus::kTooLarge);
  // A header that declares three gigabytes over a body that is not a frame at all: refused for its size, never decoded.
  const std::string lying = cargo_test::MakeFrame(3000000000ull, 64);
  l = Roomy();
  QCHECK(ParseFrame(lying, &m, l).status == nc::ParseStatus::kTooLarge);
  l.ok = false;
  QCHECK(ParseFrame(frame, &m, l).status == nc::ParseStatus::kNoLimits);
}

void AFrameTheDecoderRefusesOrThatDecodesToAnotherSizeIsRefused() {
  nc::Manifest m;
  json entries = json::array();
  for (int i = 0; i < 100; ++i) entries.push_back(Entry(kArchiveType, ("0x" + std::to_string(i + 1)).c_str(), 'a'));
  const std::string frame = cargo_test::ZstdFrameOf(Doc(entries));
  std::string flipped = frame;
  flipped[flipped.size() / 2] ^= 0x55;
  nc::ParseResult r = ParseFrame(flipped, &m);
  QCHECK(r.status == nc::ParseStatus::kDecompress || r.status == nc::ParseStatus::kNotJson);  // a flipped bit decodes to nothing or to text that is not the document
  QCHECK(ParseFrame(frame.substr(0, frame.size() - 7), &m).status == nc::ParseStatus::kDecompress);  // cut short
  QCHECK(std::string(nc::ParseStatusName(nc::ParseStatus::kDecompress)) == "decompress");
}

// ── the plan ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
nc::Manifest ParseOk(const json& entries, const char* label = "game") {
  nc::Manifest m;
  QCHECK(Parse(Doc(entries, json::array(), label), &m).status == nc::ParseStatus::kOk);
  return m;
}

const auto kNothingHeld = [](const nc::Entry&) { return false; };

void TheGameLabelIsRequiredByCodeAndAnUnlistedLabelIsOff() {
  nc::Policy policy;
  QCHECK(nc::StateOf("game", policy) == nc::LabelState::kRequired);
  QCHECK(nc::StateOf("lobby_posters", policy) == nc::LabelState::kOn);
  QCHECK(nc::StateOf("whatever", policy) == nc::LabelState::kOff);
  policy.overrides["game"] = nc::LabelState::kOff;
  QCHECK(nc::StateOf("game", policy) == nc::LabelState::kOff);
}

void AWholeGameOfNonTexturesPlansWithoutAnyPairing() {
  json entries = json::array();
  for (int i = 0; i < 100; ++i) entries.push_back(Entry(kArchiveType, ("0x" + std::to_string(i + 1)).c_str(), 'a' + (i % 5), 1000 + i));
  const nc::Plan plan = nc::MakePlan(ParseOk(entries), nc::kAndroidArm64, nc::Policy(), Roomy(), 0, kNothingHeld);
  QCHECK(plan.labels.size() == 1);
  QCHECK(plan.labels[0].skipped == nc::SkipReason::kNone);
  QCHECK(plan.labels[0].entries == 100 && plan.labels[0].refused.empty());
  QCHECK(plan.labels[0].to_fetch == 5);  // five distinct blobs: a blob is requested once however many entries name it
  QCHECK(plan.to_fetch == 5 && plan.fetch_bytes == plan.labels[0].fetch_bytes);
}

void ATexturePayloadThatDiffersFromStockStillNeedsItsDescriptor() {
  json px = Entry(kPixType, "0x7", 'f', 100, 5000);
  px["stock_size"] = 4000;
  json entries = json::array({px, Entry(kArchiveType, "0x8", 'a')});
  nc::Plan plan = nc::MakePlan(ParseOk(entries), nc::kAndroidArm64, nc::Policy(), Roomy(), 0, kNothingHeld);
  QCHECK(plan.labels[0].refused.size() == 1 && plan.labels[0].refused[0].name == 7);
  QCHECK(plan.labels[0].entries == 1);
  entries = json::array({px, Entry(kDescType, "0x7", 'd')});
  plan = nc::MakePlan(ParseOk(entries), nc::kAndroidArm64, nc::Policy(), Roomy(), 0, kNothingHeld);
  QCHECK(plan.labels[0].refused.empty() && plan.labels[0].entries == 2);
  // A payload with no stock size states nothing to compare against: it is not held to the rule.
  entries = json::array({Entry(kPixType, "0x7", 'f', 100, 5000)});
  plan = nc::MakePlan(ParseOk(entries), nc::kAndroidArm64, nc::Policy(), Roomy(), 0, kNothingHeld);
  QCHECK(plan.labels[0].refused.empty() && plan.labels[0].entries == 1);
}

void AHeldBlobIsNotFetchedAndIsNeverHashedByThePlan() {
  json entries = json::array({Entry(kArchiveType, "0x1", 'a', 100), Entry(kArchiveType, "0x2", 'b', 300)});
  int asked = 0;
  const nc::Plan plan = nc::MakePlan(ParseOk(entries), nc::kAndroidArm64, nc::Policy(), Roomy(), 0, [&](const nc::Entry& e) {
    ++asked;
    return e.sha256 == Sha('a');
  });
  QCHECK(plan.labels[0].cached == 1 && plan.labels[0].to_fetch == 1 && plan.labels[0].fetch_bytes == 300);
  QCHECK(asked >= 2);  // the plan asks the store about each blob (a lookup), and reads nothing
}

void ALabelThatDoesNotFitTheStorageBudgetIsSkippedWholeAndSaysWhy() {
  json entries = json::array({Entry(kArchiveType, "0x1", 'a', 600), Entry(kArchiveType, "0x2", 'b', 600)});
  nc::Limits l = Roomy();
  l.storage_budget = 1199;
  nc::Plan plan = nc::MakePlan(ParseOk(entries), nc::kAndroidArm64, nc::Policy(), l, 0, kNothingHeld);
  QCHECK(plan.labels[0].skipped == nc::SkipReason::kOverStorage);
  QCHECK(std::string(nc::SkipReasonName(plan.labels[0].skipped)) == "over_storage");
  QCHECK(plan.to_fetch == 0 && plan.fetch_bytes == 0);
  l.storage_budget = 1200;
  plan = nc::MakePlan(ParseOk(entries), nc::kAndroidArm64, nc::Policy(), l, 0, kNothingHeld);
  QCHECK(plan.labels[0].skipped == nc::SkipReason::kNone && plan.fetch_bytes == 1200);
  // What the store already holds costs the budget nothing.
  l.storage_budget = 600;
  plan = nc::MakePlan(ParseOk(entries), nc::kAndroidArm64, nc::Policy(), l, 0, [](const nc::Entry& e) { return e.sha256 == Sha('a'); });
  QCHECK(plan.labels[0].skipped == nc::SkipReason::kNone && plan.fetch_bytes == 600);
}

void ALabelThatNeedsMoreStoreFilesThanTheDescriptorsAllowIsSkipped() {
  json entries = json::array({Entry(kArchiveType, "0x1", 'a', 600), Entry(kArchiveType, "0x2", 'b', 600), Entry(kArchiveType, "0x3", 'c', 600)});
  nc::Limits l = Roomy();
  l.store_file_bytes = 1000;  // one 600-byte object per file: three files
  l.max_store_files = 2;
  nc::Plan plan = nc::MakePlan(ParseOk(entries), nc::kAndroidArm64, nc::Policy(), l, 0, kNothingHeld);
  QCHECK(plan.labels[0].skipped == nc::SkipReason::kOverFiles);
  QCHECK(std::string(nc::SkipReasonName(plan.labels[0].skipped)) == "over_files");
  l.max_store_files = 3;
  plan = nc::MakePlan(ParseOk(entries), nc::kAndroidArm64, nc::Policy(), l, 0, kNothingHeld);
  QCHECK(plan.labels[0].skipped == nc::SkipReason::kNone);
  // What is held already fills files, and an object is not split across two: 1500 held bytes fill two 1000-byte files with 500 free in
  // the second, which holds a 400-byte object; 1700 held leave 300 free, so the object opens a third file.
  l.store_file_bytes = 1000;
  l.max_store_files = 2;
  plan = nc::MakePlan(ParseOk(json::array({Entry(kArchiveType, "0x1", 'a', 400)})), nc::kAndroidArm64, nc::Policy(), l, 1500, kNothingHeld);
  QCHECK(plan.labels[0].skipped == nc::SkipReason::kNone);
  plan = nc::MakePlan(ParseOk(json::array({Entry(kArchiveType, "0x1", 'a', 400)})), nc::kAndroidArm64, nc::Policy(), l, 1700, kNothingHeld);
  QCHECK(plan.labels[0].skipped == nc::SkipReason::kOverFiles);
}

void RequiredLabelsComeFirstAndRequiredOnlyDropsTheRest() {
  json labels = json::object();
  labels["extra"] = json{{"entries", json::array({Entry(kArchiveType, "0x1", 'a')})}};
  labels["game"] = json{{"entries", json::array({Entry(kArchiveType, "0x2", 'b')})}};
  const std::string text = json{{"schema", "nevr-cargo-manifest"}, {"schema_version", 1}, {"labels", labels}}.dump();
  nc::Manifest m;
  QCHECK(Parse(text, &m).status == nc::ParseStatus::kOk);
  nc::Policy policy;
  policy.overrides["extra"] = nc::LabelState::kOn;
  nc::Plan plan = nc::MakePlan(m, nc::kAndroidArm64, policy, Roomy(), 0, kNothingHeld);
  QCHECK(plan.labels.size() == 2 && plan.labels[0].label == "game" && plan.labels[1].label == "extra");
  policy.required_only = true;
  plan = nc::MakePlan(m, nc::kAndroidArm64, policy, Roomy(), 0, kNothingHeld);
  QCHECK(plan.labels[1].skipped == nc::SkipReason::kRequiredOnly);
}

void AnotherPlatformsEntriesAreNotPlanned() {
  json e = Entry(kArchiveType, "0x1", 'a');
  e["platform"] = "windows-x86_64";
  const nc::Plan plan = nc::MakePlan(ParseOk(json::array({e})), nc::kAndroidArm64, nc::Policy(), Roomy(), 0, kNothingHeld);
  QCHECK(plan.platform_entries == 0 && plan.labels[0].skipped == nc::SkipReason::kNoEntries);
}

}  // namespace

int main() {
  AManifestFrameDecodesAndParsesLikeItsText();
  PlainJsonIsNotAManifest();
  AFrameThatStatesNoContentSizeIsRefused();
  TheDeclaredSizeIsHeldToTheLimitBeforeAnythingIsDecoded();
  AFrameTheDecoderRefusesOrThatDecodesToAnotherSizeIsRefused();
  ANonTextureEntryNeedsNoStockSize();
  ThereIsNoEntryCapOfTwoThousandAndFortyEight();
  TheEntryCountAndTheTextAreHeldToTheComputedLimits();
  ARecordTheValidatorRefusesIsLeftOutAndListedByRule();
  BothEntriesOfADuplicateKeyAreRefused();
  AKeySharedByTwoLabelsIsADuplicateToo();
  TheEmptyListIsReadAndItsRecordsAreChecked();
  ADocumentWithNoEmptyListParses();
  AShapeThatIsNotTheSchemaIsRefusedWhole();
  AnUnknownKeyIsCountedNeverNamed();
  TheEntryMemoryCostIsWhatTheRunHolds();
  TheGameLabelIsRequiredByCodeAndAnUnlistedLabelIsOff();
  AWholeGameOfNonTexturesPlansWithoutAnyPairing();
  ATexturePayloadThatDiffersFromStockStillNeedsItsDescriptor();
  AHeldBlobIsNotFetchedAndIsNeverHashedByThePlan();
  ALabelThatDoesNotFitTheStorageBudgetIsSkippedWholeAndSaysWhy();
  ALabelThatNeedsMoreStoreFilesThanTheDescriptorsAllowIsSkipped();
  RequiredLabelsComeFirstAndRequiredOnlyDropsTheRest();
  AnotherPlatformsEntriesAreNotPlanned();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "cargo_manifest_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("cargo_manifest_test: all checks passed\n");
  return 0;
}
