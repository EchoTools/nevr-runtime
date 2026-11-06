package main

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/testkit"
)

// The fixtures are written by hand with the standard library: a zstd frame of raw blocks (RFC 8878, section 3.1.1)
// is a frame the decoder reads like any other, so a test needs no encoder to make a manifest or a package.

const testSet = "48037dc70b0ecab2"

// rawFrame is a zstd frame holding plain in raw blocks: the magic, a frame header with Single_Segment_Flag and a
// 4-byte Frame_Content_Size (0xA0), then blocks of at most 128 KiB, the last one marked.
func rawFrame(plain []byte) []byte {
	f := binary.LittleEndian.AppendUint32(nil, zstdMagic)
	f = append(f, 0xA0)
	f = binary.LittleEndian.AppendUint32(f, uint32(len(plain)))
	for at := 0; ; {
		n := min(len(plain)-at, 128<<10)
		h := uint32(n) << 3
		if at+n == len(plain) {
			h |= 1
		}
		f = append(f, byte(h), byte(h>>8), byte(h>>16))
		f = append(f, plain[at:at+n]...)
		at += n
		if h&1 == 1 {
			return f
		}
	}
}

// manifestFile is a package manifest as the game reads it (manifest.go): the container and one frame of the body.
func manifestFile(packages uint32, recs []record, chunks []chunk) []byte {
	body := make([]byte, bodyArrays)
	binary.LittleEndian.PutUint32(body[0:], packages)
	binary.LittleEndian.PutUint32(body[4:], blockLimit)
	binary.LittleEndian.PutUint64(body[0x38:], uint64(len(recs)))
	binary.LittleEndian.PutUint64(body[0xb8:], uint64(len(chunks)))
	for _, r := range recs {
		body = binary.LittleEndian.AppendUint64(body, r.Key.Type)
		body = binary.LittleEndian.AppendUint64(body, r.Key.Name)
		body = binary.LittleEndian.AppendUint32(body, r.Chunk)
		body = binary.LittleEndian.AppendUint32(body, r.OffsetInChunk)
		body = binary.LittleEndian.AppendUint32(body, r.Size)
		body = binary.LittleEndian.AppendUint32(body, r.Alignment)
	}
	for _, c := range chunks {
		body = binary.LittleEndian.AppendUint32(body, c.Package)
		body = binary.LittleEndian.AppendUint32(body, c.FileOffset)
		body = binary.LittleEndian.AppendUint32(body, c.Compressed)
		body = binary.LittleEndian.AppendUint32(body, c.Decompressed)
	}
	frame := rawFrame(body)
	f := []byte("ZSTD")
	f = binary.LittleEndian.AppendUint32(f, 0x10)
	f = binary.LittleEndian.AppendUint64(f, uint64(len(body)))
	f = binary.LittleEndian.AppendUint64(f, uint64(len(frame)))
	return append(f, frame...)
}

type item struct {
	k    key
	data []byte
}

func fill(n int, b byte) []byte { return bytes.Repeat([]byte{b}, n) }

// archiveBytes is an archive's body (docs/engine/asset-system.md, "Archives"): its dependencies, its entries, an empty
// type table and a closing 0.
func archiveBytes(deps []uint64, entries []key) []byte {
	b := binary.LittleEndian.AppendUint32(nil, uint32(len(deps)))
	for _, d := range deps {
		b = binary.LittleEndian.AppendUint64(b, d)
	}
	b = binary.LittleEndian.AppendUint32(b, uint32(len(entries)))
	for _, e := range entries {
		b = binary.LittleEndian.AppendUint64(b, e.Type)
		b = binary.LittleEndian.AppendUint64(b, e.Name)
	}
	b = binary.LittleEndian.AppendUint32(b, 0)
	return binary.LittleEndian.AppendUint32(b, 0)
}

var (
	stockA    = item{key{0x1111, 0xa}, fill(100, 'a')}
	stockB    = item{key{0x1111, 0xb}, fill(50, 'b')}
	levelRoot = item{key{levelRootType, 0x100}, fill(64, 'L')}
	// The level's archive: it depends on itself (a cycle the walk ends) and lists its root. 40 bytes.
	archive = item{key{archiveType, 0x100}, archiveBytes([]uint64{0x100}, []key{{levelRootType, 0x100}})}
	texture = item{key{0x2222, 0x300}, fill(200, 'T')}
)

// world is a stock manifest of one chunk (stockA, stockB) in package 0, and an export that starts as the same manifest
// with package 0 copied and an empty package 1, the package new chunks go to.
type world struct {
	t       *testing.T
	dir     string
	stock   []record
	chunks  []chunk // the export's chunks; chunk 0 is the stock chunk
	records []record
	pkg0    []byte
	pkg1    []byte
}

