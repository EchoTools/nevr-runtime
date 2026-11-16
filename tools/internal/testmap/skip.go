package testmap

import (
	"fmt"

	"github.com/EchoTools/nevr-runtime/tools/internal/testshelf"
)

// This file is where the shelf (tools/test_shelf.json, internal/testshelf) and the heavy classification (heavy.go) become
// one decision. A test id a land does not run is skipped for exactly one of two kinds:
//
//   - SkipShelved: the shelf names it. It is over the ceiling every test runner holds a test to, so no runner starts it,
//     `-heavy` included. It comes back only by being rewritten to run under the ceiling and removed from the shelf.
//   - SkipHeavy: heavy.go classifies it (the toolchain name rule, or a fastest run over HeavyBound) and the shelf does not
//     name it. A land skips it and `-heavy` runs it.
//
// The shelf is asked first, so an id both lists could name is a shelved one and heavy.go needs no entry for it;
// ValidateAgainstShelf refuses a Measured entry for a shelved id.

// The kinds of a Skip.
const (
	SkipShelved = "shelved"
	SkipHeavy   = "heavy"
)

// Skip is one id a run does not start, with the kind that skipped it and why.
type Skip struct {
	ID     string
	Kind   string // SkipShelved or SkipHeavy
	Rule   string // heavy only: RuleToolchain or RuleMeasurement
	Millis int64  // heavy only: the fastest recorded run, 0 when none is kept
	Date   string // shelved only: the day it was shelved
	Reason string // one line: why it is not run
}

// Plan splits ids, in their order, into the ids to run and the ids to skip. A shelved id is skipped whatever includeHeavy
// says; a heavy id is skipped unless includeHeavy. An id listed twice is refused: the plan of a run names a test once. A
// nil shelf shelves nothing.
func Plan(shelf *testshelf.Shelf, ids []string, includeHeavy bool) (run []string, skipped []Skip, err error) {
	seen := map[string]bool{}
	for _, id := range ids {
		if seen[id] {
			return nil, nil, fmt.Errorf("test %q is in the plan twice", id)
		}
		seen[id] = true
		if shelf != nil {
			if e, ok := shelf.Shelved(id); ok {
				skipped = append(skipped, Skip{ID: id, Kind: SkipShelved, Date: e.Date, Reason: e.Reason})
				continue
			}
		}
		if c := Classify(id); c.Heavy && !includeHeavy {
			skipped = append(skipped, Skip{ID: id, Kind: SkipHeavy, Rule: c.Rule, Millis: c.Millis, Reason: c.Reason})
			continue
		}
		run = append(run, id)
	}
	return run, skipped, nil
}

// ValidateAgainstShelf is Validate and one more rule: no id is in both lists. A Measured entry for an id the shelf names
// is refused, because the shelf already keeps it out of every run and a second, weaker entry would be a second answer.
func ValidateAgainstShelf(m *Map, measured []Measurement, shelf *testshelf.Shelf) error {
	if err := Validate(m, measured); err != nil {
		return err
	}
	if shelf == nil {
		return nil
	}
	for _, e := range measured {
		if _, ok := shelf.Shelved(e.ID); ok {
			return fmt.Errorf("%q is in the heavy measurements and on the shelf (%s): the shelf alone classifies it, remove the heavy entry", e.ID, testshelf.DefaultPath)
		}
	}
	return nil
}
