// Package policy holds the machine-checked half of ADR 0044: which Python files under tools/ remain, and why; and the
// scan for sleeping to wait in Go. The data is python_to_port.txt (the files still to port; it only shrinks) and Stays
// (the files that stay Python, mirrored by the ADR's table); policy_test.go and reason_test.go are the checks.
package policy

import (
	"bufio"
	_ "embed"
	"fmt"
	"go/ast"
	"go/parser"
	"go/token"
	"os/exec"
	"regexp"
	"sort"
	"strconv"
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
		EndsWhen: "a change alters its behaviour; that change ports what it alters",
	},
	{
		Path:     "tools/run_tool_tests.py",
		Reason:   "retired by the unit that replaces it with a Go runner (G1a of #2132)",
		EndsWhen: "that unit merges",
	},
}

//go:embed python_to_port.txt
var portFile string

// Entry is a line of python_to_port.txt: a Python file under tools/ that is still to be ported, and why it is still
// Python.
type Entry struct{ Path, Reason string }

// MinReasonLen is the shortest reason that counts as one.
const MinReasonLen = 10

// PortEntries returns the Python files under tools/ that are to be ported, with their reasons, in file order.
func PortEntries() []Entry { return parseEntries(portFile) }

// ToPort returns the paths of PortEntries, sorted as the file keeps them.
func ToPort() []string { return paths(PortEntries()) }

func paths(entries []Entry) []string {
	out := make([]string, len(entries))
	for i, e := range entries {
		out[i] = e.Path
	}
	return out
}

// parseEntries reads "path<TAB>reason" lines; blank lines and lines that start with # are skipped, and a line without a
// tab has an empty reason.
func parseEntries(text string) []Entry {
	var out []Entry
	sc := bufio.NewScanner(strings.NewReader(text))
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		path, reason, _ := strings.Cut(line, "\t")
		out = append(out, Entry{strings.TrimSpace(path), strings.TrimSpace(reason)})
	}
	return out
}

func parseList(text string) []string { return paths(parseEntries(text)) }

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

