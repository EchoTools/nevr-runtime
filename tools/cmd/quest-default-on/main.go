// Command quest-default-on puts our own assets/afk_default_on.txt into a Quest APK repack, and checks the signed result.
//
//	quest-default-on stage -apk SOURCE.apk -list LIST -stage DIR
//	    Refuses unless the list passes afkdefaulton.Problems. When SOURCE.apk holds assets/afk_default_on.txt (the reference
//	    build does; the store APK does not), writes the list's bytes to DIR/assets/afk_default_on.txt for the recipe to zip over
//	    the source's entry. A source without the entry gets nothing staged.
//	quest-default-on check -apk SIGNED.apk -source SOURCE.apk -list LIST
//	    Refuses unless SIGNED.apk holds the entry exactly when SOURCE.apk does, and then with the list's bytes.
//
// Exit codes: 0 done; 1 the list or the APK is refused (each reason printed); 2 a flag is wrong or a file cannot be read.
package main

import (
	"archive/zip"
	"bytes"
	"flag"
	"fmt"
	"io"
	"log/slog"
	"os"
	"path/filepath"

	"github.com/EchoTools/nevr-runtime/tools/internal/afkdefaulton"
	"github.com/EchoTools/nevr-runtime/tools/internal/runlog"
)

const (
	exitRefused = 1
	exitUsage   = 2
	dirPerm     = 0o755
	filePerm    = 0o644
)

func main() { os.Exit(run(os.Args[1:], os.Getenv, os.Stdout, os.Stderr)) }

func run(args []string, getenv func(string) string, stdout, stderr io.Writer) int {
	return runlog.Invocation("quest-default-on", args, getenv, stderr, func(*slog.Logger) int { return execute(args, stdout, stderr) })
}

func execute(args []string, stdout, stderr io.Writer) int {
	if len(args) == 0 || (args[0] != "stage" && args[0] != "check") {
		fmt.Fprintln(stderr, "quest-default-on: the first argument is stage or check")
		return exitUsage
	}
	fl := flag.NewFlagSet("quest-default-on "+args[0], flag.ContinueOnError)
	fl.SetOutput(stderr)
	apk := fl.String("apk", "", "the source APK (stage) or the signed APK (check)")
	source := fl.String("source", "", "check: the APK the signed one was made from")
	list := fl.String("list", "", "the list of switch files")
	stage := fl.String("stage", "", "stage: the directory the entry is written under")
	if err := fl.Parse(args[1:]); err != nil {
		return exitUsage
	}
	if *apk == "" || *list == "" || (args[0] == "stage" && *stage == "") || (args[0] == "check" && *source == "") {
		fmt.Fprintln(stderr, "quest-default-on: a flag is missing")
		return exitUsage
	}
	listBytes, err := os.ReadFile(*list)
	if err != nil {
		fmt.Fprintf(stderr, "quest-default-on: %v\n", err)
		return exitUsage
	}
	if len(listBytes) > afkdefaulton.MaxBytes {
		fmt.Fprintf(stderr, "quest-default-on: refused: %s is %d bytes; the runtime ignores a list over %d\n", *list, len(listBytes), afkdefaulton.MaxBytes)
		return exitRefused
	}
	names, err := afkdefaulton.Parse(string(listBytes))
	if err != nil {
		fmt.Fprintf(stderr, "quest-default-on: %s: %v\n", *list, err)
		return exitRefused
	}
	if problems := afkdefaulton.Problems(names); len(problems) > 0 {
		for _, p := range problems {
			fmt.Fprintln(stderr, "quest-default-on: refused:", p)
		}
		return exitRefused
	}
	if args[0] == "stage" {
		return doStage(*apk, *stage, listBytes, len(names), stdout, stderr)
	}
	return doCheck(*apk, *source, listBytes, stdout, stderr)
}

// entryBytes returns the bytes of Entry in an APK, and whether the APK holds it.
func entryBytes(path string) ([]byte, bool, error) {
	r, err := zip.OpenReader(path)
	if err != nil {
		return nil, false, err
	}
	defer r.Close()
	for _, f := range r.File {
		if f.Name != afkdefaulton.Entry {
			continue
		}
		rc, err := f.Open()
		if err != nil {
			return nil, true, err
		}
		defer rc.Close()
		b, err := io.ReadAll(rc)
		return b, true, err
	}
	return nil, false, nil
}

func doStage(apk, stage string, listBytes []byte, count int, stdout, stderr io.Writer) int {
	_, has, err := entryBytes(apk)
	if err != nil {
		fmt.Fprintf(stderr, "quest-default-on: %s: %v\n", apk, err)
		return exitUsage
	}
	if !has {
		fmt.Fprintf(stdout, "quest-default-on: %s holds no %s: nothing staged\n", filepath.Base(apk), afkdefaulton.Entry)
		return 0
	}
	dest := filepath.Join(stage, filepath.FromSlash(afkdefaulton.Entry))
	if err := os.MkdirAll(filepath.Dir(dest), dirPerm); err != nil {
		fmt.Fprintf(stderr, "quest-default-on: %v\n", err)
		return exitUsage
	}
	if err := os.WriteFile(dest, listBytes, filePerm); err != nil {
		fmt.Fprintf(stderr, "quest-default-on: %v\n", err)
		return exitUsage
	}
	fmt.Fprintf(stdout, "quest-default-on: staged %s (%d switches)\n", afkdefaulton.Entry, count)
	return 0
}

func doCheck(signed, source string, listBytes []byte, stdout, stderr io.Writer) int {
	_, sourceHas, err := entryBytes(source)
	if err != nil {
		fmt.Fprintf(stderr, "quest-default-on: %s: %v\n", source, err)
		return exitUsage
	}
	got, signedHas, err := entryBytes(signed)
	if err != nil {
		fmt.Fprintf(stderr, "quest-default-on: %s: %v\n", signed, err)
		return exitUsage
	}
	switch {
	case sourceHas != signedHas:
		fmt.Fprintf(stderr, "quest-default-on: refused: the source APK holds %s: %v, the signed APK: %v\n", afkdefaulton.Entry, sourceHas, signedHas)
		return exitRefused
	case signedHas && !bytes.Equal(got, listBytes):
		fmt.Fprintf(stderr, "quest-default-on: refused: the signed APK's %s is not the repository's list\n", afkdefaulton.Entry)
		return exitRefused
	}
	fmt.Fprintln(stdout, "quest-default-on: OK")
	return 0
}
