// Command test-go-tools runs the tool suite's two halves, the Python half and the Go half, and keeps the Go half:
//
//	test-go-tools [-C dir] [package patterns] [-- python command and arguments]
//
// -C names the directory go test runs in, as go's own -C does (the module is tools/go.mod, and a recipe runs from the
// repository root); the Python command runs where test-go-tools was started.
//
// With a Python command after "--" it runs that first (its output passes through, its exit code is kept), then, unless it
// was interrupted (an exit code above 128), the Go half whatever the Python half did, so a red Python test never hides
// the Go tests and the ADR 0044 policy checks. The Go half is `go test -count=1 -json -timeout 5m` over ./... (or the
// patterns given), started with the hermetic environment of testkit.EnvAt (no credentials, display, git configuration
// or signing key; the Go caches kept), its stream written byte for byte to a file in the test-run log directory, the
// failing and skipped tests named on standard output, and go_start and go_end records appended to the Python runner's
// runs.jsonl. The run ends with one line that names each failed half, and the exit code is the Python half's when it
// failed, else go's.
//
// A run of the whole suite (the Python command is the runner alone: nothing follows its script) holds the machine-wide
// test lock of tools/run_tool_tests.py (an exclusive flock on <log dir>/run.lock, waited for up to
// $NEVR_TEST_LOCK_WAIT_S, 5400 s by default; exit 75 when it is not free in time) across both halves, so two seats' Go
// halves never overlap either. The lock file names this tool as its holder, which is how the runner, whose parent this is,
// knows the lock is already its own and does not wait for it. A run given test paths or other arguments takes no lock
// here, as the runner decides for itself.
//
// The Python command's standard input is the caller's, except a terminal, which is replaced by /dev/null: the command
// runs in a process group of its own, so a read of the terminal would stop it for good (SIGTTIN).
//
// The stream is <stem>-go.jsonl in $NEVR_TEST_LOG_DIR, else test-runs/tools-tests in the work root ($NEVR_WORK_ROOT,
// else ~/src/nevr-runtime-work), where <stem> is the Python run's file stem (read from its "test-tools: log <path>"
// line, so the two halves of one run share a name), or a stem of its own. Exit codes: as above; 2 when the run could not
// be kept or go could not be started; 74 when the tool's own run log fails (internal/runlog).
//
// ADR 0044 and `just test-tools` and `just verify` use it.
package main

import (
	"bufio"
	"bytes"
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"syscall"
	"time"
	"unsafe"

	"github.com/EchoTools/nevr-runtime/tools/internal/runlog"
	"github.com/EchoTools/nevr-runtime/tools/internal/testkit"
)

func main() {
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	code := run(ctx, os.Args[1:], os.Getenv, os.Stdin, os.Stdout, os.Stderr)
	stop()
	os.Exit(code)
}

func run(ctx context.Context, args []string, getenv func(string) string, stdin io.Reader, stdout, stderr io.Writer) int {
	return runWith(defaultOptions, ctx, args, getenv, stdin, stdout, stderr)
}

// options are the waits and limits of a run; a test gives shorter ones.
type options struct {
	// leftoverDelay is how long a Python half that has exited may leave a process holding its output before the tool
	// stops reading it, keeps the exit status and names the process.
	leftoverDelay time.Duration
	// interruptGrace is how long the Python half has to end after SIGINT before it is killed. It outlasts the Python
	// runner's own interrupt path (NEVR_TEST_INTERRUPT_GRACE_S 30 s, then up to two NEVR_TEST_DRAIN_S 10 s waits) so the
	// runner's end record is not lost to a kill.
	interruptGrace time.Duration
	// goGrace is the same for go test.
	goGrace time.Duration
	// lockWait is how long a whole-suite run waits for the machine-wide lock when $NEVR_TEST_LOCK_WAIT_S is unset.
	lockWait time.Duration
}

