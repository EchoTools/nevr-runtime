package testmap

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// This file is the map's form on disk (see the package comment): ParseFiles and Load read it and refuse what the
// lookups could not trust, Encode and Write write the one canonical form, Diff names where two forms differ, and
// Uncovered and Stale hold a map against a list of tracked files.

// The permissions of the map's files and directories.
const (
	fileMode = 0o644
	dirMode  = 0o755
)

// header is map.json: everything of the map but its components.
type header struct {
	Schema   int                 `json:"schema"`
	About    []string            `json:"about"`
	Targets  map[string][]string `json:"targets"`
	Scanners []Scanner           `json:"scanners"`
}

// componentHead is the first line of a component file.
type componentHead struct {
	Component string `json:"component"`
	Dir       string `json:"dir"`
}

// fileRecord is one line of a component file after the first, as it is read: a record with neither tests nor none, or
// with both, is refused, so the two are pointers (an empty list or an empty reason is a different refusal).
type fileRecord struct {
	File  string    `json:"file"`
	Tests *[]string `json:"tests"`
	None  *string   `json:"none"`
}

// writtenRecord is a fileRecord as it is written.
type writtenRecord struct {
	File  string   `json:"file"`
	Tests []string `json:"tests,omitempty"`
	None  string   `json:"none,omitempty"`
}

// ComponentFile is the name, relative to the map directory, of the file that holds the component called name.
func ComponentFile(name string) string {
	return ComponentsDir + pathSeparator + strings.ReplaceAll(name, pathSeparator, nameSlash) + ComponentExt
}

// ParseFiles reads and checks a map given as its files: the names relative to the map directory (HeaderFile, and
// ComponentFile of each component) and their contents. It refuses a header with an unknown key, data after its object,
// any schema but 2, a target with no name or no argv, and a scanner with no name, a repeated name, no globs, an
// invalid glob, no tests or a test that does not resolve; a file that is neither the header nor a component file; a
// component file that does not hold the component whose file it is (so a component is in one file, and a name is not
// in two), that has no files, or has a line that is blank, not one object or has an unknown key; a component whose
// directory is not clean or is not its name, a path that is not clean or is in another directory than its component, a
// path listed twice or in both tests and none, an empty test list or reason, a test id listed twice or one that
// Command cannot resolve.
func ParseFiles(files map[string][]byte) (*Map, error) {
	data, ok := files[HeaderFile]
	if !ok {
		return nil, fmt.Errorf("%s is missing", HeaderFile)
	}
	// The schema is read before anything else, so a map of another schema is refused by its number, not by a key
	// that schema has and this one does not.
	var version struct {
		Schema int `json:"schema"`
	}
	if err := json.NewDecoder(bytes.NewReader(data)).Decode(&version); err != nil {
		return nil, fmt.Errorf("%s: %w", HeaderFile, err)
	}
	if version.Schema != Schema {
		return nil, fmt.Errorf("%s: schema is %d; this reader knows %d", HeaderFile, version.Schema, Schema)
	}
	var h header
	if err := decodeStrict(data, &h); err != nil {
		return nil, fmt.Errorf("%s: %w", HeaderFile, err)
	}
	m := Map{Schema: h.Schema, About: h.About, Targets: h.Targets, Scanners: h.Scanners, byDir: map[string]int{}}
	for _, name := range sortedKeys(m.Targets) {
		argv := m.Targets[name]
		if name == "" || strings.Contains(name, kindSeparator) {
			return nil, fmt.Errorf("target %q: a target's name is not empty and has no ':' (that is a kind's)", name)
		}
		if len(argv) == 0 || argv[0] == "" {
			return nil, fmt.Errorf("target %q has no command", name)
		}
	}
	if err := m.checkScanners(); err != nil {
		return nil, err
	}
	for _, name := range sortedKeys(files) {
		if name == HeaderFile {
			continue
		}
		if !strings.HasPrefix(name, ComponentsDir+pathSeparator) || !strings.HasSuffix(name, ComponentExt) ||
			strings.Contains(strings.TrimPrefix(name, ComponentsDir+pathSeparator), pathSeparator) {
			return nil, fmt.Errorf("%s is not %s or a %s<name>%s file", name, HeaderFile, ComponentsDir+pathSeparator, ComponentExt)
		}
		c, err := m.parseComponentFile(name, files[name])
		if err != nil {
			return nil, err
		}
		m.byDir[c.Dir] = len(m.Components)
		m.Components = append(m.Components, *c)
	}
	sort.Slice(m.Components, func(i, j int) bool { return m.Components[i].Dir < m.Components[j].Dir })
	for i := range m.Components {
		m.byDir[m.Components[i].Dir] = i
	}
	return &m, nil
}

