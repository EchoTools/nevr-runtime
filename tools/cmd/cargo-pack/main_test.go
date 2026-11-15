package main

import (
	"encoding/hex"
	"encoding/json"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"crypto/sha256"

	"github.com/klauspost/compress/zstd"

	"github.com/EchoTools/nevr-runtime/tools/internal/scriptgate"
)

const (
	testWindow                 = 1 << 12
	testDirPerm                = 0o755
	testFilePerm               = 0o644
	testAlign                  = 16
	smallBytes                 = 1000
	seedA, seedB, seedC, seedD = 3, 2, 1, 9 // distinct fill seeds
	bodyStride                 = 7          // any odd step: the bytes differ from one position to the next
	notPowerOfTwo              = 24         // an alignment the engine cannot use
	aboveMaxAlign              = 8192       // a power of two above the 4096 limit of cdn_validate.h
	largeBytes                 = 3 * testWindow
	// topKeyCount and entryKeyCount are the keys of a manifest document and of an entry with a stock_size.
	topKeyCount   = 4
	entryKeyCount = 9
)

func quiet() *slog.Logger { return slog.New(slog.NewJSONHandler(io.Discard, nil)) }

func decode(t *testing.T, frame []byte) []byte {
	t.Helper()
	dec, err := zstd.NewReader(nil)
	if err != nil {
		t.Fatal(err)
	}
	defer dec.Close()
	out, err := dec.DecodeAll(frame, nil)
	if err != nil {
		t.Fatal(err)
	}
	return out
}

func body(n int, seed byte) []byte {
	b := make([]byte, n)
	for i := range b {
		b[i] = seed + byte(i*bodyStride)
	}
	return b
}

func writeSpec(t *testing.T, entries []specEntry, files map[string][]byte) string {
	t.Helper()
	dir := t.TempDir()
	for name, data := range files {
		if err := os.WriteFile(filepath.Join(dir, name), data, testFilePerm); err != nil {
			t.Fatal(err)
		}
	}
	raw, err := json.Marshal(spec{Entries: entries})
	if err != nil {
		t.Fatal(err)
	}
	p := filepath.Join(dir, "spec.json")
	if err := os.WriteFile(p, raw, testFilePerm); err != nil {
		t.Fatal(err)
	}
	return p
}

func entry(name, file string) specEntry {
	return specEntry{Label: "lobby_posters", Platform: "android-arm64", Type: "0x489bb35d53ca50e9", Name: name, File: file, Align: testAlign}
}

func TestPackWritesNamedFramesAndAManifestThatListsThem(t *testing.T) {
	small, large := body(smallBytes, seedC), body(largeBytes, seedB)
	stock := uint64(largeBytes)
	e2 := entry("0x0000000000000002", "large.bin")
	e2.StockSize = &stock
	spec := writeSpec(t, []specEntry{entry("0x0000000000000001", "small.bin"), e2}, map[string][]byte{"small.bin": small, "large.bin": large})
	out := t.TempDir()
	sum, err := packSpec(spec, out, testWindow)
	if err != nil {
		t.Fatalf("pack: %v", err)
	}
	for i, want := range [][]byte{small, large} {
		o := sum.Objects[i]
		frame, err := os.ReadFile(filepath.Join(out, "blobs", "sha256", o.Sha256))
		if err != nil {
			t.Fatal(err)
		}
		h := sha256.Sum256(frame)
		if hex.EncodeToString(h[:]) != o.Sha256 || uint64(len(frame)) != o.Size || o.Bytes != uint64(len(want)) {
			t.Errorf("object %d: name, size or bytes do not match the file it holds: %+v", i, o)
		}
		if string(decode(t, frame)) != string(want) {
			t.Errorf("object %d does not decode to its file", i)
		}
	}
	mframe, err := os.ReadFile(filepath.Join(out, manifestName))
	if err != nil {
		t.Fatal(err)
	}
	var doc manifestDoc
	if err := json.Unmarshal(decode(t, mframe), &doc); err != nil {
		t.Fatal(err)
	}
	got := doc.Labels["lobby_posters"].Entries
	if doc.Schema != schemaName || len(got) != 2 || got[1].StockSize == nil || *got[1].StockSize != stock || got[0].StockSize != nil {
		t.Fatalf("manifest = %+v", doc)
	}
	if _, err := os.Stat(filepath.Join(out, "manifest.json")); err == nil {
		t.Error("a plain manifest.json was written")
	}
}

