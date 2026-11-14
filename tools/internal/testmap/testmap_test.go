package testmap

import (
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"regexp"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/testkit"
)

func TestParseRefusesAMapEveryLookupCouldNotTrust(t *testing.T) {
	t.Parallel()
	cases := []struct{ name, json, want string }{
		{"schema", `{"schema": 2, "components": []}`, "schema is 2"},
		{"unknown key", `{"schema": 1, "extra": 1, "components": []}`, "unknown field"},
		{"no paths", `{"schema": 1, "components": [{"name": "a", "paths": [], "tests": ["just:x"]}]}`, "has no paths"},
		{"two owners", `{"schema": 1, "components": [{"name": "a", "paths": ["x/"], "tests": ["just:x"]}, {"name": "b", "paths": ["x/"], "tests": ["just:y"]}]}`, "a path has one owner"},
		{"same name", `{"schema": 1, "components": [{"name": "a", "paths": ["x"], "tests": ["just:x"]}, {"name": "a", "paths": ["y"], "tests": ["just:y"]}]}`, "used twice"},
		{"neither", `{"schema": 1, "components": [{"name": "a", "paths": ["x"], "tests": []}]}`, "not both, not neither"},
		{"both", `{"schema": 1, "components": [{"name": "a", "paths": ["x"], "tests": ["just:x"], "none": "why"}]}`, "not both, not neither"},
		{"unknown kind", `{"schema": 1, "components": [{"name": "a", "paths": ["x"], "tests": ["cargo:x"]}]}`, `kind "cargo"`},
		{"unknown target", `{"schema": 1, "components": [{"name": "a", "paths": ["x"], "tests": ["lint"]}]}`, "neither a target"},
		{"empty name", `{"schema": 1, "components": [{"name": "a", "paths": ["x"], "tests": ["go:"]}]}`, "neither a target"},
		{"repeat", `{"schema": 1, "components": [{"name": "a", "paths": ["x"], "tests": ["just:x", "just:x"]}]}`, "twice"},
		{"dirty entry", `{"schema": 1, "components": [{"name": "a", "paths": ["x/../y"], "tests": ["just:x"]}]}`, "not a clean"},
		{"absolute entry", `{"schema": 1, "components": [{"name": "a", "paths": ["/x"], "tests": ["just:x"]}]}`, "not a clean"},
		{"empty target", `{"schema": 1, "targets": {"t": []}, "components": []}`, "has no command"},
		{"target with a kind's colon", `{"schema": 1, "targets": {"go:x": ["true"]}, "components": []}`, "no ':'"},
	}
	for _, c := range cases {
		_, err := Parse([]byte(c.json))
		if err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: Parse error = %v, want one containing %q", c.name, err, c.want)
		}
	}
}

const sample = `{"schema": 1,
 "targets": {"build": ["just", "build"], "seams": ["python3", "tools/verify_host_seams.py", "build/{preset}"]},
 "components": [
  {"name": "dir", "paths": ["src/a/"], "tests": ["build", "gtest:test_a"]},
  {"name": "deeper", "paths": ["src/a/b/"], "tests": ["just:test-quest-b"]},
  {"name": "file", "paths": ["src/a/b/c.cpp"], "tests": ["go:cmd/x", "seams"]},
  {"name": "docs", "paths": ["docs/"], "none": "no test reads it"}
 ]}`

func TestOwnerIsTheLongestMatchingEntry(t *testing.T) {
	t.Parallel()
	m, err := Parse([]byte(sample))
	if err != nil {
		t.Fatal(err)
	}
	cases := map[string]string{
		"src/a/x.cpp":       "dir",
		"src/a/b/y.cpp":     "deeper",
		"src/a/b/c.cpp":     "file",
		"src/a/b/c/d.cpp":   "deeper",
		"docs/x/y.md":       "docs",
		"src/ab/x.cpp":      "",
		"src/a":             "",
		"other/src/a/x.cpp": "",
	}
	for p, want := range cases {
		c, entry, ok := m.Owner(p)
		got := ""
		if ok {
			got = c.Name
		}
		if got != want {
			t.Errorf("Owner(%q) = %q (entry %q), want %q", p, got, entry, want)
		}
	}
}

