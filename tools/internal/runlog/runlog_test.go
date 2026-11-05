package runlog

import (
	"bytes"
	"encoding/json"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
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
	l.Logger.Info("started", "pid", 42, "args", []string{"a", "b"})
	// Read before Close: the line is on disk when Info returns.
	if fi, err := os.Stat(filepath.Join(state, "nevr-runtime", "my-tool.jsonl")); err != nil || fi.Mode().Perm()&0o600 != 0o600 {
		t.Errorf("the log file's mode is %v (%v), want it readable and writable by its owner", fi, err)
	}
	lines := readLines(t, filepath.Join(state, "nevr-runtime", "my-tool.jsonl"))
	if len(lines) != 1 {
		t.Fatalf("%d lines after one event, want 1: %v", len(lines), lines)
	}
	got := lines[0]
	if got["msg"] != "started" || got["level"] != "INFO" || got["pid"] != float64(42) {
		t.Errorf("line = %v", got)
	}
	if _, ok := got["time"].(string); !ok {
		t.Errorf("line has no time: %v", got)
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
	if len(lines) != 2 || lines[0]["msg"] != "first" || lines[1]["msg"] != "second" {
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
	if lines := readLines(t, path); len(lines) != 1 || lines[0]["level"] != "WARN" {
		t.Errorf("lines = %v", lines)
	}
}
