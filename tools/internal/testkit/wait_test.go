package testkit

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"syscall"
	"testing"
	"testing/synctest"
	"time"
)

func TestWaitForReturnsAtOnceWhenTheConditionHolds(t *testing.T) {
	t.Parallel()
	synctest.Test(t, func(t *testing.T) {
		ft := newFakeT(context.Background(), t.TempDir())
		start := time.Now()
		WaitFor(ft, "already true", func() bool { return true })
		if got := time.Since(start); got != 0 {
			t.Errorf("waited %v on a true condition", got)
		}
	})
}

func TestWaitForReturnsAsSoonAsTheConditionTurnsTrue(t *testing.T) {
	t.Parallel()
	synctest.Test(t, func(t *testing.T) {
		ft := newFakeT(context.Background(), t.TempDir())
		var flag atomic.Bool
		go func() {
			time.Sleep(3 * time.Second) // the event, on the bubble's clock
			flag.Store(true)
		}()
		start := time.Now()
		WaitFor(ft, "the flag", flag.Load)
		got := time.Since(start)
		if got < 3*time.Second || got > 3*time.Second+50*time.Millisecond { // a short poll interval, written out so a change to it is noticed
			t.Errorf("returned after %v, want within 50ms of 3s", got)
		}
	})
}

func TestWaitForFailsAtTheDefaultBudgetAndNamesWhatItWaitedFor(t *testing.T) {
	t.Parallel()
	synctest.Test(t, func(t *testing.T) {
		ft := newFakeT(context.Background(), t.TempDir())
		start := time.Now()
		msg := fatalOf(func() { WaitFor(ft, "the server to listen", func() bool { return false }) })
		mustContain(t, "message", msg, "the server to listen", "30s")
		if got := time.Since(start); got != 30*time.Second || DefaultBudget != 30*time.Second {
			t.Errorf("gave up after %v (DefaultBudget %v), want exactly 30s", got, DefaultBudget)
		}
	})
}

func TestWaitForStopsWhenTheTestsContextEnds(t *testing.T) {
	t.Parallel()
	synctest.Test(t, func(t *testing.T) {
		ctx, cancel := context.WithCancel(context.Background())
		ft := newFakeT(ctx, t.TempDir())
		go func() {
			time.Sleep(2 * time.Second)
			cancel()
		}()
		start := time.Now()
		msg := fatalOf(func() { WaitFor(ft, "never", func() bool { return false }) })
		mustContain(t, "message", msg, "never", "canceled")
		if got := time.Since(start); got != 2*time.Second {
			t.Errorf("stopped after %v, want 2s", got)
		}
	})
}

func TestWithinSetsTheBudgetAndTheEarlierDeadlineWins(t *testing.T) {
	t.Parallel()
	synctest.Test(t, func(t *testing.T) {
		parent, cancel := context.WithTimeout(context.Background(), 4*time.Second)
		defer cancel()
		ft := newFakeT(parent, t.TempDir())
		start := time.Now()
		msg := fatalOf(func() { WaitFor(Within(ft, time.Minute), "x", func() bool { return false }) })
		if msg == "" || time.Since(start) != 4*time.Second {
			t.Errorf("parent's 4s deadline did not win over Within(1m): %q after %v", msg, time.Since(start))
		}

		ft2 := newFakeT(context.Background(), t.TempDir())
		start = time.Now()
		msg = fatalOf(func() { WaitFor(Within(ft2, 7*time.Second), "y", func() bool { return false }) })
		mustContain(t, "message", msg, "y", "7s")
		if got := time.Since(start); got != 7*time.Second {
			t.Errorf("Within(7s) gave up after %v", got)
		}
		// A budget longer than the default is honoured: Within is how a slow wait asks for it.
		ft3 := newFakeT(context.Background(), t.TempDir())
		start = time.Now()
		msg = fatalOf(func() { WaitFor(Within(ft3, 2*time.Minute), "z", func() bool { return false }) })
		mustContain(t, "message", msg, "z", "2m0s")
		if got := time.Since(start); got != 2*time.Minute {
			t.Errorf("Within(2m) gave up after %v", got)
		}
		ft.finish()
		ft2.finish()
		ft3.finish()
	})
}

// A condition that turns true at the very instant the budget ends is true: WaitFor looks once more before it fails. The
// poll tick and the deadline are both due then, and select picks either, so many rounds make the order irrelevant.
func TestWaitForTakesAConditionThatTurnsTrueAtTheDeadline(t *testing.T) {
	t.Parallel()
	for range 40 {
		synctest.Test(t, func(t *testing.T) {
			w := Within(newFakeT(context.Background(), t.TempDir()), time.Second)
			ctx := w.Context()
			if msg := fatalOf(func() { WaitFor(w, "at the deadline", func() bool { return ctx.Err() != nil }) }); msg != "" {
				t.Errorf("failed although the condition held at the deadline: %s", msg)
			}
		})
	}
}