var defaultOptions = options{leftoverDelay: 10 * time.Second, interruptGrace: 90 * time.Second, goGrace: 30 * time.Second, lockWait: 5400 * time.Second}

func runWith(opts options, ctx context.Context, args []string, getenv func(string) string, stdin io.Reader, stdout, stderr io.Writer) int {
	return runlog.Run("test-go-tools", getenv, stderr, func(log *slog.Logger) int {
		return testTools(opts, ctx, args, getenv, stdin, stdout, stderr, log)
	})
}

const (
	// exitSetup is the exit code of a run that could not be made or kept.
	exitSetup = 2
	// goTestTimeout ends a hung test with its name and a stack, in place of go's ten minutes a package.
	goTestTimeout = 5 * time.Minute
	// pythonLogPrefix starts the line in which the Python runner names its log (tools/run_tool_tests.py).
	pythonLogPrefix = "test-tools: log "
)

func testTools(opts options, ctx context.Context, args []string, getenv func(string) string, stdin io.Reader, stdout, stderr io.Writer, log *slog.Logger) int {
	patterns, pyCmd, hasPy := splitArgs(args)
	moduleDir := ""
	if len(patterns) > 0 && patterns[0] == "-C" {
		if len(patterns) < 2 {
			fmt.Fprintln(stderr, "test-tools: FAIL usage: -C needs a directory")
			return exitSetup
		}
		moduleDir, patterns = patterns[1], patterns[2:]
	}
	if hasPy && len(pyCmd) == 0 {
		fmt.Fprintln(stderr, "test-tools: FAIL usage: test-go-tools [-C dir] [package patterns] [-- python command and arguments]; nothing follows --")
		return exitSetup
	}
	if len(patterns) == 0 {
		patterns = []string{"./..."}
	}
	goPath, err := lookPath("go", getenv("PATH"))
	if err != nil {
		fmt.Fprintf(stderr, "test-tools: FAIL cannot find go: %v\n", err)
		log.Error("no go", "error", err.Error())
		return exitSetup
	}
	dir := logDir(getenv)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		fmt.Fprintf(stderr, "test-tools: FAIL cannot write the run record to %s: %v\n", dir, err)
		log.Error("no log dir", "dir", dir, "error", err.Error())
		return exitSetup
	}

	pyRC, stem, leftover := -1, "", []string(nil)
	if hasPy && wholeSuite(pyCmd) {
		lock, code := takeLock(ctx, opts, dir, getenv, stderr, log)
		if code != 0 {
			return code
		}
		defer lock.release()
	}
	if hasPy {
		pyRC, stem, leftover = runPython(opts, ctx, pyCmd, getenv, dir, stdin, stdout, stderr)
		log.Info("python half", "cmd", pyCmd, "exit", pyRC, "stem", stem, "leftover", leftover)
		// An interrupt is a signal this tool itself received (a terminal's CTRL+C reaches it as well as the runner). A
		// Python half that ended by a signal it was not sent (an OOM kill, timeout -k) is a failure like any other, and
		// the Go half still runs.
		if ctx.Err() != nil {
			fmt.Fprintf(stdout, "test-tools: python half exited %d (interrupted): go half not run\n", pyRC)
			if pyRC <= 128 {
				return 130
			}
			return pyRC
		}
	}
	paired := stem != ""
	if !paired {
		if stem, err = newStem(); err != nil {
			fmt.Fprintf(stderr, "test-tools: FAIL %v\n", err)
			return exitSetup
		}
	}
	stream, err := os.OpenFile(filepath.Join(dir, stem+"-go.jsonl"), os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o644)
	if err != nil {
		fmt.Fprintf(stderr, "test-tools: FAIL cannot write the go test stream in %s: %v\n", dir, err)
		log.Error("no stream", "dir", dir, "error", err.Error())
		return exitSetup
	}
	scratch, err := os.MkdirTemp("", "test-go-tools-")
	if err != nil {
		stream.Close()
		fmt.Fprintf(stderr, "test-tools: FAIL cannot make a scratch directory for go test: %v\n", err)
		return exitSetup
	}
	defer os.RemoveAll(scratch)
	env, err := testkit.EnvAt(scratch, getenv, goPath)
	if err != nil {
		stream.Close()
		fmt.Fprintf(stderr, "test-tools: FAIL cannot make go test's environment: %v\n", err)
		return exitSetup
	}

	recorded := true
	record := func(rec map[string]any) {
		rec["ts"] = time.Now().UTC().Format("2006-01-02T15:04:05.000Z")
		rec["stem"], rec["paired"], rec["stream"] = stem, paired, stream.Name()
		if pyRC >= 0 {
			rec["python_rc"] = pyRC
		}
		if len(leftover) > 0 {
			rec["leftover"] = leftover
		}
		if err := appendRecord(filepath.Join(dir, "runs.jsonl"), rec); err != nil {
			recorded = false
			fmt.Fprintf(stderr, "test-tools: FAIL cannot append to %s: %v\n", filepath.Join(dir, "runs.jsonl"), err)
			log.Error("no record", "error", err.Error())
		}
	}
	goArgs := append([]string{"test", "-count=1", "-json", "-timeout", goTestTimeout.String()}, patterns...)
	record(map[string]any{"event": "go_start", "cmd": append([]string{"go"}, goArgs...)})
	log.Info("go half", "go", goPath, "args", goArgs, "stream", stream.Name())

	col := &collector{}
	cmd := exec.CommandContext(ctx, goPath, goArgs...)
	cmd.Dir = moduleDir
	cmd.Env = env
	// A tool that dies without its cleanup (SIGKILL, a crash) must not leave go running, or a fake that holds a
	// worktree (remove-worktree refuses a process with its cwd inside). The kernel kills the child when its parent
	// thread exits; the tool's own grace kill covers the normal paths.
	cmd.SysProcAttr = &syscall.SysProcAttr{Pdeathsig: syscall.SIGKILL}
	cmd.Cancel = func() error { return cmd.Process.Signal(syscall.SIGINT) } // go test stops its test binaries on SIGINT
	cmd.WaitDelay = opts.goGrace
	cmd.Stdout = io.MultiWriter(stream, col)
	cmd.Stderr = stderr
	runErr := cmd.Run()
	col.flush()
	closeErr := stream.Close()

	goRC := 0
	var exitErr *exec.ExitError
	switch {
	case runErr == nil:
	case errors.As(runErr, &exitErr):
		goRC = exitCode(exitErr)
	default:
		fmt.Fprintf(stderr, "test-tools: FAIL go test did not run: %v (stream %s)\n", runErr, stream.Name())
		log.Error("go did not run", "error", runErr.Error(), "stream", stream.Name())
		return exitSetup
	}
	if closeErr != nil {
		fmt.Fprintf(stderr, "test-tools: FAIL the go test stream %s was not written whole: %v\n", stream.Name(), closeErr)
		log.Error("stream not written", "stream", stream.Name(), "error", closeErr.Error())
		return exitSetup
	}
	col.report(stdout, goRC, stream.Name())
	record(map[string]any{"event": "go_end", "rc": goRC, "passed": col.passed, "failed": nonNil(col.failedTests),
		"failed_packages": nonNil(col.failedPackages), "skipped": nonNil(col.skipped), "packages": len(col.packages)})
	log.Info("end", "exit", goRC, "passed", col.passed, "failed", len(col.failedTests), "failed_packages", len(col.failedPackages),
		"skipped", len(col.skipped), "packages", len(col.packages), "stream", stream.Name())

	var failed []string
	if pyRC > 0 {
		failed = append(failed, fmt.Sprintf("python half exit %d", pyRC))
	}
	if goRC != 0 {
		failed = append(failed, fmt.Sprintf("go half exit %d", goRC))
	}
	if len(failed) > 0 {
		fmt.Fprintf(stdout, "test-tools: FAIL: %s\n", strings.Join(failed, ", "))
	}
	switch {
	case pyRC > 0:
		return pyRC
	case goRC != 0:
		return goRC
	case !recorded:
		return exitSetup
	}
	return 0
}

