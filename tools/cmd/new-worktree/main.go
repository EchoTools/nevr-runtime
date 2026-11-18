// Command new-worktree makes an agent's worktree and writes its ledger row, the steps of AGENTS.md "Branch lifecycle"
// and "Ownership and cleanup" in one command.
//
//	new-worktree [-repo <checkout>] [-purpose <text>] <seat>/<slug>
//
// It fetches origin main, then creates .claude/worktrees/<seat>/<slug> on the new branch <seat>/<slug> cut with
// --no-track from origin/main, locks the worktree with the reason "<seat>: delete when it lands on main or is abandoned",
// sets branch.<branch>.description to that reason, and appends one row (artifact, owner, purpose, remove-when) to the
// ledger. The owner is the branch's first path component. The ledger is $REAP_LEDGER, else <work root>/LEDGER.md (the
// work root is $NEVR_WORK_ROOT, else $HOME/src/nevr-runtime-work) when REAP_LEDGER is unset or empty, made with its header
// row when absent; a ledger named by $REAP_LEDGER that does not exist is refused, as tools/reap_merged.py refuses it. -repo names any
// checkout of the repository (default the current directory); the worktree goes under the main checkout.
//
// Every check of the arguments, the branch, the path and the ledger runs before the first write (the ledger file of
// the default location, the worktree, the row): a refusal makes none of them. A failed fetch or git step after that
// leaves what the earlier steps made and says so.
//
// Exit codes: 0 done; 1 a refusal or a failed step (the reason is printed to standard error); 2 a usage error.
package main

import (
	"errors"
	"flag"
	"fmt"
	"io"
	"log/slog"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"

	"github.com/EchoTools/nevr-runtime/tools/internal/runlog"
	"github.com/EchoTools/nevr-runtime/tools/internal/runroot"
)

const (
	exitOK     = 0
	exitFailed = 1
	exitUsage  = 2
	ledgerName = "LEDGER.md"
	ledgerHead = "| artifact | owner | purpose | remove-when |\n|---|---|---|---|\n"
	removeWhen = "when it lands on main or is abandoned"
	dirMode    = 0o755
	fileMode   = 0o644
)

