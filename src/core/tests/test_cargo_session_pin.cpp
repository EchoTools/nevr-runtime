// The cargo pin's pure core (core/cargo_session_pin.h; PLAN-v7 5.5 R7): the golden pin, the record rules, the whole-set rule, the gate-first
// boot status and the 16 decision rows with their notice lines. No Env, no game, no Win32.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cdn/cdn_manifest.h"
#include "cdn/cdn_plan.h"
#include "cdn/cdn_script_gate.h"
#include "cdn/cdn_store.h"
#include "core/cargo_session_pin.h"
#include "core/combat_level_class.h"
#include "core/map_session_pin.h"
#include "nevr/symbol_hash.h"
#include "quest/data/sha256.h"

namespace nc = nevr_cdn;
namespace ncp = nevr_cargo_pin;
using nevr_quest::data::Sha256Hex;

namespace {

constexpr const char* kPc = "windows-x86_64";
constexpr std::uint64_t kType1 = 0x991421b41de58370ULL;
constexpr std::uint64_t kName1 = 0x94ba7ca39fc97e1aULL;
constexpr std::uint64_t kType2 = 0x489bb35d53ca50e9ULL;
constexpr std::uint64_t kName2 = 0x3fa6d25bc24a7b01ULL;
constexpr std::uint64_t kEmptyType = 0xe2efe7289d5985b8ULL;
constexpr std::uint64_t kGoldenPin = 0xc7572cc7b54501d4ULL;
constexpr const char* kGoldenSha = "c7572cc7b54501d4cab9bb907ab52dc57f416f065c18734afeaf700639955053";

// The manifest's and the server's pins for the decision rows: three distinct values, none of them a "none" or "absent" pin.
constexpr std::uint64_t kLocal = 0x1111111111111111ULL;
constexpr std::uint64_t kServer = 0x2222222222222222ULL;
constexpr std::uint64_t kManifest = 0x3333333333333333ULL;
constexpr std::size_t kLineMax = nevr_map_session_pin::kNoticeLineMax;
constexpr std::size_t kLongTokenBytes = 40;  // longer than the 24 bytes row 9 has room for: its field is cut
constexpr std::size_t kDigestChars = 64;      // a SHA-256 hex digest's length
constexpr std::size_t kWholeNames = 3;        // the names the whole-set mount in Whole() counts for the label
constexpr std::size_t kPinHexChars = 16;      // the pin is the first 8 bytes of the digest: 16 hex characters
constexpr std::size_t kDigestRestChars = kDigestChars - kPinHexChars;  // the rest of a 64-character digest
constexpr std::size_t kRefusedHeadBytes = 20;  // "EchoCombat refused (" is 20 bytes: the head row 9 keeps before its token

std::string Sha(const std::string& bytes) { return Sha256Hex(bytes.data(), bytes.size()); }

nc::Entry Ent(std::uint64_t type, std::uint64_t name, char fill, const char* platform = kPc) {
  nc::Entry e;
  e.label = "combat_content";
  e.platform = platform;
  e.type = type;
  e.name = name;
  e.sha256 = std::string(kDigestChars, fill);
  return e;
}

nc::EmptyRecord Empty(std::uint64_t type, std::uint64_t name, const char* platform = kPc) {
  nc::EmptyRecord r;
  r.label = "combat_content";
  r.platform = platform;
  r.type = type;
  r.name = name;
  return r;
}

nc::Manifest Golden() {
  nc::Manifest m;
  m.entries = {Ent(kType1, kName1, 'a'), Ent(kType2, kName2, 'b')};
  m.empties = {Empty(kEmptyType, kName2)};
  return m;
}

const nc::Platform& Pc() { return nc::kWindowsX64; }

nc::Policy PolicyWith(nc::LabelState state) {
  nc::Policy policy;
  policy.overrides["combat_content"] = state;
  return policy;
}

nc::Mounted Whole() {
  nc::Mounted m;
  m.per_label.push_back(nc::LabelMount{"combat_content", kWholeNames, 0, 0});
  return m;
}

std::string Line(const char* label, const char* platform, const char* type, const char* name, const std::string& digest) {
  return std::string(label) + "\t" + platform + "\t" + type + "\t" + name + "\t" + digest + "\n";
}

// The decision rows' view: a whole set of this run, a fetch that settled, a manifest this run parsed. Each test changes one input.
ncp::CargoView View() {
  ncp::CargoView v;
  v.local = kLocal;
  v.status.state = ncp::CargoBoot::kWhole;
  v.fetch = ncp::CargoFetch::kNone;
  v.manifest_known = true;
  v.manifest = kManifest;
  return v;
}

ncp::CargoView Refused(const std::string& token) {
  ncp::CargoView v = View();
  v.status.state = ncp::CargoBoot::kRefused;
  v.status.token = token;
  return v;
}

std::uint64_t Level() { return nevr_combat_level::kCombatMaps[0]; }

}  // namespace