func TestPackRefusesBadInputAndWritesNothing(t *testing.T) {
	files := map[string][]byte{"a.bin": body(smallBytes, seedA)}
	bad := entry("0x0000000000000003", "a.bin")
	bad.Align = notPowerOfTwo
	huge := entry("0x0000000000000007", "a.bin")
	huge.Align = aboveMaxAlign
	dup := entry("0x0000000000000004", "a.bin")
	cases := map[string][]specEntry{
		"alignment": {bad},
		"too large": {huge},
		"duplicate": {dup, dup},
		"name":      {entry("0x5", "a.bin")},
		"absolute":  {entry("0x0000000000000006", "/etc/hostname")},
	}
	for name, entries := range cases {
		out := t.TempDir()
		if _, err := packSpec(writeSpec(t, entries, files), out, testWindow); err == nil {
			t.Errorf("%s: accepted", name)
		}
		if left, _ := os.ReadDir(out); len(left) != 0 {
			t.Errorf("%s: wrote %d entries", name, len(left))
		}
	}
}

func TestEncodeFrameStatesTheSizeAndKeepsTheWindow(t *testing.T) {
	for _, n := range []int{smallBytes, largeBytes} {
		frame, err := encodeFrame(body(n, seedD), testWindow)
		if err != nil {
			t.Fatal(err)
		}
		var h zstd.Header
		if err := h.Decode(frame); err != nil || !h.HasFCS || h.FrameContentSize != uint64(n) {
			t.Errorf("%d bytes: header %+v err %v", n, h, err)
		}
		w := h.WindowSize
		if h.SingleSegment {
			w = h.FrameContentSize
		}
		if w > testWindow {
			t.Errorf("%d bytes: the frame asks for a window of %d, above %d", n, w, testWindow)
		}
		if h.SingleSegment != (n <= testWindow) {
			t.Errorf("%d bytes: single segment = %t, want %t", n, h.SingleSegment, n <= testWindow)
		}
		if h.HasCheckSum {
			t.Errorf("%d bytes: frame carries a checksum", n)
		}
	}
}

// The manifest keys and values are the ones src/cdn/cdn_manifest.cpp reads (kTopKeys, kEntryKeys, kSchema, kSchemaVersion,
// kFormatZstdFrame): a renamed tag would round-trip through manifestDoc and be refused by every client.
func TestManifestUsesTheKeysTheClientParserReads(t *testing.T) {
	stock := uint64(smallBytes)
	e := entry("0x0000000000000001", "small.bin")
	e.StockSize = &stock
	spec := writeSpec(t, []specEntry{e}, map[string][]byte{"small.bin": body(smallBytes, seedC)})
	out := t.TempDir()
	if _, err := packSpec(spec, out, testWindow); err != nil {
		t.Fatal(err)
	}
	mframe, err := os.ReadFile(filepath.Join(out, manifestName))
	if err != nil {
		t.Fatal(err)
	}
	var top map[string]any
	if err := json.Unmarshal(decode(t, mframe), &top); err != nil {
		t.Fatal(err)
	}
	if top["schema"] != "nevr-cargo-manifest" || top["schema_version"] != float64(1) {
		t.Errorf("schema = %v, version = %v", top["schema"], top["schema_version"])
	}
	if _, ok := top["empty"].([]any); !ok || len(top) != topKeyCount {
		t.Errorf("top-level keys = %v", top)
	}
	labels := top["labels"].(map[string]any)
	entries := labels["lobby_posters"].(map[string]any)["entries"].([]any)
	got := entries[0].(map[string]any)
	for _, k := range []string{"platform", "type", "name", "format", "sha256", "size", "bytes", "align", "stock_size"} {
		if _, ok := got[k]; !ok {
			t.Errorf("entry has no key %q: %v", k, got)
		}
	}
	if got["format"] != "zstd-frame" || len(got) != entryKeyCount {
		t.Errorf("entry = %v", got)
	}
}

