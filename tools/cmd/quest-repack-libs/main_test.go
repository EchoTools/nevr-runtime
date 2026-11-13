package main

import (
	"archive/zip"
	"bytes"
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/questrepack/repackfixture"
	"github.com/EchoTools/nevr-runtime/tools/internal/testkit"
)

// TestMain makes the test binary the stand-in patcher's modes too (testkit.Main, ADR 0044 rule 10).
func TestMain(m *testing.M) {
	testkit.Main(m, repackfixture.Fakes())
}

// The command-line tests (the Python tests that run tools/cmd/quest-repack-libs as a process): the exit codes, the OK
// lines and the problem lines of each subcommand. run is called in this process; the environment is testkit's.

// runCLI runs the command with args and returns its exit code, standard output and standard error.
func runCLI(t *testing.T, args ...string) (int, string, string) {
	t.Helper()
	env := testkit.Env(t)
	var stdout, stderr bytes.Buffer
	code := run(context.Background(), args, testkit.Getenv(env), nil, &stdout, &stderr)
	return code, stdout.String(), stderr.String()
}

// zipOf writes a zip of (name, data) entries, deflated, at path.
func zipOf(t *testing.T, path string, names []string, datas [][]byte) string {
	t.Helper()
	repackfixture.WriteZip(t, path, zip.Deflate, names, datas)
	return path
}

// TestCheckPluginName_CommandLine is test_the_command_line_exits_one_and_names_the_entry.
func TestCheckPluginName_CommandLine(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	apk := zipOf(t, filepath.Join(dir, "store.apk"), []string{"lib/arm64-v8a/libr15.so"}, [][]byte{[]byte("x")})
	plugin := filepath.Join(dir, "libr15.so")
	writeOne(t, plugin, "p")
	code, stdout, stderr := runCLI(t, "check-plugin-name", "--apk", apk, "--plugin", plugin)
	if code != 1 || !strings.Contains(stderr, "plugin libr15.so would replace lib/arm64-v8a/libr15.so") || strings.Contains(stdout, "OK") {
		t.Fatalf("code %d, stdout %q, stderr %q", code, stdout, stderr)
	}
	ok := filepath.Join(dir, "libok.so")
	writeOne(t, ok, "p")
	code, stdout, stderr = runCLI(t, "check-plugin-name", "--apk", apk, "--plugin", ok)
	if code != 0 || !strings.Contains(stdout, "check-plugin-name OK") {
		t.Fatalf("code %d, stdout %q, stderr %q", code, stdout, stderr)
	}
}

// writeOne writes text at path.
func writeOne(t *testing.T, path, text string) {
	t.Helper()
	if err := os.WriteFile(path, []byte(text), 0o644); err != nil {
		t.Fatalf("%v", err)
	}
}

// TestCheckPluginBuildIDs_CommandLine is test_the_command_reads_the_store_apk_libr15.
func TestCheckPluginBuildIDs_CommandLine(t *testing.T) {
	t.Parallel()
	requireBuildTools(t)
	const (
		gameID  = "b243509c08ce677aeb95fa348016949b3fc45230"
		otherID = "0123456789abcdef0123456789abcdef01234567"
	)
	dir := t.TempDir()
	libr15 := repackfixture.BuildAArch64(t, filepath.Join(dir, "libr15.so"), "int game = 1;\n", gameID)
	apk := zipOf(t, filepath.Join(dir, "store.apk"), []string{"lib/arm64-v8a/libr15.so"}, [][]byte{libr15})
	repackfixture.BuildAArch64(t, filepath.Join(dir, "libother.so"), repackfixture.NevrNote(t, repackfixture.RepoRoot(t), otherID), "")
	badPath := filepath.Join(dir, "libother.so")
	code, _, stderr := runCLI(t, "check-plugin-build-ids", "--apk", apk, "--plugin", badPath)
	if code != 1 || !strings.Contains(stderr, "build_id_unsupported") {
		t.Fatalf("code %d, stderr %q", code, stderr)
	}
	repackfixture.BuildAArch64(t, filepath.Join(dir, "libg.so"), repackfixture.NevrNote(t, repackfixture.RepoRoot(t), gameID), "")
	code, stdout, stderr := runCLI(t, "check-plugin-build-ids", "--apk", apk, "--plugin", filepath.Join(dir, "libg.so"))
	if code != 0 || !strings.Contains(stdout, "check-plugin-build-ids OK") {
		t.Fatalf("code %d, stdout %q, stderr %q", code, stdout, stderr)
	}
}

