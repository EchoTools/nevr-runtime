package main

// The DOT dampener step over synthetic tables with the shipped layout; no row of the game's is used.

import (
	"bytes"
	"encoding/binary"
	"math"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

const dotRowSize = 0x50 // a synthetic row size; the real tables' sizes are not needed

func dotRow(actor uint64, enabled uint32, damp float32, move [3]float32) []byte {
	r := row(dotRowSize, map[int]uint64{keyOff: actor, 0x00: 0x77})
	binary.LittleEndian.PutUint32(r[dampenerEnabledOff:], enabled)
	setF32(r, dampenerDampingOff, damp)
	setF32(r, dampenerDampingOff+floatLen, damp)
	for i, v := range move {
		setF32(r, transformMoveOff+i*floatLen, v)
	}
	setF32(r, transformMoveEnd, 1) // w stays 1
	return r
}

var stockMove = [3]float32{stockMoveX, 0, stockMoveZ}

func TestTheDotDampenerStartsOffWithTheArcsDamping(t *testing.T) {
	other := dotRow(0x1234, 1, stockDampening, [3]float32{})
	tb := parse(t, table(dotRowSize, []byte("tail"), other, dotRow(dotMagazineActor, 1, stockDampening, stockMove)))
	if err := dotDampener(tb); err != nil {
		t.Fatal(err)
	}
	r := parse(t, tb.Bytes()).Rows[1]
	if binary.LittleEndian.Uint32(r[dampenerEnabledOff:]) != 0 || f32(r, dampenerDampingOff) != arcDamping || f32(r, dampenerDampingOff+floatLen) != arcDamping {
		t.Errorf("DOT row %x", r)
	}
	if !bytes.Equal(tb.Rows[0], other) || !bytes.Equal(tb.Tail, []byte("tail")) {
		t.Error("another actor's row or the tail changed")
	}
	// Only the three declared fields change.
	want := dotRow(dotMagazineActor, 0, arcDamping, stockMove)
	if !bytes.Equal(r, want) {
		t.Errorf("row differs beyond the declared fields: %x want %x", r, want)
	}
}

func TestADotDampenerRowThatIsNotStockIsRefused(t *testing.T) {
	cases := map[string][]byte{
		"already off":    dotRow(dotMagazineActor, 0, stockDampening, stockMove),
		"other damping":  dotRow(dotMagazineActor, 1, 500, stockMove),
		"no DOT row":     dotRow(0x1234, 1, stockDampening, stockMove),
		"two DOT rows":   nil,
		"rows too short": nil,
	}
	for name, r := range cases {
		var tb *radpack.RowTable
		switch name {
		case "two DOT rows":
			tb = parse(t, table(dotRowSize, nil, dotRow(dotMagazineActor, 1, stockDampening, stockMove), dotRow(dotMagazineActor, 1, stockDampening, stockMove)))
		case "rows too short":
			tb = parse(t, table(dampenerEnd-1, nil, row(dampenerEnd-1, map[int]uint64{keyOff: dotMagazineActor})))
		default:
			tb = parse(t, table(dotRowSize, nil, r))
		}
		before := tb.Bytes()
		if err := dotDampener(tb); err == nil {
			t.Errorf("%s: changed", name)
		} else if name == "other damping" && !strings.Contains(err.Error(), "stock is") {
			t.Errorf("%s: the error does not name the stock value: %v", name, err)
		}
		if !bytes.Equal(tb.Bytes(), before) {
			t.Errorf("%s: the table changed though it was refused", name)
		}
	}
}

func TestEveryDotNodeTranslationIsZeroedAndNoOtherNodes(t *testing.T) {
	keep := dotRow(0x1234, 1, 1, [3]float32{5, 6, 7})
	tb := parse(t, table(dotRowSize, nil, dotRow(dotMagazineActor, 1, 1, stockMove), keep, dotRow(dotMagazineActor, 1, 1, stockMove)))
	if err := dotTransforms(tb); err != nil {
		t.Fatal(err)
	}
	for _, i := range []int{0, 2} {
		r := tb.Rows[i]
		if !bytes.Equal(r, dotRow(dotMagazineActor, 1, 1, [3]float32{})) || f32(r, transformMoveEnd) != 1 {
			t.Errorf("row %d: %x", i, r)
		}
	}
	if !bytes.Equal(tb.Rows[1], keep) {
		t.Error("another actor's node changed")
	}
}

func TestADotNodeAwayFromTheStockTranslationIsRefused(t *testing.T) {
	for name, move := range map[string][3]float32{
		"zero":    {},
		"x moved": {-1, 0, stockMoveZ},
		"y set":   {stockMoveX, 0.5, stockMoveZ},
		"nan":     {float32(math.NaN()), 0, stockMoveZ},
	} {
		tb := parse(t, table(dotRowSize, nil, dotRow(dotMagazineActor, 1, 1, move)))
		if err := dotTransforms(tb); err == nil || !strings.Contains(err.Error(), "stock is") {
			t.Errorf("%s: %v", name, err)
		}
	}
	if err := dotTransforms(parse(t, table(dotRowSize, nil, dotRow(0x1234, 1, 1, stockMove)))); err == nil {
		t.Error("a table with no DOT node was changed")
	}
	if err := dotTransforms(parse(t, table(transformMoveEnd-1, nil, row(transformMoveEnd-1, map[int]uint64{keyOff: dotMagazineActor})))); err == nil {
		t.Error("rows ending before the translation were read")
	}
}

func TestTheDotDampStepChangesTheFiveCombatLevelsAndNoOther(t *testing.T) {
	const other = 0x1234
	res := map[radpack.Key][]byte{}
	var tt uint64 = transformType // not a constant conversion: the id overflows int64
	for _, lvl := range append(append([]uint64(nil), droneLevels...), other) {
		res[radpack.Key{Type: int64(dampenerTable), Name: int64(lvl)}] = table(dotRowSize, nil, dotRow(dotMagazineActor, 1, stockDampening, stockMove))
		res[radpack.Key{Type: int64(tt), Name: int64(lvl)}] = table(dotRowSize, nil, dotRow(dotMagazineActor, 1, stockDampening, stockMove))
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
	if err := dotDampApply(p); err != nil {
		t.Fatal(err)
	}
	got := p.changes[Sets[0]]
	if len(got) != 10 {
		t.Errorf("%d resources changed, want 10 (2 tables x 5 levels)", len(got))
	}
	for _, lvl := range droneLevels {
		d := got[radpack.Key{Type: int64(dampenerTable), Name: int64(lvl)}]
		x := got[radpack.Key{Type: int64(tt), Name: int64(lvl)}]
		if d == nil || x == nil {
			t.Fatalf("level %016x: a table was not written", lvl)
		}
		if !bytes.Equal(parse(t, d).Rows[0], dotRow(dotMagazineActor, 0, arcDamping, stockMove)) {
			t.Errorf("level %016x: dampener row", lvl)
		}
		if !bytes.Equal(parse(t, x).Rows[0], dotRow(dotMagazineActor, 1, stockDampening, [3]float32{})) {
			t.Errorf("level %016x: transform row", lvl)
		}
	}
	if _, ok := got[radpack.Key{Type: int64(dampenerTable), Name: other}]; ok {
		t.Error("a level that is not a combat level was changed")
	}
}

func TestTheDotDampFieldsReadWhatTheStepWrites(t *testing.T) {
	const other = 0x1234
	otherMove := [3]float32{5, 6, 7}
	damp := parse(t, table(dotRowSize, nil, dotRow(other, 1, stockDampening, otherMove), dotRow(dotMagazineActor, 1, stockDampening, stockMove)))
	if err := dotDampener(damp); err != nil {
		t.Fatal(err)
	}
	got, err := readDotDampener(damp.Bytes())
	want := make([]byte, u32Len+2*floatLen) // enabled 0, then the two damping values
	setF32(want, u32Len, arcDamping)
	setF32(want, u32Len+floatLen, arcDamping)
	if err != nil || !bytes.Equal(got, want) {
		t.Errorf("dampener field %x, want %x (%v)", got, want, err)
	}
	if _, err := readDotDampener(table(dotRowSize, nil, dotRow(other, 1, stockDampening, stockMove))); err == nil {
		t.Error("a dampener table with no DOT row was read")
	}

	moves := parse(t, table(dotRowSize, nil, dotRow(dotMagazineActor, 1, 1, stockMove), dotRow(other, 1, 1, otherMove),
		dotRow(dotMagazineActor, 1, 1, stockMove)))
	before, err := readDotTranslations(moves.Bytes())
	if err != nil || len(before) != 2*(transformMoveEnd-transformMoveOff) || f32(before, 0) == 0 {
		t.Errorf("stock translations %x, %v", before, err)
	}
	if err := dotTransforms(moves); err != nil {
		t.Fatal(err)
	}
	after, err := readDotTranslations(moves.Bytes())
	if err != nil || !bytes.Equal(after, make([]byte, 2*(transformMoveEnd-transformMoveOff))) {
		t.Errorf("translations %x, want two zeroed (%v)", after, err)
	}
	if _, err := readDotTranslations(table(transformMoveEnd-1, nil, row(transformMoveEnd-1, map[int]uint64{keyOff: dotMagazineActor}))); err == nil {
		t.Error("rows ending before the translation were read")
	}
}