// splitArgs separates the package patterns from the Python command after the first "--".
func splitArgs(args []string) (patterns, pyCmd []string, hasPy bool) {
	for i, a := range args {
		if a == "--" {
			return args[:i], args[i+1:], true
		}
	}
	return args, nil, false
}

// runPython runs the Python half: its standard output passes through and is read for the line that names its log; its
// standard error and input pass through. It returns the exit code (127 when the command could not be started, 128 plus
// the signal when it was killed by one), the stem of the log it named ("" when it named none, or named a file that is not
// in the log directory), and, when the command exited but a process it started still holds its output after
// opts.leftoverDelay, that process (the exit status is kept and the tool stops reading).
//
// The command runs in a process group of its own, so the processes it leaves can be found; the tool forwards SIGINT when
// it receives one, which a terminal would otherwise have sent to the group directly.
func runPython(opts options, ctx context.Context, argv []string, getenv func(string) string, logDir string, stdin io.Reader, stdout, stderr io.Writer) (rc int, stem string, leftover []string) {
	path := argv[0]
	if !strings.ContainsRune(path, '/') {
		var err error
		if path, err = lookPath(argv[0], getenv("PATH")); err != nil {
			fmt.Fprintf(stderr, "test-tools: FAIL cannot start the python half %q: %v\n", argv[0], err)
			return 127, "", nil
		}
	}
	scan := &lineScan{prefix: pythonLogPrefix}
	cmd := exec.Command(path, argv[1:]...)
	cmd.Stdin = stdinFor(stdin)
	cmd.Stdout = io.MultiWriter(stdout, scan)
	cmd.Stderr = stderr
	// Pdeathsig: as for the go half above, the python half does not outlive the tool that started it.
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true, Pdeathsig: syscall.SIGKILL}
	cmd.WaitDelay = opts.leftoverDelay
	if err := cmd.Start(); err != nil {
		fmt.Fprintf(stderr, "test-tools: FAIL cannot start the python half %q: %v\n", argv[0], err)
		return 127, "", nil
	}
	done := make(chan error, 1)
	go func() { done <- cmd.Wait() }()
	var err error
	select {
	case err = <-done:
	case <-ctx.Done():
		cmd.Process.Signal(syscall.SIGINT) // the runner turns it into its own clean end
		select {
		case err = <-done:
		case <-time.After(opts.interruptGrace):
			cmd.Process.Kill()
			err = <-done
		}
	}
	scan.flush()
	var exitErr *exec.ExitError
	switch {
	case err == nil:
		rc = 0
	case errors.Is(err, exec.ErrWaitDelay):
		// The command exited (cmd.ProcessState has its status) and a process it started still holds the output.
		rc = cmd.ProcessState.ExitCode() // the command exited by itself: ErrWaitDelay is only returned for a success
		leftover = processesInGroup(cmd.Process.Pid)
		fmt.Fprintf(stderr, "test-tools: python half exited %d, but a process it started still holds its output after %s and was left running: %s\n",
			rc, opts.leftoverDelay, strings.Join(leftover, "; "))
	case errors.As(err, &exitErr):
		rc = exitCode(exitErr)
	default:
		fmt.Fprintf(stderr, "test-tools: FAIL the python half %q: %v\n", argv[0], err)
		return 127, "", nil
	}
	if scan.value != "" {
		stem = stemOf(scan.value, logDir, stderr)
	}
	return rc, stem, leftover
}

