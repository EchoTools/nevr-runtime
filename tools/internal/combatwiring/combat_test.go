package combatwiring

import (
	"regexp"
	"sort"
	"strings"
	"testing"
)

// docs/adr/0047: EchoCombat on PC is opted into. The rule (`combat.enabled`, false unless config.yaml says true) is driven by
// src/runtime/tests/test_combat_settings.cpp (CombatIsOffUnlessTheConfigOptsIn) against the real header. combat.cpp hooks the
// game and links into no test binary, so these read it as source: both entry points boot.cpp calls ask the opt-in before
// anything else, so a run with no config installs nothing of combat's, and boot.cpp reaches combat through nothing else.

const combatCpp = "src/runtime/combat/combat.cpp"

func index(t *testing.T, text, what string) int {
	t.Helper()
	i := strings.Index(text, what)
	if i < 0 {
		t.Fatalf("%q not found", what)
	}
	return i
}

func TestTheOptInReadsTheEnabledKeyThroughThePureRuleOnce(t *testing.T) {
	b := body(t, code(t, combatCpp), "bool OptedIn() {")
	if !strings.Contains(b, "ReadEnabled([](const char* key) { return NevrCfgCombatValue(key); })") {
		t.Fatal("OptedIn does not read combat.enabled through ReadEnabled and NevrCfgCombatValue")
	}
	if !strings.Contains(b, "static const bool on") {
		t.Fatal("OptedIn is not read once: the two entry points must share one answer and one line")
	}
}

func TestRegisterBalanceAsksTheOptInBeforeItRegisters(t *testing.T) {
	b := body(t, code(t, combatCpp), "void RegisterBalance() {")
	if index(t, b, "if (!OptedIn()) return;") > index(t, b, "RegisterResourceTransform(") {
		t.Fatal("RegisterBalance registers the transform before it asks the opt-in")
	}
}

func TestInstallAsksTheOptInBeforeAnythingElse(t *testing.T) {
	b := body(t, code(t, combatCpp), "void Install(std::uintptr_t gameBase, bool isServer) {")
	gate := index(t, b, "if (!OptedIn()) return;")
	if index(t, b, "std::call_once(once") > gate {
		t.Fatal("the opt-in is asked outside the once block")
	}
	for _, later := range []string{"ReadSettings(", "cat::InstallFaultHandler()", "dot_script::Register()",
		"shared_magazine::Register()", "GateWrite(", "dronefix::Start(", "veilnet::Start(", "powerboost::Start(",
		"veilnet::StartCleanExit()"} {
		if index(t, b, later) < gate {
			t.Errorf("%s runs before the opt-in is asked", later)
		}
	}
}

func TestBootReachesCombatOnlyThroughTheTwoGatedEntryPoints(t *testing.T) {
	seen := map[string]bool{}
	for _, m := range regexp.MustCompile(`\bnevr_combat::(\w+)\s*\(`).FindAllStringSubmatch(code(t, "src/runtime/lifecycle/boot.cpp"), -1) {
		seen[m[1]] = true
	}
	var calls []string
	for name := range seen {
		calls = append(calls, name)
	}
	sort.Strings(calls)
	if strings.Join(calls, ",") != "Install,RegisterBalance" {
		t.Fatalf("boot.cpp calls nevr_combat::%v; want only Install and RegisterBalance", calls)
	}
}
