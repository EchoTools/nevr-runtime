package main

import (
	"archive/zip"
	"bytes"
	"context"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
	"time"

	"github.com/EchoTools/nevr-runtime/tools/internal/axmlfix"
	"github.com/EchoTools/nevr-runtime/tools/internal/questapkfix"
	"github.com/EchoTools/nevr-runtime/tools/internal/testkit"
)

// The Go twin of tools/tests/test_quest_repack_version.py: `just android-repack-apk` end to end (#923). The recipe runs for
// real over a fixture APK and a compiled fixture shim, signed through the real tools/quest_keystore.sh with a fixture key
// made here (NEVR_QUEST_KEYSTORE and NEVR_QUEST_KEYSTORE_PASS name it; the real key is never touched). The versionCode is
// read back from the SIGNED APK by aapt2, a reader that is not ours. The expected numbers are written out here, derived
// by hand from the rule. The tools the recipe replaces with fakes are Go (testkit.Fake), not scripts.

const (
	repackTimeout       = 300 * time.Second
	executableMode      = 0o755
	fixtureAlias        = "nevr"
	fixturePassphrase   = "fixture-pass-7f3a91"
	patcherName         = "quest-patch-libr15"
	signedName          = "r15_nevr-sentinel_signed.apk"
	stampMarker         = "\n#NEVR-STANDIN-PATCHED\n"
	behaviourFile       = "behaviour"
	realFile            = "real"
	behaviourPass       = "pass"
	behaviourRewrite    = "rewrite"
	behaviourFail       = "fail"
	behaviourVerifyFail = "verify-fails"
	exitFakeSetup       = 125
	fakeAapt2Fail       = 3
	fakeApksignerFail   = 1
	fakeNmFail          = 1
	storeCodeNumber     = "4987566"
	installerCode       = 4987570
	shimAllBytes        = 256
	libr15Repeats       = 50
	dataRepeats         = 20
	devTail             = "6.1-dev.37+3a35e0b"
	majorDev37          = 601037
	releaseSlot         = 999
	majorShiftFactor    = 10_000_000
	releaseMinorScale   = 100_000
	releaseMinorValue   = 6
	buildInfoHeader     = "inline constexpr const char* kVersion = \"%s\";\ninline constexpr const char* kCommit = \"3a35e0b9\";\n"
	shimSource          = "\n/* The shim's stand-in: exports the marker the recipe greps for and carries its build version NUL-bounded. */\nint nevr_sentinel_marker = 1;\nconst char nevr_fixture_version[] = \"\\0\" VERSION \"\\0\";\n"
	originalSource      = "int nevr_original_loader_symbol = 2;\n"
	soNameLoader        = "libovrplatformloader.so"
	apkManifestName     = "AndroidManifest.xml"
	storeLibrary        = "lib/arm64-v8a/libovrplatformloader.so"
	gameLibrary         = "lib/arm64-v8a/libr15.so"
	dataEntry           = "assets/data.bin"
	signedRE            = `versionCode='[0-9]*'`
)

// repackTools are the programs the recipe needs; a missing one skips the recipe tests, as the Python tests did.
var repackTools = []string{"just", "cc", "patchelf", "zip", "unzip", "zipalign", "apksigner", "aapt2", "readelf", "nm", "keytool"}

// requireRepackTools skips the test when a tool the recipe runs is missing.
func requireRepackTools(t *testing.T) {
	t.Helper()
	for _, tool := range repackTools {
		if _, err := exec.LookPath(tool); err != nil {
			t.Skipf("the recipe needs %s", tool)
		}
	}
}

// repackRoot is the checkout the recipe runs in.
func repackRoot(t *testing.T) string { return repoPath(t) }

// fakeBehaviour is the mode the test chose for this fake: a word in the file behaviourFile beside it.
func fakeBehaviour(c *testkit.Call) string {
	raw, err := os.ReadFile(filepath.Join(c.FakeDir, behaviourFile))
	if err != nil {
		return behaviourPass
	}
	return strings.TrimSpace(string(raw))
}

