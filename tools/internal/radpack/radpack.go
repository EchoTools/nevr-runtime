// Package radpack reads the game's package sets (a manifest and its package files) and writes a content pack's set: the
// stock manifest with the pack's resources added or replaced, and the one package that holds their bytes
// (docs/reference/content-packs.md, docs/engine/asset-system.md "Manifest"). It is the format code under
// tools/cmd/content-pack-build.
//
// A manifest file is a 24-byte header ("ZSTD", u32 0x10, u64 decompressed length, u64 compressed length) and one zstd
// frame. Decompressed: u32 package count, u32 block size, then three table descriptors of 0x40 bytes from 0x08
// (resources 0x20 B each, file records 0x28 B each, chunks 0x10 B each; +0x08 the capacity times the record size,
// +0x28 the capacity, +0x30 the count), then the tables from 0xc0, back to back, each `count` records long. Measured on the PC
// stock data (5932408047): the resource and file tables are parallel (the same type and name at each index) and sorted
// by (type, name) as signed 64-bit integers; the chunk table holds every package's chunks in package order, then one end
// record per package, (package, the package file's size, any, 0), in package order.
package radpack

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"

	"github.com/klauspost/compress/zstd"
)

const (
	headerSize  = 24
	tablesStart = 0xc0
	descRes     = 0x08
	descFiles   = 0x48
	descChunks  = 0x88
	resSize     = 0x20
	fileSize    = 0x28
	chunkSize   = 0x10
)

// Resource is a resource record: an asset of the set, in one chunk at an offset.
type Resource struct {
	Type, Name          int64
	Chunk, Offset, Size uint32
	Alignment           uint32
}

// Chunk is a chunk record: one zstd frame in a package file, or (Decompressed 0) a package's end record.
type Chunk struct {
	Package, Offset, Compressed, Decompressed uint32
}

// Manifest is a decompressed manifest, its tables decoded. Head keeps the first 0xc0 bytes as they were.
type Manifest struct {
	Head      []byte
	Resources []Resource
	Files     [][fileSize]byte // parallel to Resources
	Chunks    []Chunk
}

// PackageCount is the number of package files the set has.
func (m *Manifest) PackageCount() uint32 { return binary.LittleEndian.Uint32(m.Head[0:]) }

// BlockSize is the largest chunk of packed small assets the set uses.
func (m *Manifest) BlockSize() uint32 { return binary.LittleEndian.Uint32(m.Head[4:]) }

// ParsePlain decodes a decompressed manifest.
func ParsePlain(plain []byte) (*Manifest, error) {
	if len(plain) < tablesStart {
		return nil, errors.New("manifest shorter than its header")
	}
	le := binary.LittleEndian
	count := func(desc int) uint64 { return le.Uint64(plain[desc+0x30:]) }
	rc, fc, cc := count(descRes), count(descFiles), count(descChunks)
	if rc != fc {
		return nil, fmt.Errorf("manifest has %d resources and %d file records; they are parallel tables", rc, fc)
	}
	end := uint64(tablesStart) + rc*resSize + fc*fileSize + cc*chunkSize
	if end != uint64(len(plain)) {
		return nil, fmt.Errorf("manifest tables end at %d, the manifest at %d", end, len(plain))
	}
	m := &Manifest{Head: append([]byte(nil), plain[:tablesStart]...)}
	off := tablesStart
	for i := uint64(0); i < rc; i++ {
		b := plain[off:]
		m.Resources = append(m.Resources, Resource{
			Type: int64(le.Uint64(b[0:])), Name: int64(le.Uint64(b[8:])),
			Chunk: le.Uint32(b[16:]), Offset: le.Uint32(b[20:]), Size: le.Uint32(b[24:]), Alignment: le.Uint32(b[28:]),
		})
		off += resSize
	}
	for i := uint64(0); i < fc; i++ {
		var f [fileSize]byte
		copy(f[:], plain[off:off+fileSize])
		m.Files = append(m.Files, f)
		off += fileSize
	}
	for i := uint64(0); i < cc; i++ {
		b := plain[off:]
		m.Chunks = append(m.Chunks, Chunk{le.Uint32(b[0:]), le.Uint32(b[4:]), le.Uint32(b[8:]), le.Uint32(b[12:])})
		off += chunkSize
	}
	return m, nil
}

