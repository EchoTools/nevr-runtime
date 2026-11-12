package main

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"log/slog"
	"math/rand/v2"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"
	"time"

	"github.com/klauspost/compress/zstd"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
	"github.com/EchoTools/nevr-runtime/tools/internal/testkit"
)

// The fixture is a data root built by the test in the game's own layout: two package sets, each a manifest and its
// package files, with records packed several to a chunk, a record larger than the block size alone in its chunk, a record
// of 0 bytes, and records whose bytes are the same under different names. No byte of it comes from the game.

const (
	setA = "48037dc70b0ecab2"
	setB = "2b47aab238f60515"
)

type res struct {
	typ, name int64
	data      []byte
	align     uint32
}

// chunkSpec is the records of one chunk, back to back at their alignment, in package pkg.
type chunkSpec struct {
	pkg  uint32
	recs []res
}

func fill(n int, b byte) []byte { return bytes.Repeat([]byte{b}, n) }

// noise is n bytes that do not compress, the same every time.
func noise(n int, seed uint64) []byte {
	r := rand.New(rand.NewPCG(seed, seed))
	b := make([]byte, n)
	for i := range b {
		b[i] = byte(r.Uint32())
	}
	return b
}

// writeSet writes the set `id` under <root>/rad15/<folder>: a manifest of the chunks as given and one package file per
// package, with the end record each package has in the shipped sets.
func writeSet(t *testing.T, root, folder, id string, specs []chunkSpec) {
	t.Helper()
	data := filepath.Join(root, "rad15", folder)
	for _, d := range []string{"manifests", "packages"} {
		if err := os.MkdirAll(filepath.Join(data, d), 0o755); err != nil {
			t.Fatal(err)
		}
	}
	enc, err := zstd.NewWriter(nil)
	if err != nil {
		t.Fatal(err)
	}
	defer enc.Close()
	m := &radpack.Manifest{Head: make([]byte, 0xc0)}
	files := map[uint32][]byte{}
	for ci, sp := range specs {
		var plain []byte
		for _, r := range sp.recs {
			for uint32(len(plain))%r.align != 0 {
				plain = append(plain, 0)
			}
			m.Resources = append(m.Resources, radpack.Resource{Type: r.typ, Name: r.name, Chunk: uint32(ci),
				Offset: uint32(len(plain)), Size: uint32(len(r.data)), Alignment: r.align})
			plain = append(plain, r.data...)
		}
		frame := enc.EncodeAll(plain, nil)
		m.Chunks = append(m.Chunks, radpack.Chunk{Package: sp.pkg, Offset: uint32(len(files[sp.pkg])),
			Compressed: uint32(len(frame)), Decompressed: uint32(len(plain))})
		files[sp.pkg] = append(files[sp.pkg], frame...)
	}
	var pkgs []uint32
	for p := range files {
		pkgs = append(pkgs, p)
	}
	sort.Slice(pkgs, func(a, b int) bool { return pkgs[a] < pkgs[b] })
	for _, p := range pkgs {
		m.Chunks = append(m.Chunks, radpack.Chunk{Package: p, Offset: uint32(len(files[p])), Compressed: 0x7f000000}) // an end record's other words are not a frame's: the shipped sets hold arbitrary values there
		if err := os.WriteFile(filepath.Join(data, "packages", fmt.Sprintf("%s_%d", id, p)), files[p], 0o644); err != nil {
			t.Fatal(err)
		}
	}
	sort.SliceStable(m.Resources, func(a, b int) bool {
		x, y := m.Resources[a], m.Resources[b]
		return x.Type < y.Type || (x.Type == y.Type && x.Name < y.Name)
	})
	for _, r := range m.Resources {
		var f [40]byte
		binary.LittleEndian.PutUint64(f[0:], uint64(r.Type))
		binary.LittleEndian.PutUint64(f[8:], uint64(r.Name))
		m.Files = append(m.Files, f)
	}
	binary.LittleEndian.PutUint32(m.Head[0:], uint32(len(pkgs)))
	binary.LittleEndian.PutUint32(m.Head[4:], 0x80000)
	raw, err := m.EncodeFile()
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(data, "manifests", id), raw, 0o644); err != nil {
		t.Fatal(err)
	}
}

var (
	small1 = res{0x10, 1, fill(100, 'a'), 16}
	small2 = res{0x10, 2, fill(200, 'b'), 16}
	small3 = res{0x10, 3, fill(300, 'c'), 16}
	twin   = res{0x10, 4, fill(100, 'a'), 16} // the bytes of small1 under another name, in another chunk
	large  = res{0x30, 1, noise(1<<20, 7), 4096}
	empty  = res{0x20, 1, nil, 16}
	other  = res{0x10, 5, fill(100, 'a'), 16} // in the second set: the same bytes a third time
	solo   = res{0x40, 1, fill(50, 'z'), 4}
)

// fixture writes the standard data root for a platform folder and returns it.
func fixture(t *testing.T, folder string) string {
	t.Helper()
	root := t.TempDir()
	writeSet(t, root, folder, setA, []chunkSpec{
		{0, []res{small1, small2, small3, empty}},
		{1, []res{large}},
		{0, []res{twin}},
	})
	writeSet(t, root, folder, setB, []chunkSpec{{0, []res{other, solo}}})
	return root
}

type result struct {
	code           int
	stdout, stderr string
	env            []string
}

func runTool(t *testing.T, args ...string) result {
	t.Helper()
	env := testkit.Env(t)
	var stdout, stderr bytes.Buffer
	code := run(t.Context(), args, testkit.Getenv(env), strings.NewReader(""), &stdout, &stderr)
	return result{code: code, stdout: stdout.String(), stderr: stderr.String(), env: env}
}

