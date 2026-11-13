package questrepack

import (
	"bytes"
	"encoding/json"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/questrepack/repackfixture"
)

// The stamps the repack writes beside the libraries: the plugin manifest (ADR 0019 section 2), the variant stamp (ADR
// 0040 rule 1, ADR 0046 rule 5) and the shim's rows check (ADR 0021 section 11).

// TestWritePluginManifest_HashOfTheStagedBytes is test_the_hash_is_of_the_staged_bytes_not_of_the_file_the_recipe_was_given.
func TestWritePluginManifest_HashOfTheStagedBytes(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	stage := filepath.Join(dir, "stage")
	source := writeFile(t, filepath.Join(dir, "libfx.so"), []byte("the file the recipe was given"))
	writeFile(t, filepath.Join(stage, abiDir, "libfx.so"), []byte("the bytes staged for the APK"))
	if got := writePluginManifest(stage, []string{source}, nil, nil); len(got) != 0 {
		t.Fatalf("got %q", got)
	}
	var written struct {
		Plugins []struct {
			SHA256 string `json:"sha256"`
		} `json:"plugins"`
	}
	if err := json.Unmarshal(readFile(t, filepath.Join(stage, abiDir, manifestName)), &written); err != nil {
		t.Fatalf("%v", err)
	}
	if got := written.Plugins[0].SHA256; got != sha([]byte("the bytes staged for the APK")) || got == sha([]byte("the file the recipe was given")) {
		t.Fatalf("sha256 %s is not the staged file's", got)
	}
}

// TestWritePluginManifest_GoldenFile is test_the_writers_output_for_two_plugins_is_the_golden_file. The golden file is
// the one the Quest loader's parser test reads (src/quest/tests/packaged_plugins_test.cpp).
func TestWritePluginManifest_GoldenFile(t *testing.T) {
	t.Parallel()
	root := t.TempDir()
	stage := filepath.Join(root, "stage")
	var plugins []string
	for _, p := range []struct {
		name string
		data string
	}{{"libalpha.so", "alpha plugin\n"}, {"libbeta.so", "beta plugin\n"}} {
		writeFile(t, filepath.Join(stage, abiDir, p.name), []byte(p.data))
		plugins = append(plugins, filepath.Join(root, p.name))
	}
	if got := writePluginManifest(stage, plugins, []string{"libalpha.so"}, []string{"libbeta.so"}); len(got) != 0 {
		t.Fatalf("got %q", got)
	}
	golden := filepath.Join(repackfixture.RepoRoot(t), "src", "quest", "tests", "fixtures", "packaged_plugins_manifest.golden")
	if !bytes.Equal(readFile(t, filepath.Join(stage, abiDir, manifestName)), readFile(t, golden)) {
		t.Fatal("the writer's bytes differ from the golden file")
	}
}

// TestZipOf_SameEntriesSameBytes is test_the_same_entries_make_the_same_bytes_across_a_two_second_boundary. The Python
// helper read the clock for each entry's time, so two calls seconds apart differed; the Go helper reads no clock, and
// the test checks that two writes of the same entries are the same bytes.
func TestZipOf_SameEntriesSameBytes(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	first := readFile(t, zipOf(t, filepath.Join(dir, "first.zip"), entry{abiDir + "/libr15.so", []byte("x")}))
	second := readFile(t, zipOf(t, filepath.Join(dir, "second.zip"), entry{abiDir + "/libr15.so", []byte("x")}))
	if !bytes.Equal(first, second) {
		t.Fatal("the same entries made different bytes")
	}
}

// stageDir is an empty stage with its library directory.
func stageDir(t *testing.T) string {
	t.Helper()
	stage := filepath.Join(t.TempDir(), "stage")
	if err := os.MkdirAll(filepath.Join(stage, abiDir), 0o755); err != nil {
		t.Fatalf("%v", err)
	}
	return stage
}

