package main

import (
	"bufio"
	"bytes"
	"crypto/sha256"
	"encoding/hex"
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

func (r *repos) hasOutcome(outcome string) bool {
	for _, rec := range r.records() {
		if rec["outcome"] == outcome {
			return true
		}
	}
	return false
}

func (r *repos) last() map[string]any { recs := r.records(); return recs[len(recs)-1] }

func TestLandsTheBranchAndRecordsMainBeforeAndAfter(t *testing.T) {
	t.Parallel()
	r := setup(t)
	before := r.originMain()
	if code, errs := r.land(r.wt, "-review", r.mergeReview(), "-test", "true"); code != 0 {
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
	if code, errs := r.land(r.wt, "-review", r.mergeReview(), "-test", "true"); code != 0 {
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
	if code, _ := r.land(r.wt, "-review", r.mergeReview(), "-test", "true", "-test", "exit 3"); code != 5 {
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
	if code, _ := r.land(r.wt, "-review", r.mergeReview(), "-lock-wait", "300ms", "-test", "true"); code != 3 {
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
	if code, _ := r.land(r.wt, "-review", r.mergeReview(), "-test", "true"); code != 6 {
		t.Fatalf("exit %d, want 6", code)
	}
	if r.originMain() != before {
		t.Fatal("origin's main moved after a rejected push")
	}
	// The branch is put back at the reviewed commit after the refusal, so the refusal is not the last record.
	if !r.hasOutcome("push_rejected") {
		t.Fatalf("no push_rejected record in %v", r.records())
	}
}

// Planted violation: the tip commit has no evidence, so nothing is pushed and the refusal names what is missing.
func TestPlantedTipWithoutEvidenceIsRefusedBeforeTheTests(t *testing.T) {
	r := setup(t)
	r.commit(r.wt, "e.txt", "fix: bare")
	before := r.originMain()
	code, errText := r.land(r.wt, "-review", r.mergeReview(), "-test", "true")
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

// writeReview writes a review report whose verdict line is "Verdict: <verdict> <sha>" and returns its path.
func (r *repos) writeReview(verdict, sha string) string {
	r.t.Helper()
	path := filepath.Join(r.root, "review-report.md")
	// CRLF line ends, as an editor on Windows writes them: the verdict line is read without its \r.
	body := "# Review\r\n\r\nFindings: none.\r\n\r\n" + reviewVerdictPrefix + verdict + " " + sha + "\r\n"
	if err := os.WriteFile(path, []byte(body), 0o644); err != nil {
		r.t.Fatal(err)
	}
	return path
}

// mergeReview is a MERGE report of the worktree's HEAD.
func (r *repos) mergeReview() string { return r.writeReview("MERGE", r.git(r.wt, "rev-parse", "HEAD")) }

// assertRefusedBeforeTheLock checks a review refusal: its exit code, its line, origin untouched, no lock taken.
func (r *repos) assertRefusedBeforeTheLock(code int, errText, want string) {
	r.t.Helper()
	if code != exitReviewRefused || !strings.Contains(errText, want) || strings.Count(strings.TrimSpace(errText), "\n") != 0 {
		r.t.Fatalf("code %d, want %d with one line containing %q: %q", code, exitReviewRefused, want, errText)
	}
	for _, rec := range r.records() {
		if rec["event"] == "land_locked" {
			r.t.Fatal("the lock was taken before the review was checked")
		}
	}
	if r.last()["outcome"] != "review_refused" {
		r.t.Fatalf("last record %v", r.last())
	}
}

func TestReviewGateNoReviewFlagIsRefused(t *testing.T) {
	t.Parallel()
	r := setup(t)
	before := r.originMain()
	code, errText := r.land(r.wt, "-test", "true")
	r.assertRefusedBeforeTheLock(code, errText, "no -review")
	if r.originMain() != before {
		t.Fatal("origin's main moved without a review")
	}
}

func TestReviewGateMissingReportIsRefused(t *testing.T) {
	t.Parallel()
	r := setup(t)
	code, errText := r.land(r.wt, "-review", filepath.Join(r.root, "absent.md"), "-test", "true")
	r.assertRefusedBeforeTheLock(code, errText, "cannot be read")
}

func TestReviewGateFixVerdictIsRefused(t *testing.T) {
	t.Parallel()
	r := setup(t)
	path := r.writeReview("FIX", r.git(r.wt, "rev-parse", "HEAD"))
	code, errText := r.land(r.wt, "-review", path, "-test", "true")
	r.assertRefusedBeforeTheLock(code, errText, "the verdict is FIX")
}

func TestReviewGateMergeOfAnotherTipIsRefused(t *testing.T) {
	t.Parallel()
	r := setup(t)
	other := r.git(r.wt, "rev-parse", "HEAD~1")
	head := r.git(r.wt, "rev-parse", "HEAD")
	code, errText := r.land(r.wt, "-review", r.writeReview("MERGE", other), "-test", "true")
	r.assertRefusedBeforeTheLock(code, errText, "the review is of "+other+", HEAD is "+head)
}

func TestReviewGateMergeOfTheTipLandsWithTheReviewTrailer(t *testing.T) {
	t.Parallel()
	r := setup(t)
	reviewed := r.git(r.wt, "rev-parse", "HEAD")
	path := r.writeReview("MERGE", reviewed)
	if code, errs := r.land(r.wt, "-review", path, "-test", "true"); code != 0 {
		t.Fatalf("exit %d: %s", code, errs)
	}
	trailer := r.git(r.root, "--git-dir", r.origin, "log", "-1", "--format=%(trailers:key="+reviewTrailerKey+",valueonly)", "main")
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	sum := sha256.Sum256(data)
	want := "<scratch>/review-report.md " + reviewVerdictPrefix + "MERGE " + reviewed + " sha256=" + hex.EncodeToString(sum[:])
	if trailer != want {
		t.Fatalf("the landed commit's review trailer is %q, want %q", trailer, want)
	}
	if got := r.git(r.wt, "rev-parse", "HEAD"); got != r.originMain() {
		t.Fatalf("the branch is at %s, origin's main at %s", got, r.originMain())
	}
}

// After a push origin refuses, the branch is back at the reviewed commit, so the same review lands it on the next run.
func TestReviewGateARejectedPushLeavesTheBranchAtTheReviewedCommit(t *testing.T) {
	t.Parallel()
	r := setup(t)
	r.commit(r.main, "d.txt", "docs: elsewhere")
	r.git(r.main, "push", "origin", "main")
	reviewed := r.git(r.wt, "rev-parse", "HEAD")
	path := r.writeReview("MERGE", reviewed)
	hook := filepath.Join(r.origin, "hooks", "pre-receive")
	if err := os.WriteFile(hook, []byte("#!/bin/sh\nexit 1\n"), 0o755); err != nil {
		t.Fatal(err)
	}
	if code, _ := r.land(r.wt, "-review", path, "-test", "true"); code != 6 {
		t.Fatalf("exit %d, want 6", code)
	}
	if got := r.git(r.wt, "rev-parse", "HEAD"); got != reviewed {
		t.Fatalf("after the refused push the branch is at %s, want the reviewed %s", got, reviewed)
	}
	if err := os.Remove(hook); err != nil {
		t.Fatal(err)
	}
	if code, errs := r.land(r.wt, "-review", path, "-test", "true"); code != 0 {
		t.Fatalf("the second run: exit %d: %s", code, errs)
	}
}

// A report must hold exactly one verdict line, and it must name a full sha.
func TestReviewGateAReportWithoutOneWellFormedVerdictLineIsRefused(t *testing.T) {
	t.Parallel()
	r := setup(t)
	head := r.git(r.wt, "rev-parse", "HEAD")
	line := reviewVerdictPrefix + "MERGE " + head + "\n"
	for name, body := range map[string]string{
		"none":      "# Review\nMERGE\n",
		"two":       line + line,
		"short sha": reviewVerdictPrefix + "MERGE " + head[:12] + "\n",
		"no sha":    reviewVerdictPrefix + "MERGE\n",
	} {
		path := filepath.Join(r.root, strings.ReplaceAll(name, " ", "-")+".md")
		if err := os.WriteFile(path, []byte(body), 0o644); err != nil {
			t.Fatal(err)
		}
		code, errText := r.land(r.wt, "-review", path, "-test", "true")
		if code != exitReviewRefused || !strings.Contains(errText, "want") {
			t.Fatalf("%s: code %d: %s", name, code, errText)
		}
	}
}

// A push origin took is never undone on the branch: when main moves on right after the push (exit 7), the branch stays
// at the commit it pushed, so its work reads as landed.
func TestReviewGateAPushedTipIsKeptWhenMainMovesAfterThePush(t *testing.T) {
	t.Parallel()
	r := setup(t)
	path := r.mergeReview()
	// A post-receive hook stands in for another lander that pushed on top right after this one.
	hook := filepath.Join(r.origin, "hooks", "post-receive")
	script := "#!/bin/sh\nexec 2>>hook.err\nexport GIT_AUTHOR_NAME=o GIT_AUTHOR_EMAIL=o@example.org GIT_COMMITTER_NAME=o GIT_COMMITTER_EMAIL=o@example.org\nnew=$(git commit-tree refs/heads/main^{tree} -p refs/heads/main -m later) && git update-ref refs/heads/main \"$new\"\n"
	if err := os.WriteFile(hook, []byte(script), 0o755); err != nil {
		t.Fatal(err)
	}
	if code, errs := r.land(r.wt, "-review", path, "-test", "true"); code != 7 {
		hookErr, _ := os.ReadFile(filepath.Join(r.origin, "hook.err"))
		t.Fatalf("exit %d, want 7: %s; the hook's errors: %s", code, errs, hookErr)
	}
	pushed := r.git(r.root, "--git-dir", r.origin, "rev-parse", "main~1")
	if got := r.git(r.wt, "rev-parse", "HEAD"); got != pushed {
		t.Fatalf("the branch is at %s, want the pushed tip %s", got, pushed)
	}
}
