package questrepack

import (
	"archive/zip"
	"bytes"
	"encoding/json"
	"io/fs"
	"os"
	"path/filepath"
	"reflect"
	"sort"
	"strings"
	"sync"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/questrepack/repackfixture"
	"github.com/EchoTools/nevr-runtime/tools/internal/testkit"
)

// The content-pack tests (test_quest_repack_pack.py): stage-pack, pack-problems and verify-signed over a pack. Every
// file of a pack gets every check, and a refusal writes nothing.

const (
	stockAHash     = "stock manifest a"
	stockBHash     = "stock manifest b"
	packManifestN  = 40
	packPackageN   = 300
	packByteRange  = 256
	fourGiB        = 0x100000000
	apkLimitTests  = 0xFFFFFFFF
	slackTests     = 8 * 1024 * 1024
	packVersionVal = "1.2.3"
	packIDVal      = "combat-pack"
	sparseHoleZero = 0
)

var (
	versionFolder = "" // set by TestMain-free init below
	manifestID    = ""
	prefixPath    = ""
	manifestPath  = ""
	packagePath   = ""
	stockA        = sha([]byte(stockAHash))
	stockB        = sha([]byte(stockBHash))
	packManifest  = bytesRepeat([]byte("pack manifest "), packManifestN)
	packPackage   = packagePayload()
)

func bytesRepeat(b []byte, n int) []byte {
	return bytes.Repeat(b, n)
}

// packagePayload is bytes(range(256)) * 300.
func packagePayload() []byte {
	out := make([]byte, 0, packByteRange*packPackageN)
	for i := 0; i < packPackageN; i++ {
		for b := 0; b < packByteRange; b++ {
			out = append(out, byte(b))
		}
	}
	return out
}

// pinsOnce reads the data step's pins from the checkout's header, once: the parallel tests share the names.
var pinsOnce sync.Once

// initPins sets the names of the data step's pins (versionFolder, manifestID and the paths built from them).
func initPins(t *testing.T) {
	t.Helper()
	pinsOnce.Do(func() {
		version, manifest, ok := pinnedManifest(planHeaderFor(t))
		if !ok {
			t.Fatalf("the checkout's plan.h names no kManifestPath")
		}
		versionFolder, manifestID = version, manifest
		prefixPath = "_data/" + version + "/rad15/android/"
		manifestPath = "manifests/" + manifest
		packagePath = "packages/" + manifest + "_1"
	})
}

// packFixture is the Python PackFixture: a store APK, a stage, and the pack zip and its meta file made per test.
type packFixture struct {
	t      *testing.T
	tmp    string
	store  string
	stage  string
	header string
}

func newPackFixture(t *testing.T) *packFixture {
	t.Helper()
	initPins(t)
	tmp := t.TempDir()
	store := zipOf(t, filepath.Join(tmp, "store.apk"),
		entry{"AndroidManifest.xml", []byte("manifest")}, entry{"lib/arm64-v8a/libr15.so", []byte("game")})
	stage := filepath.Join(tmp, "stage")
	if err := os.Mkdir(stage, 0o755); err != nil {
		t.Fatalf("%v", err)
	}
	return &packFixture{t: t, tmp: tmp, store: store, stage: stage, header: planHeaderFor(t)}
}

// goodEntries are the manifest and the package under the prefix.
func goodEntries() []entry {
	return []entry{{prefixPath + manifestPath, packManifest}, {prefixPath + packagePath, packPackage}}
}

// descendingPair is the two stock digests in descending order (the Python test's sorted(..., reverse=True)).
func descendingPair() []string {
	asc := sortedCopy([]string{stockA, stockB})
	return []string{asc[1], asc[0]}
}

// metaWith is the pack's meta file as the tests give it, with changes applied: a key set to nil is removed.
func metaWith(changes map[string]any) map[string]any {
	doc := map[string]any{
		"id":                             packIDVal,
		"version":                        packVersionVal,
		"requires_stock_manifest_sha256": descendingPair(), // given unsorted: the tool writes them sorted
		"zip_prefix":                     prefixPath,
	}
	for k, v := range changes {
		if v == nil {
			delete(doc, k)
		} else {
			doc[k] = v
		}
	}
	return doc
}

// makePack writes the pack zip of entries (the good ones when nil) and its meta file.
func (f *packFixture) makePack(entries []entry, changes map[string]any) (string, string) {
	f.t.Helper()
	if entries == nil {
		entries = goodEntries()
	}
	pack := zipOf(f.t, filepath.Join(f.tmp, "pack.zip"), entries...)
	metaPath := filepath.Join(f.tmp, "pack-meta.json")
	writeFile(f.t, metaPath, mustJSON(f.t, metaWith(changes)))
	return pack, metaPath
}

func mustJSON(t *testing.T, v any) []byte {
	t.Helper()
	data, err := json.Marshal(v)
	if err != nil {
		t.Fatalf("%v", err)
	}
	return data
}

// stagePack stages a pack with the given entries and meta changes, and returns the problems.
func (f *packFixture) stagePack(entries []entry, changes map[string]any) []string {
	f.t.Helper()
	pack, metaPath := f.makePack(entries, changes)
	problems, err := StagePack(StagePackOptions{PackZip: pack, Meta: metaPath, Stage: f.stage, StoreAPK: f.store,
		PlanHeader: f.header, Out: &bytes.Buffer{}})
	if err != nil {
		f.t.Fatalf("stage-pack: %v", err)
	}
	return problems
}

// stagedFiles is every file under the stage's assets, slash-separated and sorted.
func (f *packFixture) stagedFiles() []string {
	root := filepath.Join(f.stage, "assets")
	var out []string
	_ = filepath.WalkDir(root, func(path string, d fs.DirEntry, err error) error {
		if err == nil && d.Type().IsRegular() {
			rel, _ := filepath.Rel(f.stage, path)
			out = append(out, filepath.ToSlash(rel))
		}
		return nil
	})
	sort.Strings(out)
	return out
}

