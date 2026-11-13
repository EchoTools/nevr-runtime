package main

import (
	"archive/zip"
	"bytes"
	"crypto/md5" //nolint:gosec // the runtime's checksum list is md5 by design (#1737)
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"sort"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/questrepack/repackfixture"
	"github.com/EchoTools/nevr-runtime/tools/internal/questruntimepin/fixture"
)

// The end-to-end tests: `just android-repack-apk` run for real over the fixture (repackfixture), with the stand-in
// patcher, a fixture key and an AArch64 plugin made by clang, and the SIGNED APK read back with the standard zip reader.
// They are the Python tests RepackWithAPluginTest, RepackNamesItsVariantTest, RepackPatchesLibr15Test and
// RepackWithAPackTest (test_quest_repack_libs.py and test_quest_repack_pack.py).

const (
	e2eGameID  = "b243509c08ce677aeb95fa348016949b3fc45230" // the fixture libr15's build ID (kLibR15BuildId)
	e2eOtherID = "0123456789abcdef0123456789abcdef01234567"
	e2eMarker  = repackfixture.Marker
)

// devVersion is the fixture's development version: its major from the newest release tag (the recipe's stamp floor).
func devVersion(t *testing.T, repo string) string {
	return fmt.Sprintf("%d.6.1-dev.37+3a35e0b", repackfixture.NewestReleaseMajor(t, repo))
}

// e2eSkip skips when the recipe's tools are missing (the repackfixture's list).
func e2eSkip(t *testing.T) {
	t.Helper()
	if missing := repackfixture.MissingTools(); len(missing) > 0 {
		t.Skipf("the recipe needs %s", strings.Join(missing, ", "))
	}
}

// readZip reads every entry of the zip at path: its bytes by name, its compression by name, and the names in order.
type signedZip struct {
	data   map[string][]byte
	method map[string]uint16
	order  []string
}

func readSignedZip(t *testing.T, path string) signedZip {
	t.Helper()
	rc, err := zip.OpenReader(path)
	if err != nil {
		t.Fatalf("%v", err)
	}
	defer rc.Close()
	out := signedZip{data: map[string][]byte{}, method: map[string]uint16{}}
	for _, f := range rc.File {
		r, err := f.Open()
		if err != nil {
			t.Fatalf("%v", err)
		}
		body, err := io.ReadAll(r)
		r.Close()
		if err != nil {
			t.Fatalf("%v", err)
		}
		out.data[f.Name] = body
		out.method[f.Name] = f.Method
		out.order = append(out.order, f.Name)
	}
	return out
}

func sha256Of(data []byte) string {
	return sha256Hex(data)
}

// realLibr15 makes the store APK's libr15.so a real AArch64 object carrying the game's build ID, which the build-ID checks
// read, and returns its bytes.
func realLibr15(t *testing.T, fx *repackfixture.Fixture) []byte {
	t.Helper()
	libr15 := repackfixture.BuildAArch64(t, filepath.Join(fx.Dir, "game", "libr15.so"), "int game = 1;\n", e2eGameID)
	fx.ReplaceLibr15(libr15)
	return libr15
}

// plugin builds an AArch64 plugin at Dir/plugins/name from source, declaring ids in its note (ids "" with noNote: none).
func plugin(t *testing.T, fx *repackfixture.Fixture, name, source, ids string, noNote bool) string {
	t.Helper()
	body := source
	if !noNote {
		body = repackfixture.NevrNote(t, fx.Repo, ids) + source
	}
	path := filepath.Join(fx.Dir, "plugins", name)
	repackfixture.BuildAArch64(t, path, body, "")
	return path
}

const pluginSource = "int plugin_marker = 1;\n"

// TestE2EPlugin_CleanPluginPackagedAndLibrariesSurvive is test_a_clean_plugin_is_packaged_and_the_game_libraries_survive.
func TestE2EPlugin_CleanPluginPackagedAndLibrariesSurvive(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := repackfixture.New(t, repackfixture.ModeOK)
	fx.MakeInputs(devVersion(t, fx.Repo), true)
	libr15 := realLibr15(t, fx)
	good := plugin(t, fx, "libgood.so", pluginSource, e2eGameID, false)
	res := fx.Recipe("work", good, "", "", fx.Patcher)
	if res.Code != 0 || !strings.Contains(res.Stdout, "verify-signed OK") {
		t.Fatalf("code %d\nstdout %s\nstderr %s", res.Code, res.Stdout, res.Stderr)
	}
	signed := readSignedZip(t, res.Signed)
	if !bytes.Equal(signed.data["lib/arm64-v8a/libgood.so"], readFile(t, good)) {
		t.Fatal("the plugin's bytes differ")
	}
	if !bytes.Equal(signed.data["lib/arm64-v8a/libr15.so"], append(append([]byte(nil), libr15...), e2eMarker...)) {
		t.Fatal("libr15.so is not the patcher's output")
	}
	if !bytes.HasPrefix(signed.data["lib/arm64-v8a/libovrplatformloader_orig.so"], []byte("\x7fELF")) {
		t.Fatal("the store loader is not carried under its renamed name")
	}
}

// TestE2EPlugin_SeveralPluginsAllPackaged is test_several_plugins_are_all_packaged.
func TestE2EPlugin_SeveralPluginsAllPackaged(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := repackfixture.New(t, repackfixture.ModeOK)
	fx.MakeInputs(devVersion(t, fx.Repo), true)
	libr15 := realLibr15(t, fx)
	first := plugin(t, fx, "libfirst.so", pluginSource, e2eGameID, false)
	second := plugin(t, fx, "libsecond.so", "int plugin_marker = 2;\n", e2eGameID, false)
	res := fx.Recipe("work", first+":"+second, "", "", fx.Patcher)
	if res.Code != 0 || !strings.Contains(res.Stdout, "verify-signed OK") {
		t.Fatalf("code %d\nstdout %s\nstderr %s", res.Code, res.Stdout, res.Stderr)
	}
	signed := readSignedZip(t, res.Signed)
	if !bytes.Equal(signed.data["lib/arm64-v8a/libfirst.so"], readFile(t, first)) ||
		!bytes.Equal(signed.data["lib/arm64-v8a/libsecond.so"], readFile(t, second)) {
		t.Fatal("a plugin's bytes differ")
	}
	if !bytes.Equal(signed.data["lib/arm64-v8a/libr15.so"], append(append([]byte(nil), libr15...), e2eMarker...)) {
		t.Fatal("libr15.so is not the patcher's output")
	}
}

