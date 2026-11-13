// Command release-verify proves the bytes of a published release against its manifest and writes the per-file record.
//
//	release-verify -manifest manifest.json -version X.Y.Z -dir <downloaded files>
//
// The manifest is the verified manifest.json of docs/reference/verifying-a-release.md (steps 1 to 6 are done before this
// command; it does not check the signature). For each entry of releases[<version>].artifacts it checks that
// <dir>/<path> exists, has exactly `size` bytes and the sha256 `sha256`, and prints one line
//
//	OK|FAIL <kind> <path> size=<n> sha256=<hex> <reason>
//
// then "manifest_sha256=<hex>" so the record is tied to the manifest it was made from, and refuses (exit 1) when any
// entry fails, the version names no artifact, or a path leaves <dir>. A release test starts only after this exits 0.
//
// Exit codes: 0 every file is OK; 1 a file fails or the version has no artifacts; 2 a bad flag or an unreadable manifest.
package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"strings"

	"github.com/EchoTools/nevr-runtime/tools/internal/runlog"
)

type artifact struct {
	Kind   string `json:"kind"`
	Path   string `json:"path"`
	Size   int64  `json:"size"`
	Sha256 string `json:"sha256"`
}

type manifest struct {
	Releases map[string]struct {
		Artifacts []artifact `json:"artifacts"`
	} `json:"releases"`
}

// exitUsage is the exit code of a bad flag, a missing argument or input that cannot be read.
const exitUsage = 2

func main() { os.Exit(run(os.Args[1:], os.Getenv, os.Stdout, os.Stderr)) }

// run is execute with the tool's run log: one "invoked" and one "finished" record in $XDG_STATE_HOME/nevr-runtime/release-verify.jsonl
// (internal/runlog). A log that cannot be written is exit 74, never a run without its record.
func run(args []string, getenv func(string) string, stdout, stderr io.Writer) int {
	return runlog.Invocation("release-verify", args, getenv, stderr, func(*slog.Logger) int { return execute(args, stdout, stderr) })
}

func execute(args []string, stdout, stderr io.Writer) int {
	fl := flag.NewFlagSet("release-verify", flag.ContinueOnError)
	fl.SetOutput(stderr)
	mpath := fl.String("manifest", "", "the verified manifest.json")
	version := fl.String("version", "", "release version, X.Y.Z")
	dir := fl.String("dir", "", "directory holding the downloaded files")
	if err := fl.Parse(args); err != nil {
		return exitUsage
	}
	if *mpath == "" || *version == "" || *dir == "" {
		fmt.Fprintln(stderr, "release-verify: -manifest, -version and -dir are required")
		return exitUsage
	}
	raw, err := os.ReadFile(*mpath)
	if err != nil {
		fmt.Fprintf(stderr, "release-verify: %v\n", err)
		return exitUsage
	}
	var m manifest
	if err := json.Unmarshal(raw, &m); err != nil {
		fmt.Fprintf(stderr, "release-verify: manifest is not JSON of the expected shape: %v\n", err)
		return exitUsage
	}
	rel, ok := m.Releases[*version]
	if !ok || len(rel.Artifacts) == 0 {
		fmt.Fprintf(stderr, "release-verify: the manifest lists no artifacts for version %s\n", *version)
		return 1
	}
	failed := 0
	for _, a := range rel.Artifacts {
		reason := verify(*dir, a)
		status := "OK"
		if reason != "" {
			status = "FAIL"
			failed++
		}
		fmt.Fprintf(stdout, "%s %s %s size=%d sha256=%s %s\n", status, a.Kind, a.Path, a.Size, a.Sha256, reason)
	}
	sum := sha256.Sum256(raw)
	fmt.Fprintf(stdout, "manifest_sha256=%s\n", hex.EncodeToString(sum[:]))
	if failed > 0 {
		fmt.Fprintf(stderr, "release-verify: %d of %d file(s) failed; this is not a release to test\n", failed, len(rel.Artifacts))
		return 1
	}
	return 0
}

func verify(dir string, a artifact) string {
	if a.Path == "" || filepath.IsAbs(a.Path) || strings.Contains(filepath.ToSlash(a.Path), "..") {
		return "path leaves the download directory"
	}
	f, err := os.Open(filepath.Join(dir, filepath.FromSlash(a.Path)))
	if err != nil {
		return "missing"
	}
	defer f.Close()
	h := sha256.New()
	// Read one byte past the stated size so a padded file is a size failure, not a long hash.
	n, err := io.Copy(h, io.LimitReader(f, a.Size+1))
	if err != nil {
		return "unreadable: " + err.Error()
	}
	if n != a.Size {
		return fmt.Sprintf("size is %d, not %d", n, a.Size)
	}
	if got := hex.EncodeToString(h.Sum(nil)); !strings.EqualFold(got, a.Sha256) {
		return "sha256 is " + got
	}
	return ""
}