func newWorld(t *testing.T) *world {
	t.Helper()
	w := &world{t: t, dir: t.TempDir()}
	plain, recs := lay(0, 16, stockA, stockB)
	frame := rawFrame(plain)
	w.pkg0 = frame
	w.chunks = []chunk{{Package: 0, FileOffset: 0, Compressed: uint32(len(frame)), Decompressed: uint32(len(plain))}}
	w.stock = recs
	w.records = append([]record(nil), recs...)
	return w
}

// lay places items in one chunk's bytes at align and returns the bytes and the records, which name chunk ci.
func lay(ci uint32, align uint32, items ...item) ([]byte, []record) {
	var plain []byte
	var recs []record
	for _, it := range items {
		for uint32(len(plain))%align != 0 {
			plain = append(plain, 0)
		}
		recs = append(recs, record{Key: it.k, Chunk: ci, OffsetInChunk: uint32(len(plain)), Size: uint32(len(it.data)), Alignment: align})
		plain = append(plain, it.data...)
	}
	return plain, recs
}

// addChunk puts items into a new chunk of package 1. An item whose key the export already has is moved there.
func (w *world) addChunk(items ...item) {
	ci := uint32(len(w.chunks))
	plain, recs := lay(ci, 16, items...)
	frame := rawFrame(plain)
	w.chunks = append(w.chunks, chunk{Package: 1, FileOffset: uint32(len(w.pkg1)), Compressed: uint32(len(frame)), Decompressed: uint32(len(plain))})
	w.pkg1 = append(w.pkg1, frame...)
	for _, r := range recs {
		moved := false
		for i := range w.records {
			if w.records[i].Key == r.Key {
				w.records[i], moved = r, true
			}
		}
		if !moved {
			w.records = append(w.records, r)
		}
	}
}

func (w *world) write(name string, b []byte) string {
	w.t.Helper()
	path := filepath.Join(w.dir, name)
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		w.t.Fatal(err)
	}
	if err := os.WriteFile(path, b, 0o644); err != nil {
		w.t.Fatal(err)
	}
	return path
}

type result struct {
	code           int
	stdout, stderr string
	rep            report
	set            []byte
	env            []string
}

// run writes the world's files and runs levelpack on them with extra arguments after the usual ones.
func (w *world) run(extra ...string) result {
	w.t.Helper()
	stockManifest := w.write("stock/"+testSet, manifestFile(1, w.stock, w.chunks[:1]))
	w.write("stock/"+testSet+"_0", w.pkg0)
	exportManifest := w.write("export/"+testSet, manifestFile(2, w.records, w.chunks))
	w.write("export/"+testSet+"_0", w.pkg0)
	w.write("export/"+testSet+"_1", w.pkg1)
	out := filepath.Join(w.dir, "out", "levels.nevrres")
	if err := os.MkdirAll(filepath.Dir(out), 0o755); err != nil {
		w.t.Fatal(err)
	}
	args := append([]string{"-manifest", exportManifest, "-packages", filepath.Join(w.dir, "export"),
		"-stock", stockManifest, "-stock-packages", filepath.Join(w.dir, "stock"), "-out", out}, extra...)
	env := testkit.Env(w.t)
	var stdout, stderr bytes.Buffer
	r := result{env: env}
	r.code = run(w.t.Context(), args, testkit.Getenv(env), strings.NewReader(""), &stdout, &stderr)
	r.stdout, r.stderr = stdout.String(), stderr.String()
	if r.code == 0 {
		if err := json.Unmarshal(stdout.Bytes(), &r.rep); err != nil {
			w.t.Fatalf("stdout is not the report: %v: %q", err, r.stdout)
		}
		set, err := os.ReadFile(out)
		if err != nil {
			w.t.Fatal(err)
		}
		r.set = set
		js, err := os.ReadFile(out + ".json")
		if err != nil {
			w.t.Fatalf("no report file beside the set: %v", err)
		}
		var onDisk report
		if err := json.Unmarshal(js, &onDisk); err != nil || onDisk.Sha256 != r.rep.Sha256 {
			w.t.Fatalf("the report file is not the report on stdout: %v", err)
		}
	} else if _, err := os.Stat(out); err == nil {
		w.t.Errorf("a refused run left %s", out)
	} else if _, err := os.Stat(out + ".partial"); err == nil {
		w.t.Errorf("a refused run left %s.partial", out)
	}
	return r
}

// setRecords reads a set's record table.
func setRecords(t *testing.T, set []byte) map[key]setRecord {
	t.Helper()
	n := binary.LittleEndian.Uint32(set[0x0c:])
	at := binary.LittleEndian.Uint64(set[0x18:])
	out := map[key]setRecord{}
	for i := uint64(0); i < uint64(n); i++ {
		p := set[at+i*setRecordBytes:]
		k := key{binary.LittleEndian.Uint64(p[0:]), binary.LittleEndian.Uint64(p[8:])}
		out[k] = setRecord{Key: k, Chunk: binary.LittleEndian.Uint32(p[0x10:]), OffsetInChunk: binary.LittleEndian.Uint32(p[0x14:]),
			Size: binary.LittleEndian.Uint32(p[0x18:]), Alignment: binary.LittleEndian.Uint32(p[0x1c:])}
	}
	return out
}

