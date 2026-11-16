package main

// The DOT FIELD model and transform steps over synthetic tables with the documented layout (crtable.go);
// no byte of the game's is used. stock_steps_test.go counts the resources the steps change over the shipped data when a
// machine has it.

import (
	"bytes"
	"encoding/binary"
	"math"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

// The test's own writer of a model table: the layout of the file comment of crtable.go, written
// straight from it (a root of crModelRoot bytes with six headers, then each array's elements, each element's nested
// array after the array's data), so the step's output is compared with a table built another way.
const (
	tRecSize   = 0x10
	tEntrySize = 0x48
	tInstSize  = 0x38
	tNestedAt  = 0x18 // the entry's own array header
	tNestedLen = 8
	tMarkerOff = 0x08 // a nonzero filler where a header's pointer would be, so no scan mistakes the element for a header
	tInstMark  = 0x10
	tHeaderAt  = crHeaderLen // headers are laid out one after the other from 0
)

type tModels struct {
	recs    [][2]uint64 // component, actor
	entries [][3]uint64 // model asset, component, nested word
	insts   [][5]uint64 // component, actor, model asset, second component, filler
	bits    [3][]uint64
}

func putHeader(raw []byte, at, count, esize int) {
	le := binary.LittleEndian
	le.PutUint64(raw[at+crSizeOff:], uint64(count*esize))
	le.PutUint32(raw[at+crOneOff:], 1)
	le.PutUint64(raw[at+crCountOff:], uint64(count))
	le.PutUint64(raw[at+crCapacityOff:], uint64(count))
}

func padTo(out []byte, esize int) []byte {
	pad := crPadNarrow
	if esize%crWideElement == 0 {
		pad = crPadWide
	}
	for len(out)%pad != 0 {
		out = append(out, 0)
	}
	return out
}

func (m tModels) build() []byte {
	root := make([]byte, crModelRoot)
	counts := []int{len(m.recs), len(m.bits[0]), len(m.entries), len(m.bits[1]), len(m.bits[2]), len(m.insts)}
	sizes := []int{tRecSize, crBitsetWord, tEntrySize, crBitsetWord, crBitsetWord, tInstSize}
	for i := range counts {
		putHeader(root, i*tHeaderAt, counts[i], sizes[i])
	}
	out := root
	le := binary.LittleEndian
	// arrays in header order; each array's data, then the nested data of its elements
	emitRecs := func() {
		out = padTo(out, tRecSize)
		for _, r := range m.recs {
			var e [tRecSize]byte
			le.PutUint64(e[idOff:], r[0])
			le.PutUint64(e[keyOff:], r[1])
			out = append(out, e[:]...)
		}
	}
	emitBits := func(w []uint64) {
		if len(w) == 0 {
			return
		}
		out = padTo(out, crBitsetWord)
		for _, x := range w {
			out = le.AppendUint64(out, x)
		}
	}
	emitEntries := func() {
		out = padTo(out, tEntrySize)
		for _, e := range m.entries {
			raw := make([]byte, tEntrySize)
			le.PutUint64(raw[modelEntryModelOff:], e[0])
			le.PutUint64(raw[tMarkerOff:], 1)
			le.PutUint64(raw[modelEntryIDOff:], e[1])
			putHeader(raw, tNestedAt, 1, tNestedLen)
			out = append(out, raw...)
		}
		for _, e := range m.entries {
			out = le.AppendUint64(padTo(out, tNestedLen), e[2])
		}
	}
	emitInsts := func() {
		out = padTo(out, tInstSize)
		for _, in := range m.insts {
			raw := make([]byte, tInstSize)
			le.PutUint64(raw[modelInstIDOff:], in[0])
			le.PutUint64(raw[keyOff:], in[1])
			le.PutUint64(raw[tInstMark:], in[4])
			le.PutUint64(raw[modelInstModelOff:], in[2])
			le.PutUint64(raw[modelInstID2Off:], in[3])
			out = append(out, raw...)
		}
	}
	emitRecs()
	emitBits(m.bits[0])
	emitEntries()
	emitBits(m.bits[1])
	emitBits(m.bits[2])
	emitInsts()
	return out
}

// models makes n models: model i has component 0x1000+i of actor 0x2000+i; the arc bubble is at arcAt, and another
// model has the arc's component under another actor.
func models(n, arcAt int) tModels {
	m := tModels{}
	words := (n + crBitsPerWord - 1) / crBitsPerWord
	for b := range m.bits {
		m.bits[b] = make([]uint64, words)
	}
	for i := 0; i < n; i++ {
		comp, actor := uint64(0x1000+i), uint64(0x2000+i)
		model := uint64(0x3000 + i)
		if i == arcAt {
			comp, actor, model = arcBubble, arcActor, arcModel
		} else if i == (arcAt+1)%n {
			comp = arcBubble // the arc's component under another actor: not the arc's record
		}
		m.recs = append(m.recs, [2]uint64{comp, actor})
		m.entries = append(m.entries, [3]uint64{model, comp, uint64(0xe000 + i)})
		m.insts = append(m.insts, [5]uint64{comp, actor, model, comp, uint64(0xf000 + i)})
		for b := range m.bits {
			if (i+b)%(b+2) == 0 || (b == 2 && i == arcAt) {
				m.bits[b][i/crBitsPerWord] |= 1 << (i % crBitsPerWord)
			}
		}
	}
	return m
}

// expected is the model table the step must write: the arc's record, entry and instance copied to the end with the DOT's
// ids, and the arc's bit in each bitset for the new model.
func (m tModels) expected(arcAt int) tModels {
	n := len(m.recs)
	e := tModels{recs: append([][2]uint64(nil), m.recs...), entries: append([][3]uint64(nil), m.entries...), insts: append([][5]uint64(nil), m.insts...)}
	e.recs = append(e.recs, [2]uint64{fieldComponent, dotMagazineActor})
	e.entries = append(e.entries, [3]uint64{redModel, fieldComponent, m.entries[arcAt][2]})
	e.insts = append(e.insts, [5]uint64{fieldComponent, dotMagazineActor, redModel, fieldComponent, m.insts[arcAt][4]})
	for b := range m.bits {
		w := append([]uint64(nil), m.bits[b]...)
		for len(w)*crBitsPerWord <= n {
			w = append(w, 0)
		}
		on := m.bits[b][arcAt/crBitsPerWord] >> (arcAt % crBitsPerWord) & 1
		w[n/crBitsPerWord] = w[n/crBitsPerWord]&^(1<<(n%crBitsPerWord)) | on<<(n%crBitsPerWord)
		e.bits[b] = w
	}
	return e
}

func TestTheModelTableGetsTheArcBubblesRecordsAsTheDOTsField(t *testing.T) {
	for _, c := range []struct{ n, arcAt int }{{3, 1}, {5, 0}, {5, 4}, {64, 5}, {70, 69}} {
		in := models(c.n, c.arcAt)
		got, err := addFieldModel(in.build())
		if err != nil {
			t.Fatalf("%d models, arc at %d: %v", c.n, c.arcAt, err)
		}
		want := in.expected(c.arcAt).build()
		if !bytes.Equal(got, want) {
			t.Errorf("%d models, arc at %d: the table is %d bytes, not the expected %d (first difference at %d)", c.n, c.arcAt, len(got), len(want), firstDiff(got, want))
		}
	}
}

func TestTheNewModelsBitIsTheArcBubblesInEachBitsetAndAWordIsAddedAtTheEdge(t *testing.T) {
	in := models(64, 5) // 64 models fill the one word of each bitset: the new model, 64, needs a second word
	out, err := addFieldModel(in.build())
	if err != nil {
		t.Fatal(err)
	}
	root, err := crParse(out, crModelRoot)
	if err != nil {
		t.Fatal(err)
	}
	for k, bi := range []int{crModelBitsA, crModelBitsB, crModelBitsC} {
		bits := root.arrays[bi]
		if len(bits.elems) != 2 {
			t.Fatalf("bitset %d has %d words, want 2", k, len(bits.elems))
		}
		on := in.bits[k][0] >> 5 & 1
		if got := binary.LittleEndian.Uint64(bits.elems[1].raw); got != on {
			t.Errorf("bitset %d word 1 is %#x, want the arc bubble's bit %d at bit 0", k, got, on)
		}
	}
	if in.bits[0][0]>>5&1 == in.bits[1][0]>>5&1 && in.bits[1][0]>>5&1 == in.bits[2][0]>>5&1 {
		t.Error("the fixture's three bitsets agree on the arc bubble's bit, so a step that copies one bit to all would pass")
	}
}

func TestAModelTableTheStepWasNotBuiltOverIsRefused(t *testing.T) {
	good := models(4, 2)
	if _, err := addFieldModel(good.build()); err != nil {
		t.Fatalf("the base case is refused: %v", err)
	}
	mut := func(f func(m *tModels)) []byte {
		m := models(4, 2)
		f(&m)
		return m.build()
	}
	already := mut(func(m *tModels) { m.recs[0] = [2]uint64{fieldComponent, dotMagazineActor} })
	cases := []struct {
		name string
		b    []byte
		want string
	}{
		{"a table that already has the DOT's field", already, "already has the DOT's FIELD record"},
		{"no arc bubble", mut(func(m *tModels) { m.recs[2][1] = 0x77 }), "no arc bubble model record"},
		{"two arc bubbles", mut(func(m *tModels) { m.recs[0] = [2]uint64{arcBubble, arcActor} }), "two arc bubble model records"},
		{"entries not parallel", mut(func(m *tModels) { m.entries = m.entries[:3] }), "not parallel"},
		{"an entry whose model asset is not the arc's", mut(func(m *tModels) { m.entries[2][0] = 0x99 }), "model entry model asset at +0x0 is 0000000000000099, expected " + "07e824dad4cd4ef3"},
		{"an entry whose component is not the arc's", mut(func(m *tModels) { m.entries[2][1] = 0x99 }), "model entry component at +0x10"},
		{"an instance whose second component is not the arc's", mut(func(m *tModels) { m.insts[2][3] = 0x99 }), "model instance second component at +0x30"},
		{"an instance whose model asset is not the arc's", mut(func(m *tModels) { m.insts[2][2] = 0x99 }), "model instance model asset at +0x20"},
		{"a table with a byte after it", append(good.build(), 0), "ended at"},
		{"a table cut short", good.build()[:len(good.build())-4], "runs past"},
	}
	for _, c := range cases {
		out, err := addFieldModel(c.b)
		if err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: err %v (out %d bytes), want it to say %q", c.name, err, len(out), c.want)
		}
	}
	noBits := models(4, 2)
	noBits.bits[1] = nil
	if _, err := addFieldModel(noBits.build()); err == nil {
		t.Error("a table with an empty bitset was changed")
	}
}