// runReal starts the real program the test recorded beside the fake, with the fake's environment and streams, and passes
// its standard output through rewrite (nil: unchanged). It returns the program's exit code.
func runReal(c *testkit.Call, args []string, rewrite func([]byte) []byte) int {
	raw, err := os.ReadFile(filepath.Join(c.FakeDir, realFile))
	if err != nil {
		fmt.Fprintf(c.Stderr, "fake %s: no real program recorded: %v\n", c.Name, err)
		return exitFakeSetup
	}
	cmd := exec.Command(strings.TrimSpace(string(raw)), args...)
	cmd.Env = c.Environ
	cmd.Stdin = c.Stdin
	var out bytes.Buffer
	cmd.Stdout = &out
	cmd.Stderr = c.Stderr
	code := 0
	if err := cmd.Run(); err != nil {
		exitErr, ok := err.(*exec.ExitError)
		if !ok {
			fmt.Fprintf(c.Stderr, "fake %s: %v\n", c.Name, err)
			return exitFakeSetup
		}
		code = exitErr.ExitCode()
	}
	data := out.Bytes()
	if rewrite != nil {
		data = rewrite(data)
	}
	c.Stdout.Write(data)
	return code
}

// fakeAapt2 is the real aapt2, or one of the two misbehaviours the recipe tests plant: a versionCode that reads back as 1,
// and an aapt2 that fails.
func fakeAapt2(c *testkit.Call) int {
	switch fakeBehaviour(c) {
	case behaviourRewrite:
		return runReal(c, c.Args, func(b []byte) []byte {
			return regexp.MustCompile(signedRE).ReplaceAll(b, []byte("versionCode='1'"))
		})
	case behaviourFail:
		fmt.Fprintln(c.Stderr, "aapt2: cannot read the apk")
		return fakeAapt2Fail
	default:
		return runReal(c, c.Args, nil)
	}
}

// fakeApksigner is the real apksigner, or one whose verify fails.
func fakeApksigner(c *testkit.Call) int {
	if fakeBehaviour(c) == behaviourVerifyFail && len(c.Args) > 0 && c.Args[0] == "verify" {
		fmt.Fprintln(c.Stderr, "apksigner: DOES NOT VERIFY")
		return fakeApksignerFail
	}
	return runReal(c, c.Args, nil)
}

// fakeNm is an nm that reports no symbols: a failure after the signed APK exists.
func fakeNm(c *testkit.Call) int {
	fmt.Fprintln(c.Stderr, "nm: no symbols")
	return fakeNmFail
}

// fakePatcher is the libr15 patcher's stand-in: it takes the real tool's command line (--stock, --out, --report) and writes
// the stock bytes followed by the marker, so its output differs from its input and depends on it.
func fakePatcher(c *testkit.Call) int {
	flags := map[string]string{}
	for i := 0; i+1 < len(c.Args); i += 2 {
		flags[c.Args[i]] = c.Args[i+1]
	}
	stock, err := os.ReadFile(flags["--stock"])
	if err != nil {
		fmt.Fprintf(c.Stderr, "standin patcher: %v\n", err)
		return exitFakeSetup
	}
	if err := os.WriteFile(flags["--out"], append(stock, []byte(stampMarker)...), 0o644); err != nil {
		fmt.Fprintf(c.Stderr, "standin patcher: %v\n", err)
		return exitFakeSetup
	}
	if err := os.WriteFile(flags["--report"], []byte("{}\n"), 0o644); err != nil {
		fmt.Fprintf(c.Stderr, "standin patcher: %v\n", err)
		return exitFakeSetup
	}
	return 0
}

// repackFixture is one test's inputs and environment: a fixture key, a store-shaped APK, the compiled shim and the build
// info, and the stand-in patcher.
type repackFixture struct {
	t        *testing.T
	tmp      string
	env      []string
	apk      string
	shim     string
	patcher  string
	signer   string
	signed   string
	original []byte
}

