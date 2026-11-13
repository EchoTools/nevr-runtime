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
	dirPerm  = 0o755
)

func tree(t *testing.T, files map[string]string) string {
	t.Helper()
	root := t.TempDir()
	for p, c := range files {
		full := filepath.Join(root, filepath.FromSlash(p))
		os.MkdirAll(filepath.Dir(full), dirPerm)
		if err := os.WriteFile(full, []byte(c), filePerm); err != nil {
			t.Fatal(err)
		}
	}
	return root
}

func do(root string) (int, string) {
	var o, e bytes.Buffer
	c := execute([]string{"-root", root}, &o, &e)
	return c, o.String() + e.String()
}

const fullTests = "package main\nfunc TestPlantedX(t *testing.T){}\nfunc TestGreenX(t *testing.T){}\nfunc TestBadInput(t *testing.T){}\n"

func files(tests string) map[string]string {
	return map[string]string{
		"docs/standards/guards.txt": "g\tg\tjust test-tools\n",
		"tools/cmd/g/main.go":       "package main\n",
		"tools/cmd/g/main_test.go":  tests,
		"AGENTS.md":                 "run just test-tools\n",
	}
}

func TestGreenProvenGuard(t *testing.T) {
	if c, out := do(tree(t, files(fullTests))); c != 0 {
		t.Fatalf("%d %s", c, out)
	}
}

// Planted violation: a guard whose tests lack the planted, green or bad-input case.
func TestPlantedGuardWithoutProofIsRefused(t *testing.T) {
	for _, drop := range []string{"TestPlantedX", "TestGreenX", "TestBadInput"} {
		c, out := do(tree(t, files(strings.Replace(fullTests, drop, "TestOther", 1))))
		if c != 1 || !strings.Contains(out, "unproven: g") {
			t.Errorf("%s dropped: %d %s", drop, c, out)
		}
	}
	f := files(fullTests)
	delete(f, "tools/cmd/g/main_test.go")
	if c, _ := do(tree(t, f)); c != 1 {
		t.Error("missing test file passed")
	}
	f = files(fullTests)
	f["AGENTS.md"] = "nothing\n"
	if c, _ := do(tree(t, f)); c != 1 {
		t.Error("an unnamed step passed")
	}
}

func TestBadRegistry(t *testing.T) {
	f := files(fullTests)
	f["docs/standards/guards.txt"] = "# none\n"
	if c, _ := do(tree(t, f)); c != exitUsage {
		t.Fatal("an empty registry passed")
	}
}

func TestRealTreeGuardsAreProven(t *testing.T) {
	if _, err := os.Stat("../../../docs/standards/guards.txt"); err != nil {
		t.Skip("no registry in this tree")
	}
	if c, out := do("../../.."); c != 0 {
		t.Fatalf("%d %s", c, out)
	}
}

// Planted violation of the logging rule: a run that leaves no record in the tools log fails here. The run is the real
// command through its own entry point, on a state directory of the test's own.
func TestARunIsRecordedInTheToolsLog(t *testing.T) {
	env := testkit.Env(t)
	root := tree(t, files(fullTests))
	args := []string{"-root", root}
	var o, e bytes.Buffer
	code := run(args, testkit.Getenv(env), &o, &e)
	if code != 0 {
		t.Fatalf("exit %d: %s%s", code, o.String(), e.String())
	}
	testkit.RequireRunRecorded(t, env, "guard-proof", code, args)
}