func TestTheCRTableReaderRefusesWhatItCannotWriteBack(t *testing.T) {
	good := models(3, 1).build()
	for _, p := range []int{crModelRoot + 1, len(good) + 1} {
		if _, err := crParse(append(good[:p:p], make([]byte, 4)...), crModelRoot); err == nil && p == len(good)+1 {
			t.Errorf("padding of %d bytes was read", p)
		}
	}
	// Bytes in a gap the layout does not account for: the walk ends before the table does.
	gap := append(append([]byte(nil), good[:crModelRoot]...), append(make([]byte, crPadWide), good[crModelRoot:]...)...)
	if _, err := crParse(gap, crModelRoot); err == nil {
		t.Error("a table with a gap after the root was read")
	}
}

// --- transform records ---------------------------------------------------------------------------------------------

const tTransformRow = 0x60

func transformRow(id, actor uint64, scale float32, actor2 uint64) []byte {
	r := make([]byte, tTransformRow)
	binary.LittleEndian.PutUint64(r[idOff:], id)
	binary.LittleEndian.PutUint64(r[keyOff:], actor)
	binary.LittleEndian.PutUint64(r[transformActor2Off:], actor2)
	for i := 0; i < 3; i++ {
		binary.LittleEndian.PutUint32(r[transformScaleOff+4*i:], math.Float32bits(scale))
	}
	return r
}

