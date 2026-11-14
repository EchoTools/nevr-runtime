// Command suite-run records each run of a whole test suite, and tells the owner when an agent started one (AGENTS.md,
// "Builds": no agent runs a whole suite):
//
//	suite-run -recipe <name> -- <command> [args...]
//
// It appends a suite_start record to <work root>/test-runs/suite-run.jsonl (the work root is $NEVR_WORK_ROOT, else
// ~/src/nevr-runtime-work), runs the command with NEVR_SUITE_RECIPE=<name> and NEVR_SUITE_PARENT=<this run's id> in its
// environment, appends a suite_end record with its exit code and duration, and exits with the command's code. The
// whole-suite recipes (verify, test-full, test-tools with no argument naming a file, test-auth-unit with no binary and
// no GTEST_FILTER) start themselves again through it unless NEVR_SUITE_RECIPE already names them.
//
// suite_start names who ran it: the time, this process and its parent, the working directory, the command, and the
// agent session the run came from: the nearest ancestor process whose program is an agent's (claude, codex, opencode),
// with that process's id and working directory. A run with no such ancestor but CLAUDECODE in its environment is an
// agent's whose session was not found; a run with neither is a person's shell.
//
// An agent's run that is not inside another suite run (NEVR_SUITE_PARENT is empty) and whose session directory is not
// one of $NEVR_SUITE_ALERT_EXEMPT (a colon-separated list) runs the alert hook: the program $NEVR_SUITE_ALERT names, as
//
//	$NEVR_SUITE_ALERT --from nevr-runtime "<session directory> ran <recipe>"
//
// The hook is empty unless the machine sets it, so a clone elsewhere alerts nobody. The record says what the alert
// did (sent, failed, exempt, nested, not_an_agent, no_hook); a hook that fails is a line on standard error too, and the
// suite still runs.
//
// Exit codes: the command's (128 + the signal when a signal ended it); 2 bad flags; 74 the record could not be written,
// in which case the command does not run.
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/EchoTools/nevr-runtime/tools/internal/runlog"
)

const (
	exitUsage      = 2
	exitNotStarted = 127
	exitSignalBase = 128
	// maxAncestors bounds the walk up the process tree; a real chain is a handful deep.
	maxAncestors = 64
	// statPPidField is the parent's pid among the fields of /proc/<pid>/stat that follow the program's name.
	statPPidField = 1
)

// alertTimeout is how long the alert hook may take before the run goes on without it.
const alertTimeout = 10 * time.Second

// tool is the program's name: the prefix of its lines.
const tool = "suite-run"

// logRel is the record file, relative to the work root. Its name is the tool's name: runlog takes a record's tool from it.
var logRel = filepath.Join("test-runs", tool+".jsonl")

// agentPrograms are the programs whose descendants are an agent's runs, by their name in /proc/<pid>/comm.
var agentPrograms = map[string]bool{"claude": true, "codex": true, "opencode": true}

// agentMarker is set in the environment of every command a Claude Code session runs.
const agentMarker = "CLAUDECODE"

func main() {
	os.Exit(run(context.Background(), os.Args[1:], os.Environ(), os.Stdin, os.Stdout, os.Stderr, system{proc: "/proc", ppid: os.Getppid()}))
}

