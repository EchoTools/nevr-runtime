package main

import (
	"bytes"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// privateMode is the mode of the files the tests make.
const (
	privateMode    = 0o600
	privateDirMode = 0o750
)

const ledgerHeader = "| artifact | owner | purpose | remove-when |\n|---|---|---|---|\n"

// runGit runs git in dir and fails the test on a nonzero exit.
func runGit(t *testing.T, dir string, args ...string) string {
	t.Helper()
	cmd := exec.Command("git", append([]string{"-C", dir}, args...)...)
	cmd.Env = append(os.Environ(), "GIT_CONFIG_GLOBAL=/dev/null", "GIT_CONFIG_SYSTEM=/dev/null",
		"GIT_AUTHOR_NAME=t", "GIT_AUTHOR_EMAIL=t@example.com", "GIT_COMMITTER_NAME=t", "GIT_COMMITTER_EMAIL=t@example.com")
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("git %v: %v\n%s", args, err, out)
	}
	return strings.TrimSpace(string(out))
}

// repoWithOrigin makes a checkout whose origin holds main, and returns the checkout.
func repoWithOrigin(t *testing.T) string {
	t.Helper()
	root := t.TempDir()
	origin := filepath.Join(root, "origin.git")
	main := filepath.Join(root, "main")
	runGit(t, root, "init", "-q", "--bare", "-b", "main", origin)
	runGit(t, root, "init", "-q", "-b", "main", main)
	if err := os.WriteFile(filepath.Join(main, "f"), []byte("x"), privateMode); err != nil {
		t.Fatal(err)
	}
	runGit(t, main, "add", "f")
	runGit(t, main, "-c", "commit.gpgsign=false", "commit", "-q", "-m", "init")
	runGit(t, main, "remote", "add", "origin", origin)
	runGit(t, main, "push", "-q", "origin", "main")
	return main
}

func call(t *testing.T, env map[string]string, args ...string) (int, string, string) {
	t.Helper()
	full := map[string]string{"XDG_STATE_HOME": t.TempDir()}
	for k, v := range env {
		full[k] = v
	}
	var out, errb bytes.Buffer
	code := run(args, func(k string) string { return full[k] }, &out, &errb)
	return code, out.String(), errb.String()
}

func TestCreatesTheWorktreeLockDescriptionAndLedgerRow(t *testing.T) {
	main := repoWithOrigin(t)
	ledger := filepath.Join(t.TempDir(), "LEDGER.md")
	if err := os.WriteFile(ledger, []byte(ledgerHeader), privateMode); err != nil {
		t.Fatal(err)
	}
	code, _, errs := call(t, map[string]string{"REAP_LEDGER": ledger}, "-repo", main, "-purpose", "a test unit", "seat-a/12-slug")
	if code != 0 {
		t.Fatalf("exit %d: %s", code, errs)
	}
	wt := filepath.Join(main, ".claude", "worktrees", "seat-a", "12-slug")
	if got := runGit(t, wt, "rev-parse", "HEAD"); got != runGit(t, main, "rev-parse", "origin/main") {
		t.Fatalf("worktree HEAD %s is not origin/main", got)
	}
	if got := runGit(t, wt, "branch", "--show-current"); got != "seat-a/12-slug" {
		t.Fatalf("branch %q", got)
	}
	if out, err := exec.Command("git", "-C", main, "config", "--get", "branch.seat-a/12-slug.remote").Output(); err == nil {
		t.Fatalf("branch tracks an upstream: %s", out)
	}
	if got := runGit(t, main, "config", "branch.seat-a/12-slug.description"); !strings.HasPrefix(got, "seat-a: delete when it lands") {
		t.Fatalf("description %q", got)
	}
	if list := runGit(t, main, "worktree", "list", "--porcelain"); !strings.Contains(list, "locked seat-a: delete when it lands") {
		t.Fatalf("worktree not locked with a reason:\n%s", list)
	}
	row, err := os.ReadFile(ledger)
	if err != nil {
		t.Fatal(err)
	}
	want := ledgerHeader + "| .claude/worktrees/seat-a/12-slug (branch seat-a/12-slug) | seat-a | a test unit | when it lands on main or is abandoned |\n"
	if string(row) != want {
		t.Fatalf("ledger:\n%s\nwant:\n%s", row, want)
	}
}

func TestDefaultLedgerIsMadeInTheWorkRoot(t *testing.T) {
	main := repoWithOrigin(t)
	wr := filepath.Join(t.TempDir(), "wr")
	code, _, errs := call(t, map[string]string{"NEVR_WORK_ROOT": wr}, "-repo", main, "seat-b/x")
	if code != 0 {
		t.Fatalf("exit %d: %s", code, errs)
	}
	got, err := os.ReadFile(filepath.Join(wr, "LEDGER.md"))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.HasPrefix(string(got), ledgerHeader+"| .claude/worktrees/seat-b/x (branch seat-b/x) | seat-b |") {
		t.Fatalf("default ledger:\n%s", got)
	}
}

func TestAnEmptyReapLedgerIsTheDefault(t *testing.T) {
	main := repoWithOrigin(t)
	wr := filepath.Join(t.TempDir(), "wr")
	env := map[string]string{"NEVR_WORK_ROOT": wr, "REAP_LEDGER": ""}
	if code, _, errs := call(t, env, "-repo", main, "seat-c/x"); code != 0 {
		t.Fatalf("exit %d: %s", code, errs)
	}
	got, err := os.ReadFile(filepath.Join(wr, "LEDGER.md"))
	if err != nil || !strings.HasPrefix(string(got), ledgerHeader+"| .claude/worktrees/seat-c/x ") {
		t.Fatalf("ledger %q, err %v", got, err)
	}
}