// stemOf is the stem of the log file the Python runner named, when the file is in the log directory; "" otherwise.
func stemOf(named, logDir string, stderr io.Writer) string {
	clean, dir := filepath.Clean(named), filepath.Clean(logDir)
	if abs, err := filepath.Abs(clean); err == nil {
		clean = abs
	}
	if abs, err := filepath.Abs(dir); err == nil {
		dir = abs
	}
	fi, err := os.Stat(clean)
	if filepath.Dir(clean) != dir || !strings.HasSuffix(clean, ".log") || err != nil || !fi.Mode().IsRegular() {
		fmt.Fprintf(stderr, "test-tools: ignoring the python half's log line %q: it is not an existing .log file in %s\n", named, logDir)
		return ""
	}
	return strings.TrimSuffix(filepath.Base(clean), ".log")
}

// processesInGroup lists the processes of a process group as "pid N command", from /proc.
func processesInGroup(pgid int) []string {
	entries, err := os.ReadDir("/proc")
	if err != nil {
		return nil
	}
	var out []string
	for _, e := range entries {
		pid, err := strconv.Atoi(e.Name())
		if err != nil {
			continue
		}
		stat, err := os.ReadFile("/proc/" + e.Name() + "/stat")
		if err != nil {
			continue
		}
		// pid (comm) state ppid pgrp ...: comm may hold spaces and parentheses, so split after the last ')'.
		text := string(stat)
		rest := strings.Fields(text[strings.LastIndexByte(text, ')')+1:])
		if len(rest) < 3 {
			continue
		}
		if g, err := strconv.Atoi(rest[2]); err != nil || g != pgid {
			continue
		}
		cmdline, _ := os.ReadFile("/proc/" + e.Name() + "/cmdline")
		name := strings.TrimSpace(strings.ReplaceAll(string(cmdline), "\x00", " "))
		if name == "" {
			name = text[strings.IndexByte(text, '(')+1 : strings.LastIndexByte(text, ')')]
		}
		out = append(out, fmt.Sprintf("pid %d %s", pid, name))
	}
	return out
}

