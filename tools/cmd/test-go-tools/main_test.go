package main

import (
	"bytes"
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/runlog"
	"github.com/EchoTools/nevr-runtime/tools/internal/testkit"
)

// The fake `go`: prints the file "stdout" beside it to standard output and "stderr" to standard error, and exits with
// the number in "exit" (0 when there is none). A test writes those files before it runs the tool.
var fakes = testkit.Fakes{"go": func(c *testkit.Call) int {
	if out, err := os.ReadFile(filepath.Join(c.FakeDir, "stdout")); err == nil {
		c.Stdout.Write(out)
	}
	if out, err := os.ReadFile(filepath.Join(c.FakeDir, "stderr")); err == nil {
		c.Stderr.Write(out)
	}
	if raw, err := os.ReadFile(filepath.Join(c.FakeDir, "exit")); err == nil {
		n, _ := strconv.Atoi(strings.TrimSpace(string(raw)))
		return n
	}
	return 0
}}

func TestMain(m *testing.M) { testkit.Main(m, fakes) }

// rig is one run of the tool: a fake go that behaves as the test scripted, a log directory, an environment.
type rig struct {
	t      *testing.T
	fake   *testkit.FakeCommand
	logDir string
	env    []string
}

func newRig(t *testing.T, stdout string, exit int) *rig {
	t.Helper()
	f := testkit.Fake(t, "go")
	write := func(name, content string) {
		if err := os.WriteFile(filepath.Join(f.Dir(), name), []byte(content), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	write("stdout", stdout)
	write("exit", strconv.Itoa(exit))
	logDir := filepath.Join(t.TempDir(), "runs")
	env := testkit.WithPath(testkit.Env(t, "NEVR_TEST_LOG_DIR="+logDir), f.Dir())
	return &rig{t: t, fake: f, logDir: logDir, env: env}
}

func (r *rig) run(args ...string) (code int, stdout, stderr string) {
	r.t.Helper()
	var out, errb bytes.Buffer
	code = run(r.t.Context(), args, testkit.Getenv(r.env), strings.NewReader(""), &out, &errb)
	return code, out.String(), errb.String()
}

func (r *rig) streams() []string {
	r.t.Helper()
	files, err := filepath.Glob(filepath.Join(r.logDir, "*-go.jsonl"))
	if err != nil {
		r.t.Fatal(err)
	}
	return files
}

func ev(action, pkg, test string) string {
	b, _ := json.Marshal(map[string]string{"Action": action, "Package": pkg, "Test": test})
	return string(b) + "\n"
}

const pkg = "example.test/tools/internal/thing"

func passingStream() string {
	return ev("start", pkg, "") + ev("run", pkg, "TestA") + ev("pass", pkg, "TestA") + ev("run", pkg, "TestB") +
		ev("pass", pkg, "TestB") + ev("pass", pkg, "")
}

func TestAPassingRunExitsZeroAndKeepsTheStreamByteForByte(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	code, stdout, stderr := r.run()
	if code != 0 {
		t.Fatalf("exit %d, stdout %q, stderr %q", code, stdout, stderr)
	}
	files := r.streams()
	if len(files) != 1 {
		t.Fatalf("streams = %v, want one", files)
	}
	got, _ := os.ReadFile(files[0])
	if string(got) != passingStream() {
		t.Errorf("the kept stream differs from go test's output:\n%q", got)
	}
	for _, want := range []string{"2 tests passed", "1 package", files[0]} {
		if !strings.Contains(stdout, want) {
			t.Errorf("stdout %q lacks %q", stdout, want)
		}
	}
	if strings.Contains(stdout, "FAIL") {
		t.Errorf("stdout names a failure on a passing run: %q", stdout)
	}
}

func TestTheStreamIsNamedWithAUTCStampAndKeptBesideTheRunnersLog(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	r.run()
	r.run()
	files := r.streams()
	if len(files) != 2 {
		t.Fatalf("two runs left %d streams: %v", len(files), files)
	}
	stamp := regexp.MustCompile(`^\d{8}T\d{6}\.\d{6}Z-\d+-[0-9a-f]{16}-go\.jsonl$`)
	for _, f := range files {
		if !stamp.MatchString(filepath.Base(f)) {
			t.Errorf("%s does not match <UTC stamp>-<pid>-<random>-go.jsonl", filepath.Base(f))
		}
	}
}

func TestGoIsStartedWithTestCountOneJSONAndEveryPackage(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	r.run()
	calls := r.fake.Calls(t)
	if len(calls) != 1 || strings.Join(calls[0].Args, " ") != "test -count=1 -json ./..." {
		t.Errorf("go was called as %+v, want: go test -count=1 -json ./...", calls)
	}
}

func TestPackagePatternsGivenOnTheCommandLineReplaceTheDefault(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	r.run("./internal/testkit/", "./symcache/")
	calls := r.fake.Calls(t)
	if len(calls) != 1 || strings.Join(calls[0].Args, " ") != "test -count=1 -json ./internal/testkit/ ./symcache/" {
		t.Errorf("go was called as %+v", calls)
	}
}

func TestAFailingTestIsNamedAndTheExitCodeIsGos(t *testing.T) {
	t.Parallel()
	stream := ev("run", pkg, "TestA") + ev("pass", pkg, "TestA") + ev("run", pkg, "TestB") + ev("fail", pkg, "TestB") + ev("fail", pkg, "")
	r := newRig(t, stream, 1)
	code, stdout, _ := r.run()
	if code != 1 {
		t.Errorf("exit %d, want go test's 1", code)
	}
	if !strings.Contains(stdout, "FAIL "+pkg+" TestB") {
		t.Errorf("stdout does not name the failing test:\n%s", stdout)
	}
	if strings.Contains(stdout, pkg+" TestA") || strings.Contains(stdout, "did not build") {
		t.Errorf("stdout names a passing test:\n%s", stdout)
	}
	if !strings.Contains(stdout, "1 tests passed") || !strings.Contains(stdout, "1 failed") {
		t.Errorf("stdout lacks the counts:\n%s", stdout)
	}
	if len(r.streams()) != 1 {
		t.Error("a failing run kept no stream")
	}
}

func TestAFailingSubtestAndItsParentAreBothNamed(t *testing.T) {
	t.Parallel()
	stream := ev("fail", pkg, "TestP/sub") + ev("fail", pkg, "TestP") + ev("fail", pkg, "")
	_, stdout, _ := newRig(t, stream, 1).run()
	for _, want := range []string{"FAIL " + pkg + " TestP/sub", "FAIL " + pkg + " TestP\n"} {
		if !strings.Contains(stdout, want) {
			t.Errorf("stdout lacks %q:\n%s", want, stdout)
		}
	}
}

func TestAPackageThatDoesNotBuildIsNamedWithItsCompilerOutput(t *testing.T) {
	t.Parallel()
	build := func(action, out string) string {
		b, _ := json.Marshal(map[string]string{"ImportPath": pkg + " [" + pkg + ".test]", "Action": action, "Output": out})
		return string(b) + "\n"
	}
	stream := build("build-output", "# "+pkg+"\n") + build("build-output", "thing.go:3:1: undefined: Nope\n") +
		build("build-fail", "") + ev("start", pkg, "") + ev("output", pkg, "") + ev("fail", pkg, "")
	code, stdout, _ := newRig(t, stream, 1).run()
	if code != 1 {
		t.Errorf("exit %d, want 1", code)
	}
	for _, want := range []string{"FAIL " + pkg, "thing.go:3:1: undefined: Nope"} {
		if !strings.Contains(stdout, want) {
			t.Errorf("stdout lacks %q:\n%s", want, stdout)
		}
	}
}

func TestAPackageThatFailsWithNoFailingTestShowsItsOutput(t *testing.T) {
	t.Parallel()
	out, _ := json.Marshal(map[string]string{"Action": "output", "Package": pkg, "Output": "panic: boom in TestMain\n"})
	stream := string(out) + "\n" + ev("fail", pkg, "")
	_, stdout, _ := newRig(t, stream, 1).run()
	for _, want := range []string{"FAIL " + pkg, "panic: boom in TestMain"} {
		if !strings.Contains(stdout, want) {
			t.Errorf("stdout lacks %q:\n%s", want, stdout)
		}
	}
}

func TestAnExitWithNoFailureInTheStreamIsStillAFailureAndSaysSo(t *testing.T) {
	t.Parallel()
	r := newRig(t, "go: not a json line\n", 3)
	code, stdout, stderr := r.run()
	if code != 3 {
		t.Errorf("exit %d, want go's 3", code)
	}
	if !strings.Contains(stdout+stderr, "exited 3") || !strings.Contains(stdout+stderr, "names no failing test") {
		t.Errorf("no explanation: stdout %q stderr %q", stdout, stderr)
	}
}

func TestLinesThatAreNotJSONAreKeptAndDoNotStopTheCount(t *testing.T) {
	t.Parallel()
	stream := "go: downloading something\n" + passingStream()
	r := newRig(t, stream, 0)
	if code, stdout, _ := r.run(); code != 0 || !strings.Contains(stdout, "2 tests passed") {
		t.Errorf("exit %d, stdout %q", code, stdout)
	}
	got, _ := os.ReadFile(r.streams()[0])
	if string(got) != stream {
		t.Errorf("the kept stream lost the non-JSON line: %q", got)
	}
}

func TestAStreamWithNoFinalNewlineIsStillCounted(t *testing.T) {
	t.Parallel()
	r := newRig(t, strings.TrimSuffix(passingStream(), "\n"), 0)
	if code, stdout, _ := r.run(); code != 0 || !strings.Contains(stdout, "2 tests passed") {
		t.Errorf("exit %d, stdout %q", code, stdout)
	}
}

func TestGoStandardErrorIsPassedOn(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	if err := os.WriteFile(filepath.Join(r.fake.Dir(), "stderr"), []byte("go: some notice\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, _, stderr := r.run(); !strings.Contains(stderr, "go: some notice") {
		t.Errorf("stderr = %q", stderr)
	}
}

func TestNoGoOnPATHExitsTwoNamingIt(t *testing.T) {
	t.Parallel()
	env := testkit.Env(t, "PATH="+t.TempDir())
	var out, errb bytes.Buffer
	code := run(t.Context(), nil, testkit.Getenv(env), strings.NewReader(""), &out, &errb)
	if code != 2 || !strings.Contains(errb.String(), "go") || !strings.Contains(errb.String(), "PATH") {
		t.Errorf("exit %d, stderr %q", code, errb.String())
	}
}

func TestALogDirectoryThatCannotBeWrittenExitsTwoAndGoIsNotRun(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	blocker := filepath.Join(t.TempDir(), "file")
	if err := os.WriteFile(blocker, nil, 0o600); err != nil {
		t.Fatal(err)
	}
	r.env = testkit.WithPath(testkit.Env(t, "NEVR_TEST_LOG_DIR="+blocker), r.fake.Dir())
	code, _, stderr := r.run()
	if code != 2 || !strings.Contains(stderr, blocker) {
		t.Errorf("exit %d, stderr %q; want 2 naming %s", code, stderr, blocker)
	}
	if n := len(r.fake.Calls(t)); n != 0 {
		t.Errorf("go ran %d times with nowhere to keep its output", n)
	}
}

func TestTheLogDirectoryDefaultsToTheWorkRootsTestRunsTree(t *testing.T) {
	t.Parallel()
	f := testkit.Fake(t, "go")
	work := t.TempDir()
	env := testkit.WithPath(testkit.Env(t, "NEVR_WORK_ROOT="+work), f.Dir())
	var out, errb bytes.Buffer
	if code := run(t.Context(), nil, testkit.Getenv(env), strings.NewReader(""), &out, &errb); code != 0 {
		t.Fatalf("exit %d: %s", code, errb.String())
	}
	files, _ := filepath.Glob(filepath.Join(work, "test-runs", "tools-tests", "*-go.jsonl"))
	if len(files) != 1 {
		t.Errorf("streams under the work root's test-runs/tools-tests = %v", files)
	}
}

func TestAnUnwritableRunLogIsExitSeventyFourAndGoIsNotRun(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	blocker := filepath.Join(t.TempDir(), "file")
	if err := os.WriteFile(blocker, nil, 0o600); err != nil {
		t.Fatal(err)
	}
	r.env = testkit.WithPath(testkit.Env(t, "NEVR_TEST_LOG_DIR="+r.logDir, "XDG_STATE_HOME="+blocker), r.fake.Dir())
	code, _, stderr := r.run()
	if code != runlog.ExitLogFailure || !strings.Contains(stderr, "run log") {
		t.Errorf("exit %d, stderr %q", code, stderr)
	}
	if n := len(r.fake.Calls(t)); n != 0 {
		t.Errorf("go ran %d times", n)
	}
}

func TestTheRunIsLoggedToTheToolsRunLog(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	r.run()
	state := testkit.Getenv(r.env)("XDG_STATE_HOME")
	raw, err := os.ReadFile(filepath.Join(state, "nevr-runtime", "test-go-tools.jsonl"))
	if err != nil {
		t.Fatal(err)
	}
	var sawStart, sawEnd bool
	for _, line := range strings.Split(strings.TrimSpace(string(raw)), "\n") {
		var rec map[string]any
		if err := json.Unmarshal([]byte(line), &rec); err != nil {
			t.Fatalf("not JSON: %q", line)
		}
		switch rec["msg"] {
		case "start":
			sawStart = true
		case "end":
			sawEnd = true
			if rec["exit"] != float64(0) || rec["passed"] != float64(2) || rec["stream"] == "" {
				t.Errorf("end record = %v", rec)
			}
		}
	}
	if !sawStart || !sawEnd {
		t.Errorf("start %v, end %v in %s", sawStart, sawEnd, raw)
	}
}

func TestACancelledContextStopsGoAndIsReported(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	ctx, cancel := context.WithCancel(t.Context())
	cancel()
	var out, errb bytes.Buffer
	code := run(ctx, nil, testkit.Getenv(r.env), strings.NewReader(""), &out, &errb)
	if code == 0 {
		t.Errorf("a run whose context was cancelled before go started exited 0 (stderr %q)", errb.String())
	}
}