// TestE2EPlugin_PathWithASpace is test_a_plugin_path_with_a_space_is_packaged.
func TestE2EPlugin_PathWithASpace(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := repackfixture.New(t, repackfixture.ModeOK)
	fx.MakeInputs(devVersion(t, fx.Repo), true)
	realLibr15(t, fx)
	built := plugin(t, fx, "libspaced.so", pluginSource, e2eGameID, false)
	spaced := filepath.Join(fx.Dir, "with space", "libspaced.so")
	writeBytes(t, spaced, readFile(t, built))
	res := fx.Recipe("work", spaced, "", "", fx.Patcher)
	if res.Code != 0 {
		t.Fatalf("code %d\nstderr %s", res.Code, res.Stderr)
	}
	if !bytes.Equal(readSignedZip(t, res.Signed).data["lib/arm64-v8a/libspaced.so"], readFile(t, built)) {
		t.Fatal("the plugin's bytes differ")
	}
}

// writeBytes writes data at path, creating its directory.
func writeBytes(t *testing.T, path string, data []byte) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatalf("%v", err)
	}
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatalf("%v", err)
	}
}

// readFile reads path and fails the test on error.
func readFile(t *testing.T, path string) []byte {
	t.Helper()
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("%v", err)
	}
	return data
}

// TestE2EPlugin_EveryPluginChecked is test_every_plugin_is_checked_not_only_the_first.
func TestE2EPlugin_EveryPluginChecked(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := repackfixture.New(t, repackfixture.ModeOK)
	fx.MakeInputs(devVersion(t, fx.Repo), true)
	realLibr15(t, fx)
	good := plugin(t, fx, "libgood.so", pluginSource, e2eGameID, false)
	other := plugin(t, fx, "libother.so", pluginSource, e2eOtherID, false)
	res := fx.Recipe("work", good+":"+other, "", "", fx.Patcher)
	if res.Code == 0 || !strings.Contains(res.Stderr, "build_id_unsupported") || exists(res.Signed) {
		t.Fatalf("code %d, signed exists %v\nstderr %s", res.Code, exists(res.Signed), res.Stderr)
	}
}

// TestE2EPlugin_TwoWithOneFileName is test_two_plugins_with_one_file_name_are_refused.
func TestE2EPlugin_TwoWithOneFileName(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := repackfixture.New(t, repackfixture.ModeOK)
	fx.MakeInputs(devVersion(t, fx.Repo), true)
	realLibr15(t, fx)
	good := plugin(t, fx, "libgood.so", pluginSource, e2eGameID, false)
	same := filepath.Join(fx.Dir, "plugins", "other", "libgood.so")
	repackfixture.BuildAArch64(t, same, repackfixture.NevrNote(t, fx.Repo, e2eGameID)+"int plugin_marker = 3;\n", "")
	res := fx.Recipe("work", good+":"+same, "", "", fx.Patcher)
	if res.Code == 0 || !strings.Contains(res.Stderr, "two plugins are named libgood.so") || exists(res.Signed) {
		t.Fatalf("code %d\nstderr %s", res.Code, res.Stderr)
	}
}

// refusedBefore checks the common outcome of a refused plugin: the recipe fails, names want, and signs nothing.
func refusedBefore(t *testing.T, res repackfixture.Result, want string) {
	t.Helper()
	if res.Code == 0 || !strings.Contains(res.Stderr, want) || exists(res.Signed) {
		t.Fatalf("want %q refused, code %d, signed exists %v\nstderr %s", want, res.Code, exists(res.Signed), res.Stderr)
	}
}

// TestE2EPlugin_NamedLikeGameLibraryRefused is test_a_plugin_named_like_a_game_library_is_refused_before_anything_is_signed.
func TestE2EPlugin_NamedLikeGameLibraryRefused(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := repackfixture.New(t, repackfixture.ModeOK)
	fx.MakeInputs(devVersion(t, fx.Repo), true)
	realLibr15(t, fx)
	refusedBefore(t, fx.Recipe("work", plugin(t, fx, "libr15.so", pluginSource, e2eGameID, false), "", "", fx.Patcher),
		"plugin libr15.so would replace lib/arm64-v8a/libr15.so")
}

// TestE2EPlugin_NamedLikeStagedLibraryRefused is test_a_plugin_named_like_a_library_the_recipe_stages_is_refused.
func TestE2EPlugin_NamedLikeStagedLibraryRefused(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := repackfixture.New(t, repackfixture.ModeOK)
	fx.MakeInputs(devVersion(t, fx.Repo), true)
	realLibr15(t, fx)
	refusedBefore(t, fx.Recipe("work", plugin(t, fx, "libovrplatformloader_orig.so", pluginSource, e2eGameID, false), "", "", fx.Patcher),
		"staged by the recipe")
}

// TestE2EPlugin_NamedLikeOtherABIRefused is test_a_plugin_named_like_a_library_of_another_abi_is_refused.
func TestE2EPlugin_NamedLikeOtherABIRefused(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := repackfixture.New(t, repackfixture.ModeOK)
	fx.MakeInputs(devVersion(t, fx.Repo), true)
	realLibr15(t, fx)
	fx.AddStoreEntry("lib/armeabi-v7a/libold.so", []byte("old"))
	refusedBefore(t, fx.Recipe("work", plugin(t, fx, "libold.so", pluginSource, e2eGameID, false), "", "", fx.Patcher),
		"lib/armeabi-v7a/libold.so")
}

// TestE2EPlugin_LoadTimeInitializerRefused is test_a_plugin_with_a_load_time_initializer_is_refused.
func TestE2EPlugin_LoadTimeInitializerRefused(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := repackfixture.New(t, repackfixture.ModeOK)
	fx.MakeInputs(devVersion(t, fx.Repo), true)
	realLibr15(t, fx)
	ctor := "__attribute__((constructor)) static void run(void) {}\nint plugin_marker = 2;\n"
	refusedBefore(t, fx.Recipe("work", plugin(t, fx, "libctor.so", ctor, e2eGameID, false), "", "", fx.Patcher),
		"is not an expected initializer")
}

