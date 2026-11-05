package main

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"os"

	"github.com/klauspost/compress/zstd"
)

// The resource set a plugin embeds (sdk/include/nevr/resource_set.h, NEVRRES1): a 48-byte header, the record, chunk
// and override tables, the chunks' zstd frames and the overrides' bytes, little endian, offsets from the set's start.

const (
	setHeaderBytes   = 48
	setRecordBytes   = 0x20
	setChunkBytes    = 0x10
	setOverrideBytes = 0x18
	blockLimit       = 0x80000 // the reader's block cache: a record of at most this sits in a chunk of at most this
	zstdMagic        = 0xFD2FB528
	maxSetBytes      = 0xFFFFFFFF // every offset in the set is a u32
)

var setMagic = []byte("NEVRRES1")

type setRecord struct {
	Key           key
	Chunk         uint32
	OffsetInChunk uint32
	Size          uint32
	Alignment     uint32
}

// setChunk is one frame of the set: copied from a package file (Path, Offset, Length) or held in memory (Frame).
type setChunk struct {
	Path         string
	Offset       int64
	Length       uint32
	Frame        []byte
	Decompressed uint32
}

func (c setChunk) size() uint32 {
	if c.Frame != nil {
		return uint32(len(c.Frame))
	}
	return c.Length
}

type setOverride struct {
	Key   key
	Bytes []byte
}

type setPlan struct {
	Records   []setRecord
	Chunks    []setChunk
	Overrides []setOverride
}

// writeSet writes the plan's set to w and returns its size.
func writeSet(w io.Writer, plan setPlan) (uint64, error) {
	recordTable := uint64(setHeaderBytes)
	chunkTable := recordTable + uint64(len(plan.Records))*setRecordBytes
	overrideTable := chunkTable + uint64(len(plan.Chunks))*setChunkBytes
	at := overrideTable + uint64(len(plan.Overrides))*setOverrideBytes
	frameAt := make([]uint64, len(plan.Chunks))
	for i, c := range plan.Chunks {
		frameAt[i] = at
		at += uint64(c.size())
	}
	overrideAt := make([]uint64, len(plan.Overrides))
	for i, o := range plan.Overrides {
		overrideAt[i] = at
		at += uint64(len(o.Bytes))
	}
	if at > maxSetBytes {
		return 0, fmt.Errorf("the set would be %d bytes; its offsets are u32, so it must stay under 4 GiB", at)
	}

	head := make([]byte, overrideTable+uint64(len(plan.Overrides))*setOverrideBytes)
	copy(head, setMagic)
	binary.LittleEndian.PutUint32(head[0x08:], setHeaderBytes)
	binary.LittleEndian.PutUint32(head[0x0c:], uint32(len(plan.Records)))
	binary.LittleEndian.PutUint32(head[0x10:], uint32(len(plan.Chunks)))
	binary.LittleEndian.PutUint32(head[0x14:], uint32(len(plan.Overrides)))
	binary.LittleEndian.PutUint64(head[0x18:], recordTable)
	binary.LittleEndian.PutUint64(head[0x20:], chunkTable)
	binary.LittleEndian.PutUint64(head[0x28:], overrideTable)
	for i, r := range plan.Records {
		p := head[recordTable+uint64(i)*setRecordBytes:]
		binary.LittleEndian.PutUint64(p[0:], r.Key.Type)
		binary.LittleEndian.PutUint64(p[8:], r.Key.Name)
		binary.LittleEndian.PutUint32(p[0x10:], r.Chunk)
		binary.LittleEndian.PutUint32(p[0x14:], r.OffsetInChunk)
		binary.LittleEndian.PutUint32(p[0x18:], r.Size)
		binary.LittleEndian.PutUint32(p[0x1c:], r.Alignment)
	}
	for i, c := range plan.Chunks {
		p := head[chunkTable+uint64(i)*setChunkBytes:]
		binary.LittleEndian.PutUint32(p[4:], uint32(frameAt[i]))
		binary.LittleEndian.PutUint32(p[8:], c.size())
		binary.LittleEndian.PutUint32(p[12:], c.Decompressed)
	}
	for i, o := range plan.Overrides {
		p := head[overrideTable+uint64(i)*setOverrideBytes:]
		binary.LittleEndian.PutUint64(p[0:], o.Key.Type)
		binary.LittleEndian.PutUint64(p[8:], o.Key.Name)
		binary.LittleEndian.PutUint32(p[0x10:], uint32(overrideAt[i]))
		binary.LittleEndian.PutUint32(p[0x14:], uint32(len(o.Bytes)))
	}
	if _, err := w.Write(head); err != nil {
		return 0, err
	}
	files := map[string]*os.File{}
	defer func() {
		for _, f := range files {
			f.Close()
		}
	}()
	for _, c := range plan.Chunks {
		if c.Frame != nil {
			if _, err := w.Write(c.Frame); err != nil {
				return 0, err
			}
			continue
		}
		f := files[c.Path]
		if f == nil {
			var err error
			if f, err = os.Open(c.Path); err != nil {
				return 0, err
			}
			files[c.Path] = f
		}
		n, err := io.Copy(w, io.NewSectionReader(f, c.Offset, int64(c.Length)))
		if err != nil {
			return 0, err
		}
		if n != int64(c.Length) {
			return 0, fmt.Errorf("%s: %d bytes at %d, wanted %d", c.Path, n, c.Offset, c.Length)
		}
	}
	for _, o := range plan.Overrides {
		if _, err := w.Write(o.Bytes); err != nil {
			return 0, err
		}
	}
	return at, nil
}

