package policy

import (
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/testkit"
)

// The fakes of the recipe tests: python3 and go, each scripted by the files "stdout" and "exit" beside it.
func scripted(c *testkit.Call) int {
	if out, err := os.ReadFile(filepath.Join(c.FakeDir, "stdout")); err == nil {
		c.Stdout.Write(out)
	}
	if raw, err := os.ReadFile(filepath.Join(c.FakeDir, "exit")); err == nil {
		n, _ := strconv.Atoi(strings.TrimSpace(string(raw)))
		return n
	}
	return 0
}

func TestMain(m *testing.M) {
	testkit.Main(m, testkit.Fakes{"python3": scripted, "go": scripted})
}

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

// ---- the list grows only with a reason ----

func TestAddedSplitsNewEntriesByWhetherTheyCarryAReason(t *testing.T) {
	t.Parallel()
	old := []string{"tools/a.py", "tools/b.py"}
	now := []Entry{
		{"tools/a.py", ""}, // already there: its reason is the Every-entry test's business
		{"tools/b.py", "ported by its unit"},
		{"tools/c.py", "extends an existing Python test pattern; port with the G-units"},
		{"tools/d.py", ""},
		{"tools/e.py", "   "},
		{"tools/f.py", "short"},
		{"tools/g.py", "0123456789"},   // exactly the shortest reason
		{"tools/h.py", "   short    "}, // long only by its padding
	}
	with, without := Added(old, now)
	paths := func(es []Entry) string {
		var out []string
		for _, e := range es {
			out = append(out, e.Path)
		}
		return strings.Join(out, ",")
	}
	if paths(with) != "tools/c.py,tools/g.py" || paths(without) != "tools/d.py,tools/e.py,tools/f.py,tools/h.py" {
		t.Errorf("with %q, without %q", paths(with), paths(without))
	}
	if with, without := Added(old, now[:2]); len(with)+len(without) != 0 {
		t.Errorf("an unchanged or shrunk list added %v %v", with, without)
	}
	if with, without := Added(nil, now[2:3]); len(with) != 1 || len(without) != 0 {
		t.Errorf("against an empty baseline: %v %v", with, without)
	}
}

func TestEveryPortListEntryHasAReason(t *testing.T) {
	t.Parallel()
	entries := PortEntries()
	if len(entries) < 100 {
		t.Fatalf("%d entries: the list is read from the wrong place", len(entries))
	}
	for _, e := range entries {
		if len(strings.TrimSpace(e.Reason)) < MinReasonLen {
			t.Errorf("%s has no reason on tools/internal/policy/python_to_port.txt (path, a tab, then why it is still Python and what ends it)", e.Path)
		}
	}
}

func TestParseEntriesReadsPathTabReasonAndSkipsComments(t *testing.T) {
	t.Parallel()
	got := parseEntries("# c\n\ntools/a.py\twhy a\ntools/b.py\n  tools/c.py \t  why c  \n")
	want := []Entry{{"tools/a.py", "why a"}, {"tools/b.py", ""}, {"tools/c.py", "why c"}}
	if len(got) != 3 || got[0] != want[0] || got[1] != want[1] || got[2] != want[2] {
		t.Errorf("got %q, want %q", got, want)
	}
}

func TestEntriesAddedToThePortListSinceTheMergeBaseCarryAReason(t *testing.T) {
	t.Parallel()
	old, skipped, err := BaselineList(repoRoot(t), testkit.Env(t))
	if err != nil {
		t.Fatal(err)
	}
	if skipped != "" {
		t.Skip(skipped)
	}
	with, without := Added(old, PortEntries())
	for _, e := range without {
		t.Errorf("%s was added to the port list without a reason (a Python file needs one, ADR 0044 rule 2: path, a tab, why)", e.Path)
	}
	for _, e := range with {
		t.Logf("added to the port list with a reason: %s: %s", e.Path, e.Reason)
	}
}