// newRepackFixture makes the key, the environment and the patcher stand-in.
func newRepackFixture(t *testing.T) *repackFixture {
	t.Helper()
	requireRepackTools(t)
	r := &repackFixture{t: t, tmp: t.TempDir()}
	run := filepath.Join(r.tmp, "run")
	if err := os.Mkdir(run, 0o700); err != nil {
		t.Fatal(err)
	}
	ks := filepath.Join(r.tmp, "fixture.jks")
	mustRun(t, "keytool", "-genkeypair", "-keystore", ks, "-alias", fixtureAlias, "-keyalg", "RSA", "-keysize", "2048",
		"-validity", "30", "-storepass", fixturePassphrase, "-keypass", fixturePassphrase, "-dname", "CN=Quest Repack Version Test, O=test")
	listing := mustRun(t, "keytool", "-list", "-keystore", ks, "-alias", fixtureAlias, "-storepass", fixturePassphrase)
	for _, line := range strings.Split(listing, "\n") {
		if strings.Contains(line, "SHA-256") || strings.Contains(line, "SHA256") {
			r.signer = strings.ToLower(strings.ReplaceAll(strings.TrimSpace(strings.SplitN(line, ":", 2)[1]), ":", ""))
			break
		}
	}
	r.env = testkit.Env(t, "NEVR_QUEST_KEYSTORE="+ks, "NEVR_QUEST_KEYSTORE_PASS="+fixturePassphrase, "XDG_RUNTIME_DIR="+run)
	patcher := testkit.Fake(t, patcherName)
	r.patcher = filepath.Join(patcher.Dir(), patcherName)
	copyTestBinary(t, r.patcher)
	return r
}

// copyTestBinary replaces the link Fake made at path with a copy of this test binary. The recipe's patcher is resolved
// with a path that follows links (quest_repack_libs.py, Path.resolve), so a link would start the test binary under its own
// name, which is not a fake. A copy keeps the name the fake is found by.
func copyTestBinary(t *testing.T, path string) {
	t.Helper()
	exe, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	src, err := os.Open(exe)
	if err != nil {
		t.Fatal(err)
	}
	defer src.Close()
	if err := os.Remove(path); err != nil {
		t.Fatal(err)
	}
	dst, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_EXCL, executableMode)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := io.Copy(dst, src); err != nil {
		t.Fatal(err)
	}
	if err := dst.Close(); err != nil {
		t.Fatal(err)
	}
}

// mustRun runs a program in the test's environment and returns its standard output; a failure fails the test.
func mustRun(t *testing.T, name string, args ...string) string {
	t.Helper()
	out, err := exec.Command(name, args...).CombinedOutput()
	if err != nil {
		t.Fatalf("%s: %v\n%s", name, err, out)
	}
	return string(out)
}

// buildLib compiles a shared object from source with the soname, with the version as the VERSION macro when given.
func buildLib(t *testing.T, path, source, soname, version string) {
	t.Helper()
	src := strings.TrimSuffix(path, filepath.Ext(path)) + ".c"
	if err := os.WriteFile(src, []byte(source), 0o644); err != nil {
		t.Fatal(err)
	}
	args := []string{"-shared", "-fPIC", "-Wl,-soname," + soname, "-o", path, src}
	if version != "" {
		args = append([]string{"-DVERSION=\"" + version + "\""}, args...)
	}
	mustRun(t, "cc", args...)
}

// makeInputs writes the store APK, the shim and the build info for a version. shimVersion (when set) is the shim's own.
func (r *repackFixture) makeInputs(version string, manifest []byte, header bool, shimVersion string) {
	r.t.Helper()
	original := filepath.Join(r.tmp, "original", soNameLoader)
	if err := os.MkdirAll(filepath.Dir(original), 0o755); err != nil {
		r.t.Fatal(err)
	}
	buildLib(r.t, original, originalSource, soNameLoader, "")
	r.original = readOrFail(r.t, original)
	if manifest == nil {
		manifest = axmlfix.Default().Build()
	}
	r.apk = filepath.Join(r.tmp, "store.apk")
	writeEntries(r.t, r.apk, []questapkfix.Entry{
		{Name: apkManifestName, Data: manifest, Method: questapkfix.Deflate},
		{Name: storeLibrary, Data: r.original, Method: questapkfix.Deflate},
		{Name: gameLibrary, Data: bytes.Repeat([]byte("\x7fELF the game\x00"), libr15Repeats), Method: questapkfix.Deflate},
		{Name: dataEntry, Data: bytes.Repeat(allByteValues(), dataRepeats), Method: questapkfix.Deflate},
		{Name: "META-INF/CERT.SF", Data: []byte("signature file\n"), Method: questapkfix.Deflate},
		{Name: "META-INF/CERT.RSA", Data: []byte{0x30, 0x00}, Method: questapkfix.Deflate},
		{Name: "META-INF/MANIFEST.MF", Data: []byte("Manifest-Version: 1.0\n"), Method: questapkfix.Deflate},
	})
	if shimVersion == "" {
		shimVersion = version
	}
	r.shim = filepath.Join(r.tmp, "build", "sentinel", soNameLoader)
	if err := os.MkdirAll(filepath.Dir(r.shim), 0o755); err != nil {
		r.t.Fatal(err)
	}
	buildLib(r.t, r.shim, shimSource, soNameLoader, shimVersion)
	if header {
		info := filepath.Join(r.tmp, "build", "generated")
		if err := os.Mkdir(info, 0o755); err != nil {
			r.t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(info, "nevr_build_info.h"), []byte(fmt.Sprintf(buildInfoHeader, version)), 0o644); err != nil {
			r.t.Fatal(err)
		}
	}
}