// assertRefused is the Python assert_refused: the problems hold each needle, and nothing was staged.
func (f *packFixture) assertRefused(problems []string, needles ...string) {
	f.t.Helper()
	if len(problems) == 0 {
		f.t.Fatalf("a pack that must be refused was accepted")
	}
	for _, n := range needles {
		if !hasSub(problems, n) {
			f.t.Fatalf("want a problem containing %q, got %q", n, problems)
		}
	}
	if got := f.stagedFiles(); len(got) != 0 {
		f.t.Fatalf("a refused pack must write nothing, staged %q", got)
	}
}

// sha256OfJSON is the expected pack.json fields the staging writes, as the JSON the tests compare with.
func packJSONExpected(zipSHA string, zipSize int64) map[string]any {
	return map[string]any{
		"schema":                         1,
		"id":                             packIDVal,
		"version":                        packVersionVal,
		"stock_data_version":             versionFolder,
		"requires_stock_manifest_sha256": sortedCopy([]string{stockA, stockB}),
		"files": []any{
			map[string]any{"path": manifestPath, "sha256": sha(packManifest), "size": len(packManifest)},
			map[string]any{"path": packagePath, "sha256": sha(packPackage), "size": len(packPackage)},
		},
		"source_zip_sha256": zipSHA,
		"source_zip_size":   zipSize,
	}
}

// decodeJSON parses data into a generic value, failing the test on bad JSON.
func decodeJSON(t *testing.T, data []byte) any {
	t.Helper()
	var v any
	if err := json.Unmarshal(data, &v); err != nil {
		t.Fatalf("%v", err)
	}
	return v
}

// normalize is v as a generic JSON value (numbers as float64), so a Go int and a decoded number compare equal.
func normalize(t *testing.T, v any) any {
	return decodeJSON(t, mustJSON(t, v))
}

// TestStagePack_CleanPackStaged is test_a_clean_pack_is_staged_with_the_bytes_and_a_description_of_them.
func TestStagePack_CleanPackStaged(t *testing.T) {
	t.Parallel()
	f := newPackFixture(t)
	if problems := f.stagePack(nil, nil); len(problems) != 0 {
		t.Fatalf("got %q", problems)
	}
	want := []string{"assets/nevr-pack/files/" + manifestPath, "assets/nevr-pack/files/" + packagePath, "assets/nevr-pack/pack.json"}
	sort.Strings(want)
	if got := f.stagedFiles(); !reflect.DeepEqual(got, want) {
		t.Fatalf("staged %q, want %q", got, want)
	}
	if !bytes.Equal(readFile(t, filepath.Join(f.stage, "assets/nevr-pack/files", manifestPath)), packManifest) {
		t.Fatal("the manifest's bytes differ")
	}
	if !bytes.Equal(readFile(t, filepath.Join(f.stage, "assets/nevr-pack/files", packagePath)), packPackage) {
		t.Fatal("the package's bytes differ")
	}
	document := readFile(t, filepath.Join(f.stage, packJSONEntry))
	zipPath := filepath.Join(f.tmp, "pack.zip")
	info, err := os.Stat(zipPath)
	if err != nil {
		t.Fatalf("%v", err)
	}
	want2 := normalize(t, packJSONExpected(sha(readFile(t, zipPath)), info.Size()))
	if got := normalize(t, decodeJSON(t, document)); !reflect.DeepEqual(got, want2) {
		t.Fatalf("pack.json:\n%s\nwant:\n%s", document, mustJSON(t, want2))
	}
	if problems := PackProblems(f.stage, f.header); len(problems) != 0 {
		t.Fatalf("pack-problems: %q", problems)
	}
}

// TestStagePack_DescriptionIsTheSameBytesEveryTime is test_the_description_is_the_same_bytes_every_time.
func TestStagePack_DescriptionIsTheSameBytesEveryTime(t *testing.T) {
	t.Parallel()
	f := newPackFixture(t)
	if problems := f.stagePack(nil, nil); len(problems) != 0 {
		t.Fatalf("got %q", problems)
	}
	first := readFile(t, filepath.Join(f.stage, packJSONEntry))
	if err := os.RemoveAll(filepath.Join(f.stage, "assets")); err != nil {
		t.Fatalf("%v", err)
	}
	if problems := f.stagePack(nil, nil); len(problems) != 0 {
		t.Fatalf("got %q", problems)
	}
	if !bytes.Equal(readFile(t, filepath.Join(f.stage, packJSONEntry)), first) {
		t.Fatal("the description differs between two stagings")
	}
	if !bytes.HasSuffix(first, []byte("}\n")) || bytes.Contains(bytes.ReplaceAll(first, []byte(packIDVal), nil), []byte(" ")) {
		t.Fatalf("the description is not compact JSON with one trailing newline:\n%s", first)
	}
}

// TestStagePack_OneDigestAndSortedDigests is test_one_digest_is_enough_and_the_digests_are_written_sorted.
func TestStagePack_OneDigestAndSortedDigests(t *testing.T) {
	t.Parallel()
	f := newPackFixture(t)
	if problems := f.stagePack(nil, map[string]any{"requires_stock_manifest_sha256": []string{stockA}}); len(problems) != 0 {
		t.Fatalf("got %q", problems)
	}
	doc := decodeJSON(t, readFile(t, filepath.Join(f.stage, packJSONEntry))).(map[string]any)
	if !reflect.DeepEqual(doc["requires_stock_manifest_sha256"], []any{stockA}) {
		t.Fatalf("requires = %v", doc["requires_stock_manifest_sha256"])
	}
}

// TestStagePack_EveryEntryNameTheReaderRefuses is test_an_entry_name_the_sentinels_reader_refuses_is_refused.
func TestStagePack_EveryEntryNameTheReaderRefuses(t *testing.T) {
	t.Parallel()
	names := []string{"../escape", "/abs/path", "a\\b", "C:drive", "a/./b", "a//b", strings.Repeat("x", 300),
		strings.TrimSuffix(strings.Repeat("d/", 17), "/"), "bad\x01name"}
	for _, name := range names {
		t.Run(name, func(t *testing.T) {
			t.Parallel()
			f := newPackFixture(t)
			key := prefixPath + name
			if strings.HasPrefix(name, "/") {
				key = name
			}
			entries := append(goodEntries(), entry{key, []byte("x")})
			f.assertRefused(f.stagePack(entries, nil), "entry name the sentinel's zip reader refuses")
		})
	}
}