func extractArgs(root, out string, extra ...string) []string {
	return append([]string{"extract", "-root", root, "-platform", "windows-x86_64", "-out", out}, extra...)
}

func mustExtract(t *testing.T, root, out string, extra ...string) summary {
	t.Helper()
	r := runTool(t, extractArgs(root, out, extra...)...)
	if r.code != 0 {
		t.Fatalf("extract exit %d: %s", r.code, r.stderr)
	}
	var sum summary
	if err := json.Unmarshal([]byte(r.stdout), &sum); err != nil {
		t.Fatalf("the summary is not JSON: %v: %q", err, r.stdout)
	}
	return sum
}

func readManifest(t *testing.T, out string) manifestDoc {
	t.Helper()
	raw, err := os.ReadFile(filepath.Join(out, "manifest.json"))
	if err != nil {
		t.Fatal(err)
	}
	var doc manifestDoc
	if err := json.Unmarshal(raw, &doc); err != nil {
		t.Fatal(err)
	}
	return doc
}

func sha(b []byte) string {
	h := sha256.Sum256(b)
	return hex.EncodeToString(h[:])
}

func TestExtractThenVerifyPasses(t *testing.T) {
	t.Parallel()
	root := fixture(t, "win10")
	out := filepath.Join(t.TempDir(), "out")
	sum := mustExtract(t, root, out)

	// 8 records, 1 of them of 0 bytes; the three records of 100 'a' bytes are one object: 5 objects, 2 saved.
	if sum.Records != 8 || sum.Entries != 7 || sum.ZeroByteRecords != 1 || sum.ObjectsWritten != 5 || sum.RecordsWithObject != 7 || sum.Objects != 5 {
		t.Errorf("summary = %+v", sum)
	}
	if sum.RecordsWithObject != sum.Entries {
		t.Errorf("records_with_object %d is not the %d records that have bytes", sum.RecordsWithObject, sum.Entries)
	}
	if sum.RecordsByType["0x0000000000000010"] != 5 || sum.RecordsByType["0x0000000000000020"] != 1 || len(sum.RecordsByType) != 4 {
		t.Errorf("records_by_type = %v", sum.RecordsByType)
	}
	if sum.RecordBytes != 100+200+300+100+(1<<20)+100+50 {
		t.Errorf("record_bytes = %d", sum.RecordBytes)
	}
	if sum.LargestRecordBytes != 1<<20 || sum.LargestObject.Bytes < 1<<20 {
		t.Errorf("largest record %d, largest object %+v", sum.LargestRecordBytes, sum.LargestObject)
	}
	var packageBytes uint64
	packages, _ := filepath.Glob(filepath.Join(root, "rad15", "win10", "packages", "*"))
	for _, p := range packages {
		fi, err := os.Stat(p)
		if err != nil {
			t.Fatal(err)
		}
		packageBytes += uint64(fi.Size())
	}
	if sum.StockChunkBytes != packageBytes || len(packages) != 3 {
		t.Errorf("stock_chunk_bytes = %d, the %d package files hold %d (an end record is not a chunk)", sum.StockChunkBytes, len(packages), packageBytes)
	}
	if sum.ObjectBytes == 0 || sum.StockChunkPlain == 0 {
		t.Errorf("bytes: objects %d, stock chunks %d (%d decompressed)", sum.ObjectBytes, sum.StockChunkBytes, sum.StockChunkPlain)
	}

	doc := readManifest(t, out)
	if doc.Schema != "nevr-cargo-manifest" || doc.SchemaVersion != 1 || len(doc.Labels) != 1 || len(doc.Sets) != 2 {
		t.Errorf("manifest = %+v", doc)
	}
	entries := doc.Labels["game"].Entries
	if len(entries) != 7 || len(doc.Empty) != 1 || doc.Empty[0] != (emptyDoc{Type: "0x0000000000000020", Name: "0x0000000000000001", Align: 16}) {
		t.Fatalf("%d entries, empty = %+v", len(entries), doc.Empty)
	}
	want := map[string][]byte{}
	for _, r := range []res{small1, small2, small3, twin, large, other, solo} {
		want[fmt.Sprintf("0x%016x:0x%016x", uint64(r.typ), uint64(r.name))] = r.data
	}
	for _, e := range entries {
		data, ok := want[e.Type+":"+e.Name]
		if !ok {
			t.Errorf("entry %s:%s is not a fixture record", e.Type, e.Name)
			continue
		}
		frame, err := os.ReadFile(filepath.Join(out, "blobs", "sha256", e.Sha256))
		if err != nil {
			t.Fatalf("entry %s:%s: %v", e.Type, e.Name, err)
		}
		if sha(frame) != e.Sha256 || uint64(len(frame)) != e.Size || e.Bytes != uint64(len(data)) || e.Format != "zstd-frame" || e.Platform != "windows-x86_64" {
			t.Errorf("entry %+v does not describe its object (%d bytes, sha %s)", e, len(frame), sha(frame))
		}
		if binary.LittleEndian.Uint32(frame) != zstdMagic {
			t.Errorf("entry %s:%s: the object does not start with the zstd magic", e.Type, e.Name)
		}
		var h zstd.Header
		if err := h.Decode(frame); err != nil || !h.HasFCS || h.FrameContentSize != e.Bytes {
			t.Errorf("entry %s:%s: frame header %+v (%v) does not state %d bytes", e.Type, e.Name, h, err, e.Bytes)
		}
		dec, err := zstd.NewReader(nil)
		if err != nil {
			t.Fatal(err)
		}
		got, err := dec.DecodeAll(frame, nil)
		dec.Close()
		if err != nil || !bytes.Equal(got, data) {
			t.Errorf("entry %s:%s: the frame does not decompress to the record's bytes (%v)", e.Type, e.Name, err)
		}
	}
	if blobs, _ := os.ReadDir(filepath.Join(out, "blobs", "sha256")); len(blobs) != 5 {
		t.Errorf("%d files in blobs/sha256, want 5", len(blobs))
	}

	v := runTool(t, "verify", out, root)
	if v.code != 0 {
		t.Fatalf("verify exit %d: %s", v.code, v.stderr)
	}
	var vs verifySummary
	if err := json.Unmarshal([]byte(v.stdout), &vs); err != nil {
		t.Fatal(err)
	}
	if vs.StockRecords != 8 || vs.Entries != 7 || vs.EmptyRecords != 1 || vs.ObjectsRead != 7 || vs.Mismatches != 0 || vs.BytesCompared != sum.RecordBytes {
		t.Errorf("verify summary = %+v", vs)
	}
}