// validateSet checks a written set with the rules nevr_resource_set::Parse enforces, so the runtime takes what this
// writes. It reads the header and tables, and the first 4 bytes of each frame.
func validateSet(r io.ReaderAt, size uint64) error {
	if size < setHeaderBytes {
		return errors.New("too_small")
	}
	head := make([]byte, setHeaderBytes)
	if _, err := r.ReadAt(head, 0); err != nil {
		return err
	}
	if string(head[:8]) != string(setMagic) {
		return errors.New("magic")
	}
	if binary.LittleEndian.Uint32(head[8:]) != setHeaderBytes {
		return errors.New("header")
	}
	records := uint64(binary.LittleEndian.Uint32(head[0x0c:]))
	chunks := uint64(binary.LittleEndian.Uint32(head[0x10:]))
	overrides := uint64(binary.LittleEndian.Uint32(head[0x14:]))
	recordTable := binary.LittleEndian.Uint64(head[0x18:])
	chunkTable := binary.LittleEndian.Uint64(head[0x20:])
	overrideTable := binary.LittleEndian.Uint64(head[0x28:])
	if records == 0 {
		return errors.New("no_records")
	}
	inside := func(off, n uint64) bool { return off <= size && n <= size-off }
	if !inside(recordTable, records*setRecordBytes) || !inside(chunkTable, chunks*setChunkBytes) ||
		!inside(overrideTable, overrides*setOverrideBytes) {
		return errors.New("table_range")
	}
	read := func(off, n uint64) ([]byte, error) {
		b := make([]byte, n)
		_, err := r.ReadAt(b, int64(off))
		return b, err
	}
	ct, err := read(chunkTable, chunks*setChunkBytes)
	if err != nil {
		return err
	}
	decompressed := make([]uint32, chunks)
	for i := uint64(0); i < chunks; i++ {
		p := ct[i*setChunkBytes:]
		off := uint64(binary.LittleEndian.Uint32(p[4:]))
		n := uint64(binary.LittleEndian.Uint32(p[8:]))
		decompressed[i] = binary.LittleEndian.Uint32(p[12:])
		if n < 4 || !inside(off, n) {
			return fmt.Errorf("chunk_range at index %d", i)
		}
		magic, err := read(off, 4)
		if err != nil {
			return err
		}
		if binary.LittleEndian.Uint32(magic) != zstdMagic {
			return fmt.Errorf("chunk_not_zstd at index %d", i)
		}
		if decompressed[i] == 0 {
			return fmt.Errorf("chunk_size at index %d", i)
		}
	}
	rt, err := read(recordTable, records*setRecordBytes)
	if err != nil {
		return err
	}
	seen := map[key]bool{}
	for i := uint64(0); i < records; i++ {
		p := rt[i*setRecordBytes:]
		k := key{binary.LittleEndian.Uint64(p[0:]), binary.LittleEndian.Uint64(p[8:])}
		c := uint64(binary.LittleEndian.Uint32(p[0x10:]))
		off := uint64(binary.LittleEndian.Uint32(p[0x14:]))
		n := uint64(binary.LittleEndian.Uint32(p[0x18:]))
		align := binary.LittleEndian.Uint32(p[0x1c:])
		if c >= chunks {
			return fmt.Errorf("record_chunk at index %d", i)
		}
		d := uint64(decompressed[c])
		if off > d || n > d-off { // a record of 0 bytes is allowed: the game's manifest holds 2,515
			return fmt.Errorf("record_range at index %d", i)
		}
		if align == 0 || align&(align-1) != 0 {
			return fmt.Errorf("record_alignment at index %d", i)
		}
		if n <= blockLimit {
			if d > blockLimit {
				return fmt.Errorf("record_in_large_chunk at index %d", i)
			}
		} else if off != 0 || n != d {
			return fmt.Errorf("record_not_alone at index %d", i)
		}
		if seen[k] {
			return fmt.Errorf("duplicate at index %d", i)
		}
		seen[k] = true
	}
	ot, err := read(overrideTable, overrides*setOverrideBytes)
	if err != nil {
		return err
	}
	seenOverride := map[key]bool{}
	for i := uint64(0); i < overrides; i++ {
		p := ot[i*setOverrideBytes:]
		k := key{binary.LittleEndian.Uint64(p[0:]), binary.LittleEndian.Uint64(p[8:])}
		off := uint64(binary.LittleEndian.Uint32(p[0x10:]))
		n := uint64(binary.LittleEndian.Uint32(p[0x14:]))
		if n == 0 || !inside(off, n) {
			return fmt.Errorf("override_range at index %d", i)
		}
		if seenOverride[k] {
			return fmt.Errorf("override_duplicate at index %d", i)
		}
		if seen[k] {
			return fmt.Errorf("override_is_record at index %d", i)
		}
		seenOverride[k] = true
	}
	return nil
}

