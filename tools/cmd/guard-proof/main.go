// Command guard-proof checks that each registered guard has its red and green proof in its own tests.
//
//	guard-proof [-root dir] [-registry file]
//
// The registry (default docs/standards/guards.txt) has one line per guard, three tab-separated fields: the guard's
// name, its directory under tools/cmd, and the step that runs it (a recipe or step named in AGENTS.md). For each guard
// it fails, naming the guard, when
//   - tools/cmd/<dir>/main.go is missing;
//   - tools/cmd/<dir>/main_test.go has no test whose name contains "Planted" (the planted violation is refused), none
//     whose name contains "Green" or "RealTree" (the real tree passes), or none whose name contains "Bad" or
//     "Absent" (the guard's own wrong input fails and does not pass);
//   - the step field is empty or names text that AGENTS.md does not contain.
//
// An empty registry fails: a guard list with no guards guards nothing.
//
// Exit codes: 0 every guard is proven; 1 a guard lacks a proof (each printed); 2 a bad flag or registry.
package main

import (
	"bufio"
	"flag"
	"fmt"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"regexp"
	"strings"

	"github.com/EchoTools/nevr-runtime/tools/internal/runlog"
)

// A registry line has registryFields tab-separated fields: the guard name, its directory, and the step (at fieldStep).
const (
	registryFields = 3
	fieldStep      = 2
)

type guard struct{ name, dir, step string }

var testName = regexp.MustCompile(`(?m)^func (Test\w+)\(`)

// exitUsage is the exit code of a bad flag, a missing argument or input that cannot be read.
const exitUsage = 2

func main() { os.Exit(run(os.Args[1:], os.Getenv, os.Stdout, os.Stderr)) }

// run is execute with the tool's run log: one "invoked" and one "finished" record in $XDG_STATE_HOME/nevr-runtime/guard-proof.jsonl
// (internal/runlog). A log that cannot be written is exit 74, never a run without its record.
func run(args []string, getenv func(string) string, stdout, stderr io.Writer) int {
	return runlog.Invocation("guard-proof", args, getenv, stderr, func(*slog.Logger) int { return execute(args, stdout, stderr) })
}

func execute(args []string, stdout, stderr io.Writer) int {
	fl := flag.NewFlagSet("guard-proof", flag.ContinueOnError)
	fl.SetOutput(stderr)
	root := fl.String("root", ".", "repository root")
	registry := fl.String("registry", "docs/standards/guards.txt", "registry file, relative to -root")
	if err := fl.Parse(args); err != nil {
		return exitUsage
	}
	guards, err := readRegistry(filepath.Join(*root, *registry))
	if err != nil {
		fmt.Fprintf(stderr, "guard-proof: %v\n", err)
		return exitUsage
	}
	agents, _ := os.ReadFile(filepath.Join(*root, "AGENTS.md"))
	var bad []string
	for _, g := range guards {
		base := filepath.Join(*root, "tools", "cmd", g.dir)
		if _, err := os.Stat(filepath.Join(base, "main.go")); err != nil {
			bad = append(bad, g.name+": tools/cmd/"+g.dir+"/main.go is missing")
			continue
		}
		tb, err := os.ReadFile(filepath.Join(base, "main_test.go"))
		if err != nil {
			bad = append(bad, g.name+": tools/cmd/"+g.dir+"/main_test.go is missing")
			continue
		}
		var names []string
		for _, m := range testName.FindAllStringSubmatch(string(tb), -1) {
			names = append(names, m[1])
		}
		for _, need := range []struct {
			what string
			subs []string
		}{
			{"a planted-violation test (name contains \"Planted\")", []string{"Planted"}},
			{"a green test (name contains \"Green\" or \"RealTree\")", []string{"Green", "RealTree"}},
			{"a bad-input test (name contains \"Bad\" or \"Absent\")", []string{"Bad", "Absent"}},
		} {
			if !anyContains(names, need.subs) {
				bad = append(bad, g.name+": lacks "+need.what)
			}
		}
		if !strings.Contains(string(agents), g.step) {
			bad = append(bad, g.name+": the step "+fmt.Sprintf("%q", g.step)+" is not named in AGENTS.md")
		}
	}
	for _, b := range bad {
		fmt.Fprintln(stdout, "unproven:", b)
	}
	if len(bad) > 0 {
		fmt.Fprintf(stderr, "guard-proof: %d gap(s) in the guards' proofs\n", len(bad))
		return 1
	}
	fmt.Fprintf(stdout, "guard-proof: OK (%d guards)\n", len(guards))
	return 0
}

func anyContains(names, subs []string) bool {
	for _, n := range names {
		for _, s := range subs {
			if strings.Contains(n, s) {
				return true
			}
		}
	}
	return false
}

func readRegistry(path string) ([]guard, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, fmt.Errorf("registry: %w", err)
	}
	defer f.Close()
	var out []guard
	sc := bufio.NewScanner(f)
	for n := 1; sc.Scan(); n++ {
		l := sc.Text()
		if strings.TrimSpace(l) == "" || strings.HasPrefix(l, "#") {
			continue
		}
		p := strings.Split(l, "\t")
		if len(p) != registryFields || p[0] == "" || p[1] == "" || p[fieldStep] == "" {
			return nil, fmt.Errorf("registry line %d: want name, directory and step separated by tabs", n)
		}
		out = append(out, guard{p[0], p[1], p[fieldStep]})
	}
	if len(out) == 0 {
		return nil, fmt.Errorf("registry %s names no guard", path)
	}
	return out, nil
}
