package testmap

import (
	"bytes"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"regexp"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/testkit"
)

// Named constants for the literals the tests use (AGENTS.md, "No magic numbers").
const (
	testFileMode       = 0o600
	presetArgIndex     = 2
	sampleComponents   = 4
	sampleTestPreset   = "mingw-dev"
	sampleSeamsPresetA = "build/mingw-dev"
)

// sampleMap is a schema 2 map small enough to read, with each shape the reader handles: a target with {preset}, a file
// listed by its component, a file in none, a file in a directory with a component of its own, a scanner that adds tests
// (one of them already owned by a file, to test that a repeat is dropped), and the root component. Its components are
// deliberately not in sorted order, so Encode's sorting is tested.
const sampleMap = `{"schema": 2,
 "about": ["a sample"],
 "targets": {"alpha": ["true"], "build": ["just", "build"], "seams": ["python3", "tools/verify_host_seams.py", "build/{preset}"]},
 "scanners": [
  {"name": "go-policy", "globs": ["tools/**/*.go", "tools/**/*.py"], "tests": ["go:internal/policy", "go:cmd/x"]},
  {"name": "one-home", "globs": ["**/*.md"], "tests": ["go:cmd/one-home", "build"]}
 ],
 "components": [
  {"name": "src/a", "dir": "src/a",
   "files": {"src/a/x.cpp": ["build", "gtest:test_a"], "src/a/y.h": ["gtest:test_a"]},
   "none": {"src/a/README.md": "documentation: only the scanners read it"}},
  {"name": "src/a/b", "dir": "src/a/b", "files": {"src/a/b/c.cpp": ["just:test-quest-b"]}},
  {"name": "tools", "dir": "tools", "files": {"tools/x.go": ["go:cmd/x"]}},
  {"name": "(root)", "dir": "", "files": {"go.mod": ["go:cmd/x"]}, "none": {"LICENSE": "license text"}}
 ]}`

func mustParseSample(t *testing.T) *Map {
	t.Helper()
	m, err := Parse([]byte(sampleMap))
	if err != nil {
		t.Fatal(err)
	}
	return m
}

