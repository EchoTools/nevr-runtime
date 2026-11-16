package testmap

import (
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/testshelf"
)

// The shelf and heavy.go are one classification: an id is in one list or the other, and -heavy runs a heavy id but never a
// shelved one.
func TestTheShelfAndTheHeavyListAreOneClassification(t *testing.T) {
	t.Parallel()
	const shelfJSON = `{"schema":1,"about":[],"ceiling_s":30,"entries":[` +
		`{"id":"just:test-android","reason":"cold run 226 s","measured":"fixture","date":"2026-11-16"},` +
		`{"id":"target:seams","reason":"226 s","measured":"fixture","date":"2026-11-16"}]}`
	shelf, err := testshelf.Parse([]byte(shelfJSON))
	if err != nil {
		t.Fatal(err)
	}
	over := HeavyBound.Milliseconds() + 1

	// No id is in both lists: a heavy measurement for a shelved id (bare or kind-spelled target, or a recipe) is refused.
	for _, id := range []string{"just:test-android", "seams"} {
		err := ValidateAgainstShelf(nil, []Measurement{{id, over}}, shelf)
		if err == nil || !strings.Contains(err.Error(), "on the shelf") {
			t.Errorf("%s in both lists: error = %v, want a refusal naming the shelf", id, err)
		}
	}
	if err := ValidateAgainstShelf(nil, []Measurement{{"pytest:slow", over}}, shelf); err != nil {
		t.Errorf("a heavy id the shelf does not name: %v", err)
	}
	// The repo's own two lists hold together.
	real, err := testshelf.Load("../../test_shelf.json")
	if err != nil {
		t.Fatal(err)
	}
	if err := ValidateAgainstShelf(nil, Measured, real); err != nil {
		t.Errorf("the repo's shelf and heavy measurements: %v", err)
	}

	// -heavy (includeHeavy) runs a heavy id and never a shelved one, even one heavy.go's name rule also names.
	ids := []string{"gtest:a", "just:test-android", "just:test-quest-foo", "seams"}
	run, skipped, err := Plan(shelf, ids, true)
	if err != nil {
		t.Fatal(err)
	}
	if strings.Join(run, " ") != "gtest:a just:test-quest-foo" {
		t.Errorf("-heavy ran %v, want the unit and the heavy id and no shelved one", run)
	}
	if len(skipped) != 2 || skipped[0].ID != "just:test-android" || skipped[0].Kind != SkipShelved ||
		skipped[1].ID != "seams" || skipped[1].Kind != SkipShelved {
		t.Errorf("-heavy skipped %+v, want the two shelved ids as shelved", skipped)
	}
	// A land skips both kinds and names which each was.
	run, skipped, err = Plan(shelf, ids, false)
	if err != nil {
		t.Fatal(err)
	}
	if strings.Join(run, " ") != "gtest:a" || len(skipped) != 3 || skipped[1].Kind != SkipHeavy || skipped[1].ID != "just:test-quest-foo" {
		t.Errorf("a land ran %v and skipped %+v", run, skipped)
	}
}
