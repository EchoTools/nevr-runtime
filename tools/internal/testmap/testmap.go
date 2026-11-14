// Package testmap reads tools/test_map.json (schema 2) and writes it back in its canonical form. The map is the one
// `just test-changed` (tools/cmd/test-changed) reads and `just test-map` (tools/cmd/test-map) writes.
//
// The map is a hierarchy. A file belongs to the component of its own directory, directly: a component's files and none
// hold the tracked files directly in its dir, and a path whose directory has no component, or that is in neither of
// its maps, is not covered. A file's tests are its own list (none has none) plus the tests of every scanner with a
// glob that matches it: a scanner is a check that reads every file its globs match, wherever the file lives. A test id
// is one of the map's own targets (an argv, where {preset} stands for the build preset), or a kind and a name:
//
//	gtest:<binary>   just test-auth-unit <binary>
//	just:<recipe>    just <recipe>
//	go:<package>     go -C tools test -count=1 ./<package> (never a cached result)
//	pytest:<name>    python3 tools/run_tool_tests.py tools/tests/<name>.py
package testmap

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path"
	"sort"
	"strings"
)

// Schema is the only map format this package reads.
const Schema = 2

// DefaultPath is the map's place in the repository.
const DefaultPath = "tools/test_map.json"

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

// Parse reads and checks a map. It refuses an unknown key, data after the object, any schema but 2, a target with no
// name or no argv, a component with no name or a name or directory another component has, a path that is not clean or
// is in another directory than its component, a path in both files and none, an empty test list or reason, a test id
// listed twice or one that Command cannot resolve, and a scanner with no name, a repeated name, no globs, an invalid
// glob, no tests or a test that does not resolve.
func Parse(data []byte) (*Map, error) {
	// The schema is read before anything else, so a map of another schema is refused by its number, not by a key
	// that schema has and this one does not.
	var version struct {
		Schema int `json:"schema"`
	}
	if err := json.NewDecoder(bytes.NewReader(data)).Decode(&version); err != nil {
		return nil, err
	}
	if version.Schema != Schema {
		return nil, fmt.Errorf("schema is %d; this reader knows %d", version.Schema, Schema)
	}
	dec := json.NewDecoder(bytes.NewReader(data))
	dec.DisallowUnknownFields()
	var m Map
	if err := dec.Decode(&m); err != nil {
		return nil, err
	}
	if _, err := dec.Token(); err != io.EOF {
		return nil, fmt.Errorf("the map has data after its object")
	}
	for _, name := range sortedKeys(m.Targets) {
		argv := m.Targets[name]
		if name == "" || strings.Contains(name, kindSeparator) {
			return nil, fmt.Errorf("target %q: a target's name is not empty and has no ':' (that is a kind's)", name)
		}
		if len(argv) == 0 || argv[0] == "" {
			return nil, fmt.Errorf("target %q has no command", name)
		}
	}
	m.byDir = map[string]int{}
	names := map[string]bool{}
	for i := range m.Components {
		c := &m.Components[i]
		if c.Name == "" {
			return nil, fmt.Errorf("component %d has no name", i)
		}
		if names[c.Name] {
			return nil, fmt.Errorf("component name %q is used twice", c.Name)
		}
		names[c.Name] = true
		if j, ok := m.byDir[c.Dir]; ok {
			return nil, fmt.Errorf("directory %q is the component %q and %q: one component per directory", c.Dir, m.Components[j].Name, c.Name)
		}
		m.byDir[c.Dir] = i
		if err := m.checkComponent(c); err != nil {
			return nil, err
		}
	}
	scanners := map[string]bool{}
	for i, s := range m.Scanners {
		if s.Name == "" {
			return nil, fmt.Errorf("scanner %d has no name", i)
		}
		if scanners[s.Name] {
			return nil, fmt.Errorf("scanner %q is named twice", s.Name)
		}
		scanners[s.Name] = true
		if len(s.Globs) == 0 {
			return nil, fmt.Errorf("scanner %q has no globs", s.Name)
		}
		for _, g := range s.Globs {
			if err := checkGlob(g); err != nil {
				return nil, fmt.Errorf("scanner %q: %w", s.Name, err)
			}
		}
		if len(s.Tests) == 0 {
			return nil, fmt.Errorf("scanner %q has no tests", s.Name)
		}
		if err := m.checkTests(s.Tests); err != nil {
			return nil, fmt.Errorf("scanner %q: %w", s.Name, err)
		}
	}
	return &m, nil
}

