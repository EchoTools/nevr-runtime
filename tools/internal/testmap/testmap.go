// Package testmap reads tools/test_map.json, the map `just test-changed` (tools/cmd/test-changed) reads: which
// component owns a path, and which tests a change to that component runs (AGENTS.md, "Builds").
//
// The map is a hierarchy: a path belongs to exactly one component, the one with the longest entry that matches it (an
// entry ending in "/" matches every path under that directory, any other entry matches that one file); the component
// lists its tests, or says in "none" why it has none. A test id is one of the map's own targets (an argv, where
// {preset} stands for the build preset), or a kind and a name:
//
//	gtest:<binary>   just test-auth-unit <binary>
//	just:<recipe>    just <recipe>
//	go:<package>     go -C tools test ./<package>
//	pytest:<name>    python3 tools/run_tool_tests.py tools/tests/<name>.py
package testmap

import (
	"bytes"
	"encoding/json"
	"fmt"
	"os"
	"path"
	"sort"
	"strings"
)

// Schema is the only map format this package reads.
const Schema = 1

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

// Component is one entry of the map's components.
type Component struct {
	Name  string   `json:"name"`
	Paths []string `json:"paths"`
	Tests []string `json:"tests"`
	None  string   `json:"none,omitempty"`
}

// Map is a parsed, checked map.
type Map struct {
	Schema     int                 `json:"schema"`
	About      []string            `json:"about"`
	Targets    map[string][]string `json:"targets"`
	Components []Component         `json:"components"`

	owner map[string]int
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

// Parse reads and checks a map: an unknown key, a component without paths, a path entry two components claim, a test
// id that names nothing, a component with neither tests nor a reason for none (or both), or a target with no argv is
// an error, so a map that loads is one every lookup can trust.
func Parse(data []byte) (*Map, error) {
	dec := json.NewDecoder(bytes.NewReader(data))
	dec.DisallowUnknownFields()
	var m Map
	if err := dec.Decode(&m); err != nil {
		return nil, err
	}
	if m.Schema != Schema {
		return nil, fmt.Errorf("schema is %d; this reader knows %d", m.Schema, Schema)
	}
	for name, argv := range m.Targets {
		if name == "" || strings.Contains(name, ":") {
			return nil, fmt.Errorf("target %q: a target's name is not empty and has no ':' (that is a kind's)", name)
		}
		if len(argv) == 0 || argv[0] == "" {
			return nil, fmt.Errorf("target %q has no command", name)
		}
	}
	m.owner = map[string]int{}
	names := map[string]bool{}
	for i, c := range m.Components {
		if c.Name == "" || names[c.Name] {
			return nil, fmt.Errorf("component %d: name %q is empty or used twice", i, c.Name)
		}
		names[c.Name] = true
		if len(c.Paths) == 0 {
			return nil, fmt.Errorf("component %q has no paths", c.Name)
		}
		for _, p := range c.Paths {
			if err := checkEntry(p); err != nil {
				return nil, fmt.Errorf("component %q: %w", c.Name, err)
			}
			if j, ok := m.owner[p]; ok {
				return nil, fmt.Errorf("path %q is in component %q and in %q: a path has one owner", p, m.Components[j].Name, c.Name)
			}
			m.owner[p] = i
		}
		if (len(c.Tests) == 0) == (c.None == "") {
			return nil, fmt.Errorf("component %q: a component lists its tests, or says in none why it has none; not both, not neither", c.Name)
		}
		seen := map[string]bool{}
		for _, t := range c.Tests {
			if seen[t] {
				return nil, fmt.Errorf("component %q lists test %q twice", c.Name, t)
			}
			seen[t] = true
			if _, err := m.Command(t, DefaultPreset); err != nil {
				return nil, fmt.Errorf("component %q: %w", c.Name, err)
			}
		}
	}
	return &m, nil
}

// checkEntry refuses an entry that cannot match a repository-relative path as git prints it.
func checkEntry(p string) error {
	clean := strings.TrimSuffix(p, "/")
	if clean == "" || path.Clean(clean) != clean || strings.HasPrefix(clean, "/") || clean == "." || strings.HasPrefix(clean, "../") || clean == ".." {
		return fmt.Errorf("path entry %q is not a clean repository-relative path", p)
	}
	return nil
}

// Owner returns the component that owns file (a repository-relative path, as git prints it) and the entry that
// matched: the file's own entry, else the entry of the deepest directory that holds it. ok is false when no entry
// matches.
func (m *Map) Owner(file string) (c *Component, entry string, ok bool) {
	if i, found := m.owner[file]; found {
		return &m.Components[i], file, true
	}
	for dir := path.Dir(file); dir != "." && dir != "/"; dir = path.Dir(dir) {
		if i, found := m.owner[dir+"/"]; found {
			return &m.Components[i], dir + "/", true
		}
	}
	return nil, "", false
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
	kind, name, ok := strings.Cut(id, ":")
	if !ok || name == "" {
		return nil, fmt.Errorf("test %q is neither a target of the map nor <kind>:<name>", id)
	}
	switch kind {
	case KindGTest:
		return []string{"just", "test-auth-unit", name}, nil
	case KindJust:
		return []string{"just", name}, nil
	case KindGo:
		return []string{"go", "-C", "tools", "test", "./" + name}, nil
	case KindPytest:
		return []string{"python3", "tools/run_tool_tests.py", "tools/tests/" + name + ".py"}, nil
	}
	return nil, fmt.Errorf("test %q: kind %q is not one of %s", id, kind, strings.Join(kindOrder, ", "))
}

// Order returns ids without repeats in the order a run takes them: the build target, the map's other targets, then
// each kind in turn (gtest, just, go, pytest), each group sorted.
func (m *Map) Order(ids []string) []string {
	rank := func(id string) int {
		if id == BuildTarget {
			return 0
		}
		if _, ok := m.Targets[id]; ok {
			return 1
		}
		kind, _, _ := strings.Cut(id, ":")
		for i, k := range kindOrder {
			if k == kind {
				return 2 + i
			}
		}
		return 2 + len(kindOrder)
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
