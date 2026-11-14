package main

import (
	"bytes"
	"context"
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
	filePerm = 0o644
	dirPerm  = 0o755
	// failCode is the exit code a fake is told to return for the test that fails.
	failCode = 7
	// componentATests is how many tests component a of testMap lists.
	componentATests = 3
)

// rcFile is the file in a fake's directory that holds the exit codes it is to return, one "<args>\t<code>" a line.
const rcFile = "rc.tsv"

func fake(c *testkit.Call) int {
	line := c.Name + " " + strings.Join(c.Args, " ")
	if seq := lookup(c.Environ)(seqEnv); seq != "" {
		f, err := os.OpenFile(seq, os.O_WRONLY|os.O_APPEND|os.O_CREATE, filePerm)
		if err != nil {
			return exitUsage
		}
		f.WriteString(line + "\n")
		f.Close()
	}
	c.Stdout.Write([]byte("fake " + line + "\n"))
	data, _ := os.ReadFile(filepath.Join(c.FakeDir, rcFile))
	for _, row := range strings.Split(string(data), "\n") {
		args, code, ok := strings.Cut(row, "\t")
		if ok && args == strings.Join(c.Args, " ") {
			n, _ := strconv.Atoi(code)
			return n
		}
	}
	return 0
}

func TestMain(m *testing.M) {
	testkit.Main(m, testkit.Fakes{"just": fake, "go": fake, "python3": fake})
}

const testMap = `{"schema": 1,
 "targets": {"build": ["just", "build"], "seams": ["python3", "tools/verify_host_seams.py", "build/{preset}"]},
 "components": [
  {"name": "a", "paths": ["src/a/"], "tests": ["build", "gtest:test_a", "seams"]},
  {"name": "tool", "paths": ["tools/y.py"], "tests": ["pytest:test_y", "go:cmd/x"]},
  {"name": "old", "paths": ["src/old.cpp"], "tests": ["gtest:test_old"]},
  {"name": "map", "paths": ["tools/test_map.json"], "tests": ["go:internal/testmap"]},
  {"name": "docs", "paths": ["docs/"], "none": "no test reads it"}
 ]}
`

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

// newFixture is a checkout whose origin/main is a commit holding the map and one file of each component, with the
// three programs the map's tests start replaced by fakes.
func newFixture(t *testing.T) *fixture {
	t.Helper()
	f := &fixture{t: t, dir: filepath.Join(t.TempDir(), "repo"), fake: map[string]*testkit.FakeCommand{}}
	f.seq = filepath.Join(t.TempDir(), "seq")
	var dirs []string
	for _, name := range []string{"just", "go", "python3"} {
		f.fake[name] = testkit.Fake(t, name, "DISPLAY", "WAYLAND_DISPLAY", "PRESET")
		dirs = append(dirs, f.fake[name].Dir())
	}
	f.env = testkit.WithPath(testkit.Env(t, "DISPLAY=:99", "WAYLAND_DISPLAY=wayland-9", "PRESET=mingw-x", seqEnv+"="+f.seq), dirs...)
	if err := os.MkdirAll(f.dir, dirPerm); err != nil {
		t.Fatal(err)
	}
	f.git("init", "-q", "-b", "main")
	f.write("tools/test_map.json", testMap)
	f.write("src/a/x.cpp", "x\n")
	f.write("src/old.cpp", "old\n")
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
		"just test-auth-unit test_a",
		"go -C tools test ./cmd/x",
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
		}
	}
	for _, line := range []string{"src/a/x.cpp -> component a (src/a/)", "tools/y.py -> component tool (tools/y.py)", "gtest:test_a exit 0", "5 test(s): all passed"} {
		if !strings.Contains(out, line) {
			t.Errorf("output lacks %q:\n%s", line, out)
		}
	}
	recs := testkit.RunLogRecords(t, f.env, tool)
	var ends []string
	for _, r := range recs {
		if r["event"] == "test_end" {
			ends = append(ends, r["test"].(string)+"="+strconv.Itoa(int(r["exit"].(float64))))
		}
	}
	if want := []string{"build=0", "seams=0", "gtest:test_a=0", "go:cmd/x=0", "pytest:test_y=0"}; !reflect.DeepEqual(ends, want) {
		t.Errorf("test_end records = %q, want %q", ends, want)
	}
}

