package questrepack

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"os"
	"path/filepath"
	"reflect"
	"sort"
	"strings"
	"testing"
)

// The script-library staging tests (StageLibsTest and StageRedirectedLibsTest in the Python file): stage-libs stages a
// combat build's libraries only when each is named, AArch64 and listed; the nine redirected ones are staged all or none.

// zipEntries reads every entry of a zip as (name, data), in archive order.
func zipEntries(t *testing.T, path string) []entry {
	t.Helper()
	z, err := openZip(path)
	if err != nil {
		t.Fatalf("%v", err)
	}
	defer z.Close()
	var out []entry
	for _, f := range z.rc.File {
		data, err := z.readAll(f.Name)
		if err != nil {
			t.Fatalf("%v", err)
		}
		out = append(out, entry{f.Name, data})
	}
	return out
}

// appendEntry rewrites the zip at path with one more entry at the end (the Python test's "a" mode).
func appendEntry(t *testing.T, path, name string, data []byte) {
	t.Helper()
	entries := zipEntries(t, path)
	entries = append(entries, entry{name, data})
	zipOf(t, path, entries...)
}

// storeZip writes the store APK of the tests: its entries in order.
func storeZip(t *testing.T, path string, entries ...entry) string {
	t.Helper()
	return zipOf(t, path, entries...)
}

// libsDirFixture is the Python StageLibsTest: a store with libr15 and an unrelated lib, a libraries directory with two
// AArch64 libraries and their SHA256SUMS, and an empty stage.
type libsDirFixture struct {
	t     *testing.T
	root  string
	store string
	libs  string
	stage string
	names map[string][]byte
}

func newLibsDirFixture(t *testing.T) *libsDirFixture {
	t.Helper()
	root := t.TempDir()
	f := &libsDirFixture{t: t, root: root, libs: filepath.Join(root, "libs"), stage: filepath.Join(root, "stage"),
		names: map[string][]byte{
			"lib0123456789abcdef.so": aarch64SO([]byte("a")),
			"lib00000000000000ff.so": aarch64SO([]byte("b")),
		}}
	f.store = storeZip(t, filepath.Join(root, "store.apk"), entry{"lib/arm64-v8a/libr15.so", []byte("stock")},
		entry{"lib/arm64-v8a/libcollide0123.so", []byte("store")})
	if err := os.MkdirAll(f.libs, 0o755); err != nil {
		t.Fatalf("%v", err)
	}
	if err := os.MkdirAll(f.stage, 0o755); err != nil {
		t.Fatalf("%v", err)
	}
	for name, data := range f.names {
		writeFile(t, filepath.Join(f.libs, name), data)
	}
	f.writeSums(nil)
	return f
}