// checkScanners refuses a scanner with no name, a repeated name, no globs, an invalid glob, no tests or a test that
// does not resolve.
func (m *Map) checkScanners() error {
	scanners := map[string]bool{}
	for i, s := range m.Scanners {
		if s.Name == "" {
			return fmt.Errorf("scanner %d has no name", i)
		}
		if scanners[s.Name] {
			return fmt.Errorf("scanner %q is named twice", s.Name)
		}
		scanners[s.Name] = true
		if len(s.Globs) == 0 {
			return fmt.Errorf("scanner %q has no globs", s.Name)
		}
		for _, g := range s.Globs {
			if err := checkGlob(g); err != nil {
				return fmt.Errorf("scanner %q: %w", s.Name, err)
			}
		}
		if len(s.Tests) == 0 {
			return fmt.Errorf("scanner %q has no tests", s.Name)
		}
		if err := m.checkTests(s.Tests); err != nil {
			return fmt.Errorf("scanner %q: %w", s.Name, err)
		}
	}
	return nil
}

// parseComponentFile reads the component file called name and checks the component it holds.
func (m *Map) parseComponentFile(name string, data []byte) (*Component, error) {
	lines := bytes.Split(data, []byte("\n"))
	if len(lines[len(lines)-1]) != 0 {
		return nil, fmt.Errorf("%s: the last line has no newline", name)
	}
	lines = lines[:len(lines)-1]
	if len(lines) < 2 {
		return nil, fmt.Errorf("%s: a component file has its component's line and at least one file's", name)
	}
	var head componentHead
	if err := decodeStrict(lines[0], &head); err != nil {
		return nil, fmt.Errorf("%s: line 1: %w", name, err)
	}
	if want := ComponentFile(head.Component); want != name {
		return nil, fmt.Errorf("%s holds the component %q, whose file is %s: a component is in one file, named for it", name, head.Component, want)
	}
	c := Component{Name: head.Component, Dir: head.Dir, Files: map[string][]string{}, None: map[string]string{}}
	if c.Name == "" {
		return nil, fmt.Errorf("%s: the component has no name", name)
	}
	if err := checkComponentDir(&c); err != nil {
		return nil, err
	}
	for i, line := range lines[1:] {
		var r fileRecord
		if err := decodeStrict(line, &r); err != nil {
			return nil, fmt.Errorf("%s: line %d: %w", name, i+2, err)
		}
		if err := checkFileKey(r.File, c.Dir); err != nil {
			return nil, fmt.Errorf("component %q: line %d: %w", c.Name, i+2, err)
		}
		_, inFiles := c.Files[r.File]
		_, inNone := c.None[r.File]
		if inFiles || inNone {
			return nil, fmt.Errorf("component %q: file %q is listed twice", c.Name, r.File)
		}
		switch {
		case r.Tests != nil && r.None != nil:
			return nil, fmt.Errorf("component %q: file %q has tests and a reason it has none", c.Name, r.File)
		case r.Tests == nil && r.None == nil:
			return nil, fmt.Errorf("component %q: file %q has neither tests nor a reason it has none", c.Name, r.File)
		case r.Tests != nil:
			if len(*r.Tests) == 0 {
				return nil, fmt.Errorf("component %q: file %q has no tests", c.Name, r.File)
			}
			if err := m.checkTests(*r.Tests); err != nil {
				return nil, fmt.Errorf("component %q: file %q: %w", c.Name, r.File, err)
			}
			c.Files[r.File] = *r.Tests
		default:
			if *r.None == "" {
				return nil, fmt.Errorf("component %q: file %q has an empty reason in none", c.Name, r.File)
			}
			c.None[r.File] = *r.None
		}
	}
	if len(c.Files) == 0 {
		c.Files = nil
	}
	if len(c.None) == 0 {
		c.None = nil
	}
	return &c, nil
}

// decodeStrict reads one JSON value from data into v, refusing an unknown key and anything after the value.
func decodeStrict(data []byte, v any) error {
	dec := json.NewDecoder(bytes.NewReader(data))
	dec.DisallowUnknownFields()
	if err := dec.Decode(v); err != nil {
		return err
	}
	if _, err := dec.Token(); err != io.EOF {
		return errors.New("data after the object")
	}
	return nil
}

// ReadFiles reads the map directory dir into the form ParseFiles takes. It reads only HeaderFile and the files of
// ComponentsDir; any other entry in either is read too and refused by ParseFiles by its name, and an entry that is a
// directory is an error here.
func ReadFiles(dir string) (map[string][]byte, error) {
	files := map[string][]byte{}
	read := func(rel string) error {
		data, err := os.ReadFile(filepath.Join(dir, filepath.FromSlash(rel)))
		if err != nil {
			return err
		}
		files[rel] = data
		return nil
	}
	if err := read(HeaderFile); err != nil {
		return nil, err
	}
	entries, err := os.ReadDir(filepath.Join(dir, ComponentsDir))
	if err != nil {
		return nil, err
	}
	for _, e := range entries {
		if err := read(ComponentsDir + pathSeparator + e.Name()); err != nil {
			return nil, err
		}
	}
	return files, nil
}

