package questwiring

import (
	"strings"
	"testing"
)

// #2179: combat::NoteProcessEnd writes each combat module's final counts (sendguard's combat_sendguard_stats). The host tests drive it through
// the shipped table; this pins the production call that reaches it, which they cannot see: the process-end callback calls the forward, the
// forward calls the combat area's function, and the call comes after the not-started record (a step that never ran has nothing to report).
func TestTheProcessEndCallbackReachesTheCombatModulesFinalCounts(t *testing.T) {
	end := body(t, stripComments(read(t, "src/quest/integration/production_steps.cpp")), "void OnGameProcessEnd()")
	if strings.Count(end, "nevr_quest::integration::NoteCombatProcessEnd();") != 1 {
		t.Errorf("OnGameProcessEnd does not call NoteCombatProcessEnd exactly once:\n%s", end)
	}
	before(t, end, "NoteCombatNotStarted(", "NoteCombatProcessEnd(")
	forward := body(t, stripComments(read(t, "src/quest/sentinel/entry.cpp")), "void NoteCombatProcessEnd()")
	if !strings.Contains(forward, "nevr_quest::combat::NoteProcessEnd();") {
		t.Errorf("the forward in entry.cpp does not call combat::NoteProcessEnd: %s", forward)
	}
	if !strings.Contains(stripComments(read(t, "src/quest/integration/entry_hooks.h")), "void NoteCombatProcessEnd() noexcept;") {
		t.Error("entry_hooks.h does not declare NoteCombatProcessEnd")
	}
	table := stripComments(read(t, "src/quest/combat/combat_install.cpp"))
	if !strings.Contains(table, "&sendguard::NoteEnd") {
		t.Error("sendguard's row has no end step")
	}
}
