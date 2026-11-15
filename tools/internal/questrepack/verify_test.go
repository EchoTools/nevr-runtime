package questrepack

import (
	"bytes"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/EchoTools/nevr-runtime/tools/internal/questrepack/repackfixture"
	"github.com/EchoTools/nevr-runtime/tools/internal/questruntimepin"
	pinfx "github.com/EchoTools/nevr-runtime/tools/internal/questruntimepin/fixture"
	"github.com/EchoTools/nevr-runtime/tools/internal/testkit"
)

// The verify-signed tests (VerifySignedTest in the Python file): the signed APK against the store APK, the staged files
// and the patcher's output (ADR 0021 section 9, test 7). The patcher is the stand-in (repackfixture.Fakes).

const (
	runtimePath    = abiDir + "/libvrapi.so"
	shortTimeout   = 100 * time.Millisecond // the Python test's 1 s patcher timeout, shortened to a bound that is still quick
	storeLibr15    = "the game"
	pluginName     = "libplugin.so"
	manifestStaged = "libnevr_plugin_manifest.so"
)

// verifyFixture is one run's inputs: the store APK and the signed APK as name -> bytes, the staged files, and a patcher.
type verifyFixture struct {
	t         *testing.T
	tmp       string
	store     map[string][]byte
	signed    map[string][]byte
	staged    map[string][]byte
	patcher   string
	fake      *testkit.FakeCommand
	extraRows string
	timeout   time.Duration
	header    string // the data step's header, resolved before any test changes directory
}

// newVerifyFixture is the Python setUp: the store, the signed APK that keeps every store library but the two the recipe
// replaces, the staged files, and the stand-in patcher in the ok mode.
func newVerifyFixture(t *testing.T) *verifyFixture {
	t.Helper()
	f := &verifyFixture{
		t:   t,
		tmp: t.TempDir(),
		store: map[string][]byte{
			"AndroidManifest.xml":                   []byte("manifest"),
			"lib/arm64-v8a/libovrplatformloader.so": []byte("store loader"),
			"lib/arm64-v8a/libr15.so":               []byte(storeLibr15),
			"lib/arm64-v8a/libpnsovr.so":            []byte("pns"),
			"lib/armeabi-v7a/libold.so":             []byte("old abi"),
		},
		staged: map[string][]byte{
			"libovrplatformloader.so":      []byte("the shim"),
			"libovrplatformloader_orig.so": []byte("store loader, renamed"),
			"libr15.so":                    []byte(storeLibr15 + repackfixture.Marker),
			pluginName:                     []byte("a plugin"),
		},
		timeout: patcherTimeoutDefault,
		header:  planHeaderFor(t),
	}
	f.signed = map[string][]byte{}
	for k, v := range f.store {
		if k != "lib/arm64-v8a/libovrplatformloader.so" && k != "lib/arm64-v8a/libr15.so" {
			f.signed[k] = v
		}
	}
	for k, v := range f.staged {
		f.signed[abiDir+"/"+k] = v
	}
	f.setPatcher(repackfixture.ModeOK)
	return f
}

// setPatcher makes the stand-in in mode and points the run at it.
func (f *verifyFixture) setPatcher(mode string) {
	f.t.Helper()
	f.fake = testkit.Fake(f.t, mode)
	f.patcher = filepath.Join(f.fake.Dir(), mode)
}