// ── the pin ──────────────────────────────────────────────────────────────────────────────────────────────────────────────────

TEST(CargoSessionPin, GoldenFixtureGivesTheGoldenTextAndPin) {
  const std::string text = ncp::CargoPinText(Golden(), Pc(), nc::Policy());
  const std::string expected =
      Line("combat_content", kPc, "489bb35d53ca50e9", "3fa6d25bc24a7b01", std::string(kDigestChars, 'b')) +
      Line("combat_content", kPc, "991421b41de58370", "94ba7ca39fc97e1a", std::string(kDigestChars, 'a')) +
      Line("combat_content", kPc, "e2efe7289d5985b8", "3fa6d25bc24a7b01", "empty");
  EXPECT_EQ(text, expected);
  EXPECT_EQ(Sha(text), kGoldenSha);
  EXPECT_EQ(ncp::CargoPinOf(Golden(), Pc(), nc::Policy(), Sha), kGoldenPin);
}

TEST(CargoSessionPin, ThePinDoesNotDependOnRecordOrder) {
  nc::Manifest reversed = Golden();
  std::reverse(reversed.entries.begin(), reversed.entries.end());
  EXPECT_EQ(ncp::CargoPinOf(reversed, Pc(), nc::Policy(), Sha), kGoldenPin);
}

TEST(CargoSessionPin, EachFieldOfARecordChangesThePin) {
  nc::Manifest type = Golden();
  type.entries[0].type += 1;
  nc::Manifest name = Golden();
  name.entries[0].name += 1;
  nc::Manifest digest = Golden();
  digest.entries[0].sha256 = std::string(kDigestChars, 'c');
  nc::Manifest other_platform = Golden();
  other_platform.entries[0].platform = "android-arm64";  // not this platform: the record leaves the set
  for (const nc::Manifest* m : {&type, &name, &digest, &other_platform}) {
    EXPECT_NE(ncp::CargoPinOf(*m, Pc(), nc::Policy(), Sha), kGoldenPin);
  }
}

TEST(CargoSessionPin, AnEmptyRecordAndAnEntryWithTheSameTypeAndNameDiffer) {
  nc::Manifest entry_only;
  entry_only.entries = {Ent(kType1, kName1, 'a')};
  nc::Manifest empty_only;
  empty_only.empties = {Empty(kType1, kName1)};
  EXPECT_NE(ncp::CargoPinText(entry_only, Pc(), nc::Policy()), ncp::CargoPinText(empty_only, Pc(), nc::Policy()));
  EXPECT_NE(ncp::CargoPinOf(entry_only, Pc(), nc::Policy(), Sha), ncp::CargoPinOf(empty_only, Pc(), nc::Policy(), Sha));
}

TEST(CargoSessionPin, AnEmptyRecordWithNoPlatformGivesTheLineOfItsWindowsForm) {
  // G10: a top-level empty record (platform "") belongs to the plan's platform, so both roles write the same bytes.
  nc::Manifest blank;
  blank.empties = {Empty(kEmptyType, kName2, "")};
  nc::Manifest windows;
  windows.empties = {Empty(kEmptyType, kName2, kPc)};
  const std::string text = ncp::CargoPinText(blank, Pc(), nc::Policy());
  EXPECT_EQ(text, ncp::CargoPinText(windows, Pc(), nc::Policy()));
  EXPECT_EQ(text, Line("combat_content", kPc, "e2efe7289d5985b8", "3fa6d25bc24a7b01", "empty"));
  EXPECT_EQ(ncp::CargoPinOf(blank, Pc(), nc::Policy(), Sha), ncp::CargoPinOf(windows, Pc(), nc::Policy(), Sha));
}

