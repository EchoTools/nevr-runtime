// Command test-map writes tools/test_map.json, the map `just test-changed` reads (tools/cmd/test-changed). For every
// tracked file it records the test ids whose run compiles, reads or names it, and for a file no test covers, the reason:
//
//	test-map [-repo dir] [-trees dir] [-out file] [-check]
//
// -repo is the checkout whose tracked files the map covers (default: the git top level of the working directory). -trees
// is the directories the build trees are looked for in, separated by ':', the first that holds a tree giving it
// (default: the build checkout's build/ ($NEVR_BUILD_CHECKOUT, else <work root>/build-checkout/nevr-runtime), then the
// main checkout's build/, then -repo's build/). A tree is <checkout>/build/<name>,
// and its ninja paths name files of that checkout, so a tree of another checkout maps onto the files of -repo by their
// repository paths. -out is the map (default:
// <repo>/tools/test_map.json). -check writes nothing and exits 1 when the file on disk differs from what the generator
// writes, naming the first differing component.
//
// Exit codes: 0 written, or -check found the same; 1 the map disagrees with the tree (a tracked file has no test and no
// reason, or -check found a difference); 2 the map cannot be made (bad flags, a build tree that is missing, a command
// that failed). Every run is one record in $XDG_STATE_HOME/nevr-runtime/test-map.jsonl (internal/runlog).
package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"io/fs"
	"log/slog"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"

	"github.com/EchoTools/nevr-runtime/tools/internal/runlog"
	"github.com/EchoTools/nevr-runtime/tools/internal/testmap"
)

const tool = "test-map"

// The exit codes of the generator. 0 is success.
const (
	exitUncovered  = 1 // the map disagrees with the tree: a file has no test and no reason, or -check found a difference
	exitCannotMake = 2 // the map cannot be made: bad flags, a build tree is missing, a command failed
)

// The names the default build trees are found under.
const (
	envBuildCheckout  = "NEVR_BUILD_CHECKOUT"
	treesSubdir       = "build"
	buildCheckoutDir  = "build-checkout"
	buildCheckoutRepo = "nevr-runtime"
)

// fileMode and dirMode are the permissions of the map and of the directories made for it.
const (
	fileMode = 0o644
	dirMode  = 0o755
)

func main() {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	os.Exit(run(ctx, os.Args[1:], os.Environ(), os.Stdout, os.Stderr))
}

// lookup returns the value of key in env, the last one given, as os.Getenv would have it.
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
	repo := flags.String("repo", "", "the checkout whose tracked files the map covers (default: the git top level of the working directory)")
	trees := flags.String("trees", "", "the directories the build trees are looked for in, ':'-separated, first match wins (default: the build checkout's build/, the main checkout's build/, -repo's build/)")
	out := flags.String("out", "", "the map to write (default: <repo>/"+testmap.DefaultPath+")")
	check := flags.Bool("check", false, "write nothing; exit 1 when the map on disk differs from what the generator writes")
	if err := flags.Parse(args); err != nil {
		return exitCannotMake
	}
	if flags.NArg() > 0 {
		fmt.Fprintf(stderr, "%s: unexpected argument %q\n", tool, flags.Arg(0))
		return exitCannotMake
	}
	return runlog.Invocation(tool, args, getenv, stderr, func(log *slog.Logger) int {
		g := &generator{ctx: ctx, env: env, getenv: getenv, stdout: stdout, stderr: stderr, log: log,
			repo: *repo, trees: *trees, out: *out, check: *check}
		return g.do()
	})
}

// failure is an error with the exit code it ends the run with.
type failure struct {
	code int
	msg  string
}

func (f failure) Error() string { return f.msg }

type generator struct {
	ctx    context.Context
	env    []string
	getenv func(string) string
	stdout io.Writer
	stderr io.Writer
	log    *slog.Logger
	repo   string
	trees  string
	out    string
	check  bool
}

func (g *generator) say(format string, a ...any) { fmt.Fprintf(g.stdout, tool+": "+format+"\n", a...) }

// fail prints err, logs it and returns its exit code (exitCannotMake for an error that is not a failure).
func (g *generator) fail(err error) int {
	code := exitCannotMake
	var f failure
	if errors.As(err, &f) {
		code = f.code
	}
	fmt.Fprintf(g.stderr, "%s: FAIL %v\n", tool, err)
	g.log.Error("refused", "reason", err.Error(), "exit", code)
	return code
}

