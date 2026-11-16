package main

import (
	"encoding/hex"
	"encoding/json"
	"io"
	"log/slog"
	"os"
	"path/filepath"
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
	e.Label, e.Platform = "combat_content", "windows-x86_64"
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
	other.Label, other.Platform = "second_label", "windows-x86_64"
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
