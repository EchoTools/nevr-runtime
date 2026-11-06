package questwiring

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// #2179, #2234: src/quest/combat/mutants.sh is the one mutation runner every combat module uses. These tests run the real script against a
// fake checkout whose cmake and ctest are scripts that record their arguments, so they pin what the host tests of a module cannot: that a
// build is for the target the list line names and nowhere else (never a whole tree, where an unrelated program that does not link fails the
// run), that every build is capped by CMAKE_BUILD_PARALLEL_LEVEL, that a failed final rebuild keeps its log and says why, and that a run that
// ran nothing fails.

type harness struct {
	dir   string // the fake checkout
	calls string // cmake and ctest calls, one per line
	bin   string
}

func newHarness(t *testing.T, list string, failRebuild bool) *harness {
	t.Helper()
	dir := t.TempDir()
	combat := filepath.Join(dir, "src/quest/combat")
	for _, d := range []string{filepath.Join(combat, "mutants.d"), filepath.Join(dir, "build/quest-host-combat"), filepath.Join(dir, "src/quest/combat"), filepath.Join(dir, "bin")} {
		if err := os.MkdirAll(d, 0o755); err != nil {
			t.Fatal(err)
		}
	}
	write := func(path, text string, mode os.FileMode) {
		if err := os.WriteFile(path, []byte(text), mode); err != nil {
			t.Fatal(err)
		}
	}
	write(filepath.Join(combat, "mutants.sh"), read(t, "src/quest/combat/mutants.sh"), 0o755)
	write(filepath.Join(combat, "mutants.d/fake.sh"), list, 0o644)
	write(filepath.Join(combat, "fake.cpp"), "int a = 1;\n", 0o644)
	write(filepath.Join(dir, "build/quest-host-combat/build.ninja"), "", 0o644)
	calls := filepath.Join(dir, "calls.txt")
	// A cmake that records its arguments; the second and later builds fail when asked to (the final rebuild), printing an error line.
	cmake := "#!/bin/sh\necho \"cmake $*\" >> " + calls + "\n"
	if failRebuild {
		cmake += "if [ -f " + dir + "/mutated-once ]; then echo 'ld: undefined reference to unrelated' >&2; echo 'error: ld returned 1 exit status'; exit 1; fi\n"
	}
	write(filepath.Join(dir, "bin/cmake"), cmake, 0o755)
	// A ctest that fails every mutant (killed) and records itself.
	write(filepath.Join(dir, "bin/ctest"), "#!/bin/sh\necho \"ctest $*\" >> "+calls+"\ntouch "+dir+"/mutated-once\nexit 1\n", 0o755)
	// No systemd-run: the script falls back to a plain bash.
	write(filepath.Join(dir, "bin/systemd-run"), "#!/bin/sh\nexit 1\n", 0o755)
	return &harness{dir: dir, calls: calls, bin: filepath.Join(dir, "bin")}
}

func (h *harness) run(t *testing.T, jobs string, args ...string) (string, int) {
	t.Helper()
	cmd := exec.Command("bash", append([]string{filepath.Join(h.dir, "src/quest/combat/mutants.sh")}, args...)...)
	cmd.Env = append(os.Environ(), "PATH="+h.bin+":"+os.Getenv("PATH"), "NEVR_WORK_ROOT="+h.dir)
	if jobs != "" {
		cmd.Env = append(cmd.Env, "CMAKE_BUILD_PARALLEL_LEVEL="+jobs)
	}
	out, err := cmd.CombinedOutput()
	code := 0
	if ee, ok := err.(*exec.ExitError); ok {
		code = ee.ExitCode()
	} else if err != nil {
		t.Fatal(err)
	}
	return string(out), code
}

func (h *harness) cmakeCalls(t *testing.T) []string {
	t.Helper()
	b, err := os.ReadFile(h.calls)
	if err != nil {
		return nil
	}
	var out []string
	for _, l := range strings.Split(strings.TrimSpace(string(b)), "\n") {
		if strings.HasPrefix(l, "cmake ") {
			out = append(out, l)
		}
	}
	return out
}

const oneMutant = "runm m1 src/quest/combat/fake.cpp \"s/a = 1/a = 2/\"\n"

func TestEveryBuildNamesItsTargetAndIsCappedAtTheJobLimit(t *testing.T) {
	for _, tc := range []struct{ jobs, want string }{{"", "-j 4"}, {"7", "-j 7"}, {"zero", "-j 4"}} {
		h := newHarness(t, oneMutant+"runm m2 src/quest/combat/fake.cpp \"s/a = 1/a = 3/\" quest-host-shared other_test\n", false)
		if _, err := os.Stat(filepath.Join(h.dir, "build/quest-host-shared")); err != nil {
			_ = os.MkdirAll(filepath.Join(h.dir, "build/quest-host-shared"), 0o755)
		}
		out, code := h.run(t, tc.jobs, "fake")
		if code != 0 {
			t.Fatalf("jobs=%q: exit %d\n%s", tc.jobs, code, out)
		}
		calls := h.cmakeCalls(t)
		if len(calls) == 0 {
			t.Fatalf("jobs=%q: no cmake call\n%s", tc.jobs, out)
		}
		for _, c := range calls {
			if !strings.Contains(c, "--target ") {
				t.Errorf("jobs=%q: a build without a target (it would build the whole tree): %s", tc.jobs, c)
			}
			if !strings.Contains(c, tc.want) {
				t.Errorf("jobs=%q: a build not capped (%s): %s", tc.jobs, tc.want, c)
			}
		}
	}
}

func TestTheFinalRebuildIsOfTheTargetsTheRunBuiltAlone(t *testing.T) {
	h := newHarness(t, oneMutant, false)
	out, code := h.run(t, "3", "fake")
	if code != 0 {
		t.Fatalf("exit %d\n%s", code, out)
	}
	var targets []string
	for _, c := range h.cmakeCalls(t) {
		f := strings.Fields(c)
		for i, w := range f {
			if w == "--target" {
				targets = append(targets, f[i+1])
			}
		}
	}
	// One build for the mutant, one for the rebuild after the run; both the module's own test program and nothing else.
	if len(targets) != 2 || targets[0] != "fake_test" || targets[1] != "fake_test" {
		t.Errorf("targets built = %v, want [fake_test fake_test]", targets)
	}
	if !strings.Contains(out, "rebuilt quest-host-combat fake_test") {
		t.Errorf("the rebuild is not reported:\n%s", out)
	}
}

func TestAFailedFinalRebuildKeepsItsLogPrintsItsErrorAndFailsTheRun(t *testing.T) {
	h := newHarness(t, oneMutant, true)
	out, code := h.run(t, "3", "fake")
	if code != 1 {
		t.Errorf("exit %d, want 1\n%s", code, out)
	}
	if !strings.Contains(out, "the rebuild of quest-host-combat fake_test after the run failed") {
		t.Errorf("the failure is not named:\n%s", out)
	}
	if !strings.Contains(out, "undefined reference to unrelated") || !strings.Contains(out, "error: ld returned 1 exit status") {
		t.Errorf("the rebuild's error lines are not printed:\n%s", out)
	}
}

func TestARunThatRanNoMutantFails(t *testing.T) {
	h := newHarness(t, oneMutant, false)
	out, code := h.run(t, "3", "fake", "^no_such_mutant$")
	if code != 2 {
		t.Errorf("exit %d, want 2\n%s", code, out)
	}
	if len(h.cmakeCalls(t)) != 0 {
		t.Errorf("a run with no mutant built something")
	}
}