func (g *generator) do() int {
	tc := toolchain{ctx: g.ctx, env: g.env}
	repo := g.repo
	if repo == "" {
		top, err := tc.output("", "git", "rev-parse", "--show-toplevel")
		if err != nil {
			return g.fail(failure{exitCannotMake, err.Error()})
		}
		repo = strings.TrimSpace(top)
	}
	repo, err := filepath.Abs(repo)
	if err != nil {
		return g.fail(err)
	}
	treesArg := g.trees
	if treesArg == "" {
		treesArg = g.defaultTrees(tc, repo)
	}
	var trees []string
	for _, t := range filepath.SplitList(treesArg) {
		if t == "" {
			continue
		}
		abs, err := filepath.Abs(t)
		if err != nil {
			return g.fail(err)
		}
		trees = append(trees, abs)
	}
	out := g.out
	if out == "" {
		out = filepath.Join(repo, filepath.FromSlash(testmap.DefaultPath))
	}
	if out, err = filepath.Abs(out); err != nil {
		return g.fail(err)
	}
	m, err := newMapper(tc, g.log, repo, trees)
	if err != nil {
		return g.fail(err)
	}
	if err := m.cover(); err != nil {
		return g.fail(err)
	}
	mp, err := buildMap(m.tracked, m.cov)
	if err != nil {
		return g.fail(err)
	}
	var buf bytes.Buffer
	if err := testmap.Encode(&buf, mp); err != nil {
		return g.fail(err)
	}
	g.log.Info("map", "repo", repo, "trees", trees, "out", out, "tracked", len(m.tracked),
		"components", len(mp.Components), "gtest_from_sources", m.fallbacks)
	if g.check {
		return g.checkFile(out, buf.Bytes())
	}
	if err := writeFile(out, buf.Bytes()); err != nil {
		return g.fail(err)
	}
	g.say("wrote %s: %d components, %d tracked files", out, len(mp.Components), len(m.tracked))
	if len(m.fallbacks) > 0 {
		g.say("gtest executables the trees do not have, covered by their sources: %s", strings.Join(m.fallbacks, ", "))
	}
	return 0
}

// defaultTrees is the search path when -trees is not given: the build checkout's trees ($NEVR_BUILD_CHECKOUT, else the
// work root's build-checkout/nevr-runtime), then the main checkout's (the parent of git's common directory), then the
// repository's own. A tree missing from all three is the failure that names the recipe making it.
func (g *generator) defaultTrees(tc toolchain, repo string) string {
	checkout := g.getenv(envBuildCheckout)
	if checkout == "" {
		checkout = filepath.Join(runlog.WorkRoot(g.getenv), buildCheckoutDir, buildCheckoutRepo)
	}
	dirs := []string{filepath.Join(checkout, treesSubdir)}
	if common, err := tc.output(repo, "git", "rev-parse", "--path-format=absolute", "--git-common-dir"); err == nil {
		dirs = append(dirs, filepath.Join(filepath.Dir(strings.TrimSpace(common)), treesSubdir))
	}
	dirs = append(dirs, filepath.Join(repo, treesSubdir))
	var out []string
	seen := map[string]bool{}
	for _, d := range dirs {
		if !seen[d] {
			seen[d] = true
			out = append(out, d)
		}
	}
	return strings.Join(out, string(filepath.ListSeparator))
}

// checkFile is -check: it compares the map on disk with want and names the first component that differs.
func (g *generator) checkFile(out string, want []byte) int {
	have, err := os.ReadFile(out)
	if errors.Is(err, fs.ErrNotExist) {
		fmt.Fprintf(g.stderr, "%s: FAIL -check: %s does not exist; run just test-map\n", tool, out)
		g.log.Error("check", "out", out, "result", "missing", "exit", exitUncovered)
		return exitUncovered
	}
	if err != nil {
		return g.fail(err)
	}
	if bytes.Equal(have, want) {
		g.say("-check: %s is what the generator writes", out)
		g.log.Info("check", "out", out, "result", "same")
		return 0
	}
	where := firstDifference(string(have), string(want))
	fmt.Fprintf(g.stderr, "%s: FAIL -check: %s differs from the generator; %s\n", tool, out, where)
	g.log.Error("check", "out", out, "result", "differs", "where", where, "exit", exitUncovered)
	return exitUncovered
}

// firstDifference names the first line that differs between have and want: the component it belongs to when the line is
// one (Encode writes one component to a line), else its number.
func firstDifference(have, want string) string {
	hl, wl := strings.Split(have, "\n"), strings.Split(want, "\n")
	for i := 0; i < len(hl) || i < len(wl); i++ {
		var h, w string
		if i < len(hl) {
			h = hl[i]
		}
		if i < len(wl) {
			w = wl[i]
		}
		if h == w {
			continue
		}
		var c testmap.Component
		if json.Unmarshal([]byte(strings.TrimSuffix(strings.TrimSpace(w), ",")), &c) == nil && c.Name != "" {
			return "first differing component: " + c.Name
		}
		return fmt.Sprintf("first difference is outside the components, at line %d", i+1)
	}
	return "the files differ only in their final newline"
}

// writeFile writes data to path through a temporary file beside it, so a reader never sees half a map.
func writeFile(path string, data []byte) error {
	if err := os.MkdirAll(filepath.Dir(path), dirMode); err != nil {
		return err
	}
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, data, fileMode); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}