// TestE2EPlugin_NoBuildIDsRefused is test_a_plugin_that_declares_no_build_ids_is_refused_before_anything_is_signed.
func TestE2EPlugin_NoBuildIDsRefused(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := repackfixture.New(t, repackfixture.ModeOK)
	fx.MakeInputs(devVersion(t, fx.Repo), true)
	realLibr15(t, fx)
	refusedBefore(t, fx.Recipe("work", plugin(t, fx, "libnonote.so", pluginSource, "", true), "", "", fx.Patcher),
		"build_ids_undeclared")
}

// TestE2EPlugin_OtherGameBuildRefused is test_a_plugin_for_another_game_build_is_refused_before_anything_is_signed.
func TestE2EPlugin_OtherGameBuildRefused(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := repackfixture.New(t, repackfixture.ModeOK)
	fx.MakeInputs(devVersion(t, fx.Repo), true)
	realLibr15(t, fx)
	refusedBefore(t, fx.Recipe("work", plugin(t, fx, "libother.so", pluginSource, e2eOtherID, false), "", "", fx.Patcher),
		"build_id_unsupported")
}

// TestE2EPlugin_RepackWithoutPluginVerifies is test_a_repack_without_a_plugin_still_verifies_the_store_libraries.
func TestE2EPlugin_RepackWithoutPluginVerifies(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := repackfixture.New(t, repackfixture.ModeOK)
	fx.MakeInputs(devVersion(t, fx.Repo), true)
	res := fx.Repack("work")
	if res.Code != 0 || !strings.Contains(res.Stdout, "verify-signed OK") {
		t.Fatalf("code %d\nstderr %s", res.Code, res.Stderr)
	}
}

// manifestIn reads the plugin manifest of the signed APK, as the JSON document it is.
func manifestIn(t *testing.T, signed string) map[string]any {
	t.Helper()
	var doc map[string]any
	if err := json.Unmarshal(readSignedZip(t, signed).data["lib/arm64-v8a/libnevr_plugin_manifest.so"], &doc); err != nil {
		t.Fatalf("%v", err)
	}
	return doc
}

// TestE2EManifest_PackagedPluginEntry is test_a_packaged_plugin_gets_a_manifest_entry_in_the_signed_apk. The bytes are
// the writer's: sorted keys, no spaces, one trailing newline.
func TestE2EManifest_PackagedPluginEntry(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := repackfixture.New(t, repackfixture.ModeOK)
	fx.MakeInputs(devVersion(t, fx.Repo), true)
	realLibr15(t, fx)
	good := plugin(t, fx, "libgood.so", pluginSource, e2eGameID, false)
	res := fx.Recipe("work", good, "", "", fx.Patcher)
	if res.Code != 0 {
		t.Fatalf("code %d\nstderr %s", res.Code, res.Stderr)
	}
	want := map[string]any{"schema": float64(1), "plugins": []any{map[string]any{"name": "good", "file": "libgood.so",
		"enabled": true, "early": false, "sha256": sha256Of(readFile(t, good))}}}
	if !reflect.DeepEqual(manifestIn(t, res.Signed), want) {
		t.Fatalf("manifest %v, want %v", manifestIn(t, res.Signed), want)
	}
	expected := fmt.Sprintf(`{"plugins":[{"early":false,"enabled":true,"file":"libgood.so","name":"good","sha256":"%s"}],"schema":1}`+"\n",
		sha256Of(readFile(t, good)))
	if got := readSignedZip(t, res.Signed).data["lib/arm64-v8a/libnevr_plugin_manifest.so"]; string(got) != expected {
		t.Fatalf("manifest bytes\n%s\nwant\n%s", got, expected)
	}
}

// TestE2EManifest_OrderKept is test_several_plugins_keep_their_argument_order_in_the_manifest.
func TestE2EManifest_OrderKept(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := repackfixture.New(t, repackfixture.ModeOK)
	fx.MakeInputs(devVersion(t, fx.Repo), true)
	realLibr15(t, fx)
	first := plugin(t, fx, "libfirst.so", pluginSource, e2eGameID, false)
	second := plugin(t, fx, "libsecond.so", "int plugin_marker = 2;\n", e2eGameID, false)
	res := fx.Recipe("work", first+":"+second, "", "", fx.Patcher)
	if res.Code != 0 {
		t.Fatalf("code %d\nstderr %s", res.Code, res.Stderr)
	}
	var names []string
	for _, p := range manifestIn(t, res.Signed)["plugins"].([]any) {
		names = append(names, p.(map[string]any)["name"].(string))
	}
	if !reflect.DeepEqual(names, []string{"first", "second"}) {
		t.Fatalf("order %q", names)
	}
}

// TestE2EManifest_NoPluginNoEntry is test_no_plugin_adds_no_manifest_entry.
func TestE2EManifest_NoPluginNoEntry(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := repackfixture.New(t, repackfixture.ModeOK)
	fx.MakeInputs(devVersion(t, fx.Repo), true)
	res := fx.Repack("work")
	if res.Code != 0 {
		t.Fatalf("code %d\nstderr %s", res.Code, res.Stderr)
	}
	if _, ok := readSignedZip(t, res.Signed).data["lib/arm64-v8a/libnevr_plugin_manifest.so"]; ok {
		t.Fatal("a manifest with no plugin")
	}
}

// TestE2EManifest_EarlyAndOffRecorded is test_plugin_early_and_plugin_off_are_recorded.
func TestE2EManifest_EarlyAndOffRecorded(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := repackfixture.New(t, repackfixture.ModeOK)
	fx.MakeInputs(devVersion(t, fx.Repo), true)
	realLibr15(t, fx)
	first := plugin(t, fx, "libfirst.so", pluginSource, e2eGameID, false)
	second := plugin(t, fx, "libsecond.so", "int plugin_marker = 2;\n", e2eGameID, false)
	res := fx.Recipe("work", first+":"+second, "", "", fx.Patcher, "", "libfirst.so", "libsecond.so")
	if res.Code != 0 {
		t.Fatalf("code %d\nstderr %s", res.Code, res.Stderr)
	}
	plugins := map[string]map[string]any{}
	for _, p := range manifestIn(t, res.Signed)["plugins"].([]any) {
		plugins[p.(map[string]any)["name"].(string)] = p.(map[string]any)
	}
	if plugins["first"]["early"] != true || plugins["first"]["enabled"] != true ||
		plugins["second"]["early"] != false || plugins["second"]["enabled"] != false {
		t.Fatalf("manifest %v", plugins)
	}
}

