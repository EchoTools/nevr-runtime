package runroot

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// when is the instant every test makes its run roots at: the name carries 20261114T030405Z.
var when = time.Date(2026, 11, 14, 3, 4, 5, 0, time.UTC)

const pid = 4242

// runName is the name Make gives a run root labelled label at when, pid.
func runName(label string) string { return label + "-20261114T030405Z-4242" }

func mustMake(t *testing.T, wr, label string) string {
	t.Helper()
	p, err := Make(wr, label, when, pid)
	if err != nil {
		t.Fatalf("Make(%q): %v", label, err)
	}
	return p
}

func mustExist(t *testing.T, p string) {
	t.Helper()
	if _, err := os.Lstat(p); err != nil {
		t.Fatalf("%s must survive, but: %v", p, err)
	}
}

func mustRead(t *testing.T, p string) string {
	t.Helper()
	b, err := os.ReadFile(p)
	if err != nil {
		t.Fatal(err)
	}
	return string(b)
}

func mkfile(t *testing.T, p string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(p, []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
}

// canary makes a directory outside any work root with one file in it.
func canary(t *testing.T) (dir, file string) {
	t.Helper()
	dir = t.TempDir()
	file = filepath.Join(dir, "canary.txt")
	if err := os.WriteFile(file, []byte("keep"), 0o644); err != nil {
		t.Fatal(err)
	}
	return dir, file
}

func TestMakeNamesTheRunRootByLabelUTCStampAndPid(t *testing.T) {
	wr := t.TempDir()
	p := mustMake(t, wr, "test-go-tools")
	want := filepath.Join(wr, "run-roots", "test-go-tools-20261114T030405Z-4242")
	if p != want {
		t.Fatalf("Make returned %q, want %q", p, want)
	}
	fi, err := os.Lstat(p)
	if err != nil || !fi.IsDir() {
		t.Fatalf("run root not made as a directory: %v", err)
	}
}

func TestMakeAcceptsA64ByteLabelAndRefusesALabelOutsideTheGrammar(t *testing.T) {
	if _, err := Make(t.TempDir(), strings.Repeat("a", 64), when, pid); err != nil {
		t.Errorf("a 64-byte label must be accepted: %v", err)
	}
	for _, label := range []string{"", "Upper", "-lead", "has space", "a/b", "../x", "under_score", strings.Repeat("a", 65)} {
		wr := t.TempDir()
		if _, err := Make(wr, label, when, pid); err == nil {
			t.Errorf("Make accepted label %q", label)
		}
		if _, err := os.Lstat(filepath.Join(wr, "run-roots")); err == nil {
			t.Errorf("a refused label %q still made run-roots", label)
		}
	}
}

func TestMakeRefusesAnExistingRunRoot(t *testing.T) {
	wr := t.TempDir()
	p := mustMake(t, wr, "x")
	if _, err := Make(wr, "x", when, pid); err == nil {
		t.Fatal("a second Make with the same label, stamp and pid must be refused")
	}
	mustExist(t, p)
}

func TestMakeRefusesAnEmptyWorkRoot(t *testing.T) {
	if _, err := Make("", "x", when, pid); !errors.Is(err, ErrEmptyWorkRoot) {
		t.Fatalf("Make with an empty work root: %v, want ErrEmptyWorkRoot", err)
	}
}

func TestReadmesAreWrittenOnceAndNeverOverwritten(t *testing.T) {
	wr := t.TempDir()
	mustMake(t, wr, "x")
	rootReadme := filepath.Join(wr, "README.md")
	runsReadme := filepath.Join(wr, "run-roots", "README.md")

	wantRoot := "# " + filepath.Base(wr) + "\n\n" + rootPurpose + "\n\nOwner: " + owner() + "\nRemoved: " + rootRemoved + "\n"
	wantRuns := "# run-roots\n\n" + runRootsPurpose + "\n\nOwner: " + owner() + "\nRemoved: " + runRootsRemoved + "\n"
	if got := mustRead(t, rootReadme); got != wantRoot {
		t.Errorf("root README:\n%q\nwant:\n%q", got, wantRoot)
	}
	if got := mustRead(t, runsReadme); got != wantRuns {
		t.Errorf("run-roots README:\n%q\nwant:\n%q", got, wantRuns)
	}

	if err := os.WriteFile(rootReadme, []byte("mine"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(runsReadme, []byte("mine"), 0o644); err != nil {
		t.Fatal(err)
	}
	mustMake(t, wr, "y")
	if got := mustRead(t, rootReadme); got != "mine" {
		t.Errorf("root README overwritten: %q", got)
	}
	if got := mustRead(t, runsReadme); got != "mine" {
		t.Errorf("run-roots README overwritten: %q", got)
	}
}

// TestAReadmeThatCannotBeWrittenLeavesNoRunRoot: run-roots is read-only, so its README cannot be written. Make must
// fail and leave no run root, because the READMEs are written before the run root is made.
func TestAReadmeThatCannotBeWrittenLeavesNoRunRoot(t *testing.T) {
	if os.Geteuid() == 0 {
		t.Skip("root ignores directory permissions, so the read-only directory cannot refuse the README")
	}
	wr := t.TempDir()
	runs := filepath.Join(wr, "run-roots")
	if err := os.Mkdir(runs, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(runs, 0o555); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chmod(runs, 0o755) })
	if _, err := Make(wr, "x", when, pid); err == nil {
		t.Fatal("Make into a read-only run-roots must fail")
	}
	entries, err := os.ReadDir(runs)
	if err != nil {
		t.Fatal(err)
	}
	if len(entries) != 0 {
		t.Fatalf("a failed Make left %d entries in run-roots", len(entries))
	}
}

// TestRemoveRemovesARealRunRootHoldingFilesANestedDirAndALinkToTheCanary is the success case, and it proves the
// removal does not follow a link: the link's target and everything in it survive.
func TestRemoveRemovesARealRunRootHoldingFilesANestedDirAndALinkToTheCanary(t *testing.T) {
	wr := t.TempDir()
	p := mustMake(t, wr, "good")
	mkfile(t, filepath.Join(p, "a.txt"))
	mkfile(t, filepath.Join(p, "sub", "deeper", "b.txt"))
	canDir, canFile := canary(t)
	if err := os.Symlink(canDir, filepath.Join(p, "link")); err != nil {
		t.Fatal(err)
	}

	if err := Remove(wr, p); err != nil {
		t.Fatalf("Remove on a real run root: %v", err)
	}
	if _, err := os.Lstat(p); !os.IsNotExist(err) {
		t.Errorf("run root still there: %v", err)
	}
	if got := mustRead(t, canFile); got != "keep" {
		t.Errorf("canary changed: %q", got)
	}
}

// TestRemoveRefusesWithItsReasonAndRemovesNothing: each row names the sentinel the refusal must wrap, and the paths that
// must survive. A row with no survivors still has the case's own canary, when it links one.
func TestRemoveRefusesWithItsReasonAndRemovesNothing(t *testing.T) {
	cases := []struct {
		name  string
		want  error
		setup func(t *testing.T, wr string) (runRoot string, survive []string)
	}{
		{"1 run root is a symlink to a real run root", ErrLink, func(t *testing.T, wr string) (string, []string) {
			real := filepath.Join(wr, "run-roots", runName("real"))
			mkfile(t, filepath.Join(real, "f.txt"))
			link := filepath.Join(wr, "run-roots", runName("lnk"))
			if err := os.Symlink(real, link); err != nil {
				t.Fatal(err)
			}
			return link, []string{filepath.Join(real, "f.txt")}
		}},
		{"2 run root is a symlink to the canary", ErrLink, func(t *testing.T, wr string) (string, []string) {
			canDir, canFile := canary(t)
			if err := os.MkdirAll(filepath.Join(wr, "run-roots"), 0o755); err != nil {
				t.Fatal(err)
			}
			link := filepath.Join(wr, "run-roots", runName("lnk"))
			if err := os.Symlink(canDir, link); err != nil {
				t.Fatal(err)
			}
			return link, []string{canFile}
		}},
		{"3 run root is outside the work root", ErrOutside, func(t *testing.T, wr string) (string, []string) {
			outside := t.TempDir()
			rr := filepath.Join(outside, runName("out"))
			mkfile(t, filepath.Join(rr, "f.txt"))
			return rr, []string{filepath.Join(rr, "f.txt")}
		}},
		{"4 work root run-roots is a symlink to elsewhere", ErrLink, func(t *testing.T, wr string) (string, []string) {
			elsewhere := t.TempDir()
			if err := os.Symlink(elsewhere, filepath.Join(wr, "run-roots")); err != nil {
				t.Fatal(err)
			}
			rr := filepath.Join(elsewhere, runName("run"))
			mkfile(t, filepath.Join(rr, "f.txt"))
			return filepath.Join(wr, "run-roots", runName("run")), []string{filepath.Join(rr, "f.txt")}
		}},
		{"5 run root is the run-roots directory itself", ErrOutside, func(t *testing.T, wr string) (string, []string) {
			inside := filepath.Join(wr, "run-roots", runName("in"))
			mkfile(t, filepath.Join(inside, "f.txt"))
			return filepath.Join(wr, "run-roots"), []string{filepath.Join(inside, "f.txt")}
		}},
		{"6 run root base fails the name grammar", ErrName, func(t *testing.T, wr string) (string, []string) {
			rr := filepath.Join(wr, "run-roots", "NotAName")
			mkfile(t, filepath.Join(rr, "f.txt"))
			return rr, []string{filepath.Join(rr, "f.txt")}
		}},
		{"7 run root is missing", ErrMissing, func(t *testing.T, wr string) (string, []string) {
			if err := os.MkdirAll(filepath.Join(wr, "run-roots"), 0o755); err != nil {
				t.Fatal(err)
			}
			return filepath.Join(wr, "run-roots", runName("gone")), nil
		}},
		{"8 run root path has a .. element", ErrPath, func(t *testing.T, wr string) (string, []string) {
			rr := mustMake(t, wr, "dots")
			mkfile(t, filepath.Join(rr, "f.txt"))
			// Built by concatenation: filepath.Join would clean the .. away before Remove sees it.
			return wr + "/run-roots/../run-roots/" + filepath.Base(rr), []string{filepath.Join(rr, "f.txt")}
		}},
		{"9 run root path has a trailing slash", ErrPath, func(t *testing.T, wr string) (string, []string) {
			rr := mustMake(t, wr, "slash")
			mkfile(t, filepath.Join(rr, "f.txt"))
			return rr + "/", []string{filepath.Join(rr, "f.txt")}
		}},
		{"10 an intermediate link alias/<name> to run-roots", ErrOutside, func(t *testing.T, wr string) (string, []string) {
			rr := mustMake(t, wr, "viaalias")
			mkfile(t, filepath.Join(rr, "f.txt"))
			if err := os.Symlink(filepath.Join(wr, "run-roots"), filepath.Join(wr, "alias")); err != nil {
				t.Fatal(err)
			}
			return filepath.Join(wr, "alias", filepath.Base(rr)), []string{filepath.Join(rr, "f.txt")}
		}},
		{"11 run root is a regular file with a valid name", ErrNotDir, func(t *testing.T, wr string) (string, []string) {
			f := filepath.Join(wr, "run-roots", runName("file"))
			mkfile(t, f)
			return f, []string{f}
		}},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			wr := t.TempDir()
			rr, survive := c.setup(t, wr)
			err := Remove(wr, rr)
			if err == nil {
				t.Fatalf("Remove(%s) accepted a refused run root", rr)
			}
			if !errors.Is(err, c.want) {
				t.Fatalf("Remove(%s) refused with %v, want %v", rr, err, c.want)
			}
			for _, p := range survive {
				mustExist(t, p)
			}
		})
	}
}

