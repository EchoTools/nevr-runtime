package main

import (
	"bytes"
	"context"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"strconv"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/testkit"
)

// seqEnv names the file every fake appends its call to, so a test sees the order across the three programs.
const seqEnv = "TEST_CHANGED_SEQ"

const (
	// failCode is the exit code a fake is told to return for the test that fails.
	failCode = 7
	// batchExit is the exit code a fake batch is told to return when it fails before it runs one of its binaries.
	batchExit = 3
	// componentACalls is how many calls a run of component a makes: build, seams, and one test-auth-unit call for both gtests.
	componentACalls = 3
	// componentATests is how many tests a run of component a makes: build, seams and the two gtest ids.
	componentATests = 4
)

// rcFile is the file in a fake's directory that holds the codes it is to return, one "<key>\t<code>" a line. A key is the
// whole argv, or, for test-auth-unit, "test-auth-unit <binary>" (that binary's result line: "omit" for none) and
// "exit test-auth-unit <binaries>" (the batch's own exit code).
const rcFile = "rc.tsv"

// omitted is the rcFile code that makes a fake write no result line for a binary.
const omitted = "omit"

// fakeTestAuthUnit is the justfile recipe a gtest batch runs.
const fakeTestAuthUnit = "test-auth-unit"

func fake(c *testkit.Call) int {
	line := c.Name + " " + strings.Join(c.Args, " ")
	env := lookup(c.Environ)
	if seq := env(seqEnv); seq != "" {
		if err := appendLine(seq, line); err != nil {
			return exitUsage
		}
	}
	if fifo := env(blockEnv); fifo != "" && c.Name == "just" && len(c.Args) == 1 && c.Args[0] == "build" {
		// The build waits until the test has cancelled the run: the run must then stop before the next test.
		in, err := os.Open(fifo)
		if err != nil {
			return exitUsage
		}
		io.ReadAll(in)
		in.Close()
	}
	c.Stdout.Write([]byte("fake " + line + "\n"))
	rows := codeRows(c.FakeDir)
	if c.Name == "just" && len(c.Args) > 0 && c.Args[0] == fakeTestAuthUnit {
		return gtestBatch(c.Args, rows, env(resultsEnv))
	}
	return codeOf(rows, strings.Join(c.Args, " "))
}

// gtestBatch plays test-auth-unit: one "<binary>\t<code>" line per binary to the results file, and the batch's exit code,
// which is the row for the whole batch when there is one and else 1 when any binary's code is not 0.
func gtestBatch(args []string, rows map[string]string, results string) int {
	nonzero := false
	for _, bin := range args[1:] {
		v, ok := rows[fakeTestAuthUnit+" "+bin]
		if ok && v == omitted {
			continue
		}
		n := codeOf(rows, fakeTestAuthUnit+" "+bin)
		if results != "" {
			if err := appendLine(results, bin+"\t"+strconv.Itoa(n)); err != nil {
				return exitUsage
			}
		}
		nonzero = nonzero || n != 0
	}
	batchKey := "exit " + strings.Join(args, " ")
	if _, ok := rows[batchKey]; ok {
		return codeOf(rows, batchKey)
	}
	if nonzero {
		return exitFailed
	}
	return 0
}

// codeRows reads the rcFile in dir: the key of each row, and its value.
func codeRows(dir string) map[string]string {
	rows := map[string]string{}
	data, _ := os.ReadFile(filepath.Join(dir, rcFile))
	for _, row := range strings.Split(string(data), "\n") {
		if key, v, ok := strings.Cut(row, "\t"); ok {
			rows[key] = v
		}
	}
	return rows
}

// codeOf is the exit code of key's row, 0 when there is none.
func codeOf(rows map[string]string, key string) int {
	n, _ := strconv.Atoi(rows[key])
	return n
}

// appendLine appends line and a newline to path.
func appendLine(path, line string) error {
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_APPEND|os.O_CREATE, filePerm)
	if err != nil {
		return err
	}
	if _, err := f.WriteString(line + "\n"); err != nil {
		f.Close()
		return err
	}
	return f.Close()
}

func TestMain(m *testing.M) {
	testkit.Main(m, testkit.Fakes{"just": fake, "go": fake, "python3": fake})
}