// Load reads and checks the map in the directory dir.
func Load(dir string) (*Map, error) {
	files, err := ReadFiles(dir)
	if err != nil {
		return nil, fmt.Errorf("test map: %w", err)
	}
	m, err := ParseFiles(files)
	if err != nil {
		return nil, fmt.Errorf("test map %s: %w", dir, err)
	}
	return m, nil
}

// Encode writes m in the canonical form, as its files: the header with one field to a line, each target and scanner as
// indented JSON, then one file for each component, whose lines are the component's, then each tracked file of it sorted
// by path, with each test list in Order's order. ParseFiles(Encode(m)) gives a map that Encodes to the same bytes. It
// does not change m.
func Encode(m *Map) (map[string][]byte, error) {
	files := map[string][]byte{}
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
	var b bytes.Buffer
	b.WriteString("{\n")
	for i, f := range fields {
		v, err := marshal(f.value, " ", " ")
		if err != nil {
			return nil, err
		}
		fmt.Fprintf(&b, " %q: %s", f.key, v)
		if i < len(fields)-1 {
			b.WriteByte(',')
		}
		b.WriteByte('\n')
	}
	b.WriteString("}\n")
	files[HeaderFile] = b.Bytes()
	for _, c := range m.Components {
		var cb bytes.Buffer
		line, err := marshal(componentHead{Component: c.Name, Dir: c.Dir}, "", "")
		if err != nil {
			return nil, err
		}
		cb.Write(line)
		cb.WriteByte('\n')
		paths := append(sortedKeys(c.Files), sortedKeys(c.None)...)
		sort.Strings(paths)
		for _, p := range paths {
			r := writtenRecord{File: p, None: c.None[p]}
			if ids, ok := c.Files[p]; ok {
				r.Tests = m.Order(ids)
			}
			if line, err = marshal(r, "", ""); err != nil {
				return nil, err
			}
			cb.Write(line)
			cb.WriteByte('\n')
		}
		files[ComponentFile(c.Name)] = cb.Bytes()
	}
	return files, nil
}

// Write writes the files of a map into the directory dir, each through a temporary file beside it so a reader never
// sees half a file, and removes the component files there that the map no longer has.
func Write(dir string, files map[string][]byte) error {
	if err := os.MkdirAll(filepath.Join(dir, ComponentsDir), dirMode); err != nil {
		return err
	}
	for _, rel := range sortedKeys(files) {
		target := filepath.Join(dir, filepath.FromSlash(rel))
		tmp := target + ".tmp"
		if err := os.WriteFile(tmp, files[rel], fileMode); err != nil {
			return err
		}
		if err := os.Rename(tmp, target); err != nil {
			return err
		}
	}
	entries, err := os.ReadDir(filepath.Join(dir, ComponentsDir))
	if err != nil {
		return err
	}
	for _, e := range entries {
		rel := ComponentsDir + pathSeparator + e.Name()
		if _, kept := files[rel]; !kept && !e.IsDir() {
			if err := os.Remove(filepath.Join(dir, ComponentsDir, e.Name())); err != nil {
				return err
			}
		}
	}
	return nil
}

// Diff names the first file, in name order, that differs between the map files have and the map files want, and the
// first line that differs in it; empty when they are the same.
func Diff(have, want map[string][]byte) string {
	names := map[string]bool{}
	for n := range have {
		names[n] = true
	}
	for n := range want {
		names[n] = true
	}
	for _, n := range sortedKeys(names) {
		h, hok := have[n]
		w, wok := want[n]
		switch {
		case !hok:
			return n + " is missing"
		case !wok:
			return n + " is not in the map the generator writes"
		case !bytes.Equal(h, w):
			hl, wl := strings.Split(string(h), "\n"), strings.Split(string(w), "\n")
			for i := 0; i < len(hl) || i < len(wl); i++ {
				var a, b string
				if i < len(hl) {
					a = hl[i]
				}
				if i < len(wl) {
					b = wl[i]
				}
				if a != b {
					return fmt.Sprintf("%s differs at line %d", n, i+1)
				}
			}
		}
	}
	return ""
}

// Uncovered is the files of tracked that the map does not cover, in the order given.
func (m *Map) Uncovered(tracked []string) []string {
	var out []string
	for _, f := range tracked {
		if _, ok := m.Lookup(f); !ok {
			out = append(out, f)
		}
	}
	return out
}

// Stale is the files and none entries of the map that are not in tracked, sorted: each is a path of the map that no
// file of the tree has.
func (m *Map) Stale(tracked []string) []string {
	have := map[string]bool{}
	for _, f := range tracked {
		have[f] = true
	}
	var out []string
	for _, c := range m.Components {
		for _, f := range append(sortedKeys(c.Files), sortedKeys(c.None)...) {
			if !have[f] {
				out = append(out, f)
			}
		}
	}
	sort.Strings(out)
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