// setFrame returns chunk c's frame and decompressed length from a set.
func setFrame(t *testing.T, set []byte, c uint32) ([]byte, uint32) {
	t.Helper()
	p := set[binary.LittleEndian.Uint64(set[0x20:])+uint64(c)*setChunkBytes:]
	off, n := binary.LittleEndian.Uint32(p[4:]), binary.LittleEndian.Uint32(p[8:])
	return set[off : off+n], binary.LittleEndian.Uint32(p[12:])
}

func TestALevelTheGameDoesNotHaveGoesIntoTheSetFrameForFrame(t *testing.T) {
	t.Parallel()
	w := newWorld(t)
	w.addChunk(levelRoot, archive, texture)
	r := w.run()
	if r.code != 0 {
		t.Fatalf("exit %d: %s", r.code, r.stderr)
	}
	if r.rep.Records != 3 || r.rep.Chunks != 1 || r.rep.ChunksCopied != 1 || r.rep.ChunksRepacked != 0 ||
		r.rep.StockUnchanged != 2 || r.rep.StockCopiesDropped != 0 || len(r.rep.StockEdited) != 0 || r.rep.Overrides != 0 ||
		r.rep.FramesDecoded != 1 || r.rep.RecordsCompared != 0 {
		t.Errorf("report = %+v", r.rep)
	}
	if len(r.rep.Levels) != 1 || r.rep.Levels[0] != "0x0000000000000100" {
		t.Errorf("levels = %v", r.rep.Levels)
	}
	if uint64(len(r.set)) != r.rep.Bytes {
		t.Errorf("the set is %d bytes, the report says %d", len(r.set), r.rep.Bytes)
	}
	recs := setRecords(t, r.set)
	if _, ok := recs[stockA.k]; ok {
		t.Error("a stock record the export did not change is in the set")
	}
	frame, _ := setFrame(t, r.set, 0)
	if !bytes.Equal(frame, w.pkg1) {
		t.Error("the set's frame is not the export's frame, byte for byte")
	}
	want := map[key]uint32{levelRoot.k: 0, archive.k: 64, texture.k: 112}
	for k, off := range want {
		if got := recs[k]; got.Chunk != 0 || got.OffsetInChunk != off || got.Alignment != 16 {
			t.Errorf("record %s = %+v, want chunk 0 offset %d", k, got, off)
		}
	}
}

func TestAStockRecordMovedButNotChangedIsDropped(t *testing.T) {
	t.Parallel()
	w := newWorld(t)
	w.addChunk(stockA)
	w.addChunk(levelRoot, archive)
	r := w.run()
	if r.code != 0 {
		t.Fatalf("exit %d: %s", r.code, r.stderr)
	}
	if r.rep.StockCopiesDropped != 1 || r.rep.StockUnchanged != 1 || r.rep.Records != 2 || len(r.rep.StockEdited) != 0 {
		t.Errorf("report = %+v", r.rep)
	}
	if _, ok := setRecords(t, r.set)[stockA.k]; ok {
		t.Error("the moved copy of a stock record is in the set")
	}
}

func TestAnEditedStockRecordIsListedAndLeftOut(t *testing.T) {
	t.Parallel()
	w := newWorld(t)
	w.addChunk(item{stockB.k, fill(70, 'B')})
	w.addChunk(levelRoot, archive)
	r := w.run()
	if r.code != 0 {
		t.Fatalf("exit %d: %s", r.code, r.stderr)
	}
	want := edited{Key: stockB.k.String(), StockSize: 50, Size: 70, Included: false}
	if len(r.rep.StockEdited) != 1 || r.rep.StockEdited[0] != want || r.rep.Overrides != 0 {
		t.Errorf("stock_edited = %+v, overrides = %d", r.rep.StockEdited, r.rep.Overrides)
	}
	if _, ok := setRecords(t, r.set)[stockB.k]; ok {
		t.Error("an edited stock record is in the set as a record")
	}
}

func TestAnOverrideCarriesTheEditedBytes(t *testing.T) {
	t.Parallel()
	w := newWorld(t)
	edit := fill(70, 'B')
	w.addChunk(item{stockB.k, edit})
	w.addChunk(levelRoot, archive)
	r := w.run("-override", stockB.k.String())
	if r.code != 0 {
		t.Fatalf("exit %d: %s", r.code, r.stderr)
	}
	if r.rep.Overrides != 1 || len(r.rep.StockEdited) != 1 || !r.rep.StockEdited[0].Included || r.rep.RecordsCompared != 1 {
		t.Errorf("report = %+v", r.rep)
	}
	ot := binary.LittleEndian.Uint64(r.set[0x28:])
	if k := (key{binary.LittleEndian.Uint64(r.set[ot:]), binary.LittleEndian.Uint64(r.set[ot+8:])}); k != stockB.k {
		t.Fatalf("override key = %s", k)
	}
	off, n := binary.LittleEndian.Uint32(r.set[ot+0x10:]), binary.LittleEndian.Uint32(r.set[ot+0x14:])
	if !bytes.Equal(r.set[off:off+n], edit) {
		t.Error("the override does not hold the export's bytes")
	}
}