// TestStagePack_LinkOrSpecialFileRefused is test_a_link_or_a_special_file_is_refused.
func TestStagePack_LinkOrSpecialFileRefused(t *testing.T) {
	t.Parallel()
	const (
		sIFLNK   = 0o120000
		sIFCHR   = 0o020000
		sIFIFO   = 0o010000
		mode0777 = 0o777
		mode0600 = 0o600
	)
	for _, mode := range []uint32{sIFLNK | mode0777, sIFCHR | mode0600, sIFIFO | mode0600} {
		t.Run("mode", func(t *testing.T) {
			t.Parallel()
			f := newPackFixture(t)
			pack := filepath.Join(f.tmp, "pack.zip")
			w, err := os.Create(pack)
			if err != nil {
				t.Fatalf("%v", err)
			}
			zw := zip.NewWriter(w)
			mustCreate(t, zw, &zip.FileHeader{Name: prefixPath + manifestPath, Method: zip.Store}, packManifest)
			mustCreate(t, zw, &zip.FileHeader{Name: prefixPath + packagePath, Method: zip.Store, CreatorVersion: zipCreatorUnix << 8,
				ExternalAttrs: mode << 16}, packPackage)
			if err := zw.Close(); err != nil {
				t.Fatalf("%v", err)
			}
			w.Close()
			metaPath := writeFile(t, filepath.Join(f.tmp, "m.json"), mustJSON(t, metaWith(nil)))
			problems, err := StagePack(StagePackOptions{PackZip: pack, Meta: metaPath, Stage: f.stage, StoreAPK: f.store,
				PlanHeader: f.header, Out: &bytes.Buffer{}})
			if err != nil {
				t.Fatalf("%v", err)
			}
			f.assertRefused(problems, "a link or another special file")
		})
	}
}

func mustCreate(t *testing.T, zw *zip.Writer, h *zip.FileHeader, data []byte) {
	t.Helper()
	w, err := zw.CreateHeader(h)
	if err != nil {
		t.Fatalf("%v", err)
	}
	if _, err := w.Write(data); err != nil {
		t.Fatalf("%v", err)
	}
}

// centralOffset is the offset of the central directory entry of name in data (the Python test's search).
func centralOffset(t *testing.T, data []byte, name string) int {
	t.Helper()
	for i := 0; i+46 <= len(data); i++ {
		if bytes.HasPrefix(data[i:], []byte("PK\x01\x02")) && bytes.HasPrefix(data[i+46:], []byte(name)) {
			return i
		}
	}
	t.Fatalf("no central directory entry for %s", name)
	return -1
}

// TestStagePack_EncryptedEntryRefused is test_an_encrypted_entry_is_refused.
func TestStagePack_EncryptedEntryRefused(t *testing.T) {
	t.Parallel()
	f := newPackFixture(t)
	pack := zipOf(t, filepath.Join(f.tmp, "pack.zip"), goodEntries()...)
	data := readFile(t, pack)
	at := centralOffset(t, data, prefixPath+packagePath)
	data[at+8] |= zipEncryptedBit // the central directory's general purpose flags: bit 0 is "encrypted"
	writeFile(t, pack, data)
	metaPath := writeFile(t, filepath.Join(f.tmp, "m.json"), mustJSON(t, metaWith(nil)))
	problems, err := StagePack(StagePackOptions{PackZip: pack, Meta: metaPath, Stage: f.stage, StoreAPK: f.store,
		PlanHeader: f.header, Out: &bytes.Buffer{}})
	if err != nil {
		t.Fatalf("%v", err)
	}
	f.assertRefused(problems, prefixPath+packagePath+": encrypted")
}

// TestStagePack_TwoNamesThatAreOneName is test_two_names_that_are_one_name_are_refused.
func TestStagePack_TwoNamesThatAreOneName(t *testing.T) {
	t.Parallel()
	f := newPackFixture(t)
	entries := append(goodEntries(), entry{prefixPath + "packages/Same_1", []byte("a")}, entry{prefixPath + "packages/same_1", []byte("b")})
	f.assertRefused(f.stagePack(entries, nil), "the same name as")
}

// TestStagePack_SameNameTwiceInTheZip is test_the_same_name_twice_in_the_zip_is_refused.
func TestStagePack_SameNameTwiceInTheZip(t *testing.T) {
	t.Parallel()
	f := newPackFixture(t)
	f.assertRefused(f.stagePack(append(goodEntries(), entry{prefixPath + packagePath, []byte("other bytes")}), nil),
		"the same name as")
}

// TestStagePack_FileOutsideThePrefix is test_a_file_outside_the_prefix_is_refused.
func TestStagePack_FileOutsideThePrefix(t *testing.T) {
	t.Parallel()
	f := newPackFixture(t)
	f.assertRefused(f.stagePack(append(goodEntries(), entry{"elsewhere/file", []byte("x")}), nil), "elsewhere/file: outside")
}

// TestStagePack_DirectoryEntries is test_a_directory_entry_outside_the_prefix_is_refused_and_one_on_the_way_to_it_is_not.
func TestStagePack_DirectoryEntries(t *testing.T) {
	t.Parallel()
	f := newPackFixture(t)
	dirs := []entry{{"_data/", nil}, {"_data/" + versionFolder + "/", nil}, {prefixPath + "packages/", nil}}
	if problems := f.stagePack(append(goodEntries(), dirs...), nil); len(problems) != 0 {
		t.Fatalf("the directories on the way to the prefix were refused: %q", problems)
	}
	if err := os.RemoveAll(filepath.Join(f.stage, "assets")); err != nil {
		t.Fatalf("%v", err)
	}
	f.assertRefused(f.stagePack(append(goodEntries(), append(dirs, entry{"other-dir/", nil})...), nil), "a directory outside")
}

