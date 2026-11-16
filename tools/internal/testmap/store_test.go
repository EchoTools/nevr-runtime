package testmap

import (
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
)

const (
	okHeader    = `{"schema": 2, "targets": {"build": ["just", "build"]}}`
	okComponent = `{"component":"a","dir":"a"}` + "\n" + `{"file":"a/x","tests":["just:x"]}` + "\n"
)

func withComponent(name, content string) map[string][]byte {
	return map[string][]byte{HeaderFile: []byte(okHeader), ComponentFile("a"): []byte(okComponent), name: []byte(content)}
}

// A map's files are the only thing the loader has, so each way they can contradict each other, or a path be listed
// twice, is refused by name.
func TestParseFilesRefusesWhatTheLookupsCouldNotTrust(t *testing.T) {
	t.Parallel()
	cases := []struct {
		name  string
		files map[string][]byte
		want  string
	}{
		{"a path listed twice in one component", map[string][]byte{HeaderFile: []byte(okHeader),
			ComponentFile("a"): []byte(`{"component":"a","dir":"a"}` + "\n" + `{"file":"a/x","tests":["just:x"]}` + "\n" + `{"file":"a/x","tests":["just:y"]}` + "\n")},
			`file "a/x" is listed twice`},
		{"a path in tests and in none, in two lines", map[string][]byte{HeaderFile: []byte(okHeader),
			ComponentFile("a"): []byte(`{"component":"a","dir":"a"}` + "\n" + `{"file":"a/x","tests":["just:x"]}` + "\n" + `{"file":"a/x","none":"why"}` + "\n")},
			"is listed twice"},
		{"a record with tests and none", map[string][]byte{HeaderFile: []byte(okHeader),
			ComponentFile("a"): []byte(`{"component":"a","dir":"a"}` + "\n" + `{"file":"a/x","tests":["just:x"],"none":"why"}` + "\n")},
			"has tests and a reason"},
		{"a record with neither", map[string][]byte{HeaderFile: []byte(okHeader),
			ComponentFile("a"): []byte(`{"component":"a","dir":"a"}` + "\n" + `{"file":"a/x"}` + "\n")},
			"has neither tests nor"},
		{"a record with an unknown key", map[string][]byte{HeaderFile: []byte(okHeader),
			ComponentFile("a"): []byte(`{"component":"a","dir":"a"}` + "\n" + `{"file":"a/x","tests":["just:x"],"extra":1}` + "\n")},
			"unknown field"},
		{"a blank line", map[string][]byte{HeaderFile: []byte(okHeader),
			ComponentFile("a"): []byte(`{"component":"a","dir":"a"}` + "\n\n" + `{"file":"a/x","tests":["just:x"]}` + "\n")},
			"line 2"},
		{"no final newline", map[string][]byte{HeaderFile: []byte(okHeader),
			ComponentFile("a"): []byte(`{"component":"a","dir":"a"}` + "\n" + `{"file":"a/x","tests":["just:x"]}`)},
			"no newline"},
		{"a component with no files", map[string][]byte{HeaderFile: []byte(okHeader),
			ComponentFile("a"): []byte(`{"component":"a","dir":"a"}` + "\n")},
			"at least one file"},
		{"the same component in a second file", withComponent("components/a-copy.jsonl", okComponent),
			"a component is in one file"},
		{"a component in the file of another", withComponent(ComponentFile("b"), okComponent),
			"a component is in one file"},
		{"a file that is not a component file", withComponent("components/notes.txt", "x"), "is not map.json"},
		{"a file outside components", withComponent("extra.jsonl", "x"), "is not map.json"},
		{"a file in a subdirectory of components", withComponent("components/x/y.jsonl", "x"), "is not map.json"},
		{"no header", map[string][]byte{ComponentFile("a"): []byte(okComponent)}, "map.json is missing"},
		{"a path of another directory", map[string][]byte{HeaderFile: []byte(okHeader),
			ComponentFile("a"): []byte(`{"component":"a","dir":"a"}` + "\n" + `{"file":"b/x","tests":["just:x"]}` + "\n")},
			"is in directory"},
		{"a directory with the separator the file name uses", map[string][]byte{HeaderFile: []byte(okHeader),
			ComponentFile("a~b"): []byte(`{"component":"a~b","dir":"a~b"}` + "\n" + `{"file":"a~b/x","tests":["just:x"]}` + "\n")},
			`has a "~"`},
	}
	for _, c := range cases {
		if _, err := ParseFiles(c.files); err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: ParseFiles error = %v, want one containing %q", c.name, err, c.want)
		}
	}
	if _, err := ParseFiles(map[string][]byte{HeaderFile: []byte(okHeader), ComponentFile("a"): []byte(okComponent)}); err != nil {
		t.Errorf("the control map is refused: %v", err)
	}
}