// exitCode is the process's exit code, 128 plus the signal when a signal ended it.
func exitCode(e *exec.ExitError) int {
	if ws, ok := e.Sys().(syscall.WaitStatus); ok && ws.Signaled() {
		return 128 + int(ws.Signal())
	}
	return e.ExitCode()
}

// lineScan remembers the text after prefix on the last line that starts with it.
type lineScan struct {
	prefix  string
	partial []byte
	value   string
}

func (l *lineScan) Write(p []byte) (int, error) {
	l.partial = append(l.partial, p...)
	for {
		i := bytes.IndexByte(l.partial, '\n')
		if i < 0 {
			return len(p), nil
		}
		l.line(string(l.partial[:i]))
		l.partial = l.partial[i+1:]
	}
}

func (l *lineScan) flush() {
	if len(l.partial) > 0 {
		l.line(string(l.partial))
		l.partial = nil
	}
}

func (l *lineScan) line(s string) {
	if v, ok := strings.CutPrefix(s, l.prefix); ok {
		l.value = strings.TrimSpace(v)
	}
}

// appendRecord appends rec to the file as one JSON line, with one write on a file opened for appending.
func appendRecord(path string, rec map[string]any) error {
	line, err := json.Marshal(rec)
	if err != nil {
		return err
	}
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_APPEND|os.O_CREATE, 0o644)
	if err != nil {
		return err
	}
	if _, err := f.Write(append(line, '\n')); err != nil {
		f.Close()
		return err
	}
	return f.Close()
}

func nonNil(s []string) []string {
	if s == nil {
		return []string{}
	}
	return s
}

// logDir is $NEVR_TEST_LOG_DIR, else test-runs/tools-tests in the work root, as the Python runner has it.
func logDir(getenv func(string) string) string {
	if d := getenv("NEVR_TEST_LOG_DIR"); d != "" {
		return d
	}
	root := getenv("NEVR_WORK_ROOT")
	if root == "" {
		root = filepath.Join(getenv("HOME"), "src", "nevr-runtime-work")
	}
	return filepath.Join(root, "test-runs", "tools-tests")
}