// TestStagePack_OnlyManifestsAndPackagesArePlaces is test_only_manifests_and_packages_are_places_for_a_file.
func TestStagePack_OnlyManifestsAndPackagesArePlaces(t *testing.T) {
	t.Parallel()
	for _, p := range []string{"scripts/x", "packages/sub/x", "x", "manifests/other-manifest", "packages/.hidden",
		"packages/x.nevr-pack.tmp", "packages/x.stock", "packages/x.nevr-owned", "packages/sp ace"} {
		t.Run(p, func(t *testing.T) {
			t.Parallel()
			f := newPackFixture(t)
			f.assertRefused(f.stagePack(append(goodEntries(), entry{prefixPath + p, []byte("x")}), nil), p)
		})
	}
}

// TestStagePack_NoManifestRefused is test_a_pack_with_no_manifest_is_refused.
func TestStagePack_NoManifestRefused(t *testing.T) {
	t.Parallel()
	f := newPackFixture(t)
	f.assertRefused(f.stagePack([]entry{{prefixPath + packagePath, packPackage}}, nil), "the zip has no "+prefixPath+manifestPath)
}

// TestStagePack_EmptyFileRefused is test_an_empty_file_is_refused.
func TestStagePack_EmptyFileRefused(t *testing.T) {
	t.Parallel()
	f := newPackFixture(t)
	f.assertRefused(f.stagePack([]entry{{prefixPath + manifestPath, packManifest}, {prefixPath + packagePath, nil}}, nil), "0 bytes")
}

// TestStagePack_NineFilesRefused is test_nine_files_are_refused. Eight extras with the manifest and package make ten;
// nine is refused; eight is fine.
func TestStagePack_NineFilesRefused(t *testing.T) {
	t.Parallel()
	f := newPackFixture(t)
	extras := func(n int) []entry {
		var out []entry
		for i := 0; i < n; i++ {
			out = append(out, entry{prefixPath + "packages/extra" + string(rune('0'+i)), []byte("x")})
		}
		return out
	}
	f.assertRefused(f.stagePack(append(goodEntries(), extras(8)...), nil), "at most 8")
	if err := os.RemoveAll(f.stage); err != nil {
		t.Fatalf("%v", err)
	}
	if err := os.Mkdir(f.stage, 0o755); err != nil {
		t.Fatalf("%v", err)
	}
	f.assertRefused(f.stagePack(append(goodEntries(), extras(7)...), nil), "9 files", "at most 8")
	if problems := f.stagePack(append(goodEntries(), extras(6)...), nil); len(problems) != 0 {
		t.Fatalf("eight files were refused: %q", problems)
	}
}

// TestStagePack_RecordedSizeNotTheSize is test_an_entry_whose_recorded_size_is_not_its_size_is_refused.
func TestStagePack_RecordedSizeNotTheSize(t *testing.T) {
	t.Parallel()
	f := newPackFixture(t)
	pack := zipOf(t, filepath.Join(f.tmp, "pack.zip"), goodEntries()...)
	data := readFile(t, pack)
	at := centralOffset(t, data, prefixPath+packagePath)
	// The central directory entry of the package: its uncompressed size (offset 24 in the header) is made one byte longer.
	const sizeOffset = 24
	size := uint32(data[at+sizeOffset]) | uint32(data[at+sizeOffset+1])<<8 | uint32(data[at+sizeOffset+2])<<16 | uint32(data[at+sizeOffset+3])<<24
	size++
	data[at+sizeOffset], data[at+sizeOffset+1], data[at+sizeOffset+2], data[at+sizeOffset+3] = byte(size), byte(size>>8), byte(size>>16), byte(size>>24)
	writeFile(t, pack, data)
	metaPath := writeFile(t, filepath.Join(f.tmp, "m.json"), mustJSON(t, metaWith(nil)))
	problems, err := StagePack(StagePackOptions{PackZip: pack, Meta: metaPath, Stage: f.stage, StoreAPK: f.store,
		PlanHeader: f.header, Out: &bytes.Buffer{}})
	if err != nil {
		t.Fatalf("%v", err)
	}
	f.assertRefused(problems, packagePath)
}

// TestStagePack_NotAZipRefused is test_a_file_that_is_not_a_zip_is_refused.
func TestStagePack_NotAZipRefused(t *testing.T) {
	t.Parallel()
	f := newPackFixture(t)
	pack := writeFile(t, filepath.Join(f.tmp, "pack.zip"), []byte("not a zip"))
	metaPath := writeFile(t, filepath.Join(f.tmp, "m.json"), mustJSON(t, metaWith(nil)))
	problems, err := StagePack(StagePackOptions{PackZip: pack, Meta: metaPath, Stage: f.stage, StoreAPK: f.store,
		PlanHeader: f.header, Out: &bytes.Buffer{}})
	if err != nil {
		t.Fatalf("%v", err)
	}
	f.assertRefused(problems, "not a readable zip")
}

// TestStagePack_EveryUnusableMetaRefused is test_every_unusable_meta_is_refused.
func TestStagePack_EveryUnusableMetaRefused(t *testing.T) {
	t.Parallel()
	cases := map[string]map[string]any{
		"an id with a capital":                      {"id": "Combat"},
		"an id with a slash":                        {"id": "a/b"},
		"no id":                                     {"id": nil},
		"a two-part version":                        {"version": "1.2"},
		"a version with a letter":                   {"version": "1.2.x"},
		"no version":                                {"version": nil},
		"no digest":                                 {"requires_stock_manifest_sha256": []string{}},
		"three digests":                             {"requires_stock_manifest_sha256": []string{sha([]byte("1")), sha([]byte("2")), sha([]byte("3"))}},
		"a digest in capitals":                      {"requires_stock_manifest_sha256": []string{strings.ToUpper(stockA)}},
		"a short digest":                            {"requires_stock_manifest_sha256": []string{stockA[:len(stockA)-1]}},
		"the same digest twice":                     {"requires_stock_manifest_sha256": []string{stockA, stockA}},
		"a prefix that is not a data directory":     {"zip_prefix": "_data/x/"},
		"a prefix without the trailing slash":       {"zip_prefix": prefixPath[:len(prefixPath)-1]},
		"another data version than the data step's": {"zip_prefix": "_data/1234567890/rad15/android/"},
		"no prefix":                                 {"zip_prefix": nil},
	}
	for label, changes := range cases {
		t.Run(label, func(t *testing.T) {
			t.Parallel()
			f := newPackFixture(t)
			f.assertRefused(f.stagePack(nil, changes), "meta")
		})
	}
}

