package gtestdeadline

import (
	"bytes"
	"context"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"
)

const (
	ceiling   = 400 * time.Millisecond
	underSecs = "0.15"
	overSecs  = "20"
	// killBudget is how long a run that kills a test may take: far under the program's own sleep.
	killBudget = 8 * time.Second
	scriptMode = 0o755
	failExit   = 3
	fifoMode   = 0o600
)

// program writes a shell script that plays a gtest binary and returns argv for it.
func program(t *testing.T, body string) []string {
	t.Helper()
	p := filepath.Join(t.TempDir(), "prog.sh")
	if err := os.WriteFile(p, []byte("#!/bin/sh\n"+body), scriptMode); err != nil {
		t.Fatal(err)
	}
	return []string{p}
}

func run(t *testing.T, argv []string) (int, string, string, time.Duration) {
	t.Helper()
	var out, errb bytes.Buffer
	start := time.Now()
	code := Run(context.Background(), ceiling, argv, os.Environ(), strings.NewReader(""), &out, &errb)
	return code, out.String(), errb.String(), time.Since(start)
}

func TestEachTestIsHeldToTheCeilingNotTheWholeProgram(t *testing.T) {
	// Four tests of 0.15 s each: 0.6 s in all, past the 0.4 s ceiling, and none of them over it.
	body := ""
	for _, n := range []string{"One", "Two", "Three", "Four"} {
		body += "echo '[ RUN      ] S." + n + "'; sleep " + underSecs + "; echo '[       OK ] S." + n + " (150 ms)'\n"
	}
	code, out, errb, _ := run(t, program(t, body))
	if code != 0 || !strings.Contains(out, "[       OK ] S.Four") || errb != "" {
		t.Errorf("exit %d, stdout %q, stderr %q", code, out, errb)
	}
}

func TestATestOverTheCeilingIsNamedAndTheProgramAndItsChildrenAreKilled(t *testing.T) {
	// The test's child holds the write end of a FIFO open for as long as it lives, so the read end sees end of file when the
	// child is gone: the test waits for that event, not for a time.
	fifo := filepath.Join(t.TempDir(), "child.fifo")
	if err := syscall.Mkfifo(fifo, fifoMode); err != nil {
		t.Fatal(err)
	}
	gone := make(chan struct{})
	go func() {
		defer close(gone)
		f, err := os.Open(fifo) // blocks until the child opens its end
		if err != nil {
			return
		}
		defer f.Close()
		_, _ = io.Copy(io.Discard, f)
	}()
	body := "echo '[ RUN      ] S.Fast'; echo '[       OK ] S.Fast (0 ms)'; echo '[ RUN      ] S.Slow'\n" +
		"sleep " + overSecs + " > " + fifo + " &\nwait\n"
	code, out, errb, took := run(t, program(t, body))
	if code != ExitOverCeiling {
		t.Errorf("exit %d, want %d", code, ExitOverCeiling)
	}
	if !strings.Contains(errb, "FAIL S.Slow ran longer than 400ms") || strings.Contains(errb, "S.Fast") {
		t.Errorf("stderr %q names the wrong test", errb)
	}
	if !strings.Contains(out, "[ RUN      ] S.Slow") {
		t.Errorf("the output up to the kill was not passed on: %q", out)
	}
	if took > killBudget {
		t.Errorf("the run took %v: the test was not killed at the ceiling", took)
	}
	select {
	case <-gone:
	case <-time.After(killBudget):
		t.Error("the test's child is still alive after the kill")
	}
}

func TestAProgramWithNoTestLinesIsOneTest(t *testing.T) {
	code, _, errb, took := run(t, program(t, "sleep "+overSecs+"\n"))
	if code != ExitOverCeiling || !strings.Contains(errb, "prog.sh ran longer than") || took > killBudget {
		t.Errorf("exit %d, stderr %q, took %v", code, errb, took)
	}
}

func TestTheProgramsOwnExitCodeIsKept(t *testing.T) {
	code, out, _, _ := run(t, program(t, "echo '[ RUN      ] S.A'; echo '[  FAILED  ] S.A (1 ms)'; exit "+strconv.Itoa(failExit)+"\n"))
	if code != failExit || !strings.Contains(out, "[  FAILED  ] S.A") {
		t.Errorf("exit %d, stdout %q", code, out)
	}
}

func TestAProgramThatCannotBeStartedExits127(t *testing.T) {
	code, _, errb, _ := run(t, []string{filepath.Join(t.TempDir(), "absent")})
	if code != ExitNotStarted || !strings.Contains(errb, "cannot start") {
		t.Errorf("exit %d, stderr %q", code, errb)
	}
}
