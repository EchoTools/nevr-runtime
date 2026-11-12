package serverwiring

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// #1737 (review G1, F-1; review G1fix2, F-1): the join hold's two call sites in src/runtime/server/join_hold.cpp are checked by
// source, because no test target links that file. Install arms the hold only when ArmHooks reports kArmed, and ReleaseDue hands
// Deliver the real answer to "can forward" (a broadcaster and its send function), never a constant. The host test test_join_hold
// drives the gate's rules; these pin what it cannot see. The helpers are the ones the sibling wiring packages carry (root, read,
// stripComments, body, before); no package holds them in common, so each keeps its own copy.

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

func root(t *testing.T) string {
	t.Helper()
	wd, err := os.Getwd()
	if err != nil {
		t.Fatal(err)
	}
	r := filepath.Join(wd, "..", "..", "..")
	if _, err := os.Stat(filepath.Join(r, "tools", "go.mod")); err != nil {
		t.Fatalf("%s is not the repository root: %v", r, err)
	}
	return r
}

func read(t *testing.T, path string) string {
	t.Helper()
	b, err := os.ReadFile(filepath.Join(root(t), path))
	if err != nil {
		t.Fatal(err)
	}
	return string(b)
}

var blockComment = regexp.MustCompile(`(?s)/\*.*?\*/`)
var lineComment = regexp.MustCompile(`//[^\n]*`)

func stripComments(s string) string {
	return lineComment.ReplaceAllString(blockComment.ReplaceAllString(s, ""), "")
}

// body returns the text of the braced function whose signature starts with sig.
func body(t *testing.T, code, sig string) string {
	t.Helper()
	at := strings.Index(code, sig)
	if at < 0 {
		t.Fatalf("no %q", sig)
	}
	open := strings.Index(code[at:], "{")
	depth := 0
	for i := at + open; i < len(code); i++ {
		switch code[i] {
		case '{':
			depth++
		case '}':
			depth--
			if depth == 0 {
				return code[at : i+1]
			}
		}
	}
	t.Fatalf("unbalanced %q", sig)
	return ""
}

func before(t *testing.T, s, a, b string) {
	t.Helper()
	ia, ib := strings.Index(s, a), strings.Index(s, b)
	if ia < 0 || ib < 0 || ia >= ib {
		t.Errorf("want %q before %q (at %d and %d)", a, b, ia, ib)
	}
}
