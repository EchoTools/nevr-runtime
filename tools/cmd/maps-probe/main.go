// Command maps-probe packages, installs and restores the custom-map probe build (internal/probepkg; docs/reference/maps-channel.md,
// "Owner probes"; ADR 0044).
//
//	maps-probe package -build-dir <build/mingw-map-probes> -tool <windows maps-probe.exe> [-out <dir>]
//	maps-probe install [-package <dir>] [-dir <...\bin\win10>] [-root <nevr folder>] [-whatif]
//	maps-probe restore [-root <nevr folder>] [-backup <probe-backup-name>] [-force] [-whatif]
//
// package runs on the build host and writes nevr-probes-<date>.zip. install and restore run on the Windows PC that has the game:
// install replaces the three probe files after backing each up, restore puts them back from that backup. A run refuses while
// echovr.exe runs.
//
// Exit codes: 0 done; 1 a refusal or a failure (the reason is printed, and a refusal changes nothing); 2 a usage error.
package main

import (
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"

	"github.com/EchoTools/nevr-runtime/tools/internal/probepkg"
)

// Exit codes, as the package comment names them.
const (
	exitOK      = 0
	exitFailed  = 1
	exitUsage   = 2
	usageText   = "usage: maps-probe package|install|restore [flags]"
	windowsOS   = "windows"
	gameProcess = "echovr.exe"
)

// libraryRoots are the places an Oculus or Meta Horizon library usually sits.
var libraryRoots = []string{
	`C:\Program Files\Meta Horizon`, `C:\Program Files\Oculus`, `C:\Program Files (x86)\Oculus`,
	`D:\Oculus`, `D:\Meta Horizon`, `E:\Oculus`, `E:\Meta Horizon`,
}

func main() {
	os.Exit(run(os.Args[1:], os.Getenv, gameRunning, os.Stdout, os.Stderr))
}

// gameRunning asks Windows whether echovr.exe has a process. Off Windows there is no game to run, so it is false.
func gameRunning() (bool, error) {
	if runtime.GOOS != windowsOS {
		return false, nil
	}
	out, err := exec.Command("tasklist", "/FI", "IMAGENAME eq "+gameProcess, "/NH").Output()
	if err != nil {
		return false, err
	}
	return strings.Contains(strings.ToLower(string(out)), gameProcess), nil
}

func run(args []string, getenv func(string) string, running func() (bool, error), stdout, stderr io.Writer) int {
	if len(args) == 0 {
		fmt.Fprintln(stderr, usageText)
		return exitUsage
	}
	switch args[0] {
	case "package":
		return runPackage(args[1:], stdout, stderr)
	case "install":
		return runInstall(args[1:], getenv, running, stdout, stderr)
	case "restore":
		return runRestore(args[1:], getenv, running, stdout, stderr)
	}
	fmt.Fprintf(stderr, "maps-probe: unknown subcommand %q; %s\n", args[0], usageText)
	return exitUsage
}

func runPackage(args []string, stdout, stderr io.Writer) int {
	fl := flag.NewFlagSet("maps-probe package", flag.ContinueOnError)
	fl.SetOutput(stderr)
	buildDir := fl.String("build-dir", "", "the probe build's directory (build/mingw-map-probes)")
	tool := fl.String("tool", "", "the windows maps-probe.exe to put in the package")
	out := fl.String("out", "", "where the package is made (default: the build directory)")
	if err := fl.Parse(args); err != nil {
		return exitUsage
	}
	if *buildDir == "" {
		fmt.Fprintln(stderr, "maps-probe package: -build-dir is required")
		return exitUsage
	}
	zip, err := probepkg.Package(probepkg.PackageOptions{BuildDir: *buildDir, OutDir: *out, ToolPath: *tool})
	if err != nil {
		fmt.Fprintln(stderr, "ERROR: "+err.Error())
		return exitFailed
	}
	fmt.Fprintln(stdout, "probe package: "+zip)
	return exitOK
}

func runInstall(args []string, getenv func(string) string, running func() (bool, error), stdout, stderr io.Writer) int {
	fl := flag.NewFlagSet("maps-probe install", flag.ContinueOnError)
	fl.SetOutput(stderr)
	pkg := fl.String("package", "", "the folder with the package's files (default: the folder of this program)")
	dir := fl.String("dir", "", "the game folder, <...>\\ready-at-dawn-echo-arena\\bin\\win10 (default: found in the usual places)")
	root := fl.String("root", "", "the nEVR folder (default: %LOCALAPPDATA%\\nevr\\echovr)")
	whatIf := fl.Bool("whatif", false, "print the plan and write nothing")
	if err := fl.Parse(args); err != nil {
		return exitUsage
	}
	if *pkg == "" {
		exe, err := os.Executable()
		if err != nil {
			fmt.Fprintln(stderr, "ERROR: "+err.Error())
			return exitFailed
		}
		*pkg = filepath.Dir(exe)
	}
	written, err := probepkg.Install(probepkg.Options{Package: *pkg, Root: *root, LocalAppData: getenv("LOCALAPPDATA"),
		Dir: *dir, LibraryRoots: libraryRoots, WhatIf: *whatIf, GameRunning: running, Out: stdout})
	if err != nil {
		fmt.Fprintln(stderr, "ERROR: "+err.Error())
		if written {
			fmt.Fprintln(stderr, "Some files may already be replaced: run maps-probe restore to put back what was there.")
		}
		return exitFailed
	}
	if !*whatIf {
		fmt.Fprintln(stdout, "To undo: maps-probe restore, with the same -root if you gave one")
	}
	return exitOK
}

func runRestore(args []string, getenv func(string) string, running func() (bool, error), stdout, stderr io.Writer) int {
	fl := flag.NewFlagSet("maps-probe restore", flag.ContinueOnError)
	fl.SetOutput(stderr)
	root := fl.String("root", "", "the nEVR folder (default: %LOCALAPPDATA%\\nevr\\echovr)")
	backup := fl.String("backup", "", "the probe-backup-<time> folder to restore, when more than one is open")
	force := fl.Bool("force", false, "also put back or remove a DLL that matches neither digest")
	whatIf := fl.Bool("whatif", false, "print the plan and write nothing")
	if err := fl.Parse(args); err != nil {
		return exitUsage
	}
	err := probepkg.Restore(probepkg.Options{Root: *root, LocalAppData: getenv("LOCALAPPDATA"), Backup: *backup, Force: *force,
		WhatIf: *whatIf, GameRunning: running, Out: stdout})
	if err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return exitUsage
		}
		fmt.Fprintln(stderr, "ERROR: "+err.Error())
		return exitFailed
	}
	return exitOK
}
