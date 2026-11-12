package questwiring

import (
	"strings"
	"testing"
)

// #2180: the three headset gates are the runtime's. The host tests (headset_gates_test, combat_gates_test, combat_install_test) and
// check/headset_gates.sh drive the behaviour and the arm64 entries; these pin what they cannot see on the host: that each continuation reads the word
// the manager publishes with an acquire load (the x86 build cannot tell a relaxed one), that the install hands the manager that very word, that the
// entries are reviewed and are the only assembly, and that the plugin installs on neither slot.

func TestEachContinuationReadsItsPublishedWordWithAnAcquireLoadAndTheInstallPublishesThatWord(t *testing.T) {
	code := stripComments(read(t, "src/quest/combat/headset_gates.cpp"))
	for _, c := range []struct{ fn, word, entry string }{
		{"NextParty", "g_partyNext", "InstallAsmEntry(g_partyHook"},
		{"NextHeadset", "g_headsetNext", "InstallAsmEntry(g_headsetHook"},
	} {
		b := body(t, code, "std::int32_t "+c.fn+"(")
		if !strings.Contains(b, "__atomic_load_n(&"+c.word+", __ATOMIC_ACQUIRE)") {
			t.Errorf("%s must read %s with __atomic_load_n(..., __ATOMIC_ACQUIRE) (ldar on arm64), got %q", c.fn, c.word, b)
		}
		if strings.Contains(b, "__ATOMIC_RELAXED") || strings.Contains(b, c.word+";") {
			t.Errorf("%s must not read %s any other way, got %q", c.fn, c.word, b)
		}
		at := strings.Index(code, c.entry)
		if at < 0 || !strings.Contains(code[at:at+260], "&"+c.word) {
			t.Errorf("the install must hand the manager &%s as the entry's next word (%s)", c.word, c.entry)
		}
	}
}

func TestTheOnlyHooksAreTheTwoReviewedEntriesAndTheSensorListsBoth(t *testing.T) {
	code := stripComments(read(t, "src/quest/combat/headset_gates.cpp"))
	for _, forbidden := range []string{"InstallThunk", "NEVR_HOOK_RECORD", "GotDataSlot", "mprotect", "mmap", "PROT_WRITE"} {
		if strings.Contains(code, forbidden) {
			t.Errorf("headset_gates.cpp must install only through InstallAsmEntry and write no page; it names %s", forbidden)
		}
	}
	if n := strings.Count(code, "NEVR_ASM_ENTRY_RECORD("); n != 2 {
		t.Errorf("want exactly two NEVR_ASM_ENTRY_RECORD, got %d", n)
	}
	sensor := read(t, "tests/quest/frames_sensor_test.go")
	for _, name := range []string{"nevr_combat_headset_tramp", "nevr_combat_party_tramp"} {
		if !strings.Contains(sensor, "`^"+name+"$`") {
			t.Errorf("tests/quest/frames_sensor_test.go reviewedAsmEntries must list %s", name)
		}
	}
}

func TestTheEntriesAreTopLevelAsmWithCFIAndNeverNaked(t *testing.T) {
	inc := read(t, "src/quest/combat/headset_gates_entry.inc")
	asm := stripComments(inc)
	if strings.Contains(asm, "naked") {
		t.Error("an assembly entry is a top-level asm() block, never an __attribute__((naked)) function")
	}
	for _, name := range []string{"nevr_combat_headset_tramp", "nevr_combat_party_tramp"} {
		at := strings.Index(asm, name+":")
		if at < 0 {
			t.Fatalf("no %s label", name)
		}
		end := strings.Index(asm[at:], ".size")
		entry := asm[at : at+end]
		for _, want := range []string{".cfi_startproc", "hint #34", "\n  b nevr_combat_", ".cfi_endproc"} {
			if !strings.Contains(entry, want) {
				t.Errorf("%s must contain %q, got:\n%s", name, want, entry)
			}
		}
		if strings.Contains(entry, "bl ") || strings.Contains(entry, "blr") {
			t.Errorf("%s acts on every call and tail-branches; it must not call, got:\n%s", name, entry)
		}
	}
	cpp := stripComments(read(t, "src/quest/combat/headset_gates.cpp"))
	if !strings.Contains(cpp, `#include "quest/combat/headset_gates_entry.inc"`) {
		t.Error("headset_gates.cpp must build the entries from the .inc the qemu check runs")
	}
}

func TestTheGatesRegisterLastInTheNormalTable(t *testing.T) {
	table := stripComments(read(t, "src/quest/combat/combat_install.cpp"))
	if !strings.Contains(table, `Module{"gates", nullptr, &GatesNormal}`) {
		t.Error("the gates must be a normal-step row of the registration table")
	}
	tick := strings.Index(table, `Module{"tick"`)
	gates := strings.Index(table, `Module{"gates"`)
	if tick < 0 || gates < tick {
		t.Error("the gates row must come after the tick row: the bot gate registers a tick callback")
	}
}

func TestEveryGateKeyIsOnTheSchemaTheExampleAndTheConfigLine(t *testing.T) {
	schema := read(t, "src/core/nevr_config.cpp")
	example := read(t, "docs/reference/example-config.yaml")
	line := read(t, "src/quest/combat/combat_install.cpp")
	for _, key := range []string{"gate_perm", "gate_bots", "gate_link", "gate_link_libs"} {
		if !strings.Contains(schema, `"`+key+`"`) {
			t.Errorf("KnownSections must list combat.%s", key)
		}
		if !strings.Contains(example, "  # "+key+":") {
			t.Errorf("the example config must document combat.%s", key)
		}
		if !strings.Contains(line, `{"`+key+`_source"`) && !strings.Contains(line, `{"`+key+`", config.`) {
			t.Errorf("combat_config_gates must report combat.%s", key)
		}
	}
}
