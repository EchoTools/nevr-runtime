package main

// The DOT FIELD red bubble's texture-streaming and material steps over synthetic records (no byte of the game's):
// stock_steps_test.go runs them over the shipped data when a machine has it.

import (
	"bytes"
	"encoding/binary"
	"math"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

const (
	texRowSize      = 0x50 // a synthetic record long enough for the model id at +0x20 and the probes below
	texProbeOff     = 0x10 // a u64 of the arc's record the copy keeps
	texProbeByteOff = 0x40 // a byte of the arc's record the copy keeps
	texProbe        = 0x77
	texProbeByte    = 9
	texOtherID      = 0x1234
	texOtherActor   = 0x5678
	texWrongModel   = 0x99
	texShortRow     = 0x20 // ends before the model id
	materialSize    = 0x200
	materialFill    = 0xab
	notBlue         = 0.5
)

// stockBlue is a light blue the step accepts (b 1, a 1, g above r).
var stockBlue = [colourFloats]float32{0.06, 0.48, 1, 1}

func TestTheDotFieldTexMatNamesAreTheirCSymbol64(t *testing.T) {
	for name, want := range map[string]uint64{
		"CTextureStreamingCRWin10": texStreamTableType,
	} {
		if got := csymbol64(name); got != want {
			t.Errorf("CSymbol64(%q) = %016x, the step uses %016x", name, got, want)
		}
	}
	for _, m := range dotFieldMaterials {
		if got := csymbol64(m.name); got != m.red {
			t.Errorf("CSymbol64(%q) = %016x, the step uses %016x", m.name, got, m.red)
		}
	}
}

func texTable(rows ...[]byte) []byte { return table(texRowSize, nil, rows...) }

func arcTexRow() []byte {
	r := row(texRowSize, map[int]uint64{idOff: arcBubble, keyOff: arcActor, texProbeOff: texProbe, texModelOff: arcModel})
	r[texProbeByteOff] = texProbeByte
	return r
}

func TestTheDotFieldTexStreamRecordIsACopyOfTheArcBubblesForTheDot(t *testing.T) {
	other := row(texRowSize, map[int]uint64{idOff: texOtherID, keyOff: texOtherActor})
	tb := parse(t, texTable(other, arcTexRow()))
	if err := dotFieldTexStreamRow(tb); err != nil {
		t.Fatal(err)
	}
	out := parse(t, tb.Bytes())
	if len(out.Rows) != 3 {
		t.Fatalf("%d rows, want 3", len(out.Rows))
	}
	n := out.Rows[2]
	if radpack.U64(n, idOff) != fieldComponent || radpack.U64(n, keyOff) != dotMagazineActor || radpack.U64(n, texModelOff) != redModel {
		t.Errorf("new row ids %x", n[:texModelOff+u64Len])
	}
	if radpack.U64(n, texProbeOff) != texProbe || n[texProbeByteOff] != texProbeByte {
		t.Error("the copy lost a field of the arc bubble's record")
	}
	if !bytes.Equal(out.Rows[0], other) || !bytes.Equal(out.Rows[1], arcTexRow()) {
		t.Error("an existing row changed: the arc keeps its own record")
	}
	// The declared field reads exactly the new row.
	if got, err := readFieldTexStream(tb.Bytes()); err != nil || !bytes.Equal(got, n) {
		t.Errorf("declared field %x, %v; want the new row %x", got, err, n)
	}
	if _, err := readFieldTexStream(texTable(other, arcTexRow())); err == nil || !strings.Contains(err.Error(), "no FIELD texture streaming row") {
		t.Errorf("a stock table's declared field: %v", err)
	}
	// Refusals: the stock value differs, or the edit was already made.
	bad := arcTexRow()
	radpack.SetU64(bad, texModelOff, texWrongModel)
	for name, c := range map[string]struct {
		b    []byte
		want string
	}{
		"already there":    {texTable(arcTexRow(), row(texRowSize, map[int]uint64{idOff: fieldComponent, keyOff: dotMagazineActor})), "already"},
		"no arc record":    {texTable(other), "0 arc bubble"},
		"two arc records":  {texTable(arcTexRow(), arcTexRow()), "2 arc bubble"},
		"different model":  {texTable(bad), "model id"},
		"rows too short":   {table(texShortRow, nil, row(texShortRow, map[int]uint64{idOff: arcBubble, keyOff: arcActor})), "end before"},
		"bytes after rows": {table(texRowSize, []byte("tail"), arcTexRow()), "after its rows"},
	} {
		err := dotFieldTexStreamRow(parse(t, c.b))
		if err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: error %v, want one naming %q", name, err, c.want)
		}
	}
}