// run writes the store, the signed APK and the stage, and runs verify-signed on them.
func (f *verifyFixture) run() ([]string, string) {
	f.t.Helper()
	source := zipOfMap(f.t, filepath.Join(f.tmp, "store.apk"), f.store)
	signed := zipOfMap(f.t, filepath.Join(f.tmp, "signed.apk"), f.signed)
	stage := filepath.Join(f.tmp, "stage")
	if err := os.RemoveAll(stage); err != nil {
		f.t.Fatalf("%v", err)
	}
	for name, data := range f.staged {
		writeFile(f.t, filepath.Join(stage, abiDir, name), data)
	}
	var out bytes.Buffer
	problems, err := VerifySigned(VerifyOptions{
		Source: source, Signed: signed, Stage: stage, Patcher: f.patcher, ExtraRows: f.extraRows,
		Scratch: f.tmp, PlanHeader: f.header, PatcherTimeout: f.timeout, Out: &out,
	})
	if err != nil {
		f.t.Fatalf("verify-signed: %v", err)
	}
	return problems, out.String()
}

// onlyProblem is the one problem of the run; the test fails when there are not exactly one.
func (f *verifyFixture) onlyProblem() string {
	f.t.Helper()
	problems, _ := f.run()
	if len(problems) != 1 {
		f.t.Fatalf("want exactly one problem, got %d: %q", len(problems), problems)
	}
	return problems[0]
}

// planHeaderFor is the data step's header in this checkout, for the pack's pins.
func planHeaderFor(t *testing.T) string {
	return filepath.Join(repackfixture.RepoRoot(t), planHeaderPath)
}

// zipOfMap writes a zip of the entries in name order (the order makes the bytes the same every time).
func zipOfMap(t *testing.T, path string, files map[string][]byte) string {
	t.Helper()
	names := make([]string, 0, len(files))
	for n := range files {
		names = append(names, n)
	}
	sortStringsInPlace(names)
	entries := make([]entry, 0, len(names))
	for _, n := range names {
		entries = append(entries, entry{n, files[n]})
	}
	return zipOf(t, path, entries...)
}

// sortStringsInPlace sorts names in place (insertion sort: the maps here are small).
func sortStringsInPlace(names []string) {
	for i := 1; i < len(names); i++ {
		for j := i; j > 0 && names[j-1] > names[j]; j-- {
			names[j-1], names[j] = names[j], names[j-1]
		}
	}
}

// addManifest is the Python add_manifest: the staged manifest and the signed APK holding the same bytes.
func (f *verifyFixture) addManifest(plugins []map[string]any) {
	f.t.Helper()
	text, err := json.Marshal(map[string]any{"schema": 1, "plugins": plugins})
	if err != nil {
		f.t.Fatalf("%v", err)
	}
	text = append(text, '\n')
	f.staged[manifestStaged] = text
	f.signed[abiDir+"/"+manifestStaged] = text
}

// pluginEntry is one plugin of a manifest the tests write.
func pluginEntry(sha string) []map[string]any {
	return []map[string]any{{"name": "plugin", "file": pluginName, "enabled": true, "early": false, "sha256": sha}}
}

// useRuntime is the Python use_runtime: the source APK carries the reference runtime; the staged and signed copy is copy,
// by default the runtime pinned to the md5 of the staged libr15.so. It returns that copy.
func (f *verifyFixture) useRuntime(copyData []byte) []byte {
	f.store[runtimePath] = pinfx.MakeRuntime(pinfx.OLD)
	if copyData == nil {
		copyData = pinfx.MakeRuntime(md5Hex(f.staged["libr15.so"]))
	}
	f.staged["libvrapi.so"] = copyData
	f.signed[runtimePath] = copyData
	return copyData
}

// TestVerifySigned_EverythingSurvived is test_an_apk_where_everything_survived_has_no_problems.
func TestVerifySigned_EverythingSurvived(t *testing.T) {
	t.Parallel()
	if problems, _ := newVerifyFixture(t).run(); len(problems) != 0 {
		t.Fatalf("want none, got %q", problems)
	}
}

// TestVerifySigned_ManifestHashNotTheStagedPlugins is test_verify_signed_refuses_a_manifest_whose_hash_is_not_the_staged_plugins.
func TestVerifySigned_ManifestHashNotTheStagedPlugins(t *testing.T) {
	t.Parallel()
	f := newVerifyFixture(t)
	f.addManifest(pluginEntry(strings.Repeat("0", 64)))
	problems, _ := f.run()
	if !hasSub(problems, manifestStaged) || !hasSub(problems, "sha256") {
		t.Fatalf("got %q", problems)
	}
}