// testMap is the fixture's map, schema 2: component a holds two gtests, tools holds the map itself, and docs has no tests.
// The lines the tests edit are whole lines of it (oldSrcLine, scannersLine), so an edit is one exact replacement.
const testMap = `{"schema": 2,
 "about": ["the fixture of tools/cmd/test-changed's tests"],
 "targets": {"build": ["just", "build"], "seams": ["python3", "tools/verify_host_seams.py", "build/{preset}"]},
 "scanners": [],
 "components": [
  {"name": "src/a", "dir": "src/a", "files": {"src/a/x.cpp": ["build", "gtest:test_a", "gtest:test_b", "seams"]}},
  {"name": "tools", "dir": "tools", "files": {"tools/y.py": ["pytest:test_y", "go:cmd/x"], "tools/test_map.json": ["go:internal/testmap"]}},
  {"name": "src/h", "dir": "src/h", "files": {"src/h/x.cpp": ["build", "gtest:test_h", "just:test-quest-foo", "just:test-android", "pytest:test_launch_client", "pytest:test_fast"]}},
  {"name": "src", "dir": "src", "files": {"src/old.cpp": ["gtest:test_old"]}},
  {"name": "docs", "dir": "docs", "none": {"docs/a.md": "no test reads it"}}
 ]}
`

// oldSrcLine is the component of src/old.cpp, as testMap writes it (with its trailing comma and newline).
const oldSrcLine = `  {"name": "src", "dir": "src", "files": {"src/old.cpp": ["gtest:test_old"]}},` + "\n"

// schema2Map is testMap without the src component: src/old.cpp is then covered only by the map at the merge-base.
func schema2Map() string { return strings.Replace(testMap, oldSrcLine, "", 1) }

type fixture struct {
	t    *testing.T
	dir  string
	env  []string
	seq  string
	fake map[string]*testkit.FakeCommand
}

func (f *fixture) git(args ...string) {
	f.t.Helper()
	cmd := exec.Command("git", args...)
	cmd.Dir, cmd.Env = f.dir, f.env
	if out, err := cmd.CombinedOutput(); err != nil {
		f.t.Fatalf("git %s: %v\n%s", strings.Join(args, " "), err, out)
	}
}

func (f *fixture) write(rel, text string) {
	f.t.Helper()
	p := filepath.Join(f.dir, rel)
	if err := os.MkdirAll(filepath.Dir(p), dirPerm); err != nil {
		f.t.Fatal(err)
	}
	if err := os.WriteFile(p, []byte(text), filePerm); err != nil {
		f.t.Fatal(err)
	}
}

func (f *fixture) commit(msg string) {
	f.t.Helper()
	f.git("add", "-A")
	f.git("commit", "-q", "-m", msg)
}

// rows writes the fake just's rcFile: one row a line.
func (f *fixture) rows(lines ...string) {
	f.t.Helper()
	text := strings.Join(lines, "\n") + "\n"
	if err := os.WriteFile(filepath.Join(f.fake["just"].Dir(), rcFile), []byte(text), filePerm); err != nil {
		f.t.Fatal(err)
	}
}

// resultsDir is the directory a run's results files go in: below NEVR_WORK_ROOT of the fixture's environment.
func (f *fixture) resultsDir() string {
	return filepath.Join(testkit.Getenv(f.env)("NEVR_WORK_ROOT"), testRunsDir, tool)
}

