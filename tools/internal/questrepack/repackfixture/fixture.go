package repackfixture

import (
	"archive/zip"
	"bytes"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/testkit"
)

// Fixture is one run's inputs: a fixture key, a store-like APK, a compiled shim with its build info, and the stand-in
// patcher. Its files are under the test's temporary directory.
type Fixture struct {
	T       testkit.T
	Dir     string   // the temporary directory every input is under
	Env     []string // the environment the recipe runs in: the fixture key named, and testkit's hermetic environment
	APK     string   // the store-like APK
	Shim    string   // the compiled shim
	Patcher string   // the stand-in patcher's path
	Repo    string   // the checkout the recipe runs in
	Signer  string   // the fixture key's certificate SHA-256, lower case, no colons
	fake    *testkit.FakeCommand
}

// Passphrase and Alias are the fixture key's; the key is made here and never the real one.
const (
	passphrase = "fixture-pass-7f3a91"
	alias      = "nevr"
)

// storeVersionCode is the store APK's versionCode (the recipe repacks it).
const storeVersionCode = 4987566

// Tools are the commands the recipe needs; a test that needs the recipe skips when one is missing.
var Tools = []string{"just", "cc", "patchelf", "zip", "unzip", "zipalign", "apksigner", "aapt2", "readelf", "nm", "keytool"}

// MissingTools are the Tools not on PATH.
func MissingTools() []string {
	var missing []string
	for _, tool := range Tools {
		if _, err := exec.LookPath(tool); err != nil {
			missing = append(missing, tool)
		}
	}
	return missing
}

// RepoRoot is the checkout: the directory above tools/ that holds the justfile. It is found from the working directory.
func RepoRoot(t testkit.T) string {
	t.Helper()
	dir, err := os.Getwd()
	if err != nil {
		t.Fatalf("the working directory: %v", err)
	}
	for {
		if _, err := os.Stat(filepath.Join(dir, "justfile")); err == nil {
			if _, err := os.Stat(filepath.Join(dir, "tools", "go.mod")); err == nil {
				return dir
			}
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			t.Fatalf("no checkout (a directory with justfile and tools/go.mod) above the test's directory")
		}
		dir = parent
	}
}

// New makes a fixture: the key, the stand-in patcher in the given mode, and the recipe's environment. It skips the test
// when a tool the recipe needs is missing.
func New(t *testing.T, mode string) *Fixture {
	t.Helper()
	if missing := MissingTools(); len(missing) > 0 {
		t.Skipf("the recipe needs %s", strings.Join(missing, ", "))
	}
	dir := t.TempDir()
	ks := filepath.Join(dir, "fixture.jks")
	runTool(t, dir, "keytool", "-genkeypair", "-keystore", ks, "-alias", alias, "-keyalg", "RSA", "-keysize", "2048",
		"-validity", "30", "-storepass", passphrase, "-keypass", passphrase, "-dname", "CN=Quest Repack Test, O=test")
	listing := runTool(t, dir, "keytool", "-list", "-keystore", ks, "-alias", alias, "-storepass", passphrase)
	signer := ""
	for _, line := range strings.Split(listing, "\n") {
		if strings.Contains(line, "SHA-256") || strings.Contains(line, "SHA256") {
			parts := strings.SplitN(line, ":", 2)
			signer = strings.ToLower(strings.ReplaceAll(strings.TrimSpace(parts[1]), ":", ""))
			break
		}
	}
	if signer == "" {
		t.Fatalf("keytool listed no SHA-256 for the fixture key:\n%s", listing)
	}
	run := filepath.Join(dir, "run")
	if err := os.Mkdir(run, 0o700); err != nil {
		t.Fatalf("%v", err)
	}
	f := &Fixture{T: t, Dir: dir, Repo: RepoRoot(t), Signer: signer}
	f.Env = testkit.Env(t, "NEVR_QUEST_KEYSTORE="+ks, "NEVR_QUEST_KEYSTORE_PASS="+passphrase, "XDG_RUNTIME_DIR="+run)
	f.fake = testkit.Fake(t, mode)
	f.Patcher = filepath.Join(f.fake.Dir(), mode)
	return f
}

// Calls are the stand-in patcher's recorded runs, oldest first.
func (f *Fixture) Calls() []testkit.Record { return f.fake.Calls(f.T) }

// runTool runs a command in dir with the fixture's test environment and fails the test on a non-zero exit.
func runTool(t testkit.T, dir, name string, args ...string) string {
	t.Helper()
	cmd := exec.Command(name, args...)
	cmd.Dir = dir
	cmd.Env = testkit.Env(t)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("%s %v: %v\n%s", name, args, err, out)
	}
	return string(out)
}

// BuildLib compiles a shared library from source (C) at path; soname names it, and version (when given) is -DVERSION.
func BuildLib(t testkit.T, path, source, soname, version string) {
	t.Helper()
	src := strings.TrimSuffix(path, filepath.Ext(path)) + ".c"
	if err := os.WriteFile(src, []byte(source), 0o644); err != nil {
		t.Fatalf("%v", err)
	}
	args := []string{"-shared", "-fPIC", "-Wl,-soname," + soname, "-o", path, src}
	if version != "" {
		args = append([]string{`-DVERSION="` + version + `"`}, args...)
	}
	runTool(t, filepath.Dir(path), "cc", args...)
}

