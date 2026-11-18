package main

import (
	"fmt"
	"path"
	"regexp"
	"sort"
	"strconv"
	"strings"

	"github.com/EchoTools/nevr-runtime/tools/internal/testmap"
)

// A test that walks the tree reads files its text never names: test_spectatorstream_order rglobs src/ for *.cpp, so a new
// file there can break it, and `refs` cannot know which. The generator therefore reads each test's walk out of its text
// and writes it as a scanner (the same thing rules.go's scanners are): the files the walk reaches select the test.
//
// Python (tools/tests/test_*.py) is read here: the walk calls .rglob/.glob/.iterdir, os.walk and `git ls-files`, each on
// an expression that starts at a name that is the repository (a Path(__file__).parents[N] assignment) and is extended by
// string literals. A walk on anything else (a temporary directory, an attribute of self, a name imported from another module,
// os.path.join, glob.glob, os.listdir) makes no scanner: such a walk is left to the references (`refs`) that name its files,
// and is a limit of this reader. A part of the path or a glob pattern that is not a whole literal widens the walk to
// everything below the literal part; iterdir and glob("*") read directory entries, so they widen to everything below.
// Go packages under tools/ are not parsed: a package whose code walks the tree (filepath.Walk, fs.WalkDir, `ls-files`) is
// declared in goWalks, with the globs it reads or the reason it reads none, and the generator refuses one that is not.

// walkScannerPrefix starts the name of each scanner this file derives.
const walkScannerPrefix = "walk-"

// parentDirs names the directory of a test file in tools/tests, then tools, then the repository, by the number of
// parents (parents[0], parents[1], parents[2]).
var parentDirs = []string{"tools/tests", "tools", ""}

// The submatch groups of the regexps below that are read by number, and the shortest text trimParens can unwrap ("()").
const (
	literalSingleGroup = 2 // the single-quoted body of pyLiteralRe
	forListGroup       = 2 // what a `for name in ...` loops over (pyForRe)
	assignRhsGroup     = 2 // the right-hand side of pyAssignRe
	paramDefaultGroup  = 2 // the default of a def parameter (pyParamRe)
	minParenthesised   = 2
	parentTailGroup    = 2 // the .parent steps that follow parents[N] (pyParentsRe)
)

var (
	pyAssignRe  = regexp.MustCompile(`(?m)^[ \t]*([A-Za-z_]\w*)[ \t]*(?::[^=\n]+)?=[ \t]*([^=\n].*)$`)
	pyDefRe     = regexp.MustCompile(`(?m)^[ \t]*def[ \t]+\w+\(([^)]*)\)`)
	pyParamRe   = regexp.MustCompile(`^\s*([A-Za-z_]\w*)\s*(?::[^=]+)?=\s*([A-Za-z_]\w*)\s*$`)
	pyForRe     = regexp.MustCompile(`\bfor\s+([A-Za-z_][\w, ]*?)\s+in\s+([^\n]*)`)
	pyAugRe     = regexp.MustCompile(`(?m)^[ \t]*([A-Za-z_]\w*)[ \t]*(?:[-+*/%&|^@]|//|<<|>>|\*\*)=`)
	pyUnpackRe  = regexp.MustCompile(`(?m)^[ \t]*([A-Za-z_][\w ]*(?:,[ \w]*)+)=[^=]`)
	pyLiteralRe = regexp.MustCompile(`"([^"\n]*)"|'([^'\n]*)'`)
	pyParentsRe = regexp.MustCompile(`parents\[(\d+)\]((?:\.parent)*)`)
	pyNameRe    = regexp.MustCompile(`^[A-Za-z_]\w*$`)
	pyWalkRe    = regexp.MustCompile(`\.(rglob|glob|iterdir)\(|\bos\.walk\(`)
	pyWordRe    = regexp.MustCompile(`[A-Za-z_]\w*`)
	lsFilesRe   = regexp.MustCompile(`["']ls-files["']`)
	goWalkRe    = regexp.MustCompile(`filepath\.Walk(Dir)?\(|fs\.WalkDir\(|"ls-files"`)
)