// Added splits the entries of now whose path old did not have into those that carry a reason and those that do not: a
// file may be added to the port list when the change says why it is still Python.
func Added(old []string, now []Entry) (withReason, withoutReason []Entry) {
	had := toSet(old)
	for _, e := range now {
		if had[e.Path] {
			continue
		}
		if len(strings.TrimSpace(e.Reason)) >= MinReasonLen {
			withReason = append(withReason, e)
		} else {
			withoutReason = append(withoutReason, e)
		}
	}
	return withReason, withoutReason
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

// BaselineList returns the port list as it is at the merge-base of HEAD and origin/main, for the check that the list
// only shrinks. skipped is a reason, when the comparison cannot be made for a reason that is not a fault of the tree
// (there is no origin/main here, or the list is new in this change); err is set when it should be possible and is not:
// origin/main exists but shares no history with HEAD, as in a shallow clone, which must not look like a pass.
func BaselineList(root string, env []string) (list []string, skipped string, err error) {
	git := func(args ...string) (string, error) {
		cmd := exec.Command("git", args...)
		cmd.Dir, cmd.Env = root, env
		out, err := cmd.Output()
		return strings.TrimSpace(string(out)), err
	}
	if _, err := git("rev-parse", "--verify", "--quiet", "refs/remotes/origin/main"); err != nil {
		return nil, "there is no origin/main in this clone: the list cannot be compared with the one at the merge-base; it is compared wherever origin/main exists", nil
	}
	base, err := git("merge-base", "HEAD", "origin/main")
	if err != nil || base == "" {
		return nil, "", fmt.Errorf("origin/main exists but git merge-base HEAD origin/main found nothing (%v): the history is too shallow to compare the port list with the merge-base; fetch more history (git fetch --deepen=100 origin main, or --unshallow)", err)
	}
	text, err := git("show", base+":tools/internal/policy/python_to_port.txt")
	if err != nil {
		return nil, "the port list does not exist at the merge-base " + base + ": it is new in this change", nil
	}
	return parseList(text), "", nil
}

// ServeForeverExempt reports whether a Python test file is left out of the serve_forever scan: the file that holds the
// scan's own Python implementation and its examples, which names serve_forever in code the scan would flag.
func ServeForeverExempt(path string) bool { return path == "tools/tests/test_run_tool_tests.py" }

// MaxPollInterval is the longest poll_interval a test server may use, in seconds. socketserver's serve_forever notices a
// stop request once per poll_interval (0.5 s by default), so every shutdown() of a test server waits out up to that.
const MaxPollInterval = 0.05

var (
	callPollRE   = regexp.MustCompile(`(?:^|,)\s*poll_interval\s*=\s*([^,\s)]+)`)
	dictPollRE   = regexp.MustCompile(`["']poll_interval["']\s*:\s*([^,}\s]+)`)
	kwargsDictRE = regexp.MustCompile(`kwargs\s*=\s*\{`)
	targetRE     = regexp.MustCompile(`target\s*=\s*[\w.]*$`)
)

// ServeForeverProblems returns one "name:line: problem" for each serve_forever in the Python source that is not given a
// poll_interval of MaxPollInterval or less: a call `x.serve_forever(poll_interval=0.01)`, or a reference
// `Thread(target=x.serve_forever, kwargs={"poll_interval": 0.01})`. Any other use is a problem too, since the scan cannot
// read it. Comments are skipped. This is the scan the Python suite had in tools/tests/test_run_tool_tests.py, read from
// the text because Go has no Python parser.
func ServeForeverProblems(name, src string) []string {
	var problems []string
	add := func(i int, msg string) {
		problems = append(problems, fmt.Sprintf("%s:%d: %s", name, 1+strings.Count(src[:i], "\n"), msg))
	}
	for from := 0; ; {
		rel := strings.Index(src[from:], "serve_forever")
		if rel < 0 {
			break
		}
		i := from + rel
		from = i + len("serve_forever")
		lineStart := strings.LastIndexByte(src[:i], '\n') + 1
		if strings.Contains(src[lineStart:i], "#") {
			continue
		}
		after := src[from:]
		if strings.HasPrefix(after, "(") {
			args := balanced(after, '(', ')')
			if m := callPollRE.FindStringSubmatch(args); m != nil {
				if msg := checkInterval("serve_forever call", m[1]); msg != "" {
					add(i, msg)
				}
			} else {
				add(i, "serve_forever call: no poll_interval (the default is 0.5 s)")
			}
			continue
		}
		if !targetRE.MatchString(src[lineStart:i]) && !targetRE.MatchString(src[max(0, i-80):i]) {
			add(i, "serve_forever used in a way this scan does not read; start it as "+
				`Thread(target=x.serve_forever, kwargs={"poll_interval": 0.01}) or x.serve_forever(poll_interval=0.01)`)
			continue
		}
		call := enclosingCall(src, i)
		if loc := kwargsDictRE.FindStringIndex(call); loc != nil {
			dict := balanced(call[loc[1]-1:], '{', '}')
			if m := dictPollRE.FindStringSubmatch(dict); m != nil {
				if msg := checkInterval("Thread target=serve_forever", m[1]); msg != "" {
					add(i, msg)
				}
				continue
			}
		}
		add(i, "Thread target=serve_forever: no poll_interval (the default is 0.5 s)")
	}
	sort.Strings(problems)
	return problems
}

func checkInterval(what, literal string) string {
	v, err := strconv.ParseFloat(literal, 64)
	if err != nil {
		return what + ": poll_interval is not a literal number"
	}
	if v > MaxPollInterval {
		return fmt.Sprintf("%s: poll_interval=%v is over %v", what, literal, MaxPollInterval)
	}
	return ""
}

// balanced returns the text after the opening bracket that starts s, up to its matching close; all of s when unmatched.
func balanced(s string, open, close byte) string {
	depth := 0
	for i := 0; i < len(s); i++ {
		switch s[i] {
		case open:
			depth++
		case close:
			depth--
			if depth == 0 {
				return s[1:i]
			}
		}
	}
	return s[1:]
}

// enclosingCall returns the text of the call that contains offset i: from its opening parenthesis to its closing one.
func enclosingCall(src string, i int) string {
	depth := 0
	start := 0
	for j := i - 1; j >= 0; j-- {
		switch src[j] {
		case ')':
			depth++
		case '(':
			if depth == 0 {
				start = j
				return src[start : start+1+len(balanced(src[start:], '(', ')'))+1]
			}
			depth--
		}
	}
	return src[i:]
}

// Recipe returns the signature line and the body lines (without their four-space indent) of the recipe called name in
// a justfile; attribute lines such as [positional-arguments] directly above it are returned in attrs.
func Recipe(justfile, name string) (attrs []string, header string, body []string, ok bool) {
	lines := strings.Split(justfile, "\n")
	for i, l := range lines {
		if !strings.HasPrefix(l, name+" ") && !strings.HasPrefix(l, name+":") {
			continue
		}
		for j := i - 1; j >= 0 && strings.HasPrefix(lines[j], "["); j-- {
			attrs = append([]string{lines[j]}, attrs...)
		}
		for _, b := range lines[i+1:] {
			if b != "" && !strings.HasPrefix(b, "    ") && !strings.HasPrefix(b, "\t") {
				break
			}
			body = append(body, strings.TrimPrefix(b, "    "))
		}
		return attrs, l, body, true
	}
	return nil, "", nil, false
}