// TestE2EManifest_EarlyNameNotPackagedRefused is test_an_early_name_that_is_not_packaged_is_refused_before_anything_is_signed.
func TestE2EManifest_EarlyNameNotPackagedRefused(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := repackfixture.New(t, repackfixture.ModeOK)
	fx.MakeInputs(devVersion(t, fx.Repo), true)
	realLibr15(t, fx)
	good := plugin(t, fx, "libgood.so", pluginSource, e2eGameID, false)
	res := fx.Recipe("work", good, "", "", fx.Patcher, "", "libmissing.so")
	refusedBefore(t, res, "libmissing.so")
}

// TestE2EManifest_PluginNamedLikeManifestRefused is test_a_plugin_named_like_the_manifest_is_refused.
func TestE2EManifest_PluginNamedLikeManifestRefused(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := repackfixture.New(t, repackfixture.ModeOK)
	fx.MakeInputs(devVersion(t, fx.Repo), true)
	realLibr15(t, fx)
	refusedBefore(t, fx.Recipe("work", plugin(t, fx, "libnevr_plugin_manifest.so", pluginSource, e2eGameID, false), "", "", fx.Patcher),
		"staged by the recipe")
}

// TestE2EManifest_SameBytesOnTwoRuns is test_the_manifest_bytes_are_the_same_on_two_runs.
func TestE2EManifest_SameBytesOnTwoRuns(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := repackfixture.New(t, repackfixture.ModeOK)
	fx.MakeInputs(devVersion(t, fx.Repo), true)
	realLibr15(t, fx)
	good := plugin(t, fx, "libgood.so", pluginSource, e2eGameID, false)
	first := fx.Recipe("work", good, "", "", fx.Patcher)
	if first.Code != 0 {
		t.Fatalf("code %d\nstderr %s", first.Code, first.Stderr)
	}
	once := readSignedZip(t, first.Signed).data["lib/arm64-v8a/libnevr_plugin_manifest.so"]
	second := fx.Recipe("work2", good, "", "", fx.Patcher)
	if second.Code != 0 {
		t.Fatalf("code %d\nstderr %s", second.Code, second.Stderr)
	}
	if !bytes.Equal(readSignedZip(t, second.Signed).data["lib/arm64-v8a/libnevr_plugin_manifest.so"], once) {
		t.Fatal("two runs wrote different manifests")
	}
}

// signedStamp repacks with the variant (the recipe's last parameter) and returns the stamp's bytes in the signed APK,
// which must be stored.
func signedStamp(t *testing.T, fx *repackfixture.Fixture, variant ...string) []byte {
	t.Helper()
	positional := []string{"", "", "", fx.Patcher, "", "", "", "", "", ""}
	positional = append(positional, variant...)
	res := fx.Recipe("work", positional...)
	if res.Code != 0 || !strings.Contains(res.Stdout, "verify-signed OK") {
		t.Fatalf("code %d\nstdout %s\nstderr %s", res.Code, res.Stdout, res.Stderr)
	}
	z := readSignedZip(t, res.Signed)
	if z.method["lib/arm64-v8a/libnevr_build_variant.so"] != zip.Store {
		t.Fatalf("the stamp is not stored")
	}
	return z.data["lib/arm64-v8a/libnevr_build_variant.so"]
}

// TestE2EVariant_Words is test_q15t_is_in_the_signed_apk_stored and the three words after it.
func TestE2EVariant_Words(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	for _, word := range []string{"q15t", "combat", "perf-probe", "arena"} {
		t.Run(word, func(t *testing.T) {
			t.Parallel()
			fx := repackfixture.New(t, repackfixture.ModeOK)
			fx.MakeInputs(devVersion(t, fx.Repo), true)
			if got := signedStamp(t, fx, word); string(got) != word {
				t.Fatalf("stamp %q", got)
			}
		})
	}
}

// TestE2EVariant_EmptyAndNoWordGiveAPlainStamp is test_an_empty_word_gives_a_plain_release_stamp and
// test_no_word_gives_a_plain_release_stamp.
func TestE2EVariant_EmptyAndNoWordGiveAPlainStamp(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := repackfixture.New(t, repackfixture.ModeOK)
	fx.MakeInputs(devVersion(t, fx.Repo), true)
	if got := signedStamp(t, fx, ""); len(got) != 0 {
		t.Fatalf("stamp %q, want empty", got)
	}
	fx2 := repackfixture.New(t, repackfixture.ModeOK)
	fx2.MakeInputs(devVersion(t, fx2.Repo), true)
	if got := signedStamp(t, fx2); len(got) != 0 {
		t.Fatalf("stamp %q, want empty", got)
	}
}

// TestE2EVariant_UnknownWordRefused is test_an_unknown_word_is_refused_before_anything_is_staged_or_signed.
func TestE2EVariant_UnknownWordRefused(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := repackfixture.New(t, repackfixture.ModeOK)
	fx.MakeInputs(devVersion(t, fx.Repo), true)
	res := fx.Recipe("work", "", "", "", fx.Patcher, "", "", "", "", "", "", "bogus")
	if res.Code == 0 || !strings.Contains(res.Stderr, "not a variant word") || len(fx.Calls()) != 0 || exists(res.Signed) {
		t.Fatalf("code %d, runs %d, signed %v\nstderr %s", res.Code, len(fx.Calls()), exists(res.Signed), res.Stderr)
	}
}

// patchFixture is the RepackPatchesLibr15Test setup: the default store (a fixture libr15), the stand-in patcher.
func patchFixture(t *testing.T) *repackfixture.Fixture {
	t.Helper()
	fx := repackfixture.New(t, repackfixture.ModeOK)
	fx.MakeInputs(devVersion(t, fx.Repo), true)
	return fx
}