// writeSums writes SHA256SUMS for the names (sorted), each with its digest of the bytes (override when given).
func (f *libsDirFixture) writeSums(overrides map[string][]byte) {
	f.t.Helper()
	var keys []string
	for k := range f.names {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	var b strings.Builder
	for _, k := range keys {
		data := f.names[k]
		if o, ok := overrides[k]; ok {
			data = o
		}
		b.WriteString(sha(data) + "  " + k + "\n")
	}
	writeFile(f.t, filepath.Join(f.libs, libsSums), []byte(b.String()))
}

func (f *libsDirFixture) stageLibs() []string {
	f.t.Helper()
	problems, err := StageLibs(StageLibsOptions{Libs: f.libs, Stage: f.stage, StoreAPK: f.store, Out: &bytes.Buffer{}})
	if err != nil {
		f.t.Fatalf("stage-libs: %v", err)
	}
	return problems
}

// TestStageLibs_EveryListedLibraryStaged is test_every_listed_library_is_staged_beside_the_plugins_and_no_replacement_is_recorded.
func TestStageLibs_EveryListedLibraryStaged(t *testing.T) {
	t.Parallel()
	f := newLibsDirFixture(t)
	if got := f.stageLibs(); len(got) != 0 {
		t.Fatalf("got %q", got)
	}
	for name, data := range f.names {
		if !bytes.Equal(readFile(t, filepath.Join(f.stage, abiDir, name)), data) {
			t.Fatalf("%s: bytes differ", name)
		}
	}
	if got := readFile(t, filepath.Join(f.stage, stagedReplace)); len(got) != 0 {
		t.Fatalf("REPLACE record %q, want empty", got)
	}
	if got, _ := replacedNames(f.stage); len(got) != 0 {
		t.Fatalf("replaced %q", got)
	}
}

// TestStageLibs_StagedNamesRecordedForTheManifestCheck is test_the_staged_names_are_recorded_so_the_plugin_manifest_check_skips_them.
func TestStageLibs_StagedNamesRecordedForTheManifestCheck(t *testing.T) {
	t.Parallel()
	f := newLibsDirFixture(t)
	if got := f.stageLibs(); len(got) != 0 {
		t.Fatalf("got %q", got)
	}
	var want []string
	for n := range f.names {
		want = append(want, n)
	}
	sort.Strings(want)
	got := stagedScriptLibs(f.stage)
	sort.Strings(got)
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("recorded %q, want %q", got, want)
	}
	abi := filepath.Join(f.stage, abiDir)
	writeFile(t, filepath.Join(abi, "libplugin.so"), []byte("plugin"))
	manifest, err := json.Marshal(map[string]any{"plugins": []any{map[string]any{
		"name": "plugin", "file": "libplugin.so", "enabled": true, "early": false, "sha256": sha([]byte("plugin"))}}})
	if err != nil {
		t.Fatalf("%v", err)
	}
	writeFile(t, filepath.Join(abi, manifestName), manifest)
	if got := pluginManifestProblems(f.stage); len(got) != 0 {
		t.Fatalf("got %q", got)
	}
	writeFile(t, filepath.Join(abi, "libstray.so"), []byte("staged by nothing"))
	got2 := pluginManifestProblems(f.stage)
	if len(got2) != 1 || got2[0] != "lib/arm64-v8a/libstray.so: staged, not listed in "+manifestName {
		t.Fatalf("got %q", got2)
	}
}

// TestStageLibs_UnlistedLibraryRefused is test_an_unlisted_library_is_refused.
func TestStageLibs_UnlistedLibraryRefused(t *testing.T) {
	t.Parallel()
	f := newLibsDirFixture(t)
	writeFile(t, filepath.Join(f.libs, libsSums), []byte(sha(f.names["lib0123456789abcdef.so"])+"  lib0123456789abcdef.so\n"))
	got := f.stageLibs()
	if !hasSub(got, "lib00000000000000ff.so: not listed in SHA256SUMS") {
		t.Fatalf("got %q", got)
	}
	if exists(filepath.Join(f.stage, abiDir)) {
		t.Fatal("a refused set staged files")
	}
}

// TestStageLibs_BytesDifferFromListedDigestRefused is test_a_library_whose_bytes_differ_from_its_listed_digest_is_refused.
func TestStageLibs_BytesDifferFromListedDigestRefused(t *testing.T) {
	t.Parallel()
	f := newLibsDirFixture(t)
	f.writeSums(map[string][]byte{"lib0123456789abcdef.so": aarch64SO([]byte("not what was listed"))})
	if got := f.stageLibs(); !hasSub(got, "lib0123456789abcdef.so: its sha-256 differs") {
		t.Fatalf("got %q", got)
	}
}

// TestStageLibs_ListedNameWithNoFileRefused is test_a_listed_name_with_no_file_is_refused.
func TestStageLibs_ListedNameWithNoFileRefused(t *testing.T) {
	t.Parallel()
	f := newLibsDirFixture(t)
	if err := os.Remove(filepath.Join(f.libs, "lib00000000000000ff.so")); err != nil {
		t.Fatalf("%v", err)
	}
	if got := f.stageLibs(); !hasSub(got, "lib00000000000000ff.so: listed in SHA256SUMS and missing") {
		t.Fatalf("got %q", got)
	}
}

// TestStageLibs_NotAArch64Refused is test_a_file_that_is_not_aarch64_is_refused.
func TestStageLibs_NotAArch64Refused(t *testing.T) {
	t.Parallel()
	f := newLibsDirFixture(t)
	x86 := aarch64SO([]byte("c"))
	binary.LittleEndian.PutUint16(x86[elfMachineOffset:], 62) // EM_X86_64
	f.names["lib0123456789abcdef.so"] = x86
	writeFile(t, filepath.Join(f.libs, "lib0123456789abcdef.so"), x86)
	f.writeSums(nil)
	if got := f.stageLibs(); !hasSub(got, "lib0123456789abcdef.so: not a 64-bit AArch64 shared object") {
		t.Fatalf("got %q", got)
	}
}

