// Host test of combat_gates.h (#2180): which of the three headset gates (permissions, bot body, menu) a run installs, from the `combat:` keys
// gate_perm, gate_bots and gate_link. Moved from the q15t plugin's combat_gates_test: a key set to false replaces the build option and
// the kill file as the thing that turns a gate off, so a fixture's "not built" and "off file" are one input here (the reason token is off_config);
// and no gate depends on another key. The plugin's assertion that levelname off turns the bot gate off is in headset_gates_test (TheBotGateNeedsLevelnameInstalledInTheGame), where it is the game's state that decides.
//
// Built and run by `just test-quest-combat`.
#include <cstring>

#include "quest/combat/combat_gates.h"
#include "quest/tests/test_check.h"

namespace {

namespace gates = nevr_quest::combat::gates;

bool Eq(const char* a, const char* b) { return std::strcmp(a, b) == 0; }

// The shipped defaults: every key true. All three installed, none with a reason.
void TheShippedDefaultsInstallAllThree() {
  const gates::Choice c = gates::Decide({true, true, true});
  QCHECK(c.perm && c.bots && c.link);
  QCHECK(Eq(c.permReason, "") && Eq(c.botsReason, "") && Eq(c.linkReason, ""));  // an installed gate carries no reason
}

// Each key turns its own gate off, and nothing else.
void EachKeyTurnsItsOwnGateOff() {
  {
    const gates::Choice c = gates::Decide({false, true, true});
    QCHECK(!c.perm && Eq(c.permReason, "off_config") && c.bots && c.link);
  }
  {
    const gates::Choice c = gates::Decide({true, false, true});
    QCHECK(c.perm && !c.bots && Eq(c.botsReason, "off_config") && c.link);
  }
  {
    const gates::Choice c = gates::Decide({true, true, false});
    QCHECK(c.perm && c.bots && !c.link && Eq(c.linkReason, "off_config"));
  }
}

// Nothing asked: all off, every reason the key.
void NothingAskedIsAllOff() {
  const gates::Choice c = gates::Decide({false, false, false});
  QCHECK(!c.perm && !c.bots && !c.link);
  QCHECK(Eq(c.permReason, "off_config") && Eq(c.botsReason, "off_config") && Eq(c.linkReason, "off_config"));
}

}  // namespace

int main() {
  TheShippedDefaultsInstallAllThree();
  EachKeyTurnsItsOwnGateOff();
  NothingAskedIsAllOff();
  if (quest_test::Failures() == 0) std::printf("combat_gates_test: all checks pass\n");
  return quest_test::Failures() == 0 ? 0 : 1;
}
