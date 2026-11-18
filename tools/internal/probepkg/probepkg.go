// Package probepkg packages, installs and restores the custom-map probe build (docs/reference/maps-channel.md, "Owner probes").
//
// A probe install replaces exactly three files: nevr-runtime.dll and maps_probe.yaml in the nEVR folder, and BugSplat64.dll (the
// host) in the game folder. The probe runtime is unsigned, so it needs the probe package's dev host: a release host refuses it and
// the game stops (ADR 0012). Install copies each file it replaces into <nEVR folder>\installer\probe-backup-<time>\ with a manifest
// of SHA-256 values; Restore puts them back from that manifest. Every copy goes through <file>.probe-partial beside its target and
// is swapped in once it reads back right, so a run that stops partway leaves every file whole. Nothing else is written or
// deleted, and config files are never read or written. The release installer cannot install these DLLs: it fetches the signed
// release ones over them.
package probepkg

import (
	"archive/zip"
	"bufio"
	"bytes"
	"crypto/sha256"
	_ "embed"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

// Place is the folder a probe file lives in.
type Place string

const (
	// PlaceRoot is the nEVR folder (%LOCALAPPDATA%\nevr\echovr).
	PlaceRoot Place = "root"
	// PlaceGame is the game folder (the one that holds echovr.exe).
	PlaceGame Place = "game"
)

// File is one file a probe install owns.
type File struct {
	Name  string
	Place Place
	// Mutable is true for a file the owner edits (maps_probe.yaml): a changed copy is expected at restore.
	Mutable bool
}

// Files are the three files a probe install writes, and nothing else.
var Files = []File{
	{Name: "nevr-runtime.dll", Place: PlaceRoot},
	{Name: "BugSplat64.dll", Place: PlaceGame},
	{Name: "maps_probe.yaml", Place: PlaceRoot, Mutable: true},
}

// Names of the package's and the backup's own files, and the literals a probe build carries.
const (
	SumsFile         = "PROBE_SHA256SUMS"
	ManifestName     = "manifest.tsv"
	ManifestHeader   = "nevr-probe-install 1"
	RestoredMarker   = "restored.txt"
	PartialSuffix    = ".probe-partial"
	BackupPrefix     = "probe-backup-"
	InstallerDir     = "installer"
	BackupTimeLayout = "20060102-150405"
	PackageDateFmt   = "20060102"
	DevHostLiteral   = "NEVR-HOST-BUILD dev"
	DevRuntimeLit    = "NEVR-PLUGIN-TRUST dev"
	// Absent is the digest recorded for a file that was not there.
	Absent = "absent"
	// GameExe is the file that makes a folder the game folder.
	GameExe = "echovr.exe"
	// ToolName is the installer tool's file in a package.
	ToolName = "maps-probe.exe"
)

// Permissions of the files and folders a probe install makes.
const (
	dirMode  fs.FileMode = 0o755
	fileMode fs.FileMode = 0o644
)

// A file that carries one of these literals is an nEVR runtime or host (the literals the release installer reads).
var nevrLiterals = []string{"NEVR-BUILD ", "nevr-runtime/1.0", "NEVR-HOST-BUILD "}

// The CMake cache entries that mark a probe build.
var probeCacheEntries = []string{"NEVR_MAP_PROBES", "NEVR_ALLOW_UNSIGNED_PLUGINS"}

//go:embed maps_probe.yaml
var probeConfig []byte

// ProbeConfig is the maps_probe.yaml a package carries: every probe off.
func ProbeConfig() []byte { return append([]byte(nil), probeConfig...) }

// Sha is the SHA-256 of the file at path in lower-case hex, or Absent when there is no such file.
func Sha(path string) (string, error) {
	f, err := os.Open(path)
	if errors.Is(err, fs.ErrNotExist) {
		return Absent, nil
	}
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

// HasLiteral reports whether the file at path holds text.
func HasLiteral(path, text string) (bool, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return false, err
	}
	return bytes.Contains(data, []byte(text)), nil
}

// IsNevrBinary reports whether the file at path exists and carries one of the nEVR build literals.
func IsNevrBinary(path string) bool {
	data, err := os.ReadFile(path)
	if err != nil {
		return false
	}
	for _, lit := range nevrLiterals {
		if bytes.Contains(data, []byte(lit)) {
			return true
		}
	}
	return false
}

// copyVerified copies from to a <to>.probe-partial, swaps it in once it reads back as sha, and leaves <to> as it was when it
// does not.
func copyVerified(from, to, sha string) error {
	partial := to + PartialSuffix
	data, err := os.ReadFile(from)
	if err != nil {
		return err
	}
	if err := os.WriteFile(partial, data, fileMode); err != nil {
		return err
	}
	got, err := Sha(partial)
	if err != nil {
		return err
	}
	if got != sha {
		_ = os.Remove(partial)
		return fmt.Errorf("the copy to %s reads back as %s, not %s", filepath.Base(to), got, sha)
	}
	// Rename replaces an existing target on every platform Go supports.
	return os.Rename(partial, to)
}

func placeDir(place Place, nevrRoot, gameDir string) (string, error) {
	switch place {
	case PlaceRoot:
		return nevrRoot, nil
	case PlaceGame:
		return gameDir, nil
	}
	return "", fmt.Errorf("unknown place %q", place)
}

func shownName(place Place, name string) string {
	if place == PlaceRoot {
		return `nevr folder\` + name
	}
	return `game folder\` + name
}

// ResolveRoot is the nEVR folder: root when given (absolute), else <localAppData>\nevr\echovr.
func ResolveRoot(root, localAppData string) (string, error) {
	if root != "" {
		if !filepath.IsAbs(root) {
			return "", errors.New("-root must be an absolute path")
		}
		return filepath.Clean(root), nil
	}
	if localAppData == "" {
		return "", errors.New("LOCALAPPDATA is not set; pass -root <nevr folder>")
	}
	return filepath.Join(localAppData, "nevr", "echovr"), nil
}

// LibrarySuffixes are the paths below an Oculus or Meta Horizon library that hold the game folder.
var LibrarySuffixes = []string{
	filepath.Join("Software", "Software", "ready-at-dawn-echo-arena", "bin", "win10"),
	filepath.Join("Software", "ready-at-dawn-echo-arena", "bin", "win10"),
	filepath.Join("ready-at-dawn-echo-arena", "bin", "win10"),
}

// FindGameDir is the game folder: dir when given (it must hold echovr.exe), else the one library root in roots whose suffix paths
// hold echovr.exe. Two installs or none is an error naming what to pass.
func FindGameDir(dir string, roots []string) (string, error) {
	if dir != "" {
		if !filepath.IsAbs(dir) {
			return "", errors.New("-dir must be an absolute path")
		}
		dir = filepath.Clean(dir)
		if _, err := os.Stat(filepath.Join(dir, GameExe)); err != nil {
			return "", errors.New("no " + GameExe + " in the -dir folder")
		}
		return dir, nil
	}
	var found []string
	for _, root := range roots {
		for _, suffix := range LibrarySuffixes {
			candidate := filepath.Join(root, suffix)
			if _, err := os.Stat(filepath.Join(candidate, GameExe)); err != nil {
				continue
			}
			dup := false
			for _, f := range found {
				dup = dup || strings.EqualFold(f, candidate)
			}
			if !dup {
				found = append(found, candidate)
			}
		}
	}
	switch len(found) {
	case 0:
		return "", errors.New("Echo VR was not found in the usual places; pass -dir <...\\ready-at-dawn-echo-arena\\bin\\win10>")
	case 1:
		return found[0], nil
	}
	return "", errors.New("more than one Echo VR install found; pass -dir with one of: " + strings.Join(found, "; "))
}

// openBackups are the probe backups under <nevr root>\installer that no restore has finished, by name. One with no manifest
// replaced nothing.
func openBackups(nevrRoot string) ([]string, error) {
	entries, err := os.ReadDir(filepath.Join(nevrRoot, InstallerDir))
	if errors.Is(err, fs.ErrNotExist) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	var names []string
	for _, e := range entries {
		if !e.IsDir() || !strings.HasPrefix(e.Name(), BackupPrefix) {
			continue
		}
		dir := filepath.Join(nevrRoot, InstallerDir, e.Name())
		if _, err := os.Stat(filepath.Join(dir, ManifestName)); err != nil {
			continue
		}
		if _, err := os.Stat(filepath.Join(dir, RestoredMarker)); err == nil {
			continue
		}
		names = append(names, e.Name())
	}
	sort.Strings(names)
	return names, nil
}

// ---- package ----

// PackageOptions say what Package packs.
type PackageOptions struct {
	// BuildDir is the probe build's directory (build/mingw-map-probes): its CMakeCache.txt and bin/.
	BuildDir string
	// OutDir is where nevr-probes-<date>/ and its zip are made; BuildDir when empty.
	OutDir string
	// ToolPath is the windows maps-probe.exe to put in the package; the package has no installer without it.
	ToolPath string
	Now      func() time.Time
}

func cacheHasOn(cache []byte, option string) bool {
	sc := bufio.NewScanner(bytes.NewReader(cache))
	want := option + ":BOOL=ON"
	for sc.Scan() {
		if strings.TrimSpace(sc.Text()) == want {
			return true
		}
	}
	return false
}

// Package checks a probe build and writes nevr-probes-<date>.zip: nevr-runtime.dll, BugSplat64.dll, maps_probe.yaml,
// PROBE_SHA256SUMS and the installer. The build must be the dev one: a release host refuses the unsigned probe runtime, and the game
// then stops (ADR 0012). It returns the zip's path.
func Package(o PackageOptions) (string, error) {
	if o.Now == nil {
		o.Now = time.Now
	}
	out := o.OutDir
	if out == "" {
		out = o.BuildDir
	}
	cache, err := os.ReadFile(filepath.Join(o.BuildDir, "CMakeCache.txt"))
	if err != nil {
		return "", fmt.Errorf("no CMakeCache.txt in %s", o.BuildDir)
	}
	for _, option := range probeCacheEntries {
		if !cacheHasOn(cache, option) {
			return "", fmt.Errorf("%s is not a probe build: %s is not ON (build the mingw-map-probes preset)", o.BuildDir, option)
		}
	}
	if o.ToolPath == "" {
		return "", errors.New("-tool is required: the windows maps-probe.exe that installs the package")
	}
	if _, err := os.Stat(o.ToolPath); err != nil {
		return "", fmt.Errorf("the installer %s does not exist", o.ToolPath)
	}
	runtimeDll := filepath.Join(o.BuildDir, "bin", "nevr-runtime.dll")
	hostDll := filepath.Join(o.BuildDir, "bin", "BugSplat64.dll")
	for _, c := range []struct{ path, literal, what string }{
		{hostDll, DevHostLiteral, "a dev host (" + DevHostLiteral + ")"},
		{runtimeDll, DevRuntimeLit, "a dev runtime (" + DevRuntimeLit + ")"},
	} {
		has, err := HasLiteral(c.path, c.literal)
		if err != nil {
			return "", fmt.Errorf("%s cannot be read: %v", c.path, err)
		}
		if !has {
			return "", fmt.Errorf("%s is not %s", filepath.Base(c.path), c.what)
		}
	}
	runtimeProbe, err := HasLiteral(runtimeDll, "[NEVR.PROBE]")
	if err != nil {
		return "", err
	}
	if !runtimeProbe {
		return "", errors.New("nevr-runtime.dll carries no map probe ([NEVR.PROBE]): it was not built with NEVR_MAP_PROBES")
	}

	stage := filepath.Join(out, "nevr-probes-"+o.Now().Format(PackageDateFmt))
	if err := os.RemoveAll(stage); err != nil {
		return "", err
	}
	if err := os.MkdirAll(stage, dirMode); err != nil {
		return "", err
	}
	sources := map[string]string{"nevr-runtime.dll": runtimeDll, "BugSplat64.dll": hostDll}
	for name, from := range sources {
		data, err := os.ReadFile(from)
		if err != nil {
			return "", err
		}
		if err := os.WriteFile(filepath.Join(stage, name), data, fileMode); err != nil {
			return "", err
		}
	}
	if err := os.WriteFile(filepath.Join(stage, "maps_probe.yaml"), ProbeConfig(), fileMode); err != nil {
		return "", err
	}
	tool, err := os.ReadFile(o.ToolPath)
	if err != nil {
		return "", err
	}
	if err := os.WriteFile(filepath.Join(stage, ToolName), tool, fileMode); err != nil {
		return "", err
	}
	var sums strings.Builder
	for _, f := range Files {
		sha, err := Sha(filepath.Join(stage, f.Name))
		if err != nil {
			return "", err
		}
		fmt.Fprintf(&sums, "%s  %s\n", sha, f.Name)
	}
	if err := os.WriteFile(filepath.Join(stage, SumsFile), []byte(sums.String()), fileMode); err != nil {
		return "", err
	}
	zipPath := stage + ".zip"
	if err := zipDir(stage, zipPath); err != nil {
		return "", err
	}
	return zipPath, nil
}

func zipDir(dir, zipPath string) (err error) {
	out, err := os.Create(zipPath)
	if err != nil {
		return err
	}
	defer func() {
		if cerr := out.Close(); err == nil {
			err = cerr
		}
	}()
	zw := zip.NewWriter(out)
	entries, err := os.ReadDir(dir)
	if err != nil {
		return err
	}
	for _, e := range entries {
		data, err := os.ReadFile(filepath.Join(dir, e.Name()))
		if err != nil {
			return err
		}
		w, err := zw.Create(e.Name())
		if err != nil {
			return err
		}
		if _, err := w.Write(data); err != nil {
			return err
		}
	}
	return zw.Close()
}

// ---- install ----

// Options are the inputs of Install and Restore.
type Options struct {
	// Package is the folder that holds the package's files (Install).
	Package string
	// Root is -root; LocalAppData the %LOCALAPPDATA% the default root derives from.
	Root         string
	LocalAppData string
	// Dir is -dir; LibraryRoots are the places a game folder is looked for when it is empty.
	Dir          string
	LibraryRoots []string
	// Backup names the backup to restore when more than one is open (Restore).
	Backup string
	// Force makes Restore put back or remove a DLL that matches neither digest.
	Force bool
	// WhatIf prints the plan and writes nothing.
	WhatIf bool
	// GameRunning reports whether echovr.exe runs; a run refuses while it does.
	GameRunning func() (bool, error)
	Now         func() time.Time
	Out         io.Writer
}

func (o *Options) defaults() {
	if o.Now == nil {
		o.Now = time.Now
	}
	if o.Out == nil {
		o.Out = io.Discard
	}
	if o.GameRunning == nil {
		o.GameRunning = func() (bool, error) { return false, nil }
	}
}

func (o *Options) assertGameStopped() error {
	running, err := o.GameRunning()
	if err != nil {
		return fmt.Errorf("could not tell whether echovr.exe is running: %v. Nothing was changed", err)
	}
	if running {
		return errors.New("echovr.exe is running; close the game (and any local server) first. Nothing was changed")
	}
	return nil
}

// A line of the sums file is a SHA-256 hex digest (64 digits), a space, an optional `*` (binary mode) and a name.
const (
	sumsDigestLen  = 64
	sumsMinNameLen = 1
)

func readSums(path string) (map[string]string, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("%s is not next to the installer", SumsFile)
	}
	wanted := map[string]string{}
	for _, line := range strings.Split(string(data), "\n") {
		line = strings.TrimRight(line, "\r")
		if len(line) < sumsDigestLen+sumsMinNameLen+1 || line[sumsDigestLen] != ' ' {
			continue
		}
		digest, name := line[:sumsDigestLen], strings.TrimLeft(line[sumsDigestLen:], " *")
		if _, err := hex.DecodeString(digest); err != nil || digest != strings.ToLower(digest) || name == "" {
			continue
		}
		wanted[name] = digest
	}
	return wanted, nil
}

type installStep struct {
	file      File
	target    string
	before    string
	installed string
	source    string
	backup    string
}

// Install puts the package over an existing nEVR install, after backing up each file it replaces. It returns an error whose text
// says what to do; manifestWritten tells the caller to point at Restore.
func Install(o Options) (manifestWritten bool, err error) {
	o.defaults()
	if err := o.assertGameStopped(); err != nil {
		return false, err
	}
	wanted, err := readSums(filepath.Join(o.Package, SumsFile))
	if err != nil {
		return false, err
	}
	for _, f := range Files {
		digest, ok := wanted[f.Name]
		if !ok {
			return false, fmt.Errorf("%s has no entry for %s", SumsFile, f.Name)
		}
		got, err := Sha(filepath.Join(o.Package, f.Name))
		if err != nil {
			return false, err
		}
		if got != digest {
			return false, fmt.Errorf("%s in the package does not match %s: the package is damaged", f.Name, SumsFile)
		}
	}
	if has, _ := HasLiteral(filepath.Join(o.Package, "BugSplat64.dll"), DevHostLiteral); !has {
		return false, errors.New("the package BugSplat64.dll is not a dev host; it would refuse the probe runtime. Ask for a new package")
	}
	if has, _ := HasLiteral(filepath.Join(o.Package, "nevr-runtime.dll"), DevRuntimeLit); !has {
		return false, errors.New("the package nevr-runtime.dll is not a dev build. Ask for a new package")
	}

	nevrRoot, err := ResolveRoot(o.Root, o.LocalAppData)
	if err != nil {
		return false, err
	}
	if !IsNevrBinary(filepath.Join(nevrRoot, "nevr-runtime.dll")) {
		return false, errors.New("no nEVR runtime in the nevr folder: this is not a nevr install. Install the nevr release with its install.ps1 first")
	}
	gameDir, err := FindGameDir(o.Dir, o.LibraryRoots)
	if err != nil {
		return false, err
	}
	if !IsNevrBinary(filepath.Join(gameDir, "BugSplat64.dll")) {
		return false, errors.New("the game folder BugSplat64.dll is not a nEVR host: nevr is not installed in this game. Install the nevr release with its install.ps1 first")
	}
	open, err := openBackups(nevrRoot)
	if err != nil {
		return false, err
	}
	if len(open) > 0 {
		return false, fmt.Errorf(`a probe install is already in place (nevr folder\installer\%s); run restore first. Nothing was changed`, open[0])
	}

	var plan []installStep
	for _, f := range Files {
		dir, err := placeDir(f.Place, nevrRoot, gameDir)
		if err != nil {
			return false, err
		}
		target := filepath.Join(dir, f.Name)
		before, err := Sha(target)
		if err != nil {
			return false, err
		}
		plan = append(plan, installStep{file: f, target: target, before: before, installed: wanted[f.Name],
			source: filepath.Join(o.Package, f.Name), backup: string(f.Place) + "-" + f.Name})
	}

	backupName := BackupPrefix + o.Now().Format(BackupTimeLayout)
	backupDir := filepath.Join(nevrRoot, InstallerDir, backupName)
	fmt.Fprintf(o.Out, "nevr folder: %s\n", nevrRoot)
	for _, s := range plan {
		shown := shownName(s.file.Place, s.file.Name)
		if s.before == Absent {
			fmt.Fprintf(o.Out, "  add      %s (none there now)\n", shown)
		} else {
			fmt.Fprintf(o.Out, "  replace  %s (backed up first)\n", shown)
		}
	}
	fmt.Fprintf(o.Out, "  backup   nevr folder\\installer\\%s\n", backupName)
	if o.WhatIf {
		fmt.Fprintln(o.Out, "-whatif: nothing was written.")
		return false, nil
	}

	if _, err := os.Stat(backupDir); err == nil {
		return false, fmt.Errorf(`nevr folder\installer\%s already exists; run again in a second`, backupName)
	}
	if err := os.MkdirAll(backupDir, dirMode); err != nil {
		return false, err
	}
	lines := []string{ManifestHeader, "game_dir\t" + gameDir}
	for _, s := range plan {
		if s.before != Absent {
			if err := copyVerified(s.target, filepath.Join(backupDir, s.backup), s.before); err != nil {
				return false, err
			}
		}
		lines = append(lines, fmt.Sprintf("file\t%s\t%s\t%s\t%s", s.file.Place, s.file.Name, s.before, s.installed))
	}
	// The manifest goes in whole before any target is written, so a run that stops partway can be restored.
	manifest := filepath.Join(backupDir, ManifestName)
	if err := os.WriteFile(manifest+".tmp", []byte(strings.Join(lines, "\n")+"\n"), fileMode); err != nil {
		return false, err
	}
	if err := os.Rename(manifest+".tmp", manifest); err != nil {
		return false, err
	}
	for _, s := range plan {
		if err := copyVerified(s.source, s.target, s.installed); err != nil {
			return true, err
		}
		fmt.Fprintf(o.Out, "installed %s\n", shownName(s.file.Place, s.file.Name))
	}
	fmt.Fprintf(o.Out, "Probe build installed. Backup and manifest: nevr folder\\installer\\%s\n", backupName)
	return true, nil
}

// ---- restore ----

type restoreStep struct {
	shown   string
	target  string
	saved   string
	before  string
	action  string
	partial string
}

// Restore undoes Install from its manifest. Every check comes first and a refusal changes nothing. Each backup copy must match its
// recorded SHA-256, and each DLL must still be the probe file that was installed, or already the original. A file that was absent
// before the install is removed only while it is unchanged. An edited maps_probe.yaml is left in place, because release builds never
// read it. Force also puts back (or removes) a DLL that matches neither digest. A <file>.probe-partial left by a copy that stopped
// is removed. The backup folder is kept and marked restored.txt.
func Restore(o Options) error {
	o.defaults()
	if err := o.assertGameStopped(); err != nil {
		return err
	}
	nevrRoot, err := ResolveRoot(o.Root, o.LocalAppData)
	if err != nil {
		return err
	}
	open, err := openBackups(nevrRoot)
	if err != nil {
		return err
	}
	if o.Backup != "" {
		var named []string
		for _, n := range open {
			if strings.EqualFold(n, o.Backup) {
				named = append(named, n)
			}
		}
		if len(named) == 0 {
			return fmt.Errorf(`no unrestored probe backup named %s in the nevr folder\installer`, o.Backup)
		}
		open = named
	}
	if len(open) == 0 {
		return errors.New(`no unrestored probe backup in the nevr folder\installer: nothing to restore`)
	}
	if len(open) > 1 {
		return fmt.Errorf("more than one unrestored probe backup; pass -backup with one of: %s", strings.Join(open, ", "))
	}
	backupName := open[0]
	backupDir := filepath.Join(nevrRoot, InstallerDir, backupName)

	data, err := os.ReadFile(filepath.Join(backupDir, ManifestName))
	if err != nil {
		return err
	}
	lines := strings.Split(strings.TrimRight(string(data), "\r\n"), "\n")
	const headerLines = 2
	if len(lines) < headerLines || strings.TrimRight(lines[0], "\r") != ManifestHeader {
		return fmt.Errorf("%s\\%s is not a probe install manifest", backupName, ManifestName)
	}
	gameDir, ok := strings.CutPrefix(strings.TrimRight(lines[1], "\r"), "game_dir\t")
	if !ok || gameDir == "" {
		return fmt.Errorf("%s\\%s has no game_dir", backupName, ManifestName)
	}
	if _, err := os.Stat(filepath.Join(gameDir, GameExe)); err != nil {
		return errors.New("the game folder the probe install used has no echovr.exe any more. Nothing was changed")
	}

	known := map[string]File{}
	for _, f := range Files {
		known[string(f.Place)+"/"+f.Name] = f
	}
	var plan []restoreStep
	var refusals []string
	for _, line := range lines[headerLines:] {
		line = strings.TrimRight(line, "\r")
		if line == "" {
			continue
		}
		const manifestFields = 5
		fields := strings.Split(line, "\t")
		var file File
		if len(fields) == manifestFields && fields[0] == "file" {
			file, ok = known[fields[1]+"/"+fields[2]]
		} else {
			ok = false
		}
		if !ok {
			return fmt.Errorf("%s\\%s has a line this tool does not know; nothing was changed", backupName, ManifestName)
		}
		before, installed := fields[3], fields[4]
		shown := shownName(file.Place, file.Name)
		dir, err := placeDir(file.Place, nevrRoot, gameDir)
		if err != nil {
			return err
		}
		target := filepath.Join(dir, file.Name)
		saved := filepath.Join(backupDir, string(file.Place)+"-"+file.Name)
		current, err := Sha(target)
		if err != nil {
			return err
		}
		action := ""
		if before == Absent {
			switch {
			case current == Absent:
				action = "none"
			case current == installed:
				action = "remove"
			case file.Mutable:
				action = "leave"
			case o.Force:
				action = "remove"
			default:
				refusals = append(refusals, shown+" changed since the probe install (-force removes it)")
			}
		} else {
			savedSha, err := Sha(saved)
			if err != nil {
				return err
			}
			switch {
			case savedSha != before:
				refusals = append(refusals, fmt.Sprintf("the backup of %s is missing or damaged (%s)", shown, backupName))
			case current == before:
				action = "none"
			case current == installed || file.Mutable || o.Force:
				action = "put back"
			default:
				refusals = append(refusals, shown+" changed since the probe install (another installer ran, or a copy stopped partway? -force puts the backup back)")
			}
		}
		step := restoreStep{shown: shown, target: target, saved: saved, before: before, action: action}
		if _, err := os.Stat(target + PartialSuffix); err == nil {
			step.partial = target + PartialSuffix
		}
		plan = append(plan, step)
	}
	if len(refusals) > 0 {
		return errors.New(strings.Join(refusals, "; ") + ". Nothing was changed")
	}

	fmt.Fprintf(o.Out, "nevr folder: %s, backup %s\n", nevrRoot, backupName)
	for _, s := range plan {
		switch s.action {
		case "put back":
			fmt.Fprintf(o.Out, "  put back %s\n", s.shown)
		case "remove":
			fmt.Fprintf(o.Out, "  remove   %s (the probe install added it)\n", s.shown)
		case "leave":
			fmt.Fprintf(o.Out, "  leave    %s (added by the probe install and edited since; release builds ignore it)\n", s.shown)
		case "none":
			fmt.Fprintf(o.Out, "  ok       %s (already as before)\n", s.shown)
		}
		if s.partial != "" {
			fmt.Fprintf(o.Out, "  clean    %s%s (left by a copy that stopped)\n", s.shown, PartialSuffix)
		}
	}
	if o.WhatIf {
		fmt.Fprintln(o.Out, "-whatif: nothing was written.")
		return nil
	}
	for _, s := range plan {
		if s.partial != "" {
			if err := os.Remove(s.partial); err != nil {
				return err
			}
		}
		switch s.action {
		case "put back":
			if err := copyVerified(s.saved, s.target, s.before); err != nil {
				return err
			}
		case "remove":
			if err := os.Remove(s.target); err != nil {
				return err
			}
		}
	}
	marker := o.Now().Format("2006-01-02 15:04:05") + "\r\n"
	if err := os.WriteFile(filepath.Join(backupDir, RestoredMarker), []byte(marker), fileMode); err != nil {
		return err
	}
	fmt.Fprintf(o.Out, "Restored. The backup stays in nevr folder\\installer\\%s.\n", backupName)
	return nil
}