// TestE2ELibr15_PatcherOutputStoredLikeTheStoreEntry is test_the_signed_libr15_is_the_patchers_output_and_is_stored_deflated_like_the_store_entry.
func TestE2ELibr15_PatcherOutputStoredLikeTheStoreEntry(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := patchFixture(t)
	res := fx.Recipe("work", "", "", "", fx.Patcher)
	if res.Code != 0 {
		t.Fatalf("code %d\nstderr %s", res.Code, res.Stderr)
	}
	store := readSignedZip(t, fx.APK)
	signed := readSignedZip(t, res.Signed)
	if store.method["lib/arm64-v8a/libr15.so"] != zip.Deflate || signed.method["lib/arm64-v8a/libr15.so"] != zip.Deflate {
		t.Fatalf("libr15.so is not deflated in both")
	}
	patched := append(append([]byte(nil), repackfixture.StoreLibr15...), e2eMarker...)
	if !bytes.Equal(signed.data["lib/arm64-v8a/libr15.so"], patched) {
		t.Fatal("the signed libr15.so is not the patcher's output")
	}
	if signed.method["lib/arm64-v8a/libovrplatformloader.so"] != zip.Store {
		t.Fatal("the shim is not stored")
	}
	for _, label := range []string{"sha256 patched " + sha256Of(patched), "sha256 signed  " + sha256Of(patched)} {
		if !strings.Contains(res.Stdout, label) {
			t.Fatalf("stdout lacks %q", label)
		}
	}
}

// TestE2ELibr15_PatcherRunsTwiceWithNoExtraRows is test_the_patcher_runs_twice_with_no_extra_rows_when_none_are_given.
func TestE2ELibr15_PatcherRunsTwiceWithNoExtraRows(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := patchFixture(t)
	if res := fx.Recipe("work", "", "", "", fx.Patcher); res.Code != 0 {
		t.Fatalf("code %d\nstderr %s", res.Code, res.Stderr)
	}
	calls := fx.Calls()
	if len(calls) != 2 {
		t.Fatalf("the patcher ran %d times, want 2", len(calls))
	}
	for _, c := range calls {
		if flagIn(c.Args, "--extra-rows") {
			t.Fatal("--extra-rows was given")
		}
	}
	work, err := filepath.EvalSymlinks(filepath.Join(fx.Dir, "work"))
	if err != nil {
		t.Fatalf("%v", err)
	}
	repack, verify := flagsOfArgs(calls[0].Args), flagsOfArgs(calls[1].Args)
	wantStock := filepath.Join(work, "orig", "lib", "arm64-v8a", "libr15.so")
	wantOut := filepath.Join(work, "stage", "lib", "arm64-v8a", "libr15.so")
	wantReport := filepath.Join(work, "libr15-patch-report.json")
	if resolve(t, repack["--stock"]) != wantStock || resolve(t, repack["--out"]) != wantOut || resolve(t, repack["--report"]) != wantReport {
		t.Fatalf("repack flags %v, want stock %s out %s report %s", repack, wantStock, wantOut, wantReport)
	}
	if !exists(filepath.Join(work, "libr15-patch-report.json")) {
		t.Fatal("the report was not written")
	}
	if filepath.Dir(filepath.Dir(resolve(t, verify["--stock"]))) != work {
		t.Fatalf("verify-signed's copy %s is not under the work directory", verify["--stock"])
	}
}

// resolve is the absolute path with its symlinks resolved where it exists (Python's Path.resolve() is not strict: a
// removed directory resolves to its path).
func resolve(t *testing.T, path string) string {
	t.Helper()
	abs, err := filepath.Abs(path)
	if err != nil {
		t.Fatalf("%v", err)
	}
	if out, err := filepath.EvalSymlinks(abs); err == nil {
		return out
	}
	return abs
}

// flagIn reports whether the command line has flag.
func flagIn(args []string, flag string) bool {
	for _, a := range args {
		if a == flag {
			return true
		}
	}
	return false
}

// flagsOfArgs is the flag -> value map of a command line of --flag value pairs.
func flagsOfArgs(args []string) map[string]string {
	out := map[string]string{}
	for i := 0; i+1 < len(args); i += 2 {
		out[args[i]] = args[i+1]
	}
	return out
}

// TestE2ELibr15_ExtraRowsFileToPatcherAndVerify is test_the_extra_rows_file_goes_to_the_patcher_and_to_verify_signed.
func TestE2ELibr15_ExtraRowsFileToPatcherAndVerify(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := patchFixture(t)
	rows := filepath.Join(fx.Dir, "rows.json")
	writeBytes(t, rows, []byte("{}"))
	appendToFile(t, fx.Shim, []byte("\x00libr15_extra_rows\x00"+sha256Of([]byte("{}"))+"\x00"))
	if res := fx.Recipe("work", "", "", rows, fx.Patcher); res.Code != 0 {
		t.Fatalf("code %d\nstderr %s", res.Code, res.Stderr)
	}
	calls := fx.Calls()
	if len(calls) != 2 {
		t.Fatalf("the patcher ran %d times", len(calls))
	}
	for _, c := range calls {
		if flagsOfArgs(c.Args)["--extra-rows"] != rows {
			t.Fatalf("--extra-rows %v", c.Args)
		}
	}
}

// appendToFile appends data to the file at path.
func appendToFile(t *testing.T, path string, data []byte) {
	t.Helper()
	f, err := os.OpenFile(path, os.O_APPEND|os.O_WRONLY, 0)
	if err != nil {
		t.Fatalf("%v", err)
	}
	defer f.Close()
	if _, err := f.Write(data); err != nil {
		t.Fatalf("%v", err)
	}
}

// TestE2ELibr15_ShimNotBuiltForExtraRowsRefused is test_a_shim_not_built_for_the_extra_rows_is_refused_before_anything_is_staged.
func TestE2ELibr15_ShimNotBuiltForExtraRowsRefused(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := patchFixture(t)
	rows := filepath.Join(fx.Dir, "rows.json")
	writeBytes(t, rows, []byte("{}"))
	res := fx.Recipe("work", "", "", rows, fx.Patcher)
	if res.Code == 0 || !strings.Contains(res.Stderr, "was not built with rows.json") || len(fx.Calls()) != 0 || exists(res.Signed) {
		t.Fatalf("code %d, runs %d\nstderr %s", res.Code, len(fx.Calls()), res.Stderr)
	}
}

// TestE2ELibr15_VariantShimWithoutRowsRefused is test_a_variant_shim_without_an_extra_rows_file_is_refused_before_anything_is_staged.
func TestE2ELibr15_VariantShimWithoutRowsRefused(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := patchFixture(t)
	appendToFile(t, fx.Shim, []byte("\x00libr15_extra_rows\x00"))
	res := fx.Recipe("work", "", "", "", fx.Patcher)
	if res.Code == 0 || !strings.Contains(res.Stderr, "is a variant build") || len(fx.Calls()) != 0 || exists(res.Signed) {
		t.Fatalf("code %d, runs %d\nstderr %s", res.Code, len(fx.Calls()), res.Stderr)
	}
}