// requireBuildTools skips when the AArch64 toolchain the build-ID tests use is missing.
func requireBuildTools(t *testing.T) {
	t.Helper()
	for _, tool := range []string{"clang", "ld.lld", "llvm-nm", "llvm-readelf"} {
		if _, err := exec.LookPath(tool); err != nil {
			t.Skipf("needs %s", tool)
		}
	}
}

// TestBuildVariant_CommandLineRefusesAndAccepts is test_the_command_line_refuses_an_unknown_word_with_exit_one and
// test_the_command_line_accepts_the_words_and_the_empty_word.
func TestBuildVariant_CommandLineRefusesAndAccepts(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	stage := filepath.Join(dir, "stage")
	stamp := filepath.Join(stage, "lib", "arm64-v8a", "libnevr_build_variant.so")
	for _, command := range []string{"check-build-variant", "write-build-variant"} {
		args := []string{command, "--variant", "bogus"}
		if command == "write-build-variant" {
			args = append(args, "--stage", stage)
		}
		code, _, stderr := runCLI(t, args...)
		if code != 1 || !strings.Contains(stderr, "not a variant word") || exists(stamp) {
			t.Fatalf("%s: code %d, stderr %q, stamp exists %v", command, code, stderr, exists(stamp))
		}
	}
	for _, word := range []string{"", "q15t", "combat", "perf-probe", "arena"} {
		code, stdout, stderr := runCLI(t, "write-build-variant", "--stage", stage, "--variant", word)
		if code != 0 || !strings.Contains(stdout, "write-build-variant OK") {
			t.Fatalf("%q: code %d, stdout %q, stderr %q", word, code, stdout, stderr)
		}
		got, err := os.ReadFile(stamp)
		if err != nil || string(got) != word {
			t.Fatalf("%q: stamp %q, %v", word, got, err)
		}
	}
}

// TestShimRows_CommandLine is test_the_command_line... of ShimRowsTest: the exit codes of check-shim-rows.
func TestShimRows_CommandLine(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	rows := writeOne2(t, filepath.Join(dir, "rows.json"), `{"format": 1}`+"\n")
	shim := filepath.Join(dir, "libovrplatformloader.so")
	sum := func(path string) string {
		out, err := os.ReadFile(path)
		if err != nil {
			t.Fatalf("%v", err)
		}
		return sha256Hex(out)
	}
	writeOne(t, shim, "\x7fELF public rodata\x00")
	if code, _, stderr := runCLI(t, "check-shim-rows", "--shim", shim); code != 0 {
		t.Fatalf("a public shim: code %d, stderr %q", code, stderr)
	}
	if code, _, _ := runCLI(t, "check-shim-rows", "--shim", shim, "--extra-rows", rows); code != 1 {
		t.Fatalf("a public shim with rows: code %d, want 1", code)
	}
	variant := "\x7fELF public rodata\x00\x00libr15_extra_rows\x00" + sum(rows) + "\x00"
	writeOne(t, shim, variant)
	code, _, stderr := runCLI(t, "check-shim-rows", "--shim", shim)
	if code != 1 || !strings.Contains(stderr, "build_id_mismatch") {
		t.Fatalf("a variant shim without rows: code %d, stderr %q", code, stderr)
	}
	if code, _, stderr := runCLI(t, "check-shim-rows", "--shim", shim, "--extra-rows", rows); code != 0 {
		t.Fatalf("a shim built with the rows: code %d, stderr %q", code, stderr)
	}
	if code, _, _ := runCLI(t, "check-shim-rows", "--shim", filepath.Join(dir, "missing.so")); code != 1 {
		t.Fatalf("a missing shim: code %d, want 1", code)
	}
}

