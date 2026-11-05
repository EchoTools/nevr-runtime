package main

import (
	"encoding/binary"
	"errors"
	"fmt"

	"github.com/klauspost/compress/zstd"
)

// A package manifest as the game reads it (docs/reference/package-manifest-format.md): a 24-byte container ("ZSTD",
// u32 0x10, u64 decompressed length, u64 compressed length) and one zstd frame holding the body. The body's counts are
// at 0x00 (packages), 0x04 (block size), 0x38 (records), 0x78 (files) and 0xb8 (chunks); the arrays follow from 0xc0:
// records of 0x20 bytes, files of 0x28 bytes, chunks of 0x10 bytes.

const (
	containerBytes = 24
	recordBytes    = 0x20
	fileBytes      = 0x28
	chunkBytes     = 0x10
	bodyArrays     = 0xc0
	maxBody        = 256 << 20
)

// key is a resource's on-disk type and name, the pair the game's manifests are searched by.
type key struct{ Type, Name uint64 }

func (k key) String() string { return fmt.Sprintf("0x%016x:0x%016x", k.Type, k.Name) }

type record struct {
	Key           key
	Chunk         uint32
	OffsetInChunk uint32
	Size          uint32
	Alignment     uint32
}

type chunk struct {
	Package      uint32
	FileOffset   uint32
	Compressed   uint32
	Decompressed uint32
}

type manifest struct {
	Packages  uint32
	BlockSize uint32
	Records   []record
	Chunks    []chunk
	index     map[key]int
}

func (m *manifest) find(k key) (record, bool) {
	i, ok := m.index[k]
	if !ok {
		return record{}, false
	}
	return m.Records[i], true
}

func parseManifest(file []byte) (*manifest, error) {
	if len(file) < containerBytes || string(file[:4]) != "ZSTD" {
		return nil, errors.New("not a ZSTD manifest container")
	}
	if binary.LittleEndian.Uint32(file[4:]) != 0x10 {
		return nil, errors.New("unsupported manifest container version")
	}
	plainLen := binary.LittleEndian.Uint64(file[8:])
	packedLen := binary.LittleEndian.Uint64(file[16:])
	if packedLen > uint64(len(file)-containerBytes) {
		return nil, errors.New("the compressed length runs past the end of the file")
	}
	if plainLen < bodyArrays || plainLen > maxBody {
		return nil, fmt.Errorf("decompressed length %d is out of range", plainLen)
	}
	dec, err := zstd.NewReader(nil, zstd.WithDecoderMaxMemory(maxBody))
	if err != nil {
		return nil, err
	}
	defer dec.Close()
	body, err := dec.DecodeAll(file[containerBytes:containerBytes+int(packedLen)], make([]byte, 0, plainLen))
	if err != nil {
		return nil, fmt.Errorf("the manifest frame does not decode: %w", err)
	}
	if uint64(len(body)) != plainLen {
		return nil, fmt.Errorf("the manifest frame decodes to %d bytes, the container says %d", len(body), plainLen)
	}
	m := &manifest{
		Packages:  binary.LittleEndian.Uint32(body[0:]),
		BlockSize: binary.LittleEndian.Uint32(body[4:]),
	}
	records := binary.LittleEndian.Uint64(body[0x38:])
	files := binary.LittleEndian.Uint64(body[0x78:])
	chunks := binary.LittleEndian.Uint64(body[0xb8:])
	need := uint64(bodyArrays) + records*recordBytes + files*fileBytes + chunks*chunkBytes
	if records > maxBody || files > maxBody || chunks > maxBody || need != uint64(len(body)) {
		return nil, fmt.Errorf("the counts (%d records, %d files, %d chunks) do not account for the %d-byte body", records, files, chunks, len(body))
	}
	m.Records = make([]record, records)
	m.index = make(map[key]int, records)
	for i := range m.Records {
		at := bodyArrays + i*recordBytes
		r := record{
			Key:           key{binary.LittleEndian.Uint64(body[at:]), binary.LittleEndian.Uint64(body[at+8:])},
			Chunk:         binary.LittleEndian.Uint32(body[at+0x10:]),
			OffsetInChunk: binary.LittleEndian.Uint32(body[at+0x14:]),
			Size:          binary.LittleEndian.Uint32(body[at+0x18:]),
			Alignment:     binary.LittleEndian.Uint32(body[at+0x1c:]),
		}
		if r.Chunk >= uint32(chunks) {
			return nil, fmt.Errorf("record %s names chunk %d of %d", r.Key, r.Chunk, chunks)
		}
		if _, dup := m.index[r.Key]; dup {
			return nil, fmt.Errorf("record %s appears twice", r.Key)
		}
		m.Records[i] = r
		m.index[r.Key] = i
	}
	chunkAt := bodyArrays + int(records)*recordBytes + int(files)*fileBytes
	m.Chunks = make([]chunk, chunks)
	for i := range m.Chunks {
		at := chunkAt + i*chunkBytes
		m.Chunks[i] = chunk{
			Package:      binary.LittleEndian.Uint32(body[at:]),
			FileOffset:   binary.LittleEndian.Uint32(body[at+4:]),
			Compressed:   binary.LittleEndian.Uint32(body[at+8:]),
			Decompressed: binary.LittleEndian.Uint32(body[at+12:]),
		}
	}
	return m, nil
}
