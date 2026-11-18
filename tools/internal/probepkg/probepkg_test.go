package probepkg

import (
	"archive/zip"
	"bytes"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// Named contents for the fixture's files. A DLL is its literal and a name, so two of them never share a digest.
const (
	hostBuild       = "MZ host NEVR-HOST-BUILD 5.0.0 abcdef"
	runtimeBuild    = "MZ runtime NEVR-BUILD 5.0.0 abcdef"
	probeHost       = "MZ probe host NEVR-HOST-BUILD dev"
	probeRuntime    = "MZ probe runtime NEVR-PLUGIN-TRUST dev [NEVR.PROBE] NEVR-BUILD dev"
	probeYaml       = "rules: off\n"
	editedYaml      = "rules: on\n"
	installedAt     = "2026-11-18 10:00:00"
	sumsLineFormat  = "%s  %s\n"
	someoneElsesDll = "MZ another installer NEVR-BUILD 9"
)

var fixedNow = time.Date(2026, 11, 18, 10, 0, 0, 0, time.UTC)

type rig struct {
	t       *testing.T
	pkg     string
	root    string // the nEVR folder
	game    string // the game folder
	running bool
	out     *bytes.Buffer
}

func write(t *testing.T, path, content string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
}

func read(t *testing.T, path string) string {
	t.Helper()
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	return string(b)
}

func exists(path string) bool {
	_, err := os.Stat(path)
	return err == nil
}

func newRig(t *testing.T) *rig {
	t.Helper()
	base := t.TempDir()
	r := &rig{t: t, pkg: filepath.Join(base, "pkg"), root: filepath.Join(base, "nevr"), game: filepath.Join(base, "game", "bin", "win10"), out: &bytes.Buffer{}}
	write(t, filepath.Join(r.root, "nevr-runtime.dll"), runtimeBuild)
	write(t, filepath.Join(r.game, "BugSplat64.dll"), hostBuild)
	write(t, filepath.Join(r.game, GameExe), "MZ game")
	r.writePackage(probeRuntime, probeHost, probeYaml)
	return r
}

// writePackage makes the package folder, with a PROBE_SHA256SUMS that matches it.
func (r *rig) writePackage(runtimeDll, hostDll, yaml string) {
	r.t.Helper()
	write(r.t, filepath.Join(r.pkg, "nevr-runtime.dll"), runtimeDll)
	write(r.t, filepath.Join(r.pkg, "BugSplat64.dll"), hostDll)
	write(r.t, filepath.Join(r.pkg, "maps_probe.yaml"), yaml)
	var sums strings.Builder
	for _, f := range Files {
		sha, err := Sha(filepath.Join(r.pkg, f.Name))
		if err != nil {
			r.t.Fatal(err)
		}
		sums.WriteString(sha + "  " + f.Name + "\n")
	}
	write(r.t, filepath.Join(r.pkg, SumsFile), sums.String())
}

func (r *rig) options() Options {
	r.out.Reset()
	return Options{Package: r.pkg, Root: r.root, Dir: r.game, Out: r.out, Now: func() time.Time { return fixedNow },
		GameRunning: func() (bool, error) { return r.running, nil }}
}

func (r *rig) install() {
	r.t.Helper()
	if _, err := Install(r.options()); err != nil {
		r.t.Fatalf("install: %v", err)
	}
}

func (r *rig) backupDir() string {
	return filepath.Join(r.root, InstallerDir, BackupPrefix+fixedNow.Format(BackupTimeLayout))
}

func (r *rig) paths() (rt, host, yaml string) {
	return filepath.Join(r.root, "nevr-runtime.dll"), filepath.Join(r.game, "BugSplat64.dll"), filepath.Join(r.root, "maps_probe.yaml")
}

func wantErr(t *testing.T, err error, substr string) {
	t.Helper()
	if err == nil {
		t.Fatalf("want an error containing %q, got none", substr)
	}
	if !strings.Contains(err.Error(), substr) {
		t.Fatalf("want an error containing %q, got %q", substr, err.Error())
	}
}

// ---- install ----

func TestInstallReplacesTheThreeFilesAndBacksUpTheTwoThatExisted(t *testing.T) {
	r := newRig(t)
	rt, host, yaml := r.paths()
	written, err := Install(r.options())
	if err != nil || !written {
		t.Fatalf("install = %v, %v", written, err)
	}
	if read(t, rt) != probeRuntime || read(t, host) != probeHost || read(t, yaml) != probeYaml {
		t.Fatal("the probe files are not in place")
	}
	if read(t, filepath.Join(r.backupDir(), "root-nevr-runtime.dll")) != runtimeBuild ||
		read(t, filepath.Join(r.backupDir(), "game-BugSplat64.dll")) != hostBuild {
		t.Fatal("the originals are not backed up")
	}
	if exists(filepath.Join(r.backupDir(), "root-maps_probe.yaml")) {
		t.Fatal("a file that was absent has a backup")
	}
	manifest := read(t, filepath.Join(r.backupDir(), ManifestName))
	if !strings.HasPrefix(manifest, ManifestHeader+"\ngame_dir\t"+r.game+"\n") || !strings.Contains(manifest, "file\troot\tmaps_probe.yaml\tabsent\t") {
		t.Fatalf("manifest = %q", manifest)
	}
	for _, p := range []string{rt, host, yaml} {
		if exists(p + PartialSuffix) {
			t.Fatalf("%s left a partial", p)
		}
	}
	if !strings.Contains(r.out.String(), "installed game folder\\BugSplat64.dll") {
		t.Fatalf("out = %q", r.out.String())
	}
}

func TestInstallWhatIfWritesNothing(t *testing.T) {
	r := newRig(t)
	o := r.options()
	o.WhatIf = true
	written, err := Install(o)
	if err != nil || written {
		t.Fatalf("whatif = %v, %v", written, err)
	}
	rt, host, yaml := r.paths()
	if read(t, rt) != runtimeBuild || read(t, host) != hostBuild || exists(yaml) || exists(filepath.Join(r.root, InstallerDir)) {
		t.Fatal("a -whatif run wrote something")
	}
	if !strings.Contains(r.out.String(), "-whatif: nothing was written.") {
		t.Fatalf("out = %q", r.out.String())
	}
}

func TestInstallRefusesWhileTheGameRuns(t *testing.T) {
	r := newRig(t)
	r.running = true
	_, err := Install(r.options())
	wantErr(t, err, "echovr.exe is running")
	if read(t, filepath.Join(r.root, "nevr-runtime.dll")) != runtimeBuild {
		t.Fatal("a refused install changed a file")
	}
}

func TestInstallRefusesWhenTheGameCannotBeAsked(t *testing.T) {
	r := newRig(t)
	o := r.options()
	o.GameRunning = func() (bool, error) { return false, errors.New("no tasklist") }
	_, err := Install(o)
	wantErr(t, err, "could not tell whether echovr.exe is running")
}

func TestInstallRefusesADamagedPackage(t *testing.T) {
	r := newRig(t)
	write(t, filepath.Join(r.pkg, "nevr-runtime.dll"), probeRuntime+"x")
	_, err := Install(r.options())
	wantErr(t, err, "does not match "+SumsFile)
}

func TestInstallRefusesAPackageWithoutItsSums(t *testing.T) {
	r := newRig(t)
	if err := os.Remove(filepath.Join(r.pkg, SumsFile)); err != nil {
		t.Fatal(err)
	}
	_, err := Install(r.options())
	wantErr(t, err, SumsFile+" is not next to the installer")
}

func TestInstallRefusesASumsFileThatLacksAFile(t *testing.T) {
	r := newRig(t)
	sums := read(t, filepath.Join(r.pkg, SumsFile))
	var kept []string
	for _, l := range strings.Split(sums, "\n") {
		if !strings.Contains(l, "maps_probe.yaml") {
			kept = append(kept, l)
		}
	}
	write(t, filepath.Join(r.pkg, SumsFile), strings.Join(kept, "\n"))
	_, err := Install(r.options())
	wantErr(t, err, "has no entry for maps_probe.yaml")
}

func TestInstallRefusesAReleaseHostAndAReleaseRuntime(t *testing.T) {
	r := newRig(t)
	r.writePackage(probeRuntime, hostBuild, probeYaml)
	_, err := Install(r.options())
	wantErr(t, err, "is not a dev host")
	r.writePackage(runtimeBuild, probeHost, probeYaml)
	_, err = Install(r.options())
	wantErr(t, err, "is not a dev build")
}

func TestInstallRefusesAFolderThatIsNoNevrInstall(t *testing.T) {
	r := newRig(t)
	write(t, filepath.Join(r.root, "nevr-runtime.dll"), "MZ something else")
	_, err := Install(r.options())
	wantErr(t, err, "this is not a nevr install")
	r = newRig(t)
	write(t, filepath.Join(r.game, "BugSplat64.dll"), "MZ the original crash reporter")
	_, err = Install(r.options())
	wantErr(t, err, "is not a nEVR host")
}

func TestInstallRefusesASecondInstallAndAllowsOneAfterRestore(t *testing.T) {
	r := newRig(t)
	r.install()
	_, err := Install(r.options())
	wantErr(t, err, "a probe install is already in place")
	if err := Restore(r.options()); err != nil {
		t.Fatal(err)
	}
	o := r.options()
	o.Now = func() time.Time { return fixedNow.Add(time.Second) }
	if _, err := Install(o); err != nil {
		t.Fatalf("an install after a restore: %v", err)
	}
}

func TestInstallARelativeRootOrDirIsRefused(t *testing.T) {
	r := newRig(t)
	o := r.options()
	o.Root = "relative"
	_, err := Install(o)
	wantErr(t, err, "-root must be an absolute path")
	o = r.options()
	o.Dir = "relative"
	_, err = Install(o)
	wantErr(t, err, "-dir must be an absolute path")
}

func TestInstallWithoutARootUsesLocalAppData(t *testing.T) {
	r := newRig(t)
	base := filepath.Dir(r.root)
	local := filepath.Join(base, "local")
	write(t, filepath.Join(local, "nevr", "echovr", "nevr-runtime.dll"), runtimeBuild)
	o := r.options()
	o.Root = ""
	o.LocalAppData = local
	if _, err := Install(o); err != nil {
		t.Fatal(err)
	}
	if read(t, filepath.Join(local, "nevr", "echovr", "nevr-runtime.dll")) != probeRuntime {
		t.Fatal("the LOCALAPPDATA root was not used")
	}
	o.LocalAppData = ""
	_, err := Install(o)
	wantErr(t, err, "LOCALAPPDATA is not set")
}

func TestFindGameDir(t *testing.T) {
	base := t.TempDir()
	one := filepath.Join(base, "lib1")
	two := filepath.Join(base, "lib2")
	write(t, filepath.Join(one, LibrarySuffixes[1], GameExe), "x")
	if got, err := FindGameDir("", []string{one, filepath.Join(base, "none")}); err != nil || got != filepath.Join(one, LibrarySuffixes[1]) {
		t.Fatalf("one install = %q, %v", got, err)
	}
	write(t, filepath.Join(two, LibrarySuffixes[2], GameExe), "x")
	_, err := FindGameDir("", []string{one, two})
	wantErr(t, err, "more than one Echo VR install found")
	_, err = FindGameDir("", []string{filepath.Join(base, "none")})
	wantErr(t, err, "was not found in the usual places")
	_, err = FindGameDir(filepath.Join(base, "empty"), nil)
	wantErr(t, err, "no echovr.exe in the -dir folder")
	// The same install named twice is one install.
	if _, err := FindGameDir("", []string{one, one}); err != nil {
		t.Fatalf("a duplicated root: %v", err)
	}
}

func TestInstallFromAnEmptyBackupNameCollisionIsRefused(t *testing.T) {
	r := newRig(t)
	if err := os.MkdirAll(r.backupDir(), 0o755); err != nil {
		t.Fatal(err)
	}
	_, err := Install(r.options())
	wantErr(t, err, "already exists; run again in a second")
}

// ---- restore ----

func TestRestorePutsTheOriginalsBackAndRemovesTheFileThatWasAdded(t *testing.T) {
	r := newRig(t)
	r.install()
	rt, host, yaml := r.paths()
	if err := Restore(r.options()); err != nil {
		t.Fatal(err)
	}
	if read(t, rt) != runtimeBuild || read(t, host) != hostBuild {
		t.Fatal("the originals are not back")
	}
	if exists(yaml) {
		t.Fatal("a file the install added is still there")
	}
	if !exists(filepath.Join(r.backupDir(), RestoredMarker)) {
		t.Fatal("the backup is not marked restored")
	}
	if got := read(t, filepath.Join(r.backupDir(), RestoredMarker)); got != installedAt+"\r\n" {
		t.Fatalf("marker = %q", got)
	}
	if !exists(filepath.Join(r.backupDir(), ManifestName)) {
		t.Fatal("the backup was not kept")
	}
	err := Restore(r.options())
	wantErr(t, err, "nothing to restore")
}

func TestRestoreLeavesAnEditedConfigAndPutsBackTheDlls(t *testing.T) {
	r := newRig(t)
	r.install()
	_, _, yaml := r.paths()
	write(t, yaml, editedYaml)
	if err := Restore(r.options()); err != nil {
		t.Fatal(err)
	}
	if read(t, yaml) != editedYaml {
		t.Fatal("an edited maps_probe.yaml was not left")
	}
	if !strings.Contains(r.out.String(), "leave    nevr folder\\maps_probe.yaml") {
		t.Fatalf("out = %q", r.out.String())
	}
}

func TestRestoreRefusesADllAnotherInstallerChangedAndForcePutsTheBackupBack(t *testing.T) {
	r := newRig(t)
	r.install()
	rt, _, _ := r.paths()
	write(t, rt, someoneElsesDll)
	err := Restore(r.options())
	wantErr(t, err, "changed since the probe install")
	wantErr(t, err, "Nothing was changed")
	if read(t, rt) != someoneElsesDll {
		t.Fatal("a refused restore changed a file")
	}
	o := r.options()
	o.Force = true
	if err := Restore(o); err != nil {
		t.Fatal(err)
	}
	if read(t, rt) != runtimeBuild {
		t.Fatal("-force did not put the backup back")
	}
}

func TestRestoreRefusesADamagedBackup(t *testing.T) {
	r := newRig(t)
	r.install()
	write(t, filepath.Join(r.backupDir(), "root-nevr-runtime.dll"), "MZ damaged")
	err := Restore(r.options())
	wantErr(t, err, "is missing or damaged")
	rt, _, _ := r.paths()
	if read(t, rt) != probeRuntime {
		t.Fatal("a refused restore changed a file")
	}
}

func TestRestoreRemovesAPartialLeftByAStoppedCopy(t *testing.T) {
	r := newRig(t)
	r.install()
	_, host, yaml := r.paths()
	write(t, host+PartialSuffix, "half") // its target is put back, which overwrites the partial anyway
	write(t, yaml+PartialSuffix, "half") // its target is removed, so only the cleanup removes the partial
	if err := Restore(r.options()); err != nil {
		t.Fatal(err)
	}
	if exists(host+PartialSuffix) || exists(yaml+PartialSuffix) {
		t.Fatal("a partial is still there")
	}
	if !strings.Contains(r.out.String(), "clean    nevr folder\\maps_probe.yaml"+PartialSuffix) {
		t.Fatalf("out = %q", r.out.String())
	}
}

func TestRestoreAfterAnInstallThatStoppedAfterTheManifest(t *testing.T) {
	// The manifest names the digests the install meant to write; a target still at its original reads "already as before".
	r := newRig(t)
	r.install()
	rt, host, _ := r.paths()
	write(t, rt, runtimeBuild)
	if err := Restore(r.options()); err != nil {
		t.Fatal(err)
	}
	if read(t, rt) != runtimeBuild || read(t, host) != hostBuild || !strings.Contains(r.out.String(), "ok       nevr folder\\nevr-runtime.dll") {
		t.Fatalf("out = %q", r.out.String())
	}
}

func TestRestoreWhatIfWritesNothing(t *testing.T) {
	r := newRig(t)
	r.install()
	o := r.options()
	o.WhatIf = true
	if err := Restore(o); err != nil {
		t.Fatal(err)
	}
	rt, _, _ := r.paths()
	if read(t, rt) != probeRuntime || exists(filepath.Join(r.backupDir(), RestoredMarker)) {
		t.Fatal("a -whatif restore wrote something")
	}
}

func TestRestoreRefusesWhileTheGameRunsAndWithoutABackup(t *testing.T) {
	r := newRig(t)
	wantErr(t, Restore(r.options()), "nothing to restore")
	r.install()
	r.running = true
	wantErr(t, Restore(r.options()), "echovr.exe is running")
}

func TestRestoreWithTwoOpenBackupsNeedsTheName(t *testing.T) {
	r := newRig(t)
	r.install()
	second := filepath.Join(r.root, InstallerDir, BackupPrefix+"20260101-000000")
	write(t, filepath.Join(second, ManifestName), ManifestHeader+"\n")
	err := Restore(r.options())
	wantErr(t, err, "more than one unrestored probe backup")
	o := r.options()
	o.Backup = "no-such"
	wantErr(t, Restore(o), "no unrestored probe backup named no-such")
	o = r.options()
	o.Backup = strings.ToUpper(filepath.Base(r.backupDir()))
	if err := Restore(o); err != nil {
		t.Fatalf("a backup named by another case: %v", err)
	}
}

func TestRestoreRefusesAManifestItDoesNotKnow(t *testing.T) {
	r := newRig(t)
	r.install()
	manifest := filepath.Join(r.backupDir(), ManifestName)
	write(t, manifest, read(t, manifest)+"file\troot\tconfig.yaml\tabsent\tabsent\n")
	wantErr(t, Restore(r.options()), "has a line this tool does not know")
	write(t, manifest, "not a manifest\n")
	wantErr(t, Restore(r.options()), "is not a probe install manifest")
	write(t, manifest, ManifestHeader+"\nno game dir\n")
	wantErr(t, Restore(r.options()), "has no game_dir")
}

func TestRestoreRefusesWhenTheGameFolderIsGone(t *testing.T) {
	r := newRig(t)
	r.install()
	if err := os.Remove(filepath.Join(r.game, GameExe)); err != nil {
		t.Fatal(err)
	}
	wantErr(t, Restore(r.options()), "has no echovr.exe any more")
}

func TestRestoreForceRemovesAChangedFileThatWasAbsentBefore(t *testing.T) {
	r := newRig(t)
	_, host, _ := r.paths()
	r.install()
	// Absent-before applies to maps_probe.yaml only, which is Mutable and so is left; the DLLs existed before.
	_, _, yaml := r.paths()
	write(t, yaml, editedYaml)
	o := r.options()
	o.Force = true
	if err := Restore(o); err != nil {
		t.Fatal(err)
	}
	if read(t, yaml) != editedYaml || read(t, host) != hostBuild {
		t.Fatal("restore -force changed the wrong files")
	}
}

// ---- package ----

func probeBuild(t *testing.T, cache string, runtimeDll, hostDll string) string {
	t.Helper()
	dir := t.TempDir()
	write(t, filepath.Join(dir, "CMakeCache.txt"), cache)
	write(t, filepath.Join(dir, "bin", "nevr-runtime.dll"), runtimeDll)
	write(t, filepath.Join(dir, "bin", "BugSplat64.dll"), hostDll)
	write(t, filepath.Join(dir, "maps-probe.exe"), "MZ tool")
	return dir
}

const probeCache = "CMAKE_BUILD_TYPE:STRING=Release\nNEVR_MAP_PROBES:BOOL=ON\nNEVR_ALLOW_UNSIGNED_PLUGINS:BOOL=ON\n"

func packageOptions(dir string) PackageOptions {
	return PackageOptions{BuildDir: dir, OutDir: filepath.Join(dir, "out"), ToolPath: filepath.Join(dir, "maps-probe.exe"),
		Now: func() time.Time { return fixedNow }}
}

func TestPackageWritesTheZipWithMatchingSums(t *testing.T) {
	dir := probeBuild(t, probeCache, probeRuntime, probeHost)
	zipPath, err := Package(packageOptions(dir))
	if err != nil {
		t.Fatal(err)
	}
	if filepath.Base(zipPath) != "nevr-probes-20261118.zip" {
		t.Fatalf("zip = %s", zipPath)
	}
	zr, err := zip.OpenReader(zipPath)
	if err != nil {
		t.Fatal(err)
	}
	defer zr.Close()
	got := map[string]string{}
	for _, f := range zr.File {
		rc, err := f.Open()
		if err != nil {
			t.Fatal(err)
		}
		var buf bytes.Buffer
		if _, err := buf.ReadFrom(rc); err != nil {
			t.Fatal(err)
		}
		rc.Close()
		got[f.Name] = buf.String()
	}
	for _, name := range []string{"nevr-runtime.dll", "BugSplat64.dll", "maps_probe.yaml", SumsFile, ToolName} {
		if _, ok := got[name]; !ok {
			t.Fatalf("the zip has no %s: %v", name, got)
		}
	}
	if got["nevr-runtime.dll"] != probeRuntime || got["BugSplat64.dll"] != probeHost || got[ToolName] != "MZ tool" {
		t.Fatal("the zip holds other bytes than the build's")
	}
	if !strings.Contains(got["maps_probe.yaml"], "rules: off") || strings.Contains(got["maps_probe.yaml"], "rules: on") {
		t.Fatal("the packaged config does not have every probe off")
	}
	// The package installs: its sums match its files.
	pkg := filepath.Join(dir, "out", "nevr-probes-20261118")
	r := newRig(t)
	r.pkg = pkg
	r.install()
}

func TestPackageRefusesWhatIsNotAProbeBuild(t *testing.T) {
	for name, c := range map[string]struct{ cache, runtimeDll, hostDll, want string }{
		"no probes option":     {"NEVR_ALLOW_UNSIGNED_PLUGINS:BOOL=ON\n", probeRuntime, probeHost, "NEVR_MAP_PROBES is not ON"},
		"not the dev build":    {"NEVR_MAP_PROBES:BOOL=ON\n", probeRuntime, probeHost, "NEVR_ALLOW_UNSIGNED_PLUGINS is not ON"},
		"a release host":       {probeCache, probeRuntime, hostBuild, "is not a dev host"},
		"a release runtime":    {probeCache, runtimeBuild, probeHost, "is not a dev runtime"},
		"a runtime w/o probes": {probeCache, "MZ NEVR-PLUGIN-TRUST dev", probeHost, "carries no map probe"},
	} {
		t.Run(name, func(t *testing.T) {
			dir := probeBuild(t, c.cache, c.runtimeDll, c.hostDll)
			_, err := Package(packageOptions(dir))
			wantErr(t, err, c.want)
		})
	}
}

func TestPackageNeedsABuildCacheAndATool(t *testing.T) {
	_, err := Package(PackageOptions{BuildDir: t.TempDir()})
	wantErr(t, err, "no CMakeCache.txt")
	dir := probeBuild(t, probeCache, probeRuntime, probeHost)
	o := packageOptions(dir)
	o.ToolPath = ""
	_, err = Package(o)
	wantErr(t, err, "-tool is required")
	o.ToolPath = filepath.Join(dir, "absent.exe")
	_, err = Package(o)
	wantErr(t, err, "does not exist")
}

func TestProbeConfigHasEveryProbeOff(t *testing.T) {
	for _, key := range []string{"rules", "skew", "sublevel"} {
		if !strings.Contains(string(ProbeConfig()), "\n"+key+": off") && !strings.HasPrefix(string(ProbeConfig()), key+": off") {
			t.Fatalf("maps_probe.yaml does not turn %s off", key)
		}
	}
}
