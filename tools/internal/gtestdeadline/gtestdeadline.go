// Package gtestdeadline runs a gtest program and holds each test in it to a deadline.
//
// gtest has no per-test timeout, and a ctest or timeout(1) limit bounds the whole program, which is the wrong unit for a
// binary of hundreds of tests (test_behavioral runs 560). gtest prints "[ RUN      ] Suite.Test" when a test starts and
// "[       OK ] ...", "[  FAILED  ] ..." or "[  SKIPPED ] ..." when it ends, flushing after each line; Run reads that stream as
// it is written, and when one test has been running for the ceiling it names the test and kills the program's process group.
// A program that prints no gtest lines is bounded as a whole by the ceiling (its "test" is the program).
package gtestdeadline

import (
	"bufio"
	"context"
	"fmt"
	"io"
	"os/exec"
	"regexp"
	"sync"
	"syscall"
	"time"
)

// ExitOverCeiling is the exit code of a run that killed a test over the ceiling (timeout(1)'s code for the same event).
const ExitOverCeiling = 124

// ExitNotStarted is the exit code of a run whose program could not be started.
const ExitNotStarted = 127

// initialLine and maxLine are the line buffer's first size and its limit: a gtest line is short, and a line past the limit
// ends the reading (the program is still waited for and its exit code returned).
const (
	initialLine = 64 << 10
	maxLine     = 1 << 20
)

var (
	startRE = regexp.MustCompile(`^\[ RUN      \] (\S+)`)
	endRE   = regexp.MustCompile(`^\[ +(?:OK|FAILED|SKIPPED) +\] (\S+)`)
)

// lockedWriter serialises writes: the program's standard error is copied by the exec package's goroutine while the deadline
// timer writes its line.
type lockedWriter struct {
	mu sync.Mutex
	w  io.Writer
}

func (l *lockedWriter) Write(p []byte) (int, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.w.Write(p)
}

// Run starts argv, copies its output to stdout, and returns its exit code, or ExitOverCeiling when a test ran past the
// ceiling and the program was killed (the line "gtest-deadline: FAIL <test> ran longer than <ceiling>" goes to stderr).
// The program has its own process group, so the kill takes its children (wine and what it starts) with it.
func Run(ctx context.Context, ceiling time.Duration, argv []string, env []string, stdin io.Reader, stdout, stderr io.Writer) int {
	stderr = &lockedWriter{w: stderr} // the program's stderr and this package's own lines share one writer
	cmd := exec.Command(argv[0], argv[1:]...)
	cmd.Env, cmd.Stdin, cmd.Stderr = env, stdin, stderr
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	pipe, err := cmd.StdoutPipe()
	if err != nil {
		fmt.Fprintf(stderr, "gtest-deadline: FAIL cannot read %s's output: %v\n", argv[0], err)
		return ExitNotStarted
	}
	if err := cmd.Start(); err != nil {
		fmt.Fprintf(stderr, "gtest-deadline: FAIL cannot start %s: %v\n", argv[0], err)
		return ExitNotStarted
	}
	kill := func() { _ = syscall.Kill(-cmd.Process.Pid, syscall.SIGKILL) }
	var mu sync.Mutex
	var timer *time.Timer
	var pending sync.WaitGroup // one for each timer that was armed and has neither been stopped nor finished firing
	over := ""
	cancel := func() { // mu held
		if timer != nil && timer.Stop() {
			pending.Done()
		}
		timer = nil
	}
	arm := func(test string) {
		mu.Lock()
		defer mu.Unlock()
		cancel()
		pending.Add(1)
		timer = time.AfterFunc(ceiling, func() {
			defer pending.Done()
			mu.Lock()
			over = test
			mu.Unlock()
			fmt.Fprintf(stderr, "gtest-deadline: FAIL %s ran longer than %s: the program is killed\n", test, ceiling)
			kill()
		})
	}
	disarm := func() {
		mu.Lock()
		defer mu.Unlock()
		cancel()
	}
	// A program that prints no test line is one test.
	arm(argv[0])
	stop := context.AfterFunc(ctx, kill)
	defer stop()
	sc := bufio.NewScanner(pipe)
	sc.Buffer(make([]byte, 0, initialLine), maxLine)
	for sc.Scan() {
		line := sc.Text()
		fmt.Fprintln(stdout, line)
		if m := startRE.FindStringSubmatch(line); m != nil {
			arm(m[1])
		} else if endRE.MatchString(line) {
			arm(argv[0]) // between tests the program as a whole is what is running
		}
	}
	_, _ = io.Copy(io.Discard, pipe)
	waitErr := cmd.Wait()
	disarm()
	pending.Wait() // a timer that is firing has written its line and killed the group before the result is read
	mu.Lock()
	defer mu.Unlock()
	if over != "" {
		return ExitOverCeiling
	}
	if waitErr == nil {
		return 0
	}
	if ee, ok := waitErr.(*exec.ExitError); ok {
		return ee.ExitCode()
	}
	fmt.Fprintf(stderr, "gtest-deadline: FAIL %s: %v\n", argv[0], waitErr)
	return ExitNotStarted
}