func TestParseRefusesAMapEveryLookupCouldNotTrust(t *testing.T) {
	t.Parallel()
	cases := []struct{ name, json, want string }{
		{"schema 1", `{"schema": 1, "components": []}`, "schema is 1"},
		{"not JSON", `{"schema": 2,`, "unexpected EOF"},
		{"unknown key", `{"schema": 2, "extra": 1, "components": []}`, "unknown field"},
		{"unknown key in a scanner", `{"schema": 2, "scanners": [{"name": "s", "globs": ["*"], "tests": ["just:x"], "extra": 1}], "components": []}`, "unknown field"},
		{"data after the object", `{"schema": 2, "components": []} {}`, "after its object"},
		{"target name with a colon", `{"schema": 2, "targets": {"go:x": ["true"]}, "components": []}`, "no ':'"},
		{"target with no name", `{"schema": 2, "targets": {"": ["true"]}, "components": []}`, "name is not empty"},
		{"target with no argv", `{"schema": 2, "targets": {"t": []}, "components": []}`, "has no command"},
		{"component with no name", `{"schema": 2, "components": [{"name": "", "dir": "x", "files": {"x/a": ["just:x"]}}]}`, "has no name"},
		{"two components, one name", `{"schema": 2, "components": [{"name": "a", "dir": "a", "files": {"a/x": ["just:x"]}}, {"name": "a", "dir": "b", "files": {"b/y": ["just:x"]}}]}`, "used twice"},
		{"two components, one dir", `{"schema": 2, "components": [{"name": "a", "dir": "a", "files": {"a/x": ["just:x"]}}, {"name": "a2", "dir": "a", "files": {"a/y": ["just:x"]}}]}`, "one component per directory"},
		{"dir not clean", `{"schema": 2, "components": [{"name": "a/../b", "dir": "a/../b"}]}`, "not a clean"},
		{"name is not the dir", `{"schema": 2, "components": [{"name": "x", "dir": "a", "files": {"a/x": ["just:x"]}}]}`, "name must be"},
		{"root named otherwise", `{"schema": 2, "components": [{"name": "root", "dir": "", "files": {"x": ["just:x"]}}]}`, "name must be"},
		{"file outside the dir", `{"schema": 2, "components": [{"name": "a", "dir": "a", "files": {"b/x": ["just:x"]}}]}`, "is in directory"},
		{"file path not clean", `{"schema": 2, "components": [{"name": "a", "dir": "a", "files": {"a/../a/x": ["just:x"]}}]}`, "not a clean"},
		{"file with no tests", `{"schema": 2, "components": [{"name": "a", "dir": "a", "files": {"a/x": []}}]}`, "has no tests"},
		{"file in files and none", `{"schema": 2, "components": [{"name": "a", "dir": "a", "files": {"a/x": ["just:x"]}, "none": {"a/x": "why"}}]}`, "in files and in none"},
		{"file with an empty reason", `{"schema": 2, "components": [{"name": "a", "dir": "a", "none": {"a/x": ""}}]}`, "empty reason"},
		{"test listed twice", `{"schema": 2, "components": [{"name": "a", "dir": "a", "files": {"a/x": ["just:x", "just:x"]}}]}`, "listed twice"},
		{"unknown kind", `{"schema": 2, "components": [{"name": "a", "dir": "a", "files": {"a/x": ["cargo:x"]}}]}`, `kind "cargo"`},
		{"bare id that is no target", `{"schema": 2, "components": [{"name": "a", "dir": "a", "files": {"a/x": ["lint"]}}]}`, "neither a target"},
		{"scanner with no name", `{"schema": 2, "scanners": [{"globs": ["*"], "tests": ["just:x"]}], "components": []}`, "has no name"},
		{"two scanners, one name", `{"schema": 2, "scanners": [{"name": "s", "globs": ["*"], "tests": ["just:x"]}, {"name": "s", "globs": ["*"], "tests": ["just:x"]}], "components": []}`, "named twice"},
		{"scanner with no globs", `{"schema": 2, "scanners": [{"name": "s", "tests": ["just:x"]}], "components": []}`, "has no globs"},
		{"glob with an empty segment", `{"schema": 2, "scanners": [{"name": "s", "globs": [""], "tests": ["just:x"]}], "components": []}`, "empty segment"},
		{"glob not path.Match syntax", `{"schema": 2, "scanners": [{"name": "s", "globs": ["["], "tests": ["just:x"]}], "components": []}`, "not path.Match syntax"},
		{"scanner with no tests", `{"schema": 2, "scanners": [{"name": "s", "globs": ["*"], "tests": []}], "components": []}`, "has no tests"},
		{"scanner test that names nothing", `{"schema": 2, "scanners": [{"name": "s", "globs": ["*"], "tests": ["cargo:x"]}], "components": []}`, `kind "cargo"`},
		{"scanner test listed twice", `{"schema": 2, "scanners": [{"name": "s", "globs": ["*"], "tests": ["just:x", "just:x"]}], "components": []}`, "listed twice"},
	}
	for _, c := range cases {
		_, err := Parse([]byte(c.json))
		if err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: Parse error = %v, want one containing %q", c.name, err, c.want)
		}
	}
}

func TestLoadNamesTheFileItRefuses(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	bad := filepath.Join(dir, "map.json")
	if err := os.WriteFile(bad, []byte(`{"schema": 1, "components": []}`), testFileMode); err != nil {
		t.Fatal(err)
	}
	if _, err := Load(bad); err == nil || !strings.Contains(err.Error(), bad) || !strings.Contains(err.Error(), "schema is 1") {
		t.Errorf("Load(schema 1) = %v, want an error naming %s and the schema", err, bad)
	}
	missing := filepath.Join(dir, "absent.json")
	if _, err := Load(missing); err == nil {
		t.Errorf("Load(%s) = nil error, want the file's absence", missing)
	}
}