// pv is a repository directory a walk starts at; dynamic says the path went on past dir by something not a literal.
type pv struct {
	dir     string
	dynamic bool
}

// pyWalker is what one Python text says about the repository paths its names hold.
type pyWalker struct {
	env        map[string][]pv     // names that hold a repository path, with each value an assignment gave them
	consts     map[string][]string // names that hold a string literal, or loop over literals
	nonLiteral map[string]bool     // names that are also bound to something that is not a literal: their literals are not all they hold
}

// literals returns the string literals of s, in order.
func literals(s string) []string {
	var out []string
	for _, m := range pyLiteralRe.FindAllStringSubmatch(s, -1) {
		if strings.HasPrefix(m[0], `"`) {
			out = append(out, m[1])
		} else {
			out = append(out, m[literalSingleGroup])
		}
	}
	return out
}

// splitTop splits s at sep where it is outside quotes and brackets.
func splitTop(s string, sep byte) []string {
	var parts []string
	depth, start := 0, 0
	var quote byte
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case quote != 0:
			if c == quote {
				quote = 0
			}
		case c == '"' || c == '\'':
			quote = c
		case c == '(' || c == '[' || c == '{':
			depth++
		case c == ')' || c == ']' || c == '}':
			depth--
		case c == sep && depth == 0:
			parts = append(parts, s[start:i])
			start = i + 1
		}
	}
	return append(parts, s[start:])
}

// encloses reports whether the first parenthesis of s closes at its last character.
func encloses(s string) bool {
	depth := 0
	var quote byte
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case quote != 0:
			if c == quote {
				quote = 0
			}
		case c == '"' || c == '\'':
			quote = c
		case c == '(':
			depth++
		case c == ')':
			depth--
			if depth == 0 {
				return i == len(s)-1
			}
		}
	}
	return false
}

// trimParens removes whitespace and any parentheses that enclose the whole of s.
func trimParens(s string) string {
	for {
		s = strings.TrimSpace(s)
		if len(s) < minParenthesised || s[0] != '(' || !encloses(s) {
			return s
		}
		s = s[1 : len(s)-1]
	}
}

// newPyWalker reads the assignments, parameter defaults and loops of text that make up the paths a walk may start at.
func newPyWalker(text string) *pyWalker {
	w := &pyWalker{env: map[string][]pv{}, consts: map[string][]string{}, nonLiteral: map[string]bool{}}
	for _, m := range pyForRe.FindAllStringSubmatch(text, -1) {
		names := strings.Split(m[1], ",")
		lits, whole := literalElements(m[forListGroup])
		for _, n := range names {
			n = strings.TrimSpace(n)
			if len(names) == 1 && whole {
				w.consts[n] = append(w.consts[n], lits...)
			} else {
				w.nonLiteral[n] = true // a loop over anything else, or unpacked: its values are not known
			}
		}
	}
	for _, m := range pyAugRe.FindAllStringSubmatch(text, -1) {
		w.nonLiteral[m[1]] = true
	}
	for _, m := range pyUnpackRe.FindAllStringSubmatch(text, -1) {
		for _, n := range strings.Split(m[1], ",") {
			w.nonLiteral[strings.TrimSpace(n)] = true
		}
	}
	for _, d := range pyDefRe.FindAllStringSubmatch(text, -1) {
		for _, p := range splitTop(d[1], ',') {
			if name := pyWordRe.FindString(p); name != "" {
				w.nonLiteral[name] = true // a parameter holds what its caller passes
			}
		}
	}
	assigns := pyAssignRe.FindAllStringSubmatch(text, -1)
	for _, m := range assigns {
		if rhs := strings.TrimSpace(m[assignRhsGroup]); pyLiteralRe.FindString(rhs) != rhs {
			w.nonLiteral[m[1]] = true // bound to something that is not a literal (a name that holds a path has no literals)
		}
	}
	const passes = 3 // a name may be defined from one defined after it in the text
	for pass := 0; pass < passes; pass++ {
		for _, m := range assigns {
			name, rhs := m[1], strings.TrimSpace(m[assignRhsGroup])
			if strings.Contains(rhs, "__file__") {
				parts := splitTop(rhs, '/')
				if p, ok := fileDir(parts[0]); ok {
					for _, v := range w.extend([]pv{{dir: p}}, parts[1:]) {
						w.addEnv(name, v)
					}
				}
				continue
			}
			if vals, ok := w.resolve(rhs); ok {
				for _, v := range vals {
					w.addEnv(name, v)
				}
			} else if lits := literals(rhs); len(lits) == 1 && pyLiteralRe.FindString(rhs) == rhs {
				if !contains(w.consts[name], lits[0]) {
					w.consts[name] = append(w.consts[name], lits[0])
				}
			} else {
				w.nonLiteral[name] = true
			}
		}
		for _, d := range pyDefRe.FindAllStringSubmatch(text, -1) {
			for _, p := range splitTop(d[1], ',') {
				if pm := pyParamRe.FindStringSubmatch(p); pm != nil {
					for _, v := range w.env[pm[paramDefaultGroup]] {
						w.addEnv(pm[1], v)
					}
				}
			}
		}
	}
	return w
}

