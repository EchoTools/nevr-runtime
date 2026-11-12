package main

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"testing"
	"time"
	"unsafe"

	"github.com/EchoTools/nevr-runtime/tools/internal/runlog"
	"github.com/EchoTools/nevr-runtime/tools/internal/testkit"
)

// The fake `go`: prints the file "stdout" beside it to standard output and "stderr" to standard error, and exits with
// the number in "exit" (0 when there is none). A test writes those files before it runs the tool.
func scripted(c *testkit.Call) int {
	if raw, err := os.ReadFile(filepath.Join(c.FakeDir, "probe-lock")); err == nil {
		// Notes whether the lock file named in "probe-lock" is held by someone else while this half runs.
		seen := "free\n"
		if f, err := os.OpenFile(string(raw), os.O_RDWR, 0); err == nil {
			if syscall.Flock(int(f.Fd()), syscall.LOCK_EX|syscall.LOCK_NB) != nil {
				seen = "held\n"
			}
			f.Close()
		}
		if out, err := os.OpenFile(filepath.Join(c.FakeDir, "lock-seen"), os.O_WRONLY|os.O_CREATE|os.O_APPEND, 0o644); err == nil {
			out.WriteString(seen)
			out.Close()
		}
	}
	if _, err := os.Stat(filepath.Join(c.FakeDir, "read-stdin")); err == nil {
		// Reads its standard input to the end: on a terminal that nobody types on, that never ends.
		data, _ := io.ReadAll(c.Stdin)
		fmt.Fprintf(c.Stdout, "stdin-ended after %d bytes\n", len(data))
	}
	if _, err := os.Stat(filepath.Join(c.FakeDir, "leave-process")); err == nil {
		// Exit 0 leaving a child that holds our standard output and error open.
		child := exec.Command("sleep", "60")
		child.Stdout, child.Stderr = c.Stdout, c.Stderr
		if err := child.Start(); err != nil {
			return 99
		}
		os.WriteFile(filepath.Join(c.FakeDir, "child.pid"), []byte(strconv.Itoa(child.Process.Pid)), 0o644)
		fmt.Fprintln(c.Stdout, "py done")
		return 0
	}
	if _, err := os.Stat(filepath.Join(c.FakeDir, "ignore-sigint")); err == nil {
		signal.Ignore(syscall.SIGINT)
		never := make(chan os.Signal, 1)
		signal.Notify(never, syscall.SIGUSR2) // a signal handler keeps the runtime from calling this a deadlock
		os.WriteFile(filepath.Join(c.FakeDir, "ready"), nil, 0o644)
		<-never // until killed
	}
	if _, err := os.Stat(filepath.Join(c.FakeDir, "die-by-signal")); err == nil {
		syscall.Kill(os.Getpid(), syscall.SIGKILL)
	}
	if _, err := os.Stat(filepath.Join(c.FakeDir, "wait-sigint")); err == nil {
		ch := make(chan os.Signal, 1)
		signal.Notify(ch, syscall.SIGINT)
		os.WriteFile(filepath.Join(c.FakeDir, "ready"), nil, 0o644)
		<-ch
		return 130 // what the Python runner does after an interrupt
	}
	if _, err := os.Stat(filepath.Join(c.FakeDir, "echo-stdin")); err == nil {
		io.Copy(c.Stdout, c.Stdin)
	}
	if out, err := os.ReadFile(filepath.Join(c.FakeDir, "stdout")); err == nil {
		c.Stdout.Write(out)
	}
	if out, err := os.ReadFile(filepath.Join(c.FakeDir, "stderr")); err == nil {
		c.Stderr.Write(out)
	}
	if raw, err := os.ReadFile(filepath.Join(c.FakeDir, "exit")); err == nil {
		if strings.TrimSpace(string(raw)) == "kill" {
			syscall.Kill(os.Getpid(), syscall.SIGKILL)
		}
		n, _ := strconv.Atoi(strings.TrimSpace(string(raw)))
		return n
	}
	return 0
}

// The same script plays go and python3 (the two halves); each has its own directory with its own files.
var fakes = testkit.Fakes{"go": scripted, "python3": scripted}

func TestMain(m *testing.M) { testkit.Main(m, fakes) }

// rig is one run of the tool: a fake go that behaves as the test scripted, a log directory, an environment.
type rig struct {
	t      *testing.T
	fake   *testkit.FakeCommand
	py     *testkit.FakeCommand
	logDir string
	env    []string
}

func newRig(t *testing.T, stdout string, exit int) *rig {
	return newRigExit(t, stdout, strconv.Itoa(exit))
}

func newRigExit(t *testing.T, stdout, exit string) *rig {
	t.Helper()
	f := testkit.Fake(t, "go", "DISPLAY", "GNUPGHOME", "GH_TOKEN", "GIT_CONFIG_GLOBAL", "HOME", "GOCACHE", "GIT_CONFIG_PARAMETERS", "GOFLAGS")
	py := testkit.Fake(t, "python3")
	r := &rig{t: t, fake: f, py: py}
	r.script(f, "stdout", stdout)
	r.script(f, "exit", exit)
	r.script(py, "exit", "0")
	r.logDir = filepath.Join(t.TempDir(), "runs")
	r.env = testkit.WithPath(testkit.Env(t, "NEVR_TEST_LOG_DIR="+r.logDir), f.Dir(), py.Dir())
	return r
}

func (r *rig) script(f *testkit.FakeCommand, name, content string) {
	r.t.Helper()
	if err := os.WriteFile(filepath.Join(f.Dir(), name), []byte(content), 0o644); err != nil {
		r.t.Fatal(err)
	}
}

func (r *rig) run(args ...string) (code int, stdout, stderr string) {
	r.t.Helper()
	return r.runWith(defaultOptions, args...)
}

func (r *rig) runWith(opts options, args ...string) (code int, stdout, stderr string) {
	r.t.Helper()
	var out, errb bytes.Buffer
	code = runWith(opts, r.t.Context(), args, testkit.Getenv(r.env), strings.NewReader(""), &out, &errb)
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
	if len(calls) != 1 || strings.Join(calls[0].Args, " ") != "test -count=1 -json -timeout 5m0s ./..." {
		t.Errorf("go was called as %+v, want: go test -count=1 -json ./...", calls)
	}
}

