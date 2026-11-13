package main

import (
	"bytes"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/testkit"
)

// File and directory modes of the files a test writes.
const (
	filePerm = 0o644
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

func runOn(t *testing.T, msg string) (int, string) {
	t.Helper()
	p := filepath.Join(t.TempDir(), "msg")
	if err := os.WriteFile(p, []byte(msg), filePerm); err != nil {
		t.Fatal(err)
	}
	var o, e bytes.Buffer
	code := execute([]string{"-file", p}, &o, &e)
	return code, o.String() + e.String()
}

func TestGreenCompleteMessage(t *testing.T) {
	if code, out := runOn(t, good); code != 0 {
		t.Fatalf("code %d: %s", code, out)
	}
}

// Planted violation: each section removed in turn is named and refused.
func TestPlantedMissingSectionIsRefused(t *testing.T) {
	for _, h := range []string{"## Search statement", "## Symbols", "## Mutants", "## Platform parity", "## Commands and results"} {
		code, out := runOn(t, strings.Replace(good, h, "## Other", 1))
		if code != 1 || !strings.Contains(out, "missing:") {
			t.Errorf("%s removed: code %d: %s", h, code, out)
		}
	}
}

func TestPlantedNoExitIssueTrailer(t *testing.T) {
	for _, c := range []struct{ from, to string }{
		{"exit 0", "ran"}, {"Refs #1", "later"}, {"Co-Authored-By:", "By:"},
	} {
		if code, out := runOn(t, strings.Replace(good, c.from, c.to, 1)); code != 1 {
			t.Errorf("%q: code %d: %s", c.from, code, out)
		}
	}
}

func TestFixNeedsSiblings(t *testing.T) {
	fix := strings.Replace(good, "docs: a change", "fix(x): a change", 1)
	if code, out := runOn(t, fix); code != 1 || !strings.Contains(out, "Siblings") {
		t.Fatalf("code %d: %s", code, out)
	}
	if code, out := runOn(t, fix+"\n## Siblings\nnone.\n"); code != 0 {
		t.Fatalf("code %d: %s", code, out)
	}
}

func TestEmptySectionIsMissing(t *testing.T) {
	if code, _ := runOn(t, strings.Replace(good, "Edited: a.", "", 1)); code != 1 {
		t.Fatal("an empty section passed")
	}
}

func TestBadFlags(t *testing.T) {
	var o, e bytes.Buffer
	if code := execute(nil, &o, &e); code != exitUsage {
		t.Fatalf("code %d", code)
	}
}

// Green on the tree: a real landed commit that carries the sections.
func TestRealCommitOfTheTreePasses(t *testing.T) {
	if _, err := exec.Command("git", "-C", "../../..", "rev-parse", "c60eba00").Output(); err != nil {
		t.Skip("commit c60eba00 is not in this clone")
	}
	var o, e bytes.Buffer
	if code := execute([]string{"-repo", "../../..", "-rev", "c60eba00"}, &o, &e); code != 0 {
		t.Fatalf("code %d: %s%s", code, o.String(), e.String())
	}
}

// Planted violation of the logging rule: a run that leaves no record in the tools log fails here. The run is the real
// command through its own entry point, on a state directory of the test's own.
func TestARunIsRecordedInTheToolsLog(t *testing.T) {
	env := testkit.Env(t)
	p := filepath.Join(t.TempDir(), "msg")
	if err := os.WriteFile(p, []byte(good), filePerm); err != nil {
		t.Fatal(err)
	}
	args := []string{"-file", p}
	var o, e bytes.Buffer
	code := run(args, testkit.Getenv(env), &o, &e)
	if code != 0 {
		t.Fatalf("exit %d: %s%s", code, o.String(), e.String())
	}
	testkit.RequireRunRecorded(t, env, "commit-evidence", code, args)
}