func packSpec(specPath, out string, window uint64) (*summary, error) {
	recs, err := specRecords(specPath)
	if err != nil {
		return nil, err
	}
	return build(recs, out, window, quiet(), scriptgate.Gate{})
}

// emptyLabel is the part of a manifest this test reads: each label's entries and its empty records.
type emptyLabel struct {
	Entries []entryDoc `json:"entries"`
	Empty   []emptyDoc `json:"empty"`
}

func readLabels(t *testing.T, out string) map[string]emptyLabel {
	t.Helper()
	mframe, err := os.ReadFile(filepath.Join(out, manifestName))
	if err != nil {
		t.Fatal(err)
	}
	var doc struct {
		Labels map[string]emptyLabel `json:"labels"`
		Empty  json.RawMessage       `json:"empty"`
	}
	if err := json.Unmarshal(decode(t, mframe), &doc); err != nil {
		t.Fatal(err)
	}
	if string(doc.Empty) != "[]" {
		t.Errorf("top-level empty = %s, want []", doc.Empty)
	}
	return doc.Labels
}

func countBlobs(t *testing.T, out string) int {
	t.Helper()
	left, err := os.ReadDir(filepath.Join(out, "blobs", "sha256"))
	if err != nil {
		t.Fatal(err)
	}
	return len(left)
}

func TestAFileOfZeroBytesInASpecIsAnEmptyRecordOfItsLabelAndPlatformWithNoBlob(t *testing.T) {
	e := entry("0x0000000000000009", "empty.bin")
	e.Label, e.Platform, e.Type = "combat_content", "windows-x86_64", pcPayloadType
	out := t.TempDir()
	// An empty record alone: the label has no entry and no object is written.
	sum, err := packSpec(writeSpec(t, []specEntry{e}, map[string][]byte{"empty.bin": {}}), out, testWindow)
	if err != nil {
		t.Fatalf("pack: %v", err)
	}
	if len(sum.Objects) != 0 || sum.EmptyRecords != 1 || countBlobs(t, out) != 0 {
		t.Errorf("objects %d, empty %d, blobs %d, want 0, 1, 0", len(sum.Objects), sum.EmptyRecords, countBlobs(t, out))
	}
	l := readLabels(t, out)["combat_content"]
	want := emptyDoc{Platform: "windows-x86_64", Type: e.Type, Name: e.Name, Align: testAlign}
	if len(l.Empty) != 1 || l.Empty[0] != want || len(l.Entries) != 0 {
		t.Errorf("label = %+v, want one empty record %+v and no entry", l, want)
	}
}

func TestEmptyAndNonEmptyEntriesOfASpecLandEachInTheirOwnLabel(t *testing.T) {
	full := entry("0x0000000000000001", "full.bin")
	hole := entry("0x0000000000000002", "empty.bin")
	other := entry("0x0000000000000003", "empty.bin")
	other.Label, other.Platform, other.Type = "second_label", "windows-x86_64", pcPayloadType
	files := map[string][]byte{"full.bin": body(smallBytes, seedA), "empty.bin": {}}
	out := t.TempDir()
	sum, err := packSpec(writeSpec(t, []specEntry{full, hole, other}, files), out, testWindow)
	if err != nil {
		t.Fatalf("pack: %v", err)
	}
	if len(sum.Objects) != 1 || sum.EmptyRecords != 2 || countBlobs(t, out) != 1 {
		t.Errorf("objects %d, empty %d, blobs %d, want 1, 2, 1", len(sum.Objects), sum.EmptyRecords, countBlobs(t, out))
	}
	labels := readLabels(t, out)
	posters, second := labels["lobby_posters"], labels["second_label"]
	if len(posters.Entries) != 1 || posters.Entries[0].Name != full.Name {
		t.Errorf("lobby_posters entries = %+v, want the one non-empty file", posters.Entries)
	}
	if len(posters.Empty) != 1 || posters.Empty[0] != (emptyDoc{Platform: "android-arm64", Type: hole.Type, Name: hole.Name, Align: testAlign}) {
		t.Errorf("lobby_posters empty = %+v, want the android-arm64 record named %s", posters.Empty, hole.Name)
	}
	if len(second.Entries) != 0 || len(second.Empty) != 1 || second.Empty[0].Platform != "windows-x86_64" || second.Empty[0].Name != other.Name {
		t.Errorf("second_label = %+v, want only the windows-x86_64 empty record named %s", second, other.Name)
	}
}