func TestAnObjectAlreadyPresentIsNotRewritten(t *testing.T) {
	t.Parallel()
	root := fixture(t, "win10")
	out := filepath.Join(t.TempDir(), "out")
	mustExtract(t, root, out)
	blobs := filepath.Join(out, "blobs", "sha256")
	old := time.Date(2001, 1, 1, 0, 0, 0, 0, time.UTC)
	files, err := os.ReadDir(blobs)
	if err != nil {
		t.Fatal(err)
	}
	for _, f := range files {
		if err := os.Chtimes(filepath.Join(blobs, f.Name()), old, old); err != nil {
			t.Fatal(err)
		}
	}
	again := mustExtract(t, root, out)
	if again.ObjectsWritten != 0 || again.RecordsWithObject != 7 || again.Objects != 5 {
		t.Errorf("a second run wrote %d objects, has %d records with an object and %d objects, want 0, 7 and 5", again.ObjectsWritten, again.RecordsWithObject, again.Objects)
	}
	for _, f := range files {
		fi, err := os.Stat(filepath.Join(blobs, f.Name()))
		if err != nil || !fi.ModTime().Equal(old) {
			t.Errorf("%s was rewritten (mtime %v, %v)", f.Name(), fi.ModTime(), err)
		}
	}
	if left, _ := filepath.Glob(filepath.Join(blobs, ".tmp-*")); len(left) != 0 {
		t.Errorf("temporary files left: %v", left)
	}
}

func TestAnObjectOnDiskWithAnotherSizeIsRefused(t *testing.T) {
	t.Parallel()
	root := fixture(t, "win10")
	out := filepath.Join(t.TempDir(), "out")
	mustExtract(t, root, out)
	doc := readManifest(t, out)
	e := doc.Labels["game"].Entries[0]
	if err := os.WriteFile(filepath.Join(out, "blobs", "sha256", e.Sha256), []byte("short"), 0o644); err != nil {
		t.Fatal(err)
	}
	r := runTool(t, extractArgs(root, out)...)
	if r.code != 1 || !strings.Contains(r.stderr, "is on disk with 5 bytes") {
		t.Errorf("exit %d: %s", r.code, r.stderr)
	}
}

func TestEmptyRecordsAreListedAndNotStored(t *testing.T) {
	t.Parallel()
	root := fixture(t, "win10")
	out := filepath.Join(t.TempDir(), "out")
	sum := mustExtract(t, root, out)
	doc := readManifest(t, out)
	for _, e := range doc.Labels["game"].Entries {
		if e.Type == "0x0000000000000020" {
			t.Errorf("the empty record is an entry: %+v", e)
		}
	}
	if len(doc.Empty) != sum.ZeroByteRecords || doc.Empty[0].Align != 16 {
		t.Errorf("empty = %+v", doc.Empty)
	}
}

func TestTheEncoderWindowIsNoLargerThanTheStockChunks(t *testing.T) {
	t.Parallel()
	root := fixture(t, "win10")
	out := filepath.Join(t.TempDir(), "out")
	sum := mustExtract(t, root, out)
	// The largest chunk is the 1 MiB record's, a single-segment frame: its window is its content size. Of the four stock
	// chunks the encoder wrote, one is single-segment and two state a size (the one of 624 bytes and the large one).
	if sum.StockFrames.MaxWindow != 1<<20 || sum.StockFrames.Chunks != 4 || sum.StockFrames.SingleSegment != 1 ||
		sum.StockFrames.WithContentSize != 2 {
		t.Errorf("stock_frames = %+v", sum.StockFrames)
	}
	if sum.Encoder.Window != 1<<20 || sum.Encoder.Level != defaultLevel.String() || sum.Encoder.Checksum {
		t.Errorf("encoder = %+v", sum.Encoder)
	}
	for _, e := range readManifest(t, out).Labels["game"].Entries {
		frame, err := os.ReadFile(filepath.Join(out, "blobs", "sha256", e.Sha256))
		if err != nil {
			t.Fatal(err)
		}
		var h zstd.Header
		if err := h.Decode(frame); err != nil {
			t.Fatal(err)
		}
		if frameWindow(&h) > sum.StockFrames.MaxWindow {
			t.Errorf("entry %s:%s has window %d, over the stock %d", e.Type, e.Name, frameWindow(&h), sum.StockFrames.MaxWindow)
		}
	}
}

func TestEncoderWindowIsTheLargestPowerOfTwoWithinTheStockWindow(t *testing.T) {
	t.Parallel()
	for _, c := range []struct {
		stock uint64
		want  int
	}{
		{1 << 20, 1 << 20}, {78294084, 1 << 26}, {1<<26 - 1, 1 << 25}, {5, 1 << 10}, {0, 1 << 10}, {1 << 40, 1 << 29},
	} {
		if got := encoderWindow(c.stock); got != c.want {
			t.Errorf("encoderWindow(%d) = %d, want %d", c.stock, got, c.want)
		}
	}
}

