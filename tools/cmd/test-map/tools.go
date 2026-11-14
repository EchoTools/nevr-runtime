package main

import (
	"bytes"
	"context"
	"fmt"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"strings"
)

// executableBits is the permission bits a program on PATH must have.
const executableBits = 0o111

// The text ninja's header-dependency dump puts on a header line, and the indent of a dependency under it.
const (
	depsMarker = ": #deps"
	depIndent  = "    "
)

// toolchain runs the commands the generator reads (git, ninja, go and just) in the environment it was given. A program is
// found on that environment's PATH, not this process's, so a test's fakes stand in for the real ones.
type toolchain struct {
	ctx context.Context
	env []string
}

// output runs name with args in dir ("" for the working directory) and returns its standard output; a failure carries
// its standard error.
func (tc toolchain) output(dir, name string, args ...string) (string, error) {
	prog, err := findProgram(name, lookup(tc.env)("PATH"))
	if err != nil {
		return "", failure{exitCannotMake, err.Error()}
	}
	cmd := exec.CommandContext(tc.ctx, prog, args...)
	cmd.Args[0] = name
	cmd.Dir, cmd.Env = dir, tc.env
	var out, errb bytes.Buffer
	cmd.Stdout, cmd.Stderr = &out, &errb
	if err := cmd.Run(); err != nil {
		return "", failure{exitCannotMake, fmt.Sprintf("%s %s: %v: %s", name, strings.Join(args, " "), err, strings.TrimSpace(errb.String()))}
	}
	return out.String(), nil
}

// ninja runs ninja -C builddir -t args... and returns its standard output.
func (tc toolchain) ninja(builddir string, args ...string) (string, error) {
	return tc.output("", "ninja", append([]string{"-C", builddir, "-t"}, args...)...)
}

// findProgram resolves name on path as a shell does; a name with a slash is used as it is.
func findProgram(name, path string) (string, error) {
	if strings.Contains(name, "/") {
		return name, nil
	}
	for _, dir := range filepath.SplitList(path) {
		if dir == "" {
			continue
		}
		cand := filepath.Join(dir, name)
		if st, err := os.Stat(cand); err == nil && !st.IsDir() && st.Mode()&executableBits != 0 {
			return cand, nil
		}
	}
	return "", fmt.Errorf("%s is not on PATH", name)
}

// lines splits a command's output into its non-empty lines.
func lines(out string) []string {
	var res []string
	for _, l := range strings.Split(out, "\n") {
		if l != "" {
			res = append(res, l)
		}
	}
	return res
}

// parseDeps reads the output of `ninja -t deps`: a header line per output, `<output>: #deps N, deps mtime ...`, and under
// it each dependency indented. The keys are as ninja printed them, relative to the build directory when they are.
func parseDeps(text string) map[string][]string {
	deps := map[string][]string{}
	cur := ""
	for _, line := range strings.Split(text, "\n") {
		switch {
		case !strings.HasPrefix(line, " ") && strings.Contains(line, depsMarker):
			cur, _, _ = strings.Cut(line, depsMarker)
			deps[cur] = nil
		case strings.HasPrefix(line, depIndent) && cur != "":
			deps[cur] = append(deps[cur], strings.TrimSpace(line))
		}
	}
	return deps
}

// parseTargets reads the output of `ninja -t targets all` and returns the executables under bin/: each one's file name
// mapped to its target path (bin/test_x.exe, or bin/<dir>/test_x.exe).
func parseTargets(text string) map[string]string {
	exes := map[string]string{}
	for _, line := range lines(text) {
		target, _, _ := strings.Cut(line, ":")
		if strings.HasPrefix(target, binDir+"/") && strings.HasSuffix(target, exeSuffix) {
			if _, seen := exes[path.Base(target)]; !seen {
				exes[path.Base(target)] = target
			}
		}
	}
	return exes
}

const (
	binDir    = "bin"
	exeSuffix = ".exe"
)