// TestE2ELibr15_ExtraRowsNotThereRefused is test_an_extra_rows_file_that_is_not_there_is_refused_before_anything_is_staged.
func TestE2ELibr15_ExtraRowsNotThereRefused(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := patchFixture(t)
	res := fx.Recipe("work", "", "", filepath.Join(fx.Dir, "no-rows.json"), fx.Patcher)
	if res.Code == 0 || !strings.Contains(res.Stderr, "extra rows file not found") || len(fx.Calls()) != 0 || exists(res.Signed) {
		t.Fatalf("code %d\nstderr %s", res.Code, res.Stderr)
	}
}

// TestE2ELibr15_PatcherThatRefusesStopsTheRecipe is test_a_patcher_that_refuses_stops_the_recipe_with_no_signed_apk.
func TestE2ELibr15_PatcherThatRefusesStopsTheRecipe(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := repackfixture.New(t, repackfixture.ModeFail)
	fx.MakeInputs(devVersion(t, fx.Repo), true)
	res := fx.Recipe("work", "", "", "", fx.Patcher)
	if res.Code == 0 || !strings.Contains(res.Stderr, "standin patcher: refused") || exists(res.Signed) {
		t.Fatalf("code %d\nstderr %s", res.Code, res.Stderr)
	}
}

// TestE2ELibr15_PatcherThatWritesNothingStopsTheRecipe is test_a_patcher_that_writes_nothing_stops_the_recipe_with_no_signed_apk.
func TestE2ELibr15_PatcherThatWritesNothingStopsTheRecipe(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := repackfixture.New(t, repackfixture.ModeNoWrite)
	fx.MakeInputs(devVersion(t, fx.Repo), true)
	res := fx.Recipe("work", "", "", "", fx.Patcher)
	if res.Code == 0 || !strings.Contains(res.Stderr, "the libr15 patcher wrote no libr15.so") || exists(res.Signed) {
		t.Fatalf("code %d\nstderr %s", res.Code, res.Stderr)
	}
}

// TestE2ELibr15_PatcherNotExecutableRefused is test_a_patcher_path_that_is_not_an_executable_is_refused.
func TestE2ELibr15_PatcherNotExecutableRefused(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := patchFixture(t)
	res := fx.Recipe("work", "", "", "", filepath.Join(fx.Dir, "no-such-patcher"))
	if res.Code == 0 || !strings.Contains(res.Stderr, "the libr15 patcher is not an executable file") || exists(res.Signed) {
		t.Fatalf("code %d\nstderr %s", res.Code, res.Stderr)
	}
}

// TestE2ELibr15_JSONIncludeWithoutHeaderNamesIt is test_a_json_include_without_the_header_is_refused_naming_it.
func TestE2ELibr15_JSONIncludeWithoutHeaderNamesIt(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := patchFixture(t)
	empty := filepath.Join(fx.Dir, "empty-include")
	if err := os.Mkdir(empty, 0o755); err != nil {
		t.Fatalf("%v", err)
	}
	res := fx.Recipe("work", "", "", "", "", empty)
	want := fmt.Sprintf("android-repack-apk: FAIL - %s/nlohmann/json.hpp not found (json_include)", empty)
	if res.Code == 0 || !strings.Contains(res.Stderr, want) || exists(res.Signed) {
		t.Fatalf("code %d\nstderr %s", res.Code, res.Stderr)
	}
}

// addLibvrapi adds lib/arm64-v8a/libvrapi.so to the fixture's store APK.
func addLibvrapi(t *testing.T, fx *repackfixture.Fixture, data []byte) {
	t.Helper()
	fx.AddStoreEntry("lib/arm64-v8a/libvrapi.so", data)
}

// TestE2ELibr15_ReferenceRuntimePinnedAndStored is test_a_reference_runtime_is_pinned_to_the_staged_libr15_and_stored.
func TestE2ELibr15_ReferenceRuntimePinnedAndStored(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := patchFixture(t)
	addLibvrapi(t, fx, fixture.MakeRuntime(fixture.OLD))
	res := fx.Recipe("work", "", "", "", fx.Patcher)
	if res.Code != 0 {
		t.Fatalf("code %d\nstderr %s", res.Code, res.Stderr)
	}
	patched := append(append([]byte(nil), repackfixture.StoreLibr15...), e2eMarker...)
	pinned := fixture.MakeRuntime(md5Of(patched))
	signed := readSignedZip(t, res.Signed)
	if !bytes.Equal(signed.data["lib/arm64-v8a/libvrapi.so"], pinned) || signed.method["lib/arm64-v8a/libvrapi.so"] != zip.Store {
		t.Fatal("the runtime is not the pin to the signed libr15.so, stored")
	}
	if !strings.Contains(res.Stdout, `"event": "runtime_pinned"`) {
		t.Fatalf("the pin's record is not on stdout:\n%s", res.Stdout)
	}
	if !strings.Contains(res.Stdout, "libvrapi.so sha256 signed  "+sha256Of(pinned)) {
		t.Fatalf("the signed digest is not printed:\n%s", res.Stdout)
	}
}

// md5Of is the md5 of data, lower-case hex.
func md5Of(data []byte) string {
	sum := md5.Sum(data) //nolint:gosec // see the import note
	return hex.EncodeToString(sum[:])
}

// TestE2ELibr15_RuntimeWithoutChecksumListLeftAsTheStoreHasIt is test_a_libvrapi_with_no_checksum_list_is_left_as_the_store_has_it.
func TestE2ELibr15_RuntimeWithoutChecksumListLeftAsTheStoreHasIt(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := patchFixture(t)
	storeVrapi := fixture.MakeElf(make([]byte, 8), []byte("the store's own libvrapi, no md5 list\x00"))
	addLibvrapi(t, fx, storeVrapi)
	res := fx.Recipe("work", "", "", "", fx.Patcher)
	if res.Code != 0 || !strings.Contains(res.Stdout, "carries no runtime md5 list: left as it is") {
		t.Fatalf("code %d\nstdout %s\nstderr %s", res.Code, res.Stdout, res.Stderr)
	}
	if !bytes.Equal(readSignedZip(t, res.Signed).data["lib/arm64-v8a/libvrapi.so"], storeVrapi) {
		t.Fatal("the runtime differs from the store's")
	}
}

