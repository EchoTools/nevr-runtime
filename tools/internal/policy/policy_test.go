package policy

import (
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/testkit"
)

// repoRoot is the checkout this package is in: tools/internal/policy is three levels below it.
func repoRoot(t *testing.T) string {
	t.Helper()
	wd, err := os.Getwd()
	if err != nil {
		t.Fatal(err)
	}
	root := filepath.Join(wd, "..", "..", "..")
	if _, err := os.Stat(filepath.Join(root, "tools", "go.mod")); err != nil {
		t.Fatalf("%s is not the repository root: %v", root, err)
	}
	return root
}

func git(t *testing.T, dir string, args ...string) (string, error) {
	t.Helper()
	cmd := exec.CommandContext(t.Context(), "git", args...)
	cmd.Dir = dir
	cmd.Env = testkit.Env(t)
	out, err := cmd.Output()
	return string(out), err
}

// ---- the files in the tree ----

func TestEveryTrackedPythonFileIsOnThePortListOrInTheStaysTable(t *testing.T) {
	t.Parallel()
	tracked, err := TrackedPython(repoRoot(t), testkit.Env(t))
	if err != nil {
		t.Fatal(err)
	}
	if len(tracked) < 100 {
		t.Fatalf("only %d tracked Python files under tools/: the listing is broken, not the tree", len(tracked))
	}
	if uncovered := Uncovered(tracked); len(uncovered) > 0 {
		t.Errorf("Python files under tools/ that are neither on the port list nor in ADR 0044's \"Where Python stays\" table "+
			"(a new tool or test is Go; Python needs a recorded reason, ADR 0044 rule 2):\n  %s", strings.Join(uncovered, "\n  "))
	}
}

func TestEveryFileOnThePortListIsStillTracked(t *testing.T) {
	t.Parallel()
	tracked, err := TrackedPython(repoRoot(t), testkit.Env(t))
	if err != nil {
		t.Fatal(err)
	}
	if stale := Stale(tracked); len(stale) > 0 {
		t.Errorf("files on tools/internal/policy/python_to_port.txt that are no longer tracked (the port that removed them removes the line):\n  %s",
			strings.Join(stale, "\n  "))
	}
}

func TestThePortListIsSortedWithoutDuplicatesAndNamesNoStayingFile(t *testing.T) {
	t.Parallel()
	list := ToPort()
	if len(list) < 100 {
		t.Fatalf("the port list has %d entries; it is read from the wrong place", len(list))
	}
	if !sort.StringsAreSorted(list) {
		t.Error("the port list is not sorted")
	}
	for i, p := range list {
		if i > 0 && list[i-1] == p {
			t.Errorf("%s is listed twice", p)
		}
		if !strings.HasPrefix(p, "tools/") || !strings.HasSuffix(p, ".py") {
			t.Errorf("%q is not a tools/**.py path", p)
		}
		if s, ok := StayFor(p); ok {
			t.Errorf("%s is on the port list and also stays (%s)", p, s.Path)
		}
	}
}

func TestEveryStayHasAReasonAndAnEnd(t *testing.T) {
	t.Parallel()
	if len(Stays) == 0 {
		t.Fatal("no Stays")
	}
	for _, s := range Stays {
		if s.Path == "" || len(s.Reason) < 20 || len(s.EndsWhen) < 5 {
			t.Errorf("%+v lacks a path, a reason or an end", s)
		}
	}
}

// ---- the list only shrinks ----

func TestGrownNamesWhatTheNewListAdds(t *testing.T) {
	t.Parallel()
	old := []string{"tools/a.py", "tools/b.py", "tools/c.py"}
	for _, tc := range []struct {
		name string
		now  []string
		want string
	}{
		{"same", old, ""},
		{"smaller", []string{"tools/a.py", "tools/c.py"}, ""},
		{"empty", nil, ""},
		{"one added", []string{"tools/a.py", "tools/b.py", "tools/c.py", "tools/d.py"}, "tools/d.py"},
		{"one swapped", []string{"tools/a.py", "tools/b.py", "tools/z.py"}, "tools/z.py"},
	} {
		if got := strings.Join(Grown(old, tc.now), ","); got != tc.want {
			t.Errorf("%s: Grown = %q, want %q", tc.name, got, tc.want)
		}
	}
}