TEST(CargoSessionPin, APolicyOffOrNoRecordOfThePinLabelGivesNoPin) {
  EXPECT_EQ(ncp::CargoPinOf(Golden(), Pc(), PolicyWith(nc::LabelState::kOff), Sha), ncp::kCargoPinNone);
  nc::Manifest none;
  none.entries = {Ent(kType1, kName1, 'a', "android-arm64")};
  EXPECT_EQ(ncp::CargoPinOf(none, Pc(), nc::Policy(), Sha), ncp::kCargoPinNone);
}

TEST(CargoSessionPin, ANotWholeSetHasNoPin) {
  const nc::Manifest boot = Golden();
  const std::vector<nc::GatedLabel> none;
  EXPECT_EQ(ncp::CargoBootPin(boot, Pc(), Whole(), none, nc::Policy(), Sha), kGoldenPin);

  nc::Mounted missing = Whole();
  missing.per_label[0].missing = 1;
  EXPECT_EQ(ncp::CargoBootPin(boot, Pc(), missing, none, nc::Policy(), Sha), ncp::kCargoPinNone);

  nc::Mounted refused = Whole();
  refused.per_label[0].refused = 1;
  EXPECT_EQ(ncp::CargoBootPin(boot, Pc(), refused, none, nc::Policy(), Sha), ncp::kCargoPinNone);

  nc::Mounted empty = Whole();
  empty.per_label[0].names = 0;
  EXPECT_EQ(ncp::CargoBootPin(boot, Pc(), empty, none, nc::Policy(), Sha), ncp::kCargoPinNone);

  EXPECT_EQ(ncp::CargoBootPin(boot, Pc(), Whole(), none, PolicyWith(nc::LabelState::kOff), Sha), ncp::kCargoPinNone);
  EXPECT_EQ(ncp::CargoBootPin(boot, Pc(), Whole(), {{"combat_content", "script_shadowed", {}, ""}}, nc::Policy(), Sha),
            ncp::kCargoPinNone);
}

TEST(CargoSessionPin, TheSeamForcesTheZeroAndNoneRule) {
  // The first 8 bytes of the digest are the pin; 0 and kCargoPinNone become the value XOR 1.
  const std::string zero = std::string(kPinHexChars, '0') + std::string(kDigestRestChars, 'f');
  const std::string none = "3a6a199923324d75" + std::string(kDigestRestChars, 'f');
  EXPECT_EQ(ncp::CargoPinFromHex(zero), 1U);
  EXPECT_EQ(ncp::CargoPinFromHex(none), ncp::kCargoPinNone ^ 1U);
  EXPECT_EQ(ncp::CargoPinFromHex(kGoldenSha), kGoldenPin);
  const ncp::Sha256HexFn to_zero = [&zero](const std::string&) { return zero; };
  EXPECT_EQ(ncp::CargoPinOf(Golden(), Pc(), nc::Policy(), to_zero), 1U);
}

TEST(CargoSessionPin, NoneIsTheHashOfItsNameAndNeverZero) {
  EXPECT_EQ(ncp::kCargoPinNone, EchoVR::CSymbol64Hash("nevr_cargo_pin_none"));
  EXPECT_NE(ncp::kCargoPinNone, 0U);
  EXPECT_EQ(ncp::kCargoPinAbsent, 0U);
}

TEST(CargoSessionPin, TheScopeIsTheFourCombatMapsAndTheTwoPebblesLevels) {
  for (const char* name : nevr_combat_level::kCombatMapNames) EXPECT_TRUE(ncp::IsCargoPinLevel(EchoVR::CSymbol64Hash(name)));
  EXPECT_EQ(EchoVR::CSymbol64Hash("mpl_combat_pebbles"), ncp::kCombatPebblesLevel);
  EXPECT_EQ(EchoVR::CSymbol64Hash("pty_mpl_combat_pebbles"), ncp::kCombatPebblesPayloadLevel);
  EXPECT_TRUE(ncp::IsCargoPinLevel(ncp::kCombatPebblesLevel));
  EXPECT_TRUE(ncp::IsCargoPinLevel(ncp::kCombatPebblesPayloadLevel));
  // The combat lobby is out of scope (PLAN-v7 5.5 "Scope"): the main lobby and the Arena levels stay out.
  EXPECT_FALSE(ncp::IsCargoPinLevel(EchoVR::CSymbol64Hash("mpl_lobby_b2")));
  EXPECT_FALSE(ncp::IsCargoPinLevel(EchoVR::CSymbol64Hash("mpl_lobby_b_combat")));
  EXPECT_FALSE(ncp::IsCargoPinLevel(EchoVR::CSymbol64Hash("mpl_arena_a")));
  EXPECT_FALSE(ncp::IsCargoPinLevel(0));
}

