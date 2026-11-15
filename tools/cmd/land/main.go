// Command land lands the branch of the worktree it runs in on origin's main, one lander at a time (AGENTS.md, "Landing"):
//
//	land [-C dir] [-lock-wait 30m] -review <report> -test 'command' [-test 'command' ...]
//
// Run it from the linked worktree that holds the branch, never from the main checkout. After the checks that exit 2 and before
// the lock and the fetch, it reads the -review report (required, no default): the report must hold exactly one line "Verdict: MERGE <sha>", where <sha>
// is HEAD's full commit sha, the commit about to be rebased (the commit, not its tree: the message is the evidence the
// review reproduces, and a tree would let an unreviewed message through). The nevr-runtime-engineer review agent ends
// its report with that line. A missing report, a FIX verdict, a verdict of another commit or a report with no single
// verdict line is refused before the lock and the fetch. Under an exclusive flock on <git common dir>/nevr-land.lock,
// taken before the fetch and released after the push, it:
//
//  1. fetches origin and records origin's main (`git ls-remote origin refs/heads/main`), the "before" SHA;
//  2. rebases the branch's own commits onto origin/main, so each commit keeps its message and no merge commit or squash
//     lands, or leaves the branch as it is when it already contains origin/main; a conflict aborts the rebase and stops;
//     checks the tip commit's message for the evidence "What a landed commit carries" lists (internal/evidence, the check
//     of tools/cmd/commit-evidence); a message that lacks any of it stops before the tests and the push;
//  3. runs each -test command on the result through bash -c; a non-zero exit stops before the push;
//  4. adds the trailer "Reviewed-by-report: <report> Verdict: MERGE <sha> sha256=<report's sha256>" to the tip commit
//     (<report> is <scratch>/<its file name>: a commit message names no local path), then pushes the tip with `git push origin HEAD:refs/heads/main`, never forced, so origin takes only a fast-forward;
//  5. records origin's main again, the "after" SHA, which must be the tip;
//  6. fast-forwards the main checkout to origin/main when it has main checked out and a clean tree.
//
// A run that rebases or adds the trailer and then does not push puts the branch back at the reviewed commit
// (`git reset --keep`, on a tree it found clean), so the same review lands it on the next run. Once origin has taken the
// push the branch stays at the pushed tip, whatever exit follows (7, or 9 when the second ls-remote fails).
//
// Each step is one record in $XDG_STATE_HOME/nevr-runtime/land.jsonl (internal/runlog), appended, never truncated.
//
// Exit codes: 0 landed; 2 refused before the lock (main checkout, detached HEAD, main itself, a dirty tree, no -test,
// bad flags); 3 the lock was not free within -lock-wait; 4 rebase conflict; 5 a test failed; 6 origin refused the push
// (its main moved: run it again); 7 origin's main after the push is not the tip; 8 nothing to land; 9 a git command
// failed; 10 the tip commit message lacks its evidence; 11 no MERGE review of HEAD (no -review, an unreadable report, no
// single verdict line, a FIX verdict, or a review of another commit); 74 the log could not be written.
package main

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"flag"
	"fmt"
	"io"
	"log/slog"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
	"time"

	"github.com/EchoTools/nevr-runtime/tools/internal/evidence"
	"github.com/EchoTools/nevr-runtime/tools/internal/runlog"
)

const lockName = "nevr-land.lock"

// exitEvidenceMissing is the exit code of a tip commit whose message lacks its evidence.
const exitEvidenceMissing = 10

// exitReviewRefused is the exit code of a run with no MERGE review of the tip it was given.
const exitReviewRefused = 11

// reviewVerdictPrefix starts a review report's one verdict line: "Verdict: MERGE <40-hex sha>" or "Verdict: FIX <sha>".
const reviewVerdictPrefix = "Verdict: "

// reviewTrailerKey is the trailer land adds to the tip it pushes, naming the review it landed on.
const reviewTrailerKey = "Reviewed-by-report"

type testList []string

func (l *testList) String() string     { return strings.Join(*l, "; ") }
func (l *testList) Set(v string) error { *l = append(*l, v); return nil }

func main() {
	os.Exit(run(context.Background(), os.Args[1:], os.Environ(), os.Stdout, os.Stderr))
}

type lander struct {
	ctx    context.Context
	dir    string
	env    []string
	stderr io.Writer
}