// TestE2ELibr15_RuntimeThatCannotBePinnedStopsTheRecipe is test_a_reference_runtime_that_cannot_be_pinned_stops_the_recipe_with_no_signed_apk.
func TestE2ELibr15_RuntimeThatCannotBePinnedStopsTheRecipe(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := patchFixture(t)
	addLibvrapi(t, fx, fixture.MakeRuntime(fixture.OLD, fixture.WithSites(11)))
	res := fx.Recipe("work", "", "", "", fx.Patcher)
	if res.Code == 0 || !strings.Contains(res.Stderr, "runtime_pin_refused") || exists(res.Signed) {
		t.Fatalf("code %d\nstderr %s", res.Code, res.Stderr)
	}
}

// TestE2ELibr15_NoLibvrapiStagesNone is test_an_apk_with_no_libvrapi_stages_none.
func TestE2ELibr15_NoLibvrapiStagesNone(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := patchFixture(t)
	res := fx.Recipe("work", "", "", "", fx.Patcher)
	if res.Code != 0 {
		t.Fatalf("code %d\nstderr %s", res.Code, res.Stderr)
	}
	if _, ok := readSignedZip(t, res.Signed).data["lib/arm64-v8a/libvrapi.so"]; ok {
		t.Fatal("a runtime was staged from an APK that has none")
	}
}

// makePack writes the pack zip of the good entries and the meta file for the checkout's pins; it returns their paths.
func makePack(t *testing.T, fx *repackfixture.Fixture, entries []string, datas [][]byte) (string, string) {
	t.Helper()
	version, manifest := pinsOf(t, filepath.Join(fx.Repo, "src", "quest", "data", "plan.h"))
	prefix := "_data/" + version + "/rad15/android/"
	if entries == nil {
		entries = []string{prefix + "manifests/" + manifest, prefix + "packages/" + manifest + "_1"}
		datas = [][]byte{packManifestBytes(), packPackageBytes()}
	}
	pack := filepath.Join(fx.Dir, "pack.zip")
	repackfixture.WriteZip(t, pack, zip.Deflate, entries, datas)
	meta := filepath.Join(fx.Dir, "pack-meta.json")
	writeBytes(t, meta, []byte(`{"id": "combat-pack", "version": "1.2.3", "requires_stock_manifest_sha256": ["`+
		sha256Of([]byte("stock manifest a"))+`"], "zip_prefix": "`+prefix+`"}`))
	return pack, meta
}

// packManifestBytes and packPackageBytes are the pack's two files in the Python test.
func packManifestBytes() []byte { return bytes.Repeat([]byte("pack manifest "), 40) }

func packPackageBytes() []byte {
	var out []byte
	for i := 0; i < 300; i++ {
		for b := 0; b < 256; b++ {
			out = append(out, byte(b))
		}
	}
	return out
}

// TestE2EPack_CarriedStoredWithDescription is test_a_pack_is_carried_stored_with_its_description_and_every_library_survives.
func TestE2EPack_CarriedStoredWithDescription(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := patchFixture(t)
	pack, meta := makePack(t, fx, nil, nil)
	res := fx.Recipe("work", "", "", "", fx.Patcher, "", "", "", pack, meta)
	if res.Code != 0 {
		t.Fatalf("code %d\nstdout %s\nstderr %s", res.Code, res.Stdout, res.Stderr)
	}
	for _, want := range []string{"verify-signed OK", "stage-pack OK", "pack-problems OK"} {
		if !strings.Contains(res.Stdout, want) {
			t.Fatalf("stdout lacks %q", want)
		}
	}
	signed := readSignedZip(t, res.Signed)
	for name, data := range map[string][]byte{
		"assets/nevr-pack/files/" + packManifestPath(t, fx): packManifestBytes(),
		"assets/nevr-pack/files/" + packPackagePath(t, fx):  packPackageBytes(),
	} {
		if !bytes.Equal(signed.data[name], data) || signed.method[name] != zip.Store {
			t.Fatalf("%s: not the pack's bytes, stored", name)
		}
	}
	if signed.method["assets/nevr-pack/pack.json"] != zip.Store {
		t.Fatal("pack.json is not stored")
	}
	var doc struct {
		Files []struct {
			Path string `json:"path"`
		} `json:"files"`
		SourceZipSHA256 string `json:"source_zip_sha256"`
	}
	if err := json.Unmarshal(signed.data["assets/nevr-pack/pack.json"], &doc); err != nil {
		t.Fatalf("%v", err)
	}
	if len(doc.Files) != 2 || doc.Files[0].Path != packManifestPath(t, fx) || doc.Files[1].Path != packPackagePath(t, fx) ||
		doc.SourceZipSHA256 != sha256Of(readFile(t, pack)) {
		t.Fatalf("pack.json %+v", doc)
	}
	if !bytes.Equal(signed.data["assets/data.bin"], bytes.Repeat(allBytes(), 20)) {
		t.Fatal("the store's own assets are changed")
	}
	if out, err := exec.Command("apksigner", "verify", res.Signed).CombinedOutput(); err != nil {
		t.Fatalf("apksigner verify: %v\n%s", err, out)
	}
}

// allBytes is the bytes 0 to 255.
func allBytes() []byte {
	out := make([]byte, 256)
	for i := range out {
		out[i] = byte(i)
	}
	return out
}

// packManifestPath and packPackagePath are the pack's paths below the prefix (the names the stage gives the files),
// from the checkout's pins.
func packManifestPath(t *testing.T, fx *repackfixture.Fixture) string {
	_, manifest := pinsOf(t, filepath.Join(fx.Repo, "src", "quest", "data", "plan.h"))
	return "manifests/" + manifest
}

func packPackagePath(t *testing.T, fx *repackfixture.Fixture) string {
	_, manifest := pinsOf(t, filepath.Join(fx.Repo, "src", "quest", "data", "plan.h"))
	return "packages/" + manifest + "_1"
}