// ── the boot status (G3) ────────────────────────────────────────────────────────────────────────────────────────────────

TEST(CargoSessionPin, TheGateTokenGivesItsRowEvenWhenThePolicyIsOff) {
  // ApplyGate turns a gated label off, so the policy reads `off`; the gate's token must still reach the status (G3).
  const nc::Policy off = PolicyWith(nc::LabelState::kOff);
  for (const char* token : {"script_shadowed", "script_undelivered", "pack_hash", "signed_only"}) {
    const std::vector<nc::GatedLabel> gated = {{"combat_content", token, {}, ""}};
    const ncp::CargoBootStatus s = ncp::BootStatus(Whole(), gated, off);
    EXPECT_EQ(s.state, ncp::CargoBoot::kRefused) << token;
    EXPECT_EQ(s.token, token);
  }
}

TEST(CargoSessionPin, TheBootStatusStatesWhatTheMountAndThePolicyGive) {
  EXPECT_EQ(ncp::BootStatus(Whole(), {}, nc::Policy()).state, ncp::CargoBoot::kWhole);
  EXPECT_EQ(ncp::BootStatus(Whole(), {}, PolicyWith(nc::LabelState::kOff)).state, ncp::CargoBoot::kOff);
  nc::Policy required = nc::Policy();
  required.required_only = true;
  EXPECT_EQ(ncp::BootStatus(Whole(), {}, required).state, ncp::CargoBoot::kRequiredOnlyOff);
  EXPECT_EQ(ncp::BootStatus(nc::Mounted(), {}, nc::Policy()).state, ncp::CargoBoot::kNotWhole);
  nc::Mounted missing = Whole();
  missing.per_label[0].missing = 1;
  EXPECT_EQ(ncp::BootStatus(missing, {}, nc::Policy()).state, ncp::CargoBoot::kNotWhole);
}

// ── the decision and the notice (PLAN-v7 5.5 table) ─────────────────────────────────────────────────────────────────────

TEST(CargoSessionPin, Row1StaysOutsideTheScope) {
  const ncp::CargoDecision d = ncp::DecideCargo(View(), EchoVR::CSymbol64Hash("mpl_lobby_b2"), kServer);
  EXPECT_FALSE(d.leave);
  EXPECT_EQ(d.reason, "not_combat");
}

TEST(CargoSessionPin, Row2StaysWhenTheServerStatesNoSet) {
  const ncp::CargoDecision d = ncp::DecideCargo(View(), Level(), ncp::kCargoPinAbsent);
  EXPECT_FALSE(d.leave);
  EXPECT_EQ(d.reason, "unpinned");
}

TEST(CargoSessionPin, Row3StaysOnOneSet) {
  const ncp::CargoDecision d = ncp::DecideCargo(View(), Level(), kLocal);
  EXPECT_FALSE(d.leave);
  EXPECT_TRUE(d.reason.empty());
  EXPECT_TRUE(d.line.empty());
}

TEST(CargoSessionPin, Row3StaysWhenBothPinsAreNoSet) {
  // PLAN-v7 5.5, row 3: "`server_pin == B` | stay | | one set", with no further condition. The table says "The rows, first match
  // wins" (PLAN-v7 5.5, the paragraph before the table). Both pins kCargoPinNone is a match, so the client stays. Row 10 is
  // reached only by a client that has a set: "every client with a set leaves its combat sessions (row 10)" (PLAN-v7 5.5, the
  // server-deadline bullet) and "a client with a set leaves with row 10" (PLAN-v7 5.5, the R11 test list, test 11).
  ncp::CargoView v = View();
  v.local = ncp::kCargoPinNone;
  const ncp::CargoDecision d = ncp::DecideCargo(v, Level(), ncp::kCargoPinNone);
  EXPECT_FALSE(d.leave);
  EXPECT_TRUE(d.reason.empty());
  EXPECT_TRUE(d.line.empty());
}

