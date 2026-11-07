package radpack

import (
	"encoding/binary"
	"errors"
	"fmt"
)

// RowTable is a component-table resource (the level tables the EchoCombat pack changes: bullets 4fd65c8d2b63d470,
// particle effects 21a09c2016d8f3e6, scripts d99f6bbd8009c92c): a 0x38-byte header, `count` rows of one size back to
// back, and a tail the rows do not include. Header, measured on the shipped tables: +0x08 the rows' byte size (count
// times the row size), +0x28 the capacity, +0x30 the count; every other header byte is kept. The tail is kept as it is.
type RowTable struct {
	Head []byte
	Rows [][]byte
	Size int // bytes per row
	Tail []byte
}

const rowHeader = 0x38

// ParseRowTable reads a component table.
func ParseRowTable(b []byte) (*RowTable, error) {
	if len(b) < rowHeader {
		return nil, errors.New("row table shorter than its header")
	}
	le := binary.LittleEndian
	nbytes, capacity, count := le.Uint64(b[0x08:]), le.Uint64(b[0x28:]), le.Uint64(b[0x30:])
	if count == 0 || capacity != count || nbytes%count != 0 || nbytes > uint64(len(b))-rowHeader {
		return nil, fmt.Errorf("row table header does not fit: %d bytes, capacity %d, count %d, table %d", nbytes, capacity, count, len(b))
	}
	t := &RowTable{Head: append([]byte(nil), b[:rowHeader]...), Size: int(nbytes / count), Tail: append([]byte(nil), b[rowHeader+nbytes:]...)}
	for i := uint64(0); i < count; i++ {
		at := rowHeader + int(i)*t.Size
		t.Rows = append(t.Rows, append([]byte(nil), b[at:at+t.Size]...))
	}
	return t, nil
}

// Bytes encodes the table with its count, capacity and byte size set from its rows.
func (t *RowTable) Bytes() []byte {
	le := binary.LittleEndian
	head := append([]byte(nil), t.Head...)
	n := uint64(len(t.Rows))
	le.PutUint64(head[0x08:], n*uint64(t.Size))
	le.PutUint64(head[0x28:], n)
	le.PutUint64(head[0x30:], n)
	out := head
	for _, r := range t.Rows {
		out = append(out, r...)
	}
	return append(out, t.Tail...)
}

// U64 is the row's little-endian u64 at `off`.
func U64(row []byte, off int) uint64 { return binary.LittleEndian.Uint64(row[off:]) }

// SetU64 writes v at `off` of the row.
func SetU64(row []byte, off int, v uint64) { binary.LittleEndian.PutUint64(row[off:], v) }

// Find is the index of the first row whose u64 at `off` is v, or -1.
func (t *RowTable) Find(off int, v uint64) int {
	for i, r := range t.Rows {
		if off+8 <= len(r) && U64(r, off) == v {
			return i
		}
	}
	return -1
}