// system is where the run looks at its own process tree: /proc and this process's parent, a fake tree in a test.
type system struct {
	proc string
	ppid int
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

// session is the agent session a run came from.
type session struct {
	agent   bool
	found   bool
	pid     int
	program string
	cwd     string
}

func (s system) read(pid int) (program string, ppid int, cwd string, err error) {
	dir := filepath.Join(s.proc, strconv.Itoa(pid))
	comm, err := os.ReadFile(filepath.Join(dir, "comm"))
	if err != nil {
		return "", 0, "", err
	}
	stat, err := os.ReadFile(filepath.Join(dir, "stat"))
	if err != nil {
		return "", 0, "", err
	}
	// The program's name is in parentheses and may hold spaces; the fields after the last ')' are fixed.
	end := strings.LastIndexByte(string(stat), ')')
	if end < 0 {
		return "", 0, "", fmt.Errorf("%s/stat has no program name", dir)
	}
	fields := strings.Fields(string(stat)[end+1:])
	if len(fields) <= statPPidField {
		return "", 0, "", fmt.Errorf("%s/stat is too short", dir)
	}
	ppid, err = strconv.Atoi(fields[statPPidField])
	if err != nil {
		return "", 0, "", fmt.Errorf("%s/stat: parent pid: %w", dir, err)
	}
	cwd, _ = os.Readlink(filepath.Join(dir, "cwd"))
	return strings.TrimSpace(string(comm)), ppid, cwd, nil
}

// findSession walks up from the parent to the nearest agent program.
func (s system) findSession(getenv func(string) string) session {
	pid := s.ppid
	for i := 0; i < maxAncestors && pid > 1; i++ {
		program, ppid, cwd, err := s.read(pid)
		if err != nil {
			break
		}
		if agentPrograms[program] {
			return session{agent: true, found: true, pid: pid, program: program, cwd: cwd}
		}
		pid = ppid
	}
	return session{agent: getenv(agentMarker) != ""}
}

func run(ctx context.Context, args []string, env []string, stdin io.Reader, stdout, stderr io.Writer, sys system) int {
	getenv := lookup(env)
	flags := flag.NewFlagSet(tool, flag.ContinueOnError)
	flags.SetOutput(stderr)
	recipe := flags.String("recipe", "", "the suite's recipe name")
	if err := flags.Parse(args); err != nil {
		return exitUsage
	}
	command := flags.Args()
	if *recipe == "" || len(command) == 0 {
		fmt.Fprintf(stderr, "%s: usage: %s -recipe <name> -- <command> [args...]\n", tool, tool)
		return exitUsage
	}
	logPath := filepath.Join(runlog.WorkRoot(getenv), logRel)
	l, err := runlog.OpenAt(logPath)
	if err != nil {
		fmt.Fprintf(stderr, "%s: FAIL the run of %s cannot be recorded, so it does not start: %v\n", tool, *recipe, err)
		return runlog.ExitLogFailure
	}
	cwd, _ := os.Getwd()
	sess := sys.findSession(getenv)
	parent := getenv("NEVR_SUITE_PARENT")
	where := sess.cwd
	if where == "" {
		where = cwd
	}
	alert, alertErr := sendAlert(ctx, env, getenv, sess, parent, *recipe, where)
	if alertErr != nil {
		fmt.Fprintf(stderr, "%s: the alert for this %s run failed: %v\n", tool, *recipe, alertErr)
	}
	started := time.Now()
	fields := []any{"recipe", *recipe, "command", command, "cwd", cwd, "ppid", sys.ppid, "user", getenv("USER"),
		"agent", sess.agent, "session_found", sess.found, "session_pid", sess.pid, "session_program", sess.program,
		"session_cwd", sess.cwd, "parent_run", parent, "alert", alert}
	if alertErr != nil {
		fields = append(fields, "alert_error", alertErr.Error())
	}
	l.Logger.Info("suite_start", fields...)
	if err := l.Err(); err != nil {
		l.Close()
		fmt.Fprintf(stderr, "%s: FAIL the run of %s cannot be recorded, so it does not start: %v\n", tool, *recipe, err)
		return runlog.ExitLogFailure
	}
	code := runChild(command, append(env, "NEVR_SUITE_RECIPE="+*recipe, "NEVR_SUITE_PARENT="+l.RunID()), stdin, stdout, stderr)
	l.Logger.Info("suite_end", "recipe", *recipe, "exit", code, "ms", time.Since(started).Milliseconds())
	if err := l.Close(); err != nil {
		fmt.Fprintf(stderr, "%s: FAIL the end of this %s run was not recorded: %v\n", tool, *recipe, err)
		if code == 0 {
			code = runlog.ExitLogFailure
		}
	}
	return code
}

// sendAlert decides whether this run alerts the owner, and runs the hook when it does.
func sendAlert(ctx context.Context, env []string, getenv func(string) string, sess session, parent, recipe, where string) (string, error) {
	switch {
	case parent != "":
		return "nested", nil
	case !sess.agent:
		return "not_an_agent", nil
	}
	for _, exempt := range filepath.SplitList(getenv("NEVR_SUITE_ALERT_EXEMPT")) {
		if exempt != "" && sess.cwd != "" && filepath.Clean(exempt) == filepath.Clean(sess.cwd) {
			return "exempt", nil
		}
	}
	hook := getenv("NEVR_SUITE_ALERT")
	if hook == "" {
		return "no_hook", nil
	}
	actx, cancel := context.WithTimeout(ctx, alertTimeout)
	defer cancel()
	cmd := exec.CommandContext(actx, hook, "--from", "nevr-runtime", where+" ran "+recipe)
	cmd.Env = env
	if out, err := cmd.CombinedOutput(); err != nil {
		return "failed", fmt.Errorf("%s: %v: %s", hook, err, strings.TrimSpace(string(out)))
	}
	return "sent", nil
}

// runChild runs the suite with the caller's terminal. A SIGTERM to this process is passed on; an interrupt from the
// terminal already reaches the suite, so it is only absorbed here, and the end record is still written.
func runChild(command []string, env []string, stdin io.Reader, stdout, stderr io.Writer) int {
	cmd := exec.Command(command[0], command[1:]...)
	cmd.Env = env
	cmd.Stdin, cmd.Stdout, cmd.Stderr = stdin, stdout, stderr
	sigs := make(chan os.Signal, 1)
	signal.Notify(sigs, os.Interrupt, syscall.SIGTERM)
	defer signal.Stop(sigs)
	if err := cmd.Start(); err != nil {
		fmt.Fprintf(stderr, "%s: cannot start %s: %v\n", tool, command[0], err)
		return exitNotStarted
	}
	done := make(chan struct{})
	var forwarding sync.WaitGroup
	forwarding.Add(1)
	go func() {
		defer forwarding.Done()
		for {
			select {
			case s := <-sigs:
				if s == syscall.SIGTERM {
					signalChild(cmd.Process, s, stderr)
				}
			case <-done:
				return
			}
		}
	}()
	err := cmd.Wait()
	close(done)
	forwarding.Wait() // no line is written to stderr after runChild returns
	var exitErr *exec.ExitError
	switch {
	case err == nil:
		return 0
	case errors.As(err, &exitErr):
		if ws, ok := exitErr.Sys().(syscall.WaitStatus); ok && ws.Signaled() {
			return exitSignalBase + int(ws.Signal())
		}
		return exitErr.ExitCode()
	}
	fmt.Fprintf(stderr, "%s: %s: %v\n", tool, command[0], err)
	return exitNotStarted
}

// signalChild passes s on to the suite's process. A signal that does not reach it is a line on standard error that names
// the signal and the error, so the failure is on the record of the run's own output (logging standard, "Loud").
func signalChild(p *os.Process, s os.Signal, stderr io.Writer) {
	if err := p.Signal(s); err != nil {
		fmt.Fprintf(stderr, "%s: cannot pass signal %d (%v) on to the suite: %v\n", tool, s, s, err)
	}
}
