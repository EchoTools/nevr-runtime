package radpack

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

const set = "48037dc70b0ecab2"

func stock(t *testing.T) (string, *Set) {
	t.Helper()
	data := filepath.Join(t.TempDir(), "data")
	err := WriteStockSet(data, set, []map[Key][]byte{
		{{Type: 1, Name: 10}: []byte("stock one"), {Type: -5, Name: 3}: []byte("signed first")},
		{{Type: 2, Name: 20}: bytes.Repeat([]byte("two"), 1000)},
	})
	if err != nil {
		t.Fatal(err)
	}
	s, _, err := OpenSet(data, set)
	if err != nil {
		t.Fatal(err)
	}
	return data, s
}

func TestAStockSetReadsBackAndEncodesToTheSameBytes(t *testing.T) {
	data, s := stock(t)
	if got, ok, err := s.Read(2, 20); err != nil || !ok || !bytes.Equal(got, bytes.Repeat([]byte("two"), 1000)) {
		t.Fatalf("read %q %v %v", got, ok, err)
	}
	if _, ok, _ := s.Read(9, 9); ok {
		t.Error("a resource the set does not list was found")
	}
	if s.Manifest.Resources[0].Type != -5 {
		t.Errorf("resources are not sorted as signed: %+v", s.Manifest.Resources)
	}
	raw, _ := os.ReadFile(filepath.Join(data, "manifests", set))
	again, err := DecodeFile(raw)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(again.Plain(), s.Manifest.Plain()) {
		t.Error("decode then encode changed the manifest")
	}
}

func TestATableCountThatDoesNotFitIsRefused(t *testing.T) {
	_, s := stock(t)
	plain := s.Manifest.Plain()
	// Two fewer file records (-80 bytes) and five more chunks (+80): the tables still fill the manifest exactly, but the
	// file table is no longer parallel to the resources.
	le := binary.LittleEndian
	le.PutUint64(plain[descFiles+0x30:], le.Uint64(plain[descFiles+0x30:])-2)
	le.PutUint64(plain[descChunks+0x30:], le.Uint64(plain[descChunks+0x30:])+5)
	if _, err := ParsePlain(plain); err == nil || !strings.Contains(err.Error(), "parallel") {
		t.Errorf("a manifest whose file table is not parallel to its resources was read: %v", err)
	}
	short := s.Manifest.Plain()
	if _, err := ParsePlain(short[:len(short)-1]); err == nil {
		t.Error("a manifest whose tables overrun it was read")
	}
}

func TestBuildReplacesAndAddsInANewPackage(t *testing.T) {
	data, s := stock(t)
	changes := map[Key][]byte{
		{Type: 1, Name: 10}: []byte("the pack's one"), // replaced
		{Type: 1, Name: 5}:  []byte("added, sorts before 1.10"),
		{Type: 3, Name: 1}:  []byte("added last"),
	}
	manifest, pkg, err := Build(s.Manifest, changes)
	if err != nil {
		t.Fatal(err)
	}
	// The pack's set as the game sees it: the pack's manifest, the stock packages and the new one.
	if err := os.WriteFile(filepath.Join(data, "manifests", set), manifest, 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(data, "packages", set+"_2"), pkg, 0o644); err != nil {
		t.Fatal(err)
	}
	out, _, err := OpenSet(data, set)
	if err != nil {
		t.Fatal(err)
	}
	m := out.Manifest
	if m.PackageCount() != 3 {
		t.Errorf("package count %d", m.PackageCount())
	}
	for k, want := range map[Key][]byte{
		{Type: 1, Name: 10}: []byte("the pack's one"), {Type: 1, Name: 5}: []byte("added, sorts before 1.10"),
		{Type: 3, Name: 1}: []byte("added last"), {Type: 2, Name: 20}: bytes.Repeat([]byte("two"), 1000),
		{Type: -5, Name: 3}: []byte("signed first"),
	} {
		got, ok, err := out.Read(k.Type, k.Name)
		if err != nil || !ok || !bytes.Equal(got, want) {
			t.Errorf("%v: %q %v %v", k, got, ok, err)
		}
	}
	if len(m.Resources) != 5 || len(m.Files) != 5 {
		t.Fatalf("%d resources, %d file records", len(m.Resources), len(m.Files))
	}
	for i, r := range m.Resources {
		if i > 0 && !less(m.Resources[i-1].Type, r.Type, m.Resources[i-1].Name, r.Name) {
			t.Errorf("resources not sorted at %d", i)
		}
		f := m.Files[i]
		if int64(binary.LittleEndian.Uint64(f[0:])) != r.Type || int64(binary.LittleEndian.Uint64(f[8:])) != r.Name {
			t.Errorf("file record %d is not parallel to its resource", i)
		}
	}
	// A replaced resource keeps its stock file record; an added one gets a zero tail.
	if i := m.Find(1, 10); binary.LittleEndian.Uint64(m.Files[i][32:]) != 0x1234 {
		t.Error("the replaced resource's file record changed")
	}
	if i := m.Find(1, 5); binary.LittleEndian.Uint64(m.Files[i][32:]) != 0 {
		t.Error("an added resource's file record has a tail")
	}
	// Chunks: the stock data chunks, the pack's, the stock end records, the new package's end record last.
	n := len(m.Chunks)
	if m.Chunks[n-1] != (Chunk{Package: 2, Offset: uint32(len(pkg))}) {
		t.Errorf("last chunk record %+v, want the new package's end", m.Chunks[n-1])
	}
	for i, c := range m.Chunks {
		if c.Decompressed == 0 && i < n-3 {
			t.Errorf("end record at %d before the data chunks end", i)
		}
	}
	for _, i := range []int{n - 3, n - 2} {
		if m.Chunks[i].Decompressed != 0 || m.Chunks[i].Package != uint32(i-(n-3)) {
			t.Errorf("stock end record %d: %+v", i, m.Chunks[i])
		}
	}
	// The stock resources still name their stock chunks.
	if r := m.Resources[m.Find(2, 20)]; m.Chunks[r.Chunk].Package != 1 {
		t.Errorf("a stock resource moved: %+v", r)
	}
}

