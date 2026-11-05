// Package policy holds the machine-checked half of ADR 0044: which Python files under tools/ remain, and why; and the
// scan for sleeping to wait in Go. The data is python_to_port.txt (the files still to port; it only shrinks) and Stays
// (the files that stay Python, mirrored by the ADR's table); policy_test.go is the check.
package policy

import (
	"bufio"
	_ "embed"
	"fmt"
	"go/ast"
	"go/parser"
	"go/token"
	"os/exec"
	"sort"
	"strings"
)

// ADRFile is the decision record these checks enforce, in docs/adr/.
const ADRFile = "0044-tools-and-tests-are-go.md"

// Stay is a Python file, or a directory of them (Path ends in "/"), that stays Python by a row of the ADR's
// "Where Python stays" table.
type Stay struct {
	Path     string
	Reason   string
	EndsWhen string
}

// Stays mirrors the ADR's table; a test fails when the two name different files.
var Stays = []Stay{
	{
		Path:     "tools/echomod/",
		Reason:   "port cost against saving: no test in tools/tests names it, so a port saves no test time",
		EndsWhen: "a pull request changes its behaviour; that pull request ports what it changes",
	},
	{
		Path:     "tools/run_tool_tests.py",
		Reason:   "retired by the unit that replaces it with a Go runner (G1a of #2132)",
		EndsWhen: "that unit merges",
	},
}

//go:embed python_to_port.txt
var portFile string

// ToPort returns the Python files under tools/ that are to be ported, sorted. The list only shrinks.
func ToPort() []string { return parseList(portFile) }

func parseList(text string) []string {
	var out []string
	sc := bufio.NewScanner(strings.NewReader(text))
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		out = append(out, line)
	}
	return out
}

// StayFor reports the Stays entry that covers path.
func StayFor(path string) (Stay, bool) {
	for _, s := range Stays {
		if strings.HasSuffix(s.Path, "/") {
			if strings.HasPrefix(path, s.Path) {
				return s, true
			}
		} else if path == s.Path {
			return s, true
		}
	}
	return Stay{}, false
}

// TrackedPython lists the Python files under tools/ that git knows: tracked, or untracked and not ignored. env is the
// environment for the git command (testkit.Env in a test).
func TrackedPython(root string, env []string) ([]string, error) {
	cmd := exec.Command("git", "ls-files", "--cached", "--others", "--exclude-standard", "--", "tools/*.py")
	cmd.Dir = root
	cmd.Env = env
	out, err := cmd.Output()
	if err != nil {
		return nil, fmt.Errorf("git ls-files in %s: %w", root, err)
	}
	seen := map[string]bool{}
	var files []string
	for _, f := range strings.Fields(string(out)) {
		if !seen[f] {
			seen[f] = true
			files = append(files, f)
		}
	}
	sort.Strings(files)
	return files, nil
}

// Uncovered returns the files of tracked that are neither on the port list nor covered by a Stay: a Python file added
// without a recorded reason.
func Uncovered(tracked []string) []string {
	listed := toSet(ToPort())
	var out []string
	for _, f := range tracked {
		if _, stays := StayFor(f); !stays && !listed[f] {
			out = append(out, f)
		}
	}
	return out
}

// Stale returns the port list's entries that are not in tracked: files that were ported or deleted, whose line is due
// to go.
func Stale(tracked []string) []string {
	have := toSet(tracked)
	var out []string
	for _, f := range ToPort() {
		if !have[f] {
			out = append(out, f)
		}
	}
	return out
}

// Grown returns the entries of now that old did not have: what a change added to a list that only shrinks.
func Grown(old, now []string) []string {
	had := toSet(old)
	var out []string
	for _, f := range now {
		if !had[f] {
			out = append(out, f)
		}
	}
	return out
}

func toSet(list []string) map[string]bool {
	m := make(map[string]bool, len(list))
	for _, s := range list {
		m[s] = true
	}
	return m
}

