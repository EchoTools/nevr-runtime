// Command commit-evidence fails when a commit message lacks what "What a landed commit carries" (AGENTS.md) lists.
//
//	commit-evidence [-repo dir] -rev <rev>     read the message of one commit
//	commit-evidence -file <path>               read a message from a file
//
// It checks the form, never the truth of a claim: the sections "## Search statement", "## Symbols", "## Mutants" and
// "## Platform parity" are present and not empty, a section "## Commands and results" or "## Evidence" names an exit code,
// a commit whose subject is a fix (type fix) has "## Siblings", the message names an issue (Closes, Fixes, Resolves,
// Refs, Part of or Completes #N), and a Co-Authored-By trailer is present.
//
// Exit codes: 0 the message carries everything; 1 something is missing (each missing item is printed); 2 the message
// could not be read or a flag is wrong.
package main

import (
	"flag"
	"fmt"
	"io"
	"log/slog"
	"os"
	"os/exec"

	"github.com/EchoTools/nevr-runtime/tools/internal/evidence"
	"github.com/EchoTools/nevr-runtime/tools/internal/runlog"
)

// exitUsage is the exit code of a bad flag, a missing argument or input that cannot be read.
const exitUsage = 2

func main() { os.Exit(run(os.Args[1:], os.Getenv, os.Stdout, os.Stderr)) }

// run is execute with the tool's run log: one "invoked" and one "finished" record in $XDG_STATE_HOME/nevr-runtime/commit-evidence.jsonl
// (internal/runlog). A log that cannot be written is exit 74, never a run without its record.
func run(args []string, getenv func(string) string, stdout, stderr io.Writer) int {
	return runlog.Invocation("commit-evidence", args, getenv, stderr, func(*slog.Logger) int { return execute(args, stdout, stderr) })
}

func execute(args []string, stdout, stderr io.Writer) int {
	fl := flag.NewFlagSet("commit-evidence", flag.ContinueOnError)
	fl.SetOutput(stderr)
	repo := fl.String("repo", ".", "repository for -rev")
	rev := fl.String("rev", "", "commit whose message is checked")
	file := fl.String("file", "", "file holding a message")
	if err := fl.Parse(args); err != nil {
		return exitUsage
	}
	var msg string
	switch {
	case (*rev == "") == (*file == ""):
		fmt.Fprintln(stderr, "commit-evidence: give exactly one of -rev and -file")
		return exitUsage
	case *file != "":
		b, err := os.ReadFile(*file)
		if err != nil {
			fmt.Fprintf(stderr, "commit-evidence: %v\n", err)
			return exitUsage
		}
		msg = string(b)
	default:
		out, err := exec.Command("git", "-C", *repo, "log", "-1", "--format=%B", *rev).Output()
		if err != nil {
			fmt.Fprintf(stderr, "commit-evidence: git log %s: %v\n", *rev, err)
			return exitUsage
		}
		msg = string(out)
	}
	missing := evidence.Missing(msg)
	for _, m := range missing {
		fmt.Fprintln(stdout, "missing:", m)
	}
	if len(missing) > 0 {
		fmt.Fprintf(stderr, "commit-evidence: %d item(s) missing from the message\n", len(missing))
		return 1
	}
	fmt.Fprintln(stdout, "commit-evidence: OK")
	return 0
}
