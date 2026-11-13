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

func w(t *testing.T, p, c string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(p), dirPerm); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(p, []byte(c), filePerm); err != nil {
		t.Fatal(err)
	}
}

// nevrRoot is the nevr root inside the Wine prefix of a copy, relative to echovr/.
const nevrRoot = ".wineprefix/drive_c/users/u/AppData/Local/nevr/echovr"

// fixture: <work>/copy/echovr with echovr.exe, and <work>/install beside it.
func fixture(t *testing.T) (work, copyDir, src string) {
	t.Helper()
	work = t.TempDir()
	copyDir = filepath.Join(work, "game-copy")
	src = filepath.Join(work, "install")
	w(t, filepath.Join(copyDir, "echovr", "bin", "win10", "echovr.exe"), "x")
	w(t, filepath.Join(src, "f"), "x")
	return
}

func do(work, copyDir, src string) (int, string) {
	var o, e bytes.Buffer
	c := execute([]string{"-copy", copyDir, "-src", src, "-work-root", work}, &o, &e)
	return c, o.String() + e.String()
}

func TestGreenCopy(t *testing.T) {
	work, c, s := fixture(t)
	// a link to / leaves the copy: listed, not refused
	if err := os.Symlink("/", filepath.Join(c, "echovr", "z:")); err != nil {
		t.Fatal(err)
	}
	code, out := do(work, c, s)
	if code != 0 || !strings.Contains(out, "leaves-copy:") {
		t.Fatalf("%d %s", code, out)
	}
}

// Planted violations, one per refusal.
func TestPlantedViolationsAreRefused(t *testing.T) {
	cases := map[string]func(t *testing.T, work, c, s string){
		"credentials": func(t *testing.T, _, c, _ string) {
			w(t, filepath.Join(c, "echovr", "x", ".credentials.json.bak"), "k")
		},
		"config": func(t *testing.T, _, c, _ string) { w(t, filepath.Join(c, "echovr", "_local", "config.yaml"), "k") },
		"plugin": func(t *testing.T, _, c, _ string) {
			w(t, filepath.Join(c, "echovr", "bin", "win10", "plugins", "p.dll"), "k")
		},
		"no exe": func(t *testing.T, _, c, _ string) {
			os.Remove(filepath.Join(c, "echovr", "bin", "win10", "echovr.exe"))
		},
		"nevr root config": func(t *testing.T, _, c, _ string) {
			w(t, filepath.Join(c, "echovr", nevrRoot, "config.yaml"), "k")
		},
		"nevr root credentials": func(t *testing.T, _, c, _ string) {
			w(t, filepath.Join(c, "echovr", nevrRoot, ".credentials.json"), "k")
		},
		"link into install": func(t *testing.T, _, c, s string) {
			if err := os.Symlink(filepath.Join(s, "f"), filepath.Join(c, "echovr", "content")); err != nil {
				t.Fatal(err)
			}
		},
	}
	for name, plant := range cases {
		work, c, s := fixture(t)
		plant(t, work, c, s)
		if code, out := do(work, c, s); code != 1 || !strings.Contains(out, "refused:") {
			t.Errorf("%s: code %d: %s", name, code, out)
		}
	}
}

func TestCopyOutsideWorkRootIsRefused(t *testing.T) {
	_, c, s := fixture(t)
	if code, out := do(t.TempDir(), c, s); code != 1 || !strings.Contains(out, "work root") {
		t.Fatalf("%d %s", code, out)
	}
}

func TestBadFlag(t *testing.T) {
	var o, e bytes.Buffer
	if execute(nil, &o, &e) != exitUsage {
		t.Fatal("want 2")
	}
}

// Planted: the same link into the install, with -src and -copy given as relative paths.
func TestPlantedLinkIntoInstallWithRelativeSrcIsRefused(t *testing.T) {
	work, c, s := fixture(t)
	if err := os.Symlink(filepath.Join(s, "f"), filepath.Join(c, "echovr", "content")); err != nil {
		t.Fatal(err)
	}
	t.Chdir(work)
	var o, e bytes.Buffer
	code := execute([]string{"-copy", "game-copy", "-src", "install", "-work-root", work}, &o, &e)
	if code != 1 || !strings.Contains(o.String(), "resolves into the install") {
		t.Fatalf("%d %s%s", code, o.String(), e.String())
	}
}

// Planted violation of the logging rule: a run that leaves no record in the tools log fails here. The run is the real
// command through its own entry point, on a state directory of the test's own.
func TestARunIsRecordedInTheToolsLog(t *testing.T) {
	env := testkit.Env(t)
	work, c, s := fixture(t)
	args := []string{"-copy", c, "-src", s, "-work-root", work}
	var o, e bytes.Buffer
	code := run(args, testkit.Getenv(env), &o, &e)
	if code != 0 {
		t.Fatalf("exit %d: %s%s", code, o.String(), e.String())
	}
	testkit.RequireRunRecorded(t, env, "run-copy", code, args)
}

// Planted: without -src the link check cannot run, so the copy is refused and the missing flag is named, even when a link
// into the install is present.
func TestPlantedMissingSrcIsRefusedNamingTheFlag(t *testing.T) {
	work, c, s := fixture(t)
	if err := os.Symlink(filepath.Join(s, "f"), filepath.Join(c, "echovr", "content")); err != nil {
		t.Fatal(err)
	}
	var o, e bytes.Buffer
	code := execute([]string{"-copy", c, "-work-root", work}, &o, &e)
	if code != exitUsage || !strings.Contains(e.String(), "-src is required") {
		t.Fatalf("%d %s%s", code, o.String(), e.String())
	}
}

// Planted: a directory that cannot be read could hold a sign-in file, so it is a finding and not a skipped subtree.
func TestPlantedUnreadableSubtreeIsAFinding(t *testing.T) {
	if os.Geteuid() == 0 {
		t.Skip("root reads every directory")
	}
	work, c, s := fixture(t)
	hidden := filepath.Join(c, "echovr", "hidden")
	w(t, filepath.Join(hidden, ".credentials.json"), "k")
	if err := os.Chmod(hidden, 0); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { os.Chmod(hidden, dirPerm) })
	code, out := do(work, c, s)
	if code != 1 || !strings.Contains(out, "refused: unreadable") {
		t.Fatalf("%d %s", code, out)
	}
}