// TestVerifySigned_StagedPluginNotListed is test_verify_signed_refuses_a_staged_plugin_the_manifest_does_not_list.
func TestVerifySigned_StagedPluginNotListed(t *testing.T) {
	t.Parallel()
	f := newVerifyFixture(t)
	f.addManifest(nil)
	problems, _ := f.run()
	if !hasSub(problems, pluginName) || !hasSub(problems, "manifest") {
		t.Fatalf("got %q", problems)
	}
}

// TestVerifySigned_ShimMayDiffer is test_the_loader_the_shim_replaces_may_differ.
func TestVerifySigned_ShimMayDiffer(t *testing.T) {
	t.Parallel()
	f := newVerifyFixture(t)
	f.signed["lib/arm64-v8a/libovrplatformloader.so"] = []byte("the shim")
	if problems, _ := f.run(); len(problems) != 0 {
		t.Fatalf("want none, got %q", problems)
	}
}

// TestVerifySigned_StoreLibraryMissing is test_a_store_library_missing_from_the_signed_apk_is_named.
func TestVerifySigned_StoreLibraryMissing(t *testing.T) {
	t.Parallel()
	f := newVerifyFixture(t)
	delete(f.signed, "lib/arm64-v8a/libpnsovr.so")
	p := f.onlyProblem()
	if !strings.Contains(p, "lib/arm64-v8a/libpnsovr.so") || !strings.Contains(p, "missing from the signed APK") {
		t.Fatalf("got %q", p)
	}
}

// TestVerifySigned_StoreLibraryOtherBytes is test_a_store_library_with_other_bytes_is_named.
func TestVerifySigned_StoreLibraryOtherBytes(t *testing.T) {
	t.Parallel()
	f := newVerifyFixture(t)
	f.signed["lib/arm64-v8a/libpnsovr.so"] = []byte("a plugin took this name")
	p := f.onlyProblem()
	if !strings.Contains(p, "lib/arm64-v8a/libpnsovr.so") || !strings.Contains(p, "different bytes than the store APK") {
		t.Fatalf("got %q", p)
	}
}

// TestVerifySigned_OtherABIHeldToTheSameRule is test_a_library_of_another_abi_is_held_to_the_same_rule.
func TestVerifySigned_OtherABIHeldToTheSameRule(t *testing.T) {
	t.Parallel()
	f := newVerifyFixture(t)
	delete(f.signed, "lib/armeabi-v7a/libold.so")
	f.onlyProblem()
}

// TestVerifySigned_StagedLibraryMissing is test_a_staged_library_missing_from_the_signed_apk_is_named.
func TestVerifySigned_StagedLibraryMissing(t *testing.T) {
	t.Parallel()
	f := newVerifyFixture(t)
	delete(f.signed, abiDir+"/"+pluginName)
	p := f.onlyProblem()
	if !strings.Contains(p, pluginName+": staged, missing") {
		t.Fatalf("got %q", p)
	}
}

// TestVerifySigned_StagedLibraryOtherBytes is test_a_staged_library_with_other_bytes_is_named.
func TestVerifySigned_StagedLibraryOtherBytes(t *testing.T) {
	t.Parallel()
	f := newVerifyFixture(t)
	f.signed[abiDir+"/libovrplatformloader_orig.so"] = []byte("something else")
	if p := f.onlyProblem(); !strings.Contains(p, "different bytes than the staged file") {
		t.Fatalf("got %q", p)
	}
}

