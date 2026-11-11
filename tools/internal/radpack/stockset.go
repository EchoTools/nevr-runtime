package radpack

import (
	"encoding/binary"
	"fmt"
	"os"
	"path/filepath"
	"sort"

	"github.com/klauspost/compress/zstd"
)

// WriteStockSet writes a package set with the stock layout to <data>/manifests/<id> and <data>/packages/<id>_<n>, for
// tests: `packages` is each package's resources, one chunk per resource. Records come out sorted and parallel, every
// package's chunks in package order, then one end record per package, as in the shipped sets. No test data comes from the
// game.
func WriteStockSet(data, id string, packages []map[Key][]byte) error {
	if err := os.MkdirAll(filepath.Join(data, "manifests"), 0o755); err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Join(data, "packages"), 0o755); err != nil {
		return err
	}
	enc, err := zstd.NewWriter(nil)
	if err != nil {
		return err
	}
	defer enc.Close()
	m := &Manifest{Head: make([]byte, tablesStart)}
	binary.LittleEndian.PutUint32(m.Head[0:], uint32(len(packages)))
	binary.LittleEndian.PutUint32(m.Head[4:], 0x80000)
	for _, desc := range []int{descRes, descFiles, descChunks} {
		binary.LittleEndian.PutUint64(m.Head[desc+0x18:], 1<<32)
		binary.LittleEndian.PutUint64(m.Head[desc+0x20:], 32)
	}
	var ends []Chunk
	for p, res := range packages {
		keys := make([]Key, 0, len(res))
		for k := range res {
			keys = append(keys, k)
		}
		sort.Slice(keys, func(i, j int) bool { return less(keys[i].Type, keys[j].Type, keys[i].Name, keys[j].Name) })
		var file []byte
		for _, k := range keys {
			packed := enc.EncodeAll(res[k], nil)
			m.Resources = append(m.Resources, Resource{Type: k.Type, Name: k.Name, Chunk: uint32(len(m.Chunks)), Size: uint32(len(res[k])), Alignment: 16})
			m.Chunks = append(m.Chunks, Chunk{Package: uint32(p), Offset: uint32(len(file)), Compressed: uint32(len(packed)), Decompressed: uint32(len(res[k]))})
			file = append(file, packed...)
		}
		if err := os.WriteFile(filepath.Join(data, "packages", fmt.Sprintf("%s_%d", id, p)), file, 0o644); err != nil {
			return err
		}
		ends = append(ends, Chunk{Package: uint32(p), Offset: uint32(len(file)), Compressed: 7})
	}
	m.Chunks = append(m.Chunks, ends...)
	order := make([]int, len(m.Resources))
	for i := range order {
		order[i] = i
	}
	sort.Slice(order, func(a, b int) bool {
		ra, rb := m.Resources[order[a]], m.Resources[order[b]]
		return less(ra.Type, rb.Type, ra.Name, rb.Name)
	})
	sorted := make([]Resource, len(order))
	for i, j := range order {
		sorted[i] = m.Resources[j]
	}
	m.Resources = sorted
	for _, r := range m.Resources {
		var f [fileSize]byte
		binary.LittleEndian.PutUint64(f[0:], uint64(r.Type))
		binary.LittleEndian.PutUint64(f[8:], uint64(r.Name))
		binary.LittleEndian.PutUint64(f[32:], 0x1234) // a stock file record carries values here; kept as they are
		m.Files = append(m.Files, f)
	}
	raw, err := m.EncodeFile()
	if err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(data, "manifests", id), raw, 0o644)
}