// TestStagePack_MetaNotJSONRefused is test_a_meta_that_is_not_json_is_refused.
func TestStagePack_MetaNotJSONRefused(t *testing.T) {
	t.Parallel()
	f := newPackFixture(t)
	pack, _ := f.makePack(nil, nil)
	metaPath := writeFile(t, filepath.Join(f.tmp, "pack-meta.json"), []byte("{not json"))
	problems, err := StagePack(StagePackOptions{PackZip: pack, Meta: metaPath, Stage: f.stage, StoreAPK: f.store,
		PlanHeader: f.header, Out: &bytes.Buffer{}})
	if err != nil {
		t.Fatalf("%v", err)
	}
	f.assertRefused(problems, "cannot be read as a JSON object")
}

// TestStagePack_HeaderWithNoManifestPinRefused is test_a_plan_header_with_no_manifest_pin_is_refused.
func TestStagePack_HeaderWithNoManifestPinRefused(t *testing.T) {
	t.Parallel()
	f := newPackFixture(t)
	pack, metaPath := f.makePack(nil, nil)
	header := writeFile(t, filepath.Join(f.tmp, "plan.h"), []byte("// no pin here\n"))
	problems, err := StagePack(StagePackOptions{PackZip: pack, Meta: metaPath, Stage: f.stage, StoreAPK: f.store,
		PlanHeader: header, Out: &bytes.Buffer{}})
	if err != nil {
		t.Fatalf("%v", err)
	}
	f.assertRefused(problems, "names no kManifestPath")
}

// TestStagePack_PinsAreTheDataStepsNotLiteralsOfTheTool is test_the_pins_are_the_data_steps_not_literals_of_the_tool.
func TestStagePack_PinsAreTheDataStepsNotLiteralsOfTheTool(t *testing.T) {
	t.Parallel()
	f := newPackFixture(t)
	header := writeFile(t, filepath.Join(f.tmp, "plan.h"),
		[]byte(`inline constexpr const char* kManifestPath = "_data/7777/rad15/android/manifests/abc123";`+"\n"))
	entries := []entry{{"_data/7777/rad15/android/manifests/abc123", packManifest}, {"_data/7777/rad15/android/packages/abc123_1", packPackage}}
	pack := zipOf(t, filepath.Join(f.tmp, "pack.zip"), entries...)
	metaPath := writeFile(t, filepath.Join(f.tmp, "m.json"), mustJSON(t, metaWith(map[string]any{"zip_prefix": "_data/7777/rad15/android/"})))
	problems, err := StagePack(StagePackOptions{PackZip: pack, Meta: metaPath, Stage: f.stage, StoreAPK: f.store,
		PlanHeader: header, Out: &bytes.Buffer{}})
	if err != nil || len(problems) != 0 {
		t.Fatalf("got %q, %v", problems, err)
	}
	doc := decodeJSON(t, readFile(t, filepath.Join(f.stage, packJSONEntry))).(map[string]any)
	if doc["stock_data_version"] != "7777" {
		t.Fatalf("stock_data_version = %v", doc["stock_data_version"])
	}
	if problems := PackProblems(f.stage, header); len(problems) != 0 {
		t.Fatalf("pack-problems with the header: %q", problems)
	}
	if problems := PackProblems(f.stage, f.header); len(problems) == 0 {
		t.Fatal("the real pins accepted the other pins")
	}
}

// TestStagePack_StoreAPKWithPackEntryRefused is test_a_store_apk_that_already_has_a_pack_entry_is_refused.
func TestStagePack_StoreAPKWithPackEntryRefused(t *testing.T) {
	t.Parallel()
	f := newPackFixture(t)
	f.store = zipOf(t, filepath.Join(f.tmp, "store.apk"), entry{"AndroidManifest.xml", []byte("m")}, entry{packJSONEntry, []byte("{}")})
	f.assertRefused(f.stagePack(nil, nil), "already has the entry assets/nevr-pack/pack.json")
}

// TestStagePack_StoreAPKUnreadableRefused is test_a_store_apk_that_cannot_be_read_is_refused.
func TestStagePack_StoreAPKUnreadableRefused(t *testing.T) {
	t.Parallel()
	f := newPackFixture(t)
	f.store = filepath.Join(f.tmp, "no-such.apk")
	f.assertRefused(f.stagePack(nil, nil), "cannot be read as the store APK")
}

// sparsePrefix makes a file of size bytes that ends in archive: a hole in front, so a zip reader finds the end record.
func sparsePrefix(t *testing.T, path string, size int64, archive []byte) {
	t.Helper()
	f, err := os.Create(path)
	if err != nil {
		t.Fatalf("%v", err)
	}
	defer f.Close()
	if err := f.Truncate(size - int64(len(archive))); err != nil {
		t.Fatalf("%v", err)
	}
	if _, err := f.Seek(size-int64(len(archive)), 0); err != nil {
		t.Fatalf("%v", err)
	}
	if _, err := f.Write(archive); err != nil {
		t.Fatalf("%v", err)
	}
}

// TestStagePack_FourGiBLimit is test_the_apk_must_stay_under_four_gib_with_the_files_the_slack_and_the_store.
func TestStagePack_FourGiBLimit(t *testing.T) {
	t.Parallel()
	f := newPackFixture(t)
	files := int64(len(packManifest) + len(packPackage))
	archive := readFile(t, f.store)
	exact := apkLimitTests - slackTests - files
	sparsePrefix(t, f.store, exact, archive)
	if problems := f.stagePack(nil, nil); len(problems) != 0 {
		t.Fatalf("an APK at exactly the limit was refused: %q", problems)
	}
	if err := os.RemoveAll(filepath.Join(f.stage, "assets")); err != nil {
		t.Fatalf("%v", err)
	}
	sparsePrefix(t, f.store, exact+1, archive)
	f.assertRefused(f.stagePack(nil, nil), "zip without zip64")
}