// writeOne2 writes text at path and returns the path.
func writeOne2(t *testing.T, path, text string) string {
	t.Helper()
	writeOne(t, path, text)
	return path
}

// TestStagePack_CommandLine is test_the_command_line_stages_and_prints_each_files_digest.
func TestStagePack_CommandLine(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	store := zipOf(t, filepath.Join(dir, "store.apk"), []string{"AndroidManifest.xml", "lib/arm64-v8a/libr15.so"},
		[][]byte{[]byte("manifest"), []byte("game")})
	header := filepath.Join(repackfixture.RepoRoot(t), "src", "quest", "data", "plan.h")
	version, manifest := pinsOf(t, header)
	prefix := "_data/" + version + "/rad15/android/"
	manifestEntry := prefix + "manifests/" + manifest
	packageEntry := prefix + "packages/" + manifest + "_1"
	packageBytes := []byte("package bytes")
	pack := zipOf(t, filepath.Join(dir, "pack.zip"), []string{manifestEntry, packageEntry},
		[][]byte{[]byte("manifest bytes"), packageBytes})
	meta := filepath.Join(dir, "meta.json")
	writeOne(t, meta, `{"id": "combat-pack", "version": "1.2.3", "requires_stock_manifest_sha256": ["`+sha256Hex([]byte("x"))+`"], "zip_prefix": "`+prefix+`"}`)
	stage := filepath.Join(dir, "stage")
	code, stdout, stderr := runCLI(t, "stage-pack", "--pack-zip", pack, "--meta", meta, "--stage", stage, "--store-apk", store,
		"--plan-header", header)
	if code != 0 || !strings.Contains(stdout, "pack file packages/"+manifest+"_1 sha256 "+sha256Hex(packageBytes)) ||
		!strings.Contains(stdout, "stage-pack OK") {
		t.Fatalf("code %d, stdout %q, stderr %q", code, stdout, stderr)
	}
	refused, _, _ := runCLI(t, "stage-pack", "--pack-zip", pack, "--meta", filepath.Join(dir, "missing.json"), "--stage",
		filepath.Join(dir, "other"), "--store-apk", store, "--plan-header", header)
	if refused != 1 {
		t.Fatalf("a missing meta: code %d, want 1", refused)
	}
}

// TestPackProblems_CommandLine is test_the_command_line_says_ok_or_exits_one (the pack-problems half).
func TestPackProblems_CommandLine(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	stage := filepath.Join(dir, "stage")
	code, _, stderr := runCLI(t, "pack-problems", "--stage", stage, "--plan-header", filepath.Join(repackfixture.RepoRoot(t), "src", "quest", "data", "plan.h"))
	if code != 0 {
		t.Fatalf("nothing staged: code %d, stderr %q", code, stderr)
	}
	if err := os.MkdirAll(filepath.Join(stage, "assets", "nevr-pack", "files"), 0o755); err != nil {
		t.Fatalf("%v", err)
	}
	code, _, stderr = runCLI(t, "pack-problems", "--stage", stage, "--plan-header", filepath.Join(repackfixture.RepoRoot(t), "src", "quest", "data", "plan.h"))
	if code != 1 || !strings.Contains(stderr, "without a pack.json") {
		t.Fatalf("staged files without pack.json: code %d, stderr %q", code, stderr)
	}
}

