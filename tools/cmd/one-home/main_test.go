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

// tree writes files (path -> content) under a new directory and returns it. The directory holds no .git, so the
// command walks it.
func tree(t *testing.T, files map[string]string) string {
	t.Helper()
	root := t.TempDir()
	for p, c := range files {
		full := filepath.Join(root, filepath.FromSlash(p))
		if err := os.MkdirAll(filepath.Dir(full), dirPerm); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(full, []byte(c), filePerm); err != nil {
			t.Fatal(err)
		}
	}
	return root
}

func oneHome(t *testing.T, root string) (code int, out, errOut string) {
	t.Helper()
	var o, e bytes.Buffer
	code = execute([]string{"-root", root}, &o, &e)
	return code, o.String(), e.String()
}

const registry = "# test\nlanding\tAGENTS.md\tHEAD:refs/heads/main\n"

// A procedure written in a second file is refused, and the line that does it is named.
func TestPlantedSecondCopyIsRefused(t *testing.T) {
	root := tree(t, map[string]string{
		"docs/standards/procedure-homes.txt": registry,
		"AGENTS.md":                          "Landing.\n3. git push origin HEAD:refs/heads/main\n",
		"CONTRIBUTING.md":                    "Land it as AGENTS.md says.\n(4) `git push origin HEAD:refs/heads/main`, never forced\n",
	})
	code, out, _ := oneHome(t, root)
	if code != 1 {
		t.Fatalf("exit %d, want 1; output: %q", code, out)
	}
	if want := "landing: CONTRIBUTING.md:2:"; !strings.Contains(out, want) {
		t.Fatalf("output %q does not name %q", out, want)
	}
}

// A tree where the procedure is only in its home, and a pointer to it elsewhere, passes.
func TestOneHomePasses(t *testing.T) {
	root := tree(t, map[string]string{
		"docs/standards/procedure-homes.txt": registry,
		"AGENTS.md":                          "3. git push origin HEAD:refs/heads/main\n",
		"CONTRIBUTING.md":                    "Land with `just land`; the steps are in AGENTS.md, \"Landing\".\n",
	})
	if code, out, errOut := oneHome(t, root); code != 0 {
		t.Fatalf("exit %d, want 0; stdout %q stderr %q", code, out, errOut)
	}
}

// A marker that matches nothing in its home guards nothing: that is a failure (exit 2), not a pass.
func TestMarkerAbsentFromHomeIsAFailure(t *testing.T) {
	root := tree(t, map[string]string{
		"docs/standards/procedure-homes.txt": registry,
		"AGENTS.md":                          "Landing is described elsewhere.\n",
		"CONTRIBUTING.md":                    "nothing here\n",
	})
	code, _, errOut := oneHome(t, root)
	if code != exitUsage || !strings.Contains(errOut, "matches no line of its home") {
		t.Fatalf("exit %d stderr %q, want exit 2 naming the home", code, errOut)
	}
}

// The dated records under docs/audits/ are exempt, and the registry itself may hold the marker text.
func TestAuditsAndRegistryAreExempt(t *testing.T) {
	root := tree(t, map[string]string{
		"docs/standards/procedure-homes.txt": registry,
		"AGENTS.md":                          "git push origin HEAD:refs/heads/main\n",
		"docs/audits/old.md":                 "git push origin HEAD:refs/heads/main\n",
	})
	if code, out, errOut := oneHome(t, root); code != 0 {
		t.Fatalf("exit %d, want 0; stdout %q stderr %q", code, out, errOut)
	}
}

// A malformed registry line is exit 2.
func TestBadRegistryLine(t *testing.T) {
	root := tree(t, map[string]string{
		"docs/standards/procedure-homes.txt": "landing AGENTS.md no tabs\n",
		"AGENTS.md":                          "x\n",
	})
	if code, _, _ := oneHome(t, root); code != exitUsage {
		t.Fatalf("exit %d, want 2", code)
	}
}

// The real tree: every named procedure has one home. This is the check the quick check runs (`just test-tools` runs
// `go test` in tools/).
func TestRealTreeHasOneHomePerProcedure(t *testing.T) {
	root, err := filepath.Abs(filepath.Join("..", "..", ".."))
	if err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(root, defaultRegistry)); err != nil {
		t.Fatalf("the registry is missing: %v", err)
	}
	if code, out, errOut := oneHome(t, root); code != 0 {
		t.Fatalf("exit %d: a procedure is written outside its home.\n%s%s", code, out, errOut)
	}
}

// Planted violation of the logging rule: a run that leaves no record in the tools log fails here. The run is the real
// command through its own entry point, on a state directory of the test's own.
func TestARunIsRecordedInTheToolsLog(t *testing.T) {
	env := testkit.Env(t)
	root := tree(t, map[string]string{
		"docs/standards/procedure-homes.txt": registry,
		"AGENTS.md":                          "3. git push origin HEAD:refs/heads/main\n",
	})
	args := []string{"-root", root}
	var o, e bytes.Buffer
	code := run(args, testkit.Getenv(env), &o, &e)
	if code != 0 {
		t.Fatalf("exit %d: %s%s", code, o.String(), e.String())
	}
	testkit.RequireRunRecorded(t, env, "one-home", code, args)
}
