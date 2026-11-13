package main

import (
	"bufio"
	"bytes"
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/testkit"
)

// evidenceBody is the rest of a message that carries what "What a landed commit carries" lists.
const evidenceBody = ` (Refs #1)

## Search statement
- ` + "`git grep -n x`" + `: one hit.

## Siblings
Shape: none; ` + "`git grep -n y`" + `: no other instance.

## Symbols
Edited: c.txt.

## Commands and results
- ` + "`go test`" + `: exit 0.

## Mutants
None: test fixture.

## Platform parity
Fixture.

Co-Authored-By: T <t@example.org>
`

// repos is a bare origin, a main checkout and a linked worktree on branch work with two commits of its own.
type repos struct {
	t                      *testing.T
	env                    []string
	root, origin, main, wt string
}

func setup(t *testing.T) *repos {
	t.Helper()
	root := t.TempDir()
	r := &repos{t: t, env: testkit.Env(t), root: root, origin: filepath.Join(root, "origin.git"),
		main: filepath.Join(root, "main"), wt: filepath.Join(root, "wt")}
	r.git(root, "init", "--bare", "-b", "main", r.origin)
	r.git(root, "clone", r.origin, r.main)
	r.commit(r.main, "a.txt", "chore: base")
	r.git(r.main, "push", "origin", "main")
	r.git(r.main, "worktree", "add", "--no-track", "-b", "work", r.wt, "origin/main")
	r.commit(r.wt, "b.txt", "feat: one")
	r.commit(r.wt, "c.txt", "fix: two"+evidenceBody)
	return r
}

func (r *repos) git(dir string, args ...string) string {
	r.t.Helper()
	cmd := exec.Command("git", args...)
	cmd.Dir, cmd.Env = dir, r.env
	out, err := cmd.CombinedOutput()
	if err != nil {
		r.t.Fatalf("git %v: %v\n%s", args, err, out)
	}
	return strings.TrimSpace(string(out))
}

func (r *repos) commit(dir, name, msg string) {
	r.t.Helper()
	if err := os.WriteFile(filepath.Join(dir, name), []byte(msg), 0o644); err != nil {
		r.t.Fatal(err)
	}
	r.git(dir, "add", name)
	r.git(dir, "commit", "-q", "-m", msg)
}

func (r *repos) originMain() string { return r.git(r.root, "--git-dir", r.origin, "rev-parse", "main") }

func (r *repos) land(dir string, args ...string) (int, string) {
	var out, errb bytes.Buffer
	code := run(r.t.Context(), append([]string{"-C", dir}, args...), r.env, &out, &errb)
	return code, errb.String()
}

func (r *repos) records() []map[string]any {
	r.t.Helper()
	path := filepath.Join(testkit.Getenv(r.env)("XDG_STATE_HOME"), "nevr-runtime", "land.jsonl")
	f, err := os.Open(path)
	if err != nil {
		r.t.Fatalf("the landing log: %v", err)
	}
	defer f.Close()
	var recs []map[string]any
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		var m map[string]any
		if err := json.Unmarshal(sc.Bytes(), &m); err != nil {
			r.t.Fatalf("a log line is not JSON: %q", sc.Text())
		}
		recs = append(recs, m)
	}
	return recs
}

func (r *repos) last() map[string]any { recs := r.records(); return recs[len(recs)-1] }

func TestLandsTheBranchAndRecordsMainBeforeAndAfter(t *testing.T) {
	t.Parallel()
	r := setup(t)
	before := r.originMain()
	if code, errs := r.land(r.wt, "-test", "true"); code != 0 {
		t.Fatalf("exit %d: %s", code, errs)
	}
	tip := r.git(r.wt, "rev-parse", "HEAD")
	if got := r.originMain(); got != tip {
		t.Fatalf("origin's main is %s, want the tip %s", got, tip)
	}
	done := r.last()
	if done["event"] != "land_done" || done["outcome"] != "landed" || done["main_before"] != before ||
		done["main_after"] != tip || done["main_checkout"] != "fast-forwarded" {
		t.Fatalf("last record %v", done)
	}
	if got := r.git(r.main, "rev-parse", "HEAD"); got != tip {
		t.Fatalf("the main checkout is at %s, want %s", got, tip)
	}
}

func TestAMovedMainIsRebasedUnderEachCommitAndEveryMessageIsKept(t *testing.T) {
	t.Parallel()
	r := setup(t)
	r.commit(r.main, "d.txt", "docs: elsewhere")
	r.git(r.main, "push", "origin", "main")
	if code, errs := r.land(r.wt, "-test", "true"); code != 0 {
		t.Fatalf("exit %d: %s", code, errs)
	}
	subjects := r.git(r.root, "--git-dir", r.origin, "log", "--format=%s", "main")
	if want := "fix: two (Refs #1)\nfeat: one\ndocs: elsewhere\nchore: base"; subjects != want {
		t.Fatalf("origin's history:\n%s\nwant:\n%s", subjects, want)
	}
	if merges := r.git(r.root, "--git-dir", r.origin, "rev-list", "--merges", "main"); merges != "" {
		t.Fatalf("a merge commit landed: %s", merges)
	}
	if r.last()["rebased"] != true {
		t.Fatalf("the record does not say the branch was rebased: %v", r.last())
	}
}

