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
)

const (
	testWindow   = 1 << 12
	testDirPerm  = 0o755
	testFilePerm = 0o644
	testAlign    = 16
	smallBytes   = 1000
	largeBytes   = 3 * testWindow
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
		b[i] = seed + byte(i*7)
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
	small, large := body(smallBytes, 1), body(largeBytes, 2)
	stock := uint64(largeBytes)
	e2 := entry("0x0000000000000002", "large.bin")
	e2.StockSize = &stock
	spec := writeSpec(t, []specEntry{entry("0x0000000000000001", "small.bin"), e2}, map[string][]byte{"small.bin": small, "large.bin": large})
	out := t.TempDir()
	sum, err := pack(spec, out, testWindow, quiet())
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
	files := map[string][]byte{"a.bin": body(smallBytes, 3), "empty.bin": {}}
	bad := entry("0x0000000000000003", "a.bin")
	bad.Align = 24
	huge := entry("0x0000000000000007", "a.bin")
	huge.Align = 8192
	dup := entry("0x0000000000000004", "a.bin")
	cases := map[string][]specEntry{
		"alignment": {bad},
		"too large": {huge},
		"duplicate": {dup, dup},
		"empty":     {entry("0x0000000000000005", "empty.bin")},
		"name":      {entry("0x5", "a.bin")},
		"absolute":  {entry("0x0000000000000006", "/etc/hostname")},
	}
	for name, entries := range cases {
		out := t.TempDir()
		if _, err := pack(writeSpec(t, entries, files), out, testWindow, quiet()); err == nil {
			t.Errorf("%s: accepted", name)
		}
		if left, _ := os.ReadDir(out); len(left) != 0 {
			t.Errorf("%s: wrote %d entries", name, len(left))
		}
	}
}

func TestEncodeFrameStatesTheSizeAndKeepsTheWindow(t *testing.T) {
	for _, n := range []int{smallBytes, largeBytes} {
		frame, err := encodeFrame(body(n, 9), testWindow)
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
		if h.HasCheckSum {
			t.Errorf("%d bytes: frame carries a checksum", n)
		}
	}
}