// newStem is <UTC stamp to the microsecond>-<pid>-<16 random hex digits>, the Python runner's stem without the commit.
func newStem() (string, error) {
	var salt [8]byte
	if _, err := rand.Read(salt[:]); err != nil {
		return "", err
	}
	return fmt.Sprintf("%s-%d-%s", time.Now().UTC().Format("20060102T150405.000000Z"), os.Getpid(), hex.EncodeToString(salt[:])), nil
}

// lookPath finds an executable file called name in the directories of path (a PATH value).
func lookPath(name, path string) (string, error) {
	for _, dir := range filepath.SplitList(path) {
		if dir == "" {
			continue
		}
		candidate := filepath.Join(dir, name)
		if fi, err := os.Stat(candidate); err == nil && fi.Mode().IsRegular() && fi.Mode().Perm()&0o111 != 0 {
			return candidate, nil
		}
	}
	return "", fmt.Errorf("no executable %q in PATH", name)
}

// event is the part of a test2json line (and of go's build events) this tool reads.
type event struct {
	Action     string
	Package    string
	ImportPath string
	Test       string
	Output     string
}

// collector reads the stream as it is written: which tests passed, failed and were skipped, and the output of the
// packages that failed.
type collector struct {
	partial        []byte
	passed         int
	failedTests    []string // "package Test"
	failedPackages []string
	skipped        []string // "package Test: reason"
	packages       map[string]bool
	output         map[string]*strings.Builder // of a package, by package or import path
	testOutput     map[string][]string         // the last lines of each test, by "package Test"
}

const keptTestLines = 6

func (c *collector) Write(p []byte) (int, error) {
	c.partial = append(c.partial, p...)
	for {
		i := bytes.IndexByte(c.partial, '\n')
		if i < 0 {
			return len(p), nil
		}
		c.line(c.partial[:i])
		c.partial = c.partial[i+1:]
	}
}

func (c *collector) flush() {
	if len(c.partial) > 0 {
		c.line(c.partial)
		c.partial = nil
	}
}

func (c *collector) line(b []byte) {
	var e event
	if json.Unmarshal(b, &e) != nil {
		return // go's own notices are in the stream file, not in the count
	}
	if c.packages == nil {
		c.packages, c.output, c.testOutput = map[string]bool{}, map[string]*strings.Builder{}, map[string][]string{}
	}
	key := e.Package
	if key == "" {
		key = strings.Fields(e.ImportPath + " ")[0] // "p [p.test]" is package p
	}
	if key != "" {
		c.packages[key] = true
	}
	switch e.Action {
	case "output", "build-output":
		if e.Test == "" && key != "" {
			if c.output[key] == nil {
				c.output[key] = &strings.Builder{}
			}
			c.output[key].WriteString(e.Output)
		} else if e.Test != "" {
			k := e.Package + " " + e.Test
			lines := append(c.testOutput[k], strings.TrimRight(e.Output, "\n"))
			if len(lines) > keptTestLines {
				lines = lines[len(lines)-keptTestLines:]
			}
			c.testOutput[k] = lines
		}
	case "pass":
		if e.Test != "" {
			c.passed++
		}
	case "skip":
		if e.Test != "" {
			c.skipped = append(c.skipped, e.Package+" "+e.Test+": "+skipReason(c.testOutput[e.Package+" "+e.Test]))
		}
	case "fail":
		if e.Test != "" {
			c.failedTests = append(c.failedTests, e.Package+" "+e.Test)
		} else {
			c.failedPackages = append(c.failedPackages, e.Package)
		}
	}
}