TEST(CargoSessionPin, Row4LeavesWhenThePlayerTurnedTheLabelOff) {
  ncp::CargoView v = View();
  v.status.state = ncp::CargoBoot::kOff;
  const ncp::CargoDecision d = ncp::DecideCargo(v, Level(), kServer);
  EXPECT_TRUE(d.leave);
  EXPECT_EQ(d.reason, "label_off");
  EXPECT_EQ(d.line, "EchoCombat is off in config.yaml (assets.labels): turn it on");
}

TEST(CargoSessionPin, Row5LeavesWhenRequiredOnlyHoldsTheLabelOff) {
  ncp::CargoView v = View();
  v.status.state = ncp::CargoBoot::kRequiredOnlyOff;
  const ncp::CargoDecision d = ncp::DecideCargo(v, Level(), kServer);
  EXPECT_TRUE(d.leave);
  EXPECT_EQ(d.reason, "required_only");
  EXPECT_EQ(d.line, "EchoCombat is off: remove assets.required_only in config.yaml");
}

TEST(CargoSessionPin, Row6LeavesUnderSignedOnly) {
  const ncp::CargoDecision d = ncp::DecideCargo(Refused("signed_only"), Level(), kServer);
  EXPECT_TRUE(d.leave);
  EXPECT_EQ(d.reason, "signed_only");
  EXPECT_EQ(d.line, "EchoCombat is off under -require-signed-plugins: drop the flag");
}

TEST(CargoSessionPin, Row7LeavesWhenOldFilesShadowTheScript) {
  const ncp::CargoDecision d = ncp::DecideCargo(Refused("script_shadowed"), Level(), kServer);
  EXPECT_TRUE(d.leave);
  EXPECT_EQ(d.reason, "script_shadowed");
  EXPECT_EQ(d.line, "Old EchoCombat files in bin\\win10\\scripts block it: remove them");
}

TEST(CargoSessionPin, Row8LeavesWhenAPackReplacesIt) {
  const ncp::CargoDecision d = ncp::DecideCargo(Refused("pack_hash"), Level(), kServer);
  EXPECT_TRUE(d.leave);
  EXPECT_EQ(d.reason, "pack_hash");
  EXPECT_EQ(d.line, "A content pack replaces EchoCombat: remove the pack to play");
}

TEST(CargoSessionPin, Row9LeavesOnAnyOtherRefusalAndNamesIt) {
  const ncp::CargoDecision d = ncp::DecideCargo(Refused("script_undelivered"), Level(), kServer);
  EXPECT_TRUE(d.leave);
  EXPECT_EQ(d.reason, "refused");
  EXPECT_EQ(d.line, "EchoCombat refused (script_undelivered); see the nevr log");
}

TEST(CargoSessionPin, Row9CutsAnOverlongTokenAndFitsTheLine) {
  const ncp::CargoDecision d = ncp::DecideCargo(Refused(std::string(kLongTokenBytes, 'q')), Level(), kServer);
  EXPECT_TRUE(d.leave);
  EXPECT_LE(d.line.size(), kLineMax);
  EXPECT_EQ(d.line.compare(0, kRefusedHeadBytes, "EchoCombat refused ("), 0);
  EXPECT_NE(d.line.find('~'), std::string::npos);
  EXPECT_NE(d.line.find("); see the nevr log"), std::string::npos);
}

TEST(CargoSessionPin, Row10LeavesWhenTheServerHasNoSet) {
  const ncp::CargoDecision d = ncp::DecideCargo(View(), Level(), ncp::kCargoPinNone);
  EXPECT_TRUE(d.leave);
  EXPECT_EQ(d.reason, "server_none");
  EXPECT_EQ(d.line, "This server has no EchoCombat content: join another match");
}

TEST(CargoSessionPin, Row11LeavesWhenTheSetIsNotDownloadedYet) {
  ncp::CargoView v = View();
  v.status.state = ncp::CargoBoot::kNotWhole;
  const ncp::CargoDecision d = ncp::DecideCargo(v, Level(), kManifest);
  EXPECT_TRUE(d.leave);
  EXPECT_EQ(d.reason, "not_downloaded");
  EXPECT_EQ(d.line, "EchoCombat not downloaded yet: restart to play this match");
}