// git runs git in dir and returns its trimmed standard output and its error.
func (l lander) git(dir string, args ...string) (string, error) {
	cmd := exec.CommandContext(l.ctx, "git", args...)
	cmd.Dir = dir
	cmd.Env = l.env
	var out, errb bytes.Buffer
	cmd.Stdout, cmd.Stderr = &out, &errb
	err := cmd.Run()
	if err != nil {
		return strings.TrimSpace(out.String()), fmt.Errorf("git %s: %w: %s", strings.Join(args, " "), err, strings.TrimSpace(errb.String()))
	}
	return strings.TrimSpace(out.String()), nil
}

func (l lander) remoteMain() (string, error) {
	line, err := l.git(l.dir, "ls-remote", "origin", "refs/heads/main")
	if err != nil {
		return "", err
	}
	if f := strings.Fields(line); len(f) > 0 {
		return f[0], nil
	}
	return "", nil
}

// lock takes an exclusive flock on path, waiting up to wait (or until ctx ends). It returns the open file, or nil when
// the wait ran out. The blocking flock runs in a goroutine; one that is abandoned closes its file when it returns.
func lock(ctx context.Context, path string, wait time.Duration) (*os.File, error) {
	f, err := os.OpenFile(path, os.O_RDWR|os.O_CREATE, 0o644)
	if err != nil {
		return nil, err
	}
	if err := syscall.Flock(int(f.Fd()), syscall.LOCK_EX|syscall.LOCK_NB); err == nil {
		return f, nil
	} else if !errors.Is(err, syscall.EWOULDBLOCK) {
		f.Close()
		return nil, err
	}
	got := make(chan error, 1)
	go func() { got <- syscall.Flock(int(f.Fd()), syscall.LOCK_EX) }()
	timer := time.NewTimer(wait)
	defer timer.Stop()
	select {
	case err := <-got:
		if err != nil {
			f.Close()
			return nil, err
		}
		return f, nil
	case <-timer.C:
	case <-ctx.Done():
	}
	go func() { <-got; f.Close() }()
	return nil, nil
}