// newFixture is a checkout whose origin/main is a commit holding the map and one file of each component, with the
// three programs the map's tests start replaced by fakes.
func newFixture(t *testing.T) *fixture {
	t.Helper()
	f := &fixture{t: t, dir: filepath.Join(t.TempDir(), "repo"), fake: map[string]*testkit.FakeCommand{}}
	f.seq = filepath.Join(t.TempDir(), "seq")
	var dirs []string
	for _, name := range []string{"just", "go", "python3"} {
		f.fake[name] = testkit.Fake(t, name, "DISPLAY", "WAYLAND_DISPLAY", "PRESET", resultsEnv)
		dirs = append(dirs, f.fake[name].Dir())
	}
	// A results variable the caller's environment carries: no test but the gtest batch may see it.
	f.env = testkit.WithPath(testkit.Env(t, "DISPLAY=:99", "WAYLAND_DISPLAY=wayland-9", "PRESET=mingw-x", seqEnv+"="+f.seq, resultsEnv+"=/nonexistent/stale"), dirs...)
	if err := os.MkdirAll(f.dir, dirPerm); err != nil {
		t.Fatal(err)
	}
	f.git("init", "-q", "-b", "main")
	f.write("tools/test_map.json", testMap)
	f.write("src/a/x.cpp", "x\n")
	f.write("src/old.cpp", "old\n")
	f.write("src/h/x.cpp", "h\n")
	f.write("tools/y.py", "y\n")
	f.write("docs/a.md", "a\n")
	f.commit("base")
	f.git("update-ref", "refs/remotes/origin/main", "HEAD")
	f.git("checkout", "-q", "-b", "branch")
	return f
}

func (f *fixture) run(args ...string) (int, string, string) {
	f.t.Helper()
	var out, errb bytes.Buffer
	code := run(context.Background(), append([]string{"-repo", f.dir}, args...), f.env, &out, &errb)
	return code, out.String(), errb.String()
}

func (f *fixture) calls() []string {
	f.t.Helper()
	data, err := os.ReadFile(f.seq)
	if os.IsNotExist(err) {
		return nil
	}
	if err != nil {
		f.t.Fatal(err)
	}
	return strings.Split(strings.TrimSuffix(string(data), "\n"), "\n")
}

// testEnds is the test ids and exit codes of the run's test_end records, as "<id>=<exit>", in the order written.
func (f *fixture) testEnds() []string {
	f.t.Helper()
	var ends []string
	for _, r := range testkit.RunLogRecords(f.t, f.env, tool) {
		if r["event"] == "test_end" {
			ends = append(ends, r["test"].(string)+"="+strconv.Itoa(int(r["exit"].(float64))))
		}
	}
	return ends
}

// batchCall is the one call of the fake just that ran test-auth-unit, with its environment.
func (f *fixture) batchCall() (testkit.Record, bool) {
	f.t.Helper()
	for _, c := range f.fake["just"].Calls(f.t) {
		if len(c.Args) > 0 && c.Args[0] == fakeTestAuthUnit {
			return c, true
		}
	}
	return testkit.Record{}, false
}

func TestRunsTheUnionOfTheChangedComponentsTestsInOrderWithoutTheDisplay(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	f.write("src/a/x.cpp", "x2\n")
	f.commit("change a")
	f.write("tools/y.py", "y2\n") // uncommitted: the diff against the merge-base includes the working tree
	code, out, errs := f.run()
	if code != 0 {
		t.Fatalf("exit %d\n%s\n%s", code, out, errs)
	}
	want := []string{
		"just build",
		"python3 tools/verify_host_seams.py build/mingw-x",
		"just test-auth-unit test_a test_b",
		"go -C tools test -count=1 ./cmd/x",
		"python3 tools/run_tool_tests.py tools/tests/test_y.py",
	}
	if got := f.calls(); !reflect.DeepEqual(got, want) {
		t.Errorf("calls = %q\nwant %q", got, want)
	}
	for _, name := range []string{"just", "go", "python3"} {
		for _, c := range f.fake[name].Calls(t) {
			if _, ok := c.Env["DISPLAY"]; ok {
				t.Errorf("%s %q was started with DISPLAY", name, c.Args)
			}
			if _, ok := c.Env["WAYLAND_DISPLAY"]; ok {
				t.Errorf("%s %q was started with WAYLAND_DISPLAY", name, c.Args)
			}
			if c.Env["PRESET"] != "mingw-x" {
				t.Errorf("%s %q: PRESET = %q, want mingw-x", name, c.Args, c.Env["PRESET"])
			}
			if c.Dir != f.dir {
				t.Errorf("%s %q ran in %s, want the checkout %s", name, c.Args, c.Dir, f.dir)
			}
			// Only the gtest batch gets the results file; the stale one the caller's environment carries reaches no test.
			if len(c.Args) > 0 && c.Args[0] == fakeTestAuthUnit {
				if !strings.HasPrefix(c.Env[resultsEnv], f.resultsDir()+string(filepath.Separator)) {
					t.Errorf("batch %q: %s = %q, want a file under %s", c.Args, resultsEnv, c.Env[resultsEnv], f.resultsDir())
				}
			} else if c.Env[resultsEnv] != "" {
				t.Errorf("%s %q was started with %s=%q", name, c.Args, resultsEnv, c.Env[resultsEnv])
			}
		}
	}
	for _, line := range []string{"src/a/x.cpp -> src/a : ", "tools/y.py -> tools : ", "gtest:test_a exit 0", "gtest:test_b exit 0", "6 test(s): all passed"} {
		if !strings.Contains(out, line) {
			t.Errorf("output lacks %q:\n%s", line, out)
		}
	}
	want2 := []string{"build=0", "seams=0", "gtest:test_a=0", "gtest:test_b=0", "go:cmd/x=0", "pytest:test_y=0"}
	if got := f.testEnds(); !reflect.DeepEqual(got, want2) {
		t.Errorf("test_end records = %q, want %q", got, want2)
	}
}