TEST(CargoSessionPin, Row11aLeavesWhenTheCdnCopyIsUndeliverable) {
  ncp::CargoView v = View();
  v.status.state = ncp::CargoBoot::kNotWhole;
  v.fetch = ncp::CargoFetch::kUndeliverable;
  const ncp::CargoDecision d = ncp::DecideCargo(v, Level(), kManifest);
  EXPECT_TRUE(d.leave);
  EXPECT_EQ(d.reason, "undeliverable");
  EXPECT_EQ(d.line, "EchoCombat could not be downloaded: play another mode for now");
}

TEST(CargoSessionPin, Row11bLeavesWhenThereIsNoSpace) {
  ncp::CargoView v = View();
  v.status.state = ncp::CargoBoot::kNotWhole;
  v.fetch = ncp::CargoFetch::kNoSpace;
  const ncp::CargoDecision d = ncp::DecideCargo(v, Level(), kManifest);
  EXPECT_TRUE(d.leave);
  EXPECT_EQ(d.reason, "no_space");
  EXPECT_EQ(d.line, "No disk space for EchoCombat: free some space, then restart");
}

TEST(CargoSessionPin, Row12LeavesWhenTheSetWasUpdated) {
  const ncp::CargoDecision d = ncp::DecideCargo(View(), Level(), kManifest);
  EXPECT_TRUE(d.leave);
  EXPECT_EQ(d.reason, "updated");
  EXPECT_EQ(d.line, "EchoCombat was updated: restart the game to play this match");
}

TEST(CargoSessionPin, Row13LeavesWhenTheServerIsBehind) {
  const ncp::CargoDecision d = ncp::DecideCargo(View(), Level(), kServer);
  EXPECT_TRUE(d.leave);
  EXPECT_EQ(d.reason, "server_behind");
  EXPECT_EQ(d.line, "This server's EchoCombat is out of date: join another match");
}

TEST(CargoSessionPin, Row14LeavesUncheckedAndWritesTheMarker) {
  ncp::CargoView v = View();
  v.manifest_known = false;
  const ncp::CargoDecision d = ncp::DecideCargo(v, Level(), kServer);
  EXPECT_TRUE(d.leave);
  EXPECT_TRUE(d.write_marker);
  EXPECT_EQ(d.reason, "unchecked");
  EXPECT_EQ(d.line, "EchoCombat differs from this server: restart the game to update");
}

TEST(CargoSessionPin, EveryNoticeLineFitsTheGameBlock) {
  std::vector<ncp::CargoDecision> all;
  ncp::CargoView v = View();
  v.status.state = ncp::CargoBoot::kOff;
  all.push_back(ncp::DecideCargo(v, Level(), kServer));
  v.status.state = ncp::CargoBoot::kRequiredOnlyOff;
  all.push_back(ncp::DecideCargo(v, Level(), kServer));
  for (const char* token : {"signed_only", "script_shadowed", "pack_hash", "script_undelivered"}) {
    all.push_back(ncp::DecideCargo(Refused(token), Level(), kServer));
  }
  all.push_back(ncp::DecideCargo(Refused(std::string(kLongTokenBytes, 'q')), Level(), kServer));
  all.push_back(ncp::DecideCargo(View(), Level(), ncp::kCargoPinNone));
  ncp::CargoView not_whole = View();
  not_whole.status.state = ncp::CargoBoot::kNotWhole;
  all.push_back(ncp::DecideCargo(not_whole, Level(), kManifest));
  for (const ncp::CargoFetch fetch : {ncp::CargoFetch::kUndeliverable, ncp::CargoFetch::kNoSpace}) {
    not_whole.fetch = fetch;
    all.push_back(ncp::DecideCargo(not_whole, Level(), kManifest));
  }
  all.push_back(ncp::DecideCargo(View(), Level(), kManifest));
  all.push_back(ncp::DecideCargo(View(), Level(), kServer));
  ncp::CargoView unknown = View();
  unknown.manifest_known = false;
  all.push_back(ncp::DecideCargo(unknown, Level(), kServer));
  for (const ncp::CargoDecision& d : all) {
    EXPECT_TRUE(d.leave);
    EXPECT_LE(d.line.size(), kLineMax) << d.line;
  }
}
