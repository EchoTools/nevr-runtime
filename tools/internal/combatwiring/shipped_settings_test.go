package combatwiring

import (
	"strings"
	"testing"
)

// #600: a PowerBoost.ini key config.yaml leaves out takes the value his release ships (combat/shipped_settings.h, which
// test_combat_shipped_settings.cpp drives). These pin that the shim the port's ini reads go through
// (cat_compat.cpp, IniValue) asks config.yaml first and his shipped value second, from the embedded settings.

const catCompat = "src/runtime/combat/cat_compat.cpp"

func TestTheIniShimAsksConfigFirstThenTheShippedValue(t *testing.T) {
	src := code(t, catCompat)
	b := body(t, src, "const char* IniValue(const char* section, const char* key)")
	cfg := strings.Index(b, "NevrCfgCombatValue(")
	shipped := strings.Index(b, "nevr_combat::ShippedSetting(settings, section, key)")
	if cfg < 0 || shipped < 0 || cfg > shipped {
		t.Fatalf("IniValue no longer asks config.yaml and then the shipped value:\n%s", b)
	}
	if !strings.Contains(b, "nevr_combat::balance::kCatWeaponSettings") {
		t.Errorf("the shipped values no longer come from the embedded settings:\n%s", b)
	}
	if !strings.Contains(b, "LogSourceOnce(") {
		t.Errorf("IniValue no longer logs each key's source:\n%s", b)
	}
}

func TestBothIniReadsGoThroughTheShim(t *testing.T) {
	src := code(t, catCompat)
	for _, sig := range []string{"int IniInt(const char* section, const char* key, int fallback)",
		"DWORD IniString(const char* section, const char* key, const char* fallback, char* out, DWORD size)"} {
		if !strings.Contains(body(t, src, sig), "IniValue(section, key)") {
			t.Errorf("%s no longer reads through IniValue", sig)
		}
	}
}