func TestABranchNameGitRejectsIsRefusedBeforeAnyLedgerIsMade(t *testing.T) {
	main := repoWithOrigin(t)
	wr := filepath.Join(t.TempDir(), "wr")
	for _, name := range []string{"seat-a/b..c", "seat-a/x.lock"} {
		if code, _, _ := call(t, map[string]string{"NEVR_WORK_ROOT": wr}, "-repo", main, name); code == 0 {
			t.Errorf("%s: exit 0", name)
		}
	}
	if _, err := os.Stat(wr); err == nil {
		t.Fatal("a refusal made the work root")
	}
}

func TestANamedLedgerThatCannotBeAppendedToIsRefusedBeforeTheWorktree(t *testing.T) {
	main := repoWithOrigin(t)
	dir := t.TempDir() // a directory cannot be appended to
	if code, _, _ := call(t, map[string]string{"REAP_LEDGER": dir}, "-repo", main, "seat-a/x"); code == 0 {
		t.Fatal("exit 0 for a ledger that is a directory")
	}
	if list := runGit(t, main, "worktree", "list", "--porcelain"); strings.Count(list, "worktree ") != 1 {
		t.Fatalf("a refused ledger left a worktree:\n%s", list)
	}
}

func TestADefaultLedgerThatCannotBeAppendedToIsRefusedBeforeTheWorktree(t *testing.T) {
	main := repoWithOrigin(t)
	wr := t.TempDir()
	if err := os.Mkdir(filepath.Join(wr, "LEDGER.md"), privateDirMode); err != nil { // a directory cannot be appended to
		t.Fatal(err)
	}
	if code, _, _ := call(t, map[string]string{"NEVR_WORK_ROOT": wr}, "-repo", main, "seat-a/x"); code == 0 {
		t.Fatal("exit 0 for a default ledger that is a directory")
	}
	if list := runGit(t, main, "worktree", "list", "--porcelain"); strings.Count(list, "worktree ") != 1 {
		t.Fatalf("a refused ledger left a worktree:\n%s", list)
	}
}

func TestRefusalsChangeNothing(t *testing.T) {
	main := repoWithOrigin(t)
	ledger := filepath.Join(t.TempDir(), "LEDGER.md")
	if err := os.WriteFile(ledger, []byte(ledgerHeader), privateMode); err != nil {
		t.Fatal(err)
	}
	env := map[string]string{"REAP_LEDGER": ledger}
	cases := map[string][]string{
		"no seat prefix":           {"-repo", main, "slug-only"},
		"three components":         {"-repo", main, "a/b/c"},
		"pipe in purpose":          {"-repo", main, "-purpose", "a | b", "seat-a/x"},
		"no argument":              {"-repo", main},
		"unsafe character":         {"-repo", main, "seat-a/x y"},
		"dot in the seat":          {"-repo", main, "seat.a/x"},
		"a named ledger is absent": {"-repo", main, "seat-a/y"},
	}
	for name, args := range cases {
		e := env
		if name == "a named ledger is absent" {
			e = map[string]string{"REAP_LEDGER": filepath.Join(t.TempDir(), "absent.md")}
		}
		if code, _, _ := call(t, e, args...); code == 0 {
			t.Errorf("%s: exit 0, want a refusal", name)
		}
	}
	if list := runGit(t, main, "worktree", "list", "--porcelain"); strings.Count(list, "worktree ") != 1 {
		t.Fatalf("a refusal made a worktree:\n%s", list)
	}
	if got, _ := os.ReadFile(ledger); string(got) != ledgerHeader {
		t.Fatalf("a refusal wrote the ledger:\n%s", got)
	}
}

func TestExistingBranchOrPathIsRefusedBeforeAnyLedgerIsMade(t *testing.T) {
	main := repoWithOrigin(t)
	runGit(t, main, "branch", "seat-a/taken")
	wr := filepath.Join(t.TempDir(), "wr")
	if code, _, _ := call(t, map[string]string{"NEVR_WORK_ROOT": wr}, "-repo", main, "seat-a/taken"); code == 0 {
		t.Fatal("exit 0 for an existing branch")
	}
	if err := os.MkdirAll(filepath.Join(main, ".claude", "worktrees", "seat-a", "dir"), privateDirMode); err != nil {
		t.Fatal(err)
	}
	if code, _, _ := call(t, map[string]string{"NEVR_WORK_ROOT": wr}, "-repo", main, "seat-a/dir"); code == 0 {
		t.Fatal("exit 0 for an existing path")
	}
	if _, err := os.Stat(wr); err == nil {
		t.Fatal("a refusal made the work root")
	}
}

func TestExistingBranchIsRefusedBeforeTheLedgerIsWritten(t *testing.T) {
	main := repoWithOrigin(t)
	runGit(t, main, "branch", "seat-a/taken")
	ledger := filepath.Join(t.TempDir(), "LEDGER.md")
	if err := os.WriteFile(ledger, []byte(ledgerHeader), privateMode); err != nil {
		t.Fatal(err)
	}
	code, _, _ := call(t, map[string]string{"REAP_LEDGER": ledger}, "-repo", main, "seat-a/taken")
	if code == 0 {
		t.Fatal("exit 0 for an existing branch")
	}
	if got, _ := os.ReadFile(ledger); string(got) != ledgerHeader {
		t.Fatalf("ledger written for a refused worktree:\n%s", got)
	}
}
