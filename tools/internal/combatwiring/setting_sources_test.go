package combatwiring

import (
	"strings"
	"testing"
)

// logging.md Rule 11: the combat module logs, at Info, how many PowerBoost settings came from config.yaml, from his
// shipped values and from the code, once PowerBoost has read them; the per-key source lines stay Debug.

func TestTheSettingSourcesAreCountedOnceAndLoggedAtInfoAfterPowerBoostStarts(t *testing.T) {
	compat := code(t, "src/runtime/combat/cat_compat.cpp")
	once := body(t, compat, "void LogSourceOnce(const std::string& name, const char* source, const char* value)")
	emplace := strings.Index(once, "g_sourceLogged.emplace(name, true).second")
	count := strings.Index(once, "++g_counts.")
	for _, c := range []string{"++g_counts.config;", "++g_counts.shipped;", "++g_counts.code;"} {
		if !strings.Contains(once, c) {
			t.Errorf("LogSourceOnce no longer counts %s", c)
		}
	}
	if emplace < 0 || count < 0 || count < emplace {
		t.Errorf("LogSourceOnce no longer counts a key once, after its first-read check:\n%s", once)
	}
	if !strings.Contains(once, "EchoVR::LogLevel::Debug") {
		t.Errorf("the per-key source line is no longer Debug:\n%s", once)
	}
	if !strings.Contains(body(t, compat, "IniSourceCounts IniSources()"), "return g_counts;") {
		t.Error("IniSources no longer returns the counts")
	}
	combat := code(t, "src/runtime/combat/combat.cpp")
	start := strings.Index(combat, "powerboost::Start(")
	line := strings.Index(combat, "PowerBoost settings read config_yaml=%d shipped=%d code=%d")
	if start < 0 || line < 0 || line < start {
		t.Fatal("combat.cpp no longer logs the setting sources after PowerBoost starts")
	}
	block := combat[start:line]
	if !strings.Contains(block, "cat::IniSources()") || !strings.Contains(combat[line-200:line], "EchoVR::LogLevel::Info") {
		t.Errorf("the setting-sources line is not an Info line over IniSources():\n%s", combat[start:line+80])
	}
}
