// Package testshelf reads tools/test_shelf.json: the one file that holds the ceiling every test runner of the repo holds a
// test to (AGENTS.md, "Builds") and the tests that are over it. A test over the ceiling is shelved, not rewritten: it stays
// in the repo, the runner that reads the shelf skips it with the entry's reason and date, and it comes back only when someone
// rewrites it to run fast. The Go runners (test-changed, test-go-tools, gtest-deadline) read the file through this package;
// the Python runner reads it through tools/shelf_reader.py, the Quest ctest tests through cmake/TestShelf.cmake and the
// justfile through `python3 tools/shelf_reader.py --ceiling`.
package testshelf

import (
	"bytes"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"time"
)

// DefaultPath is the shelf's place in the checkout.
const DefaultPath = "tools/test_shelf.json"

// Schema is the file's schema number.
const Schema = 1

// maxCeilingSeconds bounds a ceiling a file may name: the owner's order is 30 s, and a ceiling past a minute is not one.
const maxCeilingSeconds = 60

// Test kinds an id may carry.
const (
	KindJust   = "just"
	KindTarget = "target"
	KindPytest = "pytest"
	KindGo     = "go"
	KindGTest  = "gtest"
	KindCTest  = "ctest"
)

// kindsWithCases are the kinds whose id may name one test of a unit after '#'.
var kindsWithCases = map[string]bool{KindPytest: true, KindGo: true, KindGTest: true}

var knownKinds = []string{KindJust, KindTarget, KindPytest, KindGo, KindGTest, KindCTest}

var dateRE = regexp.MustCompile(`^\d{4}-\d{2}-\d{2}$`)

// Entry is one shelved test: its id, why it is over the ceiling (what was measured, over how many runs), where that was
// measured, and the day it was shelved.
type Entry struct {
	ID       string `json:"id"`
	Reason   string `json:"reason"`
	Measured string `json:"measured"`
	Date     string `json:"date"`
}

// Shelf is the parsed file.
type Shelf struct {
	Schema   int      `json:"schema"`
	About    []string `json:"about"`
	CeilingS int      `json:"ceiling_s"`
	Entries  []Entry  `json:"entries"`
}

// Split reads an id as kind, unit and case ("" when the id names the whole unit).
func Split(id string) (kind, unit, testCase string, err error) {
	kind, rest, ok := strings.Cut(id, ":")
	if !ok || rest == "" {
		return "", "", "", fmt.Errorf("id %q is not <kind>:<unit>", id)
	}
	known := false
	for _, k := range knownKinds {
		known = known || k == kind
	}
	if !known {
		return "", "", "", fmt.Errorf("id %q: kind %q is not one of %s", id, kind, strings.Join(knownKinds, ", "))
	}
	unit, testCase, hasCase := strings.Cut(rest, "#")
	if unit == "" || (hasCase && testCase == "") {
		return "", "", "", fmt.Errorf("id %q has an empty unit or an empty case", id)
	}
	if hasCase && !kindsWithCases[kind] {
		return "", "", "", fmt.Errorf("id %q: a %s has no cases, so the id cannot carry a #case", id, kind)
	}
	return kind, unit, testCase, nil
}

// Parse reads and checks a shelf: strict JSON (an unknown field is an error), the schema, a ceiling of 1 to 60 s, and for
// each entry a known id, a reason, a measured-where and a date; no id twice.
func Parse(data []byte) (*Shelf, error) {
	dec := json.NewDecoder(bytes.NewReader(data))
	dec.DisallowUnknownFields()
	var s Shelf
	if err := dec.Decode(&s); err != nil {
		return nil, fmt.Errorf("test shelf: %w", err)
	}
	if s.Schema != Schema {
		return nil, fmt.Errorf("test shelf: schema %d, want %d", s.Schema, Schema)
	}
	if s.CeilingS < 1 || s.CeilingS > maxCeilingSeconds {
		return nil, fmt.Errorf("test shelf: ceiling_s %d is not 1 to %d", s.CeilingS, maxCeilingSeconds)
	}
	seen := map[string]bool{}
	for i, e := range s.Entries {
		if _, _, _, err := Split(e.ID); err != nil {
			return nil, fmt.Errorf("test shelf: entry %d: %w", i, err)
		}
		if seen[e.ID] {
			return nil, fmt.Errorf("test shelf: id %s is listed twice", e.ID)
		}
		seen[e.ID] = true
		if strings.TrimSpace(e.Reason) == "" || strings.TrimSpace(e.Measured) == "" {
			return nil, fmt.Errorf("test shelf: %s has no reason or no measured-where: a shelved test says why", e.ID)
		}
		if !dateRE.MatchString(e.Date) {
			return nil, fmt.Errorf("test shelf: %s has date %q, want YYYY-MM-DD", e.ID, e.Date)
		}
		if _, err := time.Parse("2006-01-02", e.Date); err != nil {
			return nil, fmt.Errorf("test shelf: %s has date %q: %w", e.ID, e.Date, err)
		}
	}
	return &s, nil
}