// TestVerifySigned_EveryProblemReported is test_every_problem_is_reported_not_only_the_first.
func TestVerifySigned_EveryProblemReported(t *testing.T) {
	t.Parallel()
	f := newVerifyFixture(t)
	delete(f.signed, "lib/arm64-v8a/libpnsovr.so")
	delete(f.signed, abiDir+"/"+pluginName)
	if problems, _ := f.run(); len(problems) != 2 {
		t.Fatalf("want 2, got %q", problems)
	}
}

// TestVerifySigned_SignedLibR15IsThePatcherOutput is test_the_signed_libr15_is_the_patchers_output_and_the_command_line_says_ok
// (the library half: no problems; the OK line is the command's, in its tests).
func TestVerifySigned_SignedLibR15IsThePatcherOutput(t *testing.T) {
	t.Parallel()
	if problems, _ := newVerifyFixture(t).run(); len(problems) != 0 {
		t.Fatalf("want none, got %q", problems)
	}
}

// TestVerifySigned_SignedLibR15IsTheStoreBytes is test_a_signed_libr15_that_is_the_store_bytes_is_refused.
func TestVerifySigned_SignedLibR15IsTheStoreBytes(t *testing.T) {
	t.Parallel()
	f := newVerifyFixture(t)
	f.signed[libr15Path] = []byte(storeLibr15)
	f.staged["libr15.so"] = []byte(storeLibr15)
	p := f.onlyProblem()
	if !strings.Contains(p, "lib/arm64-v8a/libr15.so: the signed APK does not hold the patcher's output") ||
		!strings.Contains(p, sha([]byte(storeLibr15))) {
		t.Fatalf("got %q", p)
	}
}

// TestVerifySigned_OtherInputPatched is test_a_signed_libr15_made_by_the_patcher_from_another_input_is_refused.
func TestVerifySigned_OtherInputPatched(t *testing.T) {
	t.Parallel()
	f := newVerifyFixture(t)
	other := []byte("another libr15" + repackfixture.Marker)
	f.signed[libr15Path] = other
	f.staged["libr15.so"] = other
	p := f.onlyProblem()
	if !strings.Contains(p, "does not hold the patcher's output") || !strings.Contains(p, sha(other)) ||
		!strings.Contains(p, sha([]byte(storeLibr15+repackfixture.Marker))) {
		t.Fatalf("got %q", p)
	}
}

// TestVerifySigned_PatcherExitsNonZero is test_a_patcher_that_exits_non_zero_is_refused_with_its_message.
func TestVerifySigned_PatcherExitsNonZero(t *testing.T) {
	t.Parallel()
	f := newVerifyFixture(t)
	f.setPatcher(repackfixture.ModeFail)
	p := f.onlyProblem()
	if !strings.Contains(p, "exited 3") || !strings.Contains(p, "standin patcher: refused") {
		t.Fatalf("got %q", p)
	}
}

// TestVerifySigned_PatcherWritesNoFile is test_a_patcher_that_writes_no_file_is_refused.
func TestVerifySigned_PatcherWritesNoFile(t *testing.T) {
	t.Parallel()
	f := newVerifyFixture(t)
	f.setPatcher(repackfixture.ModeNoWrite)
	if p := f.onlyProblem(); !strings.Contains(p, "exited 0 and wrote no file") {
		t.Fatalf("got %q", p)
	}
}

// TestVerifySigned_PatcherWritesEmptyFile is test_a_patcher_that_writes_an_empty_file_is_refused.
func TestVerifySigned_PatcherWritesEmptyFile(t *testing.T) {
	t.Parallel()
	f := newVerifyFixture(t)
	f.setPatcher(repackfixture.ModeEmpty)
	if p := f.onlyProblem(); !strings.Contains(p, "exited 0 and wrote no file") {
		t.Fatalf("got %q", p)
	}
}

// TestVerifySigned_PatcherNotThere is test_a_patcher_that_is_not_there_is_refused.
func TestVerifySigned_PatcherNotThere(t *testing.T) {
	t.Parallel()
	f := newVerifyFixture(t)
	f.patcher = filepath.Join(f.tmp, "no-such-patcher")
	if p := f.onlyProblem(); !strings.Contains(p, "did not run to its end") {
		t.Fatalf("got %q", p)
	}
}