// allByteValues is the 256 byte values, in order.
func allByteValues() []byte {
	out := make([]byte, shimAllBytes)
	for i := range out {
		out[i] = byte(i)
	}
	return out
}

// readOrFail is a file's bytes; a failure fails the test.
func readOrFail(t *testing.T, path string) []byte {
	t.Helper()
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	return data
}

// writeEntries writes entries as an APK at path, each with its own method.
func writeEntries(t *testing.T, path string, entries []questapkfix.Entry) {
	t.Helper()
	out, err := os.Create(path)
	if err != nil {
		t.Fatal(err)
	}
	zw := zip.NewWriter(out)
	for _, e := range entries {
		w, err := zw.CreateHeader(&zip.FileHeader{Name: e.Name, Method: e.Method})
		if err != nil {
			t.Fatal(err)
		}
		if _, err := io.Copy(w, bytes.NewReader(e.Data)); err != nil {
			t.Fatal(err)
		}
	}
	if err := zw.Close(); err != nil {
		t.Fatal(err)
	}
	out.Close()
}

// fake makes a fake of a real tool first on the recipe's PATH: its behaviour and the real program it replaces.
func (r *repackFixture) fake(name, behaviour string) {
	r.t.Helper()
	real, err := exec.LookPath(name)
	if err != nil {
		r.t.Fatalf("the fake %s needs the real program: %v", name, err)
	}
	f := testkit.Fake(r.t, name)
	if err := os.WriteFile(filepath.Join(f.Dir(), behaviourFile), []byte(behaviour), 0o644); err != nil {
		r.t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(f.Dir(), realFile), []byte(real), 0o644); err != nil {
		r.t.Fatal(err)
	}
	r.env = testkit.WithPath(r.env, f.Dir())
}

// repackResult is one run of the recipe: its exit code and both streams.
type repackResult struct {
	code           int
	stdout, stderr string
}