// Load reads the shelf at path.
func Load(path string) (*Shelf, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("test shelf: %w", err)
	}
	return Parse(data)
}

// Ceiling is the longest a test may run.
func (s *Shelf) Ceiling() time.Duration { return time.Duration(s.CeilingS) * time.Second }

// CeilingArg is the ceiling as the go tool and timeout(1) spell it ("30s").
func (s *Shelf) CeilingArg() string { return fmt.Sprintf("%ds", s.CeilingS) }

// Lookup is the entry with exactly this id.
func (s *Shelf) Lookup(id string) (Entry, bool) {
	for _, e := range s.Entries {
		if e.ID == id {
			return e, true
		}
	}
	return Entry{}, false
}

// Shelved is the entry that keeps a unit-level id out of a run: the id itself, a target spelled bare (the map's own
// spelling of its targets, `host-seams`) or a `target:` id.
func (s *Shelf) Shelved(id string) (Entry, bool) {
	if e, ok := s.Lookup(id); ok {
		return e, true
	}
	if !strings.Contains(id, ":") {
		return s.Lookup(KindTarget + ":" + id)
	}
	return Entry{}, false
}

// Cases are the cases the shelf names for one unit of a kind, sorted: the Go tests of a package, the gtest tests of a binary.
func (s *Shelf) Cases(kind, unit string) []string {
	var out []string
	for _, e := range s.Entries {
		k, u, c, err := Split(e.ID)
		if err == nil && k == kind && u == unit && c != "" {
			out = append(out, c)
		}
	}
	sort.Strings(out)
	return out
}

// GoSkip is the pattern for `go test -skip` that keeps a package's shelved tests out of the run ("" when none is shelved).
func (s *Shelf) GoSkip(pkg string) string {
	cases := s.Cases(KindGo, pkg)
	if len(cases) == 0 {
		return ""
	}
	quoted := make([]string, len(cases))
	for i, c := range cases {
		quoted[i] = regexp.QuoteMeta(c)
	}
	return "^(" + strings.Join(quoted, "|") + ")$"
}

// GoSkipAll is the `go test -skip` pattern for a run over several packages: the shelved Go tests of every package.
func (s *Shelf) GoSkipAll() string {
	var cases []string
	for _, e := range s.Entries {
		if k, _, c, err := Split(e.ID); err == nil && k == KindGo && c != "" {
			cases = append(cases, regexp.QuoteMeta(c))
		}
	}
	if len(cases) == 0 {
		return ""
	}
	sort.Strings(cases)
	return "^(" + strings.Join(cases, "|") + ")$"
}

// GTestExclusion is the negative part of a gtest filter that keeps a binary's shelved tests out of the run ("" when none).
func (s *Shelf) GTestExclusion(binary string) string {
	return strings.Join(s.Cases(KindGTest, binary), ":")
}

// MergeGTestFilter is the gtest filter that runs base (everything when empty) without the exclusion (a ":"-joined list of
// tests): a base that has a negative part already gets the exclusion added to it.
func MergeGTestFilter(base, exclusion string) string {
	switch {
	case exclusion == "":
		return base
	case base == "":
		return "-" + exclusion
	case strings.Contains(base, "-"):
		return base + ":" + exclusion
	}
	return base + "-" + exclusion
}

// Ids are the ids of every entry.
func (s *Shelf) Ids() []string {
	ids := make([]string, len(s.Entries))
	for i, e := range s.Entries {
		ids[i] = e.ID
	}
	return ids
}

// Missing returns, for every entry whose test does not exist in the checkout at repo, a line saying what was not found.
// A shelf that names a test that is gone shelves nothing and hides that it is gone.
func (s *Shelf) Missing(repo string) []string {
	var out []string
	for _, e := range s.Entries {
		kind, unit, testCase, err := Split(e.ID)
		if err != nil {
			out = append(out, err.Error())
			continue
		}
		if why := missing(repo, kind, unit, testCase); why != "" {
			out = append(out, fmt.Sprintf("%s: %s", e.ID, why))
		}
	}
	return out
}