// BuildAArch64 compiles an AArch64 shared object from source at out, carrying the GNU build ID buildID when given (else
// none), and returns its bytes.
func BuildAArch64(t testkit.T, out, source, buildID string) []byte {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(out), 0o755); err != nil {
		t.Fatalf("%v", err)
	}
	src := out + ".c"
	if err := os.WriteFile(src, []byte(source), 0o644); err != nil {
		t.Fatalf("%v", err)
	}
	link := "-Wl,--build-id=none"
	if buildID != "" {
		link = "-Wl,--build-id=0x" + buildID
	}
	runTool(t, filepath.Dir(out), "clang", "--target=aarch64-linux-gnu", "-shared", "-fPIC", "-nostdlib", "-fuse-ld=lld", link,
		"-o", out, src)
	data, err := os.ReadFile(out)
	if err != nil {
		t.Fatalf("%v", err)
	}
	return data
}

// ShimC and OriginalC are the shim's and the store loader's sources: the shim exports the marker the recipe greps for and
// carries its build version NUL-bounded.
const (
	shimC = "\n/* The shim's stand-in: exports the marker the recipe greps for and carries its build version NUL-bounded. */\n" +
		"int nevr_sentinel_marker = 1;\nconst char nevr_fixture_version[] = \"\\0\" VERSION \"\\0\";\n"
	originalC = "int nevr_original_loader_symbol = 2;\n"
)

// StoreLibr15 is what the default store APK carries as libr15.so.
var StoreLibr15 = []byte(strings.Repeat("\x7fELF the game\x00", 50))

// MakeInputs writes the store-like APK (the default manifest, the store's own libraries and assets, and a signature the
// recipe drops) and the shim with its build info, for the given version. header false leaves the build info out.
func (f *Fixture) MakeInputs(version string, header bool) {
	t := f.T
	t.Helper()
	original := filepath.Join(f.Dir, "original", "libovrplatformloader.so")
	if err := os.MkdirAll(filepath.Dir(original), 0o755); err != nil {
		t.Fatalf("%v", err)
	}
	BuildLib(t, original, originalC, "libovrplatformloader.so", "")
	originalBytes, err := os.ReadFile(original)
	if err != nil {
		t.Fatalf("%v", err)
	}
	f.APK = filepath.Join(f.Dir, "store.apk")
	writeZip(t, f.APK, zip.Deflate, []zipEntry{
		{"AndroidManifest.xml", StoreManifest(storeVersionCode)},
		{"lib/arm64-v8a/libovrplatformloader.so", originalBytes},
		{"lib/arm64-v8a/libr15.so", StoreLibr15},
		{"assets/data.bin", bytes.Repeat(allBytes(), 20)},
		{"META-INF/CERT.SF", []byte("signature file\n")},
		{"META-INF/CERT.RSA", []byte{0x30, 0x00}},
		{"META-INF/MANIFEST.MF", []byte("Manifest-Version: 1.0\n")},
	})
	f.Shim = filepath.Join(f.Dir, "build", "sentinel", "libovrplatformloader.so")
	if err := os.MkdirAll(filepath.Dir(f.Shim), 0o755); err != nil {
		t.Fatalf("%v", err)
	}
	BuildLib(t, f.Shim, shimC, "libovrplatformloader.so", version)
	if header {
		info := filepath.Join(f.Dir, "build", "generated")
		if err := os.MkdirAll(info, 0o755); err != nil {
			t.Fatalf("%v", err)
		}
		body := fmt.Sprintf("inline constexpr const char* kVersion = %q;\ninline constexpr const char* kCommit = \"3a35e0b9\";\n", version)
		if err := os.WriteFile(filepath.Join(info, "nevr_build_info.h"), []byte(body), 0o644); err != nil {
			t.Fatalf("%v", err)
		}
	}
}

// allBytes is the bytes 0 to 255, the store's asset payload.
func allBytes() []byte {
	out := make([]byte, 256)
	for i := range out {
		out[i] = byte(i)
	}
	return out
}

// zipEntry is one file of a zip the fixture writes.
type zipEntry struct {
	name string
	data []byte
}

// writeZip writes entries to path with the method.
func writeZip(t testkit.T, path string, method uint16, entries []zipEntry) {
	t.Helper()
	f, err := os.Create(path)
	if err != nil {
		t.Fatalf("%v", err)
	}
	defer f.Close()
	zw := zip.NewWriter(f)
	for _, e := range entries {
		w, err := zw.CreateHeader(&zip.FileHeader{Name: e.name, Method: method})
		if err != nil {
			t.Fatalf("%v", err)
		}
		if _, err := w.Write(e.data); err != nil {
			t.Fatalf("%v", err)
		}
	}
	if err := zw.Close(); err != nil {
		t.Fatalf("%v", err)
	}
}