func run(ctx context.Context, args []string, env []string, stdout, stderr io.Writer) int {
	getenv := func(k string) string {
		for i := len(env) - 1; i >= 0; i-- {
			if v, ok := strings.CutPrefix(env[i], k+"="); ok {
				return v
			}
		}
		return ""
	}
	return runlog.Run("land", getenv, stderr, func(log *slog.Logger) int {
		flags := flag.NewFlagSet("land", flag.ContinueOnError)
		flags.SetOutput(stderr)
		dir := flags.String("C", ".", "the worktree to land from")
		wait := flags.Duration("lock-wait", 30*time.Minute, "how long to wait for another landing to finish")
		review := flags.String("review", "", "the review report whose verdict line is \"Verdict: MERGE <HEAD's sha>\"; required")
		var tests testList
		flags.Var(&tests, "test", "a command rerun on the rebased tip through bash -c; repeatable, at least one")
		if err := flags.Parse(args); err != nil {
			return 2
		}
		l := lander{ctx: ctx, dir: *dir, env: env, stderr: stderr}
		log.Info("land_start", "dir", *dir, "tests", []string(tests), "review", *review)
		refuse := func(reason string) int {
			log.Warn("land_refused", "outcome", "refused", "reason", reason)
			fmt.Fprintf(stderr, "land: refused: %s\n", reason)
			return 2
		}
		failed := func(err error) int {
			log.Error("land_git_failed", "outcome", "git_failed", "error", err.Error())
			fmt.Fprintf(stderr, "land: %v\n", err)
			return 9
		}

		top, err := l.git(*dir, "rev-parse", "--show-toplevel")
		if err != nil {
			return refuse("not inside a git worktree")
		}
		gitDir, err := l.git(*dir, "rev-parse", "--absolute-git-dir")
		if err != nil {
			return failed(err)
		}
		common, err := l.git(*dir, "rev-parse", "--path-format=absolute", "--git-common-dir")
		if err != nil {
			return failed(err)
		}
		branch, _ := l.git(*dir, "symbolic-ref", "--quiet", "--short", "HEAD")
		log = log.With("worktree", top, "branch", branch)
		l.dir = top
		switch {
		case filepath.Clean(gitDir) == filepath.Clean(common):
			return refuse("this is the main checkout; land from the linked worktree that holds the branch")
		case branch == "":
			return refuse("HEAD is detached; land from a branch")
		case branch == "main":
			return refuse("the branch is main; land from a work branch")
		}
		if dirty, err := l.git(top, "status", "--porcelain"); err != nil {
			return failed(err)
		} else if dirty != "" {
			return refuse("the worktree has uncommitted changes")
		}
		if len(tests) == 0 {
			return refuse("no -test command: Landing reruns the tests the change touches")
		}
		reviewed, err := l.git(top, "rev-parse", "HEAD")
		if err != nil {
			return failed(err)
		}
		rv, reason := readReview(*review, reviewed)
		if reason != "" {
			log.Warn("land_review_refused", "outcome", "review_refused", "reason", reason, "review", *review)
			fmt.Fprintf(stderr, "land: refused: %s\n", reason)
			return exitReviewRefused
		}
		log = log.With("review", *review, "reviewed", reviewed, "review_sha256", rv.sha256)
		log.Info("land_review", "verdict", rv.line)
		pushed := false
		defer func() {
			if pushed {
				return
			}
			if now, err := l.git(top, "rev-parse", "HEAD"); err == nil && now != reviewed {
				if _, err := l.git(top, "reset", "--keep", reviewed); err != nil {
					log.Error("land_restore_failed", "error", err.Error())
					fmt.Fprintf(stderr, "land: could not put the branch back at the reviewed commit %s: %v\n", reviewed, err)
					return
				}
				log.Info("land_restored", "from", now, "to", reviewed)
			}
		}()

		lockPath := filepath.Join(common, lockName)
		f, err := lock(ctx, lockPath, *wait)
		if err != nil {
			return failed(fmt.Errorf("lock %s: %w", lockPath, err))
		}
		if f == nil {
			log.Warn("land_lock_timeout", "outcome", "lock_timeout", "lock", lockPath, "wait_s", wait.Seconds())
			fmt.Fprintf(stderr, "land: another landing holds %s; nothing was fetched or pushed\n", lockPath)
			return 3
		}
		defer f.Close()
		started := time.Now()
		log.Info("land_locked", "lock", lockPath)

		if _, err := l.git(top, "fetch", "origin"); err != nil {
			return failed(err)
		}
		before, err := l.remoteMain()
		if err != nil {
			return failed(err)
		}
		log = log.With("main_before", before)
		rebased := false
		if _, err := l.git(top, "merge-base", "--is-ancestor", "origin/main", "HEAD"); err != nil {
			if _, err := l.git(top, "rebase", "origin/main"); err != nil {
				_, _ = l.git(top, "rebase", "--abort")
				log.Warn("land_rebase_conflict", "outcome", "rebase_conflict", "error", err.Error())
				fmt.Fprintln(stderr, "land: the rebase onto origin/main conflicts; resolve it on the branch and review again")
				return 4
			}
			rebased = true
		}
		tip, err := l.git(top, "rev-parse", "HEAD")
		if err != nil {
			return failed(err)
		}
		count, err := l.git(top, "rev-list", "--count", "origin/main..HEAD")
		if err != nil {
			return failed(err)
		}
		log = log.With("tip", tip, "commits", count, "rebased", rebased)
		if count == "0" {
			log.Info("land_nothing", "outcome", "nothing_to_land")
			return 8
		}
		msg, err := l.git(top, "log", "-1", "--format=%B", "HEAD")
		if err != nil {
			return failed(err)
		}
		if missing := evidence.Missing(msg); len(missing) > 0 {
			log.Warn("land_evidence_missing", "outcome", "evidence_missing", "missing", missing)
			fmt.Fprintf(stderr, "land: the tip commit's message lacks its evidence; nothing was pushed (skill landed-commit-evidence):\n  %s\n",
				strings.Join(missing, "\n  "))
			return exitEvidenceMissing
		}
		for _, c := range tests {
			t0 := time.Now()
			cmd := exec.CommandContext(ctx, "bash", "-c", c)
			cmd.Dir, cmd.Env, cmd.Stdout, cmd.Stderr = top, env, stdout, stderr
			rc := 0
			if err := cmd.Run(); err != nil {
				var ee *exec.ExitError
				if !errors.As(err, &ee) {
					return failed(err)
				}
				rc = ee.ExitCode()
			}
			log.Info("land_test", "cmd", c, "rc", rc, "ms", time.Since(t0).Milliseconds())
			if rc != 0 {
				log.Warn("land_tests_failed", "outcome", "tests_failed", "cmd", c, "rc", rc)
				fmt.Fprintf(stderr, "land: `%s` exited %d; nothing was pushed\n", c, rc)
				return 5
			}
		}
		trailer := reviewTrailerKey + ": " + reportName(*review) + " " + rv.line + " sha256=" + rv.sha256
		if _, err := l.git(top, "commit", "--amend", "--no-edit", "--no-verify", "--no-gpg-sign", "--allow-empty",
			"--trailer", trailer); err != nil {
			return failed(err)
		}
		if tip, err = l.git(top, "rev-parse", "HEAD"); err != nil {
			return failed(err)
		}
		log = log.With("tip", tip)
		if _, err := l.git(top, "push", "origin", "HEAD:refs/heads/main"); err != nil {
			now, _ := l.remoteMain()
			log.Warn("land_push_rejected", "outcome", "push_rejected", "main_now", now, "error", err.Error())
			fmt.Fprintln(stderr, "land: origin refused the push (its main moved); run the landing again")
			return 6
		}
		pushed = true
		after, err := l.remoteMain()
		if err != nil {
			return failed(err)
		}
		log = log.With("main_after", after)
		if after != tip {
			log.Error("land_mismatch", "outcome", "main_not_tip")
			fmt.Fprintf(stderr, "land: origin's main is %s after the push, not the tip %s\n", after, tip)
			return 7
		}
		if _, err := l.git(top, "fetch", "origin"); err != nil {
			return failed(err)
		}
		mainCheckout := common
		if filepath.Base(common) == ".git" {
			mainCheckout = filepath.Dir(common)
		}
		log.Info("land_done", "outcome", "landed", "main_checkout", l.fastForward(mainCheckout),
			"ms", time.Since(started).Milliseconds())
		fmt.Fprintf(stdout, "landed: main %s -> %s\n", before, after)
		return 0
	})
}

