// Command quest-apk-version is the versionCode of the Quest APK: the numbering rule, and a strict reader and writer of the
// compiled AndroidManifest.xml. It is the Go port of tools/quest_apk_version.py (ADR 0044).
//
//	quest-apk-version [-repo dir] code <version>                       print the versionCode the rule gives a version
//	quest-apk-version read <apk>                                        print the versionCode in an APK's manifest
//	quest-apk-version [-repo dir] patch-manifest <AndroidManifest.xml> (-build-info <nevr_build_info.h> | -version <v>) -shim <so>
//	                                                                    set the versionCode in an extracted manifest, in place;
//	                                                                    print it. The version is the build info's (or the one given,
//	                                                                    for a loader built elsewhere: a release asset), and the shim must
//	                                                                    carry the same version string. A version below the newest release
//	                                                                    tag's major is refused.
//
// -repo names the repository whose release tags set the floor (default: the working directory). Each action writes JSON
// lines to standard error (tag NEVR.QUEST.APK); a refusal is one JSON line and exit 1; a bad command line is exit 2.
// The run is logged to $XDG_STATE_HOME/nevr-runtime/quest-apk-version.jsonl (internal/runlog).
//
// Exit codes: 0 done; 1 refused (the reason is on standard error); 2 a bad command line.
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
	toolName       = "quest-apk-version"
	exitRefused    = 1
	exitUsage      = 2
	manifestMode   = 0o644
	defaultRepoDir = "."
	usage          = "usage: quest-apk-version [-repo dir] code <version> | read <apk> | patch-manifest <AndroidManifest.xml> (-build-info <h> | -version <v>) -shim <so>"
	bytesChanged   = 4
	oneArgument    = 1
)

func main() {
	os.Exit(run(context.Background(), os.Args[1:], os.Getenv, os.Stdin, os.Stdout, os.Stderr))
}

// run is execute with the tool's run log (runlog.Invocation): one "invoked" and one "finished" record, and a log that
// cannot be written is exit 74.
func run(_ context.Context, args []string, getenv func(string) string, _ io.Reader, stdout, stderr io.Writer) int {
	return runlog.Invocation(toolName, args, getenv, stderr, func(*slog.Logger) int {
		return execute(args, stdout, stderr)
	})
}

// execute dispatches the command named by the first argument.
func execute(args []string, stdout, stderr io.Writer) int {
	if len(args) == 0 {
		fmt.Fprintln(stderr, usage)
		return exitUsage
	}
	switch args[0] {
	case "code":
		return cmdCode(args[1:], stdout, stderr)
	case "read":
		return cmdRead(args[1:], stdout, stderr)
	case "patch-manifest":
		return cmdPatch(args[1:], stdout, stderr)
	default:
		fmt.Fprintf(stderr, "quest-apk-version: unknown command %q\n%s\n", args[0], usage)
		return exitUsage
	}
}

// newFlags is a flag set that writes its errors to stderr and does not exit.
func newFlags(name string, stderr io.Writer) *flag.FlagSet {
	fl := flag.NewFlagSet(name, flag.ContinueOnError)
	fl.SetOutput(stderr)
	return fl
}

// refuse writes the refusal line of a command and returns the refused exit code.
func refuse(stderr io.Writer, command string, err error) int {
	questapk.Emit(stderr, questapk.LogTag, "refused", questapk.Field{Key: "command", Value: command}, questapk.Field{Key: "reason", Value: err.Error()})
	return exitRefused
}

// cmdCode prints the versionCode the rule gives a version, and says whether a stamp of it would be refused.
func cmdCode(args []string, stdout, stderr io.Writer) int {
	fl := newFlags("code", stderr)
	repo := fl.String("repo", defaultRepoDir, "the repository whose release tags set the floor")
	pos, err := questapk.ParseMixed(fl, args)
	if err != nil {
		return exitUsage
	}
	if len(pos) != oneArgument {
		fmt.Fprintln(stderr, "quest-apk-version code: takes one version")
		return exitUsage
	}
	version := pos[0]
	code, err := questapk.VersionCodeFor(version)
	if err != nil {
		return refuse(stderr, "code", err)
	}
	tag, floor, err := questapk.NewestRelease(*repo, nil)
	if err != nil {
		return refuse(stderr, "code", err)
	}
	major, _ := questapk.MajorOf(version)
	questapk.Emit(stderr, questapk.LogTag, "version_code",
		questapk.Field{Key: "version", Value: version},
		questapk.Field{Key: "version_code", Value: code},
		questapk.Field{Key: "newest_release", Value: tag},
		questapk.Field{Key: "floor_major", Value: floor},
		questapk.Field{Key: "stampable", Value: major >= floor})
	fmt.Fprintln(stdout, code)
	return 0
}