func TestAnOverrideTheExportDidNotEditIsRefused(t *testing.T) {
	t.Parallel()
	w := newWorld(t)
	w.addChunk(levelRoot, archive)
	r := w.run("-override", stockA.k.String())
	if r.code != 1 || !strings.Contains(r.stderr, "-override "+stockA.k.String()+" is not a stock record the export changed") {
		t.Errorf("exit %d: %s", r.code, r.stderr)
	}
}

func TestALevelWithoutItsArchiveIsRefused(t *testing.T) {
	t.Parallel()
	w := newWorld(t)
	w.addChunk(levelRoot, texture)
	r := w.run()
	if r.code != 1 || !strings.Contains(r.stderr, "levels without their archive in the export: 0x0000000000000100") {
		t.Errorf("exit %d: %s", r.code, r.stderr)
	}
}

func TestAnExportWithNothingNewIsRefused(t *testing.T) {
	t.Parallel()
	w := newWorld(t)
	w.addChunk(stockA)
	r := w.run()
	if r.code != 1 || !strings.Contains(r.stderr, "there is nothing to carry") {
		t.Errorf("exit %d: %s", r.code, r.stderr)
	}
}

func TestNewRecordsInAChunkWithAStockRecordAreRepacked(t *testing.T) {
	t.Parallel()
	w := newWorld(t)
	w.addChunk(stockA, levelRoot, archive, texture)
	r := w.run()
	if r.code != 0 {
		t.Fatalf("exit %d: %s", r.code, r.stderr)
	}
	if r.rep.ChunksRepacked != 1 || r.rep.ChunksCopied != 0 || r.rep.Records != 3 || r.rep.Chunks != 1 ||
		r.rep.StockCopiesDropped != 1 || r.rep.FramesDecoded != 1 || r.rep.RecordsCompared != 3 {
		t.Errorf("report = %+v", r.rep)
	}
	recs := setRecords(t, r.set)
	if recs[levelRoot.k].OffsetInChunk != 0 || recs[archive.k].OffsetInChunk != 64 || recs[texture.k].OffsetInChunk != 112 {
		t.Errorf("records = %+v (the texture goes at 112, the next 16-byte boundary after 104)", recs)
	}
	if _, plain := setFrame(t, r.set, 0); plain != 312 {
		t.Errorf("the repacked chunk holds %d bytes, want 312 (the stock record's bytes left out)", plain)
	}
}

func TestARepackKeepsEachChunkInsideTheReadersBlockAndALargeRecordAlone(t *testing.T) {
	t.Parallel()
	w := newWorld(t)
	small := []item{
		{key{0x2222, 1}, fill(0x30000, 1)}, {key{0x2222, 2}, fill(0x30000, 2)},
		{key{0x2222, 3}, fill(0x30000, 3)}, {key{0x2222, 4}, fill(0x10, 4)},
	}
	large := item{key{0x2222, 5}, fill(blockLimit+0x100, 5)}
	w.addChunk(append(small, large)...) // a chunk past the block size: it breaks the reader's rules as it is
	w.addChunk(levelRoot, archive)
	r := w.run()
	if r.code != 0 {
		t.Fatalf("exit %d: %s", r.code, r.stderr)
	}
	if r.rep.ChunksRepacked != 1 || r.rep.ChunksCopied != 1 || r.rep.FramesDecoded != 4 || r.rep.RecordsCompared != 5 {
		t.Errorf("report = %+v", r.rep)
	}
	recs := setRecords(t, r.set)
	l := recs[large.k]
	if _, plain := setFrame(t, r.set, l.Chunk); l.OffsetInChunk != 0 || plain != l.Size {
		t.Errorf("the large record shares its chunk: %+v in a chunk of %d bytes", l, plain)
	}
	for _, it := range small {
		if _, plain := setFrame(t, r.set, recs[it.k].Chunk); plain > blockLimit {
			t.Errorf("record %s is in a chunk of %d bytes", it.k, plain)
		}
	}
	if recs[small[0].k].Chunk != recs[small[1].k].Chunk || recs[small[1].k].Chunk == recs[small[2].k].Chunk {
		t.Errorf("small records are not packed together up to the block size: %+v", recs)
	}
}

func TestALargeChunkOfSmallRecordsIsRepacked(t *testing.T) {
	t.Parallel()
	w := newWorld(t)
	small := []item{{key{0x2222, 1}, fill(0x30000, 1)}, {key{0x2222, 2}, fill(0x30000, 2)}, {key{0x2222, 3}, fill(0x30000, 3)}}
	w.addChunk(small...) // 0x90000 bytes: the reader's block cache holds 0x80000
	r := w.run()
	if r.code != 0 {
		t.Fatalf("exit %d: %s", r.code, r.stderr)
	}
	if r.rep.ChunksRepacked != 1 || r.rep.ChunksCopied != 0 || r.rep.Chunks != 2 {
		t.Errorf("report = %+v", r.rep)
	}
}