// TestStageLibs_StrayNameRefused is test_a_stray_name_is_refused.
func TestStageLibs_StrayNameRefused(t *testing.T) {
	t.Parallel()
	for _, stray := range []string{"libfoo.so", "lib0123456789abcdef.so.bak", "lib0123456789ABCDEF.so"} {
		t.Run(stray, func(t *testing.T) {
			t.Parallel()
			f := newLibsDirFixture(t)
			writeFile(t, filepath.Join(f.libs, stray), aarch64SO([]byte("s")))
			if got := f.stageLibs(); !hasSub(got, stray+": not named lib<hash16>.so") {
				t.Fatalf("got %q", got)
			}
		})
	}
}

// TestStageLibs_StoreNameUnlessREPLACEListsIt is test_a_name_the_store_holds_is_refused_unless_REPLACE_lists_it.
func TestStageLibs_StoreNameUnlessREPLACEListsIt(t *testing.T) {
	t.Parallel()
	f := newLibsDirFixture(t)
	const name = "lib0123456789abcdef.so"
	appendEntry(t, f.store, "lib/arm64-v8a/"+name, []byte("store copy"))
	if got := f.stageLibs(); !hasSub(got, name+": the store APK holds lib/arm64-v8a/"+name) {
		t.Fatalf("got %q", got)
	}
	writeFile(t, filepath.Join(f.libs, libsReplace), []byte(name+"\n"))
	if got := f.stageLibs(); len(got) != 0 {
		t.Fatalf("with REPLACE: got %q", got)
	}
	if got, _ := replacedNames(f.stage); !reflect.DeepEqual(got, []string{name}) {
		t.Fatalf("replaced %q", got)
	}
}

// TestStageLibs_REPLACENamesOnlyAReplacement is test_REPLACE_names_only_a_staged_library_that_replaces_a_store_library.
func TestStageLibs_REPLACENamesOnlyAReplacement(t *testing.T) {
	t.Parallel()
	f := newLibsDirFixture(t)
	writeFile(t, filepath.Join(f.libs, libsReplace), []byte("lib0123456789abcdef.so\n"))
	if got := f.stageLibs(); !hasSub(got, "lib0123456789abcdef.so: REPLACE lists it") {
		t.Fatalf("got %q", got)
	}
}

// redirectFixture is the Python StageRedirectedLibsTest: the nine redirected libraries, a store that holds the nine stock
// names, and a stage.
type redirectFixture struct {
	t     *testing.T
	root  string
	store string
	libs  string
	stage string
	data  []byte
}

const (
	redirectHash  = "37f88adb0d8066f7"
	redirectStock = "lib" + redirectHash + ".so"
	redirected    = "lib" + redirectHash + "_nevr.so"
)

// nineLibs are the nine redirected libraries' names, in the order of the list the tool keeps.
func nineLibs() []string {
	out := make([]string, 0, len(combatRedirectHashes))
	for _, h := range combatRedirectHashes {
		out = append(out, "lib"+h+".so")
	}
	return out
}

func newRedirectFixture(t *testing.T) *redirectFixture {
	t.Helper()
	root := t.TempDir()
	f := &redirectFixture{t: t, root: root, libs: filepath.Join(root, "libs"), stage: filepath.Join(root, "stage"),
		data: aarch64SO([]byte("combat build"))}
	f.writeStore(true, nil)
	if err := os.MkdirAll(f.libs, 0o755); err != nil {
		t.Fatalf("%v", err)
	}
	if err := os.MkdirAll(f.stage, 0o755); err != nil {
		t.Fatalf("%v", err)
	}
	for _, name := range nineLibs() {
		writeFile(t, filepath.Join(f.libs, name), aarch64SO([]byte("combat build "+name)))
	}
	f.writeSums()
	return f
}

// writeStore writes the store APK: libr15, the nine stock libraries if withStock, and the extra names.
func (f *redirectFixture) writeStore(withStock bool, extra []string) {
	f.t.Helper()
	entries := []entry{{"lib/arm64-v8a/libr15.so", []byte("stock")}}
	if withStock {
		for _, name := range nineLibs() {
			entries = append(entries, entry{"lib/arm64-v8a/" + name, []byte("stock script library")})
		}
	}
	for _, name := range extra {
		entries = append(entries, entry{"lib/arm64-v8a/" + name, []byte("already there")})
	}
	f.store = storeZip(f.t, filepath.Join(f.root, "store.apk"), entries...)
}

