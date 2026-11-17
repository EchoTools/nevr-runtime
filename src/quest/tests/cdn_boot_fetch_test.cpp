// Host test for the client's boot-fetch decision and its stamp (src/cdn/cdn_boot_fetch.*; PLAN-v7 5.4 "Client start" item 1 and "The
// boot-fetch stamp"; R6b-1 tests 6-11, 15, 16 first half). Pure: no Env, no file, no request.
//
// Built and run by `just test-quest-data`.

#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>

#include "cdn/cdn_boot_fetch.h"
#include "cdn/cdn_manifest.h"
#include "cdn/cdn_plan.h"
#include "quest/tests/test_check.h"

namespace {

namespace nc = nevr_cdn;

constexpr std::uint64_t kType = 0x2a41cf1c1d9e5d32ULL;
constexpr std::uint64_t kCombatName = 0x1001;
constexpr std::uint64_t kOtherCombatName = 0x1002;
constexpr std::uint64_t kPosterName = 0x2001;
constexpr std::uint64_t kSize = 10;
constexpr std::uint64_t kBytes = 20;
constexpr std::uint32_t kAlign = 4;
constexpr char kObjectHash = 'a';
constexpr char kMissingHash = 'c';
constexpr char kSavedHashChar = 'b';
constexpr char kOtherHashChar = 'd';
constexpr std::size_t kSha256Chars = 64;
constexpr std::uint64_t kFree = 1000;
constexpr std::uint64_t kNeed = 500;
constexpr int kNoReads = 0;
constexpr std::uint64_t kNearLargestGap = 10;
constexpr std::uint64_t kHugeNeed = 100;

std::string Hash(char c) { return std::string(kSha256Chars, c); }

nc::Entry MakeEntry(const std::string& label, std::uint64_t name, char hash) {
  nc::Entry e;
  e.label = label;
  e.platform = nc::kWindowsX64.name;
  e.type = kType;
  e.name = name;
  e.sha256 = Hash(hash);
  e.size = kSize;
  e.bytes = kBytes;
  e.align = kAlign;
  return e;
}

// The saved manifest of a client that has the combat set: one combat object, held, and one poster object.
nc::Manifest WholeCombatManifest() {
  nc::Manifest m;
  m.labels = {"combat_content", "lobby_posters"};
  m.entries.push_back(MakeEntry("combat_content", kCombatName, kObjectHash));
  m.entries.push_back(MakeEntry("lobby_posters", kPosterName, kObjectHash));
  return m;
}

// A saved manifest with no record of combat_content for the platform (C5: a 6.18.1 player's first 6.18.2 start).
nc::Manifest PosterOnlyManifest() {
  nc::Manifest m;
  m.labels = {"lobby_posters"};
  m.entries.push_back(MakeEntry("lobby_posters", kPosterName, kObjectHash));
  return m;
}

// A saved manifest whose combat object is named but not held.
nc::Manifest CombatMissingManifest() {
  nc::Manifest m;
  m.labels = {"combat_content"};
  m.entries.push_back(MakeEntry("combat_content", kOtherCombatName, kMissingHash));
  return m;
}

// The base input: a client whose saved manifest holds the combat set whole, no marker, no stamp, combat_content on by default.
struct Base {
  nc::Manifest saved = WholeCombatManifest();
  nc::BootFetchInput in;
  Base() {
    in.saved = &saved;
    in.saved_sha256 = Hash(kSavedHashChar);
    in.platform = nc::kWindowsX64;
    in.boot_only = {"combat_content"};
    in.held = [](const nc::Entry&) { return true; };
  }
};

void Expect(const nc::BootFetchDecision& got, bool fetch, nc::BootFetchReason reason, const char* what) {
  if (got.fetch != fetch || got.reason != reason) {
    std::fprintf(stderr, "%s:%d %s: got fetch=%d reason=%s, expected fetch=%d reason=%s\n", __FILE__, __LINE__, what, got.fetch ? 1 : 0,
                 nc::BootFetchReasonName(got.reason), fetch ? 1 : 0, nc::BootFetchReasonName(reason));
    ++quest_test::Failures();
  }
}

// The reasons, row by row (test 11).
void EachReasonRowByRow() {
  {
    Base b;
    Expect(nc::NeedsBootFetch(b.in), false, nc::BootFetchReason::kNone, "a whole cache, no marker: no request");
  }
  {
    Base b;
    b.in.saved = nullptr;
    const nc::BootFetchDecision d = nc::NeedsBootFetch(b.in);
    Expect(d, true, nc::BootFetchReason::kNoSavedManifest, "no saved manifest");
    QCHECK(d.label == "combat_content");
  }
  {
    Base b;
    b.in.marker = true;
    Expect(nc::NeedsBootFetch(b.in), true, nc::BootFetchReason::kRefetchMarker, "the marker over a whole cache (test 10)");
  }
  {
    nc::Manifest poster = PosterOnlyManifest();
    Base b;
    b.in.saved = &poster;
    Expect(nc::NeedsBootFetch(b.in), true, nc::BootFetchReason::kLabelAbsent, "the saved manifest lacks the label (C5)");
  }
  {
    nc::Manifest missing = CombatMissingManifest();
    Base b;
    b.in.saved = &missing;
    b.in.held = [](const nc::Entry&) { return false; };
    Expect(nc::NeedsBootFetch(b.in), true, nc::BootFetchReason::kObjectsMissing, "a named object the store does not hold");
  }
  {
    // An empty record of the label for this platform counts as a record: the label is not absent.
    nc::Manifest empties;
    empties.labels = {"combat_content"};
    nc::EmptyRecord z;
    z.label = "combat_content";
    z.platform = nc::kWindowsX64.name;
    z.type = kType;
    z.name = kCombatName;
    z.align = kAlign;
    empties.empties.push_back(z);
    Base b;
    b.in.saved = &empties;
    Expect(nc::NeedsBootFetch(b.in), false, nc::BootFetchReason::kNone, "an empty record of the label holds it whole");
  }
  {
    // A record of no platform belongs to the platform the plan is made for (cdn_manifest.h): it counts for this one.
    nc::Manifest none;
    none.labels = {"combat_content"};
    nc::EmptyRecord z;
    z.label = "combat_content";
    z.platform = "";
    z.type = kType;
    z.name = kCombatName;
    z.align = kAlign;
    none.empties.push_back(z);
    Base b;
    b.in.saved = &none;
    Expect(nc::NeedsBootFetch(b.in), false, nc::BootFetchReason::kNone, "an empty record of no platform counts for this one");
  }
  {
    // A record of another platform does not count for this one.
    nc::Manifest other = WholeCombatManifest();
    for (nc::Entry& e : other.entries) e.platform = nc::kAndroidArm64.name;
    Base b;
    b.in.saved = &other;
    Expect(nc::NeedsBootFetch(b.in), true, nc::BootFetchReason::kLabelAbsent, "records of another platform are not this platform's");
  }
  {
    // Order: no saved manifest comes before the marker, the marker before label_absent.
    Base b;
    b.in.saved = nullptr;
    b.in.marker = true;
    Expect(nc::NeedsBootFetch(b.in), true, nc::BootFetchReason::kNoSavedManifest, "no saved manifest is checked before the marker");
  }
  {
    nc::Manifest poster = PosterOnlyManifest();
    Base b;
    b.in.saved = &poster;
    b.in.marker = true;
    Expect(nc::NeedsBootFetch(b.in), true, nc::BootFetchReason::kRefetchMarker, "the marker is checked before label_absent");
  }
}

// The exclusions (tests 6-9, and 16's first half).
void ExclusionsMakeNoRequest() {
  {
    Base b;
    b.in.policy.overrides["combat_content"] = nc::LabelState::kOff;
    b.in.saved = nullptr;
    Expect(nc::NeedsBootFetch(b.in), false, nc::BootFetchReason::kNone, "combat_content off by config (test 6)");
  }
  {
    Base b;
    b.in.policy.required_only = true;
    b.in.saved = nullptr;
    Expect(nc::NeedsBootFetch(b.in), false, nc::BootFetchReason::kNone, "required_only keeps combat_content off (test 7)");
  }
  {
    // required_only keeps a label that is required: the same label, set required, is fetched.
    Base b;
    b.in.policy.required_only = true;
    b.in.policy.overrides["combat_content"] = nc::LabelState::kRequired;
    b.in.saved = nullptr;
    Expect(nc::NeedsBootFetch(b.in), true, nc::BootFetchReason::kNoSavedManifest, "a required label survives required_only");
  }
  {
    Base b;
    b.in.boot_only.clear();
    b.in.saved = nullptr;
    Expect(nc::NeedsBootFetch(b.in), false, nc::BootFetchReason::kNone, "an empty boot_only (the Quest) (test 8)");
  }
  {
    Base b;
    b.in.signed_only_refuses = true;
    b.in.saved = nullptr;
    Expect(nc::NeedsBootFetch(b.in), false, nc::BootFetchReason::kNone, "a signed-only run with an unverified manifest (test 9)");
  }
  {
    Base b;
    b.in.last_update_trial = true;
    b.in.saved = nullptr;
    Expect(nc::NeedsBootFetch(b.in), false, nc::BootFetchReason::kNone, "the last start of an update trial (test 16)");
  }
}

// The stamp's settle rule (test 15; G1, G6).
void TheStampSettlesWhatItNames() {
  nc::Manifest poster = PosterOnlyManifest();
  nc::BootFetchStamp settled;
  settled.manifest_sha256 = Hash(kSavedHashChar);
  settled.cause = nc::BootFetchCause::kSettled;
  {
    Base b;
    b.in.saved = &poster;
    b.in.stamp = &settled;
    Expect(nc::NeedsBootFetch(b.in), false, nc::BootFetchReason::kNone, "a settled stamp for the saved manifest settles label_absent");
  }
  {
    // Test 15's last part: a stamp for another manifest settles nothing.
    nc::BootFetchStamp other = settled;
    other.manifest_sha256 = Hash(kOtherHashChar);
    Base b;
    b.in.saved = &poster;
    b.in.stamp = &other;
    Expect(nc::NeedsBootFetch(b.in), true, nc::BootFetchReason::kLabelAbsent, "a stamp for another manifest settles nothing");
  }
  {
    nc::BootFetchStamp undeliverable = settled;
    undeliverable.cause = nc::BootFetchCause::kUndeliverable;
    nc::Manifest missing = CombatMissingManifest();
    Base b;
    b.in.saved = &missing;
    b.in.held = [](const nc::Entry&) { return false; };
    b.in.stamp = &undeliverable;
    Expect(nc::NeedsBootFetch(b.in), false, nc::BootFetchReason::kNone, "an undeliverable stamp settles objects_missing (G6)");
  }
  {
    // A stamp that does not parse is absent; the caller passes null.
    nc::BootFetchStamp parsed;
    QCHECK(!nc::ParseBootFetchStamp("{", &parsed));
    Base b;
    b.in.saved = &poster;
    b.in.stamp = nullptr;
    Expect(nc::NeedsBootFetch(b.in), true, nc::BootFetchReason::kLabelAbsent, "an unparsed stamp is absent");
  }
}

// Test 15: a no_space stamp is settled while free space has grown by less than what the label needed.
void NoSpaceIsRearmedByFreeSpace() {
  nc::Manifest missing = CombatMissingManifest();
  nc::BootFetchStamp nospace;
  nospace.manifest_sha256 = Hash(kSavedHashChar);
  nospace.cause = nc::BootFetchCause::kNoSpace;
  nospace.free_bytes = kFree;
  nospace.need_bytes = kNeed;
  std::uint64_t free_now = 0;
  Base b;
  b.in.saved = &missing;
  b.in.held = [](const nc::Entry&) { return false; };
  b.in.stamp = &nospace;
  b.in.free_space = [&free_now](std::uint64_t* out) {
    *out = free_now;
    return true;
  };
  free_now = kFree + kNeed - 1;
  Expect(nc::NeedsBootFetch(b.in), false, nc::BootFetchReason::kNone, "free F + N - 1: the stamp still settles (test 15)");
  free_now = kFree + kNeed;
  Expect(nc::NeedsBootFetch(b.in), true, nc::BootFetchReason::kObjectsMissing, "free F + N: room grew by what is needed (test 15)");
  {
    // The sum saturates: a stamp near the largest value never wraps to a small one, so free space below the largest value still settles.
    nc::BootFetchStamp huge = nospace;
    huge.free_bytes = std::numeric_limits<std::uint64_t>::max() - kNearLargestGap;
    huge.need_bytes = kHugeNeed;
    b.in.stamp = &huge;
    free_now = std::numeric_limits<std::uint64_t>::max() - 1;
    Expect(nc::NeedsBootFetch(b.in), false, nc::BootFetchReason::kNone, "a saturating sum does not wrap to a small value");
  }
}

// The free space is read only when the stamp is a no_space stamp for the saved manifest, and an unreadable free space settles nothing.
void FreeSpaceIsReadOnlyWhenItMatters() {
  nc::Manifest missing = CombatMissingManifest();
  nc::BootFetchStamp nospace;
  nospace.manifest_sha256 = Hash(kSavedHashChar);
  nospace.cause = nc::BootFetchCause::kNoSpace;
  nospace.free_bytes = kFree;
  nospace.need_bytes = kNeed;
  int calls = kNoReads;
  Base b;
  b.in.saved = &missing;
  b.in.held = [](const nc::Entry&) { return false; };
  b.in.free_space = [&calls](std::uint64_t* out) {
    ++calls;
    *out = kFree + kNeed + kNeed;
    return true;
  };
  b.in.stamp = nullptr;
  nc::NeedsBootFetch(b.in);
  QCHECK(calls == kNoReads);  // no stamp: no read
  nospace.manifest_sha256 = Hash(kOtherHashChar);
  b.in.stamp = &nospace;
  nc::NeedsBootFetch(b.in);
  QCHECK(calls == kNoReads);  // another manifest's stamp: no read
  nospace.manifest_sha256 = Hash(kSavedHashChar);
  b.in.stamp = &nospace;
  nc::NeedsBootFetch(b.in);
  QCHECK(calls == 1);  // this manifest's no_space stamp: read once
  b.in.free_space = [](std::uint64_t*) { return false; };
  Expect(nc::NeedsBootFetch(b.in), true, nc::BootFetchReason::kObjectsMissing, "an unreadable free space does not settle no_space");
}

// The stamp's text: the format and the parse (R6b-1's format, for R6b-2's writes).
void TheStampRoundTripsAndRefusesBadText() {
  nc::BootFetchStamp s;
  s.manifest_sha256 = Hash(kSavedHashChar);
  s.cause = nc::BootFetchCause::kNoSpace;
  s.free_bytes = kFree;
  s.need_bytes = kNeed;
  nc::BootFetchStamp back;
  QCHECK(nc::ParseBootFetchStamp(nc::FormatBootFetchStamp(s), &back));
  QCHECK(back.manifest_sha256 == s.manifest_sha256 && back.cause == s.cause && back.free_bytes == s.free_bytes &&
         back.need_bytes == s.need_bytes);
  nc::BootFetchStamp out;
  QCHECK(!nc::ParseBootFetchStamp("", &out));
  QCHECK(!nc::ParseBootFetchStamp("not json", &out));
  QCHECK(!nc::ParseBootFetchStamp("[]", &out));
  QCHECK(!nc::ParseBootFetchStamp(R"({"manifest_sha256":"ABC","cause":"settled","free_bytes":0,"need_bytes":0})", &out));
  QCHECK(!nc::ParseBootFetchStamp(R"({"manifest_sha256":")" + Hash(kSavedHashChar) + R"(","cause":"other","free_bytes":0,"need_bytes":0})", &out));
  QCHECK(!nc::ParseBootFetchStamp(R"({"manifest_sha256":")" + Hash(kSavedHashChar) + R"(","cause":"settled","free_bytes":0})", &out));
  QCHECK(!nc::ParseBootFetchStamp(R"({"manifest_sha256":")" + Hash(kSavedHashChar) + R"(","cause":"settled","free_bytes":-1,"need_bytes":0})", &out));
  QCHECK(!nc::ParseBootFetchStamp(R"({"manifest_sha256":")" + Hash(kSavedHashChar) + R"(","cause":"settled","free_bytes":1.5,"need_bytes":0})", &out));
  QCHECK(nc::ParseBootFetchStamp(R"({"manifest_sha256":")" + Hash(kSavedHashChar) + R"(","cause":"undeliverable","free_bytes":0,"need_bytes":0})", &out));
  QCHECK(out.cause == nc::BootFetchCause::kUndeliverable);
}

// The names are the stamp's text and the log's words: pinned here, as the stamp file and the R6b-2 records carry them.
void TheNamesAreFixed() {
  QCHECK(std::string(nc::BootFetchCauseName(nc::BootFetchCause::kSettled)) == "settled");
  QCHECK(std::string(nc::BootFetchCauseName(nc::BootFetchCause::kUndeliverable)) == "undeliverable");
  QCHECK(std::string(nc::BootFetchCauseName(nc::BootFetchCause::kNoSpace)) == "no_space");
  QCHECK(std::string(nc::BootFetchReasonName(nc::BootFetchReason::kNone)) == "none");
  QCHECK(std::string(nc::BootFetchReasonName(nc::BootFetchReason::kNoSavedManifest)) == "no_saved_manifest");
  QCHECK(std::string(nc::BootFetchReasonName(nc::BootFetchReason::kRefetchMarker)) == "refetch_marker");
  QCHECK(std::string(nc::BootFetchReasonName(nc::BootFetchReason::kLabelAbsent)) == "label_absent");
  QCHECK(std::string(nc::BootFetchReasonName(nc::BootFetchReason::kObjectsMissing)) == "objects_missing");
  nc::BootFetchCause cause = nc::BootFetchCause::kSettled;
  QCHECK(nc::ParseBootFetchCause("no_space", &cause) && cause == nc::BootFetchCause::kNoSpace);
  QCHECK(!nc::ParseBootFetchCause("nospace", &cause));
}

}  // namespace

int main() {
  EachReasonRowByRow();
  ExclusionsMakeNoRequest();
  TheStampSettlesWhatItNames();
  NoSpaceIsRearmedByFreeSpace();
  FreeSpaceIsReadOnlyWhenItMatters();
  TheStampRoundTripsAndRefusesBadText();
  TheNamesAreFixed();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "cdn_boot_fetch_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("cdn_boot_fetch_test: all checks passed\n");
  return 0;
}
