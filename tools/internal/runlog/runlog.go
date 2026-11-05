// Package runlog is the durable log of a Go tool (ADR 0044, rule 13): one JSON line per event, appended to
// $XDG_STATE_HOME/nevr-runtime/<tool>.jsonl (with $XDG_STATE_HOME at ~/.local/state when it is unset). A log that cannot
// be opened or written is a line on standard error and a non-zero exit; a tool never goes on without its record.
//
// A tool's main is os.Exit(run(...)); run wraps its work in Run:
//
//	func run(ctx context.Context, args []string, getenv func(string) string, stdin io.Reader, stdout, stderr io.Writer) int {
//		return runlog.Run("my-tool", getenv, stderr, func(log *slog.Logger) int {
//			log.Info("start", "args", args)
//			// ...
//			return 0
//		})
//	}
package runlog

import (
	"fmt"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"regexp"
	"sync"
)

// ExitLogFailure is the exit code of a run whose log could not be opened, or whose body succeeded but lost an event
// (EX_IOERR in sysexits.h). A body that failed keeps its own code.
const ExitLogFailure = 74

var toolName = regexp.MustCompile(`^[a-z0-9][a-z0-9_-]*$`)

// Path returns the log file of tool: $XDG_STATE_HOME/nevr-runtime/<tool>.jsonl, where an unset or relative
// $XDG_STATE_HOME (the XDG specification says a relative one is invalid) is $HOME/.local/state. tool is a plain
// lower-case name (letters, digits, '-' and '_'). getenv is os.Getenv in a tool's main and a map's lookup in a test.
func Path(tool string, getenv func(string) string) (string, error) {
	if !toolName.MatchString(tool) {
		return "", fmt.Errorf("run log: %q is not a tool name (lower-case letters, digits, '-' and '_')", tool)
	}
	state := getenv("XDG_STATE_HOME")
	if state == "" || !filepath.IsAbs(state) {
		home := getenv("HOME")
		if home == "" {
			return "", fmt.Errorf("run log: neither an absolute XDG_STATE_HOME nor HOME is set, so there is no place for the log of %s", tool)
		}
		state = filepath.Join(home, ".local", "state")
	}
	return filepath.Join(state, "nevr-runtime", tool+".jsonl"), nil
}

// Log is an open run log. Logger writes each record with one write to a file opened for appending, so records from
// several goroutines, and from several processes, do not interleave.
type Log struct {
	Logger *slog.Logger
	path   string
	file   *os.File
	w      *failureWriter
}

// Open opens the log of tool (see Path) for appending, making its directory.
func Open(tool string, getenv func(string) string) (*Log, error) {
	path, err := Path(tool, getenv)
	if err != nil {
		return nil, err
	}
	return OpenAt(path)
}

// OpenAt opens the log file at path for appending, making its directory. A tool whose log path an environment
// variable overrides uses it with that path.
func OpenAt(path string) (*Log, error) {
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return nil, fmt.Errorf("run log %s: %w", path, err)
	}
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_APPEND|os.O_CREATE, 0o644)
	if err != nil {
		return nil, fmt.Errorf("run log %s: %w", path, err)
	}
	w := &failureWriter{w: f}
	return &Log{Logger: slog.New(slog.NewJSONHandler(w, nil)), path: path, file: f, w: w}, nil
}

// Path is the file this log writes.
func (l *Log) Path() string { return l.path }

// Err is the first error a write returned, or nil. slog drops a handler's write error, so a tool that cares (Run does)
// asks here.
func (l *Log) Err() error { return l.w.err() }

// Close closes the file, and returns the first write error if there was one, else the close error.
func (l *Log) Close() error {
	cerr := l.file.Close()
	if err := l.w.err(); err != nil {
		return fmt.Errorf("run log %s: %w", l.path, err)
	}
	if cerr != nil {
		return fmt.Errorf("run log %s: %w", l.path, cerr)
	}
	return nil
}

// Run opens the log of tool, calls body with its logger and returns body's exit code. When the log cannot be opened it
// writes one line to stderr and returns ExitLogFailure without calling body. When an event could not be written it
// writes one line to stderr, and returns ExitLogFailure if body returned 0 (a failed body keeps its own code).
func Run(tool string, getenv func(string) string, stderr io.Writer, body func(log *slog.Logger) int) int {
	l, err := Open(tool, getenv)
	if err != nil {
		fmt.Fprintf(stderr, "%s: %v\n", tool, err)
		return ExitLogFailure
	}
	code := body(l.Logger)
	if err := l.Close(); err != nil {
		fmt.Fprintf(stderr, "%s: %v\n", tool, err)
		if code == 0 {
			code = ExitLogFailure
		}
	}
	return code
}

// failureWriter remembers the first error of the writer it wraps, and keeps passing writes on.
type failureWriter struct {
	w  io.Writer
	mu sync.Mutex
	e  error
}

func (f *failureWriter) Write(p []byte) (int, error) {
	n, err := f.w.Write(p)
	if err != nil {
		f.mu.Lock()
		if f.e == nil {
			f.e = err
		}
		f.mu.Unlock()
	}
	return n, err
}

func (f *failureWriter) err() error {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.e
}