// TestBuildVariant_WordsAreTheOnesTheADRNames is test_the_words_are_the_ones_the_adr_names_and_the_arena_apk.
func TestBuildVariant_WordsAreTheOnesTheADRNames(t *testing.T) {
	t.Parallel()
	if !reflect.DeepEqual(variantWords, []string{"q15t", "combat", "perf-probe", "arena"}) {
		t.Fatalf("words %q", variantWords)
	}
	if variantEntry != "lib/arm64-v8a/libnevr_build_variant.so" {
		t.Fatalf("entry %q", variantEntry)
	}
}

// TestBuildVariant_EachWordIsItsBytes is test_each_word_is_written_as_exactly_its_bytes.
func TestBuildVariant_EachWordIsItsBytes(t *testing.T) {
	t.Parallel()
	stage := stageDir(t)
	stamp := filepath.Join(stage, abiDir, variantStamp)
	for _, word := range variantWords {
		if got := writeBuildVariant(stage, word); len(got) != 0 {
			t.Fatalf("%s: %q", word, got)
		}
		if !bytes.Equal(readFile(t, stamp), []byte(word)) {
			t.Fatalf("%s: stamp %q", word, readFile(t, stamp))
		}
	}
}

// TestBuildVariant_EmptyWordIsAPlainRelease is test_an_empty_word_is_a_plain_release_and_writes_an_empty_stamp.
func TestBuildVariant_EmptyWordIsAPlainRelease(t *testing.T) {
	t.Parallel()
	stage := stageDir(t)
	if got := writeBuildVariant(stage, ""); len(got) != 0 {
		t.Fatalf("got %q", got)
	}
	if got := readFile(t, filepath.Join(stage, abiDir, variantStamp)); len(got) != 0 {
		t.Fatalf("stamp %q, want empty", got)
	}
}

// TestBuildVariant_OtherWordsRefusedAndNothingWritten is test_any_other_word_is_refused_by_name_and_writes_nothing.
func TestBuildVariant_OtherWordsRefusedAndNothingWritten(t *testing.T) {
	t.Parallel()
	words := []string{"bogus", "Q15T", "Combat", " combat", "combat\n", "q15t ", "perf_probe", "combat\x00", "release", "dev",
		"Arena", "arena ", "arena\n"}
	for _, word := range words {
		t.Run(pyRepr(word), func(t *testing.T) {
			t.Parallel()
			stage := stageDir(t)
			got := writeBuildVariant(stage, word)
			if len(got) != 1 || !strings.Contains(got[0], "is not a variant word") ||
				!strings.Contains(got[0], "q15t, combat, perf-probe, arena") {
				t.Fatalf("got %q", got)
			}
			if exists(filepath.Join(stage, abiDir, variantStamp)) {
				t.Fatal("a refused word wrote a stamp")
			}
		})
	}
}

// TestBuildVariant_StampIsARecipeFile is test_a_plugin_named_like_the_stamp_is_a_collision.
func TestBuildVariant_StampIsARecipeFile(t *testing.T) {
	t.Parallel()
	if !contains(stagedByRecipe, "libnevr_build_variant.so") {
		t.Fatalf("the stamp is not staged by the recipe: %q", stagedByRecipe)
	}
}

// TestBuildVariant_StampIsNotAnUnlistedPlugin is test_the_stamp_is_not_an_unlisted_plugin.
func TestBuildVariant_StampIsNotAnUnlistedPlugin(t *testing.T) {
	t.Parallel()
	stage := stageDir(t)
	if got := writeBuildVariant(stage, "combat"); len(got) != 0 {
		t.Fatalf("got %q", got)
	}
	if got := pluginManifestProblems(stage); len(got) != 0 {
		t.Fatalf("got %q", got)
	}
}

// shimFixture writes a shim (a public one, plus parts) and two rows files, and returns their paths.
type shimFixture struct {
	t        *testing.T
	dir      string
	rows     string
	other    string
	rowsBody []byte
	otherBuf []byte
}

const (
	shimRows  = `{"format": 1}` + "\n"
	shimOther = `{"format": 2}` + "\n"
)