// TestVerifySigned_PatcherOutputIsTheStoreFile is test_a_patcher_whose_output_is_the_store_file_is_refused.
func TestVerifySigned_PatcherOutputIsTheStoreFile(t *testing.T) {
	t.Parallel()
	f := newVerifyFixture(t)
	f.setPatcher(repackfixture.ModeSame)
	f.signed[libr15Path] = []byte(storeLibr15)
	f.staged["libr15.so"] = []byte(storeLibr15)
	if p := f.onlyProblem(); !strings.Contains(p, "nothing was patched") {
		t.Fatalf("got %q", p)
	}
}

// TestVerifySigned_LibR15MissingFromSigned is test_a_libr15_missing_from_the_signed_apk_is_refused_by_name.
func TestVerifySigned_LibR15MissingFromSigned(t *testing.T) {
	t.Parallel()
	f := newVerifyFixture(t)
	delete(f.signed, libr15Path)
	delete(f.staged, "libr15.so")
	p := f.onlyProblem()
	if !strings.Contains(p, libr15Path) || !strings.Contains(p, "missing from the signed APK") {
		t.Fatalf("got %q", p)
	}
}

// TestVerifySigned_StoreAPKWithoutLibR15 is test_a_store_apk_without_libr15_is_refused.
func TestVerifySigned_StoreAPKWithoutLibR15(t *testing.T) {
	t.Parallel()
	f := newVerifyFixture(t)
	delete(f.store, libr15Path)
	if problems, _ := f.run(); !hasSub(problems, "missing from the store APK") {
		t.Fatalf("got %q", problems)
	}
}

// TestVerifySigned_DigestsPrintedOnSuccessAndRefusal is test_the_three_digests_are_printed_on_success_and_on_refusal.
func TestVerifySigned_DigestsPrintedOnSuccessAndRefusal(t *testing.T) {
	t.Parallel()
	for _, refuse := range []bool{false, true} {
		f := newVerifyFixture(t)
		if refuse {
			f.signed[libr15Path] = []byte(storeLibr15)
		}
		problems, out := f.run()
		if (len(problems) > 0) != refuse {
			t.Fatalf("refuse=%v: problems %q", refuse, problems)
		}
		signed := []byte(storeLibr15 + repackfixture.Marker)
		if refuse {
			signed = []byte(storeLibr15)
		}
		for _, want := range []string{"sha256 store   " + sha([]byte(storeLibr15)),
			"sha256 patched " + sha([]byte(storeLibr15+repackfixture.Marker)), "sha256 signed  " + sha(signed)} {
			if !strings.Contains(out, want) {
				t.Fatalf("refuse=%v: output lacks %q:\n%s", refuse, want, out)
			}
		}
	}
}

// flagsOf is the recorded command line of a run as flag -> value.
func flagsOf(args []string) map[string]string {
	out := map[string]string{}
	for i := 0; i+1 < len(args); i += 2 {
		out[args[i]] = args[i+1]
	}
	return out
}

// TestVerifySigned_PatcherGivenStoreFileAndExtraRows is test_the_patcher_is_given_the_store_file_and_the_extra_rows_file.
func TestVerifySigned_PatcherGivenStoreFileAndExtraRows(t *testing.T) {
	t.Parallel()
	f := newVerifyFixture(t)
	rows := filepath.Join(f.tmp, "rows.json")
	writeFile(t, rows, []byte("{}"))
	f.extraRows = rows
	if problems, _ := f.run(); len(problems) != 0 {
		t.Fatalf("got %q", problems)
	}
	flags := flagsOf(f.fake.Calls(t)[0].Args)
	keys := make([]string, 0, len(flags))
	for k := range flags {
		keys = append(keys, k)
	}
	sortStringsInPlace(keys)
	if strings.Join(keys, ",") != "--extra-rows,--out,--report,--stock" {
		t.Fatalf("flags %q", keys)
	}
	if flags["--extra-rows"] != rows {
		t.Fatalf("--extra-rows = %q", flags["--extra-rows"])
	}
}