// branchName is <seat>/<slug>: two path components. The seat is a word of letters, digits, underscore and hyphen (the
// only seat names tools/reap_merged.py credits in the ledger); the slug may also hold a dot.
var branchName = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9_-]*/[A-Za-z0-9][A-Za-z0-9._-]*$`)

func main() { os.Exit(run(os.Args[1:], os.Getenv, os.Stdout, os.Stderr)) }

// run is execute with the tool's run log: one "invoked" and one "finished" record in
// $XDG_STATE_HOME/nevr-runtime/new-worktree.jsonl (internal/runlog).
func run(args []string, getenv func(string) string, stdout, stderr io.Writer) int {
	return runlog.Invocation("new-worktree", args, getenv, stderr, func(*slog.Logger) int {
		return execute(args, getenv, stdout, stderr)
	})
}

func execute(args []string, getenv func(string) string, stdout, stderr io.Writer) int {
	fl := flag.NewFlagSet("new-worktree", flag.ContinueOnError)
	fl.SetOutput(stderr)
	repo := fl.String("repo", ".", "a checkout of the repository")
	purpose := fl.String("purpose", "", "what the worktree is for (default: work on <branch>)")
	if err := fl.Parse(args); err != nil {
		return exitUsage
	}
	if fl.NArg() != 1 {
		fmt.Fprintln(stderr, "new-worktree: usage: new-worktree [-repo dir] [-purpose text] <seat>/<slug>")
		return exitUsage
	}
	if err := create(*repo, fl.Arg(0), *purpose, getenv, stdout); err != nil {
		fmt.Fprintln(stderr, "new-worktree:", err)
		return exitFailed
	}
	return exitOK
}

func create(repo, branch, purpose string, getenv func(string) string, stdout io.Writer) error {
	if !branchName.MatchString(branch) {
		return fmt.Errorf("%q is not <seat>/<slug> (a seat of letters, digits, '_' and '-', then a slug that may also hold '.')", branch)
	}
	if out, err := exec.Command("git", "check-ref-format", "--branch", branch).CombinedOutput(); err != nil {
		return fmt.Errorf("%q is not a branch name git accepts: %s", branch, strings.TrimSpace(string(out)))
	}
	seat, _, _ := strings.Cut(branch, "/")
	if purpose == "" {
		purpose = "work on " + branch
	}
	if strings.ContainsAny(purpose, "|\n") {
		return errors.New("the purpose holds a '|' or a newline, which would break the ledger row")
	}

	common, err := git(repo, "rev-parse", "--path-format=absolute", "--git-common-dir")
	if err != nil {
		return err
	}
	main := filepath.Dir(common)

	if _, err := git(main, "show-ref", "--verify", "--quiet", "refs/heads/"+branch); err == nil {
		return fmt.Errorf("branch %s already exists", branch)
	}
	rel := filepath.ToSlash(filepath.Join(".claude", "worktrees", branch))
	if _, err := os.Lstat(filepath.Join(main, rel)); err == nil {
		return fmt.Errorf("%s already exists", rel)
	}

	ledger, err := prepareLedger(getenv)
	if err != nil {
		return err
	}

	reason := seat + ": delete when it lands on main or is abandoned"
	steps := [][]string{
		{"fetch", "origin", "main"},
		{"worktree", "add", "--no-track", "-b", branch, rel, "origin/main"},
		{"worktree", "lock", "--reason", reason, rel},
		{"config", "branch." + branch + ".description", reason},
	}
	for _, s := range steps {
		if _, err := git(main, s...); err != nil {
			return fmt.Errorf("%w (steps before this one are done: check `git worktree list`)", err)
		}
	}
	row := fmt.Sprintf("| %s (branch %s) | %s | %s | %s |\n", rel, branch, seat, purpose, removeWhen)
	f, err := os.OpenFile(ledger, os.O_APPEND|os.O_WRONLY, fileMode)
	if err == nil {
		_, err = f.WriteString(row)
		if cerr := f.Close(); err == nil {
			err = cerr
		}
	}
	if err != nil {
		return fmt.Errorf("worktree %s made but its ledger row was not written to %s: %w", rel, ledger, err)
	}
	fmt.Fprintf(stdout, "%s\nbranch %s, ledger row in %s\n", filepath.Join(main, rel), branch, ledger)
	return nil
}

// prepareLedger returns the ledger to append to, made with its header when it is the default and absent. A ledger
// named by REAP_LEDGER must exist.
func prepareLedger(getenv func(string) string) (string, error) {
	if named := getenv("REAP_LEDGER"); named != "" {
		return named, checkAppendable(named)
	}
	root, err := runroot.WorkRoot(getenv)
	if err != nil {
		return "", fmt.Errorf("no ledger: set REAP_LEDGER or NEVR_WORK_ROOT: %w", err)
	}
	path := filepath.Join(root, ledgerName)
	if err := os.MkdirAll(root, dirMode); err != nil {
		return "", err
	}
	f, err := os.OpenFile(path, os.O_CREATE|os.O_EXCL|os.O_WRONLY, fileMode)
	switch {
	case errors.Is(err, os.ErrExist):
	case err != nil:
		return "", err
	default:
		_, err = f.WriteString(ledgerHead)
		if cerr := f.Close(); err == nil {
			err = cerr
		}
		if err != nil {
			return "", err
		}
	}
	return path, checkAppendable(path)
}

// checkAppendable opens the ledger for append and closes it, so a ledger that cannot take its row is refused before
// the worktree is made.
func checkAppendable(path string) error {
	f, err := os.OpenFile(path, os.O_APPEND|os.O_WRONLY, fileMode)
	if err != nil {
		return fmt.Errorf("cannot append to the ledger %s: %w", path, err)
	}
	return f.Close()
}

func git(dir string, args ...string) (string, error) {
	out, err := exec.Command("git", append([]string{"-C", dir}, args...)...).CombinedOutput()
	if err != nil {
		return "", fmt.Errorf("git %s: %w: %s", strings.Join(args, " "), err, strings.TrimSpace(string(out)))
	}
	return strings.TrimSpace(string(out)), nil
}