func TestLoadReadsTheDirectoryAndNamesWhatItRefuses(t *testing.T) {
	t.Parallel()
	dir := filepath.Join(t.TempDir(), "map")
	if err := Write(dir, encodeFiles(t, mustParseSample(t))); err != nil {
		t.Fatal(err)
	}
	m, err := Load(dir)
	if err != nil {
		t.Fatal(err)
	}
	if lk, ok := m.Lookup("src/a/x.cpp"); !ok || lk.Component.Name != "src/a" {
		t.Errorf("Lookup(src/a/x.cpp) after Load = %v, %v", lk, ok)
	}
	copyOf, err := os.ReadFile(filepath.Join(dir, ComponentFile("src/a")))
	if err != nil {
		t.Fatal(err)
	}
	dup := filepath.Join(dir, ComponentsDir, "copy.jsonl")
	if err := os.WriteFile(dup, copyOf, fileMode); err != nil {
		t.Fatal(err)
	}
	if _, err := Load(dir); err == nil || !strings.Contains(err.Error(), dir) || !strings.Contains(err.Error(), "a component is in one file") {
		t.Errorf("Load of a directory with a component twice = %v, want an error naming %s and the duplicate", err, dir)
	}
	if _, err := Load(filepath.Join(dir, "absent")); err == nil {
		t.Errorf("Load of an absent directory = nil error")
	}
	if _, err := Load(t.TempDir()); err == nil {
		t.Errorf("Load of an empty directory = nil error, want the header's absence")
	}
}

func TestWriteReplacesTheFilesAndRemovesTheComponentsTheMapNoLongerHas(t *testing.T) {
	t.Parallel()
	dir := filepath.Join(t.TempDir(), "map")
	m := mustParseSample(t)
	if err := Write(dir, encodeFiles(t, m)); err != nil {
		t.Fatal(err)
	}
	have, err := ReadFiles(dir)
	if err != nil || Diff(have, encodeFiles(t, m)) != "" {
		t.Fatalf("ReadFiles after Write: %v, differs: %q", err, Diff(have, encodeFiles(t, m)))
	}
	smaller := mustParseFixture(t, `{"schema": 2, "targets": {"build": ["just", "build"]}, "components": [{"name": "tools", "dir": "tools", "files": {"tools/x.go": ["build"]}}]}`)
	if err := Write(dir, encodeFiles(t, smaller)); err != nil {
		t.Fatal(err)
	}
	left, err := ReadFiles(dir)
	if err != nil {
		t.Fatal(err)
	}
	if got := sortedKeys(left); !reflect.DeepEqual(got, []string{ComponentFile("tools"), HeaderFile}) {
		t.Errorf("files after a smaller map is written = %q, want the header and tools", got)
	}
}

func TestDiffNamesTheFirstFileAndLineThatDiffer(t *testing.T) {
	t.Parallel()
	a := map[string][]byte{HeaderFile: []byte("h\n"), "components/a.jsonl": []byte("1\n2\n"), "components/b.jsonl": []byte("1\n")}
	b := map[string][]byte{HeaderFile: []byte("h\n"), "components/a.jsonl": []byte("1\n2\n"), "components/b.jsonl": []byte("1\n")}
	if d := Diff(a, b); d != "" {
		t.Errorf("Diff of equal maps = %q", d)
	}
	b["components/a.jsonl"] = []byte("1\nX\n")
	if d := Diff(a, b); d != "components/a.jsonl differs at line 2" {
		t.Errorf("Diff = %q", d)
	}
	delete(b, "components/b.jsonl")
	b["components/a.jsonl"] = a["components/a.jsonl"]
	if d := Diff(a, b); d != "components/b.jsonl is not in the map the generator writes" {
		t.Errorf("Diff of a map with a file the other lacks = %q", d)
	}
	if d := Diff(b, a); d != "components/b.jsonl is missing" {
		t.Errorf("Diff of a map that lacks a file = %q", d)
	}
}

func TestUncoveredAndStaleHoldTheMapAgainstTheTrackedFiles(t *testing.T) {
	t.Parallel()
	m := mustParseSample(t)
	tracked := []string{"src/a/x.cpp", "src/a/y.h", "src/a/README.md", "src/a/b/c.cpp", "tools/x.go", "go.mod", "LICENSE"}
	if u, s := m.Uncovered(tracked), m.Stale(tracked); len(u) != 0 || len(s) != 0 {
		t.Fatalf("the sample against its own files: uncovered %q, stale %q", u, s)
	}
	if got := m.Uncovered(append([]string{"src/a/new.cpp", DefaultPath + "/components/x.jsonl"}, tracked...)); !reflect.DeepEqual(got, []string{"src/a/new.cpp"}) {
		t.Errorf("Uncovered = %q, want only src/a/new.cpp (the map's own files are covered by the loader)", got)
	}
	if got := m.Stale(tracked[1:]); !reflect.DeepEqual(got, []string{"src/a/x.cpp"}) {
		t.Errorf("Stale = %q, want only src/a/x.cpp", got)
	}
}

func TestLookupCoversTheMapsOwnFilesWithItsOwnPackage(t *testing.T) {
	t.Parallel()
	m := mustParseSample(t)
	for _, file := range []string{DefaultPath + "/" + HeaderFile, DefaultPath + "/components/src~a.jsonl"} {
		lk, ok := m.Lookup(file)
		if !ok || !reflect.DeepEqual(lk.Tests, []string{"go:internal/testmap"}) || lk.Component.Name != DefaultPath {
			t.Errorf("Lookup(%q) = %+v, %v, want go:internal/testmap in the component %q", file, lk, ok, DefaultPath)
		}
	}
	if lk, ok := m.Lookup("tools/test_map.json"); !ok || !reflect.DeepEqual(lk.Tests, []string{"go:internal/testmap"}) {
		t.Errorf("the path of the single file the directory replaced is not covered by the map's own package: %+v, %v", lk, ok)
	}
	if _, ok := m.Lookup(DefaultPath + ".yaml"); ok {
		t.Errorf("a path that only starts like the map directory is covered")
	}
}
