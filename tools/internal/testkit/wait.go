package testkit

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"time"
)

// DefaultBudget is how long WaitFor and WaitLines wait when the test's context has no deadline of its own. A hung wait
// then fails the test with what it waited for, well before go test's own timeout kills the binary with no explanation.
const DefaultBudget = 30 * time.Second

// pollInterval is how often WaitFor looks at its condition. It is a ticker's period, not a pause in the test: the wait
// ends within one interval of the condition becoming true.
const pollInterval = 5 * time.Millisecond

func waitContext(t T) (context.Context, context.CancelFunc) {
	ctx := t.Context()
	if _, ok := ctx.Deadline(); ok {
		return ctx, func() {}
	}
	return context.WithTimeout(ctx, DefaultBudget)
}

// WaitFor blocks until cond returns true, checking it at once and then every few milliseconds, and fails the test,
// naming what, when the deadline passes: DefaultBudget, or the deadline of t's context (see Within). Use it for state
// that cannot be waited on directly, such as a file appearing or a /proc entry; a wait on a channel, a process or a fifo
// is a select, cmd.Wait or WaitLines, and a timeout under test is a testing/synctest bubble.
func WaitFor(t T, what string, cond func() bool) {
	t.Helper()
	ctx, cancel := waitContext(t)
	defer cancel()
	if cond() {
		return
	}
	start := time.Now()
	tick := time.NewTicker(pollInterval)
	defer tick.Stop()
	for {
		select {
		case <-tick.C:
			if cond() {
				return
			}
		case <-ctx.Done():
			if cond() {
				return
			}
			t.Fatalf("testkit.WaitFor: %s: not true after %v: %v", what, time.Since(start), ctx.Err())
		}
	}
}

// Fifo makes a named pipe below t.TempDir() and returns its path.
func Fifo(t T) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "fifo")
	if err := syscall.Mkfifo(path, 0o600); err != nil {
		t.Fatalf("testkit.Fifo: %v", err)
	}
	return path
}

// WaitLines blocks until n lines have been written to the fifo, and returns them without their newlines. It opens the
// fifo for reading and writing, so it neither waits for a writer to open it nor sees end of file when a writer closes,
// and it reads one byte at a time, so it takes nothing past the newline of the nth line: a later WaitLines on the same
// fifo gets the rest, as long as a writer still holds the fifo open. It fails the test, naming the fifo and the lines
// it had, when the deadline passes (DefaultBudget, or the deadline of t's context; see Within).
func WaitLines(t T, fifo string, n int) []string {
	t.Helper()
	if n < 1 {
		t.Fatalf("testkit.WaitLines: n is %d, want at least 1", n)
	}
	fi, err := os.Stat(fifo)
	if err != nil {
		t.Fatalf("testkit.WaitLines: %v", err)
	}
	if fi.Mode()&os.ModeNamedPipe == 0 {
		t.Fatalf("testkit.WaitLines: %s is not a fifo (mode %v)", fifo, fi.Mode())
	}
	ctx, cancel := waitContext(t)
	defer cancel()
	f, err := os.OpenFile(fifo, os.O_RDWR, 0)
	if err != nil {
		t.Fatalf("testkit.WaitLines: %v", err)
	}
	defer f.Close()
	stop := context.AfterFunc(ctx, func() { f.SetReadDeadline(time.Now()) })
	defer stop()

	var lines []string
	var partial []byte
	var b [1]byte
	for len(lines) < n {
		if _, err := f.Read(b[:]); err != nil {
			why := err
			if ctx.Err() != nil { // the read was cut short by the deadline set when the context ended
				why = ctx.Err()
			}
			t.Fatalf("testkit.WaitLines: %s: have %d of %d lines (%q, then %q): %v", fifo, len(lines), n, lines, partial, why)
		}
		if b[0] == '\n' {
			lines = append(lines, strings.TrimSuffix(string(partial), "\r"))
			partial = partial[:0]
			continue
		}
		partial = append(partial, b[0])
	}
	return lines
}