func TestASetTheRuntimeWouldRefuseIsNotKept(t *testing.T) {
	t.Parallel()
	w := newWorld(t)
	w.addChunk(levelRoot, archive)
	w.records[len(w.records)-1].Size = 41 // the copied chunk holds 40 of the archive's bytes
	r := w.run()
	if r.code != 1 || !strings.Contains(r.stderr, "the written set does not check: record_range at index 1") {
		t.Errorf("exit %d: %s", r.code, r.stderr)
	}
}

func TestACopiedFrameThatDoesNotHoldItsStatedLengthIsRefused(t *testing.T) {
	t.Parallel()
	w := newWorld(t)
	w.addChunk(levelRoot, archive)
	w.chunks[1].Decompressed++ // the frame holds 104 bytes; the export's chunk entry says 105
	r := w.run()
	if r.code != 1 || !strings.Contains(r.stderr, "the written set does not check: chunk 0 decodes to 104 bytes, its entry says 105") {
		t.Errorf("exit %d: %s", r.code, r.stderr)
	}
}

func TestRecordsOfZeroBytesSurviveARepack(t *testing.T) {
	t.Parallel()
	w := newWorld(t)
	empty := []item{{key{0x3333, 1}, nil}, {key{0x3333, 2}, nil}}
	w.addChunk(append([]item{stockA}, empty...)...)
	r := w.run()
	if r.code != 0 {
		t.Fatalf("exit %d: %s", r.code, r.stderr)
	}
	recs := setRecords(t, r.set)
	for _, it := range empty {
		if got, ok := recs[it.k]; !ok || got.Size != 0 {
			t.Errorf("record %s = %+v, %v", it.k, got, ok)
		}
	}
}

func TestARecordPastTheEndOfItsChunkIsRefused(t *testing.T) {
	t.Parallel()
	w := newWorld(t)
	w.addChunk(stockA, levelRoot, archive)
	for i := range w.records {
		if w.records[i].Key == archive.k {
			w.records[i].Size = 41 // the chunk holds 40 of its bytes
		}
	}
	r := w.run()
	if r.code != 1 || !strings.Contains(r.stderr, "record "+archive.k.String()+" runs past its chunk (41 bytes at 176 of 216)") {
		t.Errorf("exit %d: %s", r.code, r.stderr)
	}
}

func TestAManifestTheGameCouldNotReadIsRefused(t *testing.T) {
	t.Parallel()
	good := manifestFile(1, []record{{Key: key{1, 1}, Size: 1, Alignment: 1}}, []chunk{{Decompressed: 1}})
	if _, err := parseManifest(good); err != nil {
		t.Fatalf("a good manifest is refused: %v", err)
	}
	set := func(b []byte, off int, v uint32) []byte {
		c := append([]byte(nil), b...)
		binary.LittleEndian.PutUint32(c[off:], v)
		return c
	}
	two := manifestFile(1, []record{{Key: key{1, 1}, Alignment: 1}, {Key: key{1, 1}, Alignment: 1}}, []chunk{{Decompressed: 1}})
	far := manifestFile(1, []record{{Key: key{1, 1}, Chunk: 1, Alignment: 1}}, []chunk{{Decompressed: 1}})
	// The body's record count is at 0x38; the body starts after the container (24 bytes) and the frame header (9).
	counts := set(good, containerBytes+9+3+0x38, 2)
	for _, c := range []struct {
		file []byte
		want string
	}{
		{good[:20], "not a ZSTD manifest container"},
		{set(good, 4, 0x11), "unsupported manifest container version"},
		{set(good, 16, uint32(len(good))), "the compressed length runs past the end of the file"},
		{set(good, 8, 0x10), "decompressed length 16 is out of range"},
		{set(good, 8, uint32(len(good))), "the container says"},
		{counts, "do not account for the"},
		{far, "names chunk 1 of 1"},
		{two, "appears twice"},
	} {
		if _, err := parseManifest(c.file); err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%q: got %v", c.want, err)
		}
	}
}