// TestVerifySigned_WithoutExtraRowsPatcherGetsNone is test_without_extra_rows_the_patcher_is_given_none.
func TestVerifySigned_WithoutExtraRowsPatcherGetsNone(t *testing.T) {
	t.Parallel()
	f := newVerifyFixture(t)
	if problems, _ := f.run(); len(problems) != 0 {
		t.Fatalf("got %q", problems)
	}
	if _, ok := flagsOf(f.fake.Calls(t)[0].Args)["--extra-rows"]; ok {
		t.Fatal("--extra-rows was given")
	}
}

// TestVerifySigned_PatcherRunsOnACopyRemovedAfter is test_the_patcher_runs_on_a_copy_of_the_store_file_in_a_directory_that_is_removed.
func TestVerifySigned_PatcherRunsOnACopyRemovedAfter(t *testing.T) {
	t.Parallel()
	f := newVerifyFixture(t)
	before := dirNames(t, f.tmp)
	if problems, _ := f.run(); len(problems) != 0 {
		t.Fatalf("got %q", problems)
	}
	stock := filepath.Dir(flagsOf(f.fake.Calls(t)[0].Args)["--stock"])
	if filepath.Dir(stock) != f.tmp {
		t.Fatalf("the copy's directory %s is not under the scratch %s", stock, f.tmp)
	}
	if exists(stock) {
		t.Fatalf("the copy's directory %s was not removed", stock)
	}
	added := map[string]bool{}
	for n := range dirNames(t, f.tmp) {
		if !before[n] {
			added[n] = true
		}
	}
	want := map[string]bool{"store.apk": true, "signed.apk": true, "stage": true}
	if len(added) != len(want) {
		t.Fatalf("new entries %v, want %v", sortedNames(added), sortedNames(want))
	}
	for n := range want {
		if !added[n] {
			t.Fatalf("new entries %v, want %v", sortedNames(added), sortedNames(want))
		}
	}
}

// dirNames is the set of entry names in dir.
func dirNames(t *testing.T, dir string) map[string]bool {
	t.Helper()
	entries, err := os.ReadDir(dir)
	if err != nil {
		t.Fatalf("%v", err)
	}
	out := map[string]bool{}
	for _, e := range entries {
		out[e.Name()] = true
	}
	return out
}

// TestVerifySigned_PatcherPathDotSlash is test_a_patcher_given_as_a_dot_slash_path_is_run_as_that_file_not_looked_up_on_path.
// It changes the working directory, so it is not parallel: no other test shares the process while it runs.
func TestVerifySigned_PatcherPathDotSlash(t *testing.T) {
	f := newVerifyFixture(t)
	here, err := os.Getwd()
	if err != nil {
		t.Fatalf("%v", err)
	}
	t.Cleanup(func() {
		if err := os.Chdir(here); err != nil {
			t.Fatalf("%v", err)
		}
	})
	if err := os.Chdir(f.fake.Dir()); err != nil {
		t.Fatalf("%v", err)
	}
	f.patcher = "./" + repackfixture.ModeOK
	if problems, _ := f.run(); len(problems) != 0 {
		t.Fatalf("got %q", problems)
	}
	if n := len(f.fake.Calls(t)); n != 1 {
		t.Fatalf("the patcher ran %d times, want 1", n)
	}
}