func TestThePortListIsNotLargerThanAtTheMergeBaseWithOriginMain(t *testing.T) {
	t.Parallel()
	root := repoRoot(t)
	base, err := git(t, root, "merge-base", "HEAD", "origin/main")
	if err != nil {
		t.Skipf("no merge-base with origin/main here (%v): the list cannot be compared; it is compared wherever origin/main exists", err)
	}
	old, err := git(t, root, "show", strings.TrimSpace(base)+":tools/internal/policy/python_to_port.txt")
	if err != nil {
		t.Skipf("the port list does not exist at the merge-base %s: it is new in this change", strings.TrimSpace(base))
	}
	if added := Grown(parseList(old), ToPort()); len(added) > 0 {
		t.Errorf("the port list grew since the merge-base; it only shrinks (a file that is not on it is Go, or has a recorded reason):\n  %s",
			strings.Join(added, "\n  "))
	}
}

// ---- the ADR's table and the Stays table say the same ----

func TestTheADRsTableAndTheStaysTableNameTheSameFiles(t *testing.T) {
	t.Parallel()
	adr, err := os.ReadFile(filepath.Join(repoRoot(t), "docs", "adr", ADRFile))
	if err != nil {
		t.Fatal(err)
	}
	fromADR, err := ADRStays(string(adr))
	if err != nil {
		t.Fatal(err)
	}
	var fromGo []string
	for _, s := range Stays {
		fromGo = append(fromGo, s.Path)
	}
	sort.Strings(fromGo)
	if strings.Join(fromADR, "\n") != strings.Join(fromGo, "\n") {
		t.Errorf("the ADR's \"Where Python stays\" table names\n  %s\nbut policy.Stays holds\n  %s",
			strings.Join(fromADR, "\n  "), strings.Join(fromGo, "\n  "))
	}
}

func TestADRStaysReadsTheFirstColumnOfTheTableUnderItsHeading(t *testing.T) {
	t.Parallel()
	doc := "## Before\n\n| File(s) | Reason |\n| --- | --- |\n| `tools/not/this.py` | x |\n\n" +
		"## Where Python stays\n\nSome words with `tools/ignored.py`.\n\n" +
		"| File(s) | Reason | Ends when |\n| --- | --- | --- |\n" +
		"| `tools/echomod/**` (33 files) and the `just-recipe` recipe | r | e |\n" +
		"| `tools/x.py` | r | e |\n\n## After\n\n| `tools/after.py` | r |\n"
	got, err := ADRStays(doc)
	if err != nil || strings.Join(got, ",") != "tools/echomod/,tools/x.py" {
		t.Errorf("ADRStays = %q, %v; want tools/echomod/ and tools/x.py (a /** pattern is its directory; other backticks are not paths)", got, err)
	}
	if _, err := ADRStays("# no such heading\n"); err == nil {
		t.Error("a document with no \"Where Python stays\" table was accepted")
	}
}

func TestStayForMatchesAFileByNameAndADirectoryByPrefix(t *testing.T) {
	t.Parallel()
	for path, want := range map[string]bool{
		"tools/echomod/clone_combat.py":        true,
		"tools/echomod/sub/deep.py":            true,
		"tools/echomod_not_it.py":              false,
		"tools/run_tool_tests.py":              true,
		"tools/run_tool_tests.py.bak":          false,
		"tools/tests/test_run_tool_tests.py":   false,
		"tools/tests/echomod/test_whatever.py": false,
	} {
		if _, got := StayFor(path); got != want {
			t.Errorf("StayFor(%q) = %v, want %v", path, got, want)
		}
	}
}