// TestE2EPack_WithoutPackCarriesNothingOfIt is test_without_a_pack_the_apk_carries_nothing_of_it_and_a_pack_changes_only_its_own_entries.
func TestE2EPack_WithoutPackCarriesNothingOfIt(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := patchFixture(t)
	bare := fx.Repack("bare")
	if bare.Code != 0 {
		t.Fatalf("code %d\nstderr %s", bare.Code, bare.Stderr)
	}
	without := entryTable(t, bare.Signed)
	for name := range without {
		if strings.HasPrefix(name, "assets/nevr-pack") {
			t.Fatalf("a build with no pack carries %s", name)
		}
	}
	if strings.Contains(bare.Stdout, "stage-pack") || strings.Contains(bare.Stdout, "pack-problems") {
		t.Fatal("the pack steps ran with no pack")
	}
	store := storeNames(t, fx.APK)
	want := map[string]bool{"lib/arm64-v8a/libovrplatformloader_orig.so": true, variantEntryName: true}
	for n := range store {
		want[n] = true
	}
	got := map[string]bool{}
	for n := range without {
		if !strings.HasPrefix(n, "META-INF/") {
			got[n] = true
		}
	}
	for n := range want {
		if strings.HasPrefix(n, "META-INF/") {
			delete(want, n)
		}
	}
	if !reflect.DeepEqual(sortedKeys(got), sortedKeys(want)) {
		t.Fatalf("entries %v, want %v", sortedKeys(got), sortedKeys(want))
	}
	pack, meta := makePack(t, fx, nil, nil)
	packed := fx.Recipe("packed", "", "", "", fx.Patcher, "", "", "", pack, meta)
	if packed.Code != 0 {
		t.Fatalf("code %d\nstderr %s", packed.Code, packed.Stderr)
	}
	with := entryTable(t, packed.Signed)
	added := map[string]bool{}
	for n := range with {
		if _, ok := without[n]; !ok {
			added[n] = true
		}
	}
	wantAdded := map[string]bool{"assets/nevr-pack/pack.json": true, "assets/nevr-pack/files/" + packManifestPath(t, fx): true,
		"assets/nevr-pack/files/" + packPackagePath(t, fx): true}
	if !reflect.DeepEqual(sortedKeys(added), sortedKeys(wantAdded)) {
		t.Fatalf("added %v, want %v", sortedKeys(added), sortedKeys(wantAdded))
	}
	for n, entry := range without {
		if !strings.HasPrefix(n, "META-INF/") && !reflect.DeepEqual(with[n], entry) {
			t.Fatalf("%s differs between the run with a pack and the run without", n)
		}
	}
}

// variantEntryName is the stamp's entry in the signed APK.
const variantEntryName = "lib/arm64-v8a/libnevr_build_variant.so"

// entryInfo is an entry's compression, CRC and size, as the Python test compares them.
type entryInfo struct {
	method uint16
	crc    uint32
	size   uint64
}

// entryTable is every entry of the zip at path by name.
func entryTable(t *testing.T, path string) map[string]entryInfo {
	t.Helper()
	rc, err := zip.OpenReader(path)
	if err != nil {
		t.Fatalf("%v", err)
	}
	defer rc.Close()
	out := map[string]entryInfo{}
	for _, f := range rc.File {
		out[f.Name] = entryInfo{f.Method, f.CRC32, f.UncompressedSize64}
	}
	return out
}

// storeNames is the entry names of the zip at path.
func storeNames(t *testing.T, path string) map[string]bool {
	t.Helper()
	out := map[string]bool{}
	for n := range entryTable(t, path) {
		out[n] = true
	}
	return out
}

// sortedKeys is the keys of a set, sorted.
func sortedKeys(m map[string]bool) []string {
	out := make([]string, 0, len(m))
	for k := range m {
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}

// TestE2EPack_RefusedPackStopsTheRecipe is test_a_pack_the_tool_refuses_stops_the_recipe_with_no_signed_apk.
func TestE2EPack_RefusedPackStopsTheRecipe(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := patchFixture(t)
	version, manifest := pinsOf(t, filepath.Join(fx.Repo, "src", "quest", "data", "plan.h"))
	prefix := "_data/" + version + "/rad15/android/"
	pack, meta := makePack(t, fx, []string{prefix + "packages/" + manifest + "_1"}, [][]byte{packPackageBytes()})
	res := fx.Recipe("work", "", "", "", fx.Patcher, "", "", "", pack, meta)
	if res.Code == 0 || !strings.Contains(res.Stderr, "the zip has no "+prefix+"manifests/"+manifest) || exists(res.Signed) {
		t.Fatalf("code %d\nstderr %s", res.Code, res.Stderr)
	}
}

// TestE2EPack_DescriptionAndPackMustComeTogether is test_a_pack_with_no_description_file_is_refused_and_so_is_a_description_with_no_pack.
func TestE2EPack_DescriptionAndPackMustComeTogether(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := patchFixture(t)
	pack, meta := makePack(t, fx, nil, nil)
	res := fx.Recipe("work", "", "", "", fx.Patcher, "", "", "", pack, "")
	if res.Code == 0 || !strings.Contains(res.Stderr, "pack_meta (its description file) is not a file") || exists(res.Signed) {
		t.Fatalf("pack without meta: code %d\nstderr %s", res.Code, res.Stderr)
	}
	res = fx.Recipe("work", "", "", "", fx.Patcher, "", "", "", "", meta)
	if res.Code == 0 || !strings.Contains(res.Stderr, "pack_meta describes a pack, and no pack is given") || exists(res.Signed) {
		t.Fatalf("meta with no pack: code %d\nstderr %s", res.Code, res.Stderr)
	}
	res = fx.Recipe("work", "", "", "", fx.Patcher, "", "", "", filepath.Join(fx.Dir, "missing.zip"), meta)
	if res.Code == 0 || !strings.Contains(res.Stderr, "pack not found") || exists(res.Signed) {
		t.Fatalf("missing pack: code %d\nstderr %s", res.Code, res.Stderr)
	}
}

// TestE2EPack_StoreWithPackEntryRefused is test_a_store_apk_that_has_a_pack_entry_already_is_refused.
func TestE2EPack_StoreWithPackEntryRefused(t *testing.T) {
	t.Parallel()
	e2eSkip(t)
	fx := patchFixture(t)
	fx.AddStoreEntry("assets/nevr-pack/pack.json", []byte("{}"))
	pack, meta := makePack(t, fx, nil, nil)
	res := fx.Recipe("work", "", "", "", fx.Patcher, "", "", "", pack, meta)
	if res.Code == 0 || !strings.Contains(res.Stderr, "already has the entry assets/nevr-pack/pack.json") || exists(res.Signed) {
		t.Fatalf("code %d\nstderr %s", res.Code, res.Stderr)
	}
}
