// Package testmap reads the test map (schema 2): the directory tools/test_map, one file for each component, and writes
// it back in its canonical form. The map is the one `just test-changed` (tools/cmd/test-changed) reads and `just
// test-map` (tools/cmd/test-map) writes.
//
// The map is a directory so that two changes to the tests of different code never edit the same file. It holds
// map.json (the schema, the about text, the targets and the scanners: written by the generator from its own rules, so
// a change to a test of some code does not touch it) and components/<name>.jsonl, one file for each component, whose
// name is the component's name with each "/" written "~". A component file is one JSON object to a line: the first
// names the component ({"component":..,"dir":..}), each next is one tracked file of its directory, sorted by path,
// either {"file":..,"tests":[..]} or {"file":..,"none":"<why no test reads it>"}. A change to the tests of one file is
// one line of one component's file, and a line of another file is never next to it.
//
// The map is a hierarchy. A file belongs to the component of its own directory, directly: a component's files and none
// hold the tracked files directly in its dir, and a path whose directory has no component, or that is in neither of
// its maps, is not covered. The files of the map's own directory (DefaultPath) are the exception: the loader covers
// them itself (Lookup), so a new component file is not a line in another component's file. A file's tests are its own
// list (none has none) plus the tests of every scanner with a glob that matches it: a scanner is a check that reads
// every file its globs match, wherever the file lives. A test id is one of the map's own targets (an argv, where
// {preset} stands for the build preset), or a kind and a name:
//
//	gtest:<binary>   just test-auth-unit <binary>
//	just:<recipe>    just <recipe>
//	go:<package>     go -C tools test -count=1 ./<package> (never a cached result)
//	pytest:<name>    python3 tools/run_tool_tests.py tools/tests/<name>.py
package testmap

import (
	"fmt"
	"path"
	"sort"
	"strings"
)

// Schema is the only map format this package reads.
const Schema = 2

// DefaultPath is the map's place in the repository: a directory.
const DefaultPath = "tools/test_map"

// The files of the map directory: the header, and the component files below ComponentsDir.
const (
	HeaderFile    = "map.json"
	ComponentsDir = "components"
	ComponentExt  = ".jsonl"
)

// ownTests are the tests of a file of the map directory: its own package reads it.
var ownTests = []string{KindGo + kindSeparator + "internal/testmap"}

// DefaultPreset is the build preset {preset} stands for when none is given: the justfile's default on Linux.
const DefaultPreset = "mingw-release"

// PresetToken is the text a target's argv carries where the build preset goes.
const PresetToken = "{preset}"

// BuildTarget is the target that runs first: the others read what it builds.
const BuildTarget = "build"

// The kinds of test id, in the order a run takes them after the map's own targets.
const (
	KindGTest  = "gtest"
	KindJust   = "just"
	KindGo     = "go"
	KindPytest = "pytest"
)

var kindOrder = []string{KindGTest, KindJust, KindGo, KindPytest}

// The ranks Order sorts by: the build target, then the map's other targets, then each kind in kindOrder.
const (
	buildRank = iota
	targetRank
	kindRankBase
)

const (
	kindSeparator = ":"
	pathSeparator = "/"
	anySegments   = "**"
	rootDir       = ""
	rootName      = "(root)"
	currentDir    = "."
	nameSlash     = "~" // a component file's name has this where the component's name has pathSeparator
)

// Scanner is a check that reads every file its globs match, wherever the file lives.
type Scanner struct {
	Name  string   `json:"name"`
	Globs []string `json:"globs"`
	Tests []string `json:"tests"`
}

// Component is one directory of the tree: its files' tests, or why a file has none.
type Component struct {
	Name  string              `json:"name"`
	Dir   string              `json:"dir"`
	Files map[string][]string `json:"files,omitempty"`
	None  map[string]string   `json:"none,omitempty"`
}

// Map is a parsed, checked map.
type Map struct {
	Schema     int                 `json:"schema"`
	About      []string            `json:"about"`
	Targets    map[string][]string `json:"targets"`
	Scanners   []Scanner           `json:"scanners"`
	Components []Component         `json:"components"`

	byDir map[string]int
}

