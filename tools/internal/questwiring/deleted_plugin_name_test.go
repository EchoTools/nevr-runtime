package questwiring

import (
	"bufio"
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"testing"
)

// #2189: no tracked file, and no tracked path, names the deleted combat plugin (the word deletedWord spells), outside the names
// listed here. The plugin is folded into the runtime (docs/adr/0046-combat-is-part-of-the-quest-runtime.md) and its tree is
// deleted, so the word names nothing in this tree: a name of it that comes back, in code, a test, a script, a recipe or a
// document, is a copy of the deleted plugin or prose about a file that does not exist (AGENTS.md, "Documentation, plans and
// findings": docs state the current tree). The scan reads every tracked file and every tracked path, case-insensitively, except
// docs/audits/ (the historical record, dated by design).
//
// deletedPluginAllowed is the names that stay: each is a string something outside this tree resolves by name (a device's
// directory, a file at a released tag, the audit's file name), and its reason says what. deleted_plugin_residue.txt is the lines
// not yet reworded, as "<path><TAB><lines>" (KNOWN-WRONG #2189): it only shrinks. A file with more lines than its entry fails,
// and so does a file with fewer, until its entry is lowered or removed. The word is spelled in two parts so that this file is
// not a hit of its own scan; the residue file holds paths and counts only.

const (
	deletedWord         = "q1" + "5t"
	deletedResidueFile  = "tools/internal/questwiring/deleted_plugin_residue.txt"
	deletedExemptPrefix = "docs/audits/"
)

var deletedWordRE = regexp.MustCompile(`(?i)` + deletedWord)

type allowance struct{ literal, reason string }

// Removed from every line before it is scanned.
var deletedPluginAllowedEverywhere = []allowance{
	{"docs/audits/2026-11-05-" + deletedWord + "-origin-citations.md",
		"the origin-citation audit's file name; docs/audits/ is the historical record and is not renamed (AGENTS.md)"},
}

// Removed from a line of one file before it is scanned.
var deletedPluginAllowed = map[string][]allowance{
	"src/quest/combat/legacy_switch.h": {
		{`kDataSubdir = "` + deletedWord + `"`, "the data directory below the app's external files folder: players' devices hold their gun-hand markers there (docs/adr/0046 rule 4), so its name is kept"},
	},
	"src/quest/tests/sendguard_test.cpp": {
		{`"/` + deletedWord + `"`, "pins the data directory's name the sendguard install looks in (legacy_switch.h kDataSubdir)"},
	},
	"tools/internal/questwiring/gun_bots_test.go": {
		{`kDataSubdir = "` + deletedWord + `"`, "pins the one definition of the data directory's name (legacy_switch.h)"},
	},
	// One entry per file. A change that keeps a name adds its entry above this line.
}

// deletedPluginScan returns the tracked paths that carry the word and, for each file, the number of its lines that carry it once
// the allowed literals are removed.
func deletedPluginScan(t *testing.T) (paths []string, counts map[string]int) {
	t.Helper()
	counts = map[string]int{}
	for _, rel := range trackedFiles(t) {
		if strings.HasPrefix(rel, deletedExemptPrefix) {
			continue
		}
		if deletedWordRE.MatchString(rel) {
			paths = append(paths, rel)
		}
		full := filepath.Join(root(t), rel)
		info, err := os.Lstat(full)
		if os.IsNotExist(err) || (err == nil && !info.Mode().IsRegular()) {
			continue // a link, a submodule or a file not on disk: no text of ours to read
		}
		if err != nil {
			t.Fatal(err)
		}
		b, err := os.ReadFile(full)
		if err != nil {
			t.Fatal(err)
		}
		literals := append(append([]allowance{}, deletedPluginAllowedEverywhere...), deletedPluginAllowed[rel]...)
		n := 0
		for _, line := range bytes.Split(b, []byte("\n")) {
			for _, a := range literals {
				line = bytes.ReplaceAll(line, []byte(a.literal), nil)
			}
			if deletedWordRE.Match(line) {
				n++
			}
		}
		if n > 0 {
			counts[rel] = n
		}
	}
	return paths, counts
}

// deletedPluginResidue reads deleted_plugin_residue.txt: "<path><TAB><lines>" per line, # comments and blank lines skipped.
func deletedPluginResidue(t *testing.T) map[string]int {
	t.Helper()
	out := map[string]int{}
	sc := bufio.NewScanner(strings.NewReader(read(t, deletedResidueFile)))
	for sc.Scan() {
		line := sc.Text()
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		path, num, ok := strings.Cut(line, "\t")
		n, err := strconv.Atoi(num)
		if !ok || err != nil || n <= 0 {
			t.Fatalf("%s: %q is not <path><TAB><lines>", deletedResidueFile, line)
		}
		if _, dup := out[path]; dup {
			t.Fatalf("%s: %s is listed twice", deletedResidueFile, path)
		}
		out[path] = n
	}
	return out
}

// residueText is the residue file's body for counts, so a failure prints what the file would hold.
func residueText(counts map[string]int) string {
	keys := make([]string, 0, len(counts))
	for k := range counts {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	var b strings.Builder
	for _, k := range keys {
		fmt.Fprintf(&b, "%s\t%d\n", k, counts[k])
	}
	return b.String()
}

func TestNoTrackedPathNamesTheDeletedPlugin(t *testing.T) {
	paths, _ := deletedPluginScan(t)
	for _, p := range paths {
		t.Errorf("%s: a tracked path outside %s names the deleted plugin", p, deletedExemptPrefix)
	}
}

func TestNoFileNamesTheDeletedPluginOnMoreLinesThanItsResidue(t *testing.T) {
	_, counts := deletedPluginScan(t)
	residue := deletedPluginResidue(t)
	for p, n := range counts {
		if n > residue[p] {
			t.Errorf("%s names the deleted plugin on %d lines; %s allows %d", p, n, deletedResidueFile, residue[p])
		}
	}
	if t.Failed() {
		t.Logf("the scan finds, as %s would hold it:\n%s", deletedResidueFile, residueText(counts))
	}
}

func TestTheResidueOnlyShrinks(t *testing.T) {
	_, counts := deletedPluginScan(t)
	for p, n := range deletedPluginResidue(t) {
		if counts[p] < n {
			t.Errorf("%s: %s lists %d lines, the file has %d: lower or remove the entry", deletedResidueFile, p, n, counts[p])
		}
	}
}

func TestEveryAllowedNameIsStillThereAndHasAReason(t *testing.T) {
	for rel, list := range deletedPluginAllowed {
		text := read(t, rel)
		for _, a := range list {
			if !strings.Contains(text, a.literal) {
				t.Errorf("%s: %q is no longer there; remove its entry (%s)", rel, a.literal, a.reason)
			}
			if strings.TrimSpace(a.reason) == "" {
				t.Errorf("%s: %q has no reason", rel, a.literal)
			}
		}
	}
}