func scaleOf(r []byte) [3]float32 {
	var s [3]float32
	for i := range s {
		s[i] = math.Float32frombits(binary.LittleEndian.Uint32(r[transformScaleOff+4*i:]))
	}
	return s
}

func TestTheTransformTableGetsTheArcBubblesRowScaledToTheDOTsRadius(t *testing.T) {
	const other = 0x4444
	rows := [][]byte{
		transformRow(other, arcActor, 1, 0),
		transformRow(arcBubble, arcActor, 1, arcActor),
		transformRow(arcBubble, other, 1, 0), // the arc's component under another actor: not the arc's row
		transformRow(other, dotMagazineActor, 0.5, 0),
	}
	in := table(tTransformRow, nil, rows...)
	out, err := addFieldTransform(in, 3.5)
	if err != nil {
		t.Fatal(err)
	}
	tb := parse(t, out)
	if len(tb.Rows) != 5 {
		t.Fatalf("%d rows, want 5", len(tb.Rows))
	}
	for i, r := range rows {
		if !bytes.Equal(tb.Rows[i], r) {
			t.Errorf("row %d changed", i)
		}
	}
	want := transformRow(fieldComponent, dotMagazineActor, 3.5, dotMagazineActor)
	if !bytes.Equal(tb.Rows[4], want) {
		t.Errorf("new row\n got %x\nwant %x", tb.Rows[4], want)
	}
	if sc := scaleOf(tb.Rows[4]); sc != [3]float32{3.5, 3.5, 3.5} {
		t.Errorf("scale %v", sc)
	}
	if !bytes.Equal(out[:0x08], in[:0x08]) || !bytes.Equal(out[0x10:0x28], in[0x10:0x28]) {
		t.Error("a header byte other than the size, capacity and count changed")
	}
	// The second actor field is swapped only where it holds the arc's.
	out, err = addFieldTransform(table(tTransformRow, nil, transformRow(arcBubble, arcActor, 1, other)), 2)
	if err != nil {
		t.Fatal(err)
	}
	if got := binary.LittleEndian.Uint64(parse(t, out).Rows[1][transformActor2Off:]); got != other {
		t.Errorf("a second actor field that was not the arc's became %x", got)
	}
}