// TestVerifySigned_PatcherThatDoesNotEnd is test_a_patcher_that_does_not_end_is_stopped_and_refused.
func TestVerifySigned_PatcherThatDoesNotEnd(t *testing.T) {
	t.Parallel()
	f := newVerifyFixture(t)
	f.setPatcher(repackfixture.ModeHang)
	f.timeout = shortTimeout
	if p := f.onlyProblem(); !strings.Contains(p, "did not run to its end") {
		t.Fatalf("got %q", p)
	}
}

// TestVerifySigned_LibR15ComparedWithThisStoreFile is test_the_signed_libr15_is_compared_with_the_patcher_run_on_this_store_file.
func TestVerifySigned_LibR15ComparedWithThisStoreFile(t *testing.T) {
	t.Parallel()
	f := newVerifyFixture(t)
	f.store[libr15Path] = []byte("a different store libr15")
	if p := f.onlyProblem(); !strings.Contains(p, "does not hold the patcher's output") {
		t.Fatalf("got %q", p)
	}
}

// The reference runtime of the source APK and its pinned copies (the #1737 cases).
func TestVerifySigned_PinnedRuntimePasses(t *testing.T) {
	t.Parallel()
	f := newVerifyFixture(t)
	copyData := f.useRuntime(nil)
	problems, out := f.run()
	if len(problems) != 0 {
		t.Fatalf("got %q", problems)
	}
	if !strings.Contains(out, "libvrapi.so sha256 source  "+sha(pinfx.MakeRuntime(pinfx.OLD))) ||
		!strings.Contains(out, "libvrapi.so sha256 signed  "+sha(copyData)) {
		t.Fatalf("digests not printed:\n%s", out)
	}
}

// TestVerifySigned_UnpinnedRuntimeRefused is test_an_unpinned_runtime_is_refused_by_name.
func TestVerifySigned_UnpinnedRuntimeRefused(t *testing.T) {
	t.Parallel()
	f := newVerifyFixture(t)
	f.useRuntime(pinfx.MakeRuntime(pinfx.OLD))
	p := f.onlyProblem()
	if !strings.Contains(p, "lib/arm64-v8a/libvrapi.so: the signed APK holds the source APK's runtime unpinned") ||
		!strings.Contains(p, md5Hex(f.staged["libr15.so"])) {
		t.Fatalf("got %q", p)
	}
}

// TestVerifySigned_RuntimePinnedToAnotherLibR15 is test_a_runtime_pinned_to_another_libr15_is_refused.
func TestVerifySigned_RuntimePinnedToAnotherLibR15(t *testing.T) {
	t.Parallel()
	f := newVerifyFixture(t)
	f.useRuntime(pinfx.MakeRuntime(md5Hex([]byte("another libr15"))))
	if p := f.onlyProblem(); !strings.Contains(p, "not hold the source APK's runtime pinned to the signed libr15.so") {
		t.Fatalf("got %q", p)
	}
}

// TestVerifySigned_RuntimePinnedToStoreLibR15 is test_a_runtime_pinned_to_the_store_libr15_is_refused.
func TestVerifySigned_RuntimePinnedToStoreLibR15(t *testing.T) {
	t.Parallel()
	f := newVerifyFixture(t)
	f.useRuntime(pinfx.MakeRuntime(md5Hex([]byte(storeLibr15))))
	if p := f.onlyProblem(); !strings.Contains(p, "not hold the source APK's runtime pinned to the signed libr15.so") {
		t.Fatalf("got %q", p)
	}
}

// TestVerifySigned_RuntimeMissingFromSigned is test_a_runtime_missing_from_the_signed_apk_is_refused.
func TestVerifySigned_RuntimeMissingFromSigned(t *testing.T) {
	t.Parallel()
	f := newVerifyFixture(t)
	f.useRuntime(nil)
	delete(f.signed, runtimePath)
	delete(f.staged, "libvrapi.so")
	p := f.onlyProblem()
	if !strings.Contains(p, "lib/arm64-v8a/libvrapi.so: the pinned runtime belongs here, missing from the signed APK") {
		t.Fatalf("got %q", p)
	}
}