// skipReason is the text the test gave t.Skip: the last output line of the form "file.go:12: reason" (go's own
// "--- SKIP" and "=== RUN" lines are not it).
func skipReason(lines []string) string {
	for i := len(lines) - 1; i >= 0; i-- {
		l := strings.TrimSpace(lines[i])
		if l == "" || strings.HasPrefix(l, "---") || strings.HasPrefix(l, "===") {
			continue
		}
		return l
	}
	return "no reason given"
}

// report prints what failed and what was skipped: each failing test, each failing package with the output it had, each
// skipped test with its reason, then the counts.
func (c *collector) report(w io.Writer, code int, stream string) {
	sort.Strings(c.failedTests)
	for _, t := range c.failedTests {
		fmt.Fprintf(w, "test-tools: go FAIL %s\n", t)
	}
	withFailedTest := map[string]bool{}
	for _, t := range c.failedTests {
		withFailedTest[strings.Fields(t)[0]] = true
	}
	seen := map[string]bool{}
	for _, p := range c.failedPackages {
		if seen[p] || withFailedTest[p] { // a package that failed because a test did is already named by the test
			continue
		}
		seen[p] = true
		fmt.Fprintf(w, "test-tools: go FAIL %s (package did not build, or failed outside a test)\n", p)
		if out := c.output[p]; out != nil {
			sc := bufio.NewScanner(strings.NewReader(out.String()))
			for sc.Scan() {
				fmt.Fprintf(w, "    %s\n", sc.Text())
			}
		}
	}
	sort.Strings(c.skipped)
	for _, s := range c.skipped {
		fmt.Fprintf(w, "test-tools: go SKIP %s\n", s)
	}
	skips := ""
	if len(c.skipped) > 0 {
		skips = fmt.Sprintf(", %d skipped", len(c.skipped))
	}
	switch {
	case code != 0 && len(c.failedTests) == 0 && len(seen) == 0:
		fmt.Fprintf(w, "test-tools: go FAIL go test exited %d and the stream names no failing test; read %s\n", code, stream)
	case code == 0:
		fmt.Fprintf(w, "test-tools: go tests passed: %d tests passed%s in %d %s; stream %s\n", c.passed, skips, len(c.packages), plural("package", len(c.packages)), stream)
		return
	}
	fmt.Fprintf(w, "test-tools: go tests failed: %d tests passed%s, %d failed, %d %s failed; exit %d; stream %s\n",
		c.passed, skips, len(c.failedTests), len(seen), plural("package", len(seen)), code, stream)
}

func plural(word string, n int) string {
	if n == 1 {
		return word
	}
	return word + "s"
}

// stdinFor is the input the Python half is given: the caller's, except a terminal. The half runs in a process group that is
// not the terminal's foreground group, so its first read of the terminal would stop it with SIGTTIN and the run would never
// end; /dev/null gives that read an end of file instead (a nil Reader is /dev/null to os/exec).
func stdinFor(in io.Reader) io.Reader {
	if f, ok := in.(*os.File); ok && isTerminal(f) {
		return nil
	}
	return in
}

// isTerminal reports whether f is a terminal (the TCGETS ioctl succeeds on it).
func isTerminal(f *os.File) bool {
	var t syscall.Termios
	_, _, errno := syscall.Syscall(syscall.SYS_IOCTL, f.Fd(), syscall.TCGETS, uintptr(unsafe.Pointer(&t)))
	return errno == 0
}

// lockName is the machine-wide lock's file in the log directory, the Python runner's (LOCK_NAME).
const lockName = "run.lock"

// wholeSuite is true for the Python command that runs the whole suite: the runner's script and nothing after it. Any
// argument may name test paths, which the runner answers without a lock, so a run with arguments takes none here.
func wholeSuite(pyCmd []string) bool { return len(pyCmd) <= 2 }

// heldLock is the lock file held open: the flock is the open file description's, so it ends with release or the process.
type heldLock struct{ f *os.File }

func (l *heldLock) release() {
	if l == nil || l.f == nil {
		return
	}
	l.f.Truncate(0) // the file names no holder once the run ends
	l.f.Close()
}

