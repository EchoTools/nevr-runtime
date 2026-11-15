package main

import (
	"archive/zip"
	"bytes"
	"context"
	"encoding/json"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/axmlfix"
	"github.com/EchoTools/nevr-runtime/tools/internal/questapk"
	"github.com/EchoTools/nevr-runtime/tools/internal/testkit"
)

// The Go twin of the Python command-line tests of tools/tests/test_quest_apk_version.py (CommandLineTest, with
// the read and the duplicate-manifest cases). The tool runs in-process through run; the recipe tests in repack_test.go
// start the real justfile recipe with fakes for the tools they replace.

const (
	storeCodeText     = "4987566"
	installerCodeText = "4987570"
	majorScale        = 10_000_000
	dev37Scale        = 601_037
	rel16Scale        = 1_602_999
	shimDevThirtySix  = "6.1-dev.36+3a35e0b"
	devSuffix         = ".6.1-dev.37+3a35e0b"
	releaseSuffix     = ".16.2"
	rootDir           = "../../.."
	tagField          = "NEVR.QUEST.APK"
	shimPrefix        = "\x7fELF\x00"
	manifestName      = "AndroidManifest.xml"
)

// testFakes are the fake commands of this package's tests: the repack recipe's aapt2, apksigner and nm and the libr15
// patcher's stand-in.
var testFakes = testkit.Fakes{
	"aapt2":              fakeAapt2,
	"apksigner":          fakeApksigner,
	"nm":                 fakeNm,
	"quest-patch-libr15": fakePatcher,
}

func TestMain(m *testing.M) { testkit.Main(m, testFakes) }

// runTool runs the tool in-process with a hermetic environment and returns its exit code and both streams.
func runTool(t *testing.T, args ...string) (int, string, string) {
	t.Helper()
	env := testkit.Env(t)
	var stdout, stderr bytes.Buffer
	code := run(context.Background(), args, testkit.Getenv(env), nil, &stdout, &stderr)
	return code, stdout.String(), stderr.String()
}

// lastJSON parses the last line of a stream as one JSON object.
func lastJSON(t *testing.T, text string) map[string]any {
	t.Helper()
	lines := strings.Split(strings.TrimRight(text, "\n"), "\n")
	var got map[string]any
	if err := json.Unmarshal([]byte(lines[len(lines)-1]), &got); err != nil {
		t.Fatalf("not one JSON line: %v\n%s", err, text)
	}
	return got
}

// currentMajor is the major of the newest release tag this checkout lists: the stamp floor. Read, not pinned.
func currentMajor(t *testing.T) int {
	t.Helper()
	_, major, err := questapk.NewestRelease(repoPath(t), nil)
	if err != nil {
		t.Fatal(err)
	}
	return major
}

// repoPath is the checkout root, three directories above this package.
func repoPath(t *testing.T) string {
	t.Helper()
	root, err := filepath.Abs(rootDir)
	if err != nil {
		t.Fatal(err)
	}
	return root
}

// intOf parses a decimal expected value.
func intOf(t *testing.T, s string) int {
	t.Helper()
	v, err := strconv.Atoi(s)
	if err != nil {
		t.Fatalf("%q: %v", s, err)
	}
	return v
}

// storeManifest is the store-shaped manifest with the code given.
func storeManifest(code int64) []byte {
	s := axmlfix.Default()
	s.VersionCode = code
	return s.Build()
}

// writeFile writes data to a file in a fresh temporary directory and returns the path.
func writeFile(t *testing.T, name string, data []byte) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), name)
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatal(err)
	}
	return path
}

// TestCode prints the rule's code and logs one line.
func TestCode(t *testing.T) {
	t.Parallel()
	code, stdout, stderr := runTool(t, "code", "5.6.1-dev.37+3a35e0b", "-repo", repoPath(t))
	if code != 0 || strings.TrimSpace(stdout) != "50601037" {
		t.Fatalf("exit %d, stdout %q, stderr %s", code, stdout, stderr)
	}
	got := lastJSON(t, stderr)
	if got["tag"] != tagField || got["event"] != "version_code" || got["version_code"] != float64(intOf(t, "50601037")) {
		t.Fatalf("record = %v", got)
	}
	if got["stampable"] != (5 >= currentMajor(t)) {
		t.Fatalf("stampable = %v", got["stampable"])
	}
}

