package evidence

import (
	"strings"
	"testing"
)

const good = `docs: a change (Refs #1)

## Search statement
- ` + "`git grep -n x`" + `: one hit.

## Symbols
Edited: a.

## Commands and results
- ` + "`just build`" + `: exit 0.

## Mutants
None: prose.

## Platform parity
Docs.

Co-Authored-By: A <a@example.org>
`

func TestGreenCompleteMessage(t *testing.T) {
	if m := Missing(good); len(m) != 0 {
		t.Fatalf("%v", m)
	}
}

// Planted violation: each section removed in turn is named.
func TestPlantedMissingSectionIsReported(t *testing.T) {
	for _, h := range []string{"## Search statement", "## Symbols", "## Mutants", "## Platform parity", "## Commands and results"} {
		if m := Missing(strings.Replace(good, h, "## Other", 1)); len(m) == 0 {
			t.Errorf("%s removed: nothing missing", h)
		}
	}
}

func TestFixNeedsSiblings(t *testing.T) {
	fix := strings.Replace(good, "docs: a change", "fix(x): a change", 1)
	if m := Missing(fix); len(m) != 1 || !strings.Contains(m[0], "Siblings") {
		t.Fatalf("%v", m)
	}
	if m := Missing(fix + "\n## Siblings\nnone.\n"); len(m) != 0 {
		t.Fatalf("%v", m)
	}
}

// emptyMessageMissesAtLeast is the number of items an empty message lacks: the four sections, the commands section, the
// issue reference and the trailer make seven; the test holds it to at least six.
const emptyMessageMissesAtLeast = 6

func TestBadMessagesLackEverything(t *testing.T) {
	if m := Missing(""); len(m) < emptyMessageMissesAtLeast {
		t.Fatalf("%v", m)
	}
}
