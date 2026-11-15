// Command quest-runtime-pin pins the reference runtime's libr15 checksum list to the libr15 the APK carries (#1737). The
// rules, and why the list is in the binary in two forms, are in internal/questruntimepin.
//
//	quest-runtime-pin pin --libvrapi IN --libr15 FINAL_LIBR15 --out OUT [--replace-index 0] [--expect-old-md5 MD5]
//	quest-runtime-pin detect --libvrapi IN
//
// pin hashes FINAL_LIBR15 itself (the file the APK will carry), replaces the entry at --replace-index in both forms and
// writes OUT. It is idempotent: the entry already holding the md5 is a no-op that writes a byte-identical file. It refuses
// (exit 1) any other count, an md5 that is already present anywhere else in the file, a file that is not an AArch64 ELF,
// an entry other than --expect-old-md5, and an OUT that is the input. detect says whether IN carries the table at all
// (exit 0) or is some other libvrapi (exit 3: the store's own library carries none, and the recipe leaves it alone).
//
// One JSON object on one line says what happened: on standard output for runtime_detected, runtime_absent,
// runtime_pinned and runtime_pin_unchanged, on standard error for runtime_pin_refused. Exit codes: 0 done, 1 refused,
// 2 a usage error, 3 detect found no table, 74 the run log failed (internal/runlog).
package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"unicode/utf16"

	"github.com/EchoTools/nevr-runtime/tools/internal/questruntimepin"
	"github.com/EchoTools/nevr-runtime/tools/internal/runlog"
)

const (
	tool = "quest-runtime-pin"
	// tag is the tag of every record the command prints.
	tag = "NEVR.QUEST.RUNTIME_PIN"

	exitDone    = 0
	exitRefused = 1
	exitUsage   = 2
	exitAbsent  = 3

	// outputPerm is the mode OUT is created with, before the umask.
	outputPerm = 0o666

	// asciiMax is the last code point a JSON line carries as itself; the rest is written as \uXXXX, as Python's
	// json.dumps does.
	asciiMax = 0x7f
)

func main() {
	os.Exit(run(context.Background(), os.Args[1:], os.Getenv, os.Stdin, os.Stdout, os.Stderr))
}

func run(_ context.Context, args []string, getenv func(string) string, _ io.Reader, stdout, stderr io.Writer) int {
	return runlog.Run(tool, getenv, stderr, func(log *slog.Logger) int {
		return command(args, stdout, stderr, log)
	})
}

// record is one printed line: the keys sorted, each separator ", " and ": ", every non-ASCII character escaped, as
// Python's json.dumps(sort_keys=True) writes it.
type record map[string]any

func (r record) line() string {
	var b strings.Builder
	b.WriteByte('{')
	for i, k := range sortedKeys(r) {
		if i > 0 {
			b.WriteString(", ")
		}
		b.WriteString(jsonText(k))
		b.WriteString(": ")
		b.WriteString(jsonText(r[k]))
	}
	b.WriteByte('}')
	return b.String()
}

func jsonText(v any) string {
	if list, ok := v.([]string); ok { // a list's items are separated by ", ", as Python writes them
		items := make([]string, len(list))
		for i, item := range list {
			items[i] = jsonText(item)
		}
		return "[" + strings.Join(items, ", ") + "]"
	}
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	enc.SetEscapeHTML(false)
	if err := enc.Encode(v); err != nil { // only the command's own strings, numbers and string lists are encoded
		panic(err)
	}
	var b strings.Builder
	for _, r := range strings.TrimSuffix(buf.String(), "\n") {
		if r <= asciiMax {
			b.WriteRune(r)
			continue
		}
		if r1, r2 := utf16.EncodeRune(r); r1 != '�' || r2 != '�' {
			fmt.Fprintf(&b, `\u%04x\u%04x`, r1, r2)
		} else {
			fmt.Fprintf(&b, `\u%04x`, r)
		}
	}
	return b.String()
}

func emit(w io.Writer, log *slog.Logger, event string, fields record) {
	rec := record{"tag": tag, "event": event}
	for k, v := range fields {
		rec[k] = v
	}
	fmt.Fprintln(w, rec.line())
	args := make([]any, 0, 2*len(fields))
	for _, k := range sortedKeys(fields) {
		args = append(args, k, fields[k])
	}
	log.Info(event, args...)
}