func TestASetTheRuntimesRegistryWouldRefuseIsNotKeptAndItsProblemsAreListed(t *testing.T) {
	t.Parallel()
	for _, c := range []struct {
		name  string
		extra []item
		want  string
	}{
		{"script", []item{{key{scriptType, 1}, fill(16, 'S')}}, "script record 0x991421b41de58370:0x0000000000000001"},
		{"texture", []item{{key{textureType, 2}, fill(256, 'D')}}, "texture 0x4a4c32c49300b8a0:0x0000000000000002 without its payload record"},
		{"unresolved", []item{{key{archiveType, 0x100}, archiveBytes(nil, []key{{0x2222, 0x999}})}},
			"level 0x0000000000000100: archive 0x0000000000000100 lists 0x0000000000002222:0x0000000000000999"},
	} {
		w := newWorld(t)
		items := []item{levelRoot}
		if c.name == "unresolved" {
			items = append(items, c.extra...)
		} else {
			items = append(items, archive)
			items = append(items, c.extra...)
		}
		w.addChunk(items...)
		r := w.run()
		if r.code != 1 || !strings.Contains(r.stderr, "the runtime would refuse this set (1 problems") ||
			!strings.Contains(r.stderr, c.want) {
			t.Errorf("%s: exit %d: %s", c.name, r.code, r.stderr)
		}
		js, err := os.ReadFile(filepath.Join(w.dir, "out", "levels.nevrres.json"))
		if err != nil {
			t.Fatalf("%s: no report: %v", c.name, err)
		}
		var rep report
		if json.Unmarshal(js, &rep) != nil || len(rep.Problems) != 1 || !strings.Contains(rep.Problems[0], c.want) {
			t.Errorf("%s: report problems = %v", c.name, rep.Problems)
		}
	}
}

func TestAnOverrideOfAnArchiveIsRefused(t *testing.T) {
	t.Parallel()
	w := newWorld(t)
	w.addChunk(levelRoot, archive)
	r := w.run("-override", key{archiveType, 0x500}.String())
	if r.code != 1 || !strings.Contains(r.stderr, "an archive is read by the archive loader, which an override does not reach") {
		t.Errorf("exit %d: %s", r.code, r.stderr)
	}
}

func TestServableProblemsAreTheRuntimesRegistryRules(t *testing.T) {
	t.Parallel()
	const L, D = 0x100, 0x200 // a level, and an archive it depends on
	stock := map[key]bool{{0x1111, 1}: true, {archiveType, 0x300}: true}
	stockHas := func(k key) bool { return stock[k] }
	set := func(archives map[uint64][]byte, extra ...key) *served {
		sv := &served{archives: archives}
		for name := range archives {
			sv.keys = append(sv.keys, key{archiveType, name})
		}
		sv.keys = append(sv.keys, key{levelRootType, L})
		return &served{keys: append(sv.keys, extra...), archives: archives}
	}
	for _, c := range []struct {
		name string
		sv   *served
		want []string
	}{
		{"an entry the game has", set(map[uint64][]byte{L: archiveBytes(nil, []key{{0x1111, 1}})}), nil},
		{"an entry the set has", set(map[uint64][]byte{L: archiveBytes(nil, []key{{0x2222, 9}})}, key{0x2222, 9}), nil},
		{"an entry nothing has", set(map[uint64][]byte{L: archiveBytes(nil, []key{{0x2222, 9}})}),
			[]string{"level 0x0000000000000100: archive 0x0000000000000100 lists 0x0000000000002222:0x0000000000000009, which neither the set nor the game has"}},
		{"a dependency the game has ends the walk", set(map[uint64][]byte{L: archiveBytes([]uint64{0x300}, nil)}), nil},
		{"a dependency nothing has", set(map[uint64][]byte{L: archiveBytes([]uint64{0x400}, nil)}),
			[]string{"level 0x0000000000000100 depends on archive 0x0000000000000400, which neither the set nor the game has"}},
		{"a dependency in the set is walked", set(map[uint64][]byte{L: archiveBytes([]uint64{D}, nil), D: archiveBytes([]uint64{L}, []key{{0x2222, 7}})}),
			[]string{"level 0x0000000000000100: archive 0x0000000000000200 lists 0x0000000000002222:0x0000000000000007, which neither the set nor the game has"}},
		{"an archive that does not parse", set(map[uint64][]byte{L: {1, 0, 0, 0}}),
			[]string{"archive 0x2a41cf1c1d9e5d32:0x0000000000000100 does not parse"}},
		{"an archive over the limit", set(map[uint64][]byte{L: append(archiveBytes(nil, nil), make([]byte, maxArchive)...)}),
			[]string{"archive 0x2a41cf1c1d9e5d32:0x0000000000000100 is 524304 bytes; the runtime reads at most 524288"}},
		{"a script", set(map[uint64][]byte{L: archiveBytes(nil, nil)}, key{scriptType, 5}),
			[]string{"script record 0x991421b41de58370:0x0000000000000005: the game loads a script as a DLL by name"}},
		{"a texture and its payload", set(map[uint64][]byte{L: archiveBytes(nil, nil)}, key{textureType, 6}, key{textureGpuType, 6}), nil},
	} {
		got := servableProblems(c.sv, stockHas)
		if strings.Join(got, "|") != strings.Join(c.want, "|") {
			t.Errorf("%s: got %q, want %q", c.name, got, c.want)
		}
	}
}

