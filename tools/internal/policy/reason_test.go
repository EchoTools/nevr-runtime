package policy

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/testkit"
)

func write(t *testing.T, dir, rel, text string) {
	t.Helper()
	path := filepath.Join(dir, rel)
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(text), 0o644); err != nil {
		t.Fatal(err)
	}
}

// oldSource is long enough that an edit of one line leaves git's rename detection a similar file.
const oldSource = "print('line 1')\nprint('line 2')\nprint('line 3')\nprint('line 4')\nprint('line 5')\n"

// baseRepo is a repository whose origin/main holds tools/old.py (no reason in it) and whose HEAD is at origin/main.
func baseRepo(t *testing.T) (dir string, env []string, run func(args ...string) string) {
	t.Helper()
	dir, env, run = scratchRepo(t)
	write(t, dir, "tools/old.py", oldSource)
	write(t, dir, "tools/kept.py", oldSource)
	run("add", "-A")
	run("commit", "-q", "-m", "base")
	run("update-ref", "refs/remotes/origin/main", "HEAD")
	return dir, env, run
}

const goodHeader = "#!/usr/bin/env python3\n# ADR 0044 recorded reason: pywinrm has no Go counterpart that speaks NTLM message encryption\nprint('x')\n"

// A new Python file with no reason in its header is refused.
func TestAddedPythonFileWithoutAReasonFails(t *testing.T) {
	t.Parallel()
	dir, env, run := baseRepo(t)
	write(t, dir, "tools/new_tool.py", "#!/usr/bin/env python3\nprint('new')\n")
	run("add", "-A")
	run("commit", "-q", "-m", "add a python tool")
	added, skipped, err := AddedPython(dir, env)
	if err != nil || skipped != "" {
		t.Fatalf("skipped %q err %v", skipped, err)
	}
	if got := WithoutReason(dir, added); strings.Join(got, ",") != "tools/new_tool.py" {
		t.Errorf("without a reason: %v, want [tools/new_tool.py]", got)
	}
}

// A new Python file whose header carries the marker and a reason passes; so does one that is still untracked.
func TestAddedPythonFileWithARecordedReasonPasses(t *testing.T) {
	t.Parallel()
	dir, env, run := baseRepo(t)
	write(t, dir, "tools/winrm_thing.py", goodHeader)
	run("add", "-A")
	run("commit", "-q", "-m", "add a python tool with a reason")
	write(t, dir, "tools/untracked.py", goodHeader)
	added, _, err := AddedPython(dir, env)
	if err != nil {
		t.Fatal(err)
	}
	if len(added) != 2 {
		t.Fatalf("added %v, want the committed and the untracked file", added)
	}
	if got := WithoutReason(dir, added); len(got) != 0 {
		t.Errorf("files refused despite a recorded reason: %v", got)
	}
}

// A marker with no reason after it, or below the header, is not a reason.
func TestAMarkerWithoutAReasonOrBelowTheHeaderIsNotARecordedReason(t *testing.T) {
	t.Parallel()
	late := strings.Repeat("# filler\n", HeaderLines) + "# " + ReasonMarker + " a long enough reason, but too far down\n"
	for name, src := range map[string]string{
		"empty":      "# " + ReasonMarker + "\n",
		"short":      "# " + ReasonMarker + " no\n",
		"below head": late,
		"absent":     "print('x')\n",
	} {
		if HasRecordedReason(src) {
			t.Errorf("%s: counted as a recorded reason", name)
		}
	}
	if !HasRecordedReason(goodHeader) {
		t.Error("a header with the marker and a reason was refused")
	}
}

// Python files already in the tree, edited or not, are not additions: the check leaves them alone.
func TestExistingPythonFilesAreUntouchedByTheCheck(t *testing.T) {
	t.Parallel()
	dir, env, run := baseRepo(t)
	write(t, dir, "tools/old.py", strings.Replace(oldSource, "line 1", "line one", 1))
	write(t, dir, "tools/kept.py", strings.Replace(oldSource, "line 2", "line two", 1)) // edited in place, stays
	run("add", "-A")
	run("commit", "-q", "-m", "edit an existing python tool")
	run("mv", "tools/old.py", "tools/renamed.py")
	run("commit", "-q", "-m", "rename it")
	added, skipped, err := AddedPython(dir, env)
	if err != nil || skipped != "" {
		t.Fatalf("skipped %q err %v", skipped, err)
	}
	if len(added) != 0 {
		t.Errorf("existing files reported as added: %v", added)
	}
}

func TestAddedPythonSkipsWithAReasonWithoutOriginMain(t *testing.T) {
	t.Parallel()
	dir, env, _ := scratchRepo(t)
	_, skipped, err := AddedPython(dir, env)
	if err != nil || !strings.Contains(skipped, "origin/main") {
		t.Errorf("skipped %q err %v", skipped, err)
	}
}