func TestGTestIdsRunAsOneCallAndEachGetsItsOwnResult(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	f.rows("test-auth-unit test_b\t" + strconv.Itoa(failCode))
	f.write("src/a/x.cpp", "x2\n")
	f.commit("change a")
	code, out, errs := f.run()
	if code != exitFailed {
		t.Fatalf("exit %d, want %d\n%s\n%s", code, exitFailed, out, errs)
	}
	var batches []string
	for _, c := range f.calls() {
		if strings.HasPrefix(c, "just test-auth-unit") {
			batches = append(batches, c)
		}
	}
	if want := []string{"just test-auth-unit test_a test_b"}; !reflect.DeepEqual(batches, want) {
		t.Errorf("gtest calls = %q, want one call with both names %q", batches, want)
	}
	for _, line := range []string{"gtest:test_a exit 0", "gtest:test_b exit " + strconv.Itoa(failCode), "4 test(s): 3 passed, 1 failed: gtest:test_b (exit " + strconv.Itoa(failCode) + ")"} {
		if !strings.Contains(out, line) {
			t.Errorf("output lacks %q:\n%s", line, out)
		}
	}
	want := []string{"build=0", "seams=0", "gtest:test_a=0", "gtest:test_b=" + strconv.Itoa(failCode)}
	if got := f.testEnds(); !reflect.DeepEqual(got, want) {
		t.Errorf("test_end records = %q, want %q", got, want)
	}
	c, ok := f.batchCall()
	if !ok {
		t.Fatal("no batch call recorded")
	}
	kept, err := os.ReadFile(c.Env[resultsEnv])
	if err != nil {
		t.Fatalf("the results file is not kept: %v", err)
	}
	if want := "test_a\t0\ntest_b\t" + strconv.Itoa(failCode) + "\n"; string(kept) != want {
		t.Errorf("results file = %q, want %q", kept, want)
	}
}

func TestAGTestWithNoResultGetsTheBatchesCode(t *testing.T) {
	t.Parallel()
	cases := []struct {
		name string
		rows []string
		exit int
		note string
		want int
	}{
		{
			name: "the batch failed before it ran the binary",
			rows: []string{"test-auth-unit test_b\t" + omitted, "exit test-auth-unit test_a test_b\t" + strconv.Itoa(batchExit)},
			exit: exitFailed,
			note: "(no result: the batch exited " + strconv.Itoa(batchExit) + " before running it)",
			want: batchExit,
		},
		{
			name: "the batch exited 0 without a line for the binary",
			rows: []string{"test-auth-unit test_b\t" + omitted},
			exit: exitFailed,
			note: "(no result: the batch exited 0 before running it)",
			want: exitFailed,
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			f := newFixture(t)
			f.rows(tc.rows...)
			f.write("src/a/x.cpp", "x2\n")
			f.commit("change a")
			code, out, errs := f.run()
			if code != tc.exit {
				t.Fatalf("exit %d, want %d\n%s\n%s", code, tc.exit, out, errs)
			}
			line := "gtest:test_b exit " + strconv.Itoa(tc.want) + " " + tc.note
			if !strings.Contains(out, line) {
				t.Errorf("output lacks %q:\n%s", line, out)
			}
			if !strings.Contains(out, "gtest:test_a exit 0\n") {
				t.Errorf("the binary with a result is not reported as passed:\n%s", out)
			}
			var found bool
			for _, r := range testkit.RunLogRecords(t, f.env, tool) {
				if r["event"] == "test_end" && r["test"] == "gtest:test_b" {
					found = true
					if r["note"] != tc.note || int(r["exit"].(float64)) != tc.want {
						t.Errorf("test_end of gtest:test_b = %v, want exit %d note %q", r, tc.want, tc.note)
					}
				}
			}
			if !found {
				t.Error("no test_end record for gtest:test_b")
			}
		})
	}
}

