package testmap

import (
	"fmt"
	"strings"
	"time"
)

// This file is the one place that says which test ids are heavy. A land runs only the fast unit tests for the changed
// code (AGENTS.md, "Builds"); a heavy id is one a land does not run, and `test-changed -heavy` runs it. The loader
// (tools/cmd/test-changed) asks Classify, and the generator (tools/cmd/test-map) checks with Validate that the list of
// measurements below still names ids the map can run, so a new test id is classified by the two rules and never by
// someone remembering it.
//
// An id is heavy when one of two rules says so:
//
//  1. The toolchain rule: it is the just recipe test-android, or a just recipe named test-quest-*. They build and run
//     the Android or Quest toolchains, whatever one run of them measures.
//  2. The measurement rule: its fastest recorded run is over HeavyBound. A recorded run is a test_end record of
//     $XDG_STATE_HOME/nevr-runtime/test-changed.jsonl; the fastest of an id's runs is the figure, so the load of the
//     machine does not make an id heavy. The id is added to Measured with that figure; Validate refuses an entry that
//     is not over the bound.
//
// Measured also holds the fastest run of the toolchain ids, for the line a land prints; those figures are not tested
// against the bound, because a cached run of them is short and a cold one builds the toolchain.
//
// An id that fails when it runs is not made heavy to keep it from failing a land: it stays out of Measured.

// HeavyBound is the fastest run an id may have and still be a land test.
const HeavyBound = 10 * time.Second

// The values of Classification.Rule.
const (
	RuleToolchain   = "toolchain"
	RuleMeasurement = "measurement"
)

// The just recipes the toolchain rule names: test-android, and every recipe that starts with questRecipePrefix.
const (
	androidRecipe     = "test-android"
	questRecipePrefix = "test-quest-"
)

// Measurement is the fastest recorded run of an id, in milliseconds.
type Measurement struct {
	ID     string
	Millis int64
}

// Measured are the fastest recorded runs, from the fastest test_end of each id in the run log of test-changed (gtest
// ids run as one batch and are not listed): the ids the measurement rule makes heavy, and the toolchain recipes seen. pytest:test_run_tool_tests and
// pytest:test_package_release are over the bound too and are not listed: a separate change disables them, and a heavy
// class must not hide a failure.
var Measured = []Measurement{
	{"host-seams", hostSeamsMs},
	{"pytest:test_launch_client", launchClientMs},
	{"go:internal/updatewiring", updateWiringMs},
	{"go:cmd/quest-repack-libs", questRepackLibsMs},
	{"pytest:test_run_tool_tests_memory", runToolTestsMemoryMs},
	{"just:test-android", androidMs},
	{"just:test-quest-hooks", questHooksMs},
	{"just:test-quest-data", questDataMs},
	{"just:test-quest-combat", questCombatMs},
	{"just:test-quest-shared", questSharedMs},
	{"just:test-quest-data-net", questDataNetMs},
	{"just:test-quest-integration", questIntegrationMs},
	{"just:test-quest-plugins", questPluginsMs},
}

// The fastest recorded runs, in milliseconds, of the ids in Measured.
const (
	hostSeamsMs          = 185563
	launchClientMs       = 31019
	updateWiringMs       = 19541
	questRepackLibsMs    = 10575
	runToolTestsMemoryMs = 10520
	androidMs            = 2718
	questHooksMs         = 54657
	questDataMs          = 37409
	questCombatMs        = 24571
	questSharedMs        = 13855
	questDataNetMs       = 9782
	questIntegrationMs   = 7827
	questPluginsMs       = 1536
)

// Classification is the answer Classify gives for one test id.
type Classification struct {
	Heavy  bool
	Rule   string // RuleToolchain or RuleMeasurement; empty for a unit test
	Millis int64  // the fastest recorded run, 0 when none is kept
	Reason string // one line: why a land does not run it
}

// Classify says whether id is heavy. It is total: every id is a unit test or a heavy one.
func Classify(id string) Classification {
	return classify(id, Measured)
}

func classify(id string, measured []Measurement) Classification {
	var ms int64
	for _, m := range measured {
		if m.ID == id {
			ms = m.Millis
		}
	}
	if kind, name, ok := strings.Cut(id, kindSeparator); ok && kind == KindJust &&
		(name == androidRecipe || strings.HasPrefix(name, questRecipePrefix)) {
		return Classification{Heavy: true, Rule: RuleToolchain, Millis: ms,
			Reason: "it builds and runs the Android or Quest toolchain (a cold run takes minutes), a heavy suite and not a land test"}
	}
	if ms > HeavyBound.Milliseconds() {
		return Classification{Heavy: true, Rule: RuleMeasurement, Millis: ms,
			Reason: fmt.Sprintf("its fastest recorded run is over the %d s bound of a land test", int64(HeavyBound/time.Second))}
	}
	return Classification{}
}

// Validate refuses a measurement list the rules cannot stand on: an id listed twice, an id the map cannot run (m may
// be nil: then only the shape is checked), a figure that is not positive, or an id the toolchain rule does not name
// whose figure is not over HeavyBound (a unit test listed as heavy).
func Validate(m *Map, measured []Measurement) error {
	seen := map[string]bool{}
	for _, e := range measured {
		if seen[e.ID] {
			return fmt.Errorf("heavy measurement for %q is listed twice", e.ID)
		}
		seen[e.ID] = true
		if m != nil {
			if _, err := m.Command(e.ID, DefaultPreset); err != nil {
				return fmt.Errorf("heavy measurement %q: %w", e.ID, err)
			}
		}
		if e.Millis <= 0 {
			return fmt.Errorf("measurement %q is %d ms: a recorded run is positive", e.ID, e.Millis)
		}
		if classify(e.ID, nil).Rule != RuleToolchain && e.Millis <= HeavyBound.Milliseconds() {
			return fmt.Errorf("heavy measurement %q is %d ms, not over the %d ms bound", e.ID, e.Millis, HeavyBound.Milliseconds())
		}
	}
	return nil
}

// HeavyID is a heavy test id with the classification that made it so.
type HeavyID struct {
	ID string
	Classification
}

// SplitHeavy splits ids, in their order, into the unit ids and the heavy ones. An id listed twice is refused: the
// plan of a run names a test once.
func SplitHeavy(ids []string) (unit []string, heavy []HeavyID, err error) {
	seen := map[string]bool{}
	for _, id := range ids {
		if seen[id] {
			return nil, nil, fmt.Errorf("test %q is in the plan twice", id)
		}
		seen[id] = true
		if c := Classify(id); c.Heavy {
			heavy = append(heavy, HeavyID{ID: id, Classification: c})
		} else {
			unit = append(unit, id)
		}
	}
	return unit, heavy, nil
}
