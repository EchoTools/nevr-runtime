// Command test-go-tools runs the Go tests of the tools module and keeps the run: `go test -json` over the module (or
// the packages given), its stream written byte for byte to a file in the test-run log directory, the failing tests
// named on standard output, and go's exit code returned. `just test-tools` runs it after the Python runner (ADR 0044).
//
// The stream is <log dir>/<UTC stamp>-<pid>-<random>-go.jsonl, in $NEVR_TEST_LOG_DIR, else test-runs/tools-tests in the
// work root ($NEVR_WORK_ROOT, else ~/src/nevr-runtime-work): beside the Python runner's <stem>.log and runs.jsonl. Exit
// codes: go's own; 2 when the run could not be kept or go could not be started; 74 when the tool's run log fails
// (internal/runlog).
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
	"path/filepath"
	"sort"
	"strings"
	"time"

	"github.com/EchoTools/nevr-runtime/tools/internal/runlog"
)

func main() {
	os.Exit(run(context.Background(), os.Args[1:], os.Getenv, os.Stdin, os.Stdout, os.Stderr))
}

func run(ctx context.Context, args []string, getenv func(string) string, _ io.Reader, stdout, stderr io.Writer) int {
	return runlog.Run("test-go-tools", getenv, stderr, func(log *slog.Logger) int {
		return testGo(ctx, args, getenv, stdout, stderr, log)
	})
}

// exitSetup is the exit code of a run that could not be made or kept.
const exitSetup = 2

func testGo(ctx context.Context, patterns []string, getenv func(string) string, stdout, stderr io.Writer, log *slog.Logger) int {
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
	stream, err := createStream(dir)
	if err != nil {
		fmt.Fprintf(stderr, "test-tools: FAIL cannot write the go test stream in %s: %v\n", dir, err)
		log.Error("no stream", "dir", dir, "error", err.Error())
		return exitSetup
	}
	log.Info("start", "go", goPath, "patterns", patterns, "stream", stream.Name())

	col := &collector{}
	cmd := exec.CommandContext(ctx, goPath, append([]string{"test", "-count=1", "-json"}, patterns...)...)
	cmd.Stdout = io.MultiWriter(stream, col)
	cmd.Stderr = stderr
	runErr := cmd.Run()
	col.flush()
	closeErr := stream.Close()

	code := 0
	var exitErr *exec.ExitError
	switch {
	case runErr == nil:
	case errors.As(runErr, &exitErr):
		code = exitErr.ExitCode()
		if code <= 0 { // killed by a signal
			code = 1
		}
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
	col.report(stdout, code, stream.Name())
	log.Info("end", "exit", code, "passed", col.passed, "failed", len(col.failedTests), "failed_packages", len(col.failedPackages),
		"packages", len(col.packages), "stream", stream.Name())
	return code
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

// createStream makes the stream's file, named by the UTC start time to the microsecond, the process id and 16 random
// hex digits, so no other run shares it.
func createStream(dir string) (*os.File, error) {
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return nil, err
	}
	var salt [8]byte
	if _, err := rand.Read(salt[:]); err != nil {
		return nil, err
	}
	name := fmt.Sprintf("%s-%d-%s-go.jsonl", time.Now().UTC().Format("20060102T150405.000000Z"), os.Getpid(), hex.EncodeToString(salt[:]))
	return os.OpenFile(filepath.Join(dir, name), os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o644)
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

// collector reads the stream as it is written: which tests passed and failed, and the output of the packages that failed.
type collector struct {
	partial        []byte
	passed         int
	failedTests    []string // "package Test"
	failedPackages []string
	packages       map[string]bool
	output         map[string]*strings.Builder // by package or import path
}

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
		c.packages, c.output = map[string]bool{}, map[string]*strings.Builder{}
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
		}
	case "pass":
		if e.Test != "" {
			c.passed++
		}
	case "fail":
		if e.Test != "" {
			c.failedTests = append(c.failedTests, e.Package+" "+e.Test)
		} else {
			c.failedPackages = append(c.failedPackages, e.Package)
		}
	case "build-fail":
		c.failedPackages = append(c.failedPackages, key)
	}
}

// report prints what failed: each failing test, each failing package with the output it had, then the counts.
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
	switch {
	case code != 0 && len(c.failedTests) == 0 && len(seen) == 0:
		fmt.Fprintf(w, "test-tools: go FAIL go test exited %d and the stream names no failing test; read %s\n", code, stream)
	case code == 0:
		fmt.Fprintf(w, "test-tools: go tests passed: %d tests passed in %d %s; stream %s\n", c.passed, len(c.packages), plural("package", len(c.packages)), stream)
		return
	}
	fmt.Fprintf(w, "test-tools: go tests failed: %d tests passed, %d failed, %d %s failed; exit %d; stream %s\n",
		c.passed, len(c.failedTests), len(seen), plural("package", len(seen)), code, stream)
}

func plural(word string, n int) string {
	if n == 1 {
		return word
	}
	return word + "s"
}
