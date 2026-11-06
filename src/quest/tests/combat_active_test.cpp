// Host test for the gate on the folded combat modules (#2170, docs/adr/0046 rule 5): `combat::Active()` is true for the `q15t` and
// `combat` variants only, a development build may force it with combat.force, a release build may not, and the decision is logged
// once with its reason. The rule is tested as a table over every variant, both build kinds and both force answers, then through the
// process state a hook reads (Resolve, Active), so a variant read wrong shows here: a plain release turning combat on, or a combat APK
// running without it.
//
// Built and run by `just test-quest-combat`.

#include "quest/combat/combat_active.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "hook_log.h"
#include "quest/tests/test_check.h"

namespace {

namespace bv = nevr_quest::build_variant;
namespace combat = nevr_quest::combat;

struct Line {
  sentinel::LogLevel level;
  std::string text;
};
std::vector<Line> g_lines;
void Capture(sentinel::LogLevel level, const char* line) { g_lines.push_back({level, line}); }

bool Has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

constexpr bv::Variant kVariants[] = {bv::Variant::kNone, bv::Variant::kQ15t, bv::Variant::kCombat, bv::Variant::kPerfProbe};

// The whole table, spelled out (not derived from the rule): variant, development build, force asked -> active, reason, force outcome.
struct Row {
  bv::Variant variant;
  bool dev;
  bool force;
  bool active;
  combat::Reason reason;
  combat::ForceOutcome outcome;
};
using R = combat::Reason;
using F = combat::ForceOutcome;
const Row kTable[] = {
    // a plain release, or no stamp: no combat unless a development build forces it
    {bv::Variant::kNone, false, false, false, R::kNotACombatVariant, F::kNotAsked},
    {bv::Variant::kNone, false, true, false, R::kNotACombatVariant, F::kRefused},
    {bv::Variant::kNone, true, false, false, R::kNotACombatVariant, F::kNotAsked},
    {bv::Variant::kNone, true, true, true, R::kForced, F::kHonoured},
    // the q15t variant carries combat in a release build and in a development build; force changes nothing
    {bv::Variant::kQ15t, false, false, true, R::kVariantCarriesCombat, F::kNotAsked},
    {bv::Variant::kQ15t, false, true, true, R::kVariantCarriesCombat, F::kRefused},
    {bv::Variant::kQ15t, true, false, true, R::kVariantCarriesCombat, F::kNotAsked},
    {bv::Variant::kQ15t, true, true, true, R::kVariantCarriesCombat, F::kRedundant},
    // the combat variant
    {bv::Variant::kCombat, false, false, true, R::kVariantCarriesCombat, F::kNotAsked},
    {bv::Variant::kCombat, false, true, true, R::kVariantCarriesCombat, F::kRefused},
    {bv::Variant::kCombat, true, false, true, R::kVariantCarriesCombat, F::kNotAsked},
    {bv::Variant::kCombat, true, true, true, R::kVariantCarriesCombat, F::kRedundant},
    // the measurement build is no combat variant
    {bv::Variant::kPerfProbe, false, false, false, R::kNotACombatVariant, F::kNotAsked},
    {bv::Variant::kPerfProbe, false, true, false, R::kNotACombatVariant, F::kRefused},
    {bv::Variant::kPerfProbe, true, false, false, R::kNotACombatVariant, F::kNotAsked},
    {bv::Variant::kPerfProbe, true, true, true, R::kForced, F::kHonoured},
};

// The state of the process before anything has been set or reset: nothing is active and the reason is "unresolved". In production this
// rests on the initial values alone (a hook that asks before the configuration step must read combat as off in every APK), so this
// runs first in main(), before any ResetForTest().
void NothingIsActiveAtProcessStart() {
  QCHECK(!combat::Active());
  QCHECK(combat::ActiveReason() == R::kUnresolved);
}

void TheTableCoversEveryVariantBuildKindAndForceAnswer() {
  QCHECK(sizeof(kTable) / sizeof(kTable[0]) == 4U * 2U * 2U);
  for (const Row& row : kTable) {
    const combat::Decision d = combat::Decide(row.variant, row.dev, row.force);
    if (d.active != row.active || d.reason != row.reason || d.force != row.outcome) {
      std::fprintf(stderr, "variant=%s dev=%d force=%d: active=%d reason=%s force=%s, wanted active=%d reason=%s force=%s\n",
                   bv::VariantName(row.variant), row.dev, row.force, d.active, combat::ReasonName(d.reason),
                   combat::ForceName(d.force), row.active, combat::ReasonName(row.reason), combat::ForceName(row.outcome));
      ++quest_test::Failures();
    }
  }
}

// A release build never honours force: no variant is active because of it, and the outcome is never "honoured".
void AReleaseBuildNeverHonoursForce() {
  for (const bv::Variant variant : kVariants) {
    const combat::Decision d = combat::Decide(variant, /*dev=*/false, /*force=*/true);
    QCHECK(d.force == F::kRefused);
    QCHECK(d.reason != R::kForced);
    QCHECK(d.active == bv::RunsFoldedCombat(variant));
  }
}

// Without force, the variant alone decides, in either build kind.
void WithoutForceOnlyTheVariantDecides() {
  for (const bv::Variant variant : kVariants) {
    for (const bool dev : {false, true}) {
      QCHECK(combat::Decide(variant, dev, false).active == bv::RunsFoldedCombat(variant));
    }
  }
}

void TheNamesAreStableTokens() {
  QCHECK(std::strcmp(combat::ReasonName(R::kVariantCarriesCombat), "variant") == 0);
  QCHECK(std::strcmp(combat::ReasonName(R::kForced), "forced") == 0);
  QCHECK(std::strcmp(combat::ReasonName(R::kNotACombatVariant), "not_a_combat_variant") == 0);
  QCHECK(std::strcmp(combat::ReasonName(R::kUnresolved), "unresolved") == 0);
  QCHECK(std::strcmp(combat::ForceName(F::kNotAsked), "not_asked") == 0);
  QCHECK(std::strcmp(combat::ForceName(F::kHonoured), "honoured") == 0);
  QCHECK(std::strcmp(combat::ForceName(F::kRedundant), "redundant") == 0);
  QCHECK(std::strcmp(combat::ForceName(F::kRefused), "refused") == 0);
}

// ---- the process state a hook reads ------------------------------------------------------------------------------------

void NothingIsActiveBeforeTheConfigurationIsResolved() {
  combat::ResetForTest();
  QCHECK(!combat::Active());
  QCHECK(combat::ActiveReason() == R::kUnresolved);
  combat::SetVariant(bv::Variant::kCombat);  // the variant is known from the first line, but the answer needs the configuration
  QCHECK(!combat::Active());
}

void EachVariantResolvesToItsAnswerAndIsLoggedOnce() {
  for (const Row& row : kTable) {
    combat::ResetForTest();
    g_lines.clear();
    const sentinel::LogSink previous = sentinel::SetLogSink(&Capture);
    combat::SetVariant(row.variant);
    combat::Resolve(row.dev, row.force);
    combat::Resolve(!row.dev, !row.force);  // a second resolve changes nothing and says nothing
    sentinel::SetLogSink(previous);
    QCHECK(combat::Active() == row.active);
    QCHECK(combat::ActiveReason() == row.reason);
    QCHECK(g_lines.size() == 1U);
    if (g_lines.size() != 1U) continue;
    const Line& line = g_lines[0];
    QCHECK(Has(line.text, "\"event\":\"combat_active\""));
    QCHECK(Has(line.text, (std::string("\"variant\":\"") + bv::VariantName(row.variant) + "\"").c_str()));
    QCHECK(Has(line.text, (std::string("\"kind\":\"") + (row.dev ? "dev" : "release") + "\"").c_str()));
    QCHECK(Has(line.text, (std::string("\"active\":\"") + (row.active ? "true" : "false") + "\"").c_str()));
    QCHECK(Has(line.text, (std::string("\"reason\":\"") + combat::ReasonName(row.reason) + "\"").c_str()));
    QCHECK(Has(line.text, (std::string("\"force\":\"") + combat::ForceName(row.outcome) + "\"").c_str()));
    // the line says what follows from it
    QCHECK(Has(line.text, row.active ? "folded combat modules install in this run" : "folded combat modules do not install"));
    // a refused force is the one Warning; everything else is Info
    QCHECK(line.level == (row.outcome == F::kRefused ? sentinel::LogLevel::kWarn : sentinel::LogLevel::kInfo));
    if (row.outcome == F::kRefused) QCHECK(Has(line.text, "combat.force is honoured in development builds only"));
  }
}

// A variant set after the answer was given does not move it: the answer is one per process.
void TheAnswerIsOnePerProcess() {
  combat::ResetForTest();
  const sentinel::LogSink previous = sentinel::SetLogSink(&Capture);
  combat::SetVariant(bv::Variant::kNone);
  combat::Resolve(false, false);
  combat::SetVariant(bv::Variant::kCombat);
  sentinel::SetLogSink(previous);
  QCHECK(!combat::Active());
}

// Resolving without ever having been told a variant is a plain release.
void NoVariantSetIsAPlainRelease() {
  combat::ResetForTest();
  const sentinel::LogSink previous = sentinel::SetLogSink(&Capture);
  combat::Resolve(false, false);
  sentinel::SetLogSink(previous);
  QCHECK(!combat::Active());
  QCHECK(combat::ActiveReason() == R::kNotACombatVariant);
}

// The variant the combat_install lines name is the one SetVariant recorded, before and after the answer is made.
void TheRecordedVariantIsTheOneSet() {
  combat::ResetForTest();
  QCHECK(combat::RecordedVariant() == bv::Variant::kNone);
  for (const bv::Variant variant : kVariants) {
    combat::ResetForTest();
    const sentinel::LogSink previous = sentinel::SetLogSink(&Capture);
    combat::SetVariant(variant);
    QCHECK(combat::RecordedVariant() == variant);
    combat::Resolve(false, false);
    sentinel::SetLogSink(previous);
    QCHECK(combat::RecordedVariant() == variant);
  }
}

}  // namespace

int main() {
  NothingIsActiveAtProcessStart();  // first: before any ResetForTest()
  TheTableCoversEveryVariantBuildKindAndForceAnswer();
  AReleaseBuildNeverHonoursForce();
  WithoutForceOnlyTheVariantDecides();
  TheNamesAreStableTokens();
  NothingIsActiveBeforeTheConfigurationIsResolved();
  EachVariantResolvesToItsAnswerAndIsLoggedOnce();
  TheAnswerIsOnePerProcess();
  NoVariantSetIsAPlainRelease();
  TheRecordedVariantIsTheOneSet();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "combat_active_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::puts("combat_active_test: all checks passed");
  return 0;
}