func missing(repo, kind, unit, testCase string) string {
	switch kind {
	case KindJust:
		if !regexp.MustCompile(`(?m)^` + regexp.QuoteMeta(unit) + `(\s[^\n:]*)?:`).Match(readFile(repo, "justfile")) {
			return "no recipe of that name in justfile"
		}
	case KindTarget:
		var m struct {
			Targets map[string]json.RawMessage `json:"targets"`
		}
		if err := json.Unmarshal(readFile(repo, mapHeaderFile), &m); err != nil {
			return mapDir + "/map.json cannot be read: " + err.Error()
		}
		if _, ok := m.Targets[unit]; !ok {
			return "no such target in " + mapDir + "/map.json"
		}
	case KindPytest:
		src := readFile(repo, "tools/tests/"+unit+".py")
		if src == nil {
			return "no file tools/tests/" + unit + ".py"
		}
		if testCase != "" && !pytestCaseExists(string(src), testCase) {
			return "the file has no " + testCase
		}
	case KindGo:
		if !dirHas(filepath.Join(repo, "tools", filepath.FromSlash(unit)), "_test.go", `func `+regexp.QuoteMeta(testCase)+`\(`, testCase == "") {
			return "no such package under tools/, or no func " + testCase + " in its tests"
		}
	case KindGTest:
		if !bytes.Contains(mapText(repo), []byte(`"gtest:`+unit+`"`)) {
			return mapDir + "/ names no such gtest binary"
		}
		if testCase != "" {
			suite, name, _ := strings.Cut(testCase, ".")
			re := `TEST[_A-Z]*\(\s*` + regexp.QuoteMeta(suite) + `\s*,\s*` + regexp.QuoteMeta(name) + `\b`
			if !treeHas(repo, "src", []string{".cpp", ".cc", ".h"}, re) {
				return "no TEST(" + suite + ", " + name + ") under src/"
			}
		}
	case KindCTest:
		re := `nevr_ht_(test|rerun)\(\s*` + regexp.QuoteMeta(unit) + `\b`
		if !treeHas(repo, "src/quest/host_tests", []string{".cmake", "CMakeLists.txt"}, re) {
			return "no nevr_ht_test or nevr_ht_rerun of that name under src/quest/host_tests"
		}
	}
	return ""
}

func pytestCaseExists(src, testCase string) bool {
	cls, name, hasName := strings.Cut(testCase, ".")
	if !hasName {
		return regexp.MustCompile(`(?m)^(class\s+` + regexp.QuoteMeta(cls) + `\b|def\s+` + regexp.QuoteMeta(cls) + `\b)`).MatchString(src)
	}
	return regexp.MustCompile(`(?m)^class\s+`+regexp.QuoteMeta(cls)+`\b`).MatchString(src) &&
		regexp.MustCompile(`(?m)^\s+def\s+`+regexp.QuoteMeta(name)+`\b`).MatchString(src)
}

// mapDir is the test map's directory (testmap.DefaultPath): map.json holds the targets, and components/ one file per
// component. testshelf cannot import testmap (testmap imports it), so the two names are kept here.
const (
	mapDir        = "tools/test_map"
	mapHeaderFile = mapDir + "/map.json"
	mapComponents = mapDir + "/components"
)

// mapText is the text of every file of the map (the header and each component file), for a check that looks for an id.
func mapText(repo string) []byte {
	var b bytes.Buffer
	b.Write(readFile(repo, mapHeaderFile))
	files, _ := filepath.Glob(filepath.Join(repo, filepath.FromSlash(mapComponents), "*.jsonl"))
	for _, f := range files {
		if data, err := os.ReadFile(f); err == nil {
			b.Write(data)
		}
	}
	return b.Bytes()
}

func readFile(repo, rel string) []byte {
	data, err := os.ReadFile(filepath.Join(repo, filepath.FromSlash(rel)))
	if err != nil {
		return nil
	}
	return data
}

// dirHas reports whether dir holds a file with the suffix whose text matches re (any file at all when anyFile).
func dirHas(dir, suffix, re string, anyFile bool) bool {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return false
	}
	rx := regexp.MustCompile(re)
	for _, e := range entries {
		if e.IsDir() || !strings.HasSuffix(e.Name(), suffix) {
			continue
		}
		if anyFile {
			return true
		}
		if data, err := os.ReadFile(filepath.Join(dir, e.Name())); err == nil && rx.Match(data) {
			return true
		}
	}
	return false
}

// treeHas reports whether a file under repo/root whose name ends in one of the suffixes holds text matching re.
func treeHas(repo, root string, suffixes []string, re string) bool {
	rx := regexp.MustCompile(re)
	found := false
	_ = filepath.WalkDir(filepath.Join(repo, filepath.FromSlash(root)), func(p string, d os.DirEntry, err error) error {
		if err != nil || found || d.IsDir() {
			return nil
		}
		for _, s := range suffixes {
			if strings.HasSuffix(d.Name(), s) {
				if data, rerr := os.ReadFile(p); rerr == nil && rx.Match(data) {
					found = true
				}
				break
			}
		}
		return nil
	})
	return found
}