// TestTheBarV6IsStrictlyAboveV5_20ByTheTool: a headset on 5.20.0 updates in place to 6.0.0 only if 6.0.0's code is higher.
func TestTheBarV6IsStrictlyAboveV5_20ByTheTool(t *testing.T) {
	t.Parallel()
	code, six, _ := runTool(t, "code", "6.0.0", "-repo", repoPath(t))
	if code != 0 || strings.TrimSpace(six) != "60000999" {
		t.Fatalf("6.0.0: exit %d, %q", code, six)
	}
	code, five, _ := runTool(t, "code", "5.20.0", "-repo", repoPath(t))
	if code != 0 || strings.TrimSpace(five) != "52000999" {
		t.Fatalf("5.20.0: exit %d, %q", code, five)
	}
	if intOf(t, strings.TrimSpace(six)) <= intOf(t, strings.TrimSpace(five)) {
		t.Fatal("6.0.0 is not above 5.20.0")
	}
}

// TestCodeBelowTheNewestMajorPrintsTheRulesNumberAndSaysItIsNotStampable.
func TestCodeBelowTheNewestMajorPrintsTheRulesNumberAndSaysItIsNotStampable(t *testing.T) {
	t.Parallel()
	code, stdout, stderr := runTool(t, "code", "0.1.0", "-repo", repoPath(t))
	if code != 0 || strings.TrimSpace(stdout) != "100999" {
		t.Fatalf("exit %d, stdout %q, stderr %s", code, stdout, stderr)
	}
	got := lastJSON(t, stderr)
	if got["event"] != "version_code" || got["version_code"] != float64(intOf(t, "100999")) || got["stampable"] != false {
		t.Fatalf("record = %v", got)
	}
}

// TestCodeRefusalIsOneJSONLineAndExit1.
func TestCodeRefusalIsOneJSONLineAndExit1(t *testing.T) {
	t.Parallel()
	code, stdout, stderr := runTool(t, "code", "210.0.0", "-repo", repoPath(t))
	if code != exitRefused || stdout != "" {
		t.Fatalf("exit %d, stdout %q", code, stdout)
	}
	got := lastJSON(t, stderr)
	if got["tag"] != tagField || got["event"] != "refused" {
		t.Fatalf("record = %v", got)
	}
	if !strings.Contains(got["reason"].(string), "major 210 is above 209") {
		t.Fatalf("reason = %v", got["reason"])
	}
}

// TestCodeWithNoReleaseTagIsRefusedAndPrintsNoCode: the rule alone would print a code; the tool also reads the newest
// release tag, so a repository with none is a refusal.
func TestCodeWithNoReleaseTagIsRefusedAndPrintsNoCode(t *testing.T) {
	t.Parallel()
	bare := testkit.GitRepo(t)
	code, stdout, stderr := runTool(t, "code", "6.0.0", "-repo", bare)
	if code != exitRefused || stdout != "" {
		t.Fatalf("exit %d, stdout %q", code, stdout)
	}
	got := lastJSON(t, stderr)
	if got["event"] != "refused" || !strings.Contains(got["reason"].(string), "no release tag") {
		t.Fatalf("record = %v", got)
	}
}

// TestPatchManifestWritesInPlaceAndLogsBeforeAndAfter.
func TestPatchManifestWritesInPlaceAndLogsBeforeAndAfter(t *testing.T) {
	t.Parallel()
	major := currentMajor(t)
	version := itoa(major) + devSuffix
	code := major*majorScale + dev37Scale
	manifest := writeFile(t, manifestName, storeManifest(intOf64(t, storeCodeText)))
	header := writeFile(t, "nevr_build_info.h", []byte(`constexpr const char* kVersion = "`+version+`";`+"\n"))
	shim := writeFile(t, "shim.so", []byte(shimPrefix+version+"\x00"))
	c, stdout, stderr := runTool(t, "patch-manifest", manifest, "-build-info", header, "-shim", shim, "-repo", repoPath(t))
	if c != 0 || strings.TrimSpace(stdout) != itoa(code) {
		t.Fatalf("exit %d, stdout %q, stderr %s", c, stdout, stderr)
	}
	got := lastJSON(t, stderr)
	if got["version_code_before"] != float64(intOf(t, storeCodeText)) || got["version_code_after"] != float64(code) || got["bytes_changed"] != float64(bytesChangedWant) {
		t.Fatalf("record = %v", got)
	}
	written, err := os.ReadFile(manifest)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(written, storeManifest(int64(code))) {
		t.Fatal("the manifest is not the document built with the new code")
	}
}

