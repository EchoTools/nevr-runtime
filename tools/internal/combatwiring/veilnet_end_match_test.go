package combatwiring

import (
	"strings"
	"testing"
)

// The F8 END MATCH is gone from VeilNet (Cat's EchoCombat 2.0.2, f0e8142: "F8 END MATCH removed"). The PC port kept it;
// this pins that its capture, its tick step, its game-over hook, its F8 key and its client ask are not in veilnet.cpp.
// The MAP SWITCH buttons, which 2.0.2 made work on every server, are not gated by an answer here: the map ask keeps its
// own privacy gate (SessionPrivacy) and nothing else.
func TestVeilnetHasNoEndMatch(t *testing.T) {
	src := code(t, "src/runtime/combat/veilnet.cpp")
	for _, gone := range []string{
		"WANT_END", "EndMatchCapture", "EndMatchStep", "EndMatchRestore", "GameOverSeen", "EVT_GAME_OVER",
		"SYM_CAPTUREPOINT", "LooksLikePoint", "FindPointData", "g_end_until", "g_rules_total_ptr", "VK_F8", "f8_was",
	} {
		if strings.Contains(src, gone) {
			t.Errorf("veilnet.cpp still has %s: F8 END MATCH was removed in EchoCombat 2.0.2", gone)
		}
	}
}

// The map button's ask is answered by the privacy gate alone: the Handle branch for WANT_MAP names SessionPrivacy and
// the no-answer path (the server's own map switch) does not wait on an answer.
func TestVeilnetMapAskGatedByPrivacyOnly(t *testing.T) {
	src := code(t, "src/runtime/combat/veilnet.cpp")
	branch := body(t, src, "if (p.type == 'C' && (p.id & WANT_KIND_MASK) == WANT_MAP")
	if !strings.Contains(branch, "SessionPrivacy()") {
		t.Errorf("the MAP SWITCH ask no longer reads SessionPrivacy(): %s", branch)
	}
	if strings.Contains(branch, "g_last_answer") || strings.Contains(branch, "answer") {
		t.Errorf("the MAP SWITCH ask waits on an answer: %s", branch)
	}
}