func TestATransformTableTheStepWasNotBuiltOverIsRefused(t *testing.T) {
	arc := transformRow(arcBubble, arcActor, 1, arcActor)
	if _, err := addFieldTransform(table(tTransformRow, nil, arc), 3.5); err != nil {
		t.Fatalf("the base case is refused: %v", err)
	}
	cases := []struct {
		name string
		b    []byte
		want string
	}{
		{"a table that already has the DOT's field", table(tTransformRow, nil, arc, transformRow(fieldComponent, dotMagazineActor, 1, 0)), "already has the DOT's FIELD row"},
		{"no arc bubble", table(tTransformRow, nil, transformRow(1, 2, 1, 0)), "no arc bubble transform row"},
		{"two arc bubbles", table(tTransformRow, nil, arc, arc), "two arc bubble transform rows"},
		{"an arc scale that is not 1", table(tTransformRow, nil, transformRow(arcBubble, arcActor, 2, 0)), "scale at +0x3c"},
		{"data after the rows", table(tTransformRow, []byte("nested"), arc), "after its rows"},
		{"rows that end before the scale", table(0x40, nil, row(0x40, map[int]uint64{idOff: arcBubble, keyOff: arcActor})), "end before the scale"},
	}
	for _, c := range cases {
		if _, err := addFieldTransform(c.b, 3.5); err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: err %v, want it to say %q", c.name, err, c.want)
		}
	}
	if _, err := addFieldTransform([]byte("short"), 3.5); err == nil {
		t.Error("a table shorter than its header was changed")
	}
}