// writeSums writes SHA256SUMS over every file of the libraries directory but the sums themselves.
func (f *redirectFixture) writeSums() {
	f.t.Helper()
	entries, err := os.ReadDir(f.libs)
	if err != nil {
		f.t.Fatalf("%v", err)
	}
	var names []string
	for _, e := range entries {
		if e.Type().IsRegular() && e.Name() != libsSums {
			names = append(names, e.Name())
		}
	}
	sort.Strings(names)
	var b strings.Builder
	for _, n := range names {
		b.WriteString(sha(readFile(f.t, filepath.Join(f.libs, n))) + "  " + n + "\n")
	}
	writeFile(f.t, filepath.Join(f.libs, libsSums), []byte(b.String()))
}

func (f *redirectFixture) stageLibs() []string {
	f.t.Helper()
	problems, err := StageLibs(StageLibsOptions{Libs: f.libs, Stage: f.stage, StoreAPK: f.store, Out: &bytes.Buffer{}})
	if err != nil {
		f.t.Fatalf("stage-libs: %v", err)
	}
	return problems
}

// TestStageRedirected_NineStagedUnderRedirectNames is test_the_nine_redirected_libraries_are_staged_under_their_redirect_names_and_no_stock_name_is.
func TestStageRedirected_NineStagedUnderRedirectNames(t *testing.T) {
	t.Parallel()
	f := newRedirectFixture(t)
	if got := f.stageLibs(); len(got) != 0 {
		t.Fatalf("got %q", got)
	}
	staged := filepath.Join(f.stage, abiDir)
	var want []string
	for _, name := range nineLibs() {
		want = append(want, stagedName(name))
		if !bytes.Equal(readFile(t, filepath.Join(staged, stagedName(name))), readFile(t, filepath.Join(f.libs, name))) {
			t.Fatalf("%s: bytes differ", name)
		}
		if exists(filepath.Join(staged, name)) {
			t.Fatalf("%s: the stock name was staged", name)
		}
	}
	entries, _ := os.ReadDir(staged)
	var got []string
	for _, e := range entries {
		got = append(got, e.Name())
	}
	sort.Strings(got)
	sort.Strings(want)
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("staged %q, want %q", got, want)
	}
	if replaced, _ := replacedNames(f.stage); len(replaced) != 0 {
		t.Fatalf("replaced %q", replaced)
	}
	recorded := stagedScriptLibs(f.stage)
	sort.Strings(recorded)
	if !reflect.DeepEqual(recorded, want) {
		t.Fatalf("recorded %q, want %q", recorded, want)
	}
}

// TestStageRedirected_SubsetRefusedAndMissingNamed is test_a_subset_of_the_nine_is_refused_and_the_missing_ones_are_named.
func TestStageRedirected_SubsetRefusedAndMissingNamed(t *testing.T) {
	t.Parallel()
	f := newRedirectFixture(t)
	nine := nineLibs()
	for _, name := range []string{nine[3], nine[8]} {
		if err := os.Remove(filepath.Join(f.libs, name)); err != nil {
			t.Fatalf("%v", err)
		}
	}
	f.writeSums()
	got := f.stageLibs()
	joined := strings.Join(got, "\n")
	if !strings.Contains(joined, nine[3]) || !strings.Contains(joined, nine[8]) {
		t.Fatalf("the missing libraries are not named: %q", got)
	}
	if strings.Contains(joined, nine[0]+" ") {
		t.Fatalf("a present library is named as missing: %q", got)
	}
	if !hasSub(got, "all nine or none") {
		t.Fatalf("got %q", got)
	}
	if exists(filepath.Join(f.stage, abiDir)) {
		t.Fatal("a refused set staged files")
	}
}

// TestStageRedirected_OneAloneRefused is test_one_redirected_library_alone_is_refused.
func TestStageRedirected_OneAloneRefused(t *testing.T) {
	t.Parallel()
	f := newRedirectFixture(t)
	nine := nineLibs()
	for _, name := range nine[1:] {
		if err := os.Remove(filepath.Join(f.libs, name)); err != nil {
			t.Fatalf("%v", err)
		}
	}
	f.writeSums()
	got := f.stageLibs()
	found := false
	for _, p := range got {
		if strings.Contains(p, "all nine or none") && strings.Contains(p, nine[1]) && strings.Contains(p, nine[8]) {
			found = true
		}
	}
	if !found || exists(filepath.Join(f.stage, abiDir)) {
		t.Fatalf("got %q", got)
	}
}