func TestLookupGivesOwnTestsThenScanners(t *testing.T) {
	t.Parallel()
	m := mustParseSample(t)
	cases := []struct {
		file, component, none string
		own, scanners, tests  []string
	}{
		{file: "src/a/x.cpp", component: "src/a", own: []string{"build", "gtest:test_a"}, tests: []string{"build", "gtest:test_a"}},
		{file: "src/a/b/c.cpp", component: "src/a/b", own: []string{"just:test-quest-b"}, tests: []string{"just:test-quest-b"}},
		{file: "tools/x.go", component: "tools", own: []string{"go:cmd/x"}, scanners: []string{"go-policy"}, tests: []string{"go:cmd/x", "go:internal/policy"}},
		{file: "src/a/README.md", component: "src/a", none: "documentation: only the scanners read it", scanners: []string{"one-home"}, tests: []string{"go:cmd/one-home", "build"}},
		{file: "go.mod", component: "(root)", own: []string{"go:cmd/x"}, tests: []string{"go:cmd/x"}},
		{file: "LICENSE", component: "(root)", none: "license text"},
	}
	for _, c := range cases {
		lk, ok := m.Lookup(c.file)
		if !ok {
			t.Errorf("Lookup(%q): not covered, want component %q", c.file, c.component)
			continue
		}
		if lk.Component == nil || lk.Component.Name != c.component {
			t.Errorf("Lookup(%q).Component = %v, want %q", c.file, lk.Component, c.component)
		}
		if lk.None != c.none {
			t.Errorf("Lookup(%q).None = %q, want %q", c.file, lk.None, c.none)
		}
		if !reflect.DeepEqual(lk.Own, c.own) {
			t.Errorf("Lookup(%q).Own = %q, want %q", c.file, lk.Own, c.own)
		}
		if !reflect.DeepEqual(lk.Scanners, c.scanners) {
			t.Errorf("Lookup(%q).Scanners = %q, want %q", c.file, lk.Scanners, c.scanners)
		}
		if !reflect.DeepEqual(lk.Tests, c.tests) {
			t.Errorf("Lookup(%q).Tests = %q, want %q", c.file, lk.Tests, c.tests)
		}
	}
}

func TestLookupRefusesAPathTheMapDoesNotCover(t *testing.T) {
	t.Parallel()
	m := mustParseSample(t)
	for _, file := range []string{
		"tools/internal/y.go", // no component for tools/internal, though go-policy matches it
		"docs/d.md",           // no component for docs, though one-home matches it
		"src/a/z.cpp",         // src/a has a component, and z.cpp is in neither of its maps
		"src/a/b/zz.cpp",      // src/a/b has a component, and zz.cpp is in neither of its maps
		"src/a/deep/x.cpp",    // a directory under src/a has no component of its own
	} {
		if lk, ok := m.Lookup(file); ok {
			t.Errorf("Lookup(%q) covered by component %v, want not covered", file, lk.Component)
		}
	}
}

func TestMatchGlobSpansWholeSegmentsOnly(t *testing.T) {
	t.Parallel()
	cases := []struct {
		glob, file string
		want       bool
	}{
		{"**/*.md", "README.md", true},
		{"**/*.md", "docs/x/y.md", true},
		{"**/*.md", "docs/x/y.txt", false},
		{"tools/**/*.go", "tools/x.go", true},
		{"tools/**/*.go", "tools/a/b/c.go", true},
		{"tools/**/*.go", "other/tools/x.go", false},
		{"tools/**/*.go", "tools/a/b.py", false},
		{"**/x", "x", true},
		{"**/x", "a/b/x", true},
		{"**/x", "a/xy", false},
		{"a/**/z", "a/z", true},
		{"a/**/z", "a/q/r/z", true},
		{"a/**/z", "b/z", false},
		{"a/**", "a/z", true},
		{"a/**", "b/a/z", false},
		{"**", "any/where/file.txt", true},
		{"*.go", "x.go", true},
		{"*.go", "a/x.go", false},
		{"src/*.cpp", "src/a.cpp", true},
		{"src/*.cpp", "src/a/b.cpp", false},
		{"src/[ab].cpp", "src/a.cpp", true},
		{"src/[ab].cpp", "src/c.cpp", false},
		{"a**b", "axb", true},
		{"a**b", "a/x/b", false},
	}
	for _, c := range cases {
		if got := MatchGlob(c.glob, c.file); got != c.want {
			t.Errorf("MatchGlob(%q, %q) = %v, want %v", c.glob, c.file, got, c.want)
		}
	}
}

