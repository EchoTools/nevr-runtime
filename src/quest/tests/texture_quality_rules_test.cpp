// Host test for the Quest 2 step-down's rules (#1737, src/quest/sentinel/texture_quality_rules.h, docs/quest/combat/texture_quest2.md): the
// look's gate (settle, load window, gap, Echo Arena), the level sent (Quest 2, the player's choice, the look) and when a look-driven Resize may be
// made (a fresh look with no load, 10 s quiet, the upload count and lock at 0). Pure: no game, no hook, no clock of its own.
//
// Built and run by `just test-quest-hooks`.

#include "texture_quality_rules.h"

#include <cstdint>
#include <cstdio>

#include "quest/tests/test_check.h"

namespace {

namespace tq = nevr_quest::texture_quality;
namespace rules = nevr_quest::texture_quality::rules;
using rules::Where;

rules::Look LookAt(Where where, bool loading, std::int64_t atMs) {
  rules::Look look;
  look.where = where;
  look.loading = loading;
  look.atMs = atMs;
  return look;
}

// Steps the gate every 500 ms from `fromMs` to `toMs` (inclusive) with the same look, as the poller and the tick do.
rules::LookGate Run(rules::LookGate gate, Where where, bool loading, std::int64_t fromMs, std::int64_t toMs, bool lobby = true) {
  rules::StepDownSwitches switches;
  switches.lobby = lobby;
  for (std::int64_t t = fromMs; t <= toMs; t += 500) gate = rules::LookStep(gate, LookAt(where, loading, t), switches, t);
  return gate;
}

void ACombatMapTurnsTheLookOnOnlyAfterTenQuietSeconds() {
  rules::LookGate gate = Run({}, Where::kCombatMap, false, 1000, 10500);  // 9.5 s in the map
  QCHECK(!gate.on);
  QCHECK(gate.reason == rules::RuleReason::kNotInCombat);
  gate = Run(gate, Where::kCombatMap, false, 11000, 11000);  // 10 s
  QCHECK(gate.on);
  QCHECK(gate.reason == rules::RuleReason::kQuest2Combat);
}

void ALoadRestartsTheSettle() {
  rules::LookGate gate = Run({}, Where::kCombatMap, false, 1000, 9000);
  gate = Run(gate, Where::kCombatMap, true, 9500, 9500);  // a sub-level loads
  QCHECK(gate.quietSinceMs == 0);
  gate = Run(gate, Where::kCombatMap, false, 10000, 19500);  // 9.5 s quiet: the place is old enough, the quiet is not
  QCHECK(!gate.on);
  gate = Run(gate, Where::kCombatMap, false, 20000, 20000);  // 10 s quiet
  QCHECK(gate.on);
}

void AStaleLookSaysNothingAndIsNotQuiet() {
  rules::LookGate gate = Run({}, Where::kCombatMap, false, 1000, 9000);
  const rules::StepDownSwitches switches;
  gate = rules::LookStep(gate, LookAt(Where::kCombatMap, false, 9000), switches, 9000 + rules::kLookFreshMs + 1);
  QCHECK(gate.quietSinceMs == 0);
  QCHECK(!gate.on);
  gate = rules::LookStep(gate, LookAt(Where::kCombatMap, false, 0), switches, 20000);  // no look yet
  QCHECK(!gate.on && gate.quietSinceMs == 0);
}

void TheCombatLobbyCountsOnlyWithTheLobbySwitch() {
  QCHECK(Run({}, Where::kCombatLobby, false, 1000, 12000, /*lobby=*/true).on);
  QCHECK(!Run({}, Where::kCombatLobby, false, 1000, 60000, /*lobby=*/false).on);
  QCHECK(!Run({}, Where::kNone, false, 1000, 60000).on);
  QCHECK(!Run({}, Where::kEchoArena, false, 1000, 60000).on);
}

void EchoArenaTurnsTheLookOffAfterItSettles() {
  rules::LookGate gate = Run({}, Where::kCombatMap, false, 1000, 12000);
  QCHECK(gate.on);
  gate = Run(gate, Where::kEchoArena, false, 12500, 22000);  // 9.5 s
  QCHECK(gate.on);
  gate = Run(gate, Where::kEchoArena, false, 22500, 22500);  // 10 s
  QCHECK(!gate.on);
  QCHECK(gate.reason == rules::RuleReason::kArena);
}

void ALoadGapKeepsTheLookOnUntilNinetySeconds() {
  rules::LookGate gate = Run({}, Where::kCombatMap, false, 1000, 12000);
  QCHECK(gate.on);
  gate = Run(gate, Where::kNone, true, 12500, 12500 + rules::kGapMs - 500);  // a lobby the look does not cover, loading
  QCHECK(gate.on);
  gate = Run(gate, Where::kNone, false, 12500 + rules::kGapMs, 12500 + rules::kGapMs);
  QCHECK(!gate.on);
  QCHECK(gate.reason == rules::RuleReason::kNoCombatLevel);
  // A lobby -> map round trip inside the gap keeps it on throughout.
  gate = Run({}, Where::kCombatMap, false, 1000, 12000);
  gate = Run(gate, Where::kNone, true, 12500, 40000);
  gate = Run(gate, Where::kCombatMap, true, 40500, 60000);
  QCHECK(gate.on);
}

rules::StepDownInputs Inputs() {
  rules::StepDownInputs in;
  in.requested = tq::Level::kLow;
  in.registered = true;
  in.quest2 = true;
  in.gate.on = true;
  in.gate.reason = rules::RuleReason::kQuest2Combat;
  return in;
}

void TheLevelSentIsVeryLowOnlyOnAQuest2InCombatWithoutAChoice() {
  rules::StepDownInputs in = Inputs();
  rules::Effective e = rules::EffectiveLevel(in);
  QCHECK(e.level == tq::Level::kVeryLow && e.reason == rules::RuleReason::kQuest2Combat);
  in = Inputs();
  in.registered = false;
  e = rules::EffectiveLevel(in);
  QCHECK(e.level == tq::Level::kLow && e.reason == rules::RuleReason::kOff);
  in = Inputs();
  in.quest2 = false;
  e = rules::EffectiveLevel(in);
  QCHECK(e.level == tq::Level::kLow && e.reason == rules::RuleReason::kNotQuest2);
  in = Inputs();
  in.playerChose = true;
  e = rules::EffectiveLevel(in);
  QCHECK(e.level == tq::Level::kLow && e.reason == rules::RuleReason::kPlayerChoice);
  in = Inputs();
  in.lookOffForRun = true;
  e = rules::EffectiveLevel(in);
  QCHECK(e.level == tq::Level::kLow && e.reason == rules::RuleReason::kLookOff);
  in = Inputs();
  in.gate.on = false;
  in.gate.reason = rules::RuleReason::kArena;
  e = rules::EffectiveLevel(in);
  QCHECK(e.level == tq::Level::kLow && e.reason == rules::RuleReason::kArena);
  QCHECK(rules::kStepDownLevel == tq::Level::kVeryLow);  // menu index 0, never `stock` (Level 0)
}

rules::ResizeInputs Quiet() {
  rules::ResizeInputs in;
  in.nowMs = 50000;
  in.look = LookAt(Where::kCombatMap, false, 49800);
  in.quietSinceMs = 50000 - rules::kSettleMs;
  in.uploads = 0;
  in.lock = 0;
  return in;
}

void AResizeIsAllowedOnlyInAQuietWindowWithNoUpload() {
  QCHECK(rules::ResizeAllowed(Quiet()));
  rules::ResizeInputs in = Quiet();
  in.uploads = 1;
  QCHECK(rules::ResizeHold(in) == rules::Hold::kUploads);
  in = Quiet();
  in.lock = 1;
  QCHECK(rules::ResizeHold(in) == rules::Hold::kLock);
  in = Quiet();
  in.look.loading = true;
  QCHECK(rules::ResizeHold(in) == rules::Hold::kLoading);
  in = Quiet();
  in.quietSinceMs = in.nowMs - rules::kSettleMs + 1;
  QCHECK(rules::ResizeHold(in) == rules::Hold::kSettling);
  in = Quiet();
  in.quietSinceMs = 0;
  QCHECK(rules::ResizeHold(in) == rules::Hold::kSettling);
  in = Quiet();
  in.look.atMs = in.nowMs - rules::kLookFreshMs - 1;
  QCHECK(rules::ResizeHold(in) == rules::Hold::kStaleLook);
  in = Quiet();
  in.look.atMs = 0;
  QCHECK(rules::ResizeHold(in) == rules::Hold::kStaleLook);
}

void ALoadTheTickSawBetweenTwoPollerSamplesBlocksForTenSeconds() {
  // The poller's quiet stretch says ten quiet seconds, but the tick saw a load 4 s ago that the poller never sampled.
  rules::ResizeInputs in = Quiet();
  in.look.lastLoadMs = in.nowMs - 4000;
  QCHECK(rules::ResizeHold(in) == rules::Hold::kSettling);
  in.look.lastLoadMs = in.nowMs - rules::kSettleMs + 1;
  QCHECK(rules::ResizeHold(in) == rules::Hold::kSettling);
  in.look.lastLoadMs = in.nowMs - rules::kSettleMs;
  QCHECK(rules::ResizeHold(in) == rules::Hold::kNone);  // exactly ten seconds: settled
  in.look.lastLoadMs = 0;                               // never saw one
  QCHECK(rules::ResizeHold(in) == rules::Hold::kNone);
  // The stricter of the two wins: the poller's stretch is short, the tick's load is old.
  in = Quiet();
  in.look.lastLoadMs = in.nowMs - 60000;
  in.quietSinceMs = in.nowMs - 3000;
  QCHECK(rules::ResizeHold(in) == rules::Hold::kSettling);
}

void TheConstantsAreTheDocumentedOnes() {
  QCHECK(rules::kSettleMs == 10000);
  QCHECK(rules::kGapMs == 90000);
  QCHECK(rules::kLookFreshMs == 1500);
}

}  // namespace

int main() {
  ACombatMapTurnsTheLookOnOnlyAfterTenQuietSeconds();
  ALoadRestartsTheSettle();
  AStaleLookSaysNothingAndIsNotQuiet();
  TheCombatLobbyCountsOnlyWithTheLobbySwitch();
  EchoArenaTurnsTheLookOffAfterItSettles();
  ALoadGapKeepsTheLookOnUntilNinetySeconds();
  TheLevelSentIsVeryLowOnlyOnAQuest2InCombatWithoutAChoice();
  AResizeIsAllowedOnlyInAQuietWindowWithNoUpload();
  ALoadTheTickSawBetweenTwoPollerSamplesBlocksForTenSeconds();
  TheConstantsAreTheDocumentedOnes();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "texture_quality_rules_test: %d failure(s)\n", quest_test::Failures());
    return 1;
  }
  std::printf("texture_quality_rules_test: ok\n");
  return 0;
}