// literalElements returns the literals of a parenthesised or bracketed list that opens s, and whether every element of it
// is a whole literal.
func literalElements(s string) ([]string, bool) {
	s = strings.TrimSpace(s)
	if s == "" || (s[0] != '(' && s[0] != '[') {
		return nil, false
	}
	body := ""
	if s[0] == '(' {
		body = callArgs(s, 0)
	} else {
		body = bracketBody(s)
	}
	var lits []string
	for _, el := range splitTop(body, ',') {
		el = strings.TrimSpace(el)
		if el == "" {
			continue
		}
		l := literals(el)
		if len(l) != 1 || pyLiteralRe.FindString(el) != el {
			return nil, false
		}
		lits = append(lits, l[0])
	}
	return lits, len(lits) > 0
}

// bracketBody is the text between the [ that opens s and its match.
func bracketBody(s string) string {
	depth := 0
	var quote byte
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case quote != 0:
			if c == quote {
				quote = 0
			}
		case c == '"' || c == '\'':
			quote = c
		case c == '[':
			depth++
		case c == ']':
			depth--
			if depth == 0 {
				return s[1:i]
			}
		}
	}
	return s[1:]
}

// tripleQuote is the length of a Python triple quote.
const tripleQuote = 3

// joinContinued puts each statement on one line: a newline inside brackets becomes a space. Comments and the text of
// strings, triple-quoted ones included, are copied as they are and do not count as brackets.
func joinContinued(text string) string {
	var out strings.Builder
	depth := 0
	var quote string // the quote that opened the string we are in
	for i := 0; i < len(text); i++ {
		c := text[i]
		switch {
		case quote != "":
			if strings.HasPrefix(text[i:], quote) {
				out.WriteString(quote)
				i += len(quote) - 1
				quote = ""
				continue
			}
			if c == '\\' && i+1 < len(text) { // an escaped character never closes the string (a raw string included)
				out.WriteByte(c)
				i++
				c = text[i]
			}
			if c == '\n' && len(quote) < tripleQuote {
				quote = ""
			}
		case c == '#':
			end := strings.IndexByte(text[i:], '\n')
			if end < 0 {
				end = len(text) - i
			}
			out.WriteString(text[i : i+end])
			i += end - 1
			continue
		case c == '"' || c == '\'':
			quote = string(c)
			if strings.HasPrefix(text[i:], strings.Repeat(string(c), tripleQuote)) {
				quote = strings.Repeat(string(c), tripleQuote)
				out.WriteString(quote)
				i += tripleQuote - 1
				continue
			}
		case c == '(' || c == '[' || c == '{':
			depth++
		case c == ')' || c == ']' || c == '}':
			if depth > 0 {
				depth--
			}
		case c == '\n' && depth > 0:
			c = ' '
		}
		out.WriteByte(c)
	}
	return out.String()
}