func TestCommandResolvesEachKindAndTheTargets(t *testing.T) {
	t.Parallel()
	m := mustParseSample(t)
	cases := map[string][]string{
		"build":             {"just", "build"},
		"seams":             {"python3", "tools/verify_host_seams.py", sampleSeamsPresetA},
		"gtest:test_a":      {"just", "test-auth-unit", "test_a"},
		"just:test-quest-b": {"just", "test-quest-b"},
		"go:cmd/x":          {"go", "-C", "tools", "test", "-count=1", "./cmd/x"},
		"pytest:test_thing": {"python3", "tools/run_tool_tests.py", "tools/tests/test_thing.py"},
	}
	for id, want := range cases {
		got, err := m.Command(id, sampleTestPreset)
		if err != nil || !reflect.DeepEqual(got, want) {
			t.Errorf("Command(%q) = %q, %v; want %q", id, got, err, want)
		}
	}
	for _, id := range []string{"cargo:x", "gtest:", "lint", ""} {
		if argv, err := m.Command(id, sampleTestPreset); err == nil {
			t.Errorf("Command(%q) = %q, want an error", id, argv)
		}
	}
	if got := m.Targets["seams"][presetArgIndex]; got != "build/{preset}" {
		t.Errorf("Command rewrote the map's target: %q", got)
	}
}

func TestOrderPutsTheBuildFirstThenTargetsThenEachKind(t *testing.T) {
	t.Parallel()
	m := mustParseSample(t)
	// alpha sorts before build by name, so only the build rank puts build first.
	got := m.Order([]string{"pytest:b", "go:a", "seams", "just:z", "gtest:y", "build", "gtest:x", "go:a", "alpha", "cargo:q"})
	want := []string{"build", "alpha", "seams", "gtest:x", "gtest:y", "just:z", "go:a", "pytest:b", "cargo:q"}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("Order = %q, want %q", got, want)
	}
	if empty := m.Order(nil); len(empty) != 0 {
		t.Errorf("Order(nil) = %q, want nothing", empty)
	}
}

// encodeString is Encode into a string, failing the test on an error.
func encodeString(t *testing.T, m *Map) string {
	t.Helper()
	var b bytes.Buffer
	if err := Encode(&b, m); err != nil {
		t.Fatal(err)
	}
	return b.String()
}

func TestEncodeWritesTheCanonicalFormAndParsesBackIdentically(t *testing.T) {
	t.Parallel()
	first := encodeString(t, mustParseSample(t))
	again, err := Parse([]byte(first))
	if err != nil {
		t.Fatalf("Parse(Encode(sample)): %v", err)
	}
	if second := encodeString(t, again); second != first {
		t.Errorf("Encode(Parse(Encode(m))) differs from Encode(m):\n%s\n--- against ---\n%s", first, second)
	}
	if n := strings.Count(first, `{"name":"`); n != sampleComponents {
		t.Errorf("%d component lines, want %d (one component per line)", n, sampleComponents)
	}
	root := strings.Index(first, `"dir":""`)
	srcA := strings.Index(first, `"dir":"src/a"`)
	tools := strings.Index(first, `"dir":"tools"`)
	if root < 0 || srcA < 0 || tools < 0 || !(root < srcA && srcA < tools) {
		t.Errorf("components are not sorted by dir: root=%d src/a=%d tools=%d", root, srcA, tools)
	}
	if !strings.HasPrefix(first, "{\n \"schema\": 2,\n") {
		t.Errorf("header is not the canonical one: %.40q", first)
	}
}