// stagedPackFixture is the Python PackProblemsTest: a pack staged whole, then changed to check each rule.
type stagedPackFixture struct {
	*packFixture
	jsonPath string
}

func newStagedPack(t *testing.T) *stagedPackFixture {
	t.Helper()
	f := newPackFixture(t)
	if problems := f.stagePack(nil, nil); len(problems) != 0 {
		t.Fatalf("staging: %q", problems)
	}
	return &stagedPackFixture{packFixture: f, jsonPath: filepath.Join(f.stage, packJSONEntry)}
}

func (s *stagedPackFixture) document() map[string]any {
	return decodeJSON(s.t, readFile(s.t, s.jsonPath)).(map[string]any)
}

func (s *stagedPackFixture) write(doc map[string]any) {
	writeFile(s.t, s.jsonPath, mustJSON(s.t, doc))
}

// clonedDoc is a deep copy of a decoded document (JSON round trip).
func clonedDoc(t *testing.T, doc map[string]any) map[string]any {
	t.Helper()
	out, ok := decodeJSON(t, mustJSON(t, doc)).(map[string]any)
	if !ok {
		t.Fatal("not an object")
	}
	return out
}

// TestPackProblems_NothingStagedAndWholePackFine is test_nothing_staged_is_fine_and_a_staged_pack_that_is_whole_is_fine.
func TestPackProblems_NothingStagedAndWholePackFine(t *testing.T) {
	t.Parallel()
	s := newStagedPack(t)
	if problems := PackProblems(s.stage, s.header); len(problems) != 0 {
		t.Fatalf("whole pack: %q", problems)
	}
	if problems := PackProblems(filepath.Join(s.tmp, "nothing-staged"), s.header); len(problems) != 0 {
		t.Fatalf("nothing staged: %q", problems)
	}
}

// TestPackProblems_EveryRuleOfTheRuntime is test_every_rule_of_the_runtime_is_checked: each mutation of the whole
// description is refused; the whole one is accepted.
func TestPackProblems_EveryRuleOfTheRuntime(t *testing.T) {
	t.Parallel()
	s := newStagedPack(t)
	pristine := s.document()
	files := func(d map[string]any) []any { return d["files"].([]any) }
	cases := map[string]func(d map[string]any){
		"schema 2":             func(d map[string]any) { d["schema"] = 2 },
		"schema true":          func(d map[string]any) { d["schema"] = true },
		"a bad id":             func(d map[string]any) { d["id"] = "A B" },
		"a bad version":        func(d map[string]any) { d["version"] = "1.2" },
		"another data version": func(d map[string]any) { d["stock_data_version"] = "1" },
		"no requires":          func(d map[string]any) { d["requires_stock_manifest_sha256"] = []any{} },
		"unsorted requires": func(d map[string]any) {
			d["requires_stock_manifest_sha256"] = []any{descendingPair()[0], descendingPair()[1]}
		},
		"duplicate requires":    func(d map[string]any) { d["requires_stock_manifest_sha256"] = []any{stockA, stockA} },
		"no files":              func(d map[string]any) { d["files"] = []any{} },
		"a path that climbs":    func(d map[string]any) { files(d)[0].(map[string]any)["path"] = "packages/../x" },
		"no manifest":           func(d map[string]any) { d["files"] = []any{files(d)[1]} },
		"a bad file digest":     func(d map[string]any) { files(d)[0].(map[string]any)["sha256"] = "zz" },
		"a zero size":           func(d map[string]any) { files(d)[0].(map[string]any)["size"] = 0 },
		"a boolean size":        func(d map[string]any) { files(d)[0].(map[string]any)["size"] = true },
		"a size over the limit": func(d map[string]any) { files(d)[0].(map[string]any)["size"] = 1 << 31 },
		"a duplicate path": func(d map[string]any) {
			first := clonedDoc(t, map[string]any{"e": files(d)[0]})["e"]
			d["files"] = append(files(d), first)
		},
		"no source digest":   func(d map[string]any) { delete(d, "source_zip_sha256") },
		"a zero source size": func(d map[string]any) { d["source_zip_size"] = 0 },
	}
	for label, change := range cases {
		t.Run(label, func(t *testing.T) {
			doc := clonedDoc(t, pristine)
			change(doc)
			s.write(doc)
			if problems := PackProblems(s.stage, s.header); len(problems) == 0 {
				t.Fatalf("%s was accepted", label)
			}
		})
	}
	s.write(clonedDoc(t, pristine))
	if problems := PackProblems(s.stage, s.header); len(problems) != 0 {
		t.Fatalf("the whole description: %q", problems)
	}
}

// TestPackProblems_NotJSONOrNotObject is test_a_pack_json_that_is_not_json_or_not_an_object_is_refused.
func TestPackProblems_NotJSONOrNotObject(t *testing.T) {
	t.Parallel()
	s := newStagedPack(t)
	writeFile(t, s.jsonPath, []byte("{"))
	if !hasSub(PackProblems(s.stage, s.header), "cannot be read as JSON") {
		t.Fatal("not JSON was accepted")
	}
	writeFile(t, s.jsonPath, []byte("[1]"))
	if !hasSub(PackProblems(s.stage, s.header), "not a JSON object") {
		t.Fatal("an array was accepted")
	}
}

// TestPackProblems_FilesWithNoPackJSON is test_files_with_no_pack_json_are_refused.
func TestPackProblems_FilesWithNoPackJSON(t *testing.T) {
	t.Parallel()
	s := newStagedPack(t)
	if err := os.Remove(s.jsonPath); err != nil {
		t.Fatalf("%v", err)
	}
	if !hasSub(PackProblems(s.stage, s.header), "without a pack.json") {
		t.Fatal("no pack.json was accepted")
	}
}

// TestPackProblems_ListedFileNotStaged is test_a_listed_file_that_is_not_staged_is_named.
func TestPackProblems_ListedFileNotStaged(t *testing.T) {
	t.Parallel()
	s := newStagedPack(t)
	if err := os.Remove(filepath.Join(s.stage, packFilesDir, packagePath)); err != nil {
		t.Fatalf("%v", err)
	}
	if !hasSub(PackProblems(s.stage, s.header), packagePath+": ") || !hasSub(PackProblems(s.stage, s.header), "not staged as a file") {
		t.Fatal("the missing file is not named")
	}
}