// checkComponent refuses a component whose directory, name, paths or tests cannot describe the directory it names.
func (m *Map) checkComponent(c *Component) error {
	if c.Dir != rootDir && !cleanRel(c.Dir) {
		return fmt.Errorf("component %q: dir %q is not a clean repository-relative path", c.Name, c.Dir)
	}
	want := c.Dir
	if c.Dir == rootDir {
		want = rootName
	}
	if c.Name != want {
		return fmt.Errorf("component %q: name must be %q", c.Name, want)
	}
	for _, f := range sortedKeys(c.Files) {
		if err := checkFileKey(f, c.Dir); err != nil {
			return fmt.Errorf("component %q: %w", c.Name, err)
		}
		if len(c.Files[f]) == 0 {
			return fmt.Errorf("component %q: file %q has no tests", c.Name, f)
		}
		if err := m.checkTests(c.Files[f]); err != nil {
			return fmt.Errorf("component %q: file %q: %w", c.Name, f, err)
		}
	}
	for _, f := range sortedKeys(c.None) {
		if err := checkFileKey(f, c.Dir); err != nil {
			return fmt.Errorf("component %q: %w", c.Name, err)
		}
		if _, both := c.Files[f]; both {
			return fmt.Errorf("component %q: file %q is in files and in none", c.Name, f)
		}
		if c.None[f] == "" {
			return fmt.Errorf("component %q: file %q has an empty reason in none", c.Name, f)
		}
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

// Load reads and checks the map at file.
func Load(file string) (*Map, error) {
	data, err := os.ReadFile(file)
	if err != nil {
		return nil, fmt.Errorf("test map: %w", err)
	}
	m, err := Parse(data)
	if err != nil {
		return nil, fmt.Errorf("test map %s: %w", file, err)
	}
	return m, nil
}

// Lookup returns what the map says about file; ok is false when the map does not cover it.
func (m *Map) Lookup(file string) (Lookup, bool) {
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

// Encode writes m in the canonical form: the header with one field to a line, each target and scanner as indented
// JSON, then components with one component to a line, sorted by dir, with the keys of files and none sorted (JSON
// encodes a map's keys sorted) and each test list in Order's order. Encode(Parse(Encode(m))) is byte-identical to
// Encode(m). It does not change m.
func Encode(w io.Writer, m *Map) error {
	var b bytes.Buffer
	scanners := make([]Scanner, len(m.Scanners))
	for i, s := range m.Scanners {
		scanners[i] = Scanner{Name: s.Name, Globs: s.Globs, Tests: m.Order(s.Tests)}
	}
	fields := []struct {
		key   string
		value any
	}{
		{"schema", m.Schema},
		{"about", m.About},
		{"targets", m.Targets},
		{"scanners", scanners},
	}
	b.WriteString("{\n")
	for _, f := range fields {
		v, err := marshal(f.value, " ", " ")
		if err != nil {
			return err
		}
		fmt.Fprintf(&b, " %q: %s,\n", f.key, v)
	}
	comps := canonicalComponents(m)
	if len(comps) == 0 {
		b.WriteString(` "components": []` + "\n}\n")
	} else {
		b.WriteString(` "components": [` + "\n")
		for i, c := range comps {
			line, err := marshal(c, "", "")
			if err != nil {
				return err
			}
			b.WriteString("  ")
			b.Write(line)
			if i < len(comps)-1 {
				b.WriteByte(',')
			}
			b.WriteByte('\n')
		}
		b.WriteString(" ]\n}\n")
	}
	_, err := w.Write(b.Bytes())
	return err
}

// canonicalComponents is m's components in Encode's order, with each test list in Order's order. The copies are new,
// so m is not changed.
func canonicalComponents(m *Map) []Component {
	out := make([]Component, 0, len(m.Components))
	for _, c := range m.Components {
		cc := Component{Name: c.Name, Dir: c.Dir, None: c.None}
		if len(c.Files) > 0 {
			cc.Files = make(map[string][]string, len(c.Files))
			for f, ids := range c.Files {
				cc.Files[f] = m.Order(ids)
			}
		}
		out = append(out, cc)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Dir < out[j].Dir })
	return out
}

// marshal is v as indented JSON, without HTML escaping (a reason may hold & or <), without the final newline.
func marshal(v any, prefix, indent string) ([]byte, error) {
	var b bytes.Buffer
	enc := json.NewEncoder(&b)
	enc.SetEscapeHTML(false)
	enc.SetIndent(prefix, indent)
	if err := enc.Encode(v); err != nil {
		return nil, err
	}
	return bytes.TrimSuffix(b.Bytes(), []byte("\n")), nil
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
