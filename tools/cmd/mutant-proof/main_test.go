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

const good = "subject\n\n## Mutants\n- M1 drop the check -> FAIL test_x: Overtaken\n- M2 never decremented -> FAIL test_x: Times out\n- restored tree: 9 tests OK\n\n## Other\nx\n"

func check(t *testing.T, msg string) (int, string) {
	t.Helper()
	p := filepath.Join(t.TempDir(), "m")
	if err := os.WriteFile(p, []byte(msg), filePerm); err != nil {
		t.Fatal(err)
	}
	var o, e bytes.Buffer
	c := execute([]string{"-file", p}, &o, &e)
	return c, o.String() + e.String()
}

func TestGreenBlock(t *testing.T) {
	if c, out := check(t, good); c != 0 {
		t.Fatalf("%d %s", c, out)
	}
	if c, out := check(t, "s\n\n## Mutants\nNone: prose only.\n"); c != 0 {
		t.Fatalf("%d %s", c, out)
	}
}

// Planted violations: a survivor, a mutant that never landed, a missing restore line, an empty reason.
func TestPlantedSurvivorIsRefused(t *testing.T) {
	for name, msg := range map[string]string{
		"survivor":   strings.Replace(good, "-> FAIL test_x: Overtaken", "-> survived", 1),
		"no landing": strings.Replace(good, "- M2 never decremented -> FAIL test_x: Times out", "- M2 never landed -> FAIL", 1),
		"no restore": strings.Replace(good, "- restored tree: 9 tests OK\n", "", 1),
		"no reason":  "s\n\n## Mutants\nNone:\n",
		"no section": "s\n",
		"no mutants": "s\n\n## Mutants\n- restored tree: OK\n",
	} {
		if c, out := check(t, msg); c != 1 {
			t.Errorf("%s: code %d: %s", name, c, out)
		}
	}
}

func git(t *testing.T, dir string, args ...string) {
	t.Helper()
	cmd := exec.Command("git", append([]string{"-C", dir, "-c", "user.name=t", "-c", "user.email=t@example.org"}, args...)...)
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("git %v: %v %s", args, err, out)
	}
}

func TestCleanRefusesADirtyTree(t *testing.T) {
	d := t.TempDir()
	git(t, d, "init", "-q")
	os.WriteFile(filepath.Join(d, "f"), []byte("a"), filePerm)
	git(t, d, "add", "f")
	git(t, d, "commit", "-q", "-m", "x")
	var o, e bytes.Buffer
	if c := execute([]string{"-clean", "-repo", d}, &o, &e); c != 0 {
		t.Fatalf("clean tree: %d %s", c, e.String())
	}
	os.WriteFile(filepath.Join(d, "f"), []byte("mutant"), filePerm)
	o.Reset()
	e.Reset()
	if c := execute([]string{"-clean", "-repo", d}, &o, &e); c != 1 || !strings.Contains(o.String(), "f") {
		t.Fatalf("dirty tree: %d %s%s", c, o.String(), e.String())
	}
}

func TestRealCommitOfTheTreePasses(t *testing.T) {
	if _, err := exec.Command("git", "-C", "../../..", "rev-parse", "c60eba00").Output(); err != nil {
		t.Skip("commit not in this clone")
	}
	var o, e bytes.Buffer
	if c := execute([]string{"-repo", "../../..", "-rev", "c60eba00"}, &o, &e); c != 0 {
		t.Fatalf("%d %s%s", c, o.String(), e.String())
	}
}

func TestBadFlags(t *testing.T) {
	var o, e bytes.Buffer
	if c := execute(nil, &o, &e); c != exitUsage {
		t.Fatalf("code %d", c)
	}
	if c := execute([]string{"-file", "/nonexistent/none"}, &o, &e); c != exitUsage {
		t.Fatalf("unreadable file: code %d", c)
	}
}

// Planted violation of the logging rule: a run that leaves no record in the tools log fails here. The run is the real
// command through its own entry point, on a state directory of the test's own.
func TestARunIsRecordedInTheToolsLog(t *testing.T) {
	env := testkit.Env(t)
	p := filepath.Join(t.TempDir(), "m")
	if err := os.WriteFile(p, []byte(good), filePerm); err != nil {
		t.Fatal(err)
	}
	args := []string{"-file", p}
	var o, e bytes.Buffer
	code := run(args, testkit.Getenv(env), &o, &e)
	if code != 0 {
		t.Fatalf("exit %d: %s%s", code, o.String(), e.String())
	}
	testkit.RequireRunRecorded(t, env, "mutant-proof", code, args)
}