// TestStageRedirected_NoneOfTheNineStagedAsBefore is test_a_set_with_none_of_the_nine_is_staged_as_before.
func TestStageRedirected_NoneOfTheNineStagedAsBefore(t *testing.T) {
	t.Parallel()
	f := newRedirectFixture(t)
	for _, name := range nineLibs() {
		if err := os.Remove(filepath.Join(f.libs, name)); err != nil {
			t.Fatalf("%v", err)
		}
	}
	const other = "lib0123456789abcdef.so"
	writeFile(t, filepath.Join(f.libs, other), aarch64SO([]byte("not redirected")))
	f.writeSums()
	if got := f.stageLibs(); len(got) != 0 {
		t.Fatalf("got %q", got)
	}
	entries, _ := os.ReadDir(filepath.Join(f.stage, abiDir))
	if len(entries) != 1 || entries[0].Name() != other {
		t.Fatalf("staged %v, want [%s]", entries, other)
	}
}

// TestStageRedirected_StoreHoldsRedirectNameRefused is test_a_store_apk_that_already_holds_a_redirected_name_is_refused.
func TestStageRedirected_StoreHoldsRedirectNameRefused(t *testing.T) {
	t.Parallel()
	f := newRedirectFixture(t)
	f.writeStore(true, []string{redirected})
	if got := f.stageLibs(); !hasSub(got, redirected+": the store APK already holds that name") {
		t.Fatalf("got %q", got)
	}
	if exists(filepath.Join(f.stage, abiDir)) {
		t.Fatal("a refused set staged files")
	}
}

// TestStageRedirected_StoreLacksTheStockRefused is test_a_redirected_library_the_store_does_not_hold_is_refused.
func TestStageRedirected_StoreLacksTheStockRefused(t *testing.T) {
	t.Parallel()
	f := newRedirectFixture(t)
	f.writeStore(false, nil)
	if got := f.stageLibs(); !hasSub(got, redirectStock+": a redirected library, but the store APK holds no") {
		t.Fatalf("got %q", got)
	}
	if exists(filepath.Join(f.stage, abiDir)) {
		t.Fatal("a refused set staged files")
	}
}

// TestStageRedirected_NamedInREPLACERefused is test_a_redirected_library_named_in_REPLACE_is_refused.
func TestStageRedirected_NamedInREPLACERefused(t *testing.T) {
	t.Parallel()
	f := newRedirectFixture(t)
	writeFile(t, filepath.Join(f.libs, libsReplace), []byte(redirectStock+"\n"))
	if got := f.stageLibs(); !hasSub(got, redirectStock+": a redirected library never replaces") {
		t.Fatalf("got %q", got)
	}
	if exists(filepath.Join(f.stage, abiDir)) {
		t.Fatal("a refused set staged files")
	}
}

// TestStageRedirected_RedirectedNameInLibsRefused is test_the_redirected_name_in_the_libraries_directory_is_refused.
func TestStageRedirected_RedirectedNameInLibsRefused(t *testing.T) {
	t.Parallel()
	f := newRedirectFixture(t)
	writeFile(t, filepath.Join(f.libs, redirected), f.data)
	f.writeSums()
	if got := f.stageLibs(); !hasSub(got, redirected+": not named lib<hash16>.so") {
		t.Fatalf("got %q", got)
	}
}

// TestStageRedirected_MappingCoversTheNineAndNoOther is test_the_redirect_mapping_covers_the_nine_and_no_other_name.
func TestStageRedirected_MappingCoversTheNineAndNoOther(t *testing.T) {
	t.Parallel()
	seen := map[string]bool{}
	for _, h := range combatRedirectHashes {
		seen[h] = true
	}
	if len(seen) != redirectedCount {
		t.Fatalf("the list has %d distinct hashes, want %d", len(seen), redirectedCount)
	}
	if !isRedirected(redirectStock) || stagedName(redirectStock) != redirected {
		t.Fatalf("stock %s: redirected=%v staged=%s", redirectStock, isRedirected(redirectStock), stagedName(redirectStock))
	}
	if isRedirected("lib0123456789abcdef.so") || stagedName("lib0123456789abcdef.so") != "lib0123456789abcdef.so" {
		t.Fatal("a name outside the nine is redirected")
	}
}