func newShimFixture(t *testing.T) *shimFixture {
	t.Helper()
	dir := t.TempDir()
	return &shimFixture{
		t:        t,
		dir:      dir,
		rows:     writeFile(t, filepath.Join(dir, "rows.json"), []byte(shimRows)),
		other:    writeFile(t, filepath.Join(dir, "other.json"), []byte(shimOther)),
		rowsBody: []byte(shimRows),
		otherBuf: []byte(shimOther),
	}
}

// shim writes libovrplatformloader.so: a public rodata, then parts.
func (s *shimFixture) shim(parts ...[]byte) string {
	data := append([]byte("\x7fELF public rodata\x00"), concat(parts...)...)
	return writeFile(s.t, filepath.Join(s.dir, shimName), data)
}

// shaNul is the sha-256 of data as the shim embeds it: the hex digest and a NUL.
func shaNul(data []byte) []byte {
	return []byte(sha(data) + "\x00")
}

// TestShimRows_PublicShimNoRowsPasses is test_a_public_shim_with_no_rows_passes.
func TestShimRows_PublicShimNoRowsPasses(t *testing.T) {
	t.Parallel()
	s := newShimFixture(t)
	if got := shimRowsProblems(s.shim(), ""); len(got) != 0 {
		t.Fatalf("got %q", got)
	}
}

// TestShimRows_VariantShimNoRowsRefused is test_a_variant_shim_with_no_rows_is_refused.
func TestShimRows_VariantShimNoRowsRefused(t *testing.T) {
	t.Parallel()
	s := newShimFixture(t)
	got := shimRowsProblems(s.shim([]byte(variantMarker), shaNul(s.rowsBody)), "")
	if len(got) != 1 || !strings.Contains(got[0], "variant build") {
		t.Fatalf("got %q", got)
	}
}

// TestShimRows_ShimBuiltWithTheRowsPasses is test_a_shim_built_with_the_rows_file_passes.
func TestShimRows_ShimBuiltWithTheRowsPasses(t *testing.T) {
	t.Parallel()
	s := newShimFixture(t)
	if got := shimRowsProblems(s.shim([]byte(variantMarker), shaNul(s.rowsBody)), s.rows); len(got) != 0 {
		t.Fatalf("got %q", got)
	}
}

// TestShimRows_PublicShimWithRowsRefused is test_a_public_shim_with_a_rows_file_is_refused.
func TestShimRows_PublicShimWithRowsRefused(t *testing.T) {
	t.Parallel()
	s := newShimFixture(t)
	got := shimRowsProblems(s.shim(), s.rows)
	if len(got) != 1 || !strings.Contains(got[0], "was not built with rows.json") || !strings.Contains(got[0], sha(s.rowsBody)) {
		t.Fatalf("got %q", got)
	}
}

// TestShimRows_ShimBuiltWithOtherRowsRefused is test_a_shim_built_with_other_rows_is_refused.
func TestShimRows_ShimBuiltWithOtherRowsRefused(t *testing.T) {
	t.Parallel()
	s := newShimFixture(t)
	if got := shimRowsProblems(s.shim([]byte(variantMarker), shaNul(s.otherBuf)), s.rows); len(got) != 1 {
		t.Fatalf("got %q", got)
	}
}

// TestShimRows_OnlyTheWholeDigestCounts is test_only_the_whole_digest_counts: a digest cut short, with its NUL, is not
// the embedded digest.
func TestShimRows_OnlyTheWholeDigestCounts(t *testing.T) {
	t.Parallel()
	s := newShimFixture(t)
	cut := []byte(sha(s.rowsBody)[:len(sha(s.rowsBody))-1] + "\x00")
	if got := shimRowsProblems(s.shim(cut), s.rows); len(got) != 1 {
		t.Fatalf("got %q", got)
	}
}

// TestShimRows_UnreadableShimRefused is test_a_shim_that_cannot_be_read_is_refused.
func TestShimRows_UnreadableShimRefused(t *testing.T) {
	t.Parallel()
	s := newShimFixture(t)
	if got := shimRowsProblems(filepath.Join(s.dir, "missing.so"), ""); len(got) != 1 {
		t.Fatalf("got %q", got)
	}
}
