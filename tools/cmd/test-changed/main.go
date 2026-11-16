// Command test-changed runs the tests that cover a branch's change, and nothing else (AGENTS.md, "Builds"):
//
//	test-changed [-repo dir] [-map file] [-base origin/main] [-preset name] [-n] [-heavy]
//
// The change is what `git diff --name-only --no-renames -z $(git merge-base origin/main HEAD)` lists: the branch's commits
// and its uncommitted edits to tracked files. --no-renames makes a file moved between two components show as a deletion
// and an addition, so both components' tests run. Each changed path is looked up in tools/test_map.json (schema 2,
// internal/testmap): the path's component, its own tests and the tests of every scanner whose glob matches it. The run is
// the union of those tests in the order testmap.Order gives, one call at a time, each printed with its exit code.
//
// Every gtest id of the run is one call of `just test-auth-unit <binary>...`, which appends one "<binary>\t<exit code>"
// line per binary it ran to the file NEVR_GTEST_RESULTS names. That file is a fresh one under
// <work root>/test-runs/test-changed/ and is kept as the run's evidence. A binary with no line gets the batch's own exit
// code (1 when the batch exited 0), and its line says the batch exited before running it.
//
// A path that was deleted is looked up in the map at the merge-base when the map no longer has it. A changed path that no
// entry of the map covers stops the run before anything starts, naming the path; the fix is `just test-map`, never a
// hand edit of the map.
//
// Every test runs from the checkout's top directory without DISPLAY and WAYLAND_DISPLAY (a GTest under Wine must not
// reach the desktop) and with PRESET set to -preset (default $PRESET, else mingw-release), so the build and the tests
// read one build tree. -n prints the plan and runs nothing.
//
// A land runs only the fast unit tests: an id testmap.Classify calls heavy (the Android and Quest suites, and any id whose
// fastest recorded run is over testmap.HeavyBound) is left out of the run, and each one is printed as "skipped heavy"
// with its fastest recorded time and the reason, and recorded in the plan. -heavy runs every selected id, heavy or not,
// for the runs that call for them. The heavy set is internal/testmap's heavy.go, the one place that says it.
//
// Exit codes: 0 every test passed, or the change needs none; 1 a test failed; 2 a flag, git, the map or the results file is
// wrong; 3 a changed path is not in the map; 127 a program a test starts is not on PATH; 130 interrupted between tests;
// 74 the run log could not be written.
//
// Each run is recorded in $XDG_STATE_HOME/nevr-runtime/test-changed.jsonl (internal/runlog): invoked, plan (the merge-base,
// the changed paths, their components and the tests), results (the results file of a gtest batch), one test_end per test
// (its id, argv, exit and milliseconds; a gtest id also its note when the batch wrote no line for it), finished.
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
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/EchoTools/nevr-runtime/tools/internal/runlog"
	"github.com/EchoTools/nevr-runtime/tools/internal/runroot"
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

// dirPerm and filePerm are the modes of the results directory and file.
const (
	dirPerm  = 0o755
	filePerm = 0o644
)

// tool is the program's name: its log file and the prefix of every line it prints.
const tool = "test-changed"

// testRunsDir is the directory under the work root where the tools keep the results of test runs.
const testRunsDir = "test-runs"

// resultsEnv is the variable test-auth-unit appends its per-binary results to (justfile).
const resultsEnv = "NEVR_GTEST_RESULTS"

// runStamp is the UTC stamp a results file is named by, the same form the run log's run id uses.
const runStamp = "20060102T150405.000000Z"

// noResultFormat is the note of a gtest id whose binary the batch wrote no result for; the code is the batch's exit code.
const noResultFormat = "(no result: the batch exited %d before running it)"

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
	heavy := flags.Bool("heavy", false, "run the heavy tests too (a land runs only the fast unit tests)")
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
		r := &runner{ctx: ctx, env: env, getenv: getenv, stdout: stdout, stderr: stderr, log: log, preset: *preset, heavy: *heavy}
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
	heavy          bool
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