// TestVerifySigned_CommandLine covers the command-line half of VerifySignedTest: the OK line, the refusal's exit code,
// the usage error with no patcher, the extra rows and the scratch directory given to the patcher, and the digests.
func TestVerifySigned_CommandLine(t *testing.T) {
	t.Parallel()
	fx := newCLIVerify(t, repackfixture.ModeOK)
	code, stdout, stderr := runCLI(t, fx.args()...)
	if code != 0 || !strings.Contains(stdout, "verify-signed OK") {
		t.Fatalf("code %d, stdout %q, stderr %q", code, stdout, stderr)
	}
	for _, want := range []string{"sha256 store   " + sha256Hex([]byte("the game")),
		"sha256 patched " + sha256Hex([]byte("the game"+repackfixture.Marker)), "sha256 signed  " + sha256Hex([]byte("the game"+repackfixture.Marker))} {
		if !strings.Contains(stdout, want) {
			t.Fatalf("stdout lacks %q:\n%s", want, stdout)
		}
	}
	calls := fx.fake.Calls(t)
	if len(calls) != 1 {
		t.Fatalf("the patcher ran %d times", len(calls))
	}
	flags := map[string]string{}
	for i := 0; i+1 < len(calls[0].Args); i += 2 {
		flags[calls[0].Args[i]] = calls[0].Args[i+1]
	}
	if flags["--extra-rows"] != fx.rows {
		t.Fatalf("--extra-rows = %q, want %q", flags["--extra-rows"], fx.rows)
	}
	if filepath.Dir(filepath.Dir(flags["--stock"])) != fx.scratch {
		t.Fatalf("the patcher's copy %q is not under --scratch %q", flags["--stock"], fx.scratch)
	}
	if code, _, stderr := runCLI(t, "verify-signed", "--source", fx.store, "--signed", fx.signed, "--stage", fx.stage); code != 2 ||
		!strings.Contains(stderr, "--patcher") || strings.Contains(stderr, "OK") {
		t.Fatalf("no patcher: code %d, stderr %q", code, stderr)
	}
	fx.setSignedLibr15([]byte("the game"))
	if code, stdout, _ := runCLI(t, fx.args()...); code != 1 || strings.Contains(stdout, "OK") {
		t.Fatalf("a store libr15 signed: code %d, stdout %q", code, stdout)
	}
}

// cliVerify is the Python VerifySignedTest's inputs, for the command line: a store, a signed APK, a stage, a patcher.
type cliVerify struct {
	t       *testing.T
	store   string
	signed  string
	stage   string
	scratch string
	rows    string
	patcher string
	fake    *testkit.FakeCommand
}

func newCLIVerify(t *testing.T, mode string) *cliVerify {
	t.Helper()
	dir := t.TempDir()
	fx := &cliVerify{t: t, scratch: dir, rows: writeOne2(t, filepath.Join(dir, "rows.json"), "{}")}
	fx.stage = filepath.Join(dir, "stage")
	fx.store = zipOf(t, filepath.Join(dir, "store.apk"), []string{"AndroidManifest.xml", "lib/arm64-v8a/libr15.so"},
		[][]byte{[]byte("m"), []byte("the game")})
	fx.signed = zipOf(t, filepath.Join(dir, "signed.apk"), []string{"AndroidManifest.xml", "lib/arm64-v8a/libr15.so"},
		[][]byte{[]byte("m"), []byte("the game" + repackfixture.Marker)})
	if err := os.MkdirAll(filepath.Join(fx.stage, "lib", "arm64-v8a"), 0o755); err != nil {
		t.Fatalf("%v", err)
	}
	writeOne(t, filepath.Join(fx.stage, "lib", "arm64-v8a", "libr15.so"), "the game"+repackfixture.Marker)
	fx.fake = testkit.Fake(t, mode)
	fx.patcher = filepath.Join(fx.fake.Dir(), mode)
	return fx
}

// args is the verify-signed command line of fx.
func (fx *cliVerify) args() []string {
	return []string{"verify-signed", "--source", fx.store, "--signed", fx.signed, "--stage", fx.stage, "--patcher", fx.patcher,
		"--extra-rows", fx.rows, "--scratch", fx.scratch}
}

// setSignedLibr15 rewrites the signed APK with its libr15.so set to data.
func (fx *cliVerify) setSignedLibr15(data []byte) {
	fx.signed = zipOf(fx.t, fx.signed, []string{"AndroidManifest.xml", "lib/arm64-v8a/libr15.so"}, [][]byte{[]byte("m"), data})
}