// fileDir is the repository directory that rhs, an expression of __file__, names: Path(__file__)...parents[N], or .parent
// repeated.
func fileDir(rhs string) (string, bool) {
	if m := pyParentsRe.FindStringSubmatch(rhs); m != nil {
		n, err := strconv.Atoi(m[1])
		n += strings.Count(m[parentTailGroup], ".parent") // each .parent after the index is one directory up
		if err == nil && n < len(parentDirs) {
			return parentDirs[n], true
		}
		return "", false
	}
	if c := strings.Count(rhs, ".parent"); c >= 1 && c <= len(parentDirs) {
		return parentDirs[c-1], true
	}
	return "", false
}

func contains(list []string, s string) bool {
	for _, x := range list {
		if x == s {
			return true
		}
	}
	return false
}

func (w *pyWalker) addEnv(name string, v pv) {
	for _, have := range w.env[name] {
		if have == v {
			return
		}
	}
	w.env[name] = append(w.env[name], v)
}

// resolve returns the repository paths expr names: a name that holds one, extended by `/ "literal"` parts, each part a
// literal, a name that holds literals, or something else that makes the path dynamic. ok is false when expr does not start
// at a repository path.
func (w *pyWalker) resolve(expr string) ([]pv, bool) {
	expr = trimParens(expr)
	parts := splitTop(expr, '/')
	first := trimParens(parts[0])
	var cur []pv
	if pyNameRe.MatchString(first) {
		cur = append(cur, w.env[first]...)
	} else if first != expr { // the first part is itself a path expression, such as a parenthesised group
		cur, _ = w.resolve(first)
	}
	if len(cur) == 0 {
		return nil, false
	}
	return w.extend(cur, parts[1:]), true
}

// extend appends each `/ part` to every path of cur: a literal, a name that holds literals, or else a dynamic tail.
func (w *pyWalker) extend(cur []pv, parts []string) []pv {
	for _, part := range parts {
		part = trimParens(part)
		var next []pv
		switch lits := literals(part); {
		case len(lits) == 1 && pyLiteralRe.FindString(part) == part:
			for _, c := range cur {
				if c.dynamic { // below an unknown part: the walk stays at its literal prefix
					next = append(next, c)
					continue
				}
				next = append(next, pv{dir: path.Join(c.dir, lits[0])})
			}
		case pyNameRe.MatchString(part) && len(w.consts[part]) > 0 && !w.nonLiteral[part]:
			for _, c := range cur {
				if c.dynamic {
					next = append(next, c)
					continue
				}
				for _, l := range w.consts[part] {
					next = append(next, pv{dir: path.Join(c.dir, l)})
				}
			}
		default:
			for _, c := range cur {
				next = append(next, pv{dir: c.dir, dynamic: true})
			}
		}
		cur = next
	}
	return cur
}

// receiverBefore is the expression the method call whose dot is at text[dot] is made on: a parenthesised group and the
// name before it, or a dotted name.
func receiverBefore(text string, dot int) string {
	j := dot - 1
	end := dot
	if j >= 0 && text[j] == ')' {
		depth := 0
		for ; j >= 0; j-- {
			if text[j] == ')' {
				depth++
			} else if text[j] == '(' {
				depth--
				if depth == 0 {
					break
				}
			}
		}
		if j < 0 {
			return ""
		}
		j--
	}
	for j >= 0 && (text[j] == '_' || text[j] == '.' || (text[j] >= '0' && text[j] <= '9') ||
		(text[j] >= 'a' && text[j] <= 'z') || (text[j] >= 'A' && text[j] <= 'Z')) {
		j--
	}
	return text[j+1 : end]
}

// callArgs is the text between the parenthesis at text[open] and its match.
func callArgs(text string, open int) string {
	depth := 0
	var quote byte
	for i := open; i < len(text); i++ {
		c := text[i]
		switch {
		case quote != 0:
			if c == quote {
				quote = 0
			}
		case c == '"' || c == '\'':
			quote = c
		case c == '(':
			depth++
		case c == ')':
			depth--
			if depth == 0 {
				return text[open+1 : i]
			}
		}
	}
	return text[open+1:]
}