// Plain encodes the manifest. Each table's count is its length; its capacity stays the stock one unless the table has
// grown past it, and its byte size is the capacity times its record size (both shipped sets have it so; the small set's
// chunk table has capacity 32 for 21 records). Every other header byte is kept.
func (m *Manifest) Plain() []byte {
	le := binary.LittleEndian
	head := append([]byte(nil), m.Head...)
	set := func(desc int, n int, size int) {
		capacity := le.Uint64(head[desc+0x28:])
		if uint64(n) > capacity {
			capacity = uint64(n)
		}
		le.PutUint64(head[desc+0x08:], capacity*uint64(size))
		le.PutUint64(head[desc+0x28:], capacity)
		le.PutUint64(head[desc+0x30:], uint64(n))
	}
	set(descRes, len(m.Resources), resSize)
	set(descFiles, len(m.Files), fileSize)
	set(descChunks, len(m.Chunks), chunkSize)
	out := bytes.NewBuffer(head)
	var b [resSize]byte
	for _, r := range m.Resources {
		le.PutUint64(b[0:], uint64(r.Type))
		le.PutUint64(b[8:], uint64(r.Name))
		le.PutUint32(b[16:], r.Chunk)
		le.PutUint32(b[20:], r.Offset)
		le.PutUint32(b[24:], r.Size)
		le.PutUint32(b[28:], r.Alignment)
		out.Write(b[:])
	}
	for _, f := range m.Files {
		out.Write(f[:])
	}
	var c [chunkSize]byte
	for _, ch := range m.Chunks {
		le.PutUint32(c[0:], ch.Package)
		le.PutUint32(c[4:], ch.Offset)
		le.PutUint32(c[8:], ch.Compressed)
		le.PutUint32(c[12:], ch.Decompressed)
		out.Write(c[:])
	}
	return out.Bytes()
}

// DecodeFile reads a manifest file's bytes.
func DecodeFile(raw []byte) (*Manifest, error) {
	le := binary.LittleEndian
	if len(raw) < headerSize || !bytes.Equal(raw[:4], []byte("ZSTD")) || le.Uint32(raw[4:]) != 0x10 ||
		le.Uint64(raw[16:]) != uint64(len(raw)-headerSize) {
		return nil, errors.New("manifest header is not ZSTD, 0x10 and its own length")
	}
	dec, err := zstd.NewReader(nil, zstd.WithDecoderMaxMemory(1<<31))
	if err != nil {
		return nil, err
	}
	defer dec.Close()
	plain, err := dec.DecodeAll(raw[headerSize:], nil)
	if err != nil || uint64(len(plain)) != le.Uint64(raw[8:]) {
		return nil, errors.New("manifest does not decompress to the length its header names")
	}
	return ParsePlain(plain)
}

// EncodeFile writes the manifest as a manifest file.
func (m *Manifest) EncodeFile() ([]byte, error) {
	plain := m.Plain()
	enc, err := zstd.NewWriter(nil)
	if err != nil {
		return nil, err
	}
	packed := enc.EncodeAll(plain, nil)
	enc.Close()
	h := make([]byte, headerSize)
	copy(h, "ZSTD")
	binary.LittleEndian.PutUint32(h[4:], 0x10)
	binary.LittleEndian.PutUint64(h[8:], uint64(len(plain)))
	binary.LittleEndian.PutUint64(h[16:], uint64(len(packed)))
	return append(h, packed...), nil
}

func less(at, bt, an, bn int64) bool { return at < bt || (at == bt && an < bn) }

// Find is the index of (typ, name) in the sorted resource table, or -1.
func (m *Manifest) Find(typ, name int64) int {
	i := sort.Search(len(m.Resources), func(i int) bool {
		r := m.Resources[i]
		return !less(r.Type, typ, r.Name, name)
	})
	if i < len(m.Resources) && m.Resources[i].Type == typ && m.Resources[i].Name == name {
		return i
	}
	return -1
}

// Set is a stock package set opened for reading: its manifest and its package files.
type Set struct {
	Manifest *Manifest
	ID       string // the set, 16 lowercase hex
	data     string // the data folder that holds manifests/ and packages/
}

// OpenSet reads `<data>/manifests/<id>`.
func OpenSet(data, id string) (*Set, []byte, error) {
	raw, err := os.ReadFile(filepath.Join(data, "manifests", id))
	if err != nil {
		return nil, nil, err
	}
	m, err := DecodeFile(raw)
	if err != nil {
		return nil, nil, fmt.Errorf("%s: %w", id, err)
	}
	return &Set{Manifest: m, ID: id, data: data}, raw, nil
}