func TestAFailedTestFailsTheRunAndTheRestStillRun(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	f.rows("test-auth-unit test_a\t" + strconv.Itoa(failCode))
	f.write("src/a/x.cpp", "x2\n")
	f.commit("change a")
	code, out, _ := f.run()
	if code != exitFailed {
		t.Fatalf("exit %d, want %d\n%s", code, exitFailed, out)
	}
	if got := len(f.calls()); got != componentACalls {
		t.Errorf("%d calls, want all %d of component a: %q", got, componentACalls, f.calls())
	}
	for _, line := range []string{"gtest:test_a exit " + strconv.Itoa(failCode), strconv.Itoa(componentATests) + " test(s): 3 passed, 1 failed: gtest:test_a (exit " + strconv.Itoa(failCode) + ")"} {
		if !strings.Contains(out, line) {
			t.Errorf("output lacks %q:\n%s", line, out)
		}
	}
}

func TestAPathTheMapDoesNotCoverFailsBeforeAnythingRuns(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	f.write("src/a/x.cpp", "x2\n")
	f.write("src/b/new.cpp", "new\n")
	f.commit("change a, add b")
	code, out, errs := f.run()
	if code != exitUncovered {
		t.Fatalf("exit %d, want %d\n%s\n%s", code, exitUncovered, out, errs)
	}
	if !strings.Contains(errs, "FAIL src/b/new.cpp is not in tools/test_map.json: run just test-map and commit the map it writes") {
		t.Errorf("stderr does not name the path and the fix:\n%s", errs)
	}
	if c := f.calls(); len(c) != 0 {
		t.Errorf("tests ran although the map does not cover a path: %q", c)
	}
	var found bool
	for _, r := range testkit.RunLogRecords(t, f.env, tool) {
		found = found || r["event"] == "uncovered"
	}
	if !found {
		t.Error("no uncovered record in the run log")
	}
}

func TestAGitMoveBetweenComponentsRunsBothComponentsTests(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	f.git("mv", "src/old.cpp", "src/a/moved.cpp")
	m := strings.Replace(schema2Map(), `"src/a/x.cpp": ["build", "gtest:test_a", "gtest:test_b", "seams"]`,
		`"src/a/x.cpp": ["build", "gtest:test_a", "gtest:test_b", "seams"], "src/a/moved.cpp": ["gtest:test_c"]`, 1)
	f.write("tools/test_map.json", m)
	f.commit("move old.cpp into component a")
	code, out, errs := f.run()
	if code != 0 {
		t.Fatalf("exit %d\n%s\n%s", code, out, errs)
	}
	// Without --no-renames git shows only the new name, and test_old would not run.
	var batch string
	for _, c := range f.calls() {
		if strings.HasPrefix(c, "just test-auth-unit") {
			batch = c
		}
	}
	for _, name := range []string{"test_c", "test_old"} {
		if !strings.Contains(" "+batch+" ", " "+name+" ") {
			t.Errorf("the gtest batch %q lacks %s: the old path's component was not run", batch, name)
		}
	}
	if !strings.Contains(out, "src/old.cpp -> src : gtest:test_old (deleted: looked up in the map at the merge-base)") {
		t.Errorf("output lacks the deleted path's line:\n%s", out)
	}
}