// globFor is the glob of the files a walk of kind (rglob, glob, iterdir, walk) with pattern pat reaches below v.
func globFor(kind string, v pv, pat string) string {
	switch kind {
	case "iterdir", "walk":
		return path.Join(v.dir, "**")
	case "rglob":
		return path.Join(v.dir, "**", pat)
	}
	if v.dynamic {
		return path.Join(v.dir, "**", path.Base(pat))
	}
	if path.Base(pat) == "*" { // a last segment that also matches a directory: a file in a new subdirectory changes the result
		return path.Join(v.dir, path.Dir(pat), "**")
	}
	return path.Join(v.dir, pat)
}

// pyWalks is the globs of the repository files the Python text walks.
func pyWalks(text string) []string {
	text = joinContinued(text)
	w := newPyWalker(text)
	set := map[string]bool{}
	for _, loc := range pyWalkRe.FindAllStringIndex(text, -1) {
		call := text[loc[0]:loc[1]]
		open := loc[1] - 1
		args := callArgs(text, open)
		var kind string
		var recv string
		switch {
		case strings.HasPrefix(call, "os.walk"):
			kind, recv = "walk", strings.TrimSpace(splitTop(args, ',')[0])
		default:
			kind, recv = strings.TrimSuffix(strings.TrimPrefix(call, "."), "("), receiverBefore(text, loc[0])
		}
		vals, ok := w.resolve(recv)
		if !ok {
			continue
		}
		pats := []string{"*"}
		unknownPattern := false
		if kind == "rglob" || kind == "glob" {
			first := strings.TrimSpace(splitTop(args, ',')[0])
			switch lits := literals(first); {
			case len(lits) == 1 && pyLiteralRe.FindString(first) == first:
				pats = lits
			case len(w.consts[first]) > 0 && !w.nonLiteral[first]:
				pats = w.consts[first]
			default:
				unknownPattern = true // a glob pattern may hold ** and /: it can reach any depth
			}
		}
		for _, v := range vals {
			if unknownPattern {
				v.dynamic = true
			}
			for _, p := range pats {
				set[globFor(kind, v, p)] = true
				if kind == "rglob" && !strings.ContainsAny(p, "*?[") { // the pattern may name a directory: a file added below it adds the name
					set[path.Join(v.dir, "**", p, "**")] = true
				}
			}
		}
	}
	for _, g := range w.lsFiles(text) {
		set[g] = true
	}
	return sortedKeys(set)
}