// --- the steps ---------------------------------------------------------------------------------------------------

func TestTheDOTFieldStepsChangeTheFiveCombatLevelsAndNoOther(t *testing.T) {
	const other = 0x1234
	var modelType, transformKind uint64 = modelTableType, transformType // not constants: the ids overflow a constant int64 conversion
	res := map[radpack.Key][]byte{}
	for _, lvl := range append(append([]uint64(nil), droneLevels...), other) {
		res[radpack.Key{Type: int64(modelType), Name: int64(lvl)}] = models(3, 1).build()
		res[radpack.Key{Type: int64(transformKind), Name: int64(lvl)}] = table(tTransformRow, nil, transformRow(arcBubble, arcActor, 1, 0))
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
	ran := 0
	for _, st := range steps {
		// the model and transform steps only: steps_dot_field_texmat.go's two steps share the name's prefix
		if st.Name != "DOT FIELD red bubble: model records" && st.Name != "DOT FIELD red bubble: transform records" {
			continue
		}
		ran++
		if err := st.Apply(p); err != nil {
			t.Fatalf("%s: %v", st.Name, err)
		}
	}
	if ran != 2 {
		t.Fatalf("%d DOT FIELD steps ran, want 2 (model and transform records)", ran)
	}
	got := p.changes[Sets[0]]
	if len(got) != dotFieldResourcesPerStep*2 {
		t.Errorf("%d resources changed, want %d", len(got), dotFieldResourcesPerStep*2)
	}
	for _, lvl := range droneLevels {
		if b := got[radpack.Key{Type: int64(modelType), Name: int64(lvl)}]; !bytes.Equal(b, models(3, 1).expected(1).build()) {
			t.Errorf("level %016x: the model table is not the expected one", lvl)
		}
		b := got[radpack.Key{Type: int64(transformKind), Name: int64(lvl)}]
		if b == nil || len(parse(t, b).Rows) != 2 || scaleOf(parse(t, b).Rows[1]) != [3]float32{dotFieldRadius, dotFieldRadius, dotFieldRadius} {
			t.Errorf("level %016x: the transform table did not get the scaled row", lvl)
		}
	}
	for _, typ := range []uint64{modelType, transformKind} {
		if _, ok := got[radpack.Key{Type: int64(typ), Name: other}]; ok {
			t.Errorf("table %016x of a level that is not a combat level was changed", typ)
		}
	}
	// A level whose table the rule refuses stops the step with the level and table named.
	res[radpack.Key{Type: int64(modelType), Name: int64(droneLevels[2])}] = models(3, 1).expected(1).build() // already has the field
	d2 := t.TempDir()
	if err := radpack.WriteStockSet(d2, Sets[0], []map[radpack.Key][]byte{res}); err != nil {
		t.Fatal(err)
	}
	s2, _, err := radpack.OpenSet(d2, Sets[0])
	if err != nil {
		t.Fatal(err)
	}
	p2 := &Pack{sets: map[string]*radpack.Set{Sets[0]: s2}, changes: map[string]map[radpack.Key][]byte{}, scripts: map[string][]byte{}}
	err = forEachLevelBytes(modelTableType, addFieldModel)(p2)
	if err == nil || !strings.Contains(err.Error(), "level 43e2da7a0c623a19: the model table already has the DOT's FIELD record") {
		t.Errorf("a refused level: err %v", err)
	}
}

// --- the constants ----------------------------------------------------------------------------------------------

// csymbol64 is sdk/include/nevr/symbol_hash.h's hash, written again here to check the ids this file keeps.
func csymbol64(name string) uint64 {
	const poly = 0x95ac9329ac4bc9b5
	var table [256]uint64
	for i := range table {
		var crc uint64
		if i&0x80 != 0 {
			crc = 0x2b5926535897936a
		}
		if i&0x40 != 0 && i&0x80 == 0 {
			crc = poly
		} else if i&0x40 != 0 {
			crc = 0xbef5b57af4dc5adf
		}
		for _, bit := range []int{0x20, 0x10, 0x08, 0x04, 0x02, 0x01} {
			crc *= 2
			if i&bit != 0 {
				crc ^= poly
			}
		}
		table[i] = crc * 2
	}
	seed := ^uint64(0)
	for _, c := range []byte(name) {
		if c >= 'A' && c <= 'Z' {
			c += 'a' - 'A'
		}
		seed = uint64(int64(int8(c))) ^ table[seed>>56] ^ seed<<8
	}
	return seed
}

func TestTheIdsAreTheNamesHashes(t *testing.T) {
	if got := csymbol64("serverdb"); got != 0x25e886012ced8064 {
		t.Fatalf("the test's hash of serverdb is %x, the header's vector is 25e886012ced8064", got)
	}
	for name, want := range map[string]uint64{
		"CModelCRWin10":       modelTableType,
		"CTransformCRWin10":   transformType,
		"fx_dot_field_bubble": redModel,
		"fx_wpn_arc_mine_rig": arcModel, // the name src/runtime/log/symcache_data.cpp gives the arc's model asset
		"fx_arc_mine":         arcBubble,
	} {
		if got := csymbol64(name); got != want {
			t.Errorf("CSymbol64(%q) is %016x, the step has %016x", name, got, want)
		}
	}
}

func TestTheRadiusIsTheEmbeddedDOTRadius(t *testing.T) {
	header, err := os.ReadFile(filepath.Join("..", "..", "..", "src", "core", "combat_balance", "balance_values.h"))
	if err != nil {
		t.Fatal(err)
	}
	m := regexp.MustCompile(`(?s)"grenade_properties": \{.*?"dot": \{\s*"radius": ([0-9.]+)`).FindSubmatch(header)
	if m == nil {
		t.Fatal("balance_values.h has no grenade_properties.dot.radius")
	}
	r, err := strconv.ParseFloat(string(m[1]), 32)
	if err != nil || float32(r) != dotFieldRadius {
		t.Errorf("the embedded radius is %s, the step's is %v", m[1], dotFieldRadius)
	}
}

func TestAFieldPastTheRecordsEndIsRefused(t *testing.T) {
	if err := expectU64(make([]byte, u64Len), 0, 0, "record", "field"); err != nil {
		t.Errorf("a field that fills the record was refused: %v", err)
	}
	if err := expectU64(make([]byte, u64Len-1), 0, 0, "record", "field"); err == nil || !strings.Contains(err.Error(), "ends before") {
		t.Errorf("a field past the record's end: err %v, want a refusal that the record ends before it", err)
	}
}

// --- the declared fields ----------------------------------------------------------------------------------------

func TestTheDOTFieldFieldsReadWhatTheStepsWrite(t *testing.T) {
	le := binary.LittleEndian
	for _, arcAt := range []int{0, 1} { // the arc's bits are (1, 0, 1) at 0 and (0, 0, 1) at 1 (models)
		in := models(3, arcAt)
		if _, err := readFieldModel(in.build()); err == nil || !strings.Contains(err.Error(), "no FIELD model record") {
			t.Errorf("arc at %d: a stock model table read as %v, want no FIELD record", arcAt, err)
		}
		out, err := addFieldModel(in.build())
		if err != nil {
			t.Fatal(err)
		}
		got, err := readFieldModel(out)
		if err != nil {
			t.Fatal(err)
		}
		if len(got) != tRecSize+tEntrySize+tInstSize+len(in.bits) {
			t.Fatalf("arc at %d: %d bytes read", arcAt, len(got))
		}
		rec, entry, inst, bits := got[:tRecSize], got[tRecSize:tRecSize+tEntrySize], got[tRecSize+tEntrySize:tRecSize+tEntrySize+tInstSize], got[tRecSize+tEntrySize+tInstSize:]
		if le.Uint64(rec[idOff:]) != fieldComponent || le.Uint64(rec[keyOff:]) != dotMagazineActor {
			t.Errorf("arc at %d: record %x", arcAt, rec)
		}
		if le.Uint64(entry[modelEntryModelOff:]) != redModel || le.Uint64(entry[modelEntryIDOff:]) != fieldComponent {
			t.Errorf("arc at %d: entry %x", arcAt, entry)
		}
		if le.Uint64(inst[modelInstIDOff:]) != fieldComponent || le.Uint64(inst[keyOff:]) != dotMagazineActor ||
			le.Uint64(inst[modelInstModelOff:]) != redModel || le.Uint64(inst[modelInstID2Off:]) != fieldComponent {
			t.Errorf("arc at %d: instance %x", arcAt, inst)
		}
		for b := range in.bits {
			if want := byte(in.bits[b][0] >> arcAt & 1); bits[b] != want {
				t.Errorf("arc at %d: bitset %d bit %d, want the arc's %d", arcAt, b, bits[b], want)
			}
		}
	}

	twice := models(3, 1).expected(1).expected(1) // the FIELD records of two builds
	if _, err := readFieldModel(twice.build()); err == nil || !strings.Contains(err.Error(), "two FIELD model records") {
		t.Errorf("two FIELD model records read as %v", err)
	}

	arc := transformRow(arcBubble, arcActor, 1, arcActor)
	dot := transformRow(0x4444, dotMagazineActor, 1, 0)                     // the DOT actor under another component
	fieldOfOther := transformRow(fieldComponent, 0x5555, dotFieldRadius, 0) // the FIELD component under another actor
	in := table(tTransformRow, nil, arc, dot, fieldOfOther)
	if _, err := readFieldTransform(in); err == nil || !strings.Contains(err.Error(), "no FIELD transform row") {
		t.Errorf("a stock transform table read as %v, want no FIELD row", err)
	}
	out, err := addFieldTransform(in, dotFieldRadius)
	if err != nil {
		t.Fatal(err)
	}
	got, err := readFieldTransform(out)
	if want := transformRow(fieldComponent, dotMagazineActor, dotFieldRadius, dotMagazineActor); err != nil || !bytes.Equal(got, want) {
		t.Errorf("transform row\n got %x\nwant %x (%v)", got, want, err)
	}
	if _, err := readFieldTransform(table(tTransformRow, nil, got, got)); err == nil || !strings.Contains(err.Error(), "two FIELD transform rows") {
		t.Errorf("two FIELD rows read as %v", err)
	}
}

// The dampener's declared field (steps_dot_damp.go) reads the DOT rows it zeroes, not the FIELD row this file's step
// appends after it with the arc bubble's translation: a difference in that row is this step's field.
func TestTheDampenerFieldDoesNotReadTheFieldRow(t *testing.T) {
	dot := transformRow(0x4444, dotMagazineActor, 1, 0)
	field := transformRow(fieldComponent, dotMagazineActor, dotFieldRadius, dotMagazineActor)
	for off := transformMoveOff; off < transformMoveEnd; off += floatLen {
		setF32(field, off, 1) // the arc's translation is not the dampener's zero
	}
	without, err := readDotTranslations(table(tTransformRow, nil, dot))
	if err != nil {
		t.Fatal(err)
	}
	with, err := readDotTranslations(table(tTransformRow, nil, dot, field))
	if err != nil || !bytes.Equal(with, without) {
		t.Errorf("with the FIELD row %x, without %x (%v)", with, without, err)
	}
}