// TestPackProblems_StagedFileWrongSizeOrDigest is test_a_staged_file_with_another_size_or_digest_is_named.
func TestPackProblems_StagedFileWrongSizeOrDigest(t *testing.T) {
	t.Parallel()
	s := newStagedPack(t)
	path := filepath.Join(s.stage, packFilesDir, packagePath)
	writeFile(t, path, append(append([]byte(nil), packPackage...), 'x'))
	if !hasSub(PackProblems(s.stage, s.header), "bytes staged, pack.json says") {
		t.Fatal("a longer file was accepted")
	}
	writeFile(t, path, append([]byte{1}, packPackage[1:]...))
	if !hasSub(PackProblems(s.stage, s.header), "sha256 pack.json lists") {
		t.Fatal("a file with another digest was accepted")
	}
}

// TestPackProblems_StagedFileIsALink is test_a_staged_file_that_is_a_link_is_named.
func TestPackProblems_StagedFileIsALink(t *testing.T) {
	t.Parallel()
	s := newStagedPack(t)
	path := filepath.Join(s.stage, packFilesDir, packagePath)
	if err := os.Remove(path); err != nil {
		t.Fatalf("%v", err)
	}
	if err := os.Symlink(filepath.Join(s.stage, packFilesDir, manifestPath), path); err != nil {
		t.Fatalf("%v", err)
	}
	if !hasSub(PackProblems(s.stage, s.header), "not staged as a file") {
		t.Fatal("a link was accepted")
	}
}

// TestPackProblems_StagedFileNotListed is test_a_staged_file_pack_json_does_not_list_is_named.
func TestPackProblems_StagedFileNotListed(t *testing.T) {
	t.Parallel()
	s := newStagedPack(t)
	writeFile(t, filepath.Join(s.stage, packFilesDir, "packages", "extra"), []byte("x"))
	if !hasSub(PackProblems(s.stage, s.header), "packages/extra: staged, not listed") {
		t.Fatal("the extra file is not named")
	}
}

// TestPackProblems_SomethingElseInThePackDirectory is test_something_else_in_the_pack_directory_is_named.
func TestPackProblems_SomethingElseInThePackDirectory(t *testing.T) {
	t.Parallel()
	s := newStagedPack(t)
	writeFile(t, filepath.Join(s.stage, packDir, "notes.txt"), []byte("x"))
	if !hasSub(PackProblems(s.stage, s.header), "notes.txt: not part of a pack") {
		t.Fatal("notes.txt is not named")
	}
}

// TestVerifySignedPack_Fixture is the Python VerifySignedPackTest fixture: the store, the stage with the loader and the
// stand-in's libr15, the signed entries (the pack stored), and the verify over them.
type verifySignedPack struct {
	*packFixture
	patcher string
	signed  []methodEntry
	store   string
}

// methodEntry is one entry of a signed APK with its method.
type methodEntry struct {
	name   string
	data   []byte
	method uint16
}

func newVerifySignedPack(t *testing.T) *verifySignedPack {
	t.Helper()
	s := newStagedPack(t)
	writeFile(t, filepath.Join(s.stage, abiDir, "libovrplatformloader.so"), []byte("shim"))
	fake := testkit.Fake(t, repackfixture.ModeOK)
	store := zipOf(t, filepath.Join(s.tmp, "store.apk"), entry{"AndroidManifest.xml", []byte("m")},
		entry{libr15Path, []byte("game")}, entry{abiDir + "/libovrplatformloader.so", []byte("loader")})
	writeFile(t, filepath.Join(s.stage, libr15Path), []byte("game"+repackfixture.Marker))
	v := &verifySignedPack{packFixture: s.packFixture, patcher: filepath.Join(fake.Dir(), repackfixture.ModeOK), store: store}
	v.signed = []methodEntry{
		{"AndroidManifest.xml", []byte("m"), zip.Deflate},
		{abiDir + "/libovrplatformloader.so", []byte("shim"), zip.Store},
		{libr15Path, []byte("game" + repackfixture.Marker), zip.Deflate},
	}
	var packNames []string
	_ = filepath.WalkDir(filepath.Join(s.stage, packDir), func(path string, d fs.DirEntry, err error) error {
		if err == nil && d.Type().IsRegular() {
			rel, _ := filepath.Rel(s.stage, path)
			packNames = append(packNames, filepath.ToSlash(rel))
		}
		return nil
	})
	sort.Strings(packNames)
	for _, name := range packNames {
		v.signed = append(v.signed, methodEntry{name, readFile(t, filepath.Join(s.stage, name)), zip.Store})
	}
	return v
}

// verify writes the signed APK (the given one, or the entries) and runs verify-signed on it.
func (v *verifySignedPack) verify(t *testing.T, signedPath string) []string {
	t.Helper()
	if signedPath == "" {
		signedPath = filepath.Join(v.tmp, "signed.apk")
		writeMethodZip(t, signedPath, v.signed)
	}
	problems, err := VerifySigned(VerifyOptions{Source: v.store, Signed: signedPath, Stage: v.stage, Patcher: v.patcher,
		Scratch: v.tmp, PlanHeader: v.header, PatcherTimeout: patcherTimeoutDefault, Out: &bytes.Buffer{}})
	if err != nil {
		t.Fatalf("verify-signed: %v", err)
	}
	return problems
}

// writeMethodZip writes entries, each with its own method.
func writeMethodZip(t *testing.T, path string, entries []methodEntry) {
	t.Helper()
	f, err := os.Create(path)
	if err != nil {
		t.Fatalf("%v", err)
	}
	defer f.Close()
	zw := zip.NewWriter(f)
	for _, e := range entries {
		mustCreate(t, zw, &zip.FileHeader{Name: e.name, Method: e.method}, e.data)
	}
	if err := zw.Close(); err != nil {
		t.Fatalf("%v", err)
	}
}