func sortedKeys(set map[string]bool) []string {
	var out []string
	for k := range set {
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}

// lsFilesFlags are the `git ls-files` options that take no value. Any other option before the pathspecs may take the next
// element as its value (--format, --with-tree, -x), so the call is read as listing every file.
var lsFilesFlags = map[string]bool{
	"-z": true, "-c": true, "-o": true, "-s": true, "-d": true, "-m": true, "-k": true, "-i": true, "-t": true, "-v": true, "-f": true, "-u": true,
	"--cached": true, "--others": true, "--deleted": true, "--modified": true, "--stage": true, "--ignored": true,
	"--exclude-standard": true, "--full-name": true, "--error-unmatch": true, "--eol": true, "--unmerged": true, "--killed": true,
}

// lsFiles is the globs of `git ls-files` calls made in the repository: the call names the repository by a name that holds
// it (-C ROOT, cwd=ROOT), and each pathspec literal is a glob; none, or a spec that is not a literal, is every file.
func (w *pyWalker) lsFiles(text string) []string {
	var globs []string
	for _, loc := range lsFilesRe.FindAllStringIndex(text, -1) {
		dirs := w.repoDirs(text, loc[0])
		if len(dirs) == 0 {
			continue
		}
		// The elements after "ls-files" up to the end of their list.
		rest := text[loc[1]:]
		end := listEnd(rest)
		var specs []string
		top := false
		wide := strings.HasPrefix(strings.TrimSpace(rest[min(end+1, len(rest)):]), "+") // the list is extended past its end
		for _, el := range splitTop(rest[:end], ',') {
			el = strings.TrimSpace(el)
			if el == "" {
				continue
			}
			if lits := literals(el); len(lits) == 1 && pyLiteralRe.FindString(el) == el {
				switch {
				case lits[0] == "--":
				case strings.HasPrefix(lits[0], "-"):
					if !lsFilesFlags[lits[0]] { // an option that may take the next element as its value: that element is no pathspec
						wide = true
					}
				case strings.HasPrefix(lits[0], magicPathspec): // git, not the directory, decides which files it lists
					top = true
				default:
					specs = append(specs, lits[0])
				}
			} else {
				wide = true
			}
		}
		if top {
			globs = append(globs, "**")
			continue
		}
		for _, d := range dirs {
			if strings.HasSuffix(d, "**") { // a directory known only down to its literal part: every file below it
				globs = append(globs, d)
				continue
			}
			if wide || len(specs) == 0 {
				globs = append(globs, path.Join(d, "**"))
				continue
			}
			for _, sp := range specs {
				for _, g := range specGlobs(sp) {
					globs = append(globs, path.Join(d, g))
				}
			}
		}
	}
	return globs
}

// repoDirs is the repository directories the git call that holds text[at] runs in: the paths of the names the call, or a
// call around it, mentions. None when it names no repository path (a temporary repository).
func (w *pyWalker) repoDirs(text string, at int) []string {
	set := map[string]bool{}
	for open := enclosingOpen(text, at); ; {
		words := map[string]bool{}
		for _, word := range pyWordRe.FindAllString(callArgs(text, open), -1) {
			words[word] = true
		}
		for name, vals := range w.env {
			if words[name] {
				for _, v := range vals {
					if v.dynamic { // the directory has a part that is not a literal: the call may run anywhere below the literal part
						set[path.Join(v.dir, "**")] = true
					} else {
						set[v.dir] = true
					}
				}
			}
		}
		next := enclosingOpen(text, open)
		if len(set) > 0 || next == 0 {
			break
		}
		open = next
	}
	return sortedKeys(set)
}

// listEnd is the index in s of the bracket that closes the list s is inside: the first ] or ) outside nested brackets.
func listEnd(s string) int {
	depth := 0
	var quote byte
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case quote != 0:
			if c == quote {
				quote = 0
			}
		case c == '"' || c == '\'':
			quote = c
		case c == '(' || c == '[':
			depth++
		case c == ')' || c == ']':
			if depth == 0 {
				return i
			}
			depth--
		}
	}
	return len(s)
}

// enclosingOpen is the index of the parenthesis that opens the call text[i] is inside, or 0.
func enclosingOpen(text string, i int) int {
	depth := 0
	for j := i - 1; j >= 0; j-- {
		switch text[j] {
		case ')':
			depth++
		case '(':
			if depth == 0 {
				return j
			}
			depth--
		}
	}
	return 0
}

// specGlobs is the globs a git pathspec stands for: a path names itself and what is below it, and a * in a pathspec
// crosses directories.
func specGlobs(spec string) []string {
	if !strings.ContainsAny(spec, "*?[") {
		return []string{spec, spec + "/**"}
	}
	segs := strings.Split(spec, "/")
	var prefix []string
	for i, s := range segs {
		if strings.ContainsAny(s, "*?[") {
			rest := strings.Join(segs[i:], "/")
			// A wildcard crosses "/" and may end inside a directory name, so only a pattern that opens with * and ends in a
			// literal names its files by their last segment; any other reaches everything below the literal prefix.
			if i == len(segs)-1 && strings.HasPrefix(s, "*") && !strings.ContainsAny(s[1:], "*?[") {
				return []string{path.Join(append(prefix, "**", rest)...)}
			}
			return []string{path.Join(append(prefix, "**")...)}
		}
		prefix = append(prefix, s)
	}
	return []string{spec}
}

// magicPathspec starts a git pathspec with magic: a pathspec that is not a path.
const magicPathspec = ":"

// walkScanner is the scanner of one test: its id and the globs it reads.
func walkScanner(stem, id string, globs []string) testmap.Scanner {
	return testmap.Scanner{Name: walkScannerPrefix + stem, Globs: globs, Tests: []string{id}}
}

// goWalk is what a Go package that walks the tree reads: the globs of the repository files, or why it reads none.
type goWalk struct {
	globs   []string
	scanner string // the name of a scanner in rules.go that already selects the package
	why     string
}