// change is one changed path and what the map says about it.
type change struct {
	path, component, text string
	tests                 []string
	fromBase              bool
}

// step is one call of the run: one test id, or the batch of every gtest id, which one call of test-auth-unit runs.
type step struct {
	ids   []string
	argv  []string
	batch bool
}

// outcome is the exit of one test id, and the note the run gives it when it has no result of its own.
type outcome struct {
	id   string
	code int
	note string
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
	diff, err := r.git("diff", "--name-only", "--no-renames", "-z", mb)
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
		lk, ok := m.Lookup(p)
		fromBase := false
		if !ok {
			if _, statErr := os.Stat(filepath.Join(r.repo, p)); errors.Is(statErr, fs.ErrNotExist) {
				if bm, err := loadBase(); err == nil {
					lk, ok = bm.Lookup(p)
					fromBase = ok
				}
			}
		}
		if !ok {
			uncovered = append(uncovered, p)
			continue
		}
		changes = append(changes, change{path: p, component: lk.Component.Name, text: describe(lk), tests: lk.Tests, fromBase: fromBase})
	}
	if len(uncovered) > 0 {
		for _, p := range uncovered {
			fmt.Fprintf(r.stderr, "%s: FAIL %s is not in %s: run just test-map and commit the map it writes\n", tool, p, relTo(r.repo, mapFile))
		}
		r.log.Error("uncovered", "merge_base", mb, "paths", uncovered, "exit", exitUncovered)
		fmt.Fprintf(r.stderr, "%s: FAIL %d changed path(s) not in the map; nothing was run\n", tool, len(uncovered))
		return exitUncovered
	}
	var all []string
	var components []string
	for _, c := range changes {
		line := fmt.Sprintf("%s -> %s : %s", c.path, c.component, c.text)
		if c.fromBase {
			line += " (deleted: looked up in the map at the merge-base)"
		}
		r.say("%s", line)
		components = append(components, c.component)
		all = append(all, c.tests...)
	}
	selected := m.Order(all)
	var skipped []testmap.HeavyID
	if !r.heavy {
		var unit []string
		if unit, skipped, err = testmap.SplitHeavy(selected); err != nil {
			return r.fail(exitUsage, "%v", err)
		}
		selected = unit
	}
	for _, h := range skipped {
		r.say("skipped heavy %s (%s): %s; run test-changed -heavy for it", h.ID, measuredText(h.Millis), h.Reason)
		r.log.Info("skipped_heavy", "test", h.ID, "rule", h.Rule, "fastest_ms", h.Millis, "reason", h.Reason)
	}
	steps, err := r.plan(m, selected)
	if err != nil {
		return r.fail(exitUsage, "%v", err)
	}
	var ids []string
	for _, st := range steps {
		ids = append(ids, st.ids...)
	}
	r.log.Info("plan", "base", base, "merge_base", mb, "paths", paths, "components", components, "tests", ids, "preset", r.preset)
	if len(ids) == 0 {
		if len(skipped) > 0 {
			r.say("no tests to run: every test of the change is heavy (listed above)")
		} else {
			r.say("no tests to run: no changed path's component has any")
		}
		return 0
	}
	r.say("%d test(s), in this order: %s", len(ids), strings.Join(ids, " "))
	if dry {
		r.say("-n: nothing run")
		return 0
	}
	results := ""
	for _, st := range steps {
		if st.batch {
			if results, err = r.newResultsFile(); err != nil {
				return r.fail(exitUsage, "%v", err)
			}
			r.log.Info("results", "file", results)
			break
		}
	}
	return r.runSteps(steps, results)
}

// measuredText is a heavy id's fastest recorded run for the "skipped heavy" line.
func measuredText(ms int64) string {
	if ms <= 0 {
		return "no run time kept"
	}
	return fmt.Sprintf("fastest run %.0f s", float64(ms)/millisPerSecond)
}