// verifySet decodes every frame of a set that validateSet passed and checks it holds the decompressed length its chunk
// states, then compares each record and override named in want with the bytes the set serves for it. It returns the
// frames decoded and the keys compared.
func verifySet(r io.ReaderAt, size uint64, dec *zstd.Decoder, want map[key][]byte) (int, int, error) {
	head := make([]byte, setHeaderBytes)
	if _, err := r.ReadAt(head, 0); err != nil {
		return 0, 0, err
	}
	records := uint64(binary.LittleEndian.Uint32(head[0x0c:]))
	chunks := uint64(binary.LittleEndian.Uint32(head[0x10:]))
	overrides := uint64(binary.LittleEndian.Uint32(head[0x14:]))
	table := func(off, n uint64) ([]byte, error) {
		b := make([]byte, n)
		_, err := r.ReadAt(b, int64(off))
		return b, err
	}
	rt, err := table(binary.LittleEndian.Uint64(head[0x18:]), records*setRecordBytes)
	if err != nil {
		return 0, 0, err
	}
	ct, err := table(binary.LittleEndian.Uint64(head[0x20:]), chunks*setChunkBytes)
	if err != nil {
		return 0, 0, err
	}
	ot, err := table(binary.LittleEndian.Uint64(head[0x28:]), overrides*setOverrideBytes)
	if err != nil {
		return 0, 0, err
	}
	byChunk := map[uint64][]int{}
	for i := uint64(0); i < records; i++ {
		c := uint64(binary.LittleEndian.Uint32(rt[i*setRecordBytes+0x10:]))
		byChunk[c] = append(byChunk[c], int(i))
	}
	compared := 0
	for c := uint64(0); c < chunks; c++ {
		p := ct[c*setChunkBytes:]
		frame, err := table(uint64(binary.LittleEndian.Uint32(p[4:])), uint64(binary.LittleEndian.Uint32(p[8:])))
		if err != nil {
			return 0, 0, err
		}
		stated := binary.LittleEndian.Uint32(p[12:])
		plain, err := dec.DecodeAll(frame, make([]byte, 0, stated))
		if err != nil {
			return 0, 0, fmt.Errorf("chunk %d does not decode: %w", c, err)
		}
		if uint32(len(plain)) != stated {
			return 0, 0, fmt.Errorf("chunk %d decodes to %d bytes, its entry says %d", c, len(plain), stated)
		}
		for _, i := range byChunk[c] {
			q := rt[uint64(i)*setRecordBytes:]
			k := key{binary.LittleEndian.Uint64(q[0:]), binary.LittleEndian.Uint64(q[8:])}
			w, ok := want[k]
			if !ok {
				continue
			}
			off := binary.LittleEndian.Uint32(q[0x14:])
			if !bytes.Equal(plain[off:off+binary.LittleEndian.Uint32(q[0x18:])], w) {
				return 0, 0, fmt.Errorf("record %s: the set serves other bytes than the export holds", k)
			}
			compared++
		}
	}
	for i := uint64(0); i < overrides; i++ {
		q := ot[i*setOverrideBytes:]
		k := key{binary.LittleEndian.Uint64(q[0:]), binary.LittleEndian.Uint64(q[8:])}
		w, ok := want[k]
		if !ok {
			continue
		}
		got, err := table(uint64(binary.LittleEndian.Uint32(q[0x10:])), uint64(binary.LittleEndian.Uint32(q[0x14:])))
		if err != nil {
			return 0, 0, err
		}
		if !bytes.Equal(got, w) {
			return 0, 0, fmt.Errorf("override %s: the set holds other bytes than the export", k)
		}
		compared++
	}
	return int(chunks), compared, nil
}