// goWalks declares each package under tools/ whose Go text walks a tree. A package with a walk call that is not here fails
// the generator: whether the walk reads the repository, and which files, is a decision a person makes here.
var goWalks = map[string]goWalk{
	"cmd/one-home":         {scanner: "one-home", why: "reads every tracked Markdown file"},
	"internal/policy":      {scanner: "go-policy", why: "reads every tracked tools/*.py and tools/*.go"},
	"internal/questwiring": {globs: []string{"**"}, why: "deleted_plugin_name_test reads every tracked file, and its other tests read src/reconstruction, src/quest, docs and tools/internal"},
	"internal/testshelf": {globs: []string{"src/**/*.cpp", "src/**/*.cc", "src/**/*.h", "src/quest/host_tests/**/*.cmake", "src/quest/host_tests/**/CMakeLists.txt"},
		why: "treeHas reads the C++ sources and the host tests' CMake files for the shelf's entries"},
	"cmd/quest-keystore":   {why: "walks a temporary directory the test makes"},
	"cmd/run-copy":         {why: "walks a copy of a game install"},
	"cmd/test-map":         {why: "lists the tracked files of the checkout it maps; a changed path is mapped by this generator"},
	"internal/questrepack": {why: "walks a staged pack directory the test makes"},
	"internal/testmap":     {why: "lists the tracked files to check each has a map entry; test-changed refuses a changed path the map lacks"},
}

// walkScanners derives a scanner for each Python test that walks the repository, and checks that each Go package that
// walks a tree is declared in goWalks.
func (m *mapper) walkScanners(decl map[string]goWalk) ([]testmap.Scanner, error) {
	var out []testmap.Scanner
	texts := map[string]string{}
	var fixtures, tests []string
	for _, f := range m.tracked {
		if path.Dir(f) != "tools/tests" || !strings.HasSuffix(f, ".py") || !strings.HasPrefix(path.Base(f), "test_") {
			continue
		}
		text, err := m.readRepo(f)
		if err != nil {
			return nil, err
		}
		texts[f] = text
		if definesPytest.MatchString(text) {
			tests = append(tests, f)
		} else {
			fixtures = append(fixtures, f)
		}
	}
	for _, f := range tests {
		stem := strings.TrimSuffix(path.Base(f), ".py")
		set := map[string]bool{}
		for _, g := range pyWalks(texts[f]) {
			set[g] = true
		}
		for _, fx := range fixtures {
			if importsStem(texts[f], strings.TrimSuffix(path.Base(fx), ".py")) {
				for _, g := range pyWalks(texts[fx]) {
					set[g] = true
				}
			}
		}
		if len(set) > 0 {
			out = append(out, walkScanner(stem, testmap.KindPytest+":"+stem, sortedKeys(set)))
		}
	}
	var undeclared, stale []string
	for _, rel := range m.goPkgs {
		walks := false
		for _, f := range m.tracked {
			if path.Dir(f) != "tools/"+rel || !strings.HasSuffix(f, ".go") {
				continue
			}
			text, err := m.readRepo(f)
			if err != nil {
				return nil, err
			}
			if goWalkRe.MatchString(text) {
				walks = true
			}
		}
		w, ok := decl[rel]
		switch {
		case walks && !ok:
			undeclared = append(undeclared, "tools/"+rel)
		case ok && !walks:
			stale = append(stale, "tools/"+rel)
		case ok && len(w.globs) > 0:
			out = append(out, walkScanner(strings.ReplaceAll(rel, "/", "-"), testmap.KindGo+":"+rel, w.globs))
		}
	}
	if len(undeclared) > 0 {
		return nil, failure{exitUncovered, fmt.Sprintf(
			"%d Go package(s) walk a tree and are not in goWalks (tools/cmd/test-map/walkers.go): %s. Declare the globs the "+
				"package reads, or the reason it reads no repository file", len(undeclared), strings.Join(undeclared, ", "))}
	}
	if len(stale) > 0 {
		return nil, failure{exitUncovered, fmt.Sprintf("goWalks names package(s) whose Go text walks no tree: %s", strings.Join(stale, ", "))}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Name < out[j].Name })
	return out, nil
}