func TestADeletedPathIsLookedUpInTheMapAtTheMergeBase(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	if err := os.Remove(filepath.Join(f.dir, "src/old.cpp")); err != nil {
		t.Fatal(err)
	}
	f.write("tools/test_map.json", schema2Map())
	f.commit("drop old.cpp and its entry")
	code, out, errs := f.run()
	if code != 0 {
		t.Fatalf("exit %d\n%s\n%s", code, out, errs)
	}
	if want := []string{"just test-auth-unit test_old", "go -C tools test -count=1 ./internal/testmap"}; !reflect.DeepEqual(f.calls(), want) {
		t.Errorf("calls = %q, want %q", f.calls(), want)
	}
	if !strings.Contains(out, "(deleted: looked up in the map at the merge-base)") {
		t.Errorf("output does not say the deleted path came from the base map:\n%s", out)
	}
}

func TestNoChangeAndANoTestComponentRunNothing(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	code, out, _ := f.run()
	if code != 0 || !strings.Contains(out, "nothing to run") {
		t.Errorf("no change: exit %d\n%s", code, out)
	}
	f.write("docs/a.md", "a2\n")
	f.commit("docs")
	code, out, _ = f.run()
	if code != 0 || !strings.Contains(out, "docs/a.md -> docs : no tests of its own: no test reads it") || !strings.Contains(out, "no tests to run") {
		t.Errorf("docs only: exit %d\n%s", code, out)
	}
	if c := f.calls(); len(c) != 0 {
		t.Errorf("tests ran: %q", c)
	}
}

func TestAScannerAddsItsTestsToEveryPathItMatches(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	m := strings.Replace(testMap, `"scanners": []`, `"scanners": [{"name": "doc-scan", "globs": ["docs/**"], "tests": ["go:cmd/doc"]}]`, 1)
	f.write("tools/test_map.json", m)
	f.commit("the map with a scanner")
	f.git("update-ref", "refs/remotes/origin/main", "HEAD")
	f.write("docs/a.md", "a2\n")
	f.commit("docs")
	code, out, errs := f.run()
	if code != 0 {
		t.Fatalf("exit %d\n%s\n%s", code, out, errs)
	}
	if want := []string{"go -C tools test -count=1 ./cmd/doc"}; !reflect.DeepEqual(f.calls(), want) {
		t.Errorf("calls = %q, want %q", f.calls(), want)
	}
	if !strings.Contains(out, "docs/a.md -> docs : no tests of its own: no test reads it ; scanners doc-scan: go:cmd/doc") {
		t.Errorf("output does not name the scanner and its test:\n%s", out)
	}
}

func TestAScannerOnAFileWithItsOwnTestsKeepsBothAndNamesWhatItAdds(t *testing.T) {
	t.Parallel()
	cases := []struct {
		name    string
		tests   string
		calls   []string
		newPart string
	}{
		{
			name:    "the scanner adds a test",
			tests:   `"go:cmd/doc"`,
			calls:   []string{"just build", "python3 tools/verify_host_seams.py build/mingw-x", "just test-auth-unit test_a test_b", "go -C tools test -count=1 ./cmd/doc"},
			newPart: "go:cmd/doc",
		},
		{
			name:    "the scanner adds nothing the file does not already have",
			tests:   `"build"`,
			calls:   []string{"just build", "python3 tools/verify_host_seams.py build/mingw-x", "just test-auth-unit test_a test_b"},
			newPart: "no new tests",
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			f := newFixture(t)
			m := strings.Replace(testMap, `"scanners": []`, `"scanners": [{"name": "a-scan", "globs": ["src/a/**"], "tests": [`+tc.tests+`]}]`, 1)
			f.write("tools/test_map.json", m)
			f.commit("the map with a scanner on component a")
			f.git("update-ref", "refs/remotes/origin/main", "HEAD")
			f.write("src/a/x.cpp", "x2\n")
			f.commit("change a")
			code, out, errs := f.run()
			if code != 0 {
				t.Fatalf("exit %d\n%s\n%s", code, out, errs)
			}
			if got := f.calls(); !reflect.DeepEqual(got, tc.calls) {
				t.Errorf("calls = %q, want %q", got, tc.calls)
			}
			line := "src/a/x.cpp -> src/a : build gtest:test_a gtest:test_b seams ; scanners a-scan: " + tc.newPart
			if !strings.Contains(out, line) {
				t.Errorf("output lacks %q:\n%s", line, out)
			}
		})
	}
}

