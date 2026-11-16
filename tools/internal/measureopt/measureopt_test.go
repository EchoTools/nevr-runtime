package measureopt

import (
	"encoding/json"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

const (
	runtimeCMake = "src/runtime/CMakeLists.txt"
	presetsFile  = "CMakePresets.json"
	adr0040      = "docs/adr/0040-every-build-names-its-variant.md"
	optionName   = "NEVR_MEASURE_SCRIPT_HOOKS"
	trustOption  = "NEVR_ALLOW_UNSIGNED_PLUGINS"
	measureWord  = "script-hook-calls"
)

func repoFile(t *testing.T, rel string) string {
	t.Helper()
	dir, err := os.Getwd()
	if err != nil {
		t.Fatal(err)
	}
	for {
		if _, err := os.Stat(filepath.Join(dir, "AGENTS.md")); err == nil {
			break
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			t.Fatalf("no AGENTS.md above the test's directory: cannot find %s", rel)
		}
		dir = parent
	}
	b, err := os.ReadFile(filepath.Join(dir, filepath.FromSlash(rel)))
	if err != nil {
		t.Fatal(err)
	}
	return string(b)
}

// stripComments drops CMake comment lines, so the prose that explains a rule cannot satisfy its check.
func stripComments(text string) string {
	var out []string
	for _, line := range strings.Split(text, "\n") {
		if strings.HasPrefix(strings.TrimSpace(line), "#") {
			continue
		}
		out = append(out, line)
	}
	return strings.Join(out, "\n")
}

var ifOrEndif = regexp.MustCompile(`(?m)^\s*(if\(|endif\(\))`)

// blockOf returns the text between `if(<cond>)` and its matching endif().
func blockOf(t *testing.T, text, cond string) string {
	t.Helper()
	open := "if(" + cond + ")"
	start := strings.Index(text, open)
	if start < 0 {
		t.Fatalf("%s: no %s", runtimeCMake, open)
	}
	body := text[start+len(open):]
	depth := 1
	for _, m := range ifOrEndif.FindAllStringSubmatchIndex(body, -1) {
		if body[m[2]] == 'i' {
			depth++
		} else {
			depth--
		}
		if depth == 0 {
			return body[:m[0]]
		}
	}
	t.Fatalf("%s: %s has no endif()", runtimeCMake, open)
	return ""
}

func TestMeasureOptionRequiresDevTrust(t *testing.T) {
	t.Parallel()
	text := stripComments(repoFile(t, runtimeCMake))

	optionRe := regexp.MustCompile(`option\(` + optionName + `\s+"[^"]*"\s+OFF\)`)
	if !optionRe.MatchString(text) {
		t.Fatalf("%s: %s must be an option that defaults to OFF (ADR 0037 rule 1)", runtimeCMake, optionName)
	}
	trustAt := strings.Index(text, "option("+trustOption)
	optionAt := strings.Index(text, "option("+optionName)
	if trustAt < 0 || optionAt < trustAt {
		t.Errorf("%s: the %s block must follow the %s option it reads", runtimeCMake, optionName, trustOption)
	}

	block := blockOf(t, text, optionName)
	guard := regexp.MustCompile(`if\(NOT ` + trustOption + `\)\s*message\(FATAL_ERROR\s+"[^"]*` + trustOption + `[^"]*"\)\s*endif\(\)`)
	loc := guard.FindStringIndex(block)
	if loc == nil {
		t.Fatalf("%s: when %s is on and %s is off, configure must stop with message(FATAL_ERROR ...) naming %s (ADR 0037 rule 5)",
			runtimeCMake, optionName, trustOption, trustOption)
	}
	define := strings.Index(block, "add_compile_definitions("+optionName)
	if define < 0 {
		t.Fatalf("%s: the option's block defines nothing for the sources", runtimeCMake)
	}
	if define < loc[1] {
		t.Errorf("%s: the definition must come after the refusal, so a refused configure defines nothing", runtimeCMake)
	}
}

func TestOnlyTheMeasurePresetSetsTheOptionAndItInheritsPluginDev(t *testing.T) {
	t.Parallel()
	var doc struct {
		ConfigurePresets []struct {
			Name           string            `json:"name"`
			Inherits       json.RawMessage   `json:"inherits"`
			CacheVariables map[string]string `json:"cacheVariables"`
		} `json:"configurePresets"`
	}
	if err := json.Unmarshal([]byte(repoFile(t, presetsFile)), &doc); err != nil {
		t.Fatal(err)
	}
	found := false
	for _, p := range doc.ConfigurePresets {
		if p.CacheVariables[optionName] == "" {
			continue
		}
		if p.Name != "mingw-measure" {
			t.Errorf("preset %s sets %s: only mingw-measure may", p.Name, optionName)
			continue
		}
		found = true
		if p.CacheVariables[optionName] != "ON" {
			t.Errorf("mingw-measure sets %s=%s, want ON", optionName, p.CacheVariables[optionName])
		}
		if string(p.Inherits) != `"mingw-plugin-dev"` {
			t.Errorf("mingw-measure inherits %s, want \"mingw-plugin-dev\" (the dev trust literal)", p.Inherits)
		}
	}
	if !found {
		t.Errorf("%s has no preset that sets %s", presetsFile, optionName)
	}
}

func TestAdr0040NamesTheMeasureWordAsAPcWordAndKeepsTheQuestFour(t *testing.T) {
	t.Parallel()
	text := repoFile(t, adr0040)
	if !strings.Contains(text, measureWord) {
		t.Errorf("%s does not name %s (decision 1's list of words)", adr0040, measureWord)
	}
	if strings.Contains(text, "not one of the four") {
		t.Errorf("%s still says the Quest repack refuses a word that is not one of the four: the list has five words, one of them PC only", adr0040)
	}
	if !strings.Contains(text, "not one of the Quest words") {
		t.Errorf("%s: the Quest sentence must read \"not one of the Quest words\"", adr0040)
	}
}
