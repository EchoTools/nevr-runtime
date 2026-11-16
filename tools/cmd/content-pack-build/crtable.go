package main

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
)

// A component table whose root holds several arrays (CModelCRWin10's level table, ea51a0d76eb90142): a RAD serialized
// struct. Layout as he_is_the_cat's tools/cr_table.py reads it (82ad7ba; its header comment says 0x38-byte headers and
// lists a +30 u64, but its code steps over 0x30 bytes, cr_table.py:48, :58, :62): each array sits inline in its struct as
// a 0x30-byte header
//
//	+00 u64 byte size  +08 u64 0 (a pointer)  +10 u32 0  +14 u32 1  +18 u64 alignment (0 or 0x20)  +20 u64 count  +28 u64 capacity
//
// and the data is not addressed: after the root struct each array's elements follow in field order, each element's own
// arrays right after the array's data (depth first). The one-array tables (transforms, texture streaming) are
// radpack.RowTable; this reader is for the tables whose elements carry arrays of their own. A parse is accepted only
// when it writes back byte for byte (cr_table.py:91-93 does the same check), so a layout this reader mistakes is refused.
const (
	crHeaderLen    = 0x30 // an array header
	crHeaderStep   = 8    // the scan for headers inside a struct moves by this much
	crSizeOff      = 0x00
	crPointerOff   = 0x08
	crZeroOff      = 0x10
	crOneOff       = 0x14
	crAlignOff     = 0x18
	crCountOff     = 0x20
	crCapacityOff  = 0x28
	crAlignWide    = 0x20 // the other alignment value a header may carry
	crWideElement  = 16   // an element size that is a multiple of this aligns its array's data to crPadWide bytes
	crPadWide      = 8
	crPadNarrow    = 4
	crBitsetWord   = 8 // a bitset array's element: one u64
	crBitsPerWord  = 64
	crModelRoot    = 0x168 // CModelCRWin10's root struct (fix_dot_field.py:57)
	crModelRecords = 0     // root arrays of a model table, by index (fix_dot_field.py:142-143)
	crModelBitsA   = 1
	crModelEntries = 2
	crModelBitsB   = 3
	crModelBitsC   = 4
	crModelInsts   = 5
	crModelArrays  = 6
)

type crNode struct {
	raw    []byte // the struct's own bytes, array headers included
	arrays []*crArr
}

type crArr struct {
	at, count, esize int // header offset in its struct, element count, element size
	elems            []*crNode
}

func crIsHeader(b []byte, o, end int) bool {
	if o+crHeaderLen > end {
		return false
	}
	le := binary.LittleEndian
	size, ptr, zero, one := le.Uint64(b[o+crSizeOff:]), le.Uint64(b[o+crPointerOff:]), le.Uint32(b[o+crZeroOff:]), le.Uint32(b[o+crOneOff:])
	align, count, capacity := le.Uint64(b[o+crAlignOff:]), le.Uint64(b[o+crCountOff:]), le.Uint64(b[o+crCapacityOff:])
	return ptr == 0 && zero == 0 && one == 1 && (align == 0 || align == crAlignWide) && count == capacity && size < uint64(len(b)) &&
		(count == 0) == (size == 0) && (count == 0 || size%count == 0)
}

func crHeaders(b []byte, start, size int) []*crArr {
	var out []*crArr
	for o := start; o+crHeaderLen <= start+size; {
		if crIsHeader(b, o, start+size) {
			size64 := binary.LittleEndian.Uint64(b[o+crSizeOff:])
			count := binary.LittleEndian.Uint64(b[o+crCountOff:])
			a := &crArr{at: o - start, count: int(count)}
			if count != 0 {
				a.esize = int(size64 / count)
			}
			out = append(out, a)
			o += crHeaderLen
		} else {
			o += crHeaderStep
		}
	}
	return out
}

func crAlign(x, esize int) int {
	n := crPadNarrow
	if esize%crWideElement == 0 {
		n = crPadWide
	}
	return (x + n - 1) &^ (n - 1)
}

func crWalk(b []byte, at, size, pos int) (*crNode, int, error) {
	if at < 0 || size < 0 || at+size > len(b) {
		return nil, 0, fmt.Errorf("a struct of %d bytes at %#x runs past the table's %#x", size, at, len(b))
	}
	n := &crNode{raw: append([]byte(nil), b[at:at+size]...)}
	n.arrays = crHeaders(b, at, size)
	for _, a := range n.arrays {
		if a.count == 0 {
			continue
		}
		pos = crAlign(pos, a.esize)
		first := pos
		pos += a.count * a.esize
		if pos > len(b) {
			return nil, 0, fmt.Errorf("an array of %d elements of %d bytes at %#x runs past the table's %#x", a.count, a.esize, first, len(b))
		}
		for i := 0; i < a.count; i++ {
			e, next, err := crWalk(b, first+i*a.esize, a.esize, pos)
			if err != nil {
				return nil, 0, err
			}
			pos = next
			a.elems = append(a.elems, e)
		}
	}
	return n, pos, nil
}

// crParse reads a table whose root struct is rootSize bytes.
func crParse(b []byte, rootSize int) (*crNode, error) {
	root, end, err := crWalk(b, 0, rootSize, rootSize)
	if err != nil {
		return nil, err
	}
	if end != len(b) {
		return nil, fmt.Errorf("the table walk ended at %#x, the table is %#x bytes", end, len(b))
	}
	if !bytes.Equal(crBuild(root), b) {
		return nil, errors.New("the table does not write back as it was read")
	}
	return root, nil
}

// crBuild writes the table: the root, then each array's data and each element's own arrays after it.
func crBuild(root *crNode) []byte {
	out := append([]byte(nil), root.raw...)
	var emit func(n *crNode)
	emit = func(n *crNode) {
		for _, a := range n.arrays {
			if a.count == 0 {
				continue
			}
			for len(out) != crAlign(len(out), a.esize) {
				out = append(out, 0)
			}
			for _, e := range a.elems {
				out = append(out, e.raw...)
			}
			for _, e := range a.elems {
				emit(e)
			}
		}
	}
	emit(root)
	return out
}

func crCopy(n *crNode) *crNode {
	c := &crNode{raw: append([]byte(nil), n.raw...)}
	for _, a := range n.arrays {
		ca := &crArr{at: a.at, count: a.count, esize: a.esize}
		for _, e := range a.elems {
			ca.elems = append(ca.elems, crCopy(e))
		}
		c.arrays = append(c.arrays, ca)
	}
	return c
}

// crAdd appends elem to the root's array `index`, with its byte size, count and capacity set in the root's header.
func crAdd(root *crNode, index int, elem *crNode) error {
	a := root.arrays[index]
	if a.esize == 0 || len(elem.raw) != a.esize {
		return fmt.Errorf("array %d has elements of %d bytes, the new one is %d", index, a.esize, len(elem.raw))
	}
	a.elems = append(a.elems, elem)
	a.count = len(a.elems)
	le := binary.LittleEndian
	le.PutUint64(root.raw[a.at+crSizeOff:], uint64(a.count*a.esize))
	le.PutUint64(root.raw[a.at+crCountOff:], uint64(a.count))
	le.PutUint64(root.raw[a.at+crCapacityOff:], uint64(a.count))
	return nil
}