func TestAndroidReadsTheAndroidFolder(t *testing.T) {
	t.Parallel()
	root := fixture(t, "android")
	out := filepath.Join(t.TempDir(), "out")
	r := runTool(t, "extract", "-root", root, "-platform", "android-arm64", "-out", out)
	if r.code != 0 {
		t.Fatalf("exit %d: %s", r.code, r.stderr)
	}
	for _, e := range readManifest(t, out).Labels["game"].Entries {
		if e.Platform != "android-arm64" {
			t.Errorf("entry %+v", e)
		}
	}
	if v := runTool(t, "verify", out, root); v.code != 0 {
		t.Errorf("verify exit %d: %s", v.code, v.stderr)
	}
	// The PC folder does not exist in this root.
	if r := runTool(t, extractArgs(root, filepath.Join(t.TempDir(), "x"))...); r.code != 1 {
		t.Errorf("a platform whose folder is absent: exit %d", r.code)
	}
}

func TestASetOptionTakesOnlyThatSet(t *testing.T) {
	t.Parallel()
	root := fixture(t, "win10")
	out := filepath.Join(t.TempDir(), "out")
	sum := mustExtract(t, root, out, "-set", setB)
	if sum.Records != 2 || len(sum.Sets) != 1 || sum.Sets[0].ID != setB {
		t.Errorf("summary = %+v", sum)
	}
	if v := runTool(t, "verify", out, root); v.code != 0 {
		t.Errorf("verify exit %d: %s", v.code, v.stderr)
	}
}

// A data root with more records than the parser's cap of entries: the true entries are written and counted.
func TestWhatTheSetBreaksOfTheParsersRulesIsCountedNotBent(t *testing.T) {
	t.Parallel()
	root := t.TempDir()
	var recs []res
	for i := 0; i < cargoMaxEntries+52; i++ {
		recs = append(recs, res{0x10, int64(i + 1), fill(8, byte(i)), 16})
	}
	recs = append(recs, empty)
	writeSet(t, root, "win10", setA, []chunkSpec{{0, recs}})
	out := filepath.Join(t.TempDir(), "out")
	sum := mustExtract(t, root, out)
	c := sum.CargoRules
	if c.Entries != cargoMaxEntries+52 || c.EntriesOverCap != 52 || c.MaxEntries != 2048 || c.ZeroByteRecords != 1 ||
		c.EntriesWithoutStockSize != c.Entries || c.EntriesOverMaxBlobBytes != 0 || c.ObjectBytesOverCap ||
		c.ManifestBytes < 1 || c.MaxManifestBytes != 1<<20 {
		t.Errorf("cargo_rules = %+v", c)
	}
	if got := len(readManifest(t, out).Labels["game"].Entries); got != cargoMaxEntries+52 {
		t.Errorf("the manifest holds %d entries: the data was bent to fit the cap", got)
	}
	raw, _ := os.ReadFile(filepath.Join(out, "manifest.json"))
	if c.ManifestBytes != len(raw) {
		t.Errorf("manifest_bytes = %d, the file is %d", c.ManifestBytes, len(raw))
	}
	if strings.Contains(string(raw), "stock_size") {
		t.Error("the manifest carries a stock_size the data does not have")
	}
}

func TestCargoRulesOfEntriesOverTheBlobLimit(t *testing.T) {
	t.Parallel()
	entries := []entryDoc{{Size: cargoMaxBlobBytes + 1, Bytes: 1, Align: 16}, {Size: 1, Bytes: cargoMaxBlobBytes + 1, Align: 16},
		{Size: cargoMaxBlobBytes, Bytes: cargoMaxBlobBytes, Align: 16}, {Size: 1, Bytes: 1, Align: 1<<16 + 1}}
	r := cargoRulesOf(entries, 3, 12, cargoDefaultCapBytes+1)
	if r.EntriesOverMaxBlobBytes != 2 || r.EntriesAlignOverCargoMax != 1 || !r.ObjectBytesOverCap || r.ZeroByteRecords != 3 ||
		r.EntriesWithoutStockSize != 4 || r.EntriesOverCap != 0 {
		t.Errorf("cargo rules = %+v", r)
	}
	if r := cargoRulesOf(entries, 0, 1, cargoDefaultCapBytes); r.ObjectBytesOverCap {
		t.Error("a total of exactly the cap is over it")
	}
}

func TestARunIsRecordedInTheToolsLog(t *testing.T) {
	t.Parallel()
	root := fixture(t, "win10")
	out := filepath.Join(t.TempDir(), "out")
	r := runTool(t, extractArgs(root, out)...)
	if r.code != 0 {
		t.Fatalf("exit %d: %s", r.code, r.stderr)
	}
	if v := runTool(t, "verify", out, root); v.code != 0 {
		t.Fatalf("verify exit %d: %s", v.code, v.stderr)
	}
	// Each run has its own environment, and so its own log: read the extract run's.
	raw, err := os.ReadFile(filepath.Join(testkit.Getenv(r.env)("XDG_STATE_HOME"), "nevr-runtime", "cargo-extract.jsonl"))
	if err != nil {
		t.Fatal(err)
	}
	var seen []string
	for _, line := range strings.Split(strings.TrimSpace(string(raw)), "\n") {
		var rec map[string]any
		if err := json.Unmarshal([]byte(line), &rec); err != nil {
			t.Fatalf("not a JSON line: %q", line)
		}
		seen = append(seen, rec["event"].(string))
		if rec["event"] == "written" && (rec["entries"] != float64(7) || rec["objects_written"] != float64(5) || rec["records_with_object"] != float64(7) || rec["objects"] != float64(5)) {
			t.Errorf("written record = %v", rec)
		}
	}
	if got := strings.Join(seen, ","); got != "start,set,set,stock_frames,planned,written" {
		t.Errorf("records = %s", got)
	}
}