// Lookup is what the map says about one path.
type Lookup struct {
	Component *Component
	Own       []string
	None      string
	Scanners  []string
	Tests     []string
}

// checkComponentDir refuses a component whose directory or name cannot describe the directory it names.
func checkComponentDir(c *Component) error {
	if c.Dir != rootDir && !cleanRel(c.Dir) {
		return fmt.Errorf("component %q: dir %q is not a clean repository-relative path", c.Name, c.Dir)
	}
	if strings.Contains(c.Dir, nameSlash) {
		return fmt.Errorf("component %q: dir %q has a %q, which a component file's name uses for %q", c.Name, c.Dir, nameSlash, pathSeparator)
	}
	want := c.Dir
	if c.Dir == rootDir {
		want = rootName
	}
	if c.Name != want {
		return fmt.Errorf("component %q: name must be %q", c.Name, want)
	}
	return nil
}

// checkFileKey refuses a path that is not clean or is not in the component's directory.
func checkFileKey(file, dir string) error {
	if !cleanRel(file) {
		return fmt.Errorf("path %q is not a clean repository-relative path", file)
	}
	if got := dirOf(file); got != dir {
		return fmt.Errorf("path %q is in directory %q, not in the component's %q", file, got, dir)
	}
	return nil
}

// checkTests refuses a test list that names a test twice or names one Command cannot resolve.
func (m *Map) checkTests(ids []string) error {
	seen := map[string]bool{}
	for _, id := range ids {
		if seen[id] {
			return fmt.Errorf("test %q is listed twice", id)
		}
		seen[id] = true
		if _, err := m.Command(id, DefaultPreset); err != nil {
			return err
		}
	}
	return nil
}

// checkGlob refuses a glob with an empty segment or a segment that is not path.Match syntax.
func checkGlob(glob string) error {
	for _, seg := range strings.Split(glob, pathSeparator) {
		if seg == "" {
			return fmt.Errorf("glob %q has an empty segment", glob)
		}
		if seg == anySegments {
			continue
		}
		if _, err := path.Match(seg, ""); err != nil {
			return fmt.Errorf("glob %q: segment %q is not path.Match syntax: %w", glob, seg, err)
		}
	}
	return nil
}

// cleanRel reports whether p is a relative path in the form path.Clean gives, that does not leave the repository.
func cleanRel(p string) bool {
	return p != "" && path.Clean(p) == p && p != currentDir && p != ".." &&
		!strings.HasPrefix(p, "../") && !strings.HasPrefix(p, pathSeparator)
}

// dirOf is the directory a repository-relative path is in: rootDir for a file at the top of the repository.
func dirOf(file string) string {
	if d := path.Dir(file); d != currentDir {
		return d
	}
	return rootDir
}

// Lookup returns what the map says about file; ok is false when the map does not cover it.
func (m *Map) Lookup(file string) (Lookup, bool) {
	if IsMapFile(file) {
		return Lookup{Component: &Component{Name: DefaultPath, Dir: DefaultPath}, Own: append([]string(nil), ownTests...), Tests: append([]string(nil), ownTests...)}, true
	}
	i, ok := m.componentAt(dirOf(file))
	if !ok {
		return Lookup{}, false
	}
	c := &m.Components[i]
	var lk Lookup
	if tests, owned := c.Files[file]; owned {
		lk.Own = append([]string(nil), tests...)
	} else if reason, none := c.None[file]; none {
		lk.None = reason
	} else {
		return Lookup{}, false
	}
	lk.Component = c
	seen := map[string]bool{}
	add := func(ids []string) {
		for _, id := range ids {
			if !seen[id] {
				seen[id] = true
				lk.Tests = append(lk.Tests, id)
			}
		}
	}
	add(lk.Own)
	for _, s := range m.Scanners {
		if matchAny(s.Globs, file) {
			lk.Scanners = append(lk.Scanners, s.Name)
			add(s.Tests)
		}
	}
	return lk, true
}