// TestRemoveRefusesAnEmptyWorkRoot: with no work root, nothing is checked and nothing is removed.
func TestRemoveRefusesAnEmptyWorkRoot(t *testing.T) {
	wr := t.TempDir()
	p := mustMake(t, wr, "keep")
	mkfile(t, filepath.Join(p, "f.txt"))
	if err := Remove("", p); !errors.Is(err, ErrEmptyWorkRoot) {
		t.Fatalf("Remove with an empty work root: %v, want ErrEmptyWorkRoot", err)
	}
	mustExist(t, filepath.Join(p, "f.txt"))
}

// TestRemoveAcceptsAWorkRootThatIsALinkAndTouchesOnlyTheRealTarget: the work root is itself a symbolic link to a real
// directory. The removal happens inside that directory's run-roots, and a sibling run root and a canary survive.
func TestRemoveAcceptsAWorkRootThatIsALinkAndTouchesOnlyTheRealTarget(t *testing.T) {
	realWR := t.TempDir()
	linkWR := filepath.Join(t.TempDir(), "wr-link")
	if err := os.Symlink(realWR, linkWR); err != nil {
		t.Fatal(err)
	}
	p := mustMake(t, realWR, "viaroot")
	other := mustMake(t, realWR, "sibling")
	mkfile(t, filepath.Join(p, "f.txt"))
	_, canFile := canary(t)

	if err := Remove(linkWR, filepath.Join(linkWR, "run-roots", filepath.Base(p))); err != nil {
		t.Fatalf("Remove through a work-root link: %v", err)
	}
	if _, err := os.Lstat(p); !os.IsNotExist(err) {
		t.Errorf("run root still there: %v", err)
	}
	mustExist(t, other)
	mustExist(t, canFile)
}