func TestBuildRefusesNothingAndEmptyBytes(t *testing.T) {
	_, s := stock(t)
	if _, _, err := Build(s.Manifest, nil); err == nil {
		t.Error("an empty change set was built")
	}
	if _, _, err := Build(s.Manifest, map[Key][]byte{{Type: 1, Name: 1}: nil}); err == nil {
		t.Error("a resource with no bytes was built")
	}
}

func TestBuildRefusesASetWhoseEndRecordsAreNotLast(t *testing.T) {
	_, s := stock(t)
	m := *s.Manifest
	m.Chunks = append([]Chunk{m.Chunks[len(m.Chunks)-1]}, m.Chunks[:len(m.Chunks)-1]...)
	if _, _, err := Build(&m, map[Key][]byte{{Type: 1, Name: 1}: []byte("x")}); err == nil {
		t.Error(fmt.Sprintf("a set with an end record first was built: %+v", m.Chunks))
	}
}

func TestATablesCapacityIsKeptAndGrowsOnlyWhenTheTableDoes(t *testing.T) {
	_, s := stock(t)
	le := binary.LittleEndian
	m := *s.Manifest
	m.Head = append([]byte(nil), m.Head...)
	le.PutUint64(m.Head[descChunks+0x28:], 32) // a capacity above the count, as the shipped small set has
	plain := m.Plain()
	if c := le.Uint64(plain[descChunks+0x28:]); c != 32 || le.Uint64(plain[descChunks+0x08:]) != 32*chunkSize {
		t.Errorf("capacity %d, byte size %d: want 32 and 512", c, le.Uint64(plain[descChunks+0x08:]))
	}
	if n := le.Uint64(plain[descChunks+0x30:]); n != uint64(len(m.Chunks)) {
		t.Errorf("count %d, want %d", n, len(m.Chunks))
	}
	// A table that grows past its capacity takes the new length as its capacity.
	raw, _, err := Build(s.Manifest, map[Key][]byte{{Type: 8, Name: 8}: []byte("a")})
	if err != nil {
		t.Fatal(err)
	}
	built, err := DecodeFile(raw)
	if err != nil {
		t.Fatal(err)
	}
	p := built.Plain()
	if c, n := le.Uint64(p[descRes+0x28:]), le.Uint64(p[descRes+0x30:]); c != n || n != uint64(len(s.Manifest.Resources)+1) {
		t.Errorf("resource capacity %d count %d after one added", c, n)
	}
}

func TestAManifestWhoseCountsWrapItsSizeIsRefused(t *testing.T) {
	// 16 bytes of tables after the header, and a chunk count of 1+2^60: count times 16 wraps to 16, so a sum that is
	// not bounded first says the tables fit.
	plain := make([]byte, tablesStart+chunkSize)
	binary.LittleEndian.PutUint64(plain[descChunks+0x30:], 1+1<<60)
	if _, err := ParsePlain(plain); err == nil {
		t.Error("a manifest whose chunk count wraps its size was read")
	}
	// 0x48 bytes of tables, and resource and file counts of 1+2^61: each times its record size, summed, wraps to 0x48.
	plain = make([]byte, tablesStart+resSize+fileSize)
	binary.LittleEndian.PutUint64(plain[descRes+0x30:], 1+1<<61)
	binary.LittleEndian.PutUint64(plain[descFiles+0x30:], 1+1<<61)
	if _, err := ParsePlain(plain); err == nil {
		t.Error("a manifest whose resource and file counts wrap its size was read")
	}
}

func TestChunksReadsAChunkAndTheHeadOfItsFrame(t *testing.T) {
	t.Parallel()
	_, s := stock(t)
	c, err := NewChunks()
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	// The stock set lists its chunks in package order, each package's by (type, name): "signed first", "stock one", then
	// the second package's resource.
	for i, want := range [][]byte{[]byte("signed first"), []byte("stock one"), bytes.Repeat([]byte("two"), 1000)} {
		got, err := c.Plain(s, i)
		if err != nil || !bytes.Equal(got, want) {
			t.Errorf("chunk %d = %q, %v", i, got, err)
		}
	}
	head, err := c.FrameHead(s, 2, 4)
	if err != nil || !bytes.Equal(head, []byte{0x28, 0xb5, 0x2f, 0xfd}) {
		t.Errorf("the frame head of chunk 2 = %x, %v", head, err)
	}
	if head, err := c.FrameHead(s, 0, 1<<20); err != nil || len(head) != int(s.Manifest.Chunks[0].Compressed) {
		t.Errorf("a head longer than the frame is %d bytes (%v), want the frame's %d", len(head), err, s.Manifest.Chunks[0].Compressed)
	}
	if _, err := c.Plain(s, len(s.Manifest.Chunks)); err == nil || !strings.Contains(err.Error(), "the manifest has 5 chunks") {
		t.Errorf("a chunk past the table: %v", err)
	}
	s.Manifest.Chunks[1].Decompressed++
	if _, err := c.Plain(s, 1); err == nil || !strings.Contains(err.Error(), "does not decompress to the resource it holds") {
		t.Errorf("a chunk whose frame holds other than its stated length: %v", err)
	}
}
