// Command issue-prose checks the text of an issue body or a close comment before it is posted.
//
//	issue-prose -kind body  -file <text>     a new issue
//	issue-prose -kind close -file <text>     a comment that closes an issue
//
// Both kinds: the text carries no local path (/home/, /var/tmp/, /mnt/, /tmp/, ~/, C:\), the public-prose rule of
// "Documentation, plans and findings" in AGENTS.md.
// body: the sections "Evidence at <sha>", "Not shown", "Falsifier" and "Work" are present (docs/standards/planning.md,
// "Write an issue as a falsifiable claim" in AGENTS.md), and the Evidence heading names a hex sha of at least 7 digits.
// close: the comment names a commit sha (7 to 40 hex digits) and an exit code ("exit 0"), so a close states what landed and
// what was run.
//
// Exit codes: 0 fine; 1 something is wrong (each reason printed); 2 an unreadable file or a bad flag.
package main

import (
	"flag"
	"fmt"
	"io"
	"log/slog"
	"os"
	"regexp"
	"strings"

	"github.com/EchoTools/nevr-runtime/tools/internal/runlog"
)

var (
	localPath = regexp.MustCompile(`(^|[\s(\x60"'])(/home/|/var/tmp/|/mnt/|/tmp/|~/|[A-Za-z]:\\)`)
	shaRe     = regexp.MustCompile(`\b[0-9a-f]{7,40}\b`)
	hasDigit  = regexp.MustCompile(`[0-9]`)
	exitRe    = regexp.MustCompile(`(?i)\bexit[ :=]*\d+`)
	evidence  = regexp.MustCompile(`(?im)^\W*(#+\s*|\*\*)?Evidence at\b.*$`)
)

// exitUsage is the exit code of a bad flag, a missing argument or input that cannot be read.
const exitUsage = 2

func main() { os.Exit(run(os.Args[1:], os.Getenv, os.Stdout, os.Stderr)) }

// run is execute with the tool's run log: one "invoked" and one "finished" record in $XDG_STATE_HOME/nevr-runtime/issue-prose.jsonl
// (internal/runlog). A log that cannot be written is exit 74, never a run without its record.
func run(args []string, getenv func(string) string, stdout, stderr io.Writer) int {
	return runlog.Invocation("issue-prose", args, getenv, stderr, func(*slog.Logger) int { return execute(args, stdout, stderr) })
}

func execute(args []string, stdout, stderr io.Writer) int {
	fl := flag.NewFlagSet("issue-prose", flag.ContinueOnError)
	fl.SetOutput(stderr)
	kind := fl.String("kind", "", "body or close")
	file := fl.String("file", "", "the text to check")
	if err := fl.Parse(args); err != nil {
		return exitUsage
	}
	if (*kind != "body" && *kind != "close") || *file == "" {
		fmt.Fprintln(stderr, "issue-prose: -kind body|close and -file are required")
		return exitUsage
	}
	b, err := os.ReadFile(*file)
	if err != nil {
		fmt.Fprintf(stderr, "issue-prose: %v\n", err)
		return exitUsage
	}
	problems := check(*kind, string(b))
	for _, p := range problems {
		fmt.Fprintln(stdout, "problem:", p)
	}
	if len(problems) > 0 {
		fmt.Fprintf(stderr, "issue-prose: %d problem(s); do not post this text\n", len(problems))
		return 1
	}
	fmt.Fprintln(stdout, "issue-prose: OK")
	return 0
}

func check(kind, text string) []string {
	var p []string
	for i, l := range strings.Split(text, "\n") {
		if localPath.MatchString(l) {
			p = append(p, fmt.Sprintf("line %d holds a local path: %s", i+1, strings.TrimSpace(l)))
		}
	}
	switch kind {
	case "body":
		for _, s := range []string{"Not shown", "Falsifier", "Work"} {
			if !regexp.MustCompile(`(?im)^\W*(#+\s*|\*\*)?` + s + `\b`).MatchString(text) {
				p = append(p, "no section \""+s+"\"")
			}
		}
		ev := evidence.FindString(text)
		if ev == "" {
			p = append(p, "no section \"Evidence at <sha>\"")
		} else if m := shaRe.FindString(ev); m == "" || !hasDigit.MatchString(m) {
			p = append(p, "the Evidence heading names no commit sha")
		}
	case "close":
		if m := shaRe.FindString(text); m == "" || !hasDigit.MatchString(m) {
			p = append(p, "the close comment names no commit sha")
		}
		if !exitRe.MatchString(text) {
			p = append(p, "the close comment names no exit code (\"exit 0\")")
		}
	}
	return p
}
