// Command quest-apk-rename-package renames the package of the reference build's APK, com.readyatdawn.afk to
// com.readyatdawn.r15, with every count asserted. It is the Go port of tools/quest_apk_rename_package.py (ADR 0044).
//
//	quest-apk-rename-package rename --apk IN.apk --out OUT.apk [-from com.readyatdawn.afk] [-to com.readyatdawn.r15] [-label TEXT]
//
// One JSON line on standard output per run (tag NEVR.QUEST.RENAME, the event, the counts, the sha256 in and out of every
// changed entry). A refusal is one JSON line on standard error and exit 1; nothing is written. A bad command line is exit
// 2. The run is logged to $XDG_STATE_HOME/nevr-runtime/quest-apk-rename-package.jsonl (internal/runlog).
//
// The flags of the Python tool are kept with their double dash (--apk, --out, --from, --to, --label); the Go flag package
// takes both spellings.
package main

import (
	"context"
	"flag"
	"fmt"
	"io"
	"log/slog"
	"os"
	"path/filepath"

	"github.com/EchoTools/nevr-runtime/tools/internal/questapk"
	"github.com/EchoTools/nevr-runtime/tools/internal/runlog"
)

const (
	toolName      = "quest-apk-rename-package"
	exitRefused   = 1
	exitUsage     = 2
	usageLine     = "usage: quest-apk-rename-package rename --apk IN.apk --out OUT.apk [--from PKG] [--to PKG] [--label TEXT]"
	renameCommand = "rename"
	refusedEvent  = "package_rename_refused"
)

func main() {
	os.Exit(run(context.Background(), os.Args[1:], os.Getenv, os.Stdin, os.Stdout, os.Stderr))
}

// run is execute with the tool's run log (runlog.Invocation): one "invoked" and one "finished" record; a log that cannot be
// written is exit 74.
func run(_ context.Context, args []string, getenv func(string) string, _ io.Reader, stdout, stderr io.Writer) int {
	return runlog.Invocation(toolName, args, getenv, stderr, func(*slog.Logger) int {
		return execute(args, stdout, stderr)
	})
}

// execute reads the one subcommand, rename.
func execute(args []string, stdout, stderr io.Writer) int {
	if len(args) == 0 || args[0] != renameCommand {
		fmt.Fprintln(stderr, usageLine)
		return exitUsage
	}
	fl := flag.NewFlagSet(renameCommand, flag.ContinueOnError)
	fl.SetOutput(stderr)
	apk := fl.String("apk", "", "the reference APK to read (required)")
	out := fl.String("out", "", "the APK to write (required)")
	from := fl.String("from", questapk.DefaultFrom, "the package name to replace")
	to := fl.String("to", questapk.DefaultTo, "the package name to write (the same length)")
	label := fl.String("label", "", "also set the manifest's launcher label to this text (in place, no longer than the old one)")
	pos, err := questapk.ParseMixed(fl, args[1:])
	if err != nil || len(pos) != 0 {
		if err == nil {
			fmt.Fprintln(stderr, "quest-apk-rename-package rename: takes no positional arguments")
		}
		return exitUsage
	}
	set := map[string]bool{}
	fl.Visit(func(f *flag.Flag) { set[f.Name] = true })
	if !set["apk"] || !set["out"] {
		fmt.Fprintln(stderr, "quest-apk-rename-package rename: -apk and -out are required")
		return exitUsage
	}
	var labelArg *string
	if set["label"] {
		labelArg = label
	}
	rec, err := questapk.RenameApk(*apk, *out, *from, *to, labelArg)
	if err != nil {
		questapk.Emit(stderr, questapk.RenameTag, refusedEvent,
			questapk.Field{Key: "apk", Value: filepath.Base(*apk)},
			questapk.Field{Key: "reason", Value: err.Error()})
		return exitRefused
	}
	line, err := rec.Line()
	if err != nil {
		fmt.Fprintf(stderr, "quest-apk-rename-package: %v\n", err)
		return exitRefused
	}
	fmt.Fprintln(stdout, line)
	return 0
}
