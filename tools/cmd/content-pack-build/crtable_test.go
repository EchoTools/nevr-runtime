package main

// The component-table reader (crtable.go) over synthetic tables of one array; no byte of the game's is used.

import (
	"strings"
	"testing"
)

// A table of one array, to place the padding between a root and its array's data. An array whose element size is a
// multiple of 16 is aligned to 8 bytes, any other to 4 (cr_table.py:68-70).
func miniTable(rootSize, esize int, pad byte) []byte {
	root := make([]byte, rootSize)
	putHeader(root, 0, 1, esize)
	out := root
	for len(out)%padTarget(esize) != 0 {
		out = append(out, pad)
	}
	return append(out, make([]byte, esize)...)
}

func padTarget(esize int) int {
	if esize%crWideElement == 0 {
		return crPadWide
	}
	return crPadNarrow
}

func TestTheCRTableReaderAlignsArrayDataAndRefusesAPaddingItCannotWriteBack(t *testing.T) {
	const odd = crHeaderLen + 1 // a root that ends 1 byte past a 4-byte boundary after its header
	for _, esize := range []int{4, crWideElement} {
		root, err := crParse(miniTable(odd, esize, 0), odd)
		if err != nil {
			t.Fatalf("elements of %d bytes: %v", esize, err)
		}
		if len(root.arrays) != 1 || root.arrays[0].count != 1 || root.arrays[0].esize != esize {
			t.Errorf("elements of %d bytes: read as %+v", esize, root.arrays)
		}
		if _, err := crParse(miniTable(odd, esize, 0xff), odd); err == nil || !strings.Contains(err.Error(), "write back") {
			t.Errorf("elements of %d bytes with nonzero padding: err %v, want a refusal that the table does not write back", esize, err)
		}
	}
}
