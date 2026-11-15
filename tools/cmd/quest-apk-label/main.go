// Command quest-apk-label reads and sets the launcher label of the Quest APK's compiled AndroidManifest.xml, in place. It is
// the Go port of tools/quest_apk_label.py (ADR 0044).
//
//	quest-apk-label show AndroidManifest.xml
//	quest-apk-label set AndroidManifest.xml OUT.xml -label "Echo VR"
//
// Each action writes one JSON line on standard output (tag NEVR.QUEST.LABEL) and exits 0. A refusal is one JSON line on
// standard error (event label_refused) and exit 1. A bad command line is exit 2. The run is logged to
// $XDG_STATE_HOME/nevr-runtime/quest-apk-label.jsonl (internal/runlog).
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
	toolName       = "quest-apk-label"
	exitRefused    = 1
	exitUsage      = 2
	labelMode      = 0o644
	usageLine      = "usage: quest-apk-label show AndroidManifest.xml | set AndroidManifest.xml OUT.xml -label TEXT"
	showCmd        = "show"
	setCmd         = "set"
	setPositionals = 2
)

func main() {
	os.Exit(run(context.Background(), os.Args[1:], os.Getenv, os.Stdin, os.Stdout, os.Stderr))
}

// run is execute with the tool's run log (runlog.Invocation); a log that cannot be written is exit 74.
func run(_ context.Context, args []string, getenv func(string) string, _ io.Reader, stdout, stderr io.Writer) int {
	return runlog.Invocation(toolName, args, getenv, stderr, func(*slog.Logger) int {
		return execute(args, stdout, stderr)
	})
}

// execute dispatches show and set.
func execute(args []string, stdout, stderr io.Writer) int {
	if len(args) == 0 {
		fmt.Fprintln(stderr, usageLine)
		return exitUsage
	}
	switch args[0] {
	case showCmd:
		return cmdShow(args[1:], stdout, stderr)
	case setCmd:
		return cmdSet(args[1:], stdout, stderr)
	default:
		fmt.Fprintln(stderr, usageLine)
		return exitUsage
	}
}

// refuse writes the refusal line and returns the refused exit code.
func refuse(stderr io.Writer, err error) int {
	questapk.Emit(stderr, questapk.LabelTag, "label_refused", questapk.Field{Key: "reason", Value: err.Error()})
	return exitRefused
}

// cmdShow prints the label of a manifest.
func cmdShow(args []string, stdout, stderr io.Writer) int {
	fl := flag.NewFlagSet(showCmd, flag.ContinueOnError)
	fl.SetOutput(stderr)
	pos, err := questapk.ParseMixed(fl, args)
	if err != nil || len(pos) != 1 {
		if err == nil {
			fmt.Fprintln(stderr, usageLine)
		}
		return exitUsage
	}
	data, err := os.ReadFile(pos[0])
	if err != nil {
		return refuse(stderr, err)
	}
	label, err := questapk.LabelOf(data)
	if err != nil {
		return refuse(stderr, err)
	}
	questapk.Emit(stdout, questapk.LabelTag, "label", questapk.Field{Key: "label", Value: label})
	return 0
}

// cmdSet writes a copy of a manifest whose label is replaced, after the input's own label is read.
func cmdSet(args []string, stdout, stderr io.Writer) int {
	fl := flag.NewFlagSet(setCmd, flag.ContinueOnError)
	fl.SetOutput(stderr)
	label := fl.String("label", "", "the new label (required)")
	pos, err := questapk.ParseMixed(fl, args)
	if err != nil || len(pos) != setPositionals {
		if err == nil {
			fmt.Fprintln(stderr, usageLine)
		}
		return exitUsage
	}
	set := map[string]bool{}
	fl.Visit(func(f *flag.Flag) { set[f.Name] = true })
	if !set["label"] {
		fmt.Fprintln(stderr, "quest-apk-label set: -label is required")
		return exitUsage
	}
	data, err := os.ReadFile(pos[0])
	if err != nil {
		return refuse(stderr, err)
	}
	before, err := questapk.LabelOf(data)
	if err != nil {
		return refuse(stderr, err)
	}
	edited, err := questapk.SetLabel(data, *label)
	if err != nil {
		return refuse(stderr, err)
	}
	if err := os.WriteFile(pos[1], edited, labelMode); err != nil {
		return refuse(stderr, err)
	}
	questapk.Emit(stdout, questapk.LabelTag, "label_set",
		questapk.Field{Key: "before", Value: before},
		questapk.Field{Key: "after", Value: *label},
		questapk.Field{Key: "out", Value: filepath.Base(pos[1])})
	return 0
}