// Every named on-disk type is one class on one platform with one suffix: no two names hash to one type.
func TestEveryNamedTypeIsOneClassOfOnePlatform(t *testing.T) {
	if want := len(platformWord) * len(resourceClasses) * len(subtypeSuffixes); len(knownTypes) != want {
		t.Errorf("%d named types, want %d: two names hash to one type, or a class is listed twice", len(knownTypes), want)
	}
	// The types the C++ tree pins with static_asserts (src/core/resource_set_registry.h kWin10Types, kAndroidTypes) and the pair
	// tables of src/cdn/cdn_manifest.h: an independent statement of the same hash.
	for typ, want := range map[string]typeOwner{
		"0x2a41cf1c1d9e5d32": {"windows-x86_64", "CArchiveResourceWin10"},
		"0x991421b41de58370": {"windows-x86_64", "CScriptResourceWin10"},
		"0x4a4c32c49300b8a0": {"windows-x86_64", "CGTextureResourceWin10"},
		"0xbeac1969cb7b8861": {"windows-x86_64", "CGTextureResourceWin10GPU"},
		"0x004c5273be3eab58": {"android-arm64", "CArchiveResourceAndroid"},
		"0x275adc4c85e7ba74": {"android-arm64", "CGameLevelResourceAndroid"},
		"0x4fa8daee4329a4fc": {"android-arm64", "CScriptResourceAndroid"},
		"0xe2efe7289d5985b8": {"android-arm64", "CGTextureResourceAndroid"},
		"0x489bb35d53ca50e9": {"android-arm64", "CGTextureResourceAndroidGPU"},
	} {
		if got := knownTypes[typ]; got != want {
			t.Errorf("type %s = %+v, want %+v", typ, got, want)
		}
	}
}

// specEntryOf is one spec entry of combat_content.
func specEntryOf(plat, typ, name, file string) specEntry {
	return specEntry{Label: "combat_content", Platform: plat, Type: typ, Name: name, File: file, Align: testAlign}
}

const (
	pcPayloadType   = "0xbeac1969cb7b8861" // CGTextureResourceWin10GPU: a windows-x86_64 record's type where entry() gives the Quest's
	modelPC         = "0xea51a0d76eb90142" // CModelCRWin10
	modelQuest      = "0x042c7bbb8da4211c" // CModelCRAndroid
	lightmapPC      = "0x7b9a4c9fd03f37eb" // CGLightMapResourceWin10GPU
	lightmapQuest   = "0x8cce0f0b134fc0ed" // CGLightMapResourceAndroidGPU
	scriptImage     = "0x06eace9f64204fd0" // the PC script image type: no class name hashes to it
	sharedName      = "0x00000000000000c1"
	emptyName       = "0x00000000000000e1"
	platformsOfOne  = 2 // one name on both platforms
	recordsOfOneKey = 1
)