// repack runs the recipe over the fixture inputs.
func (r *repackFixture) repack() repackResult {
	r.t.Helper()
	root := repackRoot(r.t)
	work := filepath.Join(r.tmp, "work")
	rel, err := filepath.Rel(root, work)
	if err != nil {
		r.t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(r.t.Context(), repackTimeout)
	defer cancel()
	cmd := exec.CommandContext(ctx, "just", "--justfile", filepath.Join(root, "justfile"), "--working-directory", root,
		"android-repack-apk", r.apk, r.shim, rel, "", "", "", r.patcher)
	cmd.Env = r.env
	var stdout, stderr bytes.Buffer
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr
	code := 0
	if err := cmd.Run(); err != nil {
		exitErr, ok := err.(*exec.ExitError)
		if !ok {
			r.t.Fatalf("just: %v", err)
		}
		code = exitErr.ExitCode()
	}
	r.signed = filepath.Join(work, signedName)
	return repackResult{code: code, stdout: stdout.String(), stderr: stderr.String()}
}

// mustRepack runs the recipe and requires success.
func (r *repackFixture) mustRepack() repackResult {
	r.t.Helper()
	res := r.repack()
	if res.code != 0 {
		r.t.Fatalf("the recipe exited %d:\n%s%s", res.code, res.stdout, res.stderr)
	}
	return res
}

// badgingCode is the versionCode aapt2 reads from an APK.
func badgingCode(t *testing.T, apk string) int {
	t.Helper()
	out := mustRun(t, "aapt2", "dump", "badging", apk)
	m := regexp.MustCompile(`(?m)^package: .*versionCode='(\d+)'`).FindStringSubmatch(out)
	if m == nil {
		t.Fatalf("no versionCode in badging:\n%s", out)
	}
	return intOf(t, m[1])
}

// xmltree is aapt2's dump of the manifest.
func xmltree(t *testing.T, apk string) string {
	t.Helper()
	return mustRun(t, "aapt2", "dump", "xmltree", "--file", apkManifestName, apk)
}

// devCode is the rule's code for the fixture's development build of this major: major*10^7 + 6*10^5 + 1*10^3 + 37.
func devCode(major int) int { return major*majorShiftFactor + majorDev37 }

// releaseCode is the rule's code for the fixture's release: major*10^7 + 6*10^5 + 0*10^3 + 999.
func releaseCode(major int) int {
	return major*majorShiftFactor + releaseMinorScale*releaseMinorValue + releaseSlot
}

// fixtureMajor is the major of the newest release tag this checkout lists: the stamp floor. Read, not pinned.
func fixtureMajor(t *testing.T) int { return currentMajor(t) }

// assertRefused checks a recipe run that must stop: a nonzero exit, no signed APK left behind, and each needle in its output.
func (r *repackFixture) assertRefused(res repackResult, needles ...string) {
	r.t.Helper()
	if res.code == 0 {
		r.t.Fatalf("the recipe succeeded:\n%s%s", res.stdout, res.stderr)
	}
	if _, err := os.Stat(r.signed); err == nil {
		r.t.Fatal("a signed APK was left behind")
	}
	text := res.stdout + res.stderr
	for _, needle := range needles {
		if !strings.Contains(text, needle) {
			r.t.Fatalf("want %q in:\n%s", needle, text)
		}
	}
}

// TestTheBuiltApkIsAboveTheInstallersGameBuild.
func TestTheBuiltApkIsAboveTheInstallersGameBuild(t *testing.T) {
	t.Parallel()
	r := newRepackFixture(t)
	major := fixtureMajor(t)
	r.makeInputs(itoa(major)+"."+devTail, nil, true, "")
	r.mustRepack()
	if code := badgingCode(t, r.signed); code <= installerCode {
		t.Fatalf("the built code %d is not above the installer's %d: an update over it would be a downgrade", code, installerCode)
	}
}

// TestADevelopmentBuildGetsTheCodeTheRuleGivesIt.
func TestADevelopmentBuildGetsTheCodeTheRuleGivesIt(t *testing.T) {
	t.Parallel()
	r := newRepackFixture(t)
	major := fixtureMajor(t)
	r.makeInputs(itoa(major)+"."+devTail, nil, true, "")
	r.mustRepack()
	if code := badgingCode(t, r.signed); code != devCode(major) {
		t.Fatalf("code = %d, want %d", code, devCode(major))
	}
}

// TestAReleaseGetsACodeAboveEveryDevelopmentBuildOfItAndBelowTheNext.
func TestAReleaseGetsACodeAboveEveryDevelopmentBuildOfItAndBelowTheNext(t *testing.T) {
	t.Parallel()
	r := newRepackFixture(t)
	major := fixtureMajor(t)
	r.makeInputs(itoa(major)+".6.0", nil, true, "")
	r.mustRepack()
	code := badgingCode(t, r.signed)
	if code != releaseCode(major) {
		t.Fatalf("code = %d, want %d", code, releaseCode(major))
	}
	if code <= releaseCode(major)-1 {
		t.Fatal("not above the same major's dev.998")
	}
	if code >= devCode(major) {
		t.Fatal("not below the next patch's development builds")
	}
}

// TestTheSignedApkVerifiesAndCarriesTheSignerOfTheKeyInPass.
func TestTheSignedApkVerifiesAndCarriesTheSignerOfTheKeyInPass(t *testing.T) {
	t.Parallel()
	r := newRepackFixture(t)
	r.makeInputs(itoa(fixtureMajor(t))+"."+devTail, nil, true, "")
	r.mustRepack()
	verify := exec.Command("apksigner", "verify", "--verbose", "--print-certs", r.signed)
	out, err := verify.CombinedOutput()
	if err != nil {
		t.Fatalf("apksigner verify: %v\n%s", err, out)
	}
	if !strings.Contains(string(out), "Verified using v3 scheme (APK Signature Scheme v3): true") {
		t.Fatalf("no v3 verification:\n%s", out)
	}
	if !strings.Contains(string(out), "certificate SHA-256 digest: "+r.signer) {
		t.Fatalf("signer %s not in:\n%s", r.signer, out)
	}
}

// TestOnlyTheVersionCodeLineOfTheManifestChanges.
func TestOnlyTheVersionCodeLineOfTheManifestChanges(t *testing.T) {
	t.Parallel()
	r := newRepackFixture(t)
	major := fixtureMajor(t)
	r.makeInputs(itoa(major)+"."+devTail, nil, true, "")
	before := strings.Split(xmltree(t, r.apk), "\n")
	r.mustRepack()
	after := strings.Split(xmltree(t, r.signed), "\n")
	if len(before) != len(after) {
		t.Fatalf("line counts %d and %d", len(before), len(after))
	}
	var changed [][2]string
	for i := range before {
		if before[i] != after[i] {
			changed = append(changed, [2]string{before[i], after[i]})
		}
	}
	if len(changed) != 1 {
		t.Fatalf("changed lines: %v", changed)
	}
	if !strings.Contains(changed[0][0], "versionCode(0x0101021b)="+storeCodeNumber) {
		t.Fatalf("before: %s", changed[0][0])
	}
	if !strings.Contains(changed[0][1], "versionCode(0x0101021b)="+itoa(devCode(major))) {
		t.Fatalf("after: %s", changed[0][1])
	}
}

// TestEveryOtherEntryOfTheStoreApkIsByteIdentical: the repack changes the game library on purpose (the stand-in's output),
// and adds the original loader beside it.
func TestEveryOtherEntryOfTheStoreApkIsByteIdentical(t *testing.T) {
	t.Parallel()
	r := newRepackFixture(t)
	r.makeInputs(itoa(fixtureMajor(t))+"."+devTail, nil, true, "")
	r.mustRepack()
	src, err := zip.OpenReader(r.apk)
	if err != nil {
		t.Fatal(err)
	}
	defer src.Close()
	out, err := zip.OpenReader(r.signed)
	if err != nil {
		t.Fatal(err)
	}
	defer out.Close()
	read := func(zr *zip.ReadCloser, name string) []byte {
		for _, f := range zr.File {
			if f.Name == name {
				rc, err := f.Open()
				if err != nil {
					t.Fatal(err)
				}
				defer rc.Close()
				data, err := io.ReadAll(rc)
				if err != nil {
					t.Fatal(err)
				}
				return data
			}
		}
		t.Fatalf("%s is not in the APK", name)
		return nil
	}
	for _, name := range []string{dataEntry, gameLibrary} {
		want := read(src, name)
		if name == gameLibrary {
			want = append(append([]byte{}, want...), []byte(stampMarker)...)
		}
		if !bytes.Equal(read(out, name), want) {
			t.Fatalf("%s differs from the store APK", name)
		}
	}
	var names []string
	for _, f := range out.File {
		names = append(names, f.Name)
	}
	if !containsName(names, "lib/arm64-v8a/libovrplatformloader_orig.so") {
		t.Fatalf("the original loader is not in the APK: %v", names)
	}
}

// containsName reports whether a name list holds name.
func containsName(names []string, name string) bool {
	for _, n := range names {
		if n == name {
			return true
		}
	}
	return false
}

// TestTheRecipeSaysWhatItStamped.
func TestTheRecipeSaysWhatItStamped(t *testing.T) {
	t.Parallel()
	r := newRepackFixture(t)
	major := fixtureMajor(t)
	r.makeInputs(itoa(major)+"."+devTail, nil, true, "")
	res := r.mustRepack()
	if !strings.Contains(strings.ReplaceAll(res.stderr, " ", ""), `"version_code_after":`+itoa(devCode(major))) {
		t.Fatalf("stderr = %s", res.stderr)
	}
	if !strings.Contains(res.stdout, "versionCode "+itoa(devCode(major))) {
		t.Fatalf("stdout = %s", res.stdout)
	}
}

// TestNoBuildInfoBesideTheShimIsARefusalNotADefault.
func TestNoBuildInfoBesideTheShimIsARefusalNotADefault(t *testing.T) {
	t.Parallel()
	r := newRepackFixture(t)
	r.makeInputs(itoa(fixtureMajor(t))+"."+devTail, nil, false, "")
	r.assertRefused(r.repack(), "nevr_build_info.h")
}

// TestAShimThatDoesNotCarryTheBuildVersionIsRefused.
func TestAShimThatDoesNotCarryTheBuildVersionIsRefused(t *testing.T) {
	t.Parallel()
	r := newRepackFixture(t)
	major := fixtureMajor(t)
	version := itoa(major) + "." + devTail
	r.makeInputs(version, nil, true, itoa(major)+".6.1-dev.36+3a35e0b")
	r.assertRefused(r.repack(), "shim", version)
}

// TestAManifestWithoutAVersionCodeIsRefused.
func TestAManifestWithoutAVersionCodeIsRefused(t *testing.T) {
	t.Parallel()
	r := newRepackFixture(t)
	s := axmlfix.Default()
	s.VersionCodeCount = 0
	r.makeInputs(itoa(fixtureMajor(t))+"."+devTail, s.Build(), true, "")
	r.assertRefused(r.repack(), "versionCode")
}

// TestAVersionBelowTheLastReleaseIsRefused.
func TestAVersionBelowTheLastReleaseIsRefused(t *testing.T) {
	t.Parallel()
	r := newRepackFixture(t)
	r.makeInputs("0.1.0", nil, true, "")
	r.assertRefused(r.repack(), "major "+itoa(fixtureMajor(t)))
}

// TestTheVersionABuildWithoutGitReportsIsRefused: cmake/set_project_version_from_git.cmake leaves "1.0.0" when git is absent.
func TestTheVersionABuildWithoutGitReportsIsRefused(t *testing.T) {
	t.Parallel()
	r := newRepackFixture(t)
	r.makeInputs("1.0.0", nil, true, "")
	r.assertRefused(r.repack(), "1.0.0", "major "+itoa(fixtureMajor(t)))
}

// TestASignedApkThatReadsBackWithAnotherCodeIsRemovedAndTheRecipeFails: the recipe compares what aapt2 reads from the
// SIGNED APK with what it stamped; an aapt2 that reports another code is refused.
func TestASignedApkThatReadsBackWithAnotherCodeIsRemovedAndTheRecipeFails(t *testing.T) {
	t.Parallel()
	r := newRepackFixture(t)
	major := fixtureMajor(t)
	r.makeInputs(itoa(major)+"."+devTail, nil, true, "")
	r.fake("aapt2", behaviourRewrite)
	r.assertRefused(r.repack(), "signed APK versionCode is '1'", itoa(devCode(major)))
}

// TestAnAapt2ThatFailsLeavesNoSignedApk: set -e quits at a failing command substitution, so the APK does not survive.
func TestAnAapt2ThatFailsLeavesNoSignedApk(t *testing.T) {
	t.Parallel()
	r := newRepackFixture(t)
	r.makeInputs(itoa(fixtureMajor(t))+"."+devTail, nil, true, "")
	r.fake("aapt2", behaviourFail)
	r.assertRefused(r.repack(), "aapt2: cannot read the apk")
}

// TestAnApksignerVerifyThatFailsLeavesNoSignedApk: sign is real, so a signed APK exists to be left behind; only verify fails.
func TestAnApksignerVerifyThatFailsLeavesNoSignedApk(t *testing.T) {
	t.Parallel()
	r := newRepackFixture(t)
	r.makeInputs(itoa(fixtureMajor(t))+"."+devTail, nil, true, "")
	r.fake("apksigner", behaviourVerifyFail)
	r.assertRefused(r.repack(), "apksigner: DOES NOT VERIFY")
}

// TestAFailureAfterTheChecksLeavesNoSignedApkEither: the ELF checks that follow run on the signed APK; any stop after
// signing removes it.
func TestAFailureAfterTheChecksLeavesNoSignedApkEither(t *testing.T) {
	t.Parallel()
	r := newRepackFixture(t)
	r.makeInputs(itoa(fixtureMajor(t))+"."+devTail, nil, true, "")
	r.fake("nm", behaviourPass)
	r.assertRefused(r.repack(), "nm: no symbols")
}

// TestASignedApkFromAnEarlierRunDoesNotSurviveARefusedOne.
func TestASignedApkFromAnEarlierRunDoesNotSurviveARefusedOne(t *testing.T) {
	t.Parallel()
	r := newRepackFixture(t)
	r.makeInputs(itoa(fixtureMajor(t))+"."+devTail, nil, true, "")
	first := r.mustRepack()
	if _, err := os.Stat(r.signed); err != nil {
		t.Fatalf("the first run left no signed APK (%v):\n%s", err, first.stderr)
	}
	if err := os.Remove(filepath.Join(r.tmp, "build", "generated", "nevr_build_info.h")); err != nil {
		t.Fatal(err)
	}
	r.assertRefused(r.repack(), "nevr_build_info.h")
}
