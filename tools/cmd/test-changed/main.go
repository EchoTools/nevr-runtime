// Command test-changed runs the tests that cover a branch's change, and nothing else (AGENTS.md, "Builds"):
//
//	test-changed [-repo dir] [-map file] [-base origin/main] [-preset name] [-n]
//
// The change is what `git diff --name-only $(git merge-base origin/main HEAD)` lists: the branch's commits and its
// uncommitted edits to tracked files. Each changed path belongs to one component of tools/test_map.json
// (internal/testmap); the run is the union of those components' tests, one at a time, the build first, each printed with
// its exit code. A path that was deleted is looked up in the map at the merge-base when the map no longer has it. A
// changed path that no entry of the map covers stops the run before anything starts, naming the path, so the map
// cannot drift away from the tree unnoticed.
//
// Every test runs from the checkout's top directory without DISPLAY and WAYLAND_DISPLAY (a GTest under Wine must not
// reach the desktop) and with PRESET set to -preset (default $PRESET, else mingw-release), so the build and the tests
// read one build tree. -n prints the plan and runs nothing.
//
// Exit codes: 0 every test passed, or the change needs none; 1 a test failed; 2 a flag, git or the map is wrong; 3 a
// changed path is not in the map; 130 interrupted between tests; 74 the run log could not be written.
//
// Each run is recorded in $XDG_STATE_HOME/nevr-runtime/test-changed.jsonl (internal/runlog): invoked, plan (the
// merge-base, the changed paths, their components and the tests), one test_end per test (its id, argv, exit and
// milliseconds), finished.
package main

import (
	"bytes"
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"io/fs"
	"log/slog"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"
	"time"

	"github.com/EchoTools/nevr-runtime/tools/internal/runlog"
	"github.com/EchoTools/nevr-runtime/tools/internal/testmap"
)

const (
	exitFailed      = 1
	exitUsage       = 2
	exitUncovered   = 3
	exitNotStarted  = 127
	exitInterrupted = 130
)

// execBits are the mode bits of which one makes a file a program a shell would start.
const execBits = 0o111

// tool is the program's name: its log file and the prefix of every line it prints.
const tool = "test-changed"

// hiddenFromTests are the variables no test is started with: a GTest under Wine that sees a display takes it over.
var hiddenFromTests = []string{"DISPLAY", "WAYLAND_DISPLAY"}

func main() {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	os.Exit(run(ctx, os.Args[1:], os.Environ(), os.Stdout, os.Stderr))
}

func lookup(env []string) func(string) string {
	return func(k string) string {
		v := ""
		for _, kv := range env {
			if key, val, ok := strings.Cut(kv, "="); ok && key == k {
				v = val
			}
		}
		return v
	}
}

func run(ctx context.Context, args []string, env []string, stdout, stderr io.Writer) int {
	getenv := lookup(env)
	flags := flag.NewFlagSet(tool, flag.ContinueOnError)
	flags.SetOutput(stderr)
	repo := flags.String("repo", "", "the checkout (default: the git top level of the working directory)")
	mapFile := flags.String("map", "", "the map (default: <repo>/"+testmap.DefaultPath+")")
	base := flags.String("base", "origin/main", "the ref whose merge-base with HEAD the change is measured from")
	preset := flags.String("preset", "", "the build preset: {preset} in the map and PRESET for the tests (default $PRESET, else "+testmap.DefaultPreset+")")
	dry := flags.Bool("n", false, "print the plan and run nothing")
	if err := flags.Parse(args); err != nil {
		return exitUsage
	}
	if flags.NArg() > 0 {
		fmt.Fprintf(stderr, "%s: unexpected argument %q\n", tool, flags.Arg(0))
		return exitUsage
	}
	if *preset == "" {
		*preset = getenv("PRESET")
	}
	if *preset == "" {
		*preset = testmap.DefaultPreset
	}
	return runlog.Invocation(tool, args, getenv, stderr, func(log *slog.Logger) int {
		r := &runner{ctx: ctx, env: env, getenv: getenv, stdout: stdout, stderr: stderr, log: log, preset: *preset}
		return r.do(*repo, *mapFile, *base, *dry)
	})
}

type runner struct {
	ctx            context.Context
	env            []string
	getenv         func(string) string
	stdout, stderr io.Writer
	log            *slog.Logger
	preset         string
	repo           string
}