// ADRStays reads the paths the ADR's "Where Python stays" table names: the backticked tools/ paths in the first column
// of each row, a trailing "**" read as the directory. Sorted, as Stays sorted by path would be.
func ADRStays(adr string) ([]string, error) {
	const heading = "## Where Python stays"
	lines := strings.Split(adr, "\n")
	start := -1
	for i, l := range lines {
		if strings.TrimSpace(l) == heading {
			start = i + 1
			break
		}
	}
	if start < 0 {
		return nil, fmt.Errorf("the ADR has no %q heading", heading)
	}
	var paths []string
	for _, l := range lines[start:] {
		if strings.HasPrefix(l, "## ") {
			break
		}
		if !strings.HasPrefix(l, "|") {
			continue
		}
		cells := strings.Split(l, "|")
		if len(cells) < 3 {
			continue
		}
		parts := strings.Split(cells[1], "`")
		for i := 1; i < len(parts); i += 2 { // the odd parts are inside backticks
			if p := parts[i]; strings.HasPrefix(p, "tools/") {
				paths = append(paths, strings.TrimSuffix(p, "**"))
			}
		}
	}
	if len(paths) == 0 {
		return nil, fmt.Errorf("the table under %q names no tools/ path", heading)
	}
	sort.Strings(paths)
	return paths, nil
}

// SleepViolations returns "file:line: ..." for each time.Sleep in src that ADR 0044 rule 9 forbids: in a _test.go
// file, one outside a synctest.Test (or synctest.Run) function; in any other file, one whose duration is a constant
// (a literal, a time unit, a package-level constant, or arithmetic on those) and not a parameter or a field.
func SleepViolations(filename string, src []byte) ([]string, error) {
	fset := token.NewFileSet()
	f, err := parser.ParseFile(fset, filename, src, 0)
	if err != nil {
		return nil, err
	}
	consts := map[string]bool{}
	for _, d := range f.Decls {
		if g, ok := d.(*ast.GenDecl); ok && g.Tok == token.CONST {
			for _, spec := range g.Specs {
				for _, n := range spec.(*ast.ValueSpec).Names {
					consts[n.Name] = true
				}
			}
		}
	}
	isTest := strings.HasSuffix(filename, "_test.go")
	var out []string
	var walk func(n ast.Node, inBubble bool)
	walk = func(root ast.Node, inBubble bool) {
		ast.Inspect(root, func(n ast.Node) bool {
			call, ok := n.(*ast.CallExpr)
			if !ok {
				return true
			}
			if isPkgCall(call, "synctest", "Test") || isPkgCall(call, "synctest", "Run") {
				walk(call.Fun, inBubble)
				for _, a := range call.Args {
					if lit, ok := a.(*ast.FuncLit); ok {
						walk(lit.Body, true)
					} else {
						walk(a, inBubble)
					}
				}
				return false
			}
			if isPkgCall(call, "time", "Sleep") {
				bad := false
				if isTest {
					bad = !inBubble
				} else {
					bad = len(call.Args) == 1 && constantDuration(call.Args[0], consts)
				}
				if bad {
					out = append(out, fmt.Sprintf("%s: time.Sleep", fset.Position(call.Pos())))
				}
			}
			return true
		})
	}
	walk(f, false)
	return out, nil
}

func isPkgCall(call *ast.CallExpr, pkg, name string) bool {
	sel, ok := call.Fun.(*ast.SelectorExpr)
	if !ok {
		return false
	}
	id, ok := sel.X.(*ast.Ident)
	return ok && id.Name == pkg && sel.Sel.Name == name
}

func constantDuration(e ast.Expr, consts map[string]bool) bool {
	switch e := e.(type) {
	case *ast.BasicLit:
		return true
	case *ast.Ident:
		return consts[e.Name]
	case *ast.SelectorExpr:
		id, ok := e.X.(*ast.Ident)
		return ok && id.Name == "time" // time.Second and the other units; a field (s.Interval) is not
	case *ast.ParenExpr:
		return constantDuration(e.X, consts)
	case *ast.UnaryExpr:
		return constantDuration(e.X, consts)
	case *ast.BinaryExpr:
		return constantDuration(e.X, consts) && constantDuration(e.Y, consts)
	case *ast.CallExpr: // a conversion such as time.Duration(n): constant only when what it converts is
		for _, a := range e.Args {
			if !constantDuration(a, consts) {
				return false
			}
		}
		return len(e.Args) > 0
	}
	return false
}