func TestAFailedTestFailsTheRunAndTheRestStillRun(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	if err := os.WriteFile(filepath.Join(f.fake["just"].Dir(), rcFile), []byte("test-auth-unit test_a\t"+strconv.Itoa(failCode)+"\n"), filePerm); err != nil {
		t.Fatal(err)
	}
	f.write("src/a/x.cpp", "x2\n")
	f.commit("change a")
	code, out, _ := f.run()
	if code != exitFailed {
		t.Fatalf("exit %d, want %d\n%s", code, exitFailed, out)
	}
	if got := len(f.calls()); got != componentATests {
		t.Errorf("%d calls, want all %d of component a: %q", got, componentATests, f.calls())
	}
	for _, line := range []string{"gtest:test_a exit " + strconv.Itoa(failCode), "3 test(s): 2 passed, 1 failed: gtest:test_a (exit " + strconv.Itoa(failCode) + ")"} {
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
	if !strings.Contains(errs, "FAIL src/b/new.cpp is not covered by tools/test_map.json") {
		t.Errorf("stderr does not name the path:\n%s", errs)
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

func TestADeletedPathIsLookedUpInTheMapAtTheMergeBase(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	if err := os.Remove(filepath.Join(f.dir, "src/old.cpp")); err != nil {
		t.Fatal(err)
	}
	f.write("tools/test_map.json", strings.Replace(testMap, `  {"name": "old", "paths": ["src/old.cpp"], "tests": ["gtest:test_old"]},`+"\n", "", 1))
	f.commit("drop old.cpp and its entry")
	code, out, errs := f.run()
	if code != 0 {
		t.Fatalf("exit %d\n%s\n%s", code, out, errs)
	}
	if want := []string{"just test-auth-unit test_old", "go -C tools test ./internal/testmap"}; !reflect.DeepEqual(f.calls(), want) {
		t.Errorf("calls = %q, want %q", f.calls(), want)
	}
	if !strings.Contains(out, "deleted: from the map at the merge-base") {
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
	if code != 0 || !strings.Contains(out, "no tests: no test reads it") || !strings.Contains(out, "no tests to run") {
		t.Errorf("docs only: exit %d\n%s", code, out)
	}
	if c := f.calls(); len(c) != 0 {
		t.Errorf("tests ran: %q", c)
	}
}

func TestDryRunPrintsThePlanAndRunsNothing(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	f.write("src/a/x.cpp", "x2\n")
	f.commit("change a")
	code, out, _ := f.run("-n", "-preset", "mingw-dev")
	if code != 0 || !strings.Contains(out, "3 test(s), in this order: build seams gtest:test_a") || !strings.Contains(out, "-n: nothing run") {
		t.Errorf("exit %d\n%s", code, out)
	}
	if c := f.calls(); len(c) != 0 {
		t.Errorf("-n ran tests: %q", c)
	}
}

func TestABrokenMapOrABaseThatIsNotThereIsAUsageError(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	f.write("tools/test_map.json", `{"schema": 2}`)
	if code, _, errs := f.run(); code != exitUsage || !strings.Contains(errs, "schema is 2") {
		t.Errorf("broken map: exit %d, stderr %s", code, errs)
	}
	f.write("tools/test_map.json", testMap)
	if code, _, errs := f.run("-base", "origin/nothing"); code != exitUsage || !strings.Contains(errs, "git merge-base origin/nothing HEAD") {
		t.Errorf("missing base: exit %d, stderr %s", code, errs)
	}
}
