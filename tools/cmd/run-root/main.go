// Command run-root makes and removes the run root of one test binary run (internal/runroot, ADR 0044).
//
//	run-root [-work-root <dir>] make <label>
//	run-root [-work-root <dir>] remove <path>
//
// make prints the new run root's path on standard output. remove deletes the run root at <path> after the checks of
// runroot.Remove, and prints nothing. -work-root names the work root; without it the work root is $NEVR_WORK_ROOT,
// else $HOME/src/nevr-runtime-work.
//
// Every invocation is recorded in $XDG_STATE_HOME/nevr-runtime/run-root.jsonl (internal/runlog): a deleter logs. A log
// that cannot be written is exit 74, never a run without its record.
//
// Exit codes: 0 done; 1 a refusal (the reason is printed to standard error, and nothing is removed); 2 a usage error
// (a bad flag, an unknown subcommand, a missing argument, or no work root to be had); 74 the run log could not be written
// (internal/runlog).
package main

import (
	"errors"
	"flag"
	"fmt"
	"io"
	"log/slog"
	"os"
	"time"

	"github.com/EchoTools/nevr-runtime/tools/internal/runlog"
	"github.com/EchoTools/nevr-runtime/tools/internal/runroot"
)

// Exit codes, as the package comment names them.
const (
	exitOK      = 0
	exitRefused = 1
	exitUsage   = 2
)

func main() { os.Exit(run(os.Args[1:], os.Getenv, os.Stdout, os.Stderr)) }

// run is execute with the tool's run log: one "invoked" and one "finished" record in run-root.jsonl (internal/runlog).
func run(args []string, getenv func(string) string, stdout, stderr io.Writer) int {
	return runlog.Invocation("run-root", args, getenv, stderr, func(log *slog.Logger) int {
		return execute(args, getenv, stdout, stderr, log)
	})
}

func execute(args []string, getenv func(string) string, stdout, stderr io.Writer, log *slog.Logger) int {
	fl := flag.NewFlagSet("run-root", flag.ContinueOnError)
	fl.SetOutput(stderr)
	workRootFlag := fl.String("work-root", "", "work root (default $NEVR_WORK_ROOT, else $HOME/src/nevr-runtime-work)")
	if err := fl.Parse(args); err != nil {
		log.Error("usage", "error", err.Error())
		return exitUsage
	}
	rest := fl.Args()
	if len(rest) < 2 {
		fmt.Fprintln(stderr, "run-root: usage: run-root [-work-root dir] make <label> | remove <path>")
		log.Error("usage", "args", rest)
		return exitUsage
	}
	wr := *workRootFlag
	if wr == "" {
		var err error
		if wr, err = runroot.WorkRoot(getenv); err != nil {
			fmt.Fprintf(stderr, "run-root: %v\n", err)
			log.Error("no work root", "error", err.Error())
			return exitUsage
		}
	}

	switch rest[0] {
	case "make":
		if len(rest) != 2 {
			fmt.Fprintln(stderr, "run-root: usage: run-root make <label>")
			log.Error("usage", "args", rest)
			return exitUsage
		}
		path, err := runroot.Make(wr, rest[1], time.Now().UTC(), os.Getpid())
		if err != nil {
			return refusal(stderr, log, "make", err)
		}
		fmt.Fprintln(stdout, path)
		log.Info("made", "path", path)
		return exitOK
	case "remove":
		if len(rest) != 2 {
			fmt.Fprintln(stderr, "run-root: usage: run-root remove <path>")
			log.Error("usage", "args", rest)
			return exitUsage
		}
		if err := runroot.Remove(wr, rest[1]); err != nil {
			return refusal(stderr, log, "remove", err)
		}
		log.Info("removed", "path", rest[1])
		return exitOK
	default:
		fmt.Fprintf(stderr, "run-root: usage: unknown subcommand %q\n", rest[0])
		log.Error("usage", "subcommand", rest[0])
		return exitUsage
	}
}

// refusal prints the reason on standard error and records it; it is exit 1.
func refusal(stderr io.Writer, log *slog.Logger, op string, err error) int {
	fmt.Fprintf(stderr, "run-root: %s %v\n", op, err)
	log.Error("refused", "op", op, "reason", err.Error(), "sentinel", sentinelName(err))
	return exitRefused
}

// sentinelName names the reason a refusal wraps, for the record.
func sentinelName(err error) string {
	for _, s := range []struct {
		name string
		err  error
	}{
		{"ErrEmptyWorkRoot", runroot.ErrEmptyWorkRoot}, {"ErrPath", runroot.ErrPath},
		{"ErrMissing", runroot.ErrMissing}, {"ErrLink", runroot.ErrLink}, {"ErrNotDir", runroot.ErrNotDir},
		{"ErrOutside", runroot.ErrOutside}, {"ErrName", runroot.ErrName},
	} {
		if errors.Is(err, s.err) {
			return s.name
		}
	}
	return ""
}