func TestUncoveredAndStaleSplitTheTreeAgainstTheList(t *testing.T) {
	t.Parallel()
	list := ToPort()
	tracked := []string{list[0], "tools/echomod/x.py", "tools/new_thing.py"}
	if got := strings.Join(Uncovered(tracked), ","); got != "tools/new_thing.py" {
		t.Errorf("Uncovered = %q, want tools/new_thing.py", got)
	}
	if got := Stale(tracked); len(got) != len(list)-1 {
		t.Errorf("Stale has %d entries, want the %d list entries the tree lacks", len(got), len(list)-1)
	}
}

// ---- no sleeping to wait (ADR 0044 rule 9) ----

func TestSleepScanFlagsATestSleepOutsideASynctestBubble(t *testing.T) {
	t.Parallel()
	src := `package x
import ("testing"; "time"; "testing/synctest")
func TestBad(t *testing.T) { time.Sleep(time.Second) }
func TestAlsoBad(t *testing.T) { go func() { time.Sleep(5 * time.Millisecond) }() }
func TestGood(t *testing.T) {
	synctest.Test(t, func(t *testing.T) {
		go func() { time.Sleep(time.Hour) }()
		time.Sleep(time.Minute)
	})
}
func TestGoodRun(t *testing.T) { synctest.Run(func() { time.Sleep(time.Hour) }) }
`
	got, err := SleepViolations("x_test.go", []byte(src))
	if err != nil {
		t.Fatal(err)
	}
	if len(got) != 2 || !strings.Contains(got[0], "x_test.go:3") || !strings.Contains(got[1], "x_test.go:4") {
		t.Errorf("violations = %q, want the sleeps on lines 3 and 4 only", got)
	}
}

func TestSleepScanFlagsAToolSleepWhoseDurationIsAConstant(t *testing.T) {
	t.Parallel()
	src := `package x
import "time"
const pause = 2 * time.Second
func bad1()  { time.Sleep(500 * time.Millisecond) }
func bad2()  { time.Sleep(pause) }
func bad3()  { time.Sleep(time.Second) }
func good1(d time.Duration) { time.Sleep(d) }
type S struct{ Interval time.Duration }
func (s S) good2() { time.Sleep(s.Interval) }
func good3(n int) { time.Sleep(time.Duration(n) * time.Millisecond) }
func bad4()  { time.Sleep((time.Second)) }
func bad5()  { time.Sleep(-time.Second) }
`
	got, err := SleepViolations("tool.go", []byte(src))
	if err != nil {
		t.Fatal(err)
	}
	if len(got) != 5 {
		t.Fatalf("violations = %q, want the five constant sleeps (lines 4, 5, 6, 11, 12)", got)
	}
	for i, line := range []string{":4", ":5", ":6", ":11", ":12"} {
		if !strings.Contains(got[i], "tool.go"+line) {
			t.Errorf("violation %d = %q, want line %s", i, got[i], line)
		}
	}
}

func TestSleepScanRejectsASourceItCannotParse(t *testing.T) {
	t.Parallel()
	if _, err := SleepViolations("broken.go", []byte("package x\nfunc (")); err == nil {
		t.Error("no error for a file that does not parse")
	}
}

func TestNoGoFileUnderToolsSleepsToWait(t *testing.T) {
	t.Parallel()
	root := repoRoot(t)
	out, err := git(t, root, "ls-files", "--cached", "--others", "--exclude-standard", "--", "tools/*.go")
	if err != nil {
		t.Fatal(err)
	}
	files := strings.Fields(out)
	if len(files) < 10 {
		t.Fatalf("only %d Go files listed under tools/: the listing is broken", len(files))
	}
	var violations []string
	for _, f := range files {
		src, err := os.ReadFile(filepath.Join(root, f))
		if err != nil {
			t.Fatal(err)
		}
		v, err := SleepViolations(f, src)
		if err != nil {
			t.Fatal(err)
		}
		violations = append(violations, v...)
	}
	if len(violations) > 0 {
		t.Errorf("time.Sleep to wait (ADR 0044 rule 9: a test blocks on the event with a deadline or runs on the synctest clock; "+
			"a tool's delay is a parameter):\n  %s", strings.Join(violations, "\n  "))
	}
}
