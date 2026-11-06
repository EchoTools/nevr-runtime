package questwiring

import (
	"strings"
	"testing"
)

// #2187: dronekill and dronemuzzle are the runtime's. The host tests (dronekill_test, dronemuzzle_test) and check/dronemuzzle.sh drive the
// behaviour and the arm64 entries; these pin what they cannot see on the host: that each continuation reads the next word the manager publishes
// with an acquire load and the install hands the manager that very word, that the entries are reviewed top-level asm and listed by the frame
// sensor, that both rows are in the normal step in the plugin's order, that the keys are on the schema and in the example, and that the q15t
// plugin carries neither module, so one owner holds each slot on a q15t-variant APK.

func TestEachDroneMuzzleContinuationReadsItsNextWordWithAnAcquireLoadAndTheInstallHandsItOver(t *testing.T) {
	code := stripComments(read(t, "src/quest/combat/dronemuzzle.cpp"))
	for _, c := range []struct{ fn, word, hook string }{
		{"inline std::uint64_t Teleport(", "nevr_combat_dm_tp_next", "InstallAsmEntry(g_teleportHook"},
		{"inline std::uint64_t Fire(", "nevr_combat_dm_fire_next", "InstallAsmEntry(g_fireHook"},
	} {
		b := body(t, code, c.fn)
		if !strings.Contains(b, "__atomic_load_n(&"+c.word+", __ATOMIC_ACQUIRE)") {
			t.Errorf("%s must read %s with __atomic_load_n(..., __ATOMIC_ACQUIRE) (ldar on arm64), got %q", c.fn, c.word, b)
		}
		if strings.Contains(b, "__ATOMIC_RELAXED") {
			t.Errorf("%s must not read %s relaxed", c.fn, c.word)
		}
		at := strings.Index(code, c.hook)
		if at < 0 || !strings.Contains(code[at:at+300], "&"+c.word) {
			t.Errorf("the install must hand the manager &%s as the entry's next word (%s)", c.word, c.hook)
		}
	}
	if n := strings.Count(code, "NEVR_ASM_ENTRY_RECORD("); n != 2 {
		t.Errorf("want exactly two NEVR_ASM_ENTRY_RECORD in dronemuzzle.cpp, got %d", n)
	}
	for _, forbidden := range []string{"InstallThunk", "GotDataSlot", "mprotect", "PROT_WRITE", "SwapWord", "InstallImportHook"} {
		if strings.Contains(code, forbidden) {
			t.Errorf("dronemuzzle.cpp installs only through InstallAsmEntry and writes no page; it names %s", forbidden)
		}
	}
}

func TestTheDroneMuzzleEntriesAreTopLevelAsmWithCFIBothWordsByLdarAndTheSensorListsThem(t *testing.T) {
	asm := stripComments(read(t, "src/quest/combat/dronemuzzle_entry.inc"))
	if strings.Contains(asm, "naked") {
		t.Error("an assembly entry is a top-level asm() block, never an __attribute__((naked)) function")
	}
	for _, name := range []string{"nevr_combat_dm_tp_entry", "nevr_combat_dm_fire_entry"} {
		at := strings.Index(asm, name+":")
		if at < 0 {
			t.Fatalf("no %s label", name)
		}
		entry := asm[at : at+strings.Index(asm[at:], ".size")]
		for _, want := range []string{".cfi_startproc", "hint #34", ".cfi_def_cfa_offset 16", ".cfi_offset 30, -8", ".cfi_endproc", "br x16"} {
			if !strings.Contains(entry, want) {
				t.Errorf("%s must contain %q", name, want)
			}
		}
		if n := strings.Count(entry, "ldar x16, [x16]"); n != 2 {
			t.Errorf("%s must read both its words with ldar, got %d", name, n)
		}
		if strings.Contains(entry, "ldr x16") || strings.Contains(entry, "blr") {
			t.Errorf("%s reads a word plainly or calls through a register", name)
		}
	}
	if !strings.Contains(stripComments(read(t, "src/quest/combat/dronemuzzle.cpp")), `#include "quest/combat/dronemuzzle_entry.inc"`) {
		t.Error("dronemuzzle.cpp must build the entries from the .inc the qemu check runs")
	}
	sensor := read(t, "tests/quest/frames_sensor_test.go")
	for _, name := range []string{"nevr_combat_dm_tp_entry", "nevr_combat_dm_fire_entry"} {
		if !strings.Contains(sensor, "`^"+name+"$`") {
			t.Errorf("tests/quest/frames_sensor_test.go reviewedAsmEntries must list %s", name)
		}
	}
}