// The check on this tree: every Python file this change adds under tools/ records why it is not Go.
func TestEveryPythonFileAddedSinceTheMergeBaseRecordsAReason(t *testing.T) {
	t.Parallel()
	root := repoRoot(t)
	added, skipped, err := AddedPython(root, testkit.Env(t))
	if err != nil {
		t.Fatal(err)
	}
	if skipped != "" {
		t.Skip(skipped)
	}
	for _, f := range WithoutReason(root, added) {
		t.Errorf("%s is a new Python file under tools/ with no recorded reason: a new tool or test is Go (ADR 0044, rule 1); "+
			"if Python is needed, put %q and the reason in the first %d lines", f, "# "+ReasonMarker+" <reason>", HeaderLines)
	}
}

// A shallow clone has origin/main and no merge-base: finding the added files is impossible, and that must be an error,
// not an empty list that looks like a pass.
func TestAddedPythonFailsWhenOriginMainExistsButHasNoMergeBase(t *testing.T) {
	t.Parallel()
	dir, env, run := baseRepo(t)
	run("checkout", "-q", "--orphan", "other")
	write(t, dir, "tools/other.py", "print('other')\n")
	run("add", "-A")
	run("commit", "-q", "-m", "unrelated history")
	added, skipped, err := AddedPython(dir, env)
	if err == nil || skipped != "" || added != nil {
		t.Fatalf("added %v skipped %q err %v, want an error", added, skipped, err)
	}
	for _, want := range []string{"merge-base", "fetch"} {
		if !strings.Contains(err.Error(), want) {
			t.Errorf("error %q lacks %q", err, want)
		}
	}
}

// A docstring that ends the marker line with quotes still counts, and quotes alone are no reason.
func TestRecordedReasonInsideADocstringCountsButQuotesAloneDoNot(t *testing.T) {
	t.Parallel()
	if !HasRecordedReason("\"\"\"ADR 0044 recorded reason: pywinrm has no maintained Go twin\"\"\"\n") {
		t.Error("a reason inside a docstring was refused")
	}
	if HasRecordedReason("\"\"\"ADR 0044 recorded reason: " + strings.Repeat("\"", 12) + "\n") {
		t.Error("a run of quotes counted as a reason")
	}
}

// A file that cannot be read is not reasoned, so it is reported rather than waved through.
func TestAnUnreadableAddedFileIsReportedWithoutAReason(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	if got := WithoutReason(dir, []string{"tools/missing.py"}); len(got) != 1 {
		t.Errorf("WithoutReason of a missing file = %v, want it reported", got)
	}
}

// A file name with a space is one name, not two.
func TestAddedPythonKeepsANameWithASpaceWhole(t *testing.T) {
	t.Parallel()
	dir, env, run := baseRepo(t)
	write(t, dir, "tools/two words.py", "print('x')\n")
	write(t, dir, "tools/second.py", "print('y')\n")
	run("add", "-A")
	run("commit", "-q", "-m", "add a file with a space in its name")
	added, _, err := AddedPython(dir, env)
	if err != nil || len(added) != 2 || added[0] != "tools/second.py" || added[1] != "tools/two words.py" {
		t.Errorf("added %q err %v", added, err)
	}
}

// Only a .py file under tools/ is an addition: a text file there and a .py file elsewhere are ignored.
func TestAddedPythonIgnoresOtherExtensionsAndOtherDirectories(t *testing.T) {
	t.Parallel()
	dir, env, run := baseRepo(t)
	write(t, dir, "tools/notes.txt", "x\n")
	write(t, dir, "src/outside.py", "print('x')\n")
	run("add", "-A")
	run("commit", "-q", "-m", "add files the check does not cover")
	write(t, dir, "tools/untracked.txt", "x\n")
	write(t, dir, "src/untracked.py", "print('x')\n")
	added, _, err := AddedPython(dir, env)
	if err != nil || len(added) != 0 {
		t.Errorf("added %q err %v, want none", added, err)
	}
}

// A git command that fails while listing the added files is an error, not an empty list that looks like a pass.
func TestAddedPythonFailsWhenGitCannotListTheFiles(t *testing.T) {
	t.Parallel()
	dir, env, _ := baseRepo(t)
	if err := os.WriteFile(filepath.Join(dir, ".git", "index"), []byte("not an index"), 0o644); err != nil {
		t.Fatal(err)
	}
	added, skipped, err := AddedPython(dir, env)
	if err == nil || skipped != "" || added != nil {
		t.Fatalf("added %v skipped %q err %v, want an error", added, skipped, err)
	}
	if !strings.Contains(err.Error(), "git ") {
		t.Errorf("error %q does not name the git command", err)
	}
}