func (r *runner) say(format string, a ...any) { fmt.Fprintf(r.stdout, tool+": "+format+"\n", a...) }

func (r *runner) fail(code int, format string, a ...any) int {
	msg := fmt.Sprintf(format, a...)
	fmt.Fprintf(r.stderr, "%s: FAIL %s\n", tool, msg)
	r.log.Error("refused", "reason", msg, "exit", code)
	return code
}

func (r *runner) git(args ...string) (string, error) {
	cmd := exec.Command("git", args...)
	cmd.Dir, cmd.Env = r.repo, r.env
	var errb bytes.Buffer
	cmd.Stderr = &errb
	out, err := cmd.Output()
	if err != nil {
		return "", fmt.Errorf("git %s: %v: %s", strings.Join(args, " "), err, strings.TrimSpace(errb.String()))
	}
	return string(out), nil
}

// change is one changed path and what owns it.
type change struct {
	path, component, entry string
	tests                  []string
	none                   string
	fromBase               bool
}

func (r *runner) do(repo, mapFile, base string, dry bool) int {
	r.repo = repo
	if r.repo == "" {
		top, err := r.git("rev-parse", "--show-toplevel")
		if err != nil {
			return r.fail(exitUsage, "%v", err)
		}
		r.repo = strings.TrimSpace(top)
	}
	if mapFile == "" {
		mapFile = filepath.Join(r.repo, testmap.DefaultPath)
	}
	m, err := testmap.Load(mapFile)
	if err != nil {
		return r.fail(exitUsage, "%v", err)
	}
	mbOut, err := r.git("merge-base", base, "HEAD")
	if err != nil {
		return r.fail(exitUsage, "%v (is %s fetched? git fetch origin main)", err, base)
	}
	mb := strings.TrimSpace(mbOut)
	diff, err := r.git("diff", "--name-only", "-z", mb)
	if err != nil {
		return r.fail(exitUsage, "%v", err)
	}
	var paths []string
	for _, p := range strings.Split(diff, "\x00") {
		if p != "" {
			paths = append(paths, p)
		}
	}
	r.say("base %s (git merge-base %s HEAD); %d changed path(s)", mb, base, len(paths))
	if len(paths) == 0 {
		r.log.Info("plan", "base", base, "merge_base", mb, "paths", paths, "tests", []string{})
		r.say("no path differs from the merge-base: nothing to run")
		return 0
	}
	var baseMap *testmap.Map
	var baseMapErr error
	loadBase := func() (*testmap.Map, error) {
		if baseMap == nil && baseMapErr == nil {
			var data string
			if data, baseMapErr = r.git("show", mb+":"+filepath.ToSlash(relTo(r.repo, mapFile))); baseMapErr == nil {
				baseMap, baseMapErr = testmap.Parse([]byte(data))
			}
		}
		return baseMap, baseMapErr
	}
	var changes []change
	var uncovered []string
	for _, p := range paths {
		c, entry, ok := m.Owner(p)
		fromBase := false
		if !ok {
			if _, statErr := os.Stat(filepath.Join(r.repo, p)); errors.Is(statErr, fs.ErrNotExist) {
				if bm, err := loadBase(); err == nil {
					c, entry, ok = bm.Owner(p)
					fromBase = ok
				}
			}
		}
		if !ok {
			uncovered = append(uncovered, p)
			continue
		}
		changes = append(changes, change{path: p, component: c.Name, entry: entry, tests: c.Tests, none: c.None, fromBase: fromBase})
	}
	if len(uncovered) > 0 {
		for _, p := range uncovered {
			fmt.Fprintf(r.stderr, "%s: FAIL %s is not covered by %s: add it to the component whose tests cover it\n", tool, p, relTo(r.repo, mapFile))
		}
		r.log.Error("uncovered", "merge_base", mb, "paths", uncovered, "exit", exitUncovered)
		fmt.Fprintf(r.stderr, "%s: FAIL %d changed path(s) not in the map; nothing was run\n", tool, len(uncovered))
		return exitUncovered
	}
	var all []string
	var components []string
	for _, c := range changes {
		where := "component " + c.component + " (" + c.entry + ")"
		if c.fromBase {
			where += ", deleted: from the map at the merge-base"
		}
		if len(c.tests) == 0 {
			r.say("%s -> %s: no tests: %s", c.path, where, c.none)
		} else {
			r.say("%s -> %s: %s", c.path, where, strings.Join(c.tests, " "))
		}
		components = append(components, c.component)
		all = append(all, c.tests...)
	}
	ids := m.Order(all)
	plan := make([][]string, len(ids))
	for i, id := range ids {
		if plan[i], err = m.Command(id, r.preset); err != nil {
			return r.fail(exitUsage, "%v", err)
		}
	}
	r.log.Info("plan", "base", base, "merge_base", mb, "paths", paths, "components", components, "tests", ids, "preset", r.preset)
	if len(ids) == 0 {
		r.say("no tests to run: no changed path's component has any")
		return 0
	}
	r.say("%d test(s), in this order: %s", len(ids), strings.Join(ids, " "))
	if dry {
		r.say("-n: nothing run")
		return 0
	}
	return r.runAll(ids, plan)
}