// millisPerSecond converts the run log's milliseconds to the seconds the line prints.
const millisPerSecond = 1000

// describe is what the map says about one path, for the plan's line.
func describe(lk testmap.Lookup) string {
	own := "no tests of its own: " + lk.None
	if len(lk.Own) > 0 {
		own = strings.Join(lk.Own, " ")
	}
	if len(lk.Scanners) == 0 {
		return own
	}
	mine := map[string]bool{}
	for _, id := range lk.Own {
		mine[id] = true
	}
	var extra []string
	for _, id := range lk.Tests {
		if !mine[id] {
			extra = append(extra, id)
		}
	}
	scanned := strings.Join(extra, " ")
	if len(extra) == 0 {
		scanned = "no new tests"
	}
	return fmt.Sprintf("%s ; scanners %s: %s", own, strings.Join(lk.Scanners, ", "), scanned)
}

// plan turns the ordered ids into the calls of the run. The gtest ids become one step, in the place of the first of them,
// whose argv is the command of the first with every other id's binary appended.
func (r *runner) plan(m *testmap.Map, ids []string) ([]step, error) {
	var steps []step
	var batch step
	var prefix, names []string
	batchAt := -1
	for _, id := range ids {
		argv, err := m.Command(id, r.preset)
		if err != nil {
			return nil, err
		}
		if len(argv) == 0 {
			return nil, fmt.Errorf("test %s has no command", id)
		}
		kind, _, _ := strings.Cut(id, ":")
		if kind != testmap.KindGTest {
			steps = append(steps, step{ids: []string{id}, argv: argv})
			continue
		}
		p := argv[:len(argv)-1]
		if batchAt < 0 {
			batchAt = len(steps)
			steps = append(steps, step{})
			prefix = append([]string(nil), p...)
		} else if !sameStrings(p, prefix) {
			return nil, fmt.Errorf("gtest test %s runs as %v, not as the other gtest tests' %v", id, argv, prefix)
		}
		names = append(names, argv[len(argv)-1])
		batch.ids = append(batch.ids, id)
	}
	if batchAt >= 0 {
		batch.argv = append(append([]string(nil), prefix...), names...)
		batch.batch = true
		steps[batchAt] = batch
	}
	return steps, nil
}

// runSteps runs the steps in order, stopping between them on an interrupt, and summarises the run.
func (r *runner) runSteps(steps []step, results string) int {
	childEnv := r.childEnv()
	total := 0
	for _, st := range steps {
		total += len(st.ids)
	}
	var failed []string
	done := 0
	for _, st := range steps {
		if r.ctx.Err() != nil {
			r.log.Warn("interrupted", "before", st.ids[0], "exit", exitInterrupted)
			fmt.Fprintf(r.stderr, "%s: interrupted before %s; %d test(s) not run\n", tool, st.ids[0], total-done)
			return exitInterrupted
		}
		var outs []outcome
		if st.batch {
			outs = r.runBatch(st, results, childEnv)
		} else {
			outs = []outcome{r.runOne(st, childEnv)}
		}
		for _, o := range outs {
			if o.code != 0 {
				failed = append(failed, fmt.Sprintf("%s (exit %d)", o.id, o.code))
			}
		}
		done += len(st.ids)
	}
	if len(failed) > 0 {
		r.say("%d test(s): %d passed, %d failed: %s", total, total-len(failed), len(failed), strings.Join(failed, ", "))
		return exitFailed
	}
	r.say("%d test(s): all passed", total)
	return 0
}

// runOne runs one test id as its own call.
func (r *runner) runOne(st step, env []string) outcome {
	id := st.ids[0]
	r.say("run %s: %s", id, strings.Join(st.argv, " "))
	started := time.Now()
	code := r.start(st.argv, env)
	ms := time.Since(started).Milliseconds()
	r.log.Info("test_end", "test", id, "argv", st.argv, "exit", code, "ms", ms)
	r.say("%s exit %d (%.1f s)", id, code, time.Since(started).Seconds())
	return outcome{id: id, code: code}
}

