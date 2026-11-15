package questrepack

import (
	"path/filepath"
	"testing"
)

// The store APK's lib/ entries of the plugin-name tests (PluginNameTest in the Python file).
var storeEntriesForNames = []string{"lib/arm64-v8a/libr15.so", "lib/arm64-v8a/libovrplatformloader.so", "lib/armeabi-v7a/libother.so"}

// TestPluginNameCollisions_NoEntryHasTheName is test_a_name_no_entry_has_collides_with_nothing.
func TestPluginNameCollisions_NoEntryHasTheName(t *testing.T) {
	t.Parallel()
	if got := pluginNameCollisions(storeEntriesForNames, "libgood.so"); len(got) != 0 {
		t.Fatalf("want none, got %q", got)
	}
}

// TestPluginNameCollisions_StoreLibraryName is test_a_store_library_name_collides.
func TestPluginNameCollisions_StoreLibraryName(t *testing.T) {
	t.Parallel()
	got := pluginNameCollisions(storeEntriesForNames, "libr15.so")
	if len(got) != 1 || got[0] != "lib/arm64-v8a/libr15.so" {
		t.Fatalf("got %q", got)
	}
}

// TestPluginNameCollisions_AnyABIDirectory is test_the_name_collides_in_any_abi_directory.
func TestPluginNameCollisions_AnyABIDirectory(t *testing.T) {
	t.Parallel()
	got := pluginNameCollisions(storeEntriesForNames, "libother.so")
	if len(got) != 1 || got[0] != "lib/armeabi-v7a/libother.so" {
		t.Fatalf("got %q", got)
	}
}

// TestPluginNameCollisions_StagedByRecipe is test_a_library_the_recipe_stages_collides_even_when_the_store_lacks_it.
func TestPluginNameCollisions_StagedByRecipe(t *testing.T) {
	t.Parallel()
	got := pluginNameCollisions(storeEntriesForNames, "libovrplatformloader_orig.so")
	if len(got) != 1 || !hasSub(got, "staged by the recipe") {
		t.Fatalf("got %q", got)
	}
}

// TestPluginNameCollisions_LoaderNameHitsTwo is test_the_loader_name_collides_with_the_store_entry_and_the_staged_shim.
func TestPluginNameCollisions_LoaderNameHitsTwo(t *testing.T) {
	t.Parallel()
	if got := pluginNameCollisions(storeEntriesForNames, "libovrplatformloader.so"); len(got) != 2 {
		t.Fatalf("want 2, got %q", got)
	}
}

// TestPluginNameCollisions_WholeNameOnly is test_only_a_whole_file_name_collides.
func TestPluginNameCollisions_WholeNameOnly(t *testing.T) {
	t.Parallel()
	if got := pluginNameCollisions(storeEntriesForNames, "libr15.so.1"); len(got) != 0 {
		t.Fatalf("libr15.so.1: want none, got %q", got)
	}
	if got := pluginNameCollisions(storeEntriesForNames, "r15.so"); len(got) != 0 {
		t.Fatalf("r15.so: want none, got %q", got)
	}
	got := pluginNameCollisions([]string{"assets/libr15.so"}, "libr15.so")
	if len(got) != 1 || got[0] != "assets/libr15.so" {
		t.Fatalf("assets/libr15.so: got %q", got)
	}
}

// TestCheckPluginName_StoreAPKIsRead is test_the_store_apk_is_read_not_assumed (CheckPluginNameFromFileTest).
func TestCheckPluginName_StoreAPKIsRead(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	apk := zipOf(t, filepath.Join(dir, "store.apk"),
		entry{"lib/arm64-v8a/libr15.so", []byte("x")}, entry{"lib/arm64-v8a/libextra.so", []byte("y")},
		entry{"assets/a.bin", []byte("z")})
	got, err := checkPluginName(apk, filepath.Join(dir, "libextra.so"))
	if err != nil || len(got) != 1 || got[0] != "lib/arm64-v8a/libextra.so" {
		t.Fatalf("libextra.so: got %q, %v", got, err)
	}
	got, err = checkPluginName(apk, filepath.Join(dir, "libfree.so"))
	if err != nil || len(got) != 0 {
		t.Fatalf("libfree.so: got %q, %v", got, err)
	}
	got, err = checkPluginName(apk, filepath.Join(dir, "a.bin"))
	if err != nil || len(got) != 0 {
		t.Fatalf("a.bin (only lib/ entries count): got %q, %v", got, err)
	}
}
