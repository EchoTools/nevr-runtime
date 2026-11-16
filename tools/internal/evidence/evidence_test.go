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

const issueItem = "an issue reference"

func hasItem(missing []string, want string) bool {
	for _, m := range missing {
		if strings.Contains(m, want) {
			return true
		}
	}
	return false
}

// withSubject swaps the subject line of good and drops its issue reference.
func withSubject(subject string) string {
	_, rest, _ := strings.Cut(good, "\n")
	return subject + "\n" + rest
}

func TestDocsNeedsNoIssue(t *testing.T) {
	for _, subject := range []string{"docs: a change", "docs(scope): a change", "Docs: a change", "DOCS(x)!: a change", "docs!: a change"} {
		if m := Missing(withSubject(subject)); len(m) != 0 {
			t.Errorf("%q: %v", subject, m)
		}
	}
}

func TestDocsWithAnIssueStillPasses(t *testing.T) {
	if m := Missing(good); len(m) != 0 {
		t.Fatalf("%v", m)
	}
}

func TestOtherTypesAndUnparseableSubjectsNeedAnIssue(t *testing.T) {
	for _, subject := range []string{"fix: a change", "feat(x): a change", "fix!: a change", "a change", "docs a change", "docs : a change", "documentation: a change", ": a change", ""} {
		msg := withSubject(subject) + "\n## Siblings\nnone.\n"
		if m := Missing(msg); !hasItem(m, issueItem) {
			t.Errorf("%q: issue reference not demanded: %v", subject, m)
		}
	}
}

func TestDocsMissingASectionIsStillRefused(t *testing.T) {
	msg := strings.Replace(withSubject("docs: a change"), "## Mutants", "## Other", 1)
	m := Missing(msg)
	if !hasItem(m, "## Mutants") || hasItem(m, issueItem) {
		t.Fatalf("%v", m)
	}
}
