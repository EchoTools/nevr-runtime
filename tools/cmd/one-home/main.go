// Command one-home fails when a named procedure is written in more than one document.
//
//	one-home [-root dir] [-registry file]
//
// The registry (default docs/standards/procedure-homes.txt, relative to -root) names each procedure, the one file that
// holds its steps, and a marker: a regular expression that matches a line of those steps. One line per marker, three
// fields separated by tabs (name, home file, regular expression); a line that starts with # is a comment.
//
//	landing<TAB>AGENTS.md<TAB>HEAD:refs/heads/main
//
// For each marker the command reads every Markdown file under -root (the files `git ls-files` lists when -root is a git
// work tree, otherwise a walk that skips .git, build, dist, extern and _temp) except the home, the registry and the dated
// records under docs/audits/, and prints each line that matches as "<name>: <file>:<line>: <text>". A marker that
// matches no line of its home is itself a failure: a marker that matches nothing in the place the procedure lives guards
// nothing.
//
// Exit codes: 0 every procedure has one home; 1 a procedure is written outside its home; 2 the registry or a flag is
// wrong, or a marker matches nothing in its home.
package main

import (
	"bufio"
	"bytes"
	"flag"
	"fmt"
	"io"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
)

const defaultRegistry = "docs/standards/procedure-homes.txt"

// exemptDirs are the directories whose records are dated by design (docs/audits/README.md) and are not read.
var exemptDirs = []string{"docs/audits/"}

// marker is one registry line.
type marker struct {
	name, home string
	re         *regexp.Regexp
	src        string
}

func main() { os.Exit(run(os.Args[1:], os.Stdout, os.Stderr)) }

func run(args []string, stdout, stderr io.Writer) int {
	fl := flag.NewFlagSet("one-home", flag.ContinueOnError)
	fl.SetOutput(stderr)
	root := fl.String("root", ".", "repository root")
	registry := fl.String("registry", defaultRegistry, "registry file, relative to -root")
	if err := fl.Parse(args); err != nil {
		return 2
	}
	markers, err := readRegistry(filepath.Join(*root, *registry))
	if err != nil {
		fmt.Fprintf(stderr, "one-home: %v\n", err)
		return 2
	}
	files, err := markdownFiles(*root)
	if err != nil {
		fmt.Fprintf(stderr, "one-home: %v\n", err)
		return 2
	}
	registryRel := filepath.ToSlash(filepath.Clean(*registry))
	lines := map[string][]string{}
	for _, f := range files {
		b, err := os.ReadFile(filepath.Join(*root, f))
		if err != nil {
			fmt.Fprintf(stderr, "one-home: %v\n", err)
			return 2
		}
		lines[f] = strings.Split(string(b), "\n")
	}

	var broken, found []string
	for _, m := range markers {
		homeLines, ok := lines[m.home]
		if !ok {
			broken = append(broken, fmt.Sprintf("%s: home %s is not a Markdown file of the tree (marker %q)", m.name, m.home, m.src))
			continue
		}
		if !anyMatch(m.re, homeLines) {
			broken = append(broken, fmt.Sprintf("%s: marker %q matches no line of its home %s", m.name, m.src, m.home))
			continue
		}
		for _, f := range files {
			if f == m.home || f == registryRel || exempt(f) {
				continue
			}
			for i, l := range lines[f] {
				if m.re.MatchString(l) {
					found = append(found, fmt.Sprintf("%s: %s:%d: %s", m.name, f, i+1, clip(l)))
				}
			}
		}
	}
	sort.Strings(broken)
	sort.Strings(found)
	for _, s := range broken {
		fmt.Fprintln(stderr, "one-home:", s)
	}
	if len(broken) > 0 {
		return 2
	}
	for _, s := range found {
		fmt.Fprintln(stdout, s)
	}
	if len(found) > 0 {
		fmt.Fprintf(stderr, "one-home: %d line(s) write a procedure outside its home; keep the steps in the home and link to them\n", len(found))
		return 1
	}
	return 0
}

func anyMatch(re *regexp.Regexp, lines []string) bool {
	for _, l := range lines {
		if re.MatchString(l) {
			return true
		}
	}
	return false
}

func exempt(f string) bool {
	for _, d := range exemptDirs {
		if strings.HasPrefix(f, d) {
			return true
		}
	}
	return false
}

func clip(s string) string {
	s = strings.TrimSpace(s)
	if len(s) > 120 {
		return s[:120] + "..."
	}
	return s
}

func readRegistry(path string) ([]marker, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, fmt.Errorf("registry: %w", err)
	}
	defer f.Close()
	var out []marker
	sc := bufio.NewScanner(f)
	for n := 1; sc.Scan(); n++ {
		line := sc.Text()
		if strings.TrimSpace(line) == "" || strings.HasPrefix(line, "#") {
			continue
		}
		parts := strings.Split(line, "\t")
		if len(parts) != 3 || parts[0] == "" || parts[1] == "" || parts[2] == "" {
			return nil, fmt.Errorf("registry line %d: want name, home and regular expression separated by tabs", n)
		}
		re, err := regexp.Compile(parts[2])
		if err != nil {
			return nil, fmt.Errorf("registry line %d: %w", n, err)
		}
		out = append(out, marker{name: parts[0], home: parts[1], re: re, src: parts[2]})
	}
	if err := sc.Err(); err != nil {
		return nil, fmt.Errorf("registry: %w", err)
	}
	if len(out) == 0 {
		return nil, fmt.Errorf("registry %s names no procedure", path)
	}
	return out, nil
}

// markdownFiles lists the Markdown files of the tree, slash-separated and relative to root.
func markdownFiles(root string) ([]string, error) {
	var all []string
	if _, err := os.Stat(filepath.Join(root, ".git")); err == nil {
		cmd := exec.Command("git", "-C", root, "ls-files", "-z")
		var out bytes.Buffer
		cmd.Stdout = &out
		if err := cmd.Run(); err != nil {
			return nil, fmt.Errorf("git ls-files: %w", err)
		}
		for _, p := range strings.Split(out.String(), "\x00") {
			if p != "" {
				all = append(all, p)
			}
		}
	} else {
		skip := map[string]bool{".git": true, "build": true, "dist": true, "extern": true, "_temp": true}
		err := filepath.WalkDir(root, func(p string, d fs.DirEntry, err error) error {
			if err != nil {
				return err
			}
			rel, _ := filepath.Rel(root, p)
			if d.IsDir() && skip[rel] {
				return filepath.SkipDir
			}
			if !d.IsDir() {
				all = append(all, filepath.ToSlash(rel))
			}
			return nil
		})
		if err != nil {
			return nil, err
		}
	}
	var md []string
	for _, p := range all {
		if !strings.HasSuffix(p, ".md") {
			continue
		}
		// A tracked file that is gone from the work tree (a deletion not yet committed) is not read.
		if _, err := os.Stat(filepath.Join(root, p)); err != nil {
			continue
		}
		md = append(md, p)
	}
	sort.Strings(md)
	return md, nil
}