// bytesChangedWant is the number of bytes a versionCode stamp changes: the 4-byte field.
const bytesChangedWant = 4

// intOf64 parses a decimal expected value as int64.
func intOf64(t *testing.T, s string) int64 { return int64(intOf(t, s)) }

// itoa is an int as decimal text.
func itoa(n int) string { return strconv.Itoa(n) }

// TestPatchManifestRefusesAShimFromAnotherBuildAndLeavesTheManifestAlone.
func TestPatchManifestRefusesAShimFromAnotherBuildAndLeavesTheManifestAlone(t *testing.T) {
	t.Parallel()
	major := currentMajor(t)
	original := storeManifest(intOf64(t, storeCodeText))
	manifest := writeFile(t, manifestName, original)
	header := writeFile(t, "nevr_build_info.h", []byte(`constexpr const char* kVersion = "`+itoa(major)+devSuffix+`";`+"\n"))
	shim := writeFile(t, "shim.so", []byte("\x00"+itoa(major)+"."+shimDevThirtySix+"\x00"))
	c, _, stderr := runTool(t, "patch-manifest", manifest, "-build-info", header, "-shim", shim, "-repo", repoPath(t))
	if c != exitRefused {
		t.Fatalf("exit %d, stderr %s", c, stderr)
	}
	if !strings.Contains(lastJSON(t, stderr)["reason"].(string), "different builds") {
		t.Fatalf("stderr = %s", stderr)
	}
	if got, _ := os.ReadFile(manifest); !bytes.Equal(got, original) {
		t.Fatal("a refused stamp changed the manifest")
	}
}

// TestPatchManifestTakesTheVersionDirectlyWhereNoBuildInfoExists: a release APK packages CI's loader and has no build tree.
func TestPatchManifestTakesTheVersionDirectlyWhereNoBuildInfoExists(t *testing.T) {
	t.Parallel()
	major := currentMajor(t)
	version := itoa(major) + releaseSuffix
	code := major*majorScale + rel16Scale
	manifest := writeFile(t, manifestName, storeManifest(intOf64(t, storeCodeText)))
	shim := writeFile(t, "shim.so", []byte(shimPrefix+version+"\x00"))
	c, stdout, stderr := runTool(t, "patch-manifest", manifest, "-version", version, "-shim", shim, "-repo", repoPath(t))
	if c != 0 || strings.TrimSpace(stdout) != itoa(code) {
		t.Fatalf("exit %d, stdout %q, stderr %s", c, stdout, stderr)
	}
	if got, _ := os.ReadFile(manifest); !bytes.Equal(got, storeManifest(int64(code))) {
		t.Fatal("the manifest is not the document built with the release code")
	}
}

// TestPatchManifestWithAVersionStillRefusesAShimThatDoesNotCarryIt.
func TestPatchManifestWithAVersionStillRefusesAShimThatDoesNotCarryIt(t *testing.T) {
	t.Parallel()
	major := currentMajor(t)
	version := itoa(major) + releaseSuffix
	original := storeManifest(intOf64(t, storeCodeText))
	manifest := writeFile(t, manifestName, original)
	shim := writeFile(t, "shim.so", []byte("\x00"+itoa(major)+".16.1\x00"))
	c, _, stderr := runTool(t, "patch-manifest", manifest, "-version", version, "-shim", shim, "-repo", repoPath(t))
	if c != exitRefused {
		t.Fatalf("exit %d", c)
	}
	if !strings.Contains(lastJSON(t, stderr)["reason"].(string), version) {
		t.Fatalf("stderr = %s", stderr)
	}
	if got, _ := os.ReadFile(manifest); !bytes.Equal(got, original) {
		t.Fatal("manifest changed")
	}
}

// TestPatchManifestRefusesAVersionBelowTheNewestMajorAndWritesNothing: a previous major, and "1.0.0" (a build with no git).
func TestPatchManifestRefusesAVersionBelowTheNewestMajorAndWritesNothing(t *testing.T) {
	t.Parallel()
	major := currentMajor(t)
	for _, low := range []string{itoa(major-1) + ".20.0", "1.0.0"} {
		t.Run(low, func(t *testing.T) {
			t.Parallel()
			original := storeManifest(intOf64(t, storeCodeText))
			manifest := writeFile(t, manifestName, original)
			shim := writeFile(t, "shim.so", []byte("\x00"+low+"\x00"))
			c, stdout, stderr := runTool(t, "patch-manifest", manifest, "-version", low, "-shim", shim, "-repo", repoPath(t))
			if c != exitRefused || stdout != "" {
				t.Fatalf("exit %d, stdout %q", c, stdout)
			}
			if !strings.Contains(lastJSON(t, stderr)["reason"].(string), "below major "+itoa(major)) {
				t.Fatalf("stderr = %s", stderr)
			}
			if got, _ := os.ReadFile(manifest); !bytes.Equal(got, original) {
				t.Fatal("manifest changed")
			}
		})
	}
}

