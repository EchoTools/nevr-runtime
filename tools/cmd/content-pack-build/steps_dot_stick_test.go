package main

// The DOT stick-on-contact step over synthetic CR15NetMagazineCR tables with the layout fix_dot_stick.py reads (a 0x38
// header, 40-byte rows, no tail); no row of the game's is used.

import (
	"bytes"
	"encoding/binary"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

func magazineTable(rows ...[]byte) []byte {
	head := make([]byte, magazineHead)
	binary.LittleEndian.PutUint64(head[magazineBytesOff:], uint64(len(rows)*magazineRow))
	binary.LittleEndian.PutUint64(head[magazineCountOff:], uint64(len(rows)))
	head[0x20] = 0xee // a header byte the step must keep
	out := head
	for _, r := range rows {
		out = append(out, r...)
	}
	return out
}

func magazineRec(actor uint64, flags uint32, other uint32) []byte {
	r := make([]byte, magazineRow)
	binary.LittleEndian.PutUint64(r[keyOff:], actor)
	binary.LittleEndian.PutUint32(r[magazineFlagsOff:], flags)
	binary.LittleEndian.PutUint32(r[0x20:], other)
	return r
}

func TestTheDotMagazineLosesTheStickBitAndNothingElse(t *testing.T) {
	in := magazineTable(
		magazineRec(0x3d8b8ca4d5292926, 0x10, 1), // the wall shield keeps its bit
		magazineRec(dotMagazineActor, 0x1f, 0),   // the DOT: bit 0x10 and other flag bits
		magazineRec(0x1111, 0x0, 0))
	out, err := dotStick(in)
	if err != nil {
		t.Fatal(err)
	}
	if len(out) != len(in) {
		t.Fatalf("length %d -> %d", len(in), len(out))
	}
	want := append([]byte(nil), in...)
	binary.LittleEndian.PutUint32(want[magazineHead+magazineRow+magazineFlagsOff:], 0x0f)
	if !bytes.Equal(out, want) {
		t.Errorf("only the DOT record's bit 0x10 may change:\n got %x\nwant %x", out, want)
	}
	if bytes.Equal(out, in) {
		t.Error("the table is unchanged")
	}
}

func TestADotTableThatIsNotAsExpectedIsRefused(t *testing.T) {
	good := magazineRec(dotMagazineActor, 0x10, 0)
	short := magazineTable(good)[:magazineHead+magazineRow-1]
	tail := append(magazineTable(good), 0)
	badCount := magazineTable(good)
	binary.LittleEndian.PutUint64(badCount[magazineCountOff:], 2)
	cases := []struct {
		name string
		blob []byte
		want string
	}{
		{"no DOT record", magazineTable(magazineRec(0x1, 0x10, 0)), "found 0"},
		{"two DOT records", magazineTable(good, good), "found 2"},
		{"the bit is already clear", magazineTable(magazineRec(dotMagazineActor, 0x0f, 0)), "want bit 0x10 set"},
		{"a tail after the rows", tail, "framing"},
		{"a count that does not fit", badCount, "framing"},
		{"shorter than its rows", short, "framing"},
		{"shorter than the header", []byte{1, 2, 3}, "header"},
	}
	for _, c := range cases {
		if _, err := dotStick(c.blob); err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: %v, want an error naming %q", c.name, err, c.want)
		}
	}
}

func TestTheDotStickStepChangesTheFiveCombatLevelsAndNoOther(t *testing.T) {
	const other = 0x1234
	res := map[radpack.Key][]byte{}
	for _, lvl := range append(append([]uint64(nil), droneLevels...), other) {
		res[radpack.Key{Type: int64(magazineTableType), Name: int64(lvl)}] = magazineTable(magazineRec(dotMagazineActor, 0x10, 0))
	}
	data := t.TempDir()
	if err := radpack.WriteStockSet(data, Sets[0], []map[radpack.Key][]byte{res}); err != nil {
		t.Fatal(err)
	}
	s, _, err := radpack.OpenSet(data, Sets[0])
	if err != nil {
		t.Fatal(err)
	}
	p := &Pack{sets: map[string]*radpack.Set{Sets[0]: s}, changes: map[string]map[radpack.Key][]byte{}, scripts: map[string][]byte{}}
	n := 0
	for _, st := range steps {
		if st.Name != "DOT magazine does not stick on contact" {
			continue
		}
		n++
		if err := st.Apply(p); err != nil {
			t.Fatal(err)
		}
	}
	if n != 1 {
		t.Fatalf("%d steps of that name, want 1", n)
	}
	got := p.changes[Sets[0]]
	if len(got) != len(droneLevels) {
		t.Errorf("%d resources changed, want %d", len(got), len(droneLevels))
	}
	for _, lvl := range droneLevels {
		b := got[radpack.Key{Type: int64(magazineTableType), Name: int64(lvl)}]
		if b == nil || binary.LittleEndian.Uint32(b[magazineHead+magazineFlagsOff:])&stickBit != 0 {
			t.Errorf("level %016x: the stick bit is still set or the table was not changed", lvl)
		}
	}
	if _, ok := got[radpack.Key{Type: int64(magazineTableType), Name: other}]; ok {
		t.Error("a level that is not a combat level was changed")
	}
}

func TestTheDotFieldReadsTheStickBitTheStepClears(t *testing.T) {
	in := magazineTable(
		magazineRec(0x3d8b8ca4d5292926, 0x10, 1),
		magazineRec(dotMagazineActor, 0x1f, 0),
		magazineRec(0x1111, 0x0, 0))
	if got, err := readDotStickBit(in); err != nil || !bytes.Equal(got, []byte{stickBit}) {
		t.Errorf("stock: %x, %v, want %x", got, err, []byte{stickBit})
	}
	out, err := dotStick(in)
	if err != nil {
		t.Fatal(err)
	}
	if got, err := readDotStickBit(out); err != nil || !bytes.Equal(got, []byte{0}) {
		t.Errorf("after the step: %x, %v, want 00", got, err)
	}
	// Bytes outside the declared bit are not the field: the DOT record's other flag bits, another record's bit 0x10.
	other := append([]byte(nil), out...)
	binary.LittleEndian.PutUint32(other[magazineHead+magazineRow+magazineFlagsOff:], 0x0e)
	binary.LittleEndian.PutUint32(other[magazineHead+magazineFlagsOff:], 0)
	if got, err := readDotStickBit(other); err != nil || !bytes.Equal(got, []byte{0}) {
		t.Errorf("other bits changed: %x, %v, want 00", got, err)
	}
	for name, blob := range map[string][]byte{
		"no DOT record":       magazineTable(magazineRec(0x1, 0x10, 0)),
		"a tail after a row":  append(magazineTable(magazineRec(dotMagazineActor, 0x10, 0)), 0),
		"shorter than a head": {1, 2, 3},
	} {
		if got, err := readDotStickBit(blob); err == nil {
			t.Errorf("%s: read %x", name, got)
		}
	}
}