func TestARunIsRecordedInTheToolsLog(t *testing.T) {
	t.Parallel()
	w := newWorld(t)
	w.addChunk(levelRoot, archive)
	r := w.run()
	if r.code != 0 {
		t.Fatalf("exit %d: %s", r.code, r.stderr)
	}
	raw, err := os.ReadFile(filepath.Join(testkit.Getenv(r.env)("XDG_STATE_HOME"), "nevr-runtime", "levelpack.jsonl"))
	if err != nil {
		t.Fatal(err)
	}
	var seen []string
	for _, line := range strings.Split(strings.TrimSpace(string(raw)), "\n") {
		var rec map[string]any
		if err := json.Unmarshal([]byte(line), &rec); err != nil {
			t.Fatalf("not a JSON line: %q", line)
		}
		seen = append(seen, rec["msg"].(string))
		if rec["msg"] == "written" && (rec["sha256"] != r.rep.Sha256 || rec["levels"] != float64(1)) {
			t.Errorf("written record = %v", rec)
		}
		if rec["msg"] == "verified" && rec["frames_decoded"] != float64(1) {
			t.Errorf("verified record = %v", rec)
		}
	}
	if strings.Join(seen, ",") != "start,classified,verified,written" {
		t.Errorf("records = %v", seen)
	}
}

func TestUsageErrorsExitTwo(t *testing.T) {
	t.Parallel()
	for _, args := range [][]string{
		{"-manifest", "m", "-stock", "s", "-stock-packages", "d"},
		{"-manifest", "m", "-stock", "s", "-stock-packages", "d", "-out", "o", "-override", "0x1"},
		{"-manifest", "m", "-stock", "s", "-stock-packages", "d", "-out", "o", "-package", "x=f"},
	} {
		var stdout, stderr bytes.Buffer
		if code := run(t.Context(), args, testkit.Getenv(testkit.Env(t)), strings.NewReader(""), &stdout, &stderr); code != 2 {
			t.Errorf("%v: exit %d: %s", args, code, stderr.String())
		}
	}
}

// goodSet is a valid set: two records in a small chunk, one record alone in a large chunk, one override.
func goodSet(t *testing.T) []byte {
	t.Helper()
	plan := setPlan{
		Records: []setRecord{
			{Key: key{1, 1}, Chunk: 0, OffsetInChunk: 0, Size: 8, Alignment: 16},
			{Key: key{1, 2}, Chunk: 0, OffsetInChunk: 16, Size: 4, Alignment: 16},
			{Key: key{1, 3}, Chunk: 1, OffsetInChunk: 0, Size: blockLimit + 1, Alignment: 16},
		},
		Chunks: []setChunk{
			{Frame: rawFrame(fill(20, 7)), Decompressed: 20},
			{Frame: rawFrame(fill(blockLimit+1, 8)), Decompressed: blockLimit + 1},
		},
		Overrides: []setOverride{{Key: key{2, 1}, Bytes: []byte("override")}},
	}
	var b bytes.Buffer
	n, err := writeSet(&b, plan)
	if err != nil || n != uint64(b.Len()) {
		t.Fatalf("writeSet = %d, %v (wrote %d)", n, err, b.Len())
	}
	return b.Bytes()
}