func TestOneLabelHoldsBothPlatformsEachWithItsOwnTypes(t *testing.T) {
	spec := writeSpec(t, []specEntry{
		specEntryOf("windows-x86_64", modelPC, sharedName, "pc.bin"),
		specEntryOf("android-arm64", modelQuest, sharedName, "quest.bin"),
		specEntryOf("windows-x86_64", lightmapPC, emptyName, "empty.bin"),
		specEntryOf("android-arm64", lightmapQuest, emptyName, "empty.bin"),
	}, map[string][]byte{"pc.bin": body(smallBytes, seedA), "quest.bin": body(smallBytes, seedB), "empty.bin": nil})
	out := t.TempDir()
	sum, err := packSpec(spec, out, testWindow)
	if err != nil {
		t.Fatal(err)
	}
	if len(sum.UnnamedTypes) != 0 {
		t.Errorf("unnamed types = %v, want none", sum.UnnamedTypes)
	}
	mframe, err := os.ReadFile(filepath.Join(out, manifestName))
	if err != nil {
		t.Fatal(err)
	}
	var doc manifestDoc
	if err := json.Unmarshal(decode(t, mframe), &doc); err != nil {
		t.Fatal(err)
	}
	l := doc.Labels["combat_content"]
	if len(doc.Labels) != recordsOfOneKey || len(l.Entries) != platformsOfOne || len(l.Empty) != platformsOfOne || len(doc.Empty) != 0 {
		t.Fatalf("labels %d, entries %d, label empty %d, top-level empty %d", len(doc.Labels), len(l.Entries), len(l.Empty), len(doc.Empty))
	}
	byPlatform := map[string]string{}
	for _, e := range l.Entries {
		byPlatform[e.Platform] = e.Type
	}
	if byPlatform["windows-x86_64"] != modelPC || byPlatform["android-arm64"] != modelQuest {
		t.Errorf("entries by platform = %v", byPlatform)
	}
	emptyBy := map[string]string{}
	for _, e := range l.Empty {
		emptyBy[e.Platform] = e.Type
	}
	if emptyBy["windows-x86_64"] != lightmapPC || emptyBy["android-arm64"] != lightmapQuest {
		t.Errorf("empty records by platform = %v: each stays with its platform in its label", emptyBy)
	}
}

func TestATypeOfTheOtherPlatformIsRefusedAndNothingIsWritten(t *testing.T) {
	for _, c := range []struct{ plat, typ, owner string }{
		{"android-arm64", modelPC, "CModelCRWin10"},
		{"windows-x86_64", modelQuest, "CModelCRAndroid"},
		{"android-arm64", lightmapPC, "CGLightMapResourceWin10GPU"},
	} {
		files := map[string][]byte{"a.bin": body(smallBytes, seedA)}
		file := "a.bin"
		if c.typ == lightmapPC {
			files, file = map[string][]byte{"e.bin": nil}, "e.bin" // a record of 0 bytes is checked too
		}
		spec := writeSpec(t, []specEntry{specEntryOf(c.plat, c.typ, sharedName, file)}, files)
		out := t.TempDir()
		_, err := packSpec(spec, out, testWindow)
		if err == nil || !strings.Contains(err.Error(), c.owner) || !strings.Contains(err.Error(), "not of "+c.plat) {
			t.Errorf("%s under %s: err = %v", c.typ, c.plat, err)
		}
		if _, err := os.Stat(filepath.Join(out, manifestName)); !os.IsNotExist(err) {
			t.Errorf("%s under %s wrote a manifest", c.typ, c.plat)
		}
	}
}

func TestAnUnnamedTypeIsTakenAndListedForItsPlatform(t *testing.T) {
	spec := writeSpec(t, []specEntry{specEntryOf("windows-x86_64", scriptImage, sharedName, "s.bin")}, map[string][]byte{"s.bin": body(smallBytes, seedC)})
	sum, err := packSpec(spec, t.TempDir(), testWindow)
	if err != nil {
		t.Fatal(err)
	}
	if got := sum.UnnamedTypes["windows-x86_64"]; len(got) != recordsOfOneKey || got[0] != scriptImage || len(sum.UnnamedTypes) != recordsOfOneKey {
		t.Errorf("unnamed types = %v, want windows-x86_64: [%s]", sum.UnnamedTypes, scriptImage)
	}
}