func TestUsageErrorsExitTwo(t *testing.T) {
	t.Parallel()
	for _, args := range [][]string{
		nil,
		{"frobnicate"},
		{"extract"},
		{"extract", "-root", "r", "-platform", "ps5", "-out", "o"},
		{"extract", "-root", "r", "-platform", "android-arm64", "-out", "o", "-level", "ludicrous"},
		{"extract", "-root", "r", "-platform", "android-arm64", "-out", "o", "-set", "XYZ"},
		{"extract", "-root", "r", "-platform", "android-arm64", "-out", "o", "stray"},
		{"verify", "only-one"},
	} {
		if r := runTool(t, args...); r.code != 2 {
			t.Errorf("%v: exit %d: %s", args, r.code, r.stderr)
		}
	}
}

// ---- the rules the tool enforces on what it writes: each refuses, names the record, and writes no manifest ----

func quietLog() *slog.Logger { return slog.New(slog.NewJSONHandler(io.Discard, nil)) }

func refusedRun(t *testing.T, root string, encode func([]byte) []byte) (string, error) {
	t.Helper()
	out := filepath.Join(t.TempDir(), "out")
	_, err := extract(context.Background(), extractOptions{root: root, platform: "windows-x86_64", out: out, level: defaultLevel, encode: encode, workers: 2}, quietLog())
	if _, serr := os.Stat(filepath.Join(out, "manifest.json")); err != nil && serr == nil {
		t.Error("a refused run wrote a manifest")
	}
	return out, err
}

func TestAnAlignmentThatIsNotAPowerOfTwoOfAtMost4096IsRefused(t *testing.T) {
	t.Parallel()
	for _, align := range []uint32{0, 24, 8192} {
		root := t.TempDir()
		writeSet(t, root, "win10", setA, []chunkSpec{{0, []res{{0x10, 1, fill(10, 'x'), 16}}}})
		patchAlignment(t, root, setA, align)
		_, err := refusedRun(t, root, nil)
		want := fmt.Sprintf("record 0x0000000000000010:0x0000000000000001: the alignment %d is not a power of two of at most 4096", align)
		if err == nil || err.Error() != want {
			t.Errorf("alignment %d: got %v", align, err)
		}
	}
	for _, align := range []uint32{1, 2, 16, 2048, 4096} {
		root := t.TempDir()
		writeSet(t, root, "win10", setA, []chunkSpec{{0, []res{{0x10, 1, fill(10, 'x'), align}}}})
		if _, err := refusedRun(t, root, nil); err != nil {
			t.Errorf("alignment %d is refused: %v", align, err)
		}
	}
}

// patchAlignment rewrites the alignment of every record of a set in its manifest.
func patchAlignment(t *testing.T, root, id string, align uint32) {
	t.Helper()
	path := filepath.Join(root, "rad15", "win10", "manifests", id)
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	m, err := radpack.DecodeFile(raw)
	if err != nil {
		t.Fatal(err)
	}
	for i := range m.Resources {
		m.Resources[i].Alignment = align
	}
	raw, err = m.EncodeFile()
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, raw, 0o644); err != nil {
		t.Fatal(err)
	}
}

func TestARecordInTwoSetsIsRefused(t *testing.T) {
	t.Parallel()
	root := t.TempDir()
	writeSet(t, root, "win10", setA, []chunkSpec{{0, []res{small1, small2}}})
	writeSet(t, root, "win10", setB, []chunkSpec{{0, []res{small2}}})
	_, err := refusedRun(t, root, nil)
	want := "record 0x0000000000000010:0x0000000000000002: it appears twice for windows-x86_64 (sets 2b47aab238f60515 and 48037dc70b0ecab2)"
	if err == nil || err.Error() != want {
		t.Errorf("got %v", err)
	}
}

func TestARecordNamingAChunkTheManifestDoesNotHaveIsRefused(t *testing.T) {
	t.Parallel()
	root := t.TempDir()
	writeSet(t, root, "win10", setA, []chunkSpec{{0, []res{small1}}})
	path := filepath.Join(root, "rad15", "win10", "manifests", setA)
	raw, _ := os.ReadFile(path)
	m, err := radpack.DecodeFile(raw)
	if err != nil {
		t.Fatal(err)
	}
	m.Resources[0].Chunk = 9
	raw, _ = m.EncodeFile()
	if err := os.WriteFile(path, raw, 0o644); err != nil {
		t.Fatal(err)
	}
	_, err = refusedRun(t, root, nil)
	if err == nil || !strings.Contains(err.Error(), "it names chunk 9 of 2 in set "+setA) {
		t.Errorf("got %v", err)
	}
}

// bigWindowFrame is a valid frame whose header asks for a 1 GiB window (not single-segment: a 4-byte content size and a
// Window_Descriptor of exponent 20), holding data in one raw block.
func bigWindowFrame(data []byte) []byte {
	f := binary.LittleEndian.AppendUint32(nil, zstdMagic)
	f = append(f, 0x80, 0xA0)
	f = binary.LittleEndian.AppendUint32(f, uint32(len(data)))
	h := uint32(len(data))<<3 | 1
	f = append(f, byte(h), byte(h>>8), byte(h>>16))
	return append(f, data...)
}

// skippableFrame is a zstd skippable frame: its magic is not the zstd magic, and a decoder passes over it.
func skippableFrame() []byte {
	f := []byte{0x50, 0x2A, 0x4D, 0x18, 4, 0, 0, 0}
	return append(f, 1, 2, 3, 4)
}