// runBatch runs the gtest batch as one call, with NEVR_GTEST_RESULTS naming the results file, then reads each id's own
// result from it. An id with no line gets the batch's exit code, or 1 when the batch exited 0.
func (r *runner) runBatch(st step, results string, env []string) []outcome {
	r.say("run %s: %s", strings.Join(st.ids, " "), strings.Join(st.argv, " "))
	started := time.Now()
	code := r.start(st.argv, withEnv(env, resultsEnv+"="+results))
	ms := time.Since(started).Milliseconds()
	got, err := readResults(results)
	if err != nil {
		r.log.Error("results_unreadable", "file", results, "error", err.Error())
		fmt.Fprintf(r.stderr, "%s: FAIL %v\n", tool, err)
	}
	outs := make([]outcome, 0, len(st.ids))
	for _, id := range st.ids {
		_, name, _ := strings.Cut(id, ":")
		o := outcome{id: id}
		if c, ok := got[name]; ok {
			o.code = c
		} else {
			o.code = code
			if o.code == 0 {
				o.code = exitFailed
			}
			o.note = fmt.Sprintf(noResultFormat, code)
		}
		args := []any{"test", id, "argv", st.argv, "exit", o.code, "ms", ms}
		line := fmt.Sprintf("%s exit %d", id, o.code)
		if o.note != "" {
			args = append(args, "note", o.note)
			line += " " + o.note
		}
		r.log.Info("test_end", args...)
		r.say("%s", line)
		outs = append(outs, o)
	}
	return outs
}

// readResults reads the "<binary>\t<exit code>" lines a test-auth-unit call appended. A line that is not one is an error,
// and the lines before it are returned with it.
func readResults(file string) (map[string]int, error) {
	data, err := os.ReadFile(file)
	if err != nil {
		return nil, err
	}
	got := map[string]int{}
	for n, row := range strings.Split(string(data), "\n") {
		if row == "" {
			continue
		}
		name, code, ok := strings.Cut(row, "\t")
		c, cerr := strconv.Atoi(code)
		if !ok || cerr != nil || name == "" {
			return got, fmt.Errorf("results file %s line %d is not <binary>, a tab and an exit code: %q", file, n+1, row)
		}
		got[name] = c
	}
	return got, nil
}

// newResultsFile makes the fresh, empty results file of a gtest batch, under the work root, and returns its path.
func (r *runner) newResultsFile() (string, error) {
	workRoot, err := runroot.WorkRoot(r.getenv)
	if err != nil {
		return "", fmt.Errorf("work root: %w", err)
	}
	dir := filepath.Join(workRoot, testRunsDir, tool)
	if err := os.MkdirAll(dir, dirPerm); err != nil {
		return "", fmt.Errorf("results directory: %w", err)
	}
	name := fmt.Sprintf("%s-%d.tsv", time.Now().UTC().Format(runStamp), os.Getpid())
	path := filepath.Join(dir, name)
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, filePerm)
	if err != nil {
		return "", fmt.Errorf("results file: %w", err)
	}
	return path, f.Close()
}

// childEnv is the environment every test starts with: this one, without the display variables, without the results
// variable (only the gtest batch gets one), with PRESET.
func (r *runner) childEnv() []string {
	var out []string
	for _, kv := range r.env {
		key, _, _ := strings.Cut(kv, "=")
		hidden := key == "PRESET" || key == resultsEnv
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

// withEnv is env with kv added at the end (a later KEY=VALUE of the same key wins in a child process).
func withEnv(env []string, kv string) []string {
	return append(append([]string(nil), env...), kv)
}

// sameStrings reports whether a and b hold the same strings in the same order.
func sameStrings(a, b []string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func relTo(root, file string) string {
	if rel, err := filepath.Rel(root, file); err == nil && !strings.HasPrefix(rel, "..") {
		return rel
	}
	return file
}
