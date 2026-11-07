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
// run), that every build is capped by CMAKE_BUILD_PARALLEL_LEVEL, that a failed final rebuild keeps its log and says why, that a run that
// ran nothing fails, and that a run removes its backup directory file by file and never anything it did not write (#2270).

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
	cmake := "#!/bin/sh\necho \"cmake $*\" >> " + calls + "\nif [ -f " + dir + "/buildbroken ]; then echo 'x.cpp:1:1: error: boom'; exit 1; fi\n"
	// With a `stray` flag file, a build leaves a file the script did not write in the run's backup directory.
	cmake += "if [ -f " + dir + "/stray ]; then for d in " + dir + "/combat-mutants.*; do [ -d \"$d\" ] && touch \"$d/stray.txt\"; done; fi\n"
	if failRebuild {
		cmake += "if [ -f " + dir + "/mutated-once ]; then echo 'g++ -Wall -Wextra -Werror -c unrelated.cpp'; echo 'ld: undefined reference to unrelated' >&2; echo 'x.cpp:3:1: error: expected unqualified-id'; exit 1; fi\n"
	}
	write(filepath.Join(dir, "bin/cmake"), cmake, 0o755)
	// A ctest that passes on the unmutated source (fake.cpp holds "a = 1") and fails on a mutated one (killed); it records itself. A
	// baseline that fails is made by a list whose first line is `broken`: the source is then unmutated but the test fails.
	write(filepath.Join(dir, "bin/ctest"), "#!/bin/sh\necho \"ctest $*\" >> "+calls+"\n[ -f "+dir+"/broken ] && exit 1\ngrep -q 'a = 1' "+filepath.Join(dir, "src/quest/combat/fake.cpp")+" && exit 0\ntouch "+dir+"/mutated-once\nexit 1\n", 0o755)
	// No systemd-run: the script falls back to a plain bash.
	write(filepath.Join(dir, "bin/systemd-run"), "#!/bin/sh\nexit 1\n", 0o755)
	return &harness{dir: dir, calls: calls, bin: filepath.Join(dir, "bin")}
}