// cmdRead prints the versionCode in an APK's manifest.
func cmdRead(args []string, stdout, stderr io.Writer) int {
	fl := newFlags("read", stderr)
	pos, err := questapk.ParseMixed(fl, args)
	if err != nil {
		return exitUsage
	}
	if len(pos) != oneArgument {
		fmt.Fprintln(stderr, "quest-apk-version read: takes one APK")
		return exitUsage
	}
	code, err := questapk.VersionCodeOfAPK(pos[0])
	if err != nil {
		return refuse(stderr, "read", err)
	}
	fmt.Fprintln(stdout, code)
	return 0
}

// cmdPatch sets the versionCode of an extracted manifest in place, after the shim is checked to carry the same version.
func cmdPatch(args []string, stdout, stderr io.Writer) int {
	fl := newFlags("patch-manifest", stderr)
	buildInfo := fl.String("build-info", "", "the nevr_build_info.h the build generated (kVersion is the version)")
	version := fl.String("version", "", "the version, for a loader built elsewhere")
	shim := fl.String("shim", "", "the shim that must carry the version (required)")
	repo := fl.String("repo", defaultRepoDir, "the repository whose release tags set the floor")
	pos, err := questapk.ParseMixed(fl, args)
	if err != nil {
		return exitUsage
	}
	set := map[string]bool{}
	fl.Visit(func(f *flag.Flag) { set[f.Name] = true })
	if len(pos) != oneArgument {
		fmt.Fprintln(stderr, "quest-apk-version patch-manifest: takes one manifest")
		return exitUsage
	}
	if set["build-info"] == set["version"] {
		fmt.Fprintln(stderr, "quest-apk-version patch-manifest: exactly one of -build-info and -version is required")
		return exitUsage
	}
	if !set["shim"] {
		fmt.Fprintln(stderr, "quest-apk-version patch-manifest: -shim is required")
		return exitUsage
	}
	manifest := pos[0]
	stamped, err := stampManifest(stderr, manifest, *buildInfo, *version, set["version"], *shim, *repo)
	if err != nil {
		return refuse(stderr, "patch-manifest", err)
	}
	fmt.Fprintln(stdout, stamped.code)
	return 0
}

// stamped is what a successful patch-manifest wrote.
type stamped struct {
	code int
}

// stampManifest is the patch-manifest action: the version, its floor, the shim's check, the manifest's 4-byte write and its
// log line. Every refusal is returned before the manifest is written.
func stampManifest(stderr io.Writer, manifest, buildInfo, version string, versionGiven bool, shim, repo string) (stamped, error) {
	sourceName := "--version"
	if !versionGiven {
		v, err := questapk.ReadBuildVersion(buildInfo)
		if err != nil {
			return stamped{}, err
		}
		version, sourceName = v, filepath.Base(buildInfo)
	}
	newCode, err := questapk.StampCodeFor(version, questapk.NewestSource(repo))
	if err != nil {
		return stamped{}, err
	}
	shimBytes, err := os.ReadFile(shim)
	if err != nil {
		return stamped{}, fmt.Errorf("cannot read the shim %s: %s", shim, err.Error())
	}
	if !questapk.ShimCarriesVersion(shimBytes, version) {
		return stamped{}, fmt.Errorf("the shim %s does not carry the version string '%s' that %s names: the version and the shim are from different builds",
			filepath.Base(shim), version, sourceName)
	}
	original, err := os.ReadFile(manifest)
	if err != nil {
		return stamped{}, err
	}
	before, err := questapk.ParseManifest(original)
	if err != nil {
		return stamped{}, err
	}
	patched, err := questapk.PatchManifest(original, newCode)
	if err != nil {
		return stamped{}, err
	}
	if err := os.WriteFile(manifest, patched, manifestMode); err != nil {
		return stamped{}, err
	}
	questapk.Emit(stderr, questapk.LogTag, "version_code_stamped",
		questapk.Field{Key: "version", Value: version},
		questapk.Field{Key: "version_code_before", Value: before.VersionCode},
		questapk.Field{Key: "version_code_after", Value: newCode},
		questapk.Field{Key: "manifest_sha256_before", Value: questapk.Sha256Hex(original)},
		questapk.Field{Key: "manifest_sha256_after", Value: questapk.Sha256Hex(patched)},
		questapk.Field{Key: "bytes_changed", Value: bytesChanged})
	return stamped{code: newCode}, nil
}