func TestDronekillHooksOnlyThroughTheManager(t *testing.T) {
	code := stripComments(read(t, "src/quest/combat/dronekill.cpp"))
	if n := strings.Count(code, "NEVR_HOOK_RECORD("); n != 4 {
		t.Errorf("want four NEVR_HOOK_RECORD in dronekill.cpp, got %d", n)
	}
	for _, forbidden := range []string{"asm(", "InstallAsmEntry", "GotDataSlot", "mprotect", "PROT_WRITE", "SwapWord", "InstallImportHook", "thread_local"} {
		if strings.Contains(code, forbidden) {
			t.Errorf("dronekill.cpp hooks through InstallThunk only and keeps no thread_local; it names %s", forbidden)
		}
	}
}

func TestTheDroneRowsAreInTheNormalStepInThePluginsOrderAndThePluginCarriesNeither(t *testing.T) {
	table := stripComments(read(t, "src/quest/combat/combat_install.cpp"))
	order := []string{`Module{"refresh_rate"`, `Module{"dronekill", nullptr, &DronekillNormal, "combat.dronekill"`, `Module{"dronemuzzle", nullptr, &DronemuzzleNormal, "combat.dronemuzzle"`, `Module{"gates"`}
	last := -1
	for _, row := range order {
		at := strings.Index(table, row)
		if at < 0 || at < last {
			t.Errorf("the normal table must hold %s after the row before it", row)
		}
		last = at
	}
	for _, gone := range []string{
		"src/quest/plugins/q15t/src/dronekill.cpp", "src/quest/plugins/q15t/src/dronekill.h", "src/quest/plugins/q15t/src/dronekill_core.h",
		"src/quest/plugins/q15t/src/dronekill_log.h", "src/quest/plugins/q15t/src/dronemuzzle.cpp", "src/quest/plugins/q15t/src/dronemuzzle.h",
		"src/quest/plugins/q15t/src/dronemuzzle_core.h", "src/quest/tests/q15t/mod/dronekill.cmake", "src/quest/tests/q15t/mod/dronemuzzle.cmake",
	} {
		if exists(t, gone) {
			t.Errorf("%s must not exist: the drone modules are the runtime's", gone)
		}
	}
	plugin := read(t, "src/quest/plugins/q15t/src/plugin.cpp") + read(t, "src/quest/plugins/q15t/CMakeLists.txt")
	for _, name := range []string{"Q15T_DRONEKILL", "Q15T_DRONEMUZZLE", "InstallDroneKill", "InstallDroneMuzzle", "DroneKill", "0x36cfa18", "0x36f3cf8",
		"0x36e2080", "0x36cacf8", "0x36c37a8", "0x36e6610"} {
		if strings.Contains(plugin, name) {
			t.Errorf("the q15t plugin must not carry %s", name)
		}
	}
}

func TestEveryDroneKeyIsOnTheSchemaAndTheExample(t *testing.T) {
	schema := read(t, "src/core/nevr_config.cpp")
	example := read(t, "docs/reference/example-config.yaml")
	reader := read(t, "src/quest/sentinel/quest_config.cpp")
	for _, key := range []string{"dronekill", "dronemuzzle"} {
		if !strings.Contains(schema, `"`+key+`"`) {
			t.Errorf("KnownSections must list combat.%s", key)
		}
		if !strings.Contains(example, "  # "+key+":") {
			t.Errorf("the example config must document combat.%s", key)
		}
		if !strings.Contains(reader, `"combat.`+key+`"`) {
			t.Errorf("quest_config.cpp must read combat.%s", key)
		}
	}
}