// formerPath is the single file the map directory replaced. Its path is covered like the directory's files, so the
// change that deletes it is a change with a test (a deleted path is otherwise looked up in the map at the merge-base,
// and a merge-base from before the directory has no directory to look in).
const formerPath = "tools/test_map.json"

// IsMapFile reports whether file is a file of the map directory, which the loader covers itself, or the path the
// directory replaced.
func IsMapFile(file string) bool {
	return file == formerPath || strings.HasPrefix(file, DefaultPath+pathSeparator)
}

// componentAt is the index of the component of dir, found through the index Parse built, or by a scan of a map that
// was not made by Parse.
func (m *Map) componentAt(dir string) (int, bool) {
	if m.byDir != nil {
		i, ok := m.byDir[dir]
		return i, ok
	}
	for i := range m.Components {
		if m.Components[i].Dir == dir {
			return i, true
		}
	}
	return 0, false
}

// Command is the argv test id runs with preset in place of {preset}.
func (m *Map) Command(id, preset string) ([]string, error) {
	if argv, ok := m.Targets[id]; ok {
		out := make([]string, len(argv))
		for i, a := range argv {
			out[i] = strings.ReplaceAll(a, PresetToken, preset)
		}
		return out, nil
	}
	kind, name, ok := strings.Cut(id, kindSeparator)
	if !ok || name == "" {
		return nil, fmt.Errorf("test %q is neither a target of the map nor <kind>:<name>", id)
	}
	switch kind {
	case KindGTest:
		return []string{"just", "test-auth-unit", name}, nil
	case KindJust:
		return []string{"just", name}, nil
	case KindGo:
		return []string{"go", "-C", "tools", "test", "-count=1", "./" + name}, nil
	case KindPytest:
		return []string{"python3", "tools/run_tool_tests.py", "tools/tests/" + name + ".py"}, nil
	}
	return nil, fmt.Errorf("test %q: kind %q is not one of %s", id, kind, strings.Join(kindOrder, ", "))
}

// Order returns ids without repeats in the order a run takes them: the build target, the map's other targets, then
// each kind in turn (gtest, just, go, pytest), each group sorted. An id of an unknown kind comes last.
func (m *Map) Order(ids []string) []string {
	rank := func(id string) int {
		if id == BuildTarget {
			return buildRank
		}
		if _, ok := m.Targets[id]; ok {
			return targetRank
		}
		kind, _, _ := strings.Cut(id, kindSeparator)
		for i, k := range kindOrder {
			if k == kind {
				return kindRankBase + i
			}
		}
		return kindRankBase + len(kindOrder)
	}
	seen := map[string]bool{}
	var out []string
	for _, id := range ids {
		if !seen[id] {
			seen[id] = true
			out = append(out, id)
		}
	}
	sort.Slice(out, func(i, j int) bool {
		ri, rj := rank(out[i]), rank(out[j])
		if ri != rj {
			return ri < rj
		}
		return out[i] < out[j]
	})
	return out
}

// MatchGlob reports whether file matches glob (** is zero or more whole segments).
func MatchGlob(glob, file string) bool {
	return matchSegments(strings.Split(glob, pathSeparator), strings.Split(file, pathSeparator))
}

// matchAny reports whether file matches one of globs.
func matchAny(globs []string, file string) bool {
	for _, g := range globs {
		if MatchGlob(g, file) {
			return true
		}
	}
	return false
}

// matchSegments matches the glob's segments against the file's, a whole ** segment standing for any run of them.
func matchSegments(glob, file []string) bool {
	if len(glob) == 0 {
		return len(file) == 0
	}
	if glob[0] == anySegments {
		for k := 0; k <= len(file); k++ {
			if matchSegments(glob[1:], file[k:]) {
				return true
			}
		}
		return false
	}
	if len(file) == 0 {
		return false
	}
	ok, err := path.Match(glob[0], file[0])
	if err != nil || !ok {
		return false
	}
	return matchSegments(glob[1:], file[1:])
}

// sortedKeys is the keys of m in sorted order.
func sortedKeys[V any](m map[string]V) []string {
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	return keys
}