// scratchRepo makes a repository whose origin/main is a ref we choose, so BaselineList can be shown each case.
func scratchRepo(t *testing.T) (dir string, env []string, run func(args ...string) string) {
	t.Helper()
	dir = t.TempDir()
	env = testkit.Env(t)
	run = func(args ...string) string {
		t.Helper()
		cmd := exec.CommandContext(t.Context(), "git", args...)
		cmd.Dir, cmd.Env = dir, env
		out, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("git %v: %v\n%s", args, err, out)
		}
		return strings.TrimSpace(string(out))
	}
	run("init", "-q")
	return dir, env, run
}

func commitList(t *testing.T, dir string, run func(...string) string, lines ...string) {
	t.Helper()
	path := filepath.Join(dir, "tools", "internal", "policy", "python_to_port.txt")
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte("# comment\n"+strings.Join(lines, "\n")+"\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	run("add", "-A")
	run("commit", "-q", "-m", "x")
}

func TestBaselineListReadsTheListAtTheMergeBase(t *testing.T) {
	t.Parallel()
	dir, env, run := scratchRepo(t)
	commitList(t, dir, run, "tools/a.py", "tools/b.py")
	run("update-ref", "refs/remotes/origin/main", "HEAD")
	commitList(t, dir, run, "tools/a.py")
	old, skipped, err := BaselineList(dir, env)
	if err != nil || skipped != "" || strings.Join(old, ",") != "tools/a.py,tools/b.py" {
		t.Errorf("old %q skipped %q err %v", old, skipped, err)
	}
}

func TestBaselineListSkipsWithAReasonWhenThereIsNoOriginMain(t *testing.T) {
	t.Parallel()
	dir, env, run := scratchRepo(t)
	commitList(t, dir, run, "tools/a.py")
	_, skipped, err := BaselineList(dir, env)
	if err != nil || !strings.Contains(skipped, "origin/main") {
		t.Errorf("skipped %q err %v, want a skip that names origin/main", skipped, err)
	}
}

// A shallow clone has origin/main and no merge-base: the check cannot be made, and a check that cannot be made must not
// look like one that passed.
func TestBaselineListFailsWhenOriginMainExistsButHasNoMergeBase(t *testing.T) {
	t.Parallel()
	dir, env, run := scratchRepo(t)
	commitList(t, dir, run, "tools/a.py")
	run("update-ref", "refs/remotes/origin/main", "HEAD")
	run("checkout", "-q", "--orphan", "other")
	commitList(t, dir, run, "tools/a.py", "tools/c.py") // a different tree: the same one would be the same root commit
	_, skipped, err := BaselineList(dir, env)
	if err == nil || skipped != "" {
		t.Fatalf("skipped %q err %v, want an error", skipped, err)
	}
	for _, want := range []string{"merge-base", "fetch"} {
		if !strings.Contains(err.Error(), want) {
			t.Errorf("error %q lacks %q", err, want)
		}
	}
}

func TestBaselineListSkipsWhenTheListIsNewInThisChange(t *testing.T) {
	t.Parallel()
	dir, env, run := scratchRepo(t)
	if err := os.WriteFile(filepath.Join(dir, "x"), nil, 0o644); err != nil {
		t.Fatal(err)
	}
	run("add", "-A")
	run("commit", "-q", "-m", "base")
	run("update-ref", "refs/remotes/origin/main", "HEAD")
	commitList(t, dir, run, "tools/a.py")
	old, skipped, err := BaselineList(dir, env)
	if err != nil || old != nil || !strings.Contains(skipped, "new") {
		t.Errorf("old %q skipped %q err %v", old, skipped, err)
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

// ---- serve_forever polls fast in the Python test files (the scan unit A3 added in Python) ----

func TestServeForeverScanReadsEachWayACallCanBeWritten(t *testing.T) {
	t.Parallel()
	ok := []string{
		"import threading\nthreading.Thread(target=s.serve_forever, kwargs={\"poll_interval\": 0.01}).start()\n",
		"threading.Thread(target=self.httpd.serve_forever, kwargs={'poll_interval': 0.05}, daemon=True).start()\n",
		"s.serve_forever(poll_interval=0.05)\n",
		"s.serve_forever(poll_interval = 0.01)\n",
		"# s.serve_forever() in a comment\nx = 1\n",
	}
	for _, src := range ok {
		if got := ServeForeverProblems("a.py", src); len(got) != 0 {
			t.Errorf("%q: %q, want none", src, got)
		}
	}
	bad := map[string]string{
		"import threading\nthreading.Thread(target=s.serve_forever).start()\n":                     "no poll_interval",
		"threading.Thread(target=s.serve_forever, kwargs={\"poll_interval\": 0.5}).start()\n":      "over 0.05",
		"threading.Thread(target=s.serve_forever, kwargs={\"poll_interval\": interval}).start()\n": "not a literal number",
		"s.serve_forever()\n":                                             "no poll_interval",
		"s.serve_forever(0.5)\n":                                          "no poll_interval",
		"s.serve_forever(poll_interval=0.5)\n":                            "over 0.05",
		"s.serve_forever(my_poll_interval=0.01)\n":                        "no poll_interval",
		"s.serve_forever(poll_interval=delay)\n":                          "not a literal number",
		"run(s.serve_forever)\n":                                          "does not read",
		"threading.Thread(target=s.serve_forever, kwargs=opts).start()\n": "no poll_interval",
	}
	for src, want := range bad {
		got := ServeForeverProblems("a.py", src)
		if len(got) != 1 || !strings.Contains(got[0], want) || !strings.HasPrefix(got[0], "a.py:") {
			t.Errorf("%q: %q, want one problem containing %q", src, got, want)
		}
	}
}

func TestServeForeverProblemsNameTheLine(t *testing.T) {
	t.Parallel()
	got := ServeForeverProblems("a.py", "x = 1\n\n    s.serve_forever()\n")
	if len(got) != 1 || !strings.HasPrefix(got[0], "a.py:3:") {
		t.Errorf("problems = %q, want line 3", got)
	}
}

func TestEveryServeForeverInTheTestFilesSetsAShortPollInterval(t *testing.T) {
	t.Parallel()
	root := repoRoot(t)
	files, err := TrackedPython(root, testkit.Env(t))
	if err != nil {
		t.Fatal(err)
	}
	var problems []string
	scanned := 0
	for _, f := range files {
		if !strings.HasPrefix(f, "tools/tests/") || strings.Count(f, "/") != 2 || ServeForeverExempt(f) {
			continue
		}
		src, err := os.ReadFile(filepath.Join(root, f))
		if err != nil {
			t.Fatal(err)
		}
		if !strings.Contains(string(src), "serve_forever") {
			continue
		}
		scanned++
		problems = append(problems, ServeForeverProblems(f, string(src))...)
	}
	if scanned == 0 {
		t.Fatal("no test file mentions serve_forever: the scan reads nothing")
	}
	if len(problems) > 0 {
		t.Errorf("a serve_forever with the default poll_interval makes shutdown() wait up to 0.5 s; pass poll_interval=0.01 at each call:\n  %s",
			strings.Join(problems, "\n  "))
	}
}

// ---- the recipes run both halves (B1) ----

// testGoToolsBinary builds cmd/test-go-tools into the test's own directory (go's build cache makes it fast), so nothing
// is left behind when the test ends.
func testGoToolsBinary(t *testing.T) string {
	t.Helper()
	bin := filepath.Join(t.TempDir(), "test-go-tools")
	cmd := exec.CommandContext(t.Context(), "go", "-C", filepath.Join(repoRoot(t), "tools"), "build", "-o", bin, "./cmd/test-go-tools")
	cmd.Env = testkit.Env(t)
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("go build: %v\n%s", err, out)
	}
	return bin
}

// halves is a project directory laid out as the recipes expect (build/go-tools/test-go-tools, the real tool), with a
// fake python3 and a fake go on PATH, both failing.
type halves struct {
	dir, logs string
	py, goBin *testkit.FakeCommand
	env       []string
}

func newHalves(t *testing.T) *halves {
	t.Helper()
	bin := testGoToolsBinary(t)
	h := &halves{dir: t.TempDir(), logs: filepath.Join(t.TempDir(), "logs")}
	if err := os.MkdirAll(filepath.Join(h.dir, "build", "go-tools"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(h.dir, "tools"), 0o755); err != nil { // the module directory, which -C names
		t.Fatal(err)
	}
	if err := os.Symlink(bin, filepath.Join(h.dir, "build", "go-tools", "test-go-tools")); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(h.logs, 0o755); err != nil { // the Python runner's log, which its "test-tools: log" line names
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(h.logs, "20261105T120000.123456Z-1-0123456789abcdef-aabbccddeeff.log"), nil, 0o644); err != nil {
		t.Fatal(err)
	}
	h.py, h.goBin = testkit.Fake(t, "python3"), testkit.Fake(t, "go")
	script := func(f *testkit.FakeCommand, name, content string) {
		if err := os.WriteFile(filepath.Join(f.Dir(), name), []byte(content), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	script(h.py, "exit", "1")
	script(h.py, "stdout", "FAILED tools/tests/test_x.py::T::test_y\ntest-tools: log "+filepath.Join(h.logs, "20261105T120000.123456Z-1-0123456789abcdef-aabbccddeeff.log")+"\n")
	script(h.goBin, "exit", "1")
	script(h.goBin, "stdout", `{"Action":"fail","Package":"p","Test":"TestB"}`+"\n"+`{"Action":"fail","Package":"p"}`+"\n")
	h.env = testkit.WithPath(testkit.Env(t, "NEVR_TEST_LOG_DIR="+h.logs), h.py.Dir(), h.goBin.Dir())
	return h
}

// check asserts what the recipe line showed when both halves failed.
func (h *halves) check(t *testing.T, out string, err error) {
	t.Helper()
	if err == nil {
		t.Error("the recipe line exited 0 with both halves red")
	}
	if n := len(h.py.Calls(t)); n != 1 {
		t.Errorf("the Python half ran %d times, want 1", n)
	}
	calls := h.goBin.Calls(t)
	if len(calls) != 1 || len(calls[0].Args) == 0 || calls[0].Args[0] != "test" {
		t.Errorf("the Go half did not run after the Python half failed: go calls %+v", calls)
	} else if want := filepath.Join(h.dir, "tools"); calls[0].Dir != want {
		t.Errorf("go test ran in %s, want the module directory %s (the module is tools/go.mod, not the root's)", calls[0].Dir, want)
	}
	for _, want := range []string{"go FAIL p TestB", "FAIL: python half exit 1, go half exit 1"} {
		if !strings.Contains(out, want) {
			t.Errorf("output lacks %q:\n%s", want, out)
		}
	}
	if _, err := os.Stat(filepath.Join(h.logs, "20261105T120000.123456Z-1-0123456789abcdef-aabbccddeeff-go.jsonl")); err != nil {
		t.Errorf("the Go stream does not carry the Python run's stem: %v", err)
	}
	raw, _ := os.ReadFile(filepath.Join(h.logs, "runs.jsonl"))
	if !strings.Contains(string(raw), `"event":"go_end"`) {
		t.Errorf("runs.jsonl has no go_end record: %s", raw)
	}
}

func TestJustTestToolsRunsTheGoHalfWhenThePythonHalfFailsAndNamesBoth(t *testing.T) {
	t.Parallel()
	if _, err := exec.LookPath("just"); err != nil {
		t.Fatalf("just is a dependency of the tool suite: %v", err)
	}
	root := repoRoot(t)
	text, err := os.ReadFile(filepath.Join(root, "justfile"))
	if err != nil {
		t.Fatal(err)
	}
	attrs, header, body, ok := Recipe(string(text), "test-tools")
	if !ok {
		t.Fatal("no test-tools recipe")
	}
	if !strings.Contains(header, ": go-tools") {
		t.Errorf("test-tools does not depend on go-tools (ADR 0044 rule 7): %q", header)
	}
	h := newHalves(t)
	// The recipe as written, beside a go-tools that does nothing (the binary is already there), run by just itself.
	jf := strings.Join(attrs, "\n") + "\n" + header + "\n    " + strings.Join(body, "\n    ") + "\ngo-tools:\n    @true\n"
	if err := os.WriteFile(filepath.Join(h.dir, "justfile"), []byte(jf), 0o644); err != nil {
		t.Fatal(err)
	}
	cmd := exec.CommandContext(t.Context(), "just", "test-tools", "tools/tests/test_x.py")
	cmd.Dir, cmd.Env = h.dir, h.env
	out, err := cmd.CombinedOutput()
	h.check(t, string(out), err)
	if calls := h.py.Calls(t); len(calls) == 1 && (len(calls[0].Args) < 2 || calls[0].Args[len(calls[0].Args)-1] != "tools/tests/test_x.py") {
		t.Errorf("the recipe's arguments did not reach the Python runner: %+v", calls)
	}
}

func TestJustVerifyRunsTheGoHalfWhenThePythonHalfFailsAndNamesBoth(t *testing.T) {
	t.Parallel()
	root := repoRoot(t)
	text, err := os.ReadFile(filepath.Join(root, "justfile"))
	if err != nil {
		t.Fatal(err)
	}
	_, _, body, ok := Recipe(string(text), "verify")
	if !ok {
		t.Fatal("no verify recipe")
	}
	var line string
	built := false
	for _, l := range body {
		if l == "just go-tools" {
			built = true
		}
		if strings.HasPrefix(l, "build/go-tools/test-go-tools -C tools -- ") {
			if line != "" {
				t.Fatalf("two test-go-tools lines in verify: %q and %q", line, l)
			}
			line = l
		}
	}
	if line == "" {
		t.Fatal("verify has no build/go-tools/test-go-tools line")
	}
	if !built {
		t.Error("verify does not run `just go-tools` (ADR 0044 rule 7)")
	}
	if !strings.Contains(line, "python3 -m unittest") {
		t.Errorf("the line does not run the Python suite: %q", line)
	}
	h := newHalves(t)
	cmd := exec.CommandContext(t.Context(), "bash", "-c", "set -euo pipefail\n"+line)
	cmd.Dir, cmd.Env = h.dir, h.env
	out, err := cmd.CombinedOutput()
	h.check(t, string(out), err)
}

func TestGoToolsRecipeBuildsEveryCommandIntoBuildGoTools(t *testing.T) {
	t.Parallel()
	text, err := os.ReadFile(filepath.Join(repoRoot(t), "justfile"))
	if err != nil {
		t.Fatal(err)
	}
	_, header, body, ok := Recipe(string(text), "go-tools")
	if !ok || header != "go-tools:" {
		t.Fatalf("go-tools recipe: %q, %v", header, ok)
	}
	if got := strings.TrimSpace(strings.Join(body, "\n")); got != "go -C tools build -trimpath -o ../build/go-tools/ ./cmd/..." {
		t.Errorf("go-tools builds with %q (ADR 0044 rule 7)", got)
	}
}

func TestRecipeFindsAttributesHeaderAndBody(t *testing.T) {
	t.Parallel()
	jf := "# comment\nother:\n    x\n\n[positional-arguments]\n[private]\nname *A: dep\n    one\n    two \"$@\"\n\nlater:\n    three\n"
	attrs, header, body, ok := Recipe(jf, "name")
	if !ok || header != "name *A: dep" || strings.Join(attrs, ",") != "[positional-arguments],[private]" || strings.Join(body, "|") != "one|two \"$@\"|" {
		t.Errorf("attrs %q header %q body %q ok %v", attrs, header, body, ok)
	}
	if _, _, _, ok := Recipe(jf, "absent"); ok {
		t.Error("found a recipe that is not there")
	}
}