func TestValidateSetAppliesTheRuntimesRules(t *testing.T) {
	t.Parallel()
	u32 := func(set []byte, off uint64, v uint32) { binary.LittleEndian.PutUint32(set[off:], v) }
	rec := func(set []byte, i uint64) uint64 { return binary.LittleEndian.Uint64(set[0x18:]) + i*setRecordBytes }
	chk := func(set []byte, i uint64) uint64 { return binary.LittleEndian.Uint64(set[0x20:]) + i*setChunkBytes }
	ovr := func(set []byte, i uint64) uint64 { return binary.LittleEndian.Uint64(set[0x28:]) + i*setOverrideBytes }
	cases := []struct {
		rule   string
		mutate func(set []byte) []byte
	}{
		{"", func(s []byte) []byte { return s }},
		{"", func(s []byte) []byte { u32(s, rec(s, 1)+0x14, 20); u32(s, rec(s, 1)+0x18, 0); return s }}, // 0 bytes at the end
		{"too_small", func(s []byte) []byte { return s[:47] }},
		{"magic", func(s []byte) []byte { s[0] = 'X'; return s }},
		{"header", func(s []byte) []byte { u32(s, 8, 40); return s }},
		{"no_records", func(s []byte) []byte { u32(s, 0x0c, 0); return s }},
		{"table_range", func(s []byte) []byte { u32(s, 0x0c, 1<<28); return s }},
		{"chunk_range", func(s []byte) []byte { u32(s, chk(s, 0)+8, uint32(len(s))); return s }},
		{"chunk_not_zstd", func(s []byte) []byte { s[binary.LittleEndian.Uint32(s[chk(s, 0)+4:])] ^= 1; return s }},
		{"chunk_size", func(s []byte) []byte { u32(s, chk(s, 0)+12, 0); return s }},
		{"record_chunk", func(s []byte) []byte { u32(s, rec(s, 0)+0x10, 2); return s }},
		{"record_range", func(s []byte) []byte { u32(s, rec(s, 1)+0x18, 5); return s }},
		{"record_alignment", func(s []byte) []byte { u32(s, rec(s, 0)+0x1c, 12); return s }},
		{"record_in_large_chunk", func(s []byte) []byte { u32(s, rec(s, 0)+0x10, 1); return s }},
		{"record_not_alone", func(s []byte) []byte { u32(s, chk(s, 1)+12, blockLimit+2); return s }}, // a large record and room beside it
		{"duplicate", func(s []byte) []byte { copy(s[rec(s, 1):rec(s, 1)+16], s[rec(s, 0):rec(s, 0)+16]); return s }},
		{"override_range", func(s []byte) []byte { u32(s, ovr(s, 0)+0x14, 0); return s }},
		{"override_is_record", func(s []byte) []byte { copy(s[ovr(s, 0):ovr(s, 0)+16], s[rec(s, 0):rec(s, 0)+16]); return s }},
		{"chunk_reserved", func(s []byte) []byte { u32(s, chk(s, 0), 1); return s }},
		{"record_alignment", func(s []byte) []byte { u32(s, rec(s, 0)+0x1c, 0x2000); return s }}, // over 4096
		{"", func(s []byte) []byte { u32(s, rec(s, 0)+0x1c, 0x1000); return s }},                 // 4096 is the game's largest
		{"record_not_alone", func(s []byte) []byte { // two large records claim chunk 1's whole bytes
			u32(s, rec(s, 1)+0x10, 1)
			u32(s, rec(s, 1)+0x14, 0)
			u32(s, rec(s, 1)+0x18, blockLimit+1)
			return s
		}},
	}
	for _, c := range cases {
		set := c.mutate(goodSet(t))
		err := validateSet(bytes.NewReader(set), uint64(len(set)))
		switch {
		case c.rule == "" && err != nil:
			t.Errorf("a valid set is refused: %v", err)
		case c.rule != "" && (err == nil || !strings.HasPrefix(err.Error(), c.rule)):
			t.Errorf("%s: got %v", c.rule, err)
		}
	}
}

func TestValidateSetRefusesTwoOverridesOfOneKey(t *testing.T) {
	t.Parallel()
	plan := setPlan{
		Records:   []setRecord{{Key: key{1, 1}, Size: 1, Alignment: 1}},
		Chunks:    []setChunk{{Frame: rawFrame([]byte{1}), Decompressed: 1}},
		Overrides: []setOverride{{Key: key{2, 1}, Bytes: []byte{1}}, {Key: key{2, 1}, Bytes: []byte{2}}},
	}
	var b bytes.Buffer
	if _, err := writeSet(&b, plan); err != nil {
		t.Fatal(err)
	}
	if err := validateSet(bytes.NewReader(b.Bytes()), uint64(b.Len())); err == nil || !strings.HasPrefix(err.Error(), "override_duplicate") {
		t.Errorf("got %v", err)
	}
}

func TestVerifySetDecodesEveryFrameAndComparesTheBytesItWasGiven(t *testing.T) {
	t.Parallel()
	dec, err := newDecoder()
	if err != nil {
		t.Fatal(err)
	}
	defer dec.Close()
	set := goodSet(t)
	want := map[key][]byte{{1, 2}: fill(4, 7), {2, 1}: []byte("override")}
	frames, compared, sv, err := verifySet(bytes.NewReader(set), uint64(len(set)), dec, want)
	if err != nil || frames != 2 || compared != 2 || len(sv.keys) != 3 {
		t.Errorf("verifySet = %d frames, %d compared, %v, %v", frames, compared, sv, err)
	}

	want[key{1, 2}] = fill(4, 9)
	if _, _, _, err := verifySet(bytes.NewReader(set), uint64(len(set)), dec, want); err == nil ||
		!strings.Contains(err.Error(), "record 0x0000000000000001:0x0000000000000002: the set serves other bytes") {
		t.Errorf("a record with other bytes: %v", err)
	}
	want[key{1, 2}] = fill(4, 7)
	want[key{2, 1}] = []byte("Override")
	if _, _, _, err := verifySet(bytes.NewReader(set), uint64(len(set)), dec, want); err == nil ||
		!strings.Contains(err.Error(), "override 0x0000000000000002:0x0000000000000001") {
		t.Errorf("an override with other bytes: %v", err)
	}

	short := goodSet(t)
	binary.LittleEndian.PutUint32(short[binary.LittleEndian.Uint64(short[0x20:])+12:], 21) // the frame holds 20
	if _, _, _, err := verifySet(bytes.NewReader(short), uint64(len(short)), dec, nil); err == nil ||
		!strings.Contains(err.Error(), "chunk 0 decodes to 20 bytes, its entry says 21") {
		t.Errorf("a frame shorter than its entry: %v", err)
	}
}