func realEncode(t *testing.T) func([]byte) []byte {
	t.Helper()
	enc, err := zstd.NewWriter(nil, zstd.WithSingleSegment(true)) // the frames the tool writes state their size
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { enc.Close() })
	return func(b []byte) []byte { return enc.EncodeAll(b, nil) }
}

func TestAFrameRuleRefusesAndNamesTheRecord(t *testing.T) {
	t.Parallel()
	for _, c := range []struct {
		name   string
		encode func(real func([]byte) []byte, data []byte) []byte
		want   string
	}{
		{"not a zstd frame", func(_ func([]byte) []byte, d []byte) []byte { return skippableFrame() }, "the frame does not start with the zstd magic"},
		{"a content size that is not the record's", func(real func([]byte) []byte, d []byte) []byte { return real(append(append([]byte(nil), d...), 0)) },
			"the frame header content size is "},
		{"a window larger than the stock chunks'", func(_ func([]byte) []byte, d []byte) []byte { return bigWindowFrame(d) },
			"the frame window is 1073741824 bytes, larger than the stock chunks' largest, "},
	} {
		t.Run(c.name, func(t *testing.T) {
			t.Parallel()
			root := t.TempDir()
			writeSet(t, root, "win10", setA, []chunkSpec{{0, []res{small1}}})
			real := realEncode(t)
			out, err := refusedRun(t, root, func(b []byte) []byte { return c.encode(real, b) })
			if err == nil || !strings.HasPrefix(err.Error(), "record 0x0000000000000010:0x0000000000000001: "+c.want) {
				t.Errorf("got %v", err)
			}
			if objs, _ := os.ReadDir(filepath.Join(out, "blobs", "sha256")); len(objs) != 0 {
				t.Errorf("a refused frame was stored: %v", objs)
			}
		})
	}
}

func TestACorrectFrameMeetsTheFrameRule(t *testing.T) {
	t.Parallel()
	data := fill(100, 'a')
	frame := realEncode(t)(data)
	if err := checkFrame(frame, 100, 1<<20); err != nil {
		t.Errorf("a frame the encoder wrote is refused: %v", err)
	}
	if err := checkFrame(frame, 101, 1<<20); err == nil {
		t.Error("a frame of 100 bytes passed for a record of 101")
	}
	if err := checkFrame(nil, 100, 1<<20); err == nil {
		t.Error("no frame passed")
	}
	if err := checkFrame(frame[:3], 100, 1<<20); err == nil {
		t.Error("a frame cut in the magic passed")
	}
	if err := checkFrame(frame, 100, 99); err == nil {
		t.Error("a single-segment frame of 100 bytes passed with a window limit of 99")
	}
}

// ---- verify names the first difference ----

func TestVerifyNamesTheFirstDifference(t *testing.T) {
	t.Parallel()
	setup := func(t *testing.T) (root, out string, doc manifestDoc) {
		t.Helper()
		root = fixture(t, "win10")
		out = filepath.Join(t.TempDir(), "out")
		mustExtract(t, root, out)
		return root, out, readManifest(t, out)
	}
	writeDoc := func(t *testing.T, out string, doc manifestDoc) {
		t.Helper()
		raw, err := json.Marshal(doc)
		if err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(out, "manifest.json"), raw, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	find := func(doc manifestDoc, typ, name string) int {
		for i, e := range doc.Labels["game"].Entries {
			if e.Type == typ && e.Name == name {
				return i
			}
		}
		t.Fatalf("no entry %s:%s", typ, name)
		return -1
	}
	small2Key := "0x0000000000000010:0x0000000000000002"
	cases := []struct {
		name   string
		mutate func(t *testing.T, root, out string, doc manifestDoc)
		want   string
	}{
		{"an object that does not hash to its name", func(t *testing.T, _, out string, doc manifestDoc) {
			path := filepath.Join(out, "blobs", "sha256", doc.Labels["game"].Entries[find(doc, "0x0000000000000010", "0x0000000000000002")].Sha256)
			b, _ := os.ReadFile(path)
			b[len(b)-1] ^= 1
			os.WriteFile(path, b, 0o644)
		}, "holds bytes that hash to"},
		{"an object that is missing", func(t *testing.T, _, out string, doc manifestDoc) {
			os.Remove(filepath.Join(out, "blobs", "sha256", doc.Labels["game"].Entries[find(doc, "0x0000000000000010", "0x0000000000000002")].Sha256))
		}, "record " + small2Key + ": open "},
		{"an entry that points at another record's object", func(t *testing.T, _, out string, doc manifestDoc) {
			a, b := find(doc, "0x0000000000000010", "0x0000000000000002"), find(doc, "0x0000000000000010", "0x0000000000000003")
			es := doc.Labels["game"].Entries
			es[a].Sha256, es[a].Size = es[b].Sha256, es[b].Size
			writeDoc(t, out, doc)
		}, "decompresses to 300 bytes that differ from the 200 bytes the stock package holds"},
		{"an entry whose size is not the object's", func(t *testing.T, _, out string, doc manifestDoc) {
			doc.Labels["game"].Entries[find(doc, "0x0000000000000010", "0x0000000000000002")].Size++
			writeDoc(t, out, doc)
		}, "bytes, the entry says "},
		{"an entry whose bytes are not the record's", func(t *testing.T, _, out string, doc manifestDoc) {
			doc.Labels["game"].Entries[find(doc, "0x0000000000000010", "0x0000000000000002")].Bytes++
			writeDoc(t, out, doc)
		}, "record " + small2Key + ": the entry says 201 bytes"},
		{"an entry the stock sets do not hold", func(t *testing.T, _, out string, doc manifestDoc) {
			es := doc.Labels["game"].Entries
			es[0].Name = "0x00000000000000ff"
			writeDoc(t, out, doc)
		}, "the manifest lists it and the stock sets do not hold it"},
		{"a stock record the manifest leaves out", func(t *testing.T, _, out string, doc manifestDoc) {
			l := doc.Labels["game"]
			l.Entries = l.Entries[1:]
			doc.Labels["game"] = l
			writeDoc(t, out, doc)
		}, "it and the manifest does not list it"},
		{"a record listed twice", func(t *testing.T, _, out string, doc manifestDoc) {
			l := doc.Labels["game"]
			l.Entries = append(l.Entries, l.Entries[0])
			doc.Labels["game"] = l
			writeDoc(t, out, doc)
		}, "the manifest lists it twice"},
		{"a record of bytes listed as empty", func(t *testing.T, _, out string, doc manifestDoc) {
			doc.Empty = append(doc.Empty, emptyDoc{Type: "0x0000000000000040", Name: "0x0000000000000001", Align: 4})
			l := doc.Labels["game"]
			l.Entries = append(l.Entries[:find(doc, "0x0000000000000040", "0x0000000000000001")], l.Entries[find(doc, "0x0000000000000040", "0x0000000000000001")+1:]...)
			doc.Labels["game"] = l
			writeDoc(t, out, doc)
		}, "it is listed as empty with alignment 4, the stock record is 50 bytes"},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			t.Parallel()
			root, out, doc := setup(t)
			c.mutate(t, root, out, doc)
			r := runTool(t, "verify", out, root)
			if r.code != 1 || !strings.Contains(r.stderr, c.want) {
				t.Errorf("exit %d, want 1 and %q: %s", r.code, c.want, r.stderr)
			}
		})
	}
}