// TestVerifySigned_RuntimeThatCannotBePinned is test_a_runtime_that_cannot_be_pinned_is_refused_with_the_reason.
func TestVerifySigned_RuntimeThatCannotBePinned(t *testing.T) {
	t.Parallel()
	f := newVerifyFixture(t)
	f.useRuntime(nil)
	f.store[runtimePath] = pinfx.MakeRuntime(pinfx.OLD, pinfx.WithSites(11))
	p := f.onlyProblem()
	if !strings.Contains(p, "cannot be pinned to the signed libr15.so") || !strings.Contains(p, "44 immediate runs") {
		t.Fatalf("got %q", p)
	}
}

// TestVerifySigned_DamagedFirstEntryNotPinnedElsewhere is test_a_source_runtime_whose_first_entry_is_damaged_is_refused_not_pinned_elsewhere.
func TestVerifySigned_DamagedFirstEntryNotPinnedElsewhere(t *testing.T) {
	t.Parallel()
	f := newVerifyFixture(t)
	f.useRuntime(nil)
	f.store[runtimePath] = bytes.Replace(pinfx.MakeRuntime(pinfx.OLD), []byte(pinfx.OLD), []byte(strings.Repeat("x", 32)), 1)
	p := f.onlyProblem()
	if !strings.Contains(p, "cannot be pinned to the signed libr15.so") ||
		!strings.Contains(p, "is "+pinfx.SECOND+", expected "+pinfx.OLD) {
		t.Fatalf("got %q", p)
	}
}

// TestVerifySigned_RuntimeWithAnotherEntryReplaced is test_a_runtime_with_another_entry_replaced_is_refused.
func TestVerifySigned_RuntimeWithAnotherEntryReplaced(t *testing.T) {
	t.Parallel()
	f := newVerifyFixture(t)
	md5 := md5Hex(f.staged["libr15.so"])
	wrong, _, err := questruntimepin.PinData(pinfx.MakeRuntime(pinfx.OLD), md5, 1, pinfx.SECOND)
	if err != nil {
		t.Fatalf("%v", err)
	}
	f.useRuntime(wrong)
	if p := f.onlyProblem(); !strings.Contains(p, "not hold the source APK's runtime pinned to the signed libr15.so") {
		t.Fatalf("got %q", p)
	}
}

// TestVerifySigned_ChecksumlessRuntimeHeldToStoreBytes is test_a_libvrapi_with_no_checksum_list_is_held_to_the_store_bytes.
func TestVerifySigned_ChecksumlessRuntimeHeldToStoreBytes(t *testing.T) {
	t.Parallel()
	storeVrapi := pinfx.MakeElf(make([]byte, 8), []byte("the store's own libvrapi, no md5 list\x00"))
	f := newVerifyFixture(t)
	f.store[runtimePath] = storeVrapi
	f.signed[runtimePath] = storeVrapi
	if problems, _ := f.run(); len(problems) != 0 {
		t.Fatalf("got %q", problems)
	}
	f.signed[runtimePath] = append(append([]byte(nil), storeVrapi...), 'x')
	if p := f.onlyProblem(); !strings.Contains(p, "lib/arm64-v8a/libvrapi.so: the signed APK holds different bytes than the store APK") {
		t.Fatalf("got %q", p)
	}
}

// TestVerifySigned_PinnedRuntimeBesidePluginManifest is test_a_pinned_runtime_staged_beside_a_plugin_manifest_is_not_an_unlisted_plugin.
func TestVerifySigned_PinnedRuntimeBesidePluginManifest(t *testing.T) {
	t.Parallel()
	f := newVerifyFixture(t)
	f.useRuntime(nil)
	f.addManifest(pluginEntry(sha(f.staged[pluginName])))
	if problems, _ := f.run(); len(problems) != 0 {
		t.Fatalf("got %q", problems)
	}
}
