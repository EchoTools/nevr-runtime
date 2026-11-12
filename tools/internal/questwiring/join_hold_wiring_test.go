package questwiring

import (
	"strings"
	"testing"
)

// #1737 (review G1, F-1): the join hold's two call sites in src/runtime/server/join_hold.cpp are checked by source, because no test
// target links that file. Install arms the hold only when ArmHooks reports kArmed, and ReleaseDue hands Deliver the real answer to
// "can forward" (a broadcaster and its send function), never a constant. The host test test_join_hold drives the gate's rules; these
// pin what it cannot see.

const joinHoldSource = "src/runtime/server/join_hold.cpp"

// squash turns every run of white space into one space, so a call a formatter breaks across lines reads as one line.
func squash(s string) string {
	return strings.Join(strings.Fields(s), " ")
}

func TestInstallArmsTheHoldOnlyWhenArmHooksReportsArmed(t *testing.T) {
	t.Parallel()
	install := squash(body(t, stripComments(read(t, joinHoldSource)), "void Install(bool isServer)"))
	if n := strings.Count(install, "ArmHooks("); n != 1 {
		t.Fatalf("Install calls ArmHooks %d times, want once", n)
	}
	const gate = "if (ArmHooks(hooker) != ArmOutcome::kArmed) return;"
	if !strings.Contains(install, gate) {
		t.Errorf("Install does not return when ArmHooks is not kArmed: want %q (the hold would arm after a prologue mismatch or a failed detour)", gate)
	}
	before(t, install, gate, "g_active.store(true")
}

func TestReleaseDueHandsDeliverTheRealCanForward(t *testing.T) {
	t.Parallel()
	release := squash(body(t, stripComments(read(t, joinHoldSource)), "void ReleaseDue("))
	if n := strings.Count(release, "Deliver("); n != 1 {
		t.Fatalf("ReleaseDue calls Deliver %d times, want once", n)
	}
	const call = "Deliver(due, now, broadcaster != nullptr && EchoVR::BroadcasterReceiveLocalEvent != nullptr, sink);"
	if !strings.Contains(release, call) {
		t.Errorf("ReleaseDue does not call %q: canForward must be the broadcaster and its send function, not a constant", call)
	}
}