func TestVerifyComparesTheDecompressedObjectWithTheStockBytes(t *testing.T) {
	t.Parallel()
	root := fixture(t, "win10")
	out := filepath.Join(t.TempDir(), "out")
	mustExtract(t, root, out)
	doc := readManifest(t, out)
	// Replace one object by a valid frame of other bytes of the same length, under the name its own hash gives, and point
	// the entry at it: the frame, its hash and its sizes all agree, and only the bytes differ from the stock ones.
	es := doc.Labels["game"].Entries
	var i int
	for j, e := range es {
		if e.Type == "0x0000000000000010" && e.Name == "0x0000000000000002" {
			i = j
		}
	}
	forged := realEncode(t)(fill(200, 'X'))
	name := sha(forged)
	if err := os.WriteFile(filepath.Join(out, "blobs", "sha256", name), forged, 0o644); err != nil {
		t.Fatal(err)
	}
	es[i].Sha256, es[i].Size = name, uint64(len(forged))
	raw, _ := json.Marshal(doc)
	if err := os.WriteFile(filepath.Join(out, "manifest.json"), raw, 0o644); err != nil {
		t.Fatal(err)
	}
	r := runTool(t, "verify", out, root)
	want := "record 0x0000000000000010:0x0000000000000002: the object " + name + " decompresses to 200 bytes that differ from the 200 bytes the stock package holds"
	if r.code != 1 || !strings.Contains(r.stderr, want) {
		t.Errorf("exit %d: %s", r.code, r.stderr)
	}
}

// Each source manifest is named in the manifest and in the summary by its set id, the SHA-256 of the manifest file and its
// length in bytes.
func TestTheManifestAndTheSummaryNameEachSourceManifestByIdHashAndLength(t *testing.T) {
	t.Parallel()
	root := fixture(t, "win10")
	out := filepath.Join(t.TempDir(), "out")
	r := runTool(t, extractArgs(root, out)...)
	if r.code != 0 {
		t.Fatalf("exit %d: %s", r.code, r.stderr)
	}
	want := []map[string]any{}
	for _, id := range []string{setB, setA} { // the sets in id order
		raw, err := os.ReadFile(filepath.Join(root, "rad15", "win10", "manifests", id))
		if err != nil {
			t.Fatal(err)
		}
		want = append(want, map[string]any{"id": id, "sha256": sha(raw), "bytes": float64(len(raw))})
	}
	manifest, err := os.ReadFile(filepath.Join(out, "manifest.json"))
	if err != nil {
		t.Fatal(err)
	}
	for name, text := range map[string][]byte{"manifest.json": manifest, "summary": []byte(r.stdout)} {
		var doc struct {
			Sets []map[string]any `json:"sets"`
		}
		if err := json.Unmarshal(text, &doc); err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		if fmt.Sprint(doc.Sets) != fmt.Sprint(want) {
			t.Errorf("%s: sets = %v, want %v", name, doc.Sets, want)
		}
	}
}