// removeEntry drops every entry named name from the signed list.
func (v *verifySignedPack) removeEntry(name string) {
	kept := v.signed[:0]
	for _, e := range v.signed {
		if e.name != name {
			kept = append(kept, e)
		}
	}
	v.signed = kept
}

func (v *verifySignedPack) setEntry(name string, data []byte, method uint16) {
	for i := range v.signed {
		if v.signed[i].name == name {
			v.signed[i] = methodEntry{name, data, method}
			return
		}
	}
	v.signed = append(v.signed, methodEntry{name, data, method})
}

// TestVerifySignedPack_SignedWithEveryPackFileStoredPasses is test_a_signed_apk_that_holds_every_pack_file_stored_passes.
func TestVerifySignedPack_SignedWithEveryPackFileStoredPasses(t *testing.T) {
	t.Parallel()
	v := newVerifySignedPack(t)
	if problems := v.verify(t, ""); len(problems) != 0 {
		t.Fatalf("got %q", problems)
	}
}

// TestVerifySignedPack_MissingPackFileNamed is test_a_pack_file_missing_from_the_signed_apk_is_named.
func TestVerifySignedPack_MissingPackFileNamed(t *testing.T) {
	t.Parallel()
	v := newVerifySignedPack(t)
	v.removeEntry("assets/nevr-pack/files/" + packagePath)
	if !hasSub(v.verify(t, ""), "assets/nevr-pack/files/"+packagePath+": staged, missing from the signed APK") {
		t.Fatal("the missing pack file is not named")
	}
}

// TestVerifySignedPack_OtherBytesNamed is test_a_pack_file_with_other_bytes_is_named.
func TestVerifySignedPack_OtherBytesNamed(t *testing.T) {
	t.Parallel()
	v := newVerifySignedPack(t)
	v.setEntry("assets/nevr-pack/files/"+packagePath, []byte("other"), zip.Store)
	if !hasSub(v.verify(t, ""), "different bytes than the staged file") {
		t.Fatal("other bytes are not named")
	}
}

// TestVerifySignedPack_DeflatedPackFileNamed is test_a_pack_file_that_is_deflated_is_named.
func TestVerifySignedPack_DeflatedPackFileNamed(t *testing.T) {
	t.Parallel()
	v := newVerifySignedPack(t)
	v.setEntry("assets/nevr-pack/files/"+packagePath, packPackage, zip.Deflate)
	if !hasSub(v.verify(t, ""), "not STORED") {
		t.Fatal("a deflated pack file is not named")
	}
}

// TestVerifySignedPack_PackJSONHeldToTheSameRule is test_pack_json_is_held_to_the_same_rule.
func TestVerifySignedPack_PackJSONHeldToTheSameRule(t *testing.T) {
	t.Parallel()
	v := newVerifySignedPack(t)
	v.setEntry(packJSONEntry, readFile(t, filepath.Join(v.stage, packJSONEntry)), zip.Deflate)
	if !hasSub(v.verify(t, ""), "not STORED") {
		t.Fatal("a deflated pack.json is not named")
	}
}

// TestVerifySignedPack_UnstagedPackEntryNamed is test_a_pack_entry_that_was_not_staged_is_named.
func TestVerifySignedPack_UnstagedPackEntryNamed(t *testing.T) {
	t.Parallel()
	v := newVerifySignedPack(t)
	v.signed = append(v.signed, methodEntry{"assets/nevr-pack/files/packages/smuggled", []byte("x"), zip.Store})
	if !hasSub(v.verify(t, ""), "smuggled: in the signed APK, not staged") {
		t.Fatal("the smuggled entry is not named")
	}
}

// TestVerifySignedPack_BuildWithNoPackCarriesNone is test_a_build_with_no_pack_carries_none.
func TestVerifySignedPack_BuildWithNoPackCarriesNone(t *testing.T) {
	t.Parallel()
	v := newVerifySignedPack(t)
	if err := os.RemoveAll(filepath.Join(v.stage, "assets")); err != nil {
		t.Fatalf("%v", err)
	}
	kept := v.signed[:0]
	for _, e := range v.signed {
		if !strings.HasPrefix(e.name, "assets/") {
			kept = append(kept, e)
		}
	}
	v.signed = kept
	for _, p := range v.verify(t, "") {
		if strings.Contains(p, "nevr-pack") {
			t.Fatalf("a build with no pack has a pack problem: %q", p)
		}
	}
	v.signed = append(v.signed, methodEntry{packJSONEntry, []byte("{}"), zip.Store})
	if !hasSub(v.verify(t, ""), "pack.json: in the signed APK, not staged") {
		t.Fatal("a pack.json with no staged pack is not named")
	}
}

// TestVerifySignedPack_ChangedAfterStagingNamed is test_a_staged_pack_the_runtime_would_refuse_is_named_before_anything_is_signed.
func TestVerifySignedPack_ChangedAfterStagingNamed(t *testing.T) {
	t.Parallel()
	v := newVerifySignedPack(t)
	writeFile(t, filepath.Join(v.stage, packFilesDir, packagePath), []byte("changed after staging"))
	p := v.verify(t, "")
	if !hasSub(p, "bytes staged, pack.json says") && !hasSub(p, "sha256 pack.json lists") {
		t.Fatalf("got %q", p)
	}
}

// TestVerifySignedPack_OverFourGiBNamed is test_a_signed_apk_over_four_gib_is_named.
func TestVerifySignedPack_OverFourGiBNamed(t *testing.T) {
	t.Parallel()
	v := newVerifySignedPack(t)
	inner := filepath.Join(v.tmp, "inner.apk")
	writeMethodZip(t, inner, v.signed)
	archive := readFile(t, inner)
	signed := filepath.Join(v.tmp, "signed.apk")
	sparsePrefix(t, signed, apkLimitTests, archive)
	if hasSub(v.verify(t, signed), "4 GiB") {
		t.Fatal("an APK of 0xFFFFFFFF bytes is not addressable")
	}
	sparsePrefix(t, signed, fourGiB, archive)
	if !hasSub(v.verify(t, signed), "4 GiB") {
		t.Fatal("an APK of 4 GiB is not named")
	}
}