// blockEnv names a fifo the fake just build reads until the test opens it for writing.
const blockEnv = "TEST_CHANGED_BLOCK_BUILD"

func TestAnInterruptBetweenTwoTestsRunsNoLaterTest(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	f.write("src/a/x.cpp", "x2\n")
	f.commit("change a")
	fifo := testkit.Fifo(t)
	f.env = append(append([]string(nil), f.env...), blockEnv+"="+fifo)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	type finished struct {
		code      int
		out, errb string
	}
	done := make(chan finished, 1)
	go func() {
		var out, errb bytes.Buffer
		code := run(ctx, []string{"-repo", f.dir}, f.env, &out, &errb)
		done <- finished{code, out.String(), errb.String()}
	}()
	testkit.WaitFor(t, "the build to start", func() bool {
		data, _ := os.ReadFile(f.seq)
		return strings.Contains(string(data), "just build")
	})
	cancel()
	w, err := os.OpenFile(fifo, os.O_WRONLY, 0)
	if err != nil {
		t.Fatal(err)
	}
	w.Close()
	res := <-done
	if res.code != exitInterrupted {
		t.Fatalf("exit %d, want %d\n%s\n%s", res.code, exitInterrupted, res.out, res.errb)
	}
	// The build ran (1 test), so 3 of the 4 are not run: the count is what is left, not the whole plan.
	if want := "interrupted before seams; 3 test(s) not run"; !strings.Contains(res.errb, want) {
		t.Errorf("stderr lacks %q:\n%s", want, res.errb)
	}
	if got := f.calls(); !reflect.DeepEqual(got, []string{"just build"}) {
		t.Errorf("calls = %q, want only the build", got)
	}
	if got := f.testEnds(); !reflect.DeepEqual(got, []string{"build=0"}) {
		t.Errorf("test_end records = %q, want only build=0", got)
	}
}

func TestAnInterruptBeforeTheFirstTestRunsNothing(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	f.write("src/a/x.cpp", "x2\n")
	f.commit("change a")
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	var out, errb bytes.Buffer
	code := run(ctx, []string{"-repo", f.dir}, f.env, &out, &errb)
	if code != exitInterrupted {
		t.Fatalf("exit %d, want %d\n%s\n%s", code, exitInterrupted, out.String(), errb.String())
	}
	if want := "interrupted before build; 4 test(s) not run"; !strings.Contains(errb.String(), want) {
		t.Errorf("stderr lacks %q:\n%s", want, errb.String())
	}
	if c := f.calls(); len(c) != 0 {
		t.Errorf("tests ran after the interrupt: %q", c)
	}
}

func TestReadResultsTakesOneExitCodePerBinaryAndRefusesOtherLines(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	good := filepath.Join(dir, "good.tsv")
	if err := os.WriteFile(good, []byte("test_a\t0\ntest_b\t"+strconv.Itoa(failCode)+"\n"), filePerm); err != nil {
		t.Fatal(err)
	}
	got, err := readResults(good)
	if err != nil || !reflect.DeepEqual(got, map[string]int{"test_a": 0, "test_b": failCode}) {
		t.Errorf("readResults(good) = %v, %v", got, err)
	}
	// Each bad file has one good line, then a line that is not a result: no tab, a code that is not a number, no binary.
	for _, bad := range []string{"not a result", "test_a\tzero", "\t0"} {
		file := filepath.Join(dir, "bad.tsv")
		if err := os.WriteFile(file, []byte("test_a\t0\n"+bad+"\n"), filePerm); err != nil {
			t.Fatal(err)
		}
		got, err := readResults(file)
		if err == nil {
			t.Errorf("the line %q was read without an error", bad)
		}
		if !reflect.DeepEqual(got, map[string]int{"test_a": 0}) {
			t.Errorf("readResults kept %v for the line %q, want the lines before it", got, bad)
		}
	}
	if _, err := readResults(filepath.Join(dir, "missing.tsv")); err == nil {
		t.Error("a results file that does not exist was read without an error")
	}
}