// takeLock takes the machine-wide lock and writes the holder into the file, waiting up to $NEVR_TEST_LOCK_WAIT_S (5400 s).
// It returns the exit code of a run that must not start: 2 for a wait that is not a number or a lock file that cannot be
// opened, 75 when the lock was not free in time, 130 when this tool was interrupted while it waited.
func takeLock(ctx context.Context, opts options, dir string, getenv func(string) string, stderr io.Writer, log *slog.Logger) (*heldLock, int) {
	wait := opts.lockWait
	if v := getenv("NEVR_TEST_LOCK_WAIT_S"); v != "" {
		sec, err := strconv.ParseFloat(v, 64)
		if err != nil || !(sec > 0) || sec > 1e7 {
			fmt.Fprintf(stderr, "test-tools: FAIL $NEVR_TEST_LOCK_WAIT_S=%q is not a positive number of seconds\n", v)
			return nil, exitSetup
		}
		wait = time.Duration(sec * float64(time.Second))
	}
	path := filepath.Join(dir, lockName)
	f, err := os.OpenFile(path, os.O_RDWR|os.O_CREATE, 0o644) // close-on-exec: the Python half does not inherit it
	if err != nil {
		fmt.Fprintf(stderr, "test-tools: FAIL cannot take the machine-wide test lock in %s: %v\n", dir, err)
		log.Error("no lock file", "path", path, "error", err.Error())
		return nil, exitSetup
	}
	start := time.Now()
	contended := false
	if err := syscall.Flock(int(f.Fd()), syscall.LOCK_EX|syscall.LOCK_NB); err != nil {
		contended = true
		fmt.Fprintf(stderr, "test-tools: waiting for the machine-wide test lock %s (holder: %s); giving up after %s\n", path, holderText(path), wait)
		got := make(chan error, 1)
		go func() { got <- syscall.Flock(int(f.Fd()), syscall.LOCK_EX) }()
		timer := time.NewTimer(wait)
		defer timer.Stop()
		select {
		case err = <-got:
		case <-timer.C:
			abandon(f, got)
			fmt.Fprintf(stderr, "test-tools: FAIL the machine-wide test lock %s was not free after %s (the deadline is $NEVR_TEST_LOCK_WAIT_S); it is held by %s, so this run did not start\n", path, wait, holderText(path))
			log.Error("lock timeout", "path", path, "wait", wait.String())
			return nil, 75
		case <-ctx.Done():
			abandon(f, got)
			fmt.Fprintf(stderr, "test-tools: FAIL interrupted while waiting for the machine-wide test lock %s, so this run did not start\n", path)
			return nil, 130
		}
		if err != nil {
			f.Close()
			fmt.Fprintf(stderr, "test-tools: FAIL cannot take the machine-wide test lock %s: %v\n", path, err)
			return nil, exitSetup
		}
	}
	holder, _ := json.Marshal(map[string]any{"pid": os.Getpid(), "tool": "test-go-tools", "start": time.Now().UTC().Format(time.RFC3339)})
	f.Truncate(0)
	f.WriteAt(holder, 0)
	log.Info("test lock held across both halves", "path", path, "contended", contended, "wait_ms", time.Since(start).Milliseconds())
	return &heldLock{f}, 0
}

// abandon gives up a lock request whose flock call is still blocked: the descriptor is closed by the goroutine once the call
// returns, so a lock won after the deadline is dropped at once.
func abandon(f *os.File, got <-chan error) {
	go func() {
		<-got
		f.Close()
	}()
}

// holderText is what the lock file says about its holder, for the line a waiting run prints.
func holderText(path string) string {
	raw, err := os.ReadFile(path)
	if err != nil || len(bytes.TrimSpace(raw)) == 0 {
		return "unknown"
	}
	return string(bytes.TrimSpace(raw))
}