func sortedKeys(r record) []string {
	keys := make([]string, 0, len(r))
	for k := range r {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	return keys
}

type pinArgs struct {
	libvrapi, libr15, out string
	replaceIndex          int
	expectOldMD5          string
}

// usage prints a usage error as argparse does and returns its exit code.
func usage(stderr io.Writer, format string, args ...any) int {
	fmt.Fprintf(stderr, "usage: %s {pin,detect} ...\n%s: error: %s\n", tool, tool, fmt.Sprintf(format, args...))
	return exitUsage
}

func command(args []string, stdout, stderr io.Writer, log *slog.Logger) int {
	if len(args) == 0 {
		return usage(stderr, "the following arguments are required: cmd")
	}
	switch args[0] {
	case "pin":
		fs := flag.NewFlagSet(tool+" pin", flag.ContinueOnError)
		fs.SetOutput(stderr)
		var p pinArgs
		fs.StringVar(&p.libvrapi, "libvrapi", "", "the reference runtime")
		fs.StringVar(&p.libr15, "libr15", "", "the libr15 the APK will carry")
		fs.StringVar(&p.out, "out", "", "the pinned runtime to write")
		fs.IntVar(&p.replaceIndex, "replace-index", 0, "the table entry to replace")
		fs.StringVar(&p.expectOldMD5, "expect-old-md5", questruntimepin.StockEntryMD5, "the md5 the entry must hold")
		if code, done := parse(fs, args[1:], stderr, "libvrapi", "libr15", "out"); done {
			return code
		}
		return pin(p, stdout, stderr, log)
	case "detect":
		fs := flag.NewFlagSet(tool+" detect", flag.ContinueOnError)
		fs.SetOutput(stderr)
		var libvrapi string
		fs.StringVar(&libvrapi, "libvrapi", "", "the library to look at")
		if code, done := parse(fs, args[1:], stderr, "libvrapi"); done {
			return code
		}
		return detect(libvrapi, stdout, stderr, log)
	}
	return usage(stderr, "argument cmd: invalid choice: %q (choose from 'pin', 'detect')", args[0])
}

// parse reads the flags of one subcommand; done is true when the command is over (a usage error, or -h).
func parse(fs *flag.FlagSet, args []string, stderr io.Writer, required ...string) (code int, done bool) {
	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return exitDone, true
		}
		return exitUsage, true
	}
	if fs.NArg() > 0 {
		return usage(stderr, "unrecognized arguments: %s", strings.Join(fs.Args(), " ")), true
	}
	seen := map[string]bool{}
	fs.Visit(func(f *flag.Flag) { seen[f.Name] = true })
	var missing []string
	for _, name := range required {
		if !seen[name] {
			missing = append(missing, "--"+name)
		}
	}
	if len(missing) > 0 {
		return usage(stderr, "the following arguments are required: %s", strings.Join(missing, ", ")), true
	}
	return exitDone, false
}

func refuse(stderr io.Writer, log *slog.Logger, fields record) int {
	fmt.Fprintln(stderr, record{"tag": tag, "event": "runtime_pin_refused"}.merge(fields).line())
	log.Warn("runtime_pin_refused", "reason", fields["reason"])
	return exitRefused
}

func (r record) merge(more record) record {
	for k, v := range more {
		r[k] = v
	}
	return r
}

// resolve is the path with its symbolic links followed as far as it exists, made absolute.
func resolve(path string) string {
	abs, err := filepath.Abs(path)
	if err != nil {
		return path
	}
	if real, err := filepath.EvalSymlinks(abs); err == nil {
		return real
	}
	if dir, err := filepath.EvalSymlinks(filepath.Dir(abs)); err == nil {
		return filepath.Join(dir, filepath.Base(abs))
	}
	return abs
}

func tableOf(data []byte) []string {
	texts := []string{}
	for _, t := range questruntimepin.Table(data) {
		texts = append(texts, string(t))
	}
	return texts
}

func detect(libvrapi string, stdout, stderr io.Writer, log *slog.Logger) int {
	data, err := os.ReadFile(libvrapi)
	if err != nil {
		return refuse(stderr, log, record{"reason": err.Error()})
	}
	if _, err := questruntimepin.ExecRanges(data); err != nil {
		return refuse(stderr, log, record{"reason": err.Error()})
	}
	found := questruntimepin.CarriesTable(data)
	fields := record{"libvrapi_sha256": questruntimepin.Sha256Hex(data), "table": []string{}}
	if !found {
		emit(stdout, log, "runtime_absent", fields)
		return exitAbsent
	}
	fields["table"] = tableOf(data)
	emit(stdout, log, "runtime_detected", fields)
	return exitDone
}

func pin(p pinArgs, stdout, stderr io.Writer, log *slog.Logger) int {
	data, err := os.ReadFile(p.libvrapi)
	if err != nil {
		return refuse(stderr, log, record{"reason": err.Error()})
	}
	if resolve(p.out) == resolve(p.libvrapi) {
		return refuse(stderr, log, record{"reason": "--out is the input file: the pin writes a new file"})
	}
	md5, err := questruntimepin.MD5OfFile(p.libr15)
	if err != nil {
		return refuse(stderr, log, record{"reason": err.Error()})
	}
	result, report, err := questruntimepin.PinData(data, md5, p.replaceIndex, p.expectOldMD5)
	if err != nil {
		var wrong questruntimepin.WrongEntryError
		if errors.As(err, &wrong) {
			return refuse(stderr, log, record{"reason": err.Error(), "replace_index": wrong.Index,
				"expected_old_md5": wrong.Expected, "found_md5": wrong.Found})
		}
		return refuse(stderr, log, record{"reason": err.Error()})
	}
	if err := os.WriteFile(p.out, result, outputPerm); err != nil {
		return refuse(stderr, log, record{"reason": err.Error()})
	}
	event := "runtime_pinned"
	if bytes.Equal(result, data) {
		event = "runtime_pin_unchanged"
	}
	emit(stdout, log, event, record{
		"libr15_md5": md5, "libvrapi_sha256_in": questruntimepin.Sha256Hex(data), "libvrapi_sha256_out": questruntimepin.Sha256Hex(result),
		"out": filepath.Base(p.out), "table": report.Table, "replace_index": report.ReplaceIndex, "old_md5": report.OldMD5,
		"new_md5": report.NewMD5, "ascii_copies": report.ASCIICopies, "sites": report.Sites, "immediate_runs": report.ImmediateRuns,
		"immediate_words": report.ImmediateWords, "changed_bytes": report.ChangedBytes,
	})
	return exitDone
}
