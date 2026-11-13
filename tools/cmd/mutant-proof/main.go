// Command mutant-proof checks the "## Mutants" block of a commit message, and the tree a mutant is made in.
//
//	mutant-proof -file <message> | -rev <rev> [-repo dir]    check the block
//	mutant-proof -clean [-repo dir]                         refuse a tree with uncommitted changes
//
// A block is either "None: <reason>" (the commit changes no behaviour) or one or more lines
//
//   - M1 <what was changed> -> FAIL <test>: <output>
//
// and a closing line "restored tree: ... OK" (or pass, green). A mutant line that says survived, PASS or
// "never landed" is a mutant that did not fail a test: it is refused. -clean exists because a mutant made in a dirty
// tree cannot be told from the work: the mutation is made after a commit, never over uncommitted work.
//
// Exit codes: 0 fine; 1 the block or the tree is wrong (each reason printed); 2 unreadable input or a bad flag.
package main

import (
	"flag"
	"fmt"
	"io"
	"log/slog"
	"os"
	"os/exec"
	"regexp"
	"strings"

	"github.com/EchoTools/nevr-runtime/tools/internal/runlog"
)

var (
	mutantLine = regexp.MustCompile(`^\s*-?\s*M\d+\b`)
	failWord   = regexp.MustCompile(`->\s*FAIL\b`)
	badWord    = regexp.MustCompile(`(?i)\b(survived|never landed|PASS(ed)?)\b`)
	restored   = regexp.MustCompile(`(?i)^\s*-?\s*restored tree:.*\b(OK|pass(ed)?|green)\b`)
)

// exitUsage is the exit code of a bad flag, a missing argument or input that cannot be read.
const exitUsage = 2

func main() { os.Exit(run(os.Args[1:], os.Getenv, os.Stdout, os.Stderr)) }

// run is execute with the tool's run log: one "invoked" and one "finished" record in $XDG_STATE_HOME/nevr-runtime/mutant-proof.jsonl
// (internal/runlog). A log that cannot be written is exit 74, never a run without its record.
func run(args []string, getenv func(string) string, stdout, stderr io.Writer) int {
	return runlog.Invocation("mutant-proof", args, getenv, stderr, func(*slog.Logger) int { return execute(args, stdout, stderr) })
}

func execute(args []string, stdout, stderr io.Writer) int {
	fl := flag.NewFlagSet("mutant-proof", flag.ContinueOnError)
	fl.SetOutput(stderr)
	repo := fl.String("repo", ".", "repository")
	rev := fl.String("rev", "", "commit whose message is checked")
	file := fl.String("file", "", "file holding a message")
	clean := fl.Bool("clean", false, "refuse a tree with uncommitted changes")
	if err := fl.Parse(args); err != nil {
		return exitUsage
	}
	if *clean {
		out, err := exec.Command("git", "-C", *repo, "status", "--porcelain").Output()
		if err != nil {
			fmt.Fprintf(stderr, "mutant-proof: git status: %v\n", err)
			return exitUsage
		}
		if len(strings.TrimSpace(string(out))) > 0 {
			fmt.Fprint(stdout, string(out))
			fmt.Fprintln(stderr, "mutant-proof: the tree has uncommitted changes; commit first, then mutate")
			return 1
		}
		fmt.Fprintln(stdout, "mutant-proof: tree clean")
		return 0
	}
	var msg string
	switch {
	case (*rev == "") == (*file == ""):
		fmt.Fprintln(stderr, "mutant-proof: give exactly one of -rev, -file, or -clean")
		return exitUsage
	case *file != "":
		b, err := os.ReadFile(*file)
		if err != nil {
			fmt.Fprintf(stderr, "mutant-proof: %v\n", err)
			return exitUsage
		}
		msg = string(b)
	default:
		out, err := exec.Command("git", "-C", *repo, "log", "-1", "--format=%B", *rev).Output()
		if err != nil {
			fmt.Fprintf(stderr, "mutant-proof: git log %s: %v\n", *rev, err)
			return exitUsage
		}
		msg = string(out)
	}
	problems := checkBlock(msg)
	for _, p := range problems {
		fmt.Fprintln(stdout, "mutants:", p)
	}
	if len(problems) > 0 {
		fmt.Fprintf(stderr, "mutant-proof: %d problem(s) in the Mutants block\n", len(problems))
		return 1
	}
	fmt.Fprintln(stdout, "mutant-proof: OK")
	return 0
}

func checkBlock(msg string) []string {
	lines := strings.Split(msg, "\n")
	start := -1
	for i, l := range lines {
		if strings.HasPrefix(l, "## Mutants") {
			start = i + 1
			break
		}
	}
	if start < 0 {
		return []string{"no \"## Mutants\" section"}
	}
	var block []string
	for _, l := range lines[start:] {
		if strings.HasPrefix(l, "## ") {
			break
		}
		if strings.TrimSpace(l) != "" {
			block = append(block, l)
		}
	}
	if len(block) == 0 {
		return []string{"the Mutants section is empty"}
	}
	if strings.HasPrefix(strings.TrimSpace(block[0]), "None:") {
		if len(strings.TrimSpace(strings.TrimPrefix(strings.TrimSpace(block[0]), "None:"))) == 0 {
			return []string{"\"None:\" gives no reason"}
		}
		return nil
	}
	var problems []string
	n, sawRestored := 0, false
	for _, l := range block {
		switch {
		case restored.MatchString(l):
			sawRestored = true
		case mutantLine.MatchString(l):
			n++
			if badWord.MatchString(l) || !failWord.MatchString(l) {
				problems = append(problems, "mutant did not fail a test: "+strings.TrimSpace(l))
			}
		}
	}
	if n == 0 {
		problems = append(problems, "no mutant line (- M1 ... -> FAIL ...)")
	}
	if !sawRestored {
		problems = append(problems, "no \"restored tree: ... OK\" line")
	}
	return problems
}
