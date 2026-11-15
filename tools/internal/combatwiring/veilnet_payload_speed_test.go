package combatwiring

import (
	"strings"
	"testing"
)

// #2242 (EchoCombat nevr-pack 82ad7ba): PAYLOAD SPEED and the payload PAUSE are live. His Install calls InstallPayloadSpeed
// (plugin/veilnet.cpp:2863). The decisions are combat/payload_speed_rules.h (test_payload_speed_rules.cpp drives them); this pins
// that veilnet.cpp installs the hook, validates it, takes the decisions from the header, and that nothing calls the feature parked.
func TestVeilnetInstallsPayloadSpeedWithMatchControl(t *testing.T) {
	src := code(t, veilnet)
	install := body(t, src, "bool Install(bool server) {")
	gate := body(t, install, "if (g_match_control) {")
	if !strings.Contains(gate, "InstallCaptureTime();") || !strings.Contains(gate, "InstallPayloadSpeed();") {
		t.Errorf("Install does not call InstallPayloadSpeed inside the g_match_control block: %s", install)
	}
	if strings.Contains(src, "[[maybe_unused]] void InstallPayloadSpeed") {
		t.Error("InstallPayloadSpeed is still marked unused")
	}
}

func TestVeilnetPayloadSpeedHookIsValidatedAndGated(t *testing.T) {
	src := code(t, veilnet)
	fn := body(t, src, "void InstallPayloadSpeed() {")
	order := []string{
		"memcmp(site, kPayloadSpeedSite",
		"reason=bytes_mismatch expected=%s actual=%s",
		"GateWrite(site",
		"ProcessMemcpy(site, jump",
		"g_payload_patched = site;",
	}
	at := 0
	for _, want := range order {
		i := strings.Index(fn[at:], want)
		if i < 0 {
			t.Errorf("InstallPayloadSpeed: %q is missing or out of order (prologue check, Warning with expected and actual, write gate, ProcessMemcpy, record)", want)
			return
		}
		at += i
	}
	if strings.Contains(fn, "VirtualProtect") {
		t.Error("InstallPayloadSpeed writes the site with VirtualProtect, not ProcessMemcpy")
	}
}

func TestVeilnetPayloadSpeedTakesItsDecisionsFromTheHeader(t *testing.T) {
	src := code(t, veilnet)
	stub := body(t, src, `extern "C" float __fastcall PayloadSpeed(`)
	for _, want := range []string{"ps::ServerStep(", "ps::ClientSpeed(", "g_payload_moved = 0", "g_payload_moved = 1"} {
		if !strings.Contains(stub, want) {
			t.Errorf("PayloadSpeed does not use %q", want)
		}
	}
	for _, want := range []string{
		"nevr_combat::payload_speed::RowText(",
		"nevr_combat::payload_speed::PauseWaitsForCheckpoint(",
	} {
		if !strings.Contains(src, want) {
			t.Errorf("veilnet.cpp does not use %q", want)
		}
	}
	// #2242: the 'G' word's payload bits are packed and read through match_control_rules.h, which veilnet.cpp and the Quest call
	shared := code(t, matchControlRules)
	for _, want := range []string{"payload_speed::PackWord(", "payload_speed::WordMap(", "payload_speed::WordQuarters("} {
		if !strings.Contains(shared, want) {
			t.Errorf("%s does not use %q", matchControlRules, want)
		}
	}
}

func TestNothingCallsPayloadSpeedParked(t *testing.T) {
	settings := code(t, "src/runtime/combat/combat_settings.h")
	if strings.Contains(settings, "payload speed") {
		t.Error("combat_settings.h still lists payload speed")
	}
	raw := readSource(t, veilnet)
	for _, bad := range []string{"parked", "never called"} {
		if strings.Contains(raw, bad) {
			t.Errorf("veilnet.cpp still says %q", bad)
		}
	}
}