// Read is a stock resource's bytes, or false when the set does not list it.
func (s *Set) Read(typ, name int64) ([]byte, bool, error) {
	i := s.Manifest.Find(typ, name)
	if i < 0 {
		return nil, false, nil
	}
	r := s.Manifest.Resources[i]
	if int(r.Chunk) >= len(s.Manifest.Chunks) {
		return nil, true, fmt.Errorf("resource %016x.%016x names chunk %d of %d", uint64(typ), uint64(name), r.Chunk, len(s.Manifest.Chunks))
	}
	c := s.Manifest.Chunks[r.Chunk]
	f, err := os.Open(filepath.Join(s.data, "packages", fmt.Sprintf("%s_%d", s.ID, c.Package)))
	if err != nil {
		return nil, true, err
	}
	defer f.Close()
	packed := make([]byte, c.Compressed)
	if _, err := f.ReadAt(packed, int64(c.Offset)); err != nil && err != io.EOF {
		return nil, true, err
	}
	dec, err := zstd.NewReader(nil, zstd.WithDecoderMaxMemory(1<<31))
	if err != nil {
		return nil, true, err
	}
	defer dec.Close()
	plain, err := dec.DecodeAll(packed, nil)
	if err != nil || uint32(len(plain)) != c.Decompressed || uint64(r.Offset)+uint64(r.Size) > uint64(len(plain)) {
		return nil, true, fmt.Errorf("chunk %d of %s does not decompress to the resource it holds", r.Chunk, s.ID)
	}
	return plain[r.Offset : r.Offset+r.Size], true, nil
}

// Key names a resource by its on-disk type and name.
type Key struct{ Type, Name int64 }

// Build writes a pack's set: the stock manifest with `changes` applied (a resource the set lists is replaced, any other
// is added), and the new package `<id>_<stock count>` that holds them, one zstd chunk each. It returns the manifest
// file and the package file. The stock chunks and their end records keep their indices' order: the new chunks go after
// the stock chunks and before the end records, and the new package's end record goes last.
func Build(stock *Manifest, changes map[Key][]byte) ([]byte, []byte, error) {
	if len(changes) == 0 {
		return nil, nil, errors.New("no resource to pack")
	}
	m := &Manifest{Head: append([]byte(nil), stock.Head...), Resources: append([]Resource(nil), stock.Resources...),
		Files: append([][fileSize]byte(nil), stock.Files...)}
	pkgs := stock.PackageCount()
	// The stock chunks, then the stock end records (Decompressed 0), kept apart.
	var data, ends []Chunk
	for _, c := range stock.Chunks {
		if c.Decompressed == 0 {
			ends = append(ends, c)
		} else {
			data = append(data, c)
		}
	}
	if len(ends) != int(pkgs) {
		return nil, nil, fmt.Errorf("the stock set has %d end records for %d packages", len(ends), pkgs)
	}
	if len(data)+len(ends) != len(stock.Chunks) || !endsLast(stock.Chunks, len(data)) {
		return nil, nil, errors.New("the stock set's end records are not the last chunk records")
	}
	keys := make([]Key, 0, len(changes))
	for k := range changes {
		keys = append(keys, k)
	}
	sort.Slice(keys, func(i, j int) bool { return less(keys[i].Type, keys[j].Type, keys[i].Name, keys[j].Name) })
	enc, err := zstd.NewWriter(nil)
	if err != nil {
		return nil, nil, err
	}
	defer enc.Close()
	var pkg bytes.Buffer
	for _, k := range keys {
		body := changes[k]
		if len(body) == 0 {
			return nil, nil, fmt.Errorf("resource %016x.%016x has no bytes", uint64(k.Type), uint64(k.Name))
		}
		packed := enc.EncodeAll(body, nil)
		ci := uint32(len(data))
		data = append(data, Chunk{Package: pkgs, Offset: uint32(pkg.Len()), Compressed: uint32(len(packed)), Decompressed: uint32(len(body))})
		pkg.Write(packed)
		if i := m.Find(k.Type, k.Name); i >= 0 {
			m.Resources[i].Chunk, m.Resources[i].Offset, m.Resources[i].Size = ci, 0, uint32(len(body))
			continue
		}
		at := sort.Search(len(m.Resources), func(i int) bool {
			return !less(m.Resources[i].Type, k.Type, m.Resources[i].Name, k.Name)
		})
		var f [fileSize]byte
		binary.LittleEndian.PutUint64(f[0:], uint64(k.Type))
		binary.LittleEndian.PutUint64(f[8:], uint64(k.Name))
		m.Resources = append(m.Resources[:at], append([]Resource{{Type: k.Type, Name: k.Name, Chunk: ci, Size: uint32(len(body)), Alignment: 16}}, m.Resources[at:]...)...)
		m.Files = append(m.Files[:at], append([][fileSize]byte{f}, m.Files[at:]...)...)
	}
	m.Chunks = append(append(data, ends...), Chunk{Package: pkgs, Offset: uint32(pkg.Len())})
	binary.LittleEndian.PutUint32(m.Head[0:], pkgs+1)
	manifest, err := m.EncodeFile()
	return manifest, pkg.Bytes(), err
}

func endsLast(chunks []Chunk, dataCount int) bool {
	for i, c := range chunks {
		if (c.Decompressed == 0) != (i >= dataCount) {
			return false
		}
	}
	return true
}