// `objects` is the number of unique objects in the output directory, counted there: a run over objects that are already
// present reports the same number as the run that wrote them, and a file that is not an object (a temporary file, any other
// name) is not counted.
func TestObjectsIsCountedFromTheOutputDirectory(t *testing.T) {
	t.Parallel()
	root := fixture(t, "win10")
	out := filepath.Join(t.TempDir(), "out")
	objects := func(r result) any {
		t.Helper()
		if r.code != 0 {
			t.Fatalf("exit %d: %s", r.code, r.stderr)
		}
		var sum map[string]any
		if err := json.Unmarshal([]byte(r.stdout), &sum); err != nil {
			t.Fatal(err)
		}
		return sum["objects"]
	}
	first := runTool(t, extractArgs(root, out)...)
	if got := objects(first); got != float64(5) {
		t.Errorf("objects = %v after the run that wrote them, want 5", got)
	}
	blobs := filepath.Join(out, "blobs", "sha256")
	for _, name := range []string{".tmp-leftover", "README", strings.Repeat("A", 64), strings.Repeat("a", 63)} {
		if err := os.WriteFile(filepath.Join(blobs, name), []byte("x"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.Mkdir(filepath.Join(blobs, strings.Repeat("b", 64)), 0o755); err != nil {
		t.Fatal(err)
	}
	again := runTool(t, extractArgs(root, out)...)
	if got := objects(again); got != float64(5) {
		t.Errorf("objects = %v after a run that wrote none, want 5", got)
	}
	var sum summary
	if err := json.Unmarshal([]byte(again.stdout), &sum); err != nil || sum.ObjectsWritten != 0 {
		t.Errorf("the second run wrote %d objects (%v)", sum.ObjectsWritten, err)
	}
}

// `records_with_object` is the records that point at an object (the entries, whatever this run wrote), `objects` the unique
// objects they point at, and `objects_deduplicated` the records whose object this run did not write (records_with_object
// less objects_written): a count of this run, so a re-run over the same data reports more of them than the first run.
func TestTheSummaryCountsRecordsWithAnObjectAndTheUniqueObjects(t *testing.T) {
	t.Parallel()
	root := fixture(t, "win10")
	out := filepath.Join(t.TempDir(), "out")
	for _, run := range []struct {
		name         string
		deduplicated float64
	}{{"the run that wrote the objects", 2}, {"a run over objects already present", 7}} {
		r := runTool(t, extractArgs(root, out)...)
		if r.code != 0 {
			t.Fatalf("exit %d: %s", r.code, r.stderr)
		}
		var sum map[string]any
		if err := json.Unmarshal([]byte(r.stdout), &sum); err != nil {
			t.Fatal(err)
		}
		if sum["records_with_object"] != float64(7) || sum["objects"] != float64(5) {
			t.Errorf("%s: records_with_object = %v, objects = %v, want 7 and 5", run.name, sum["records_with_object"], sum["objects"])
		}
		if sum["objects_deduplicated"] != run.deduplicated {
			t.Errorf("%s: objects_deduplicated = %v, want %v", run.name, sum["objects_deduplicated"], run.deduplicated)
		}
		if _, ok := sum["bytes_saved_by_dedup"]; ok {
			t.Errorf("%s: the summary has a bytes_saved_by_dedup the tool does not know", run.name)
		}
	}
}

// manifest.json.zst is one zstd frame of the manifest's bytes, written with the objects' encoder settings: the zstd magic,
// the content size in the header, a window within the stock chunks' largest.
func TestTheManifestIsAlsoWrittenAsOneZstdFrameThatStatesItsSize(t *testing.T) {
	t.Parallel()
	small := fixture(t, "win10") // a manifest that fits the window: a single-segment frame
	many := t.TempDir()          // a manifest larger than the window: a frame with a window descriptor
	var recs []res
	for i := 0; i < 2100; i++ {
		recs = append(recs, res{0x10, int64(i + 1), fill(8, byte(i)), 16})
	}
	writeSet(t, many, "win10", setA, []chunkSpec{{0, recs}})
	for name, root := range map[string]string{"a manifest within the window": small, "a manifest over the window": many} {
		out := filepath.Join(t.TempDir(), "out")
		sum := mustExtract(t, root, out)
		plain, err := os.ReadFile(filepath.Join(out, "manifest.json"))
		if err != nil {
			t.Fatal(err)
		}
		frame, err := os.ReadFile(filepath.Join(out, "manifest.json.zst"))
		if err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		if binary.LittleEndian.Uint32(frame) != zstdMagic {
			t.Errorf("%s: the frame does not start with the zstd magic", name)
		}
		var h zstd.Header
		if err := h.Decode(frame); err != nil || !h.HasFCS || h.FrameContentSize != uint64(len(plain)) || h.HasCheckSum {
			t.Errorf("%s: header %+v (%v) does not state %d bytes without a checksum", name, h, err, len(plain))
		}
		if w := frameWindow(&h); w > sum.StockFrames.MaxWindow {
			t.Errorf("%s: the frame's window %d is over the stock %d", name, w, sum.StockFrames.MaxWindow)
		}
		dec, err := zstd.NewReader(nil)
		if err != nil {
			t.Fatal(err)
		}
		got, err := dec.DecodeAll(frame, nil)
		dec.Close()
		if err != nil || !bytes.Equal(got, plain) {
			t.Errorf("%s: the frame does not decompress to the manifest's bytes (%v)", name, err)
		}
		if (name == "a manifest within the window") != h.SingleSegment {
			t.Errorf("%s: single-segment = %t", name, h.SingleSegment)
		}
	}
}

func TestTheManifestFrameMeetsTheFrameRuleToo(t *testing.T) {
	t.Parallel()
	root := t.TempDir()
	var recs []res
	for i := 0; i < 5; i++ {
		recs = append(recs, res{0x10, int64(i + 1), fill(100, byte('a'+i)), 16})
	}
	writeSet(t, root, "win10", setA, []chunkSpec{{0, recs}})
	real := realEncode(t)
	// The records are framed as usual; the manifest (over 500 bytes) comes back as a frame with a 1 GiB window.
	out, err := refusedRun(t, root, func(b []byte) []byte {
		if len(b) > 500 {
			return bigWindowFrame(b)
		}
		return real(b)
	})
	if err == nil || !strings.HasPrefix(err.Error(), "manifest.json.zst: the frame window is 1073741824 bytes") {
		t.Errorf("got %v", err)
	}
	for _, name := range []string{"manifest.json", "manifest.json.zst"} {
		if _, err := os.Stat(filepath.Join(out, name)); err == nil {
			t.Errorf("a refused manifest frame left %s", name)
		}
	}
}