// TestRemoveAcceptsARelativePath: both paths are relative to the working directory, which the test sets.
func TestRemoveAcceptsARelativePath(t *testing.T) {
	wr := t.TempDir()
	p := mustMake(t, wr, "rel")
	mkfile(t, filepath.Join(p, "f.txt"))
	t.Chdir(wr)
	if err := Remove(".", filepath.Join("run-roots", filepath.Base(p))); err != nil {
		t.Fatalf("Remove with relative paths: %v", err)
	}
	if _, err := os.Lstat(p); !os.IsNotExist(err) {
		t.Errorf("run root still there: %v", err)
	}
}

func TestWorkRootUsesTheOverrideElseHomeAndRefusesWithNeither(t *testing.T) {
	env := func(m map[string]string) func(string) string {
		return func(k string) string { return m[k] }
	}
	got, err := WorkRoot(env(map[string]string{"NEVR_WORK_ROOT": "/x/root", "HOME": "/h"}))
	if err != nil || got != "/x/root" {
		t.Errorf("override: got %q, %v", got, err)
	}
	got, err = WorkRoot(env(map[string]string{"HOME": "/h"}))
	if err != nil || got != filepath.Join("/h", "src", "nevr-runtime-work") {
		t.Errorf("home default: got %q, %v", got, err)
	}
	if _, err := WorkRoot(env(map[string]string{})); err == nil {
		t.Error("neither NEVR_WORK_ROOT nor HOME: must be an error")
	}
}