func TestAFailingTestStopsBeforeThePush(t *testing.T) {
	t.Parallel()
	r := setup(t)
	before := r.originMain()
	if code, _ := r.land(r.wt, "-test", "true", "-test", "exit 3"); code != 5 {
		t.Fatalf("exit %d, want 5", code)
	}
	if r.originMain() != before {
		t.Fatal("origin's main moved after a failed test")
	}
	if rec := r.last(); rec["outcome"] != "tests_failed" || rec["rc"] != float64(3) {
		t.Fatalf("last record %v", rec)
	}
}

func TestTheMainCheckoutIsRefused(t *testing.T) {
	t.Parallel()
	r := setup(t)
	if code, _ := r.land(r.main, "-test", "true"); code != 2 {
		t.Fatalf("exit %d, want 2", code)
	}
	if reason, _ := r.last()["reason"].(string); !strings.Contains(reason, "main checkout") {
		t.Fatalf("reason %q", reason)
	}
}

func TestADirtyWorktreeAndARunWithoutTestsAreRefused(t *testing.T) {
	t.Parallel()
	r := setup(t)
	if code, _ := r.land(r.wt); code != 2 {
		t.Fatalf("no -test: exit %d, want 2", code)
	}
	if err := os.WriteFile(filepath.Join(r.wt, "b.txt"), []byte("changed"), 0o644); err != nil {
		t.Fatal(err)
	}
	if code, _ := r.land(r.wt, "-test", "true"); code != 2 {
		t.Fatalf("dirty: exit %d, want 2", code)
	}
	if reason, _ := r.last()["reason"].(string); !strings.Contains(reason, "uncommitted") {
		t.Fatalf("reason %q", reason)
	}
}

func TestAHeldLockKeepsASecondLanderOut(t *testing.T) {
	t.Parallel()
	r := setup(t)
	before := r.originMain()
	common := r.git(r.wt, "rev-parse", "--path-format=absolute", "--git-common-dir")
	f, err := os.OpenFile(filepath.Join(common, lockName), os.O_RDWR|os.O_CREATE, 0o644)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	if err := syscall.Flock(int(f.Fd()), syscall.LOCK_EX); err != nil {
		t.Fatal(err)
	}
	if code, _ := r.land(r.wt, "-lock-wait", "300ms", "-test", "true"); code != 3 {
		t.Fatalf("exit %d, want 3", code)
	}
	if r.originMain() != before {
		t.Fatal("origin's main moved while another lander held the lock")
	}
	for _, rec := range r.records() {
		if rec["event"] == "land_locked" {
			t.Fatal("the second lander recorded taking the lock")
		}
	}
	if r.last()["outcome"] != "lock_timeout" {
		t.Fatalf("last record %v", r.last())
	}
}

func TestARejectedPushLeavesOriginAloneAndSaysSo(t *testing.T) {
	t.Parallel()
	r := setup(t)
	// A pre-receive hook stands in for a lander that moved main between this one's fetch and its push.
	hook := filepath.Join(r.origin, "hooks", "pre-receive")
	if err := os.WriteFile(hook, []byte("#!/bin/sh\nexit 1\n"), 0o755); err != nil {
		t.Fatal(err)
	}
	before := r.originMain()
	if code, _ := r.land(r.wt, "-test", "true"); code != 6 {
		t.Fatalf("exit %d, want 6", code)
	}
	if r.originMain() != before {
		t.Fatal("origin's main moved after a rejected push")
	}
	if r.last()["outcome"] != "push_rejected" {
		t.Fatalf("last record %v", r.last())
	}
}

// Planted violation: the tip commit has no evidence, so nothing is pushed and the refusal names what is missing.
func TestPlantedTipWithoutEvidenceIsRefusedBeforeTheTests(t *testing.T) {
	r := setup(t)
	r.commit(r.wt, "e.txt", "fix: bare")
	before := r.originMain()
	code, errText := r.land(r.wt, "-test", "true")
	if code != exitEvidenceMissing || !strings.Contains(errText, "## Search statement") || !strings.Contains(errText, "landed-commit-evidence") {
		t.Fatalf("code %d: %s", code, errText)
	}
	if r.originMain() != before {
		t.Fatal("a commit without evidence was pushed")
	}
	if got := r.last()["outcome"]; got != "evidence_missing" {
		t.Fatalf("outcome %v", got)
	}
}