func (r *runner) runAll(ids []string, plan [][]string) int {
	childEnv := r.childEnv()
	var failed []string
	for i, id := range ids {
		if r.ctx.Err() != nil {
			r.log.Warn("interrupted", "before", id, "exit", exitInterrupted)
			fmt.Fprintf(r.stderr, "%s: interrupted before %s; %d test(s) not run\n", tool, id, len(ids)-i)
			return exitInterrupted
		}
		argv := plan[i]
		r.say("run %s: %s", id, strings.Join(argv, " "))
		started := time.Now()
		code := r.start(argv, childEnv)
		ms := time.Since(started).Milliseconds()
		r.log.Info("test_end", "test", id, "argv", argv, "exit", code, "ms", ms)
		r.say("%s exit %d (%.1f s)", id, code, time.Since(started).Seconds())
		if code != 0 {
			failed = append(failed, fmt.Sprintf("%s (exit %d)", id, code))
		}
	}
	if len(failed) > 0 {
		r.say("%d test(s): %d passed, %d failed: %s", len(ids), len(ids)-len(failed), len(failed), strings.Join(failed, ", "))
		return exitFailed
	}
	r.say("%d test(s): all passed", len(ids))
	return 0
}

// childEnv is the environment every test starts with: this one, without the display variables, with PRESET.
func (r *runner) childEnv() []string {
	var out []string
	for _, kv := range r.env {
		key, _, _ := strings.Cut(kv, "=")
		hidden := key == "PRESET"
		for _, h := range hiddenFromTests {
			hidden = hidden || key == h
		}
		if !hidden {
			out = append(out, kv)
		}
	}
	return append(out, "PRESET="+r.preset)
}

// start runs argv in the checkout and returns its exit code; a program that cannot be started is 127, as a shell has it.
func (r *runner) start(argv []string, env []string) int {
	prog, err := findProgram(argv[0], lookup(env)("PATH"))
	if err != nil {
		fmt.Fprintf(r.stderr, "%s: cannot start %s: %v\n", tool, argv[0], err)
		return exitNotStarted
	}
	cmd := exec.Command(prog, argv[1:]...)
	cmd.Args[0] = argv[0]
	cmd.Dir, cmd.Env = r.repo, env
	cmd.Stdout, cmd.Stderr = r.stdout, r.stderr
	err = cmd.Run()
	var exitErr *exec.ExitError
	switch {
	case err == nil:
		return 0
	case errors.As(err, &exitErr):
		if code := exitErr.ExitCode(); code >= 0 {
			return code
		}
		fmt.Fprintf(r.stderr, "%s: %s ended by a signal: %v\n", tool, argv[0], err)
		return exitFailed
	default:
		fmt.Fprintf(r.stderr, "%s: cannot start %s: %v\n", tool, argv[0], err)
		return exitNotStarted
	}
}

// findProgram resolves name on path (the tests' PATH, not this process's), as a shell does.
func findProgram(name, path string) (string, error) {
	if strings.Contains(name, "/") {
		return name, nil
	}
	for _, dir := range filepath.SplitList(path) {
		if dir == "" {
			continue
		}
		cand := filepath.Join(dir, name)
		if st, err := os.Stat(cand); err == nil && !st.IsDir() && st.Mode()&execBits != 0 {
			return cand, nil
		}
	}
	return "", fmt.Errorf("%s is not on PATH", name)
}

func relTo(root, file string) string {
	if rel, err := filepath.Rel(root, file); err == nil && !strings.HasPrefix(rel, "..") {
		return rel
	}
	return file
}