// TestPatchManifestNeedsExactlyOneSourceOfTheVersion: neither and both are a bad command line, and the manifest is left.
func TestPatchManifestNeedsExactlyOneSourceOfTheVersion(t *testing.T) {
	t.Parallel()
	original := storeManifest(intOf64(t, storeCodeText))
	manifest := writeFile(t, manifestName, original)
	header := writeFile(t, "nevr_build_info.h", []byte(`constexpr const char* kVersion = "5.16.2";`+"\n"))
	shim := writeFile(t, "shim.so", []byte("\x00"+"5.16.2"+"\x00"))
	neither, _, _ := runTool(t, "patch-manifest", manifest, "-shim", shim)
	both, _, _ := runTool(t, "patch-manifest", manifest, "-version", "5.16.2", "-build-info", header, "-shim", shim)
	if neither != exitUsage || both != exitUsage {
		t.Fatalf("exits %d and %d, want %d", neither, both, exitUsage)
	}
	if got, _ := os.ReadFile(manifest); !bytes.Equal(got, original) {
		t.Fatal("manifest changed")
	}
}

// TestAnApkWithTwoManifestsIsRefused: a duplicate AndroidManifest.xml is not read.
func TestAnApkWithTwoManifestsIsRefused(t *testing.T) {
	t.Parallel()
	apk := filepath.Join(t.TempDir(), "dup.apk")
	f, err := os.Create(apk)
	if err != nil {
		t.Fatal(err)
	}
	zw := zip.NewWriter(f)
	for _, data := range [][]byte{storeManifest(intOf64(t, storeCodeText)), storeManifest(intOf64(t, "999999999"))} {
		w, err := zw.Create(manifestName)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := w.Write(data); err != nil {
			t.Fatal(err)
		}
	}
	if err := zw.Close(); err != nil {
		t.Fatal(err)
	}
	f.Close()
	c, _, stderr := runTool(t, "read", apk)
	if c != exitRefused {
		t.Fatalf("exit %d", c)
	}
	if !strings.Contains(lastJSON(t, stderr)["reason"].(string), "2 AndroidManifest.xml entries") {
		t.Fatalf("stderr = %s", stderr)
	}
}

// TestReadPrintsTheCodeOfAnApk: an APK with a manifest prints its code; one without, and a missing file, are refusals.
func TestReadPrintsTheCodeOfAnApk(t *testing.T) {
	t.Parallel()
	apk := filepath.Join(t.TempDir(), "a.apk")
	writeZipEntry(t, apk, manifestName, storeManifest(intOf64(t, storeCodeText)))
	c, stdout, stderr := runTool(t, "read", apk)
	if c != 0 || strings.TrimSpace(stdout) != storeCodeText {
		t.Fatalf("exit %d, stdout %q, stderr %s", c, stdout, stderr)
	}
	bad := filepath.Join(t.TempDir(), "b.apk")
	writeZipEntry(t, bad, "other", []byte("x"))
	if c, _, _ := runTool(t, "read", bad); c != exitRefused {
		t.Fatalf("an APK without a manifest: exit %d", c)
	}
	if c, _, _ := runTool(t, "read", filepath.Join(t.TempDir(), "missing.apk")); c != exitRefused {
		t.Fatalf("a missing APK: exit %d", c)
	}
}

// writeZipEntry writes one entry to a new APK at path.
func writeZipEntry(t *testing.T, path, name string, data []byte) {
	t.Helper()
	f, err := os.Create(path)
	if err != nil {
		t.Fatal(err)
	}
	zw := zip.NewWriter(f)
	w, err := zw.Create(name)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := io.Copy(w, bytes.NewReader(data)); err != nil {
		t.Fatal(err)
	}
	if err := zw.Close(); err != nil {
		t.Fatal(err)
	}
	f.Close()
}