// WriteZip is writeZip for the tests: entries under their names, with the method.
func WriteZip(t testkit.T, path string, method uint16, names []string, datas [][]byte) {
	t.Helper()
	entries := make([]zipEntry, len(names))
	for i := range names {
		entries[i] = zipEntry{names[i], datas[i]}
	}
	writeZip(t, path, method, entries)
}

// ReplaceLibr15 rewrites the store APK with its libr15.so replaced by data, the other entries as they were.
func (f *Fixture) ReplaceLibr15(data []byte) {
	t := f.T
	t.Helper()
	src, err := zip.OpenReader(f.APK)
	if err != nil {
		t.Fatalf("%v", err)
	}
	var entries []zipEntry
	for _, info := range src.File {
		rc, err := info.Open()
		if err != nil {
			t.Fatalf("%v", err)
		}
		buf := new(bytes.Buffer)
		if _, err := buf.ReadFrom(rc); err != nil {
			t.Fatalf("%v", err)
		}
		rc.Close()
		body := buf.Bytes()
		if info.Name == "lib/arm64-v8a/libr15.so" {
			body = data
		}
		entries = append(entries, zipEntry{info.Name, body})
	}
	src.Close()
	rebuilt := f.APK + ".rebuilt"
	writeZip(t, rebuilt, zip.Deflate, entries)
	if err := os.Rename(rebuilt, f.APK); err != nil {
		t.Fatalf("%v", err)
	}
}

// AddStoreEntry appends an entry to the store APK.
func (f *Fixture) AddStoreEntry(name string, data []byte) {
	t := f.T
	t.Helper()
	src, err := zip.OpenReader(f.APK)
	if err != nil {
		t.Fatalf("%v", err)
	}
	var entries []zipEntry
	for _, info := range src.File {
		rc, err := info.Open()
		if err != nil {
			t.Fatalf("%v", err)
		}
		buf := new(bytes.Buffer)
		if _, err := buf.ReadFrom(rc); err != nil {
			t.Fatalf("%v", err)
		}
		rc.Close()
		entries = append(entries, zipEntry{info.Name, buf.Bytes()})
	}
	src.Close()
	entries = append(entries, zipEntry{name, data})
	writeZip(t, f.APK, zip.Deflate, entries)
}

// Result is one run of the recipe.
type Result struct {
	Stdout string
	Stderr string
	Code   int
	Signed string // the signed APK's path (it may not exist)
}

// Recipe runs `just android-repack-apk` with the store APK, the shim and workdir (under the fixture's directory), and
// positional, the recipe's parameters after the shim and the work directory, in order (plugin, version, extra_rows, ...).
func (f *Fixture) Recipe(workdir string, positional ...string) Result {
	t := f.T
	t.Helper()
	work := filepath.Join(f.Dir, workdir)
	rel, err := filepath.Rel(f.Repo, work)
	if err != nil {
		t.Fatalf("%v", err)
	}
	args := []string{"--justfile", filepath.Join(f.Repo, "justfile"), "--working-directory", f.Repo, "android-repack-apk",
		f.APK, f.Shim, rel}
	args = append(args, positional...)
	cmd := exec.Command("just", args...)
	cmd.Env = f.Env
	var stdout, stderr bytes.Buffer
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr
	code := 0
	if err := cmd.Run(); err != nil {
		exitErr, ok := err.(*exec.ExitError)
		if !ok {
			t.Fatalf("just: %v", err)
		}
		code = exitErr.ExitCode()
	}
	return Result{Stdout: stdout.String(), Stderr: stderr.String(), Code: code,
		Signed: filepath.Join(work, "r15_nevr-sentinel_signed.apk")}
}

// Repack is Recipe with no plugin, version, extra rows, and the stand-in patcher: the plain repack.
func (f *Fixture) Repack(workdir string) Result {
	return f.Recipe(workdir, "", "", "", f.Patcher)
}

// NewestReleaseMajor is the major version of the newest vX.Y.Z tag git lists in repo (the recipe's stamp floor); the
// fixture's dev versions take it from git so a new major does not stale a test.
func NewestReleaseMajor(t testkit.T, repo string) int {
	t.Helper()
	cmd := exec.Command("git", "-C", repo, "tag", "--list", "v*")
	out, err := cmd.Output()
	if err != nil {
		t.Fatalf("git tag --list: %v", err)
	}
	best, bestMajor := [3]int{-1, -1, -1}, -1
	for _, name := range strings.Fields(string(out)) {
		m := releaseTag.FindStringSubmatch(name)
		if m == nil {
			continue
		}
		var v [3]int
		for i := 0; i < 3; i++ {
			v[i], _ = strconv.Atoi(m[i+1])
		}
		if v[0] > best[0] || (v[0] == best[0] && (v[1] > best[1] || (v[1] == best[1] && v[2] > best[2]))) {
			best, bestMajor = v, v[0]
		}
	}
	if bestMajor < 0 {
		t.Fatalf("no release tag (vX.Y.Z) in %s: fetch the tags", repo)
	}
	return bestMajor
}

var releaseTag = regexp.MustCompile(`^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$`)