func TestCommandOfEachKindAndTarget(t *testing.T) {
	t.Parallel()
	m, err := Parse([]byte(sample))
	if err != nil {
		t.Fatal(err)
	}
	cases := map[string][]string{
		"gtest:test_a":      {"just", "test-auth-unit", "test_a"},
		"just:test-quest-b": {"just", "test-quest-b"},
		"go:cmd/x":          {"go", "-C", "tools", "test", "./cmd/x"},
		"pytest:test_thing": {"python3", "tools/run_tool_tests.py", "tools/tests/test_thing.py"},
		"seams":             {"python3", "tools/verify_host_seams.py", "build/mingw-dev"},
		"build":             {"just", "build"},
	}
	for id, want := range cases {
		got, err := m.Command(id, "mingw-dev")
		if err != nil || !reflect.DeepEqual(got, want) {
			t.Errorf("Command(%q) = %q, %v; want %q", id, got, err, want)
		}
	}
	// The map's own argv is never changed by a substitution.
	if m.Targets["seams"][2] != "build/{preset}" {
		t.Errorf("Command rewrote the map's target: %q", m.Targets["seams"])
	}
}

func TestOrderPutsTheBuildFirstThenTargetsThenEachKind(t *testing.T) {
	t.Parallel()
	m, err := Parse([]byte(sample))
	if err != nil {
		t.Fatal(err)
	}
	got := m.Order([]string{"pytest:b", "go:a", "seams", "just:z", "gtest:y", "build", "gtest:x", "go:a"})
	want := []string{"build", "seams", "gtest:x", "gtest:y", "just:z", "go:a", "pytest:b"}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("Order = %q, want %q", got, want)
	}
}

// repoRoot is the checkout this package is in: tools/internal/testmap is three levels below it.
func repoRoot(t *testing.T) string {
	t.Helper()
	root, err := filepath.Abs(filepath.Join("..", "..", ".."))
	if err != nil {
		t.Fatal(err)
	}
	return root
}

func output(t *testing.T, dir string, name string, args ...string) string {
	t.Helper()
	cmd := exec.Command(name, args...)
	cmd.Dir = dir
	cmd.Env = testkit.Env(t)
	out, err := cmd.Output()
	if err != nil {
		t.Fatalf("%s %s: %v", name, strings.Join(args, " "), err)
	}
	return string(out)
}

// The repository's own map covers every tracked file and names only tests that exist, so `just test-changed` can
// neither skip a file nor run something that is not there.
func TestTheRepositoryMapCoversEveryTrackedFileAndNamesRealTests(t *testing.T) {
	t.Parallel()
	root := repoRoot(t)
	m, err := Load(filepath.Join(root, DefaultPath))
	if err != nil {
		t.Fatal(err)
	}
	tracked := map[string]bool{}
	dirs := map[string]bool{}
	for _, f := range strings.Split(output(t, root, "git", "ls-files", "-z"), "\x00") {
		if f == "" {
			continue
		}
		tracked[f] = true
		for d := filepath.Dir(f); d != "."; d = filepath.Dir(d) {
			dirs[d+"/"] = true
		}
		if _, _, ok := m.Owner(f); !ok {
			t.Errorf("tracked file %s has no component in %s: add it to the component whose tests cover it", f, DefaultPath)
		}
	}
	cmake, err := os.ReadFile(filepath.Join(root, "src/runtime/CMakeLists.txt"))
	if err != nil {
		t.Fatal(err)
	}
	recipes := map[string]bool{}
	for _, r := range strings.Fields(output(t, root, "just", "--justfile", filepath.Join(root, "justfile"), "--summary", "--unsorted")) {
		recipes[r] = true
	}
	used := map[string]bool{}
	for _, c := range m.Components {
		for _, p := range c.Paths {
			if strings.HasSuffix(p, "/") && !dirs[p] || !strings.HasSuffix(p, "/") && !tracked[p] {
				t.Errorf("component %q: entry %s matches no tracked file", c.Name, p)
			}
		}
		for _, id := range c.Tests {
			used[id] = true
			kind, name, _ := strings.Cut(id, ":")
			switch {
			case m.Targets[id] != nil:
			case kind == KindGTest:
				if !regexp.MustCompile(`add_executable\(` + regexp.QuoteMeta(name) + `\b`).Match(cmake) {
					t.Errorf("component %q: %s is not an add_executable of src/runtime/CMakeLists.txt", c.Name, id)
				}
			case kind == KindJust:
				if !recipes[name] {
					t.Errorf("component %q: %s is not a recipe of the justfile", c.Name, id)
				}
			case kind == KindGo:
				if gofiles, _ := filepath.Glob(filepath.Join(root, "tools", name, "*.go")); len(gofiles) == 0 {
					t.Errorf("component %q: %s is not a Go package under tools/", c.Name, id)
				}
			case kind == KindPytest:
				if !tracked["tools/tests/"+name+".py"] {
					t.Errorf("component %q: %s is not a tracked file tools/tests/%s.py", c.Name, id, name)
				}
			}
		}
	}
	for name := range m.Targets {
		if !used[name] {
			t.Errorf("target %q is used by no component", name)
		}
	}
}