func (h *harness) run(t *testing.T, jobs string, args ...string) (string, int) {
	t.Helper()
	cmd := exec.Command("bash", append([]string{filepath.Join(h.dir, "src/quest/combat/mutants.sh")}, args...)...)
	// The test's own environment: the documented slice command sets CMAKE_BUILD_PARALLEL_LEVEL, which the default case must not inherit.
	var env []string
	for _, kv := range os.Environ() {
		if !strings.HasPrefix(kv, "CMAKE_BUILD_PARALLEL_LEVEL=") && !strings.HasPrefix(kv, "PATH=") && !strings.HasPrefix(kv, "NEVR_WORK_ROOT=") {
			env = append(env, kv)
		}
	}
	cmd.Env = append(env, "PATH="+h.bin+":"+os.Getenv("PATH"), "NEVR_WORK_ROOT="+h.dir)
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
	// One build for the baseline, one for the mutant, one for the rebuild after the run; all the module's own test program and nothing else.
	if len(targets) != 3 || targets[0] != "fake_test" || targets[1] != "fake_test" || targets[2] != "fake_test" {
		t.Errorf("targets built = %v, want [fake_test fake_test fake_test]", targets)
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
	if !strings.Contains(out, "undefined reference to unrelated") || !strings.Contains(out, "error: expected unqualified-id") {
		t.Errorf("the rebuild's error lines are not printed:\n%s", out)
	}
	if strings.Contains(out, "-Werror -c unrelated.cpp") {
		t.Errorf("a compile line that carries -Werror is printed as an error:\n%s", out)
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

func TestARunStopsBeforeJudgingAnyMutantWhenTheUnmutatedTreeFails(t *testing.T) {
	h := newHarness(t, oneMutant+"runm m2 src/quest/combat/fake.cpp \"s/a = 1/a = 3/\"\n", false)
	if err := os.WriteFile(filepath.Join(h.dir, "broken"), nil, 0o644); err != nil {
		t.Fatal(err)
	}
	out, code := h.run(t, "3", "fake")
	if code != 2 {
		t.Errorf("exit %d, want 2\n%s", code, out)
	}
	if strings.Contains(out, "MUTANT") {
		t.Errorf("a mutant was judged against a tree whose test already fails:\n%s", out)
	}
	if !strings.Contains(out, "fails on the unmutated tree") {
		t.Errorf("the baseline failure is not named:\n%s", out)
	}
}

func TestTheUnmutatedTreeIsBuiltAndRunFirstOncePerProgram(t *testing.T) {
	h := newHarness(t, oneMutant+"runm m2 src/quest/combat/fake.cpp \"s/a = 1/a = 3/\"\n", false)
	out, code := h.run(t, "3", "fake")
	if code != 0 {
		t.Fatalf("exit %d\n%s", code, out)
	}
	if !strings.Contains(out, "baseline quest-host-combat fake_test passes") || strings.Count(out, "baseline ") != 1 {
		t.Errorf("want one baseline for the one program:\n%s", out)
	}
	if strings.Index(out, "baseline") > strings.Index(out, "MUTANT") {
		t.Errorf("the baseline came after a mutant:\n%s", out)
	}
}

func TestARunStopsBeforeJudgingAnyMutantWhenTheUnmutatedTreeDoesNotBuild(t *testing.T) {
	h := newHarness(t, oneMutant, false)
	if err := os.WriteFile(filepath.Join(h.dir, "buildbroken"), nil, 0o644); err != nil {
		t.Fatal(err)
	}
	out, code := h.run(t, "3", "fake")
	if code != 2 || strings.Contains(out, "MUTANT") {
		t.Errorf("exit %d, want 2 and no mutant judged\n%s", code, out)
	}
	if !strings.Contains(out, "the unmutated tree does not build: quest-host-combat fake_test") || !strings.Contains(out, "error: boom") {
		t.Errorf("the baseline build failure is not named with its error:\n%s", out)
	}
}

// backups lists the run's backup directories left in the work root (NEVR_WORK_ROOT is the fake checkout).
func (h *harness) backups(t *testing.T) []string {
	t.Helper()
	found, err := filepath.Glob(filepath.Join(h.dir, "combat-mutants.*"))
	if err != nil {
		t.Fatal(err)
	}
	return found
}

// Every way a run ends (killed, the final rebuild failed, a filter that ran nothing, a baseline that fails or does not build) leaves no
// backup directory behind: each file the run wrote there is removed by its name, then the directory.
func TestARunLeavesNoBackupDirectoryHoweverItEnds(t *testing.T) {
	cases := []struct {
		name, flag  string
		failRebuild bool
		args        []string
	}{
		{"killed", "", false, []string{"fake"}},
		{"rebuild failed", "", true, []string{"fake"}},
		{"nothing ran", "", false, []string{"fake", "^no_such_mutant$"}},
		{"baseline fails", "broken", false, []string{"fake"}},
		{"baseline does not build", "buildbroken", false, []string{"fake"}},
	}
	for _, tc := range cases {
		h := newHarness(t, oneMutant, tc.failRebuild)
		if tc.flag != "" {
			if err := os.WriteFile(filepath.Join(h.dir, tc.flag), nil, 0o644); err != nil {
				t.Fatal(err)
			}
		}
		out, _ := h.run(t, "3", tc.args...)
		if left := h.backups(t); len(left) != 0 {
			t.Errorf("%s: the run left %v\n%s", tc.name, left, out)
		}
		if strings.Contains(out, "left in place") {
			t.Errorf("%s: a clean run says it left files:\n%s", tc.name, out)
		}
	}
}

// A file in the backup directory that the run did not write is not removed: the directory stays with it, the run's own files are gone,
// one stderr line names the directory, and the run's exit status is its own.
func TestAFileTheRunDidNotWriteIsLeftAndNamed(t *testing.T) {
	h := newHarness(t, oneMutant, false)
	if err := os.WriteFile(filepath.Join(h.dir, "stray"), nil, 0o644); err != nil {
		t.Fatal(err)
	}
	out, code := h.run(t, "3", "fake")
	if code != 0 {
		t.Errorf("exit %d, want 0 (the run's own status)\n%s", code, out)
	}
	left := h.backups(t)
	if len(left) != 1 {
		t.Fatalf("want the one backup directory left, found %v\n%s", left, out)
	}
	entries, err := os.ReadDir(left[0])
	if err != nil {
		t.Fatal(err)
	}
	if len(entries) != 1 || entries[0].Name() != "stray.txt" {
		var names []string
		for _, e := range entries {
			names = append(names, e.Name())
		}
		t.Errorf("want only stray.txt left in %s, found %v", left[0], names)
	}
	if strings.Count(out, "mutants.sh: "+left[0]+" holds files the run did not write; left in place") != 1 {
		t.Errorf("the directory left is not named once:\n%s", out)
	}
}
