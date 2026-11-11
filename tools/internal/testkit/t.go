package testkit

import (
	"context"
	"time"
)

// T is what the helpers need of a test. *testing.T and *testing.B satisfy it; the kit's own tests give it a T whose
// Fatalf can be observed and whose context runs on the fake clock of testing/synctest.
type T interface {
	Helper()
	Context() context.Context
	TempDir() string
	Cleanup(func())
	Fatalf(format string, args ...any)
	Logf(format string, args ...any)
}

type within struct {
	T
	ctx context.Context
}

func (w within) Context() context.Context { return w.ctx }

// Within returns t with a context that ends d after now, or when t's context ends, whichever is first. Pass it to
// WaitFor or WaitLines to give one wait a budget other than DefaultBudget. The context is released when t's test ends.
func Within(t T, d time.Duration) T {
	t.Helper()
	ctx, cancel := context.WithTimeout(t.Context(), d)
	t.Cleanup(cancel)
	return within{T: t, ctx: ctx}
}