func mkfifo(t *testing.T) string {
	t.Helper()
	p := filepath.Join(t.TempDir(), "fifo")
	if err := syscall.Mkfifo(p, 0o600); err != nil {
		t.Fatal(err)
	}
	return p
}

// holdWriter opens the fifo's write end, writes data in one write, and keeps the end open until the test ends, so
// bytes left in the pipe stay readable by a later wait.
func holdWriter(t *testing.T, fifo, data string) {
	t.Helper()
	opened := make(chan struct{})
	go func() {
		f, err := os.OpenFile(fifo, os.O_WRONLY, 0)
		if err != nil {
			t.Error(err)
			close(opened)
			return
		}
		f.WriteString(data)
		close(opened)
		<-t.Context().Done()
		f.Close()
	}()
	t.Cleanup(func() { <-opened })
}

func TestWaitLinesReturnsTheFirstNLinesWhenTheyArrive(t *testing.T) {
	t.Parallel()
	fifo := Fifo(t)
	holdWriter(t, fifo, "alpha\nbeta\ngamma\n")
	got := WaitLines(t, fifo, 2)
	if strings.Join(got, "|") != "alpha|beta" {
		t.Errorf("lines = %q, want alpha|beta", got)
	}
}

func TestWaitLinesReadsNoByteBeyondTheLastLineItWasAskedFor(t *testing.T) {
	t.Parallel()
	fifo := Fifo(t)
	holdWriter(t, fifo, "one\ntwo\nthree\n")
	first := WaitLines(t, fifo, 1)
	second := WaitLines(t, fifo, 2)
	if strings.Join(first, "|") != "one" || strings.Join(second, "|") != "two|three" {
		t.Errorf("first %q, second %q; the second call lost data the first read past its line", first, second)
	}
}

func TestWaitLinesFailsAtTheDeadlineWithTheLinesItHad(t *testing.T) {
	t.Parallel()
	fifo := Fifo(t)
	holdWriter(t, fifo, "done\npartial")
	ctx, cancel := context.WithTimeout(t.Context(), 200*time.Millisecond)
	defer cancel()
	ft := newFakeT(ctx, t.TempDir())
	msg := fatalOf(func() { WaitLines(ft, fifo, 2) })
	mustContain(t, "message", msg, "1 of 2", "done", "partial", "context deadline exceeded") // the context's reason, not the read's "i/o timeout"
}

func TestWaitLinesFailsAtTheDeadlineWhenNothingIsWritten(t *testing.T) {
	t.Parallel()
	fifo := Fifo(t)
	ctx, cancel := context.WithTimeout(t.Context(), 100*time.Millisecond)
	defer cancel()
	ft := newFakeT(ctx, t.TempDir())
	msg := fatalOf(func() { WaitLines(ft, fifo, 1) })
	mustContain(t, "message", msg, "0 of 1")
}

func TestWaitLinesDropsACarriageReturnBeforeTheNewline(t *testing.T) {
	t.Parallel()
	fifo := Fifo(t)
	holdWriter(t, fifo, "dos\r\nunix\n")
	if got := WaitLines(t, fifo, 2); strings.Join(got, "|") != "dos|unix" {
		t.Errorf("lines = %q, want dos|unix", got)
	}
}

func TestWaitLinesRefusesAPathThatIsNotAFifo(t *testing.T) {
	t.Parallel()
	regular := filepath.Join(t.TempDir(), "file")
	if err := os.WriteFile(regular, []byte("a\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	ft := newFakeT(t.Context(), t.TempDir())
	mustContain(t, "regular file", fatalOf(func() { WaitLines(ft, regular, 1) }), "not a fifo")
	mustContain(t, "missing file", fatalOf(func() { WaitLines(ft, regular+".missing", 1) }), ".missing")
}

func TestWaitLinesNeedsAtLeastOneLine(t *testing.T) {
	t.Parallel()
	ft := newFakeT(t.Context(), t.TempDir())
	mustContain(t, "message", fatalOf(func() { WaitLines(ft, Fifo(t), 0) }), "at least 1")
}

func TestFifoMakesANamedPipeBelowTheTestsDirectory(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	p := Fifo(&dirT{T: t, dir: dir})
	fi, err := os.Stat(p)
	if err != nil || fi.Mode()&os.ModeNamedPipe == 0 {
		t.Fatalf("Fifo(t) = %q: %v, mode %v", p, err, fi)
	}
	if !strings.HasPrefix(p, dir+string(filepath.Separator)) {
		t.Errorf("fifo %q is not below %q", p, dir)
	}
}
