package testkit

import (
	"context"
	"fmt"
	"strings"
	"sync"
	"testing"
)

// fakeT is a T whose Fatalf panics with a fatalCall instead of ending the goroutine, so a test can assert that a helper
// failed, and with what message. Its context is the one the test gives it, so a wait can be run under testing/synctest
// against the fake clock.
type fakeT struct {
	ctx      context.Context
	dir      string
	mu       sync.Mutex
	cleanups []func()
}

type fatalCall struct{ msg string }

func newFakeT(ctx context.Context, dir string) *fakeT { return &fakeT{ctx: ctx, dir: dir} }

func (f *fakeT) Helper()                  {}
func (f *fakeT) Context() context.Context { return f.ctx }
func (f *fakeT) TempDir() string          { return f.dir }
func (f *fakeT) Logf(string, ...any)      {}
func (f *fakeT) Fatalf(format string, args ...any) {
	panic(fatalCall{fmt.Sprintf(format, args...)})
}
func (f *fakeT) Cleanup(fn func()) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.cleanups = append(f.cleanups, fn)
}

// finish runs the cleanups last-in first-out, as testing does.
func (f *fakeT) finish() {
	f.mu.Lock()
	cs := f.cleanups
	f.cleanups = nil
	f.mu.Unlock()
	for i := len(cs) - 1; i >= 0; i-- {
		cs[i]()
	}
}

// fatalOf runs fn and returns the message of the Fatalf it made, or "" when it returned normally.
func fatalOf(fn func()) (msg string) {
	defer func() {
		if r := recover(); r != nil {
			fc, ok := r.(fatalCall)
			if !ok {
				panic(r)
			}
			msg = fc.msg
		}
	}()
	fn()
	return ""
}

func mustContain(t *testing.T, what, got string, wants ...string) {
	t.Helper()
	for _, want := range wants {
		if !strings.Contains(got, want) {
			t.Errorf("%s = %q, want it to contain %q", what, got, want)
		}
	}
}