// blueMaterial is a synthetic material whose colours are the arc's light blue at the offsets of m.
func blueMaterial(m dotFieldMaterial) []byte {
	b := bytes.Repeat([]byte{materialFill}, materialSize)
	for _, c := range m.colours {
		for i, v := range stockBlue {
			binary.LittleEndian.PutUint32(b[c.off+floatLen*i:], math.Float32bits(v))
		}
	}
	return b
}

func TestTheDotFieldRedMaterialsTakeTheRedColoursAndNothingElse(t *testing.T) {
	for _, m := range dotFieldMaterials {
		src := blueMaterial(m)
		out, err := redMaterial(src, m)
		if err != nil {
			t.Fatal(err)
		}
		if len(out) != len(src) {
			t.Fatalf("length %d, want %d", len(out), len(src))
		}
		var want []byte
		for _, c := range m.colours {
			for i, v := range c.rgba {
				if got := math.Float32frombits(binary.LittleEndian.Uint32(out[c.off+floatLen*i:])); got != v {
					t.Errorf("material %x +%#x[%d] = %v, want %v", m.arc, c.off, i, got, v)
				}
				want = binary.LittleEndian.AppendUint32(want, math.Float32bits(v))
			}
		}
		diff := 0
		for i := range src {
			if src[i] != out[i] {
				diff++
			}
		}
		if diff == 0 || diff > len(m.colours)*colourLen {
			t.Errorf("material %x: %d bytes differ", m.arc, diff)
		}
		// The declared field reads the red colours of the copy, and the blue ones of the stock material.
		if got, err := readRedColours(m)(out); err != nil || !bytes.Equal(got, want) {
			t.Errorf("material %x: declared field %x, %v; want %x", m.arc, got, err, want)
		}
		if got, _ := readRedColours(m)(src); bytes.Equal(got, want) {
			t.Errorf("material %x: the declared field reads the stock material as red", m.arc)
		}
		if _, err := readRedColours(m)(src[:m.colours[len(m.colours)-1].off+floatLen]); err == nil {
			t.Errorf("material %x: a short material's declared field was read", m.arc)
		}
		blue := append([]byte(nil), src...)
		binary.LittleEndian.PutUint32(blue[m.colours[0].off+2*floatLen:], math.Float32bits(notBlue)) // b != 1
		if _, err := redMaterial(blue, m); err == nil || !strings.Contains(err.Error(), "no light blue") {
			t.Errorf("material %x: a colour that is not light blue was changed: %v", m.arc, err)
		}
		if _, err := redMaterial(src[:m.colours[0].off+floatLen], m); err == nil {
			t.Errorf("material %x: a short material was changed", m.arc)
		}
	}
}

// The two steps through a pack built from a synthetic stock set: one table per combat level and two new materials.
func TestTheDotFieldTexMatStepsChangeTheLevelTablesAndTwoMaterials(t *testing.T) {
	var texType, matType uint64 = texStreamTableType, materialType // not constants: the ids overflow a constant int64 conversion
	res := map[radpack.Key][]byte{}
	for _, lvl := range droneLevels {
		res[radpack.Key{Type: int64(texType), Name: int64(lvl)}] = texTable(arcTexRow())
	}
	for _, m := range dotFieldMaterials {
		res[radpack.Key{Type: int64(matType), Name: int64(m.arc)}] = blueMaterial(m)
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
	var ran []Step
	for _, st := range steps {
		if st.Name != "DOT FIELD red bubble: materials" && st.Name != "DOT FIELD red bubble: texture streaming records" {
			continue
		}
		ran = append(ran, st)
		if err := st.Apply(p); err != nil {
			t.Fatalf("%s: %v", st.Name, err)
		}
	}
	if len(ran) != 2 {
		t.Fatalf("%d DOT FIELD texture/material steps registered, want 2", len(ran))
	}
	got := p.changes[Sets[0]]
	if want := len(droneLevels) + len(dotFieldMaterials); len(got) != want {
		t.Errorf("%d resources changed, want %d", len(got), want)
	}
	for _, lvl := range droneLevels {
		b := got[radpack.Key{Type: int64(texType), Name: int64(lvl)}]
		if b == nil || len(parse(t, b).Rows) != 2 {
			t.Errorf("level %016x: texture streaming table has no new record", lvl)
		}
	}
	for _, m := range dotFieldMaterials {
		if got[radpack.Key{Type: int64(matType), Name: int64(m.red)}] == nil {
			t.Errorf("no red copy %016x of material %016x", m.red, m.arc)
		}
	}
	// Every declared field reads from what the steps wrote, through the pack, as the oracle does.
	for _, st := range ran {
		for _, f := range st.Writes {
			for _, rec := range f.Records {
				b, err := p.Read(f.Set, int64(f.Type), int64(rec))
				if err != nil {
					t.Fatalf("%s %016x: %v", f.Name, rec, err)
				}
				if v, err := f.Read(b); err != nil || len(v) == 0 {
					t.Errorf("%s %016x: %x, %v", f.Name, rec, v, err)
				}
			}
		}
	}
}
