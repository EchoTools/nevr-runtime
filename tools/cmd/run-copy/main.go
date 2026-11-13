// Command run-copy refuses a game copy that a run must not start from ("The game install" in AGENTS.md).
//
//	run-copy -copy <dir> -src <install root> [-work-root <dir>]
//
// <dir> is the directory that holds echovr/ (the copy). It fails, naming each path, when
//   - echovr/bin/win10/echovr.exe is missing;
//   - a sign-in file (.credentials.json*) exists anywhere under echovr/;
//   - echovr/_local/config.yaml or config.json exists, or the nevr root's config.yaml in the Wine prefix
//     (echovr/.wineprefix/drive_c/users/<user>/AppData/Local/nevr/echovr/config.yaml) exists, or a plugins directory (bin/win10/plugins, or the nevr root's
//     in the Wine prefix) holds a file;
//   - a symbolic link under the copy resolves into -src (the install the copy was made from), so a run could write
//     through it; -src is required, because without it that check cannot run;
//   - a path under echovr/ cannot be read (a directory it cannot list, a link it cannot resolve for a reason other than
//     a missing target), because a sign-in file could be in it;
//   - the copy is not under the work root (-work-root, else $NEVR_WORK_ROOT, else ~/src/nevr-runtime-work).
//
// Links that leave the copy for other places (the Wine prefix's device and home links) are listed on standard output
// as "leaves-copy: <link> -> <target>" for the run's record and are not failures, as the AGENTS.md text says.
//
// Exit codes: 0 the copy is usable; 1 a refusal (each printed); 2 a bad flag, or an unreadable -copy or -src (an unreadable path inside the copy is a refusal, exit 1).
package main

import (
	"errors"
	"flag"
	"fmt"
	"io"
	"io/fs"
	"log/slog"
	"os"
	"path/filepath"
	"strings"

	"github.com/EchoTools/nevr-runtime/tools/internal/runlog"
)

// exitUsage is the exit code of a bad flag, a missing argument or input that cannot be read.
const exitUsage = 2

func main() { os.Exit(run(os.Args[1:], os.Getenv, os.Stdout, os.Stderr)) }

// run is execute with the tool's run log: one "invoked" and one "finished" record in $XDG_STATE_HOME/nevr-runtime/run-copy.jsonl
// (internal/runlog). A log that cannot be written is exit 74, never a run without its record.
func run(args []string, getenv func(string) string, stdout, stderr io.Writer) int {
	return runlog.Invocation("run-copy", args, getenv, stderr, func(*slog.Logger) int { return execute(args, stdout, stderr) })
}

func execute(args []string, stdout, stderr io.Writer) int {
	fl := flag.NewFlagSet("run-copy", flag.ContinueOnError)
	fl.SetOutput(stderr)
	copyDir := fl.String("copy", "", "directory that holds echovr/")
	src := fl.String("src", "", "the install the copy was made from")
	workRoot := fl.String("work-root", "", "work root the copy must sit under")
	if err := fl.Parse(args); err != nil {
		return exitUsage
	}
	if *copyDir == "" {
		fmt.Fprintln(stderr, "run-copy: -copy is required")
		return exitUsage
	}
	root, err := filepath.Abs(*copyDir)
	if err != nil {
		fmt.Fprintf(stderr, "run-copy: %v\n", err)
		return exitUsage
	}
	root, err = filepath.EvalSymlinks(root)
	if err != nil {
		fmt.Fprintf(stderr, "run-copy: %v\n", err)
		return exitUsage
	}
	if *src == "" {
		fmt.Fprintln(stderr, "run-copy: -src is required: without the install the copy was made from, a link into the install cannot be recognised")
		return exitUsage
	}
	srcAbs, err := filepath.Abs(*src)
	if err != nil {
		fmt.Fprintf(stderr, "run-copy: -src: %v\n", err)
		return exitUsage
	}
	srcReal, err := filepath.EvalSymlinks(srcAbs)
	if err != nil {
		fmt.Fprintf(stderr, "run-copy: -src: %v\n", err)
		return exitUsage
	}
	wr := *workRoot
	if wr == "" {
		wr = os.Getenv("NEVR_WORK_ROOT")
	}
	if wr == "" {
		home, _ := os.UserHomeDir()
		wr = filepath.Join(home, "src", "nevr-runtime-work")
	}
	if r, err := filepath.EvalSymlinks(wr); err == nil {
		wr = r
	}

	var bad []string
	if !under(root, wr) {
		bad = append(bad, fmt.Sprintf("copy %s is not under the work root %s", root, wr))
	}
	echovr := filepath.Join(root, "echovr")
	if _, err := os.Stat(filepath.Join(echovr, "bin", "win10", "echovr.exe")); err != nil {
		bad = append(bad, "missing echovr/bin/win10/echovr.exe")
	}
	for _, f := range []string{"config.yaml", "config.json"} {
		if _, err := os.Lstat(filepath.Join(echovr, "_local", f)); err == nil {
			bad = append(bad, "echovr/_local/"+f+" exists")
		}
	}
	nevrRootConfigs, _ := filepath.Glob(filepath.Join(echovr, ".wineprefix", "drive_c", "users", "*", "AppData", "Local", "nevr", "echovr", "config.yaml"))
	for _, c := range nevrRootConfigs {
		rel, _ := filepath.Rel(root, c)
		bad = append(bad, "the nevr root's config.yaml exists: "+rel)
	}
	var leaves []string
	walkErr := filepath.WalkDir(echovr, func(p string, d fs.DirEntry, err error) error {
		rel, _ := filepath.Rel(root, p)
		if err != nil {
			// Fail closed: a path that cannot be read could hold a sign-in file. The walk goes on, so the other findings
			// are still listed.
			bad = append(bad, fmt.Sprintf("unreadable %s: %v", rel, err))
			return nil
		}
		if !d.IsDir() && strings.HasPrefix(d.Name(), ".credentials.json") {
			bad = append(bad, "sign-in file "+rel)
		}
		if d.Type()&fs.ModeSymlink != 0 {
			t, err := filepath.EvalSymlinks(p)
			if errors.Is(err, fs.ErrNotExist) {
				return nil // dangling: leaves nothing to write through
			}
			if err != nil {
				bad = append(bad, fmt.Sprintf("unreadable link %s: %v", rel, err))
				return nil
			}
			switch {
			case under(t, srcReal):
				bad = append(bad, fmt.Sprintf("link %s resolves into the install: %s", rel, t))
			case !under(t, root):
				leaves = append(leaves, fmt.Sprintf("leaves-copy: %s -> %s", rel, t))
			}
			return nil
		}
		if d.IsDir() && d.Name() == "plugins" {
			if ents, err := os.ReadDir(p); err == nil && len(ents) > 0 {
				bad = append(bad, "plugins directory is not empty: "+rel)
			}
		}
		return nil
	})
	if walkErr != nil {
		fmt.Fprintf(stderr, "run-copy: %v\n", walkErr)
		return exitUsage
	}
	for _, l := range leaves {
		fmt.Fprintln(stdout, l)
	}
	for _, b := range bad {
		fmt.Fprintln(stdout, "refused:", b)
	}
	if len(bad) > 0 {
		fmt.Fprintf(stderr, "run-copy: %d refusal(s); the copy is not safe to run from\n", len(bad))
		return 1
	}
	fmt.Fprintf(stdout, "run-copy: OK (%d link(s) leave the copy)\n", len(leaves))
	return 0
}

func under(p, dir string) bool {
	rel, err := filepath.Rel(dir, p)
	return err == nil && rel != ".." && !strings.HasPrefix(rel, ".."+string(filepath.Separator))
}
