package runlog

import (
	"bytes"
	"encoding/json"
	"log/slog"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

func env(kv map[string]string) func(string) string { return func(k string) string { return kv[k] } }

func readLines(t *testing.T, path string) []map[string]any {
	t.Helper()
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	var out []map[string]any
	for _, line := range strings.Split(strings.TrimSuffix(string(raw), "\n"), "\n") {
		if line == "" {
			continue
		}
		var m map[string]any
		if err := json.Unmarshal([]byte(line), &m); err != nil {
			t.Fatalf("not a JSON line: %q: %v", line, err)
		}
		out = append(out, m)
	}
	return out
}

func TestPathIsUnderXDGStateHomeInNevrRuntime(t *testing.T) {
	t.Parallel()
	got, err := Path("my-tool", env(map[string]string{"XDG_STATE_HOME": "/state", "HOME": "/home/u"}))
	if err != nil || got != "/state/nevr-runtime/my-tool.jsonl" {
		t.Errorf("Path = %q, %v", got, err)
	}
}

func TestPathDefaultsXDGStateHomeToDotLocalStateUnderHome(t *testing.T) {
	t.Parallel()
	got, err := Path("my-tool", env(map[string]string{"HOME": "/home/u"}))
	if err != nil || got != "/home/u/.local/state/nevr-runtime/my-tool.jsonl" {
		t.Errorf("Path = %q, %v", got, err)
	}
	// The XDG spec: a relative XDG_STATE_HOME is invalid and ignored.
	got, err = Path("my-tool", env(map[string]string{"XDG_STATE_HOME": "relative/state", "HOME": "/home/u"}))
	if err != nil || got != "/home/u/.local/state/nevr-runtime/my-tool.jsonl" {
		t.Errorf("Path with a relative XDG_STATE_HOME = %q, %v", got, err)
	}
}

func TestPathNeedsXDGStateHomeOrHomeAndSaysSo(t *testing.T) {
	t.Parallel()
	_, err := Path("my-tool", env(nil))
	if err == nil || !strings.Contains(err.Error(), "XDG_STATE_HOME") || !strings.Contains(err.Error(), "HOME") {
		t.Errorf("err = %v, want it to name XDG_STATE_HOME and HOME", err)
	}
}

func TestPathRefusesAToolNameThatIsNotAPlainName(t *testing.T) {
	t.Parallel()
	for _, name := range []string{"", ".", "..", "a/b", "a b", "../x", "A", "tool.py"} {
		if got, err := Path(name, env(map[string]string{"XDG_STATE_HOME": "/s"})); err == nil {
			t.Errorf("Path(%q) = %q, want an error", name, got)
		}
	}
	for _, name := range []string{"rollback-beta", "host_stop_cause", "tool2"} {
		if _, err := Path(name, env(map[string]string{"XDG_STATE_HOME": "/s"})); err != nil {
			t.Errorf("Path(%q): %v", name, err)
		}
	}
}

func TestEachEventIsOneJSONLineWrittenAtOnce(t *testing.T) {
	t.Parallel()
	state := t.TempDir()
	l, err := Open("my-tool", env(map[string]string{"XDG_STATE_HOME": state}))
	if err != nil {
		t.Fatal(err)
	}
	defer l.Close()
	l.Logger.Info("started", "count", 42, "args", []string{"a", "b"})
	// Read before Close: the line is on disk when Info returns.
	if fi, err := os.Stat(filepath.Join(state, "nevr-runtime", "my-tool.jsonl")); err != nil || fi.Mode().Perm()&0o600 != 0o600 {
		t.Errorf("the log file's mode is %v (%v), want it readable and writable by its owner", fi, err)
	}
	lines := readLines(t, filepath.Join(state, "nevr-runtime", "my-tool.jsonl"))
	if len(lines) != 1 {
		t.Fatalf("%d lines after one event, want 1: %v", len(lines), lines)
	}
	got := lines[0]
	if got["event"] != "started" || got["level"] != "info" || got["count"] != float64(42) {
		t.Errorf("line = %v", got)
	}
	if args, _ := got["args"].([]any); len(args) != 2 {
		t.Errorf("args = %v", got["args"])
	}
}

func TestOpenAppendsToTheFileAndNeverTruncatesIt(t *testing.T) {
	t.Parallel()
	state := t.TempDir()
	e := env(map[string]string{"XDG_STATE_HOME": state})
	for _, msg := range []string{"first", "second"} {
		l, err := Open("my-tool", e)
		if err != nil {
			t.Fatal(err)
		}
		l.Logger.Info(msg)
		if err := l.Close(); err != nil {
			t.Fatal(err)
		}
	}
	lines := readLines(t, filepath.Join(state, "nevr-runtime", "my-tool.jsonl"))
	if len(lines) != 2 || lines[0]["event"] != "first" || lines[1]["event"] != "second" {
		t.Errorf("lines = %v", lines)
	}
}

func TestConcurrentEventsAreWholeLines(t *testing.T) {
	t.Parallel()
	state := t.TempDir()
	l, err := Open("my-tool", env(map[string]string{"XDG_STATE_HOME": state}))
	if err != nil {
		t.Fatal(err)
	}
	var wg sync.WaitGroup
	for i := range 64 {
		wg.Add(1)
		go func() {
			defer wg.Done()
			l.Logger.Info("event", "n", i, "pad", strings.Repeat("x", 500))
		}()
	}
	wg.Wait()
	l.Close()
	if lines := readLines(t, filepath.Join(state, "nevr-runtime", "my-tool.jsonl")); len(lines) != 64 {
		t.Errorf("%d lines, want 64", len(lines))
	}
}

func TestOpenFailsWhenTheDirectoryCannotBeMade(t *testing.T) {
	t.Parallel()
	blocker := filepath.Join(t.TempDir(), "file")
	if err := os.WriteFile(blocker, nil, 0o600); err != nil {
		t.Fatal(err)
	}
	_, err := Open("my-tool", env(map[string]string{"XDG_STATE_HOME": blocker}))
	if err == nil || !strings.Contains(err.Error(), blocker) {
		t.Errorf("err = %v, want it to name the path under %s", err, blocker)
	}
}

func TestRunReturnsTheBodysCodeWhenTheLogWorks(t *testing.T) {
	t.Parallel()
	state := t.TempDir()
	var stderr bytes.Buffer
	for _, want := range []int{0, 3} {
		got := Run("my-tool", env(map[string]string{"XDG_STATE_HOME": state}), &stderr, func(log *slog.Logger) int {
			log.Info("body ran")
			return want
		})
		if got != want {
			t.Errorf("Run returned %d, want the body's %d", got, want)
		}
	}
	if stderr.Len() != 0 {
		t.Errorf("stderr = %q, want nothing", stderr.String())
	}
	if lines := readLines(t, filepath.Join(state, "nevr-runtime", "my-tool.jsonl")); len(lines) != 2 {
		t.Errorf("%d lines, want 2", len(lines))
	}
}

func TestRunDoesNotRunTheBodyWhenTheLogCannotBeOpenedAndSaysWhy(t *testing.T) {
	t.Parallel()
	blocker := filepath.Join(t.TempDir(), "file")
	if err := os.WriteFile(blocker, nil, 0o600); err != nil {
		t.Fatal(err)
	}
	var stderr bytes.Buffer
	ran := false
	got := Run("my-tool", env(map[string]string{"XDG_STATE_HOME": blocker}), &stderr, func(*slog.Logger) int { ran = true; return 0 })
	if ran {
		t.Error("the body ran with no log")
	}
	if got != 74 || ExitLogFailure != 74 { // EX_IOERR; the literal, so changing the constant is noticed
		t.Errorf("exit = %d (ExitLogFailure %d), want 74", got, ExitLogFailure)
	}
	for _, want := range []string{"my-tool", "run log", blocker} {
		if !strings.Contains(stderr.String(), want) {
			t.Errorf("stderr %q lacks %q", stderr.String(), want)
		}
	}
	if strings.Count(stderr.String(), "\n") != 1 {
		t.Errorf("stderr is not one line: %q", stderr.String())
	}
}

func TestRunFailsAfterTheBodyWhenAnEventCouldNotBeWritten(t *testing.T) {
	t.Parallel()
	state := t.TempDir()
	if err := os.MkdirAll(filepath.Join(state, "nevr-runtime"), 0o755); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat("/dev/full"); err != nil {
		t.Skipf("no /dev/full here: %v", err)
	}
	// A log file that opens and refuses every write (ENOSPC).
	if err := os.Symlink("/dev/full", filepath.Join(state, "nevr-runtime", "my-tool.jsonl")); err != nil {
		t.Fatal(err)
	}
	e := env(map[string]string{"XDG_STATE_HOME": state})

	var stderr bytes.Buffer
	got := Run("my-tool", e, &stderr, func(log *slog.Logger) int { log.Info("lost"); return 0 })
	if got != 74 {
		t.Errorf("a body that returned 0 got exit %d, want 74", got)
	}
	for _, want := range []string{"my-tool", "run log", "no space left on device"} { // /dev/full's answer to a write
		if !strings.Contains(stderr.String(), want) {
			t.Errorf("stderr %q lacks %q", stderr.String(), want)
		}
	}
	if strings.Count(stderr.String(), "\n") != 1 {
		t.Errorf("stderr is not one line (a write error is reported once, not per event): %q", stderr.String())
	}

	stderr.Reset()
	got = Run("my-tool", e, &stderr, func(log *slog.Logger) int { log.Info("lost"); log.Info("lost again"); return 5 })
	if got != 5 {
		t.Errorf("a body that returned 5 got exit %d, want its own 5 (the log failure is still on stderr)", got)
	}
	if stderr.Len() == 0 {
		t.Error("the write failure was not reported when the body failed too")
	}
}

func TestOpenReportsAWriteErrorThroughCloseAndErr(t *testing.T) {
	t.Parallel()
	if _, err := os.Stat("/dev/full"); err != nil {
		t.Skipf("no /dev/full here: %v", err)
	}
	state := t.TempDir()
	if err := os.MkdirAll(filepath.Join(state, "nevr-runtime"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink("/dev/full", filepath.Join(state, "nevr-runtime", "my-tool.jsonl")); err != nil {
		t.Fatal(err)
	}
	l, err := Open("my-tool", env(map[string]string{"XDG_STATE_HOME": state}))
	if err != nil {
		t.Fatal(err)
	}
	if l.Err() != nil {
		t.Fatalf("Err before any event = %v", l.Err())
	}
	l.Logger.Info("lost")
	if l.Err() == nil {
		t.Error("Err is nil after a failed write")
	}
	if err := l.Close(); err == nil {
		t.Error("Close returned nil after a failed write")
	}
}

func TestOpenAtWritesTheFileItIsGivenAndMakesItsDirectory(t *testing.T) {
	t.Parallel()
	path := filepath.Join(t.TempDir(), "a", "b", "custom.jsonl")
	l, err := OpenAt(path)
	if err != nil {
		t.Fatal(err)
	}
	if l.Path() != path {
		t.Errorf("Path = %q, want %q", l.Path(), path)
	}
	l.Logger.Warn("w")
	if err := l.Close(); err != nil {
		t.Fatal(err)
	}
	if lines := readLines(t, path); len(lines) != 1 || lines[0]["level"] != "warn" {
		t.Errorf("lines = %v", lines)
	}
}

// The shape the Python tools' logs have (ts in UTC, event) and the identity a record needs to be found by who and when
// (AGENTS.md, logging): ADR 0044 rule 13.
func TestEveryRecordCarriesTsInUTCEventToolPidAndRun(t *testing.T) {
	t.Parallel()
	state := t.TempDir()
	l, err := Open("my-tool", env(map[string]string{"XDG_STATE_HOME": state}))
	if err != nil {
		t.Fatal(err)
	}
	before := time.Now().UTC().Add(-time.Second)
	l.Logger.Info("start")
	l.Logger.Error("boom", "code", 3)
	l.Close()
	lines := readLines(t, filepath.Join(state, "nevr-runtime", "my-tool.jsonl"))
	if len(lines) != 2 {
		t.Fatalf("%d lines", len(lines))
	}
	for i, rec := range lines {
		for _, gone := range []string{"time", "msg"} {
			if _, ok := rec[gone]; ok {
				t.Errorf("record %d has the slog default key %q: %v", i, gone, rec)
			}
		}
		ts, _ := rec["ts"].(string)
		parsed, err := time.Parse("2006-01-02T15:04:05.000Z", ts)
		if err != nil {
			t.Fatalf("record %d: ts %q is not UTC with milliseconds and a Z: %v", i, ts, err)
		}
		if parsed.Before(before) || parsed.After(time.Now().UTC().Add(time.Second)) {
			t.Errorf("record %d: ts %v is not now", i, parsed)
		}
		if rec["tool"] != "my-tool" {
			t.Errorf("record %d: tool = %v", i, rec["tool"])
		}
		if rec["pid"] != float64(os.Getpid()) {
			t.Errorf("record %d: pid = %v, want %d", i, rec["pid"], os.Getpid())
		}
	}
	run, _ := lines[0]["run"].(string)
	if !regexp.MustCompile(`^\d{8}T\d{6}\.\d{6}Z-\d+-[0-9a-f]{16}$`).MatchString(run) {
		t.Errorf("run = %q, want <UTC stamp>-<pid>-<16 hex>, the form of the test runner's stem", run)
	}
	if !strings.Contains(run, "-"+strconv.Itoa(os.Getpid())+"-") {
		t.Errorf("run = %q does not carry this process's pid %d", run, os.Getpid())
	}
	if lines[1]["run"] != run || l.RunID() != run {
		t.Errorf("run differs between records or from RunID(): %v, %v, %q", lines[1]["run"], run, l.RunID())
	}
	if lines[1]["level"] != "error" || lines[1]["code"] != float64(3) || lines[1]["event"] != "boom" {
		t.Errorf("record 1 = %v", lines[1])
	}
}

func TestTwoOpensGetTwoRunIDs(t *testing.T) {
	t.Parallel()
	state := t.TempDir()
	e := env(map[string]string{"XDG_STATE_HOME": state})
	a, _ := Open("my-tool", e)
	b, _ := Open("my-tool", e)
	defer a.Close()
	defer b.Close()
	if a.RunID() == "" || a.RunID() == b.RunID() {
		t.Errorf("run ids %q and %q", a.RunID(), b.RunID())
	}
}

func TestLevelsAreLowerCaseNames(t *testing.T) {
	t.Parallel()
	state := t.TempDir()
	l, _ := Open("my-tool", env(map[string]string{"XDG_STATE_HOME": state}))
	l.Logger.Debug("d")
	l.Logger.Info("i")
	l.Logger.Warn("w")
	l.Logger.Error("e")
	l.Close()
	var got []string
	for _, rec := range readLines(t, filepath.Join(state, "nevr-runtime", "my-tool.jsonl")) {
		got = append(got, rec["level"].(string))
	}
	// Debug is not written: the log is for what an operator reads (docs/standards/logging.md, level guidelines).
	if strings.Join(got, ",") != "info,warn,error" {
		t.Errorf("levels = %v", got)
	}
}

// A caller's field may be named like a header key (a port keeps the Python tool's field names, and some of them are
// `level` and `time`): it is written under a prefix, never as a second header and never as a panic.
func TestAFieldNamedLikeAHeaderKeyIsPrefixedAndNeverBreaksTheRecord(t *testing.T) {
	t.Parallel()
	for _, key := range []string{"ts", "level", "event", "tool", "run", "pid", "msg", "time"} {
		state := t.TempDir()
		l, err := Open("my-tool", env(map[string]string{"XDG_STATE_HOME": state}))
		if err != nil {
			t.Fatal(err)
		}
		l.Logger.Info("step", key, "caller value")
		if err := l.Close(); err != nil {
			t.Fatalf("%s: %v", key, err)
		}
		raw, _ := os.ReadFile(filepath.Join(state, "nevr-runtime", "my-tool.jsonl"))
		rec := readLines(t, filepath.Join(state, "nevr-runtime", "my-tool.jsonl"))[0]
		if rec["event"] != "step" || rec["field_"+key] != "caller value" {
			t.Errorf("%s: record = %s", key, raw)
		}
		for _, header := range []string{`"ts"`, `"event"`, `"level"`, `"tool"`, `"run"`, `"pid"`} {
			if strings.Count(string(raw), header+":") != 1 {
				t.Errorf("%s: key %s appears %d times in %s", key, header, strings.Count(string(raw), header+":"), raw)
			}
		}
		if strings.Contains(string(raw), `"msg"`) || strings.Contains(string(raw), `"time"`) {
			t.Errorf("%s: slog's own keys leaked: %s", key, raw)
		}
	}
}

func TestATimeAndALevelValueOfTheRightKindIsAlsoPrefixed(t *testing.T) {
	t.Parallel()
	state := t.TempDir()
	l, _ := Open("my-tool", env(map[string]string{"XDG_STATE_HOME": state}))
	when := time.Date(2026, 1, 2, 3, 4, 5, 0, time.UTC)
	l.Logger.Info("step", "time", when, "level", slog.LevelWarn)
	l.Close()
	rec := readLines(t, filepath.Join(state, "nevr-runtime", "my-tool.jsonl"))[0]
	if rec["field_time"] != "2026-01-02T03:04:05Z" || rec["field_level"] == nil || rec["level"] != "info" {
		t.Errorf("record = %v", rec)
	}
}

func TestFieldsAddedWithWithAreRenamedToo(t *testing.T) {
	t.Parallel()
	state := t.TempDir()
	l, _ := Open("my-tool", env(map[string]string{"XDG_STATE_HOME": state}))
	l.Logger.With("level", "high").Info("step")
	l.Close()
	rec := readLines(t, filepath.Join(state, "nevr-runtime", "my-tool.jsonl"))[0]
	if rec["field_level"] != "high" || rec["level"] != "info" {
		t.Errorf("record = %v", rec)
	}
}

func TestAFieldInsideAGroupKeepsItsName(t *testing.T) {
	t.Parallel()
	state := t.TempDir()
	l, _ := Open("my-tool", env(map[string]string{"XDG_STATE_HOME": state}))
	l.Logger.Info("step", slog.Group("detail", "level", "high", "msg", "m"))
	l.Close()
	rec := readLines(t, filepath.Join(state, "nevr-runtime", "my-tool.jsonl"))[0]
	detail, _ := rec["detail"].(map[string]any)
	if detail["level"] != "high" || detail["msg"] != "m" {
		t.Errorf("record = %v", rec)
	}
}

// invocationRecords is what one run of Invocation writes: "invoked" and "finished".
const invocationRecords = 2

var invokedArgs = []string{"-x", "y"}

// Invocation writes one invoked and one finished record, with the arguments, the exit code and the outcome, and returns
// the body's exit code.
func TestInvocationRecordsTheArgumentsTheExitCodeAndTheOutcome(t *testing.T) {
	t.Parallel()
	state := t.TempDir()
	getenv := env(map[string]string{"XDG_STATE_HOME": state, "USER": "tester"})
	const bodyExit = 7
	code := Invocation("my-tool", invokedArgs, getenv, &bytes.Buffer{}, func(*slog.Logger) int { return bodyExit })
	if code != bodyExit {
		t.Fatalf("exit %d, want %d", code, bodyExit)
	}
	recs := readLines(t, filepath.Join(state, "nevr-runtime", "my-tool.jsonl"))
	if len(recs) != invocationRecords || recs[0]["event"] != "invoked" || recs[1]["event"] != "finished" {
		t.Fatalf("records %v", recs)
	}
	if args, _ := recs[0]["args"].([]any); len(args) != len(invokedArgs) || args[1] != "y" || recs[0]["user"] != "tester" {
		t.Fatalf("invoked record %v", recs[0])
	}
	if recs[1]["exit"] != float64(bodyExit) || recs[1]["outcome"] != "failed" {
		t.Fatalf("finished record %v", recs[1])
	}
}