func TestDryRunPrintsThePlanAndRunsNothing(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	f.write("src/a/x.cpp", "x2\n")
	f.commit("change a")
	code, out, _ := f.run("-n", "-preset", "mingw-dev")
	if code != 0 || !strings.Contains(out, "4 test(s), in this order: build seams gtest:test_a gtest:test_b") || !strings.Contains(out, "-n: nothing run") {
		t.Errorf("exit %d\n%s", code, out)
	}
	if c := f.calls(); len(c) != 0 {
		t.Errorf("-n ran tests: %q", c)
	}
	if _, err := os.Stat(f.resultsDir()); !os.IsNotExist(err) {
		t.Errorf("-n made a results directory: %v", err)
	}
}

func TestABrokenMapOrABaseThatIsNotThereIsAUsageError(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	f.write("tools/test_map.json", `{"schema": 3}`)
	if code, _, errs := f.run(); code != exitUsage || !strings.Contains(errs, "FAIL") {
		t.Errorf("broken map: exit %d, stderr %s", code, errs)
	}
	f.write("tools/test_map.json", testMap)
	if code, _, errs := f.run("-base", "origin/nothing"); code != exitUsage || !strings.Contains(errs, "git merge-base origin/nothing HEAD") {
		t.Errorf("missing base: exit %d, stderr %s", code, errs)
	}
}

// heavyChange makes the change of src/h/x.cpp, whose tests are one unit gtest, one unit pytest and four heavy ids.
func heavyChange(f *fixture) {
	f.write("src/h/x.cpp", "h2\n")
	f.commit("change h")
}

func TestALandSkipsTheHeavyTestsAndListsEachWithItsTimeAndReason(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	heavyChange(f)
	code, out, errs := f.run()
	if code != 0 {
		t.Fatalf("exit %d\n%s\n%s", code, out, errs)
	}
	want := []string{
		"just build",
		"just test-auth-unit test_h",
		"python3 tools/run_tool_tests.py tools/tests/test_fast.py",
	}
	if got := f.calls(); !reflect.DeepEqual(got, want) {
		t.Errorf("calls = %q\nwant %q", got, want)
	}
	for _, line := range []string{
		"skipped heavy just:test-android (fastest run 3 s): it builds and runs the Android or Quest toolchain",
		"skipped heavy just:test-quest-foo (no run time kept): it builds and runs the Android or Quest toolchain",
		"skipped heavy pytest:test_launch_client (fastest run 31 s): its fastest recorded run is over the 10 s bound of a land test",
	} {
		if !strings.Contains(out, line) {
			t.Errorf("output lacks %q:\n%s", line, out)
		}
	}
	if !strings.Contains(out, "3 test(s), in this order: build gtest:test_h pytest:test_fast") {
		t.Errorf("the plan line names a heavy test or is missing:\n%s", out)
	}
	skipped := 0
	for _, r := range testkit.RunLogRecords(t, f.env, tool) {
		if r["event"] == "skipped_heavy" {
			skipped++
		}
	}
	if skipped != 3 {
		t.Errorf("the run log has %d skipped_heavy records, want 3", skipped)
	}
}

func TestTheHeavyFlagRunsTheHeavyTestsToo(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	heavyChange(f)
	code, out, errs := f.run("-heavy")
	if code != 0 {
		t.Fatalf("exit %d\n%s\n%s", code, out, errs)
	}
	want := []string{
		"just build",
		"just test-auth-unit test_h",
		"just test-android",
		"just test-quest-foo",
		"python3 tools/run_tool_tests.py tools/tests/test_fast.py",
		"python3 tools/run_tool_tests.py tools/tests/test_launch_client.py",
	}
	if got := f.calls(); !reflect.DeepEqual(got, want) {
		t.Errorf("calls = %q\nwant %q", got, want)
	}
	if strings.Contains(out, "skipped heavy") {
		t.Errorf("-heavy skipped a test:\n%s", out)
	}
}

func TestADryRunNamesTheSkippedHeavyTestsAndRunsNothing(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	heavyChange(f)
	code, out, _ := f.run("-n")
	if code != 0 || !strings.Contains(out, "skipped heavy just:test-android") {
		t.Errorf("exit %d, output:\n%s", code, out)
	}
	if got := f.calls(); len(got) != 0 {
		t.Errorf("-n ran %q", got)
	}
}