func TestEncodeSortsTheListsWithoutChangingTheMap(t *testing.T) {
	t.Parallel()
	m, err := Parse([]byte(`{"schema": 2, "targets": {"build": ["just", "build"]}, "components": [{"name": "src/a", "dir": "src/a", "files": {"src/a/x.cpp": ["gtest:test_a", "build"]}}]}`))
	if err != nil {
		t.Fatal(err)
	}
	out := encodeString(t, m)
	if !strings.Contains(out, `"src/a/x.cpp":["build","gtest:test_a"]`) {
		t.Errorf("Encode did not put the list in Order's order:\n%s", out)
	}
	if got := m.Components[0].Files["src/a/x.cpp"]; !reflect.DeepEqual(got, []string{"gtest:test_a", "build"}) {
		t.Errorf("Encode changed the map's own list: %q", got)
	}
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
	direct := map[string]bool{}
	for _, f := range strings.Split(output(t, root, "git", "ls-files", "-z"), "\x00") {
		if f == "" {
			continue
		}
		tracked[f] = true
		direct[dirOf(f)] = true
		if _, ok := m.Lookup(f); !ok {
			t.Errorf("tracked file %s is in neither files nor none of its directory's component: add it to %s", f, DefaultPath)
		}
	}
	for _, c := range m.Components {
		if !direct[c.Dir] {
			t.Errorf("component %q: its directory %q holds no tracked file", c.Name, c.Dir)
		}
		for f := range c.Files {
			if !tracked[f] {
				t.Errorf("component %q: files entry %s is not a tracked file", c.Name, f)
			}
		}
		for f := range c.None {
			if !tracked[f] {
				t.Errorf("component %q: none entry %s is not a tracked file", c.Name, f)
			}
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
	var uses []string
	for _, c := range m.Components {
		for _, ids := range c.Files {
			uses = append(uses, ids...)
		}
	}
	for _, s := range m.Scanners {
		uses = append(uses, s.Tests...)
	}
	used := map[string]bool{}
	for _, id := range uses {
		used[id] = true
		kind, name, _ := strings.Cut(id, kindSeparator)
		switch {
		case m.Targets[id] != nil:
		case kind == KindGTest:
			if !regexp.MustCompile(`add_executable\(` + regexp.QuoteMeta(name) + `\b`).Match(cmake) {
				t.Errorf("%s is not an add_executable of src/runtime/CMakeLists.txt", id)
			}
		case kind == KindJust:
			if !recipes[name] {
				t.Errorf("%s is not a recipe of the justfile", id)
			}
		case kind == KindGo:
			if gofiles, _ := filepath.Glob(filepath.Join(root, "tools", name, "*.go")); len(gofiles) == 0 {
				t.Errorf("%s is not a Go package under tools/", id)
			}
		case kind == KindPytest:
			if !tracked["tools/tests/"+name+".py"] {
				t.Errorf("%s is not a tracked file tools/tests/%s.py", id, name)
			}
		default:
			t.Errorf("%s is neither a target of the map nor a kind", id)
		}
	}
	for _, match := range regexp.MustCompile(`add_executable\((test_[A-Za-z0-9_]+)`).FindAllSubmatch(cmake, -1) {
		if id := KindGTest + kindSeparator + string(match[1]); !used[id] {
			t.Errorf("add_executable(%s) in src/runtime/CMakeLists.txt is used by no file or scanner: name it in %s", match[1], DefaultPath)
		}
	}
	for name := range m.Targets {
		if !used[name] {
			t.Errorf("target %q is used by no file or scanner", name)
		}
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
