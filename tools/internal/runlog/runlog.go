// Package runlog is the durable log of a Go tool (ADR 0044, rule 13): one JSON line per event, appended to
// $XDG_STATE_HOME/nevr-runtime/<tool>.jsonl (with $XDG_STATE_HOME at ~/.local/state when it is unset). A log that cannot
// be opened or written is a line on standard error and a non-zero exit; a tool never goes on without its record.
//
// A record has the keys the Python tools' logs have: ts (UTC, RFC 3339 with milliseconds and a Z), level (debug, info,
// warn or error, lower case), event (what happened), then tool (the tool's name), run (an id for this process's run, in
// the form of the test runner's file stem: <UTC stamp>-<pid>-<16 hex>) and pid, and then the fields the call gives.
// Debug records are not written. A port that must keep more of a Python tool's fields passes them as fields.
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
	"context"
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
	"time"
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
	run    string
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
	run, err := newRunID(time.Now())
	if err != nil {
		f.Close()
		return nil, fmt.Errorf("run log %s: %w", path, err)
	}
	w := &failureWriter{w: f}
	inner := slog.NewJSONHandler(w, &slog.HandlerOptions{Level: slog.LevelInfo, ReplaceAttr: replaceAttr})
	// The header fields go on the inner handler, before the guard that renames the caller's fields.
	header := inner.WithAttrs([]slog.Attr{slog.String("tool", toolOf(path)), slog.String("run", run), slog.Int("pid", os.Getpid())})
	return &Log{Logger: slog.New(guard{header}), run: run, path: path, file: f, w: w}, nil
}

// toolOf is the tool name a log file at path belongs to: the file name without ".jsonl".
func toolOf(path string) string { return strings.TrimSuffix(filepath.Base(path), ".jsonl") }

// newRunID is <UTC stamp to the microsecond>-<pid>-<16 random hex digits>, the form of the test runner's file stem
// without the commit.
func newRunID(now time.Time) (string, error) {
	var salt [8]byte
	if _, err := rand.Read(salt[:]); err != nil {
		return "", err
	}
	return fmt.Sprintf("%s-%d-%s", now.UTC().Format("20060102T150405.000000Z"), os.Getpid(), hex.EncodeToString(salt[:])), nil
}

// reserved are the keys of a record's header, and slog's own names for three of them. A field of the caller's with one of
// these names is written as field_<name>, so it can neither be mistaken for the header nor break the renaming below.
var reserved = map[string]bool{"ts": true, "level": true, "event": true, "tool": true, "run": true, "pid": true, "msg": true, "time": true}

func fieldKey(key string) string {
	if reserved[key] {
		return "field_" + key
	}
	return key
}

func fieldAttr(a slog.Attr) slog.Attr {
	a.Key = fieldKey(a.Key)
	return a
}

// guard renames the caller's top-level fields that collide with the header (fields inside a group keep their names:
// they are below their group's key).
type guard struct{ slog.Handler }

func (g guard) Handle(ctx context.Context, r slog.Record) error {
	renamed := slog.NewRecord(r.Time, r.Level, r.Message, r.PC)
	r.Attrs(func(a slog.Attr) bool {
		renamed.AddAttrs(fieldAttr(a))
		return true
	})
	return g.Handler.Handle(ctx, renamed)
}

func (g guard) WithAttrs(attrs []slog.Attr) slog.Handler {
	renamed := make([]slog.Attr, len(attrs))
	for i, a := range attrs {
		renamed[i] = fieldAttr(a)
	}
	return guard{g.Handler.WithAttrs(renamed)}
}

func (g guard) WithGroup(name string) slog.Handler { return guard{g.Handler.WithGroup(name)} }

// replaceAttr renames slog's own keys to the repository's: time becomes ts in UTC, msg becomes event, and the level is a
// lower-case name. By the time it runs the caller's fields no longer use those names (guard), so the three keys are
// slog's.
func replaceAttr(groups []string, a slog.Attr) slog.Attr {
	if len(groups) > 0 {
		return a
	}
	switch a.Key {
	case slog.TimeKey:
		return slog.String("ts", a.Value.Time().UTC().Format("2006-01-02T15:04:05.000Z"))
	case slog.MessageKey:
		return slog.String("event", a.Value.String())
	case slog.LevelKey:
		return slog.String(slog.LevelKey, strings.ToLower(a.Value.Any().(slog.Level).String()))
	}
	return a
}

// RunID is the id carried by every record of this log.
func (l *Log) RunID() string { return l.run }

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