func TestPackagePatternsGivenOnTheCommandLineReplaceTheDefault(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	r.run("./internal/testkit/", "./symcache/")
	calls := r.fake.Calls(t)
	if len(calls) != 1 || strings.Join(calls[0].Args, " ") != "test -count=1 -json -timeout 5m0s ./internal/testkit/ ./symcache/" {
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
	// The last line is a test's pass, so a line lost for want of a newline loses a count.
	r := newRig(t, strings.TrimSuffix(ev("run", pkg, "TestA")+ev("pass", pkg, "TestA")+ev("run", pkg, "TestB")+ev("pass", pkg, "TestB"), "\n"), 0)
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
		switch rec["event"] {
		case "go half":
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

func TestAGoKilledBySignalIsAFailureWithAnExitCode(t *testing.T) {
	t.Parallel()
	r := newRigExit(t, passingStream(), "kill")
	code, stdout, _ := r.run()
	if code != 137 {
		t.Errorf("exit %d, want 128 + SIGKILL (a negative exit status is not a usable code)", code)
	}
	if !strings.Contains(stdout, "go FAIL") || !strings.Contains(stdout, "go half exit 137") {
		t.Errorf("stdout does not report the failure: %q", stdout)
	}
}

func TestWithNoLogDirOrWorkRootTheDefaultIsUnderHome(t *testing.T) {
	t.Parallel()
	f := testkit.Fake(t, "go")
	env := testkit.WithPath(testkit.Env(t, "NEVR_WORK_ROOT="), f.Dir())
	home := testkit.Getenv(env)("HOME")
	var out, errb bytes.Buffer
	if code := run(t.Context(), nil, testkit.Getenv(env), strings.NewReader(""), &out, &errb); code != 0 {
		t.Fatalf("exit %d: %s", code, errb.String())
	}
	files, _ := filepath.Glob(filepath.Join(home, "src", "nevr-runtime-work", "test-runs", "tools-tests", "*-go.jsonl"))
	if len(files) != 1 {
		t.Errorf("streams under $HOME/src/nevr-runtime-work/test-runs/tools-tests = %v", files)
	}
}

// ---- the two halves (B1): the Python half first, the Go half whatever it did ----

func (r *rig) both(pyExit string, pyOut string, patterns ...string) (int, string, string) {
	r.t.Helper()
	r.script(r.py, "exit", pyExit)
	r.script(r.py, "stdout", pyOut)
	return r.run(append(append([]string{}, patterns...), "--", "python3", "tools/run_tool_tests.py", "-q")...)
}

func TestTheGoHalfRunsWhenThePythonHalfFailsAndBothAreNamed(t *testing.T) {
	t.Parallel()
	r := newRig(t, ev("run", pkg, "TestB")+ev("fail", pkg, "TestB")+ev("fail", pkg, ""), 1)
	code, stdout, _ := r.both("1", "FAILED tools/tests/test_x.py::T::test_y\n")
	if code == 0 {
		t.Errorf("exit 0 with both halves red")
	}
	if n := len(r.fake.Calls(t)); n != 1 {
		t.Errorf("go ran %d times, want once even though the Python half failed", n)
	}
	for _, want := range []string{"FAILED tools/tests/test_x.py::T::test_y", "go FAIL " + pkg + " TestB", "FAIL: python half exit 1, go half exit 1"} {
		if !strings.Contains(stdout, want) {
			t.Errorf("stdout lacks %q:\n%s", want, stdout)
		}
	}
}

func TestOnlyThePythonHalfFailingFailsTheRunAndNamesOnlyIt(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	code, stdout, _ := r.both("3", "")
	if code != 3 {
		t.Errorf("exit %d, want the Python half's 3", code)
	}
	if !strings.Contains(stdout, "FAIL: python half exit 3\n") || strings.Contains(stdout, "go half") {
		t.Errorf("summary:\n%s", stdout)
	}
	if len(r.fake.Calls(t)) != 1 || len(r.streams()) != 1 {
		t.Error("the Go half did not run and keep its stream")
	}
}

func TestOnlyTheGoHalfFailingFailsTheRunAndNamesOnlyIt(t *testing.T) {
	t.Parallel()
	r := newRig(t, ev("fail", pkg, "TestB")+ev("fail", pkg, ""), 1)
	code, stdout, _ := r.both("0", "")
	if code != 1 {
		t.Errorf("exit %d, want go's 1", code)
	}
	if !strings.Contains(stdout, "FAIL: go half exit 1\n") || strings.Contains(stdout, "python half") {
		t.Errorf("summary:\n%s", stdout)
	}
}

func TestBothHalvesPassingExitsZeroWithNoFailLine(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	code, stdout, _ := r.both("0", "py output line\n")
	if code != 0 || strings.Contains(stdout, "FAIL") || !strings.Contains(stdout, "py output line") {
		t.Errorf("exit %d, stdout:\n%s", code, stdout)
	}
}

// Only a signal the tool itself received is an interrupt. A Python half that was killed or exits 130 on its own (an OOM
// kill, timeout -k, a signal sent to the runner alone) still gets the Go half, and both are named.
func TestAPythonHalfEndedBySomethingElseStillGetsTheGoHalf(t *testing.T) {
	t.Parallel()
	for _, rc := range []string{"130", "143", "129", "137"} {
		r := newRig(t, passingStream(), 0)
		code, stdout, _ := r.both(rc, "")
		if n, _ := strconv.Atoi(rc); code != n {
			t.Errorf("rc %s: exit %d", rc, code)
		}
		if n := len(r.fake.Calls(t)); n != 1 {
			t.Errorf("rc %s: go ran %d times, want once", rc, n)
		}
		mustHave(t, stdout, "FAIL: python half exit "+rc)
		if strings.Contains(stdout, "interrupted") {
			t.Errorf("rc %s: called an interrupt although no signal reached the tool:\n%s", rc, stdout)
		}
	}
}

func TestAPythonHalfKilledBySIGKILLReportsExit137AndGoStillRuns(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	r.script(r.py, "die-by-signal", "")
	code, stdout, _ := r.run("--", "python3")
	if code != 137 || len(r.fake.Calls(t)) != 1 {
		t.Errorf("exit %d, go calls %d", code, len(r.fake.Calls(t)))
	}
	mustHave(t, stdout, "FAIL: python half exit 137")
}

func TestAPythonHalfThatCannotStartFailsAndTheGoHalfStillRuns(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	code, stdout, stderr := r.run("--", "tk-no-such-command-anywhere")
	if code != 127 || len(r.fake.Calls(t)) != 1 {
		t.Errorf("exit %d, go calls %d", code, len(r.fake.Calls(t)))
	}
	mustHave(t, stderr, "tk-no-such-command-anywhere")
	mustHave(t, stdout, "FAIL: python half exit 127")
}

func mustHave(t *testing.T, got string, wants ...string) {
	t.Helper()
	for _, want := range wants {
		if !strings.Contains(got, want) {
			t.Errorf("output lacks %q:\n%s", want, got)
		}
	}
}

// ---- the Go stream is named like the Python run's log, and the half is recorded in runs.jsonl (B3) ----

func (r *rig) records() []map[string]any {
	r.t.Helper()
	raw, err := os.ReadFile(filepath.Join(r.logDir, "runs.jsonl"))
	if err != nil {
		r.t.Fatal(err)
	}
	var out []map[string]any
	for _, line := range strings.Split(strings.TrimSpace(string(raw)), "\n") {
		var m map[string]any
		if err := json.Unmarshal([]byte(line), &m); err != nil {
			r.t.Fatalf("runs.jsonl line %q: %v", line, err)
		}
		out = append(out, m)
	}
	return out
}

func TestTheStreamTakesThePythonRunsStemAndRunsJSONLRecordsTheGoHalf(t *testing.T) {
	t.Parallel()
	const stem = "20261105T120000.123456Z-4242-0123456789abcdef-aabbccddeeff"
	r := newRig(t, ev("fail", pkg, "TestB")+ev("fail", pkg, ""), 1)
	r.pythonLog(stem)
	code, stdout, _ := r.both("1", "noise\ntest-tools: log "+filepath.Join(r.logDir, stem+".log")+"\n")
	if code == 0 {
		t.Fatal("exit 0")
	}
	if got := r.streams(); len(got) != 1 || filepath.Base(got[0]) != stem+"-go.jsonl" {
		t.Fatalf("streams = %v, want %s-go.jsonl", got, stem)
	}
	recs := r.records()
	if len(recs) != 2 || recs[0]["event"] != "go_start" || recs[1]["event"] != "go_end" {
		t.Fatalf("records = %v, want go_start then go_end", recs)
	}
	end := recs[1]
	if end["stem"] != stem || end["rc"] != float64(1) || end["python_rc"] != float64(1) || end["stream"] == "" || end["ts"] == nil {
		t.Errorf("go_end = %v", end)
	}
	if failed, _ := end["failed"].([]any); len(failed) != 1 || failed[0] != pkg+" TestB" {
		t.Errorf("go_end failed = %v", end["failed"])
	}
	if recs[0]["stem"] != stem {
		t.Errorf("go_start = %v", recs[0])
	}
	mustHave(t, stdout, stem+"-go.jsonl")
}

func TestWithoutAPythonLogLineTheStreamHasItsOwnStemAndIsStillRecorded(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	r.both("0", "no log line here\n")
	files := r.streams()
	if len(files) != 1 {
		t.Fatal(files)
	}
	recs := r.records()
	if recs[1]["stem"] != strings.TrimSuffix(filepath.Base(files[0]), "-go.jsonl") || recs[1]["paired"] != false {
		t.Errorf("go_end = %v", recs[1])
	}
}

func TestAGoOnlyRunIsRecordedToo(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	r.run()
	recs := r.records()
	if len(recs) != 2 || recs[1]["event"] != "go_end" || recs[1]["python_rc"] != nil {
		t.Errorf("records = %v", recs)
	}
}

func TestARecordThatCannotBeWrittenFailsTheRunEvenWhenEveryTestPassed(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	if err := os.MkdirAll(filepath.Join(r.logDir, "runs.jsonl"), 0o755); err != nil { // a directory where the file goes
		t.Fatal(err)
	}
	code, _, stderr := r.run()
	if code != 2 || !strings.Contains(stderr, "runs.jsonl") {
		t.Errorf("exit %d, stderr %q", code, stderr)
	}
}

// ---- go test is started with a hermetic environment, a timeout and the caches it needs (N1, N2, N6) ----

func TestGoTestGetsATimeoutSoAHungTestEndsTheRunWithAName(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	r.run()
	calls := r.fake.Calls(t)
	if len(calls) != 1 || strings.Join(calls[0].Args, " ") != "test -count=1 -json -timeout 5m0s ./..." {
		t.Errorf("go was called as %+v", calls)
	}
}

func TestGoTestDoesNotSeeTheCallersCredentialsDisplayOrGitConfigButKeepsItsGoCache(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	// The caller's environment, as a person's shell has it.
	r.env = append(r.env, "GH_TOKEN=secret-token", "DISPLAY=:0", "GNUPGHOME=/nonexistent/gnupg",
		"GIT_CONFIG_PARAMETERS='user.signingkey=owner'", "GOCACHE=/caller/gocache", "HOME=/caller/home")
	r.run()
	calls := r.fake.Calls(t)
	if len(calls) != 1 {
		t.Fatalf("calls = %+v", calls)
	}
	env := calls[0].Env
	for _, key := range []string{"GH_TOKEN", "DISPLAY", "GNUPGHOME", "GIT_CONFIG_PARAMETERS"} {
		if v, ok := env[key]; ok {
			t.Errorf("go test saw %s=%s", key, v)
		}
	}
	if env["GIT_CONFIG_GLOBAL"] != "/dev/null" {
		t.Errorf("GIT_CONFIG_GLOBAL = %q", env["GIT_CONFIG_GLOBAL"])
	}
	if env["HOME"] == "" || env["HOME"] == "/caller/home" {
		t.Errorf("HOME = %q, want a directory of its own, not the caller's", env["HOME"])
	}
	if env["GOCACHE"] != "/caller/gocache" {
		t.Errorf("GOCACHE = %q, want the caller's", env["GOCACHE"])
	}
}

func TestGoTestGetsTheCallersPathFromTheGetenvItWasGivenAndGOFLAGSCount1(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	r.run()
	if got := r.fake.Calls(t)[0].Env["GOFLAGS"]; got != "-count=1" {
		t.Errorf("GOFLAGS = %q", got)
	}
}

func TestTheScratchHomeOfGoTestIsRemovedWhenTheRunEnds(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	r.run()
	home := r.fake.Calls(t)[0].Env["HOME"]
	if _, err := os.Stat(home); !os.IsNotExist(err) {
		t.Errorf("%s still exists after the run (%v)", home, err)
	}
}

// ---- skipped tests are reported (N4) ----

func TestASkippedTestIsNamedWithItsReasonInTheOutputAndTheRecord(t *testing.T) {
	t.Parallel()
	out := func(test, text string) string {
		b, _ := json.Marshal(map[string]string{"Action": "output", "Package": pkg, "Test": test, "Output": text})
		return string(b) + "\n"
	}
	stream := ev("run", pkg, "TestOnlyShrinks") + out("TestOnlyShrinks", "=== RUN   TestOnlyShrinks\n") +
		out("TestOnlyShrinks", "    policy_test.go:117: there is no origin/main in this clone\n") +
		out("TestOnlyShrinks", "--- SKIP: TestOnlyShrinks (0.00s)\n") + ev("skip", pkg, "TestOnlyShrinks") +
		ev("run", pkg, "TestB") + ev("pass", pkg, "TestB") + ev("pass", pkg, "")
	r := newRig(t, stream, 0)
	code, stdout, _ := r.run()
	if code != 0 {
		t.Errorf("exit %d: a skip is not a failure", code)
	}
	mustHave(t, stdout, "go SKIP "+pkg+" TestOnlyShrinks: policy_test.go:117: there is no origin/main in this clone", "1 skipped")
	end := r.records()[1]
	if sk, _ := end["skipped"].([]any); len(sk) != 1 || !strings.Contains(sk[0].(string), "TestOnlyShrinks") {
		t.Errorf("go_end skipped = %v", end["skipped"])
	}
}

func TestTheCallersStandardInputReachesThePythonHalf(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	r.script(r.py, "echo-stdin", "")
	var out, errb bytes.Buffer
	code := run(t.Context(), []string{"--", "python3"}, testkit.Getenv(r.env), strings.NewReader("typed input\n"), &out, &errb)
	if code != 0 || !strings.Contains(out.String(), "typed input") {
		t.Errorf("exit %d, stdout %q", code, out.String())
	}
}

func TestACancelledContextEndsTheRunBeforeTheGoHalfWhenThereIsAPythonHalf(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	ctx, cancel := context.WithCancel(t.Context())
	cancel()
	var out, errb bytes.Buffer
	code := run(ctx, []string{"--", "python3"}, testkit.Getenv(r.env), strings.NewReader(""), &out, &errb)
	if code != 130 || len(r.fake.Calls(t)) != 0 || !strings.Contains(out.String(), "go half not run") {
		t.Errorf("exit %d, go calls %d, stdout %q", code, len(r.fake.Calls(t)), out.String())
	}
}

func TestNothingAfterTheSeparatorIsAUsageError(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	code, _, stderr := r.run("--")
	if code != 2 || !strings.Contains(stderr, "usage") || len(r.fake.Calls(t)) != 0 {
		t.Errorf("exit %d, stderr %q", code, stderr)
	}
}

func TestWhenBothHalvesFailTheExitCodeIsThePythonHalfs(t *testing.T) {
	t.Parallel()
	r := newRig(t, ev("fail", pkg, "TestB")+ev("fail", pkg, ""), 1)
	if code, _, _ := r.both("3", ""); code != 3 {
		t.Errorf("exit %d, want the Python half's 3", code)
	}
}

func TestASkipWithNoOutputIsStillNamedWithAReason(t *testing.T) {
	t.Parallel()
	r := newRig(t, ev("skip", pkg, "TestQuiet")+ev("pass", pkg, ""), 0)
	_, stdout, _ := r.run()
	mustHave(t, stdout, "go SKIP "+pkg+" TestQuiet: no reason given")
}

func TestASkipReasonIsNeitherGosRunLineNorItsSkipLine(t *testing.T) {
	t.Parallel()
	out := func(text string) string {
		b, _ := json.Marshal(map[string]string{"Action": "output", "Package": pkg, "Test": "TestS", "Output": text})
		return string(b) + "\n"
	}
	stream := out("    x_test.go:9: the real reason\n") + out("=== NAME  TestS\n") + out("--- SKIP: TestS (0.00s)\n") + out("\n") + ev("skip", pkg, "TestS") + ev("pass", pkg, "")
	_, stdout, _ := newRig(t, stream, 0).run()
	mustHave(t, stdout, "go SKIP "+pkg+" TestS: x_test.go:9: the real reason")
}

// Cancelling the run interrupts the Python runner with SIGINT, which it turns into its own clean end (a kill would
// leave its record unfinished).
func TestCancellingTheRunInterruptsThePythonHalfWithSIGINT(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	r.script(r.py, "wait-sigint", "")
	ctx, cancel := context.WithCancel(t.Context())
	defer cancel()
	done := make(chan int, 1)
	var out, errb bytes.Buffer
	go func() {
		done <- run(ctx, []string{"--", "python3"}, testkit.Getenv(r.env), strings.NewReader(""), &out, &errb)
	}()
	testkit.WaitFor(t, "the Python fake to be waiting for SIGINT", func() bool {
		_, err := os.Stat(filepath.Join(r.py.Dir(), "ready"))
		return err == nil
	})
	cancel()
	select {
	case code := <-done:
		if code != 130 || len(r.fake.Calls(t)) != 0 || !strings.Contains(out.String(), "python half exited 130 (interrupted)") {
			t.Errorf("exit %d (want 130: SIGINT, not a kill's 137), go calls %d, stdout %q", code, len(r.fake.Calls(t)), out.String())
		}
	case <-t.Context().Done():
		t.Fatal("the run did not end after the cancel")
	}
}

// The test's own output is kept to its last lines, so a test that prints a lot costs no memory.
func TestOnlyTheLastLinesOfATestsOutputAreKeptForItsSkipReason(t *testing.T) {
	t.Parallel()
	out := func(text string) string {
		b, _ := json.Marshal(map[string]string{"Action": "output", "Package": pkg, "Test": "TestLoud", "Output": text})
		return string(b) + "\n"
	}
	stream := out("    old_test.go:1: an old reason\n")
	for range 8 {
		stream += out("--- noise\n")
	}
	stream += ev("skip", pkg, "TestLoud") + ev("pass", pkg, "")
	_, stdout, _ := newRig(t, stream, 0).run()
	mustHave(t, stdout, "go SKIP "+pkg+" TestLoud: no reason given")
}

func TestThePythonHalfsStandardErrorIsPassedOn(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	r.script(r.py, "stderr", "py: a warning\n")
	r.script(r.py, "exit", "0")
	if _, _, stderr := r.run("--", "python3"); !strings.Contains(stderr, "py: a warning") {
		t.Errorf("stderr = %q", stderr)
	}
}

// A stream is never overwritten: a second run with the same stem is an error, not a lost record.
func TestAStreamThatAlreadyExistsIsNotOverwritten(t *testing.T) {
	t.Parallel()
	const stem = "20261105T120000.123456Z-4242-0123456789abcdef-aabbccddeeff"
	r := newRig(t, passingStream(), 0)
	if err := os.MkdirAll(r.logDir, 0o755); err != nil {
		t.Fatal(err)
	}
	existing := filepath.Join(r.logDir, stem+"-go.jsonl")
	if err := os.WriteFile(existing, []byte("the earlier run\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	r.pythonLog(stem)
	code, _, stderr := r.both("0", "test-tools: log "+filepath.Join(r.logDir, stem+".log")+"\n")
	if code != 2 || !strings.Contains(stderr, stem) {
		t.Errorf("exit %d, stderr %q", code, stderr)
	}
	if got, _ := os.ReadFile(existing); string(got) != "the earlier run\n" {
		t.Errorf("the earlier stream was overwritten: %q", got)
	}
	if n := len(r.fake.Calls(t)); n != 0 {
		t.Errorf("go ran %d times with nowhere to keep its stream", n)
	}
}

// The module is not at the repository root: go test must run inside tools/, which the recipes name with -C, as go does.
func TestMinusCRunsGoTestInThatDirectory(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	mod := t.TempDir()
	code, _, stderr := r.run("-C", mod, "./internal/...")
	if code != 0 {
		t.Fatalf("exit %d: %s", code, stderr)
	}
	calls := r.fake.Calls(t)
	if len(calls) != 1 || calls[0].Dir != mod || strings.Join(calls[0].Args, " ") != "test -count=1 -json -timeout 5m0s ./internal/..." {
		t.Errorf("go call = %+v, want it run in %s with the pattern after -C's argument", calls, mod)
	}
}

func TestMinusCNeedsADirectory(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	code, _, stderr := r.run("-C")
	if code != 2 || !strings.Contains(stderr, "-C") || len(r.fake.Calls(t)) != 0 {
		t.Errorf("exit %d, stderr %q", code, stderr)
	}
}

func TestMinusCDoesNotMoveThePythonHalf(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	mod := t.TempDir()
	r.both("0", "", "-C", mod)
	here, _ := os.Getwd()
	if calls := r.py.Calls(t); len(calls) != 1 || calls[0].Dir != here {
		t.Errorf("python call %+v, want it run where the recipe runs (%s), not in %s", calls, here, mod)
	}
}

// pythonLog makes the file the Python runner would have written, in the log directory.
func (r *rig) pythonLog(stem string) {
	r.t.Helper()
	if err := os.MkdirAll(r.logDir, 0o755); err != nil {
		r.t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(r.logDir, stem+".log"), []byte("log\n"), 0o644); err != nil {
		r.t.Fatal(err)
	}
}

// The log line is trusted only when it names a file of the log directory: a line a test printed (the captured output of a
// nested runner) must not name the stream.
func TestALogLineThatDoesNotNameARealFileInTheLogDirectoryIsIgnored(t *testing.T) {
	t.Parallel()
	const stem = "20261105T120000.123456Z-4242-0123456789abcdef-aabbccddeeff"
	elsewhere := t.TempDir()
	if err := os.WriteFile(filepath.Join(elsewhere, stem+".log"), nil, 0o644); err != nil {
		t.Fatal(err)
	}
	for name, line := range map[string]string{
		"another directory":  "test-tools: log " + filepath.Join(elsewhere, stem+".log"),
		"a missing file":     "test-tools: log " + filepath.Join("LOGDIR", "missing.log"),
		"a nested directory": "test-tools: log " + filepath.Join("LOGDIR", "sub", stem+".log"),
		"not a .log":         "test-tools: log " + filepath.Join("LOGDIR", stem+".txt"),
		"a parent step":      "test-tools: log " + filepath.Join("LOGDIR", "..", stem+".log"),
		"a directory":        "test-tools: log " + filepath.Join("LOGDIR", "adir.log"),
	} {
		r := newRig(t, passingStream(), 0)
		r.pythonLog(stem)
		os.WriteFile(filepath.Join(r.logDir, stem+".txt"), nil, 0o644)
		os.MkdirAll(filepath.Join(r.logDir, "sub"), 0o755)
		os.MkdirAll(filepath.Join(r.logDir, "adir.log"), 0o755)
		os.WriteFile(filepath.Join(r.logDir, "sub", stem+".log"), nil, 0o644)
		os.WriteFile(filepath.Join(filepath.Dir(r.logDir), stem+".log"), nil, 0o644)
		code, _, stderr := r.both("0", strings.ReplaceAll(line, "LOGDIR", r.logDir)+"\n")
		if code != 0 {
			t.Errorf("%s: exit %d", name, code)
		}
		files := r.streams()
		if len(files) != 1 || strings.Contains(filepath.Base(files[0]), stem) {
			t.Errorf("%s: streams = %v, want one with a stem of its own", name, files)
		}
		mustHave(t, stderr, "ignoring")
		if recs := r.records(); recs[1]["paired"] != false {
			t.Errorf("%s: paired = %v", name, recs[1]["paired"])
		}
	}
}

// ---- a Python half that exits 0 and leaves a process holding its output (R2-B3) ----

func TestAPythonHalfThatLeavesAProcessHoldingItsOutputKeepsItsRealExitStatusAndNamesTheProcess(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	r.script(r.py, "leave-process", "")
	t.Cleanup(func() { // the leftover is the test's to remove
		if raw, err := os.ReadFile(filepath.Join(r.py.Dir(), "child.pid")); err == nil {
			if pid, err := strconv.Atoi(string(raw)); err == nil {
				syscall.Kill(pid, syscall.SIGKILL)
			}
		}
	})
	opts := defaultOptions
	opts.leftoverDelay = 300 * time.Millisecond
	start := time.Now()
	code, stdout, stderr := r.runWith(opts, "--", "python3")
	if code != 0 {
		t.Errorf("exit %d, want the Python half's real status 0 (stderr %q)", code, stderr)
	}
	if len(r.fake.Calls(t)) != 1 {
		t.Error("the Go half did not run")
	}
	if strings.Contains(stderr, "cannot start") || strings.Contains(stdout, "FAIL") {
		t.Errorf("a finished Python half was reported as failed:\nstdout %s\nstderr %s", stdout, stderr)
	}
	raw, _ := os.ReadFile(filepath.Join(r.py.Dir(), "child.pid"))
	// The line names the process by its command line, not only by its short name ("sleep").
	mustHave(t, stderr, "still holds its output", "pid "+string(raw)+" sleep 60")
	if d := time.Since(start); d > 20*time.Second {
		t.Errorf("took %v", d)
	}
	if left, _ := r.records()[1]["leftover"].([]any); len(left) != 1 || !strings.Contains(left[0].(string), "sleep 60") {
		t.Errorf("go_end leftover = %v", r.records()[1]["leftover"])
	}
}

// ---- cancelling during the Go half interrupts go, not kills it ----

func TestCancellingDuringTheGoHalfInterruptsGoWithSIGINT(t *testing.T) {
	t.Parallel()
	r := newRig(t, "", 0)
	r.script(r.fake, "wait-sigint", "")
	ctx, cancel := context.WithCancel(t.Context())
	defer cancel()
	done := make(chan int, 1)
	var out, errb bytes.Buffer
	go func() {
		done <- runWith(defaultOptions, ctx, nil, testkit.Getenv(r.env), strings.NewReader(""), &out, &errb)
	}()
	testkit.WaitFor(t, "the go fake to wait for SIGINT", func() bool {
		_, err := os.Stat(filepath.Join(r.fake.Dir(), "ready"))
		return err == nil
	})
	cancel()
	select {
	case code := <-done:
		if code != 130 || !strings.Contains(out.String(), "go half exit 130") {
			t.Errorf("exit %d (want 130: SIGINT, not a kill's 137), stdout %q", code, out.String())
		}
	case <-t.Context().Done():
		t.Fatal("the run did not end after the cancel")
	}
}

// The runner's own interrupt path (its grace, then its drains) is longer than the leftover delay; a kill comes only after
// the interrupt grace.
func TestTheInterruptGraceIsLongerThanTheRunnersOwnInterruptPath(t *testing.T) {
	t.Parallel()
	const runnerGrace, runnerDrains = 30 * time.Second, 2 * 10 * time.Second // NEVR_TEST_INTERRUPT_GRACE_S and NEVR_TEST_DRAIN_S
	if defaultOptions.interruptGrace <= runnerGrace+runnerDrains {
		t.Errorf("interruptGrace %v does not outlast the runner's %v", defaultOptions.interruptGrace, runnerGrace+runnerDrains)
	}
}

func TestAPythonHalfStillRunningAfterTheInterruptGraceIsKilled(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	r.script(r.py, "ignore-sigint", "")
	opts := defaultOptions
	opts.interruptGrace = 300 * time.Millisecond
	ctx, cancel := context.WithCancel(t.Context())
	defer cancel()
	done := make(chan int, 1)
	var out, errb bytes.Buffer
	go func() {
		done <- runWith(opts, ctx, []string{"--", "python3"}, testkit.Getenv(r.env), strings.NewReader(""), &out, &errb)
	}()
	testkit.WaitFor(t, "the python fake to be running", func() bool {
		_, err := os.Stat(filepath.Join(r.py.Dir(), "ready"))
		return err == nil
	})
	cancel()
	select {
	case code := <-done:
		if code != 137 {
			t.Errorf("exit %d, want 137 (killed after the grace)", code)
		}
	case <-t.Context().Done():
		t.Fatal("the run did not end")
	}
	requireNoFakeLeft(t, r.py.Dir())
}

// leftoverFakeProcesses lists the live processes whose argv[0] is under dir, a fake's own directory: the processes a fake
// started through its path, which keep the worktree busy after the test ends (remove-worktree refuses a process whose cwd is inside).
func leftoverFakeProcesses(dir string) []string {
	entries, err := os.ReadDir("/proc")
	if err != nil {
		return nil
	}
	var out []string
	for _, e := range entries {
		if _, err := strconv.Atoi(e.Name()); err != nil {
			continue
		}
		raw, err := os.ReadFile("/proc/" + e.Name() + "/cmdline")
		if err != nil || len(raw) == 0 {
			continue
		}
		argv := strings.Split(strings.TrimSuffix(string(raw), "\x00"), "\x00")
		if strings.HasPrefix(argv[0], dir+string(filepath.Separator)) {
			out = append(out, e.Name()+" "+strings.Join(argv, " "))
		}
	}
	return out
}

// requireNoFakeLeft waits up to three seconds for no process of the fake in dir to be alive. A process still alive then is
// a failure, and it is killed, so the leak does not outlive the test.
func requireNoFakeLeft(t *testing.T, dir string) {
	t.Helper()
	deadline := time.Now().Add(3 * time.Second)
	for {
		left := leftoverFakeProcesses(dir)
		if len(left) == 0 {
			return
		}
		if time.Now().After(deadline) {
			t.Errorf("processes started by the fake in %s are still alive after the run: %q", dir, left)
			for _, l := range left {
				if pid, err := strconv.Atoi(strings.Fields(l)[0]); err == nil {
					syscall.Kill(pid, syscall.SIGKILL)
				}
			}
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
}

// TestToolChildProcess is not a test of its own: TestKilledToolLeavesNoFakeRunning runs this binary with
// NEVR_TGT_TOOL_CHILD=1, so the tool runs in a process that the test can kill without its cleanup.
func TestToolChildProcess(t *testing.T) {
	if os.Getenv("NEVR_TGT_TOOL_CHILD") != "1" {
		t.Skip("runs only as the tool's child process")
	}
	var args []string
	if os.Getenv("NEVR_TGT_TOOL_PY") == "1" {
		args = []string{"--", "python3"}
	}
	runWith(defaultOptions, context.Background(), args, os.Getenv, strings.NewReader(""), io.Discard, io.Discard)
}

// A tool killed while its Go half runs (SIGKILL: no cleanup, no grace) must not leave that half running. The fake go
// blocks until it is killed, so it is alive the whole time the test waits for it.
func TestKilledToolLeavesNoFakeRunning(t *testing.T) {
	t.Parallel()
	r := newRig(t, "", 0)
	r.script(r.fake, "ignore-sigint", "")
	killedToolLeavesNoFake(t, r, r.fake, nil)
}

// The same for the Python half: the tool that started it is killed, and the fake python3 must not outlive it.
func TestKilledToolLeavesNoPythonRunning(t *testing.T) {
	t.Parallel()
	r := newRig(t, "", 0)
	r.script(r.py, "ignore-sigint", "")
	killedToolLeavesNoFake(t, r, r.py, []string{"NEVR_TGT_TOOL_PY=1"})
}

// killedToolLeavesNoFake runs the tool in a child of this test binary, waits for the fake to be running (its ready file),
// kills the child, and requires that no process of the fake's directory is left.
func killedToolLeavesNoFake(t *testing.T, r *rig, half *testkit.FakeCommand, extra []string) {
	t.Helper()
	self, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	cmd := exec.Command(self, "-test.run=^TestToolChildProcess$")
	cmd.Env = append(append(append([]string{}, r.env...), "NEVR_TGT_TOOL_CHILD=1"), extra...)
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	testkit.WaitFor(t, "the fake to be running under the tool", func() bool {
		_, err := os.Stat(filepath.Join(half.Dir(), "ready"))
		return err == nil
	})
	cmd.Process.Kill()
	cmd.Wait()
	requireNoFakeLeft(t, half.Dir())
}

func TestTheDefaultWaitsAreThePinnedOnes(t *testing.T) {
	t.Parallel()
	if defaultOptions.leftoverDelay != 10*time.Second || defaultOptions.goGrace != 30*time.Second || defaultOptions.interruptGrace != 90*time.Second {
		t.Errorf("defaultOptions = %+v", defaultOptions)
	}
}

func TestGoStillRunningAfterItsGraceIsKilled(t *testing.T) {
	t.Parallel()
	r := newRig(t, "", 0)
	r.script(r.fake, "ignore-sigint", "")
	opts := defaultOptions
	opts.goGrace = 300 * time.Millisecond
	ctx, cancel := context.WithCancel(t.Context())
	defer cancel()
	done := make(chan int, 1)
	var out, errb bytes.Buffer
	go func() {
		done <- runWith(opts, ctx, nil, testkit.Getenv(r.env), strings.NewReader(""), &out, &errb)
	}()
	testkit.WaitFor(t, "the go fake to be running", func() bool {
		_, err := os.Stat(filepath.Join(r.fake.Dir(), "ready"))
		return err == nil
	})
	cancel()
	select {
	case code := <-done:
		if code != 137 {
			t.Errorf("exit %d, want 137 (killed after the grace)", code)
		}
	case <-t.Context().Done():
		t.Fatal("the run did not end")
	}
	requireNoFakeLeft(t, r.fake.Dir())
}

// ---- the machine-wide test lock is held across both halves (R3-N3) ----

func (r *rig) probeLock() string {
	r.t.Helper()
	path := filepath.Join(r.logDir, lockName)
	r.script(r.py, "probe-lock", path)
	r.script(r.fake, "probe-lock", path)
	return path
}

func seenLock(t *testing.T, f *testkit.FakeCommand) string {
	t.Helper()
	raw, err := os.ReadFile(filepath.Join(f.Dir(), "lock-seen"))
	if err != nil {
		return "not run"
	}
	return strings.TrimSpace(string(raw))
}

func TestAWholeSuiteRunHoldsTheMachineLockWhileBothHalvesRunAndReleasesItAfter(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	path := r.probeLock()
	if code, out, errb := r.run("--", "python3", "tools/run_tool_tests.py"); code != 0 {
		t.Fatalf("exit %d, stdout %q, stderr %q", code, out, errb)
	}
	if got := seenLock(t, r.py); got != "held" {
		t.Errorf("during the Python half the lock was %q, want held", got)
	}
	if got := seenLock(t, r.fake); got != "held" {
		t.Errorf("during the Go half the lock was %q, want held (the lock must cover both halves)", got)
	}
	if raw, _ := os.ReadFile(path); len(raw) != 0 {
		t.Errorf("the lock file still names a holder after the run: %q", raw)
	}
	f, _ := os.Open(path)
	defer f.Close()
	// A fork by a parallel test may hold the descriptor's file description for the moment before its exec closes it.
	testkit.WaitFor(t, "the lock to be free after the run", func() bool {
		return syscall.Flock(int(f.Fd()), syscall.LOCK_EX|syscall.LOCK_NB) == nil
	})
}

func TestTheLockFileNamesTheToolAndItsPidSoTheRunnerKnowsTheLockIsItsOwn(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	path := filepath.Join(r.logDir, lockName)
	r.script(r.fake, "wait-sigint", "") // the Go half stays up, with the lock held, until the test cancels
	ctx, cancel := context.WithCancel(t.Context())
	defer cancel()
	done := make(chan int, 1)
	var out, errb bytes.Buffer
	go func() {
		done <- runWith(defaultOptions, ctx, []string{"--", "python3"}, testkit.Getenv(r.env), strings.NewReader(""), &out, &errb)
	}()
	testkit.WaitFor(t, "the go fake to wait", func() bool {
		_, err := os.Stat(filepath.Join(r.fake.Dir(), "ready"))
		return err == nil
	})
	held, _ := os.ReadFile(path)
	cancel()
	<-done
	var rec struct {
		Pid  int    `json:"pid"`
		Tool string `json:"tool"`
	}
	if err := json.Unmarshal(held, &rec); err != nil || rec.Tool != "test-go-tools" || rec.Pid != os.Getpid() {
		t.Errorf("lock file %q: want this process (pid %d) named as test-go-tools (err %v)", held, os.Getpid(), err)
	}
}

func TestARunGivenArgumentsTakesNoLockHere(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	r.probeLock()
	if code, _, errb := r.run("--", "python3", "tools/run_tool_tests.py", "tools/tests/test_x.py"); code != 0 {
		t.Fatalf("exit %d, stderr %q", code, errb)
	}
	if py, gor := seenLock(t, r.py), seenLock(t, r.fake); py != "free" || gor != "free" {
		t.Errorf("a run that names test paths held the lock: python %q, go %q", py, gor)
	}
}

func TestAGoOnlyRunTakesNoLock(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	r.probeLock()
	if code, _, errb := r.run(); code != 0 {
		t.Fatalf("exit %d, stderr %q", code, errb)
	}
	if got := seenLock(t, r.fake); got != "free" {
		t.Errorf("a run with no Python half held the lock: %q", got)
	}
}

// hold takes the lock file the way another run does, in a descriptor of the test's own.
func hold(t *testing.T, path string) (release func()) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	f, err := os.OpenFile(path, os.O_RDWR|os.O_CREATE, 0o644)
	if err != nil {
		t.Fatal(err)
	}
	if err := syscall.Flock(int(f.Fd()), syscall.LOCK_EX|syscall.LOCK_NB); err != nil {
		t.Fatal(err)
	}
	f.WriteString(`{"pid": 1, "checkout": "another"}`)
	var once sync.Once
	rel := func() { once.Do(func() { f.Truncate(0); f.Close() }) }
	t.Cleanup(rel)
	return rel
}

func TestAWholeSuiteRunThatCannotGetTheLockInTimeExits75WithoutRunningEitherHalf(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	hold(t, filepath.Join(r.logDir, lockName))
	r.env = append(r.env, "NEVR_TEST_LOCK_WAIT_S=0.2")
	code, _, errb := r.run("--", "python3")
	if code != 75 {
		t.Errorf("exit %d, want 75", code)
	}
	mustHave(t, errb, "was not free after", "another", "did not start")
	if len(r.py.Calls(t)) != 0 || len(r.fake.Calls(t)) != 0 {
		t.Error("a half ran without the lock")
	}
}

func TestAWholeSuiteRunWaitsForTheLockAndThenRunsBothHalves(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	release := hold(t, filepath.Join(r.logDir, lockName))
	done := make(chan int, 1)
	var out bytes.Buffer
	var errb syncBuffer
	go func() {
		done <- runWith(defaultOptions, t.Context(), []string{"--", "python3"}, testkit.Getenv(r.env), strings.NewReader(""), &out, &errb)
	}()
	testkit.WaitFor(t, "the run to say it waits", func() bool { return strings.Contains(errb.String(), "waiting for the machine-wide test lock") })
	if len(r.py.Calls(t)) != 0 {
		t.Error("the Python half ran before the lock was free")
	}
	release()
	select {
	case code := <-done:
		if code != 0 || len(r.py.Calls(t)) != 1 || len(r.fake.Calls(t)) != 1 {
			t.Errorf("exit %d, python calls %d, go calls %d", code, len(r.py.Calls(t)), len(r.fake.Calls(t)))
		}
	case <-t.Context().Done():
		t.Fatal("the run did not end after the lock was released")
	}
}

func TestACancelWhileWaitingForTheLockEndsTheRunWith130(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	hold(t, filepath.Join(r.logDir, lockName))
	ctx, cancel := context.WithCancel(t.Context())
	defer cancel()
	done := make(chan int, 1)
	var out bytes.Buffer
	var errb syncBuffer
	go func() {
		done <- runWith(defaultOptions, ctx, []string{"--", "python3"}, testkit.Getenv(r.env), strings.NewReader(""), &out, &errb)
	}()
	testkit.WaitFor(t, "the run to say it waits", func() bool { return strings.Contains(errb.String(), "waiting for the machine-wide test lock") })
	cancel()
	if code := <-done; code != 130 || len(r.py.Calls(t)) != 0 {
		t.Errorf("exit %d, python calls %d", code, len(r.py.Calls(t)))
	}
}

func TestALockWaitThatIsNotAPositiveNumberIsAUsageError(t *testing.T) {
	t.Parallel()
	for _, bad := range []string{"soon", "0", "-3", "NaN"} {
		r := newRig(t, passingStream(), 0)
		r.env = append(r.env, "NEVR_TEST_LOCK_WAIT_S="+bad)
		code, _, errb := r.run("--", "python3")
		if code != 2 || !strings.Contains(errb, "NEVR_TEST_LOCK_WAIT_S") || len(r.py.Calls(t)) != 0 {
			t.Errorf("%q: exit %d, stderr %q", bad, code, errb)
		}
	}
}

// syncBuffer is a bytes.Buffer a test reads while the run writes it.
type syncBuffer struct {
	mu sync.Mutex
	b  bytes.Buffer
}

func (s *syncBuffer) Write(p []byte) (int, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.b.Write(p)
}

func (s *syncBuffer) String() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.b.String()
}

// ---- a terminal is not the Python half's input (R3-N1) ----

// pty opens a pseudo-terminal pair; the slave end is a terminal that nobody types on.
func pty(t *testing.T) (slave *os.File) {
	t.Helper()
	master, err := os.OpenFile("/dev/ptmx", os.O_RDWR|syscall.O_NOCTTY, 0)
	if err != nil {
		t.Skipf("no /dev/ptmx: %v", err)
	}
	t.Cleanup(func() { master.Close() })
	var unlock, n uint32
	if _, _, e := syscall.Syscall(syscall.SYS_IOCTL, master.Fd(), syscall.TIOCSPTLCK, uintptr(unsafe.Pointer(&unlock))); e != 0 {
		t.Skipf("TIOCSPTLCK: %v", e)
	}
	if _, _, e := syscall.Syscall(syscall.SYS_IOCTL, master.Fd(), syscall.TIOCGPTN, uintptr(unsafe.Pointer(&n))); e != 0 {
		t.Skipf("TIOCGPTN: %v", e)
	}
	slave, err = os.OpenFile("/dev/pts/"+strconv.Itoa(int(n)), os.O_RDWR|syscall.O_NOCTTY, 0)
	if err != nil {
		t.Skipf("cannot open the pty slave: %v", err)
	}
	t.Cleanup(func() { slave.Close() })
	return slave
}

func TestAPythonHalfThatReadsStandardInputGetsEndOfFileWhenTheCallersInputIsATerminal(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	r.script(r.py, "read-stdin", "")
	ctx, cancel := context.WithTimeout(t.Context(), 20*time.Second)
	defer cancel()
	var out, errb bytes.Buffer
	code := runWith(defaultOptions, ctx, []string{"--", "python3"}, testkit.Getenv(r.env), pty(t), &out, &errb)
	if code != 0 || !strings.Contains(out.String(), "stdin-ended after 0 bytes") {
		t.Errorf("exit %d (a read of the terminal never ends), stdout %q, stderr %q", code, out.String(), errb.String())
	}
}

func TestAPythonHalfThatReadsStandardInputStillGetsAPipeTheCallerGaveIt(t *testing.T) {
	t.Parallel()
	r := newRig(t, passingStream(), 0)
	r.script(r.py, "read-stdin", "")
	var out, errb bytes.Buffer
	code := run(t.Context(), []string{"--", "python3"}, testkit.Getenv(r.env), strings.NewReader("abc"), &out, &errb)
	if code != 0 || !strings.Contains(out.String(), "stdin-ended after 3 bytes") {
		t.Errorf("exit %d, stdout %q", code, out.String())
	}
}