// fastForward moves the main checkout to origin/main when it has main checked out and nothing uncommitted, and says
// what it did.
func (l lander) fastForward(dir string) string {
	if _, err := os.Stat(filepath.Join(dir, ".git")); err != nil {
		return "skipped: no main checkout found"
	}
	branch, _ := l.git(dir, "symbolic-ref", "--quiet", "--short", "HEAD")
	if branch != "main" {
		return "skipped: the main checkout does not have main checked out"
	}
	if dirty, err := l.git(dir, "status", "--porcelain"); err != nil || dirty != "" {
		return "skipped: the main checkout has uncommitted changes"
	}
	if _, err := l.git(dir, "merge", "--ff-only", "origin/main"); err != nil {
		return "skipped: git merge --ff-only failed"
	}
	return "fast-forwarded"
}

// review is a report's one verdict line and the sha256 of the whole report.
type review struct{ line, sha256 string }

// readReview reads the report at path and returns its verdict, or the reason it does not let head land.
func readReview(path, head string) (review, string) {
	if path == "" {
		return review{}, "no -review report: land takes the fresh-context review of HEAD (AGENTS.md, \"Landing\")"
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return review{}, fmt.Sprintf("the review report %s cannot be read: %v", path, err)
	}
	var lines []string
	for _, ln := range strings.Split(string(data), "\n") {
		if strings.HasPrefix(ln, reviewVerdictPrefix) {
			lines = append(lines, strings.TrimRight(ln, "\r"))
		}
	}
	if len(lines) != 1 {
		return review{}, fmt.Sprintf("the review report %s has %d lines starting %q, want exactly one \"%sMERGE <sha>\"",
			path, len(lines), reviewVerdictPrefix, reviewVerdictPrefix)
	}
	f := strings.Fields(strings.TrimPrefix(lines[0], reviewVerdictPrefix))
	if len(f) != 2 || !fullSha(f[1]) {
		return review{}, fmt.Sprintf("the review report %s has the verdict line %q, want \"%s<MERGE|FIX> <40-hex sha>\"",
			path, lines[0], reviewVerdictPrefix)
	}
	if f[0] != "MERGE" {
		return review{}, fmt.Sprintf("the verdict is %s, not MERGE (%s): go back to the work", f[0], path)
	}
	if f[1] != head {
		return review{}, fmt.Sprintf("the review is of %s, HEAD is %s (%s): review the tip being landed", f[1], head, path)
	}
	sum := sha256.Sum256(data)
	return review{line: lines[0], sha256: hex.EncodeToString(sum[:])}, ""
}

// fullSha says whether s is a full commit sha in lower-case hex.
func fullSha(s string) bool {
	const shaHexLen = 40
	if len(s) != shaHexLen {
		return false
	}
	for _, c := range s {
		if !strings.ContainsRune("0123456789abcdef", c) {
			return false
		}
	}
	return true
}

// reportName is how the trailer names the report: <scratch>/<file name>. A commit message is public and names no local
// path (AGENTS.md, "Documentation, plans and findings"); the trailer's sha256 ties it to the kept file.
func reportName(path string) string { return "<scratch>/" + filepath.Base(path) }
