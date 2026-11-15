package combatwiring

import (
	"strings"
	"testing"
)

// #2242 (EchoCombat 2.0.3): a new session starts with the default RULES, and the clients get the reset with the next hello
// answers. test_veilnet_rules.cpp drives the header's defaults and the once-per-session watch; these pin that veilnet.cpp
// runs them: the server's datagram path notices a new session and resets, and the 'R' answer goes out even at 0.
func TestVeilnetResetsRulesOnANewSession(t *testing.T) {
	src := code(t, "src/runtime/combat/veilnet.cpp")
	reset := body(t, src, "void ResetRules()")
	if !strings.Contains(reset, "ApplyDefaults(g_rules)") {
		t.Errorf("ResetRules does not write the header's defaults into the RULES mailbox: %s", reset)
	}
	for _, want := range []string{"g_paused = PAUSE_OFF", "g_chassis = 0", "g_classic = LOADOUT_ALL", "g_pause_queued = 0"} {
		if !strings.Contains(reset, want) {
			t.Errorf("ResetRules does not reset %s", want)
		}
	}
	notice := body(t, src, "void NoticeNewSession()")
	if !strings.Contains(notice, "NewSessionSeen(") || !strings.Contains(notice, "SessionGeneration()") || !strings.Contains(notice, "ResetRules()") {
		t.Errorf("NoticeNewSession does not watch the session store's generation and reset on a new one: %s", notice)
	}
	handle := body(t, src, "void Handle(SOCKET s, const Packet &p, const sockaddr_in &from)")
	if !strings.Contains(handle, "NoticeNewSession();") {
		t.Errorf("Handle does not notice a new session on the server's datagram path")
	}
	if strings.Contains(src, "if (total) Send(s, from, 'R', total)") {
		t.Errorf("the 'R' answer is still skipped at 0: a new session's reset must reach the clients")
	}
	if !strings.Contains(handle, "if (g_rules_total) Send(s, from, 'R'") {
		t.Errorf("the 'R' answer is not sent whenever the RULES total exists")
	}
}
