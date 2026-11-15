package questlifts

import (
	"crypto/sha256"
	"encoding/hex"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

const (
	// pagePath is the page, from this package's directory.
	pagePath = "../../../docs/quest/combat/script_lifts.md"
	// libsEnv names the directory of the reference APK's lib<hash>.so files.
	libsEnv = "NEVR_QUEST_REFERENCE_LIBS"
	// fileMode is how the test writes its fake libraries.
	fileMode = 0o644
)

func readPage(t *testing.T) string {
	t.Helper()
	b, err := os.ReadFile(pagePath)
	if err != nil {
		t.Fatal(err)
	}
	return string(b)
}

// rowLines returns the page's table rows as lines, in page order.
func rowLines(t *testing.T, page string) []string {
	t.Helper()
	rows, err := Parse(page)
	if err != nil {
		t.Fatal(err)
	}
	lines := strings.Split(page, "\n")
	out := make([]string, 0, len(rows))
	for _, r := range rows {
		out = append(out, lines[r.Line-1])
	}
	return out
}

func TestThePageListsEachScriptOnceWithBothDigests(t *testing.T) {
	rows, err := Parse(readPage(t))
	if err != nil {
		t.Fatal(err)
	}
	if len(rows) != ScriptCount {
		t.Fatalf("%d rows, want %d", len(rows), ScriptCount)
	}
	lines := strings.Split(readPage(t), "\n")
	for _, r := range rows {
		cells := strings.Split(strings.Trim(lines[r.Line-1], "|"), "|")
		image := strings.Trim(strings.TrimSpace(cells[columnsBeforeDigests]), "`")
		lift := strings.Trim(strings.TrimSpace(cells[columnsBeforeDigests+1]), "`")
		if r.ImageSHA256 != image || r.LiftSHA256 != lift {
			t.Errorf("row %s: image %s lift %s, want the page's %s and %s (in that order)", r.Script, r.ImageSHA256, r.LiftSHA256, image, lift)
		}
	}
}

// A row-shaped line under a later heading is not part of the table.
func TestTheTableEndsAtTheNextSection(t *testing.T) {
	page := readPage(t)
	first := rowLines(t, page)[0]
	firstScript := first[strings.Index(first, "`")+1 : strings.Index(first, "`")+1+scriptHashHex]
	later := page + "\n## Later\n\n" + strings.Replace(first, firstScript, strings.Repeat("0", scriptHashHex), 1) + "\n"
	rows, err := Parse(later)
	if err != nil || len(rows) != ScriptCount {
		t.Fatalf("Parse with a row under a later heading: %d rows, %v; want %d and no error", len(rows), err, ScriptCount)
	}
}

// Each drift the check exists for, applied to the real page, must be refused with a message naming it.
func TestADriftedTableIsRefused(t *testing.T) {
	page := readPage(t)
	lines := rowLines(t, page)
	first, second := lines[0], lines[1]
	firstScript := first[strings.Index(first, "`")+1 : strings.Index(first, "`")+1+scriptHashHex]
	firstLift := strings.Split(first, "|")[columnsBeforeDigests+digestColumns]
	short := strings.Replace(first, strings.TrimSpace(firstLift), "`"+strings.Repeat("a", digestHex-1)+"`", 1)
	cases := []struct {
		name, page, want string
	}{
		{"a row removed", strings.Replace(page, first+"\n", "", 1), "want 18"},
		{"a row added", strings.Replace(page, first+"\n", first+"\n"+strings.Replace(first, firstScript,
			strings.Repeat("0", scriptHashHex), 1)+"\n", 1), "want 18"},
		{"a script named twice", strings.Replace(page, second, first, 1), "named again"},
		{"a script hash of the wrong length", strings.Replace(page, "`"+firstScript+"`", "`"+firstScript[1:]+"`", 1),
			"lowercase hex"},
		{"an upper-case script hash", strings.Replace(page, "`"+firstScript+"`", "`"+strings.ToUpper(firstScript)+"`", 1),
			"lowercase hex"},
		{"a short lift digest", strings.Replace(page, first, short, 1), "sha256"},
		{"no section", strings.Replace(page, tableHeading, "## Other", 1), "no \"## The lifts\" section"},
		{"a row with too few cells", strings.Replace(page, first, "| `"+firstScript+"` | name |", 1), "fewer than"},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			if c.page == page {
				t.Fatal("the drift did not change the page")
			}
			_, err := Parse(c.page)
			if err == nil || !strings.Contains(err.Error(), c.want) {
				t.Fatalf("Parse: %v, want an error containing %q", err, c.want)
			}
		})
	}
}

func TestCheckLibsNamesAMissingOrDifferentFile(t *testing.T) {
	dir := t.TempDir()
	good, bad, absent := []byte("good"), []byte("other"), "0000000000000000"
	sum := sha256.Sum256(good)
	digest := hex.EncodeToString(sum[:])
	rows := []Row{{Script: "1111111111111111", LiftSHA256: digest}, {Script: "2222222222222222", LiftSHA256: digest},
		{Script: absent, LiftSHA256: digest}}
	for name, b := range map[string][]byte{rows[0].Script: good, rows[1].Script: bad} {
		if err := os.WriteFile(filepath.Join(dir, "lib"+name+".so"), b, fileMode); err != nil {
			t.Fatal(err)
		}
	}
	err := CheckLibs(dir, rows)
	if err == nil {
		t.Fatal("CheckLibs accepted a wrong and a missing file")
	}
	msg := err.Error()
	if strings.Contains(msg, rows[0].Script) || !strings.Contains(msg, rows[1].Script) || !strings.Contains(msg, absent) {
		t.Fatalf("CheckLibs: %v, want the second and third scripts named and not the first", err)
	}
	if err := CheckLibs(dir, rows[:1]); err != nil {
		t.Fatalf("CheckLibs on a matching file: %v", err)
	}
}

func TestTheReferenceLibsHashToThePage(t *testing.T) {
	dir := os.Getenv(libsEnv)
	if dir == "" {
		t.Skip(libsEnv + " is not set (the directory holding the reference APK's lib<hash>.so files, its lib/arm64-v8a)")
	}
	rows, err := Parse(readPage(t))
	if err != nil {
		t.Fatal(err)
	}
	if err := CheckLibs(dir, rows); err != nil {
		t.Fatal(err)
	}
}
