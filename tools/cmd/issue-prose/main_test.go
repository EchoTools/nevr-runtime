package main

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/testkit"
)

// File and directory modes of the files a test writes.
const (
	filePerm = 0o644
)

const body = "## Evidence at ab0d9876\nthe tree says x.\n\n## Not shown\ny.\n\n## Falsifier\nz.\n\n## Work\n1. t.\n"
const closing = "Landed in ab0d9876; `just test-tools` exit 0."

func do(t *testing.T, kind, text string) (int, string) {
	t.Helper()
	p := filepath.Join(t.TempDir(), "t")
	os.WriteFile(p, []byte(text), filePerm)
	var o, e bytes.Buffer
	c := execute([]string{"-kind", kind, "-file", p}, &o, &e)
	return c, o.String() + e.String()
}

func TestGreen(t *testing.T) {
	if c, out := do(t, "body", body); c != 0 {
		t.Fatalf("%d %s", c, out)
	}
	if c, out := do(t, "close", closing); c != 0 {
		t.Fatalf("%d %s", c, out)
	}
}

// Planted violations.
func TestPlantedViolationsAreRefused(t *testing.T) {
	for name, c := range map[string]struct{ kind, text string }{
		"no falsifier":  {"body", strings.Replace(body, "## Falsifier", "## Other", 1)},
		"no sha":        {"body", strings.Replace(body, "ab0d9876", "a branch", 1)},
		"local path":    {"body", body + "see /home/x/log\n"},
		"home tilde":    {"close", closing + " (~/src/x)"},
		"close no sha":  {"close", "fixed; exit 0"},
		"close no exit": {"close", "landed in ab0d9876"},
		"windows path":  {"close", closing + ` C:\Users\x`},
	} {
		if code, out := do(t, c.kind, c.text); code != 1 {
			t.Errorf("%s: %d %s", name, code, out)
		}
	}
}

func TestWordsAreNotShas(t *testing.T) {
	// "defaced" is 7 hex letters and no digit: not a sha.
	if c, _ := do(t, "close", "defaced exit 0"); c != 1 {
		t.Fatal("a word passed as a sha")
	}
}

func TestBadFlags(t *testing.T) {
	var o, e bytes.Buffer
	if execute([]string{"-kind", "x"}, &o, &e) != exitUsage {
		t.Fatal("want 2")
	}
}

// Planted violation of the logging rule: a run that leaves no record in the tools log fails here. The run is the real
// command through its own entry point, on a state directory of the test's own.
func TestARunIsRecordedInTheToolsLog(t *testing.T) {
	env := testkit.Env(t)
	p := filepath.Join(t.TempDir(), "t")
	if err := os.WriteFile(p, []byte(body), filePerm); err != nil {
		t.Fatal(err)
	}
	args := []string{"-kind", "body", "-file", p}
	var o, e bytes.Buffer
	code := run(args, testkit.Getenv(env), &o, &e)
	if code != 0 {
		t.Fatalf("exit %d: %s%s", code, o.String(), e.String())
	}
	testkit.RequireRunRecorded(t, env, "issue-prose", code, args)
}
