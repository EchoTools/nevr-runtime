package main

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"math"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

// The DOT FIELD's red bubble, model and transform records (#2242). he_is_the_cat's
// tools/fix_dot_field.py (82ad7ba) gives the DOT magazine a visible field by copying the arc mine's arming bubble
// (model component ARC_BUBBLE f775b4bf32c0bf2f, "fx_arc_mine", of actor ARC_ACTOR) onto the DOT as FIELD
// (51b9f7653ecac7cb) of DOT_ACTOR, in each of the five combat levels (fix_dot_field.py:60-66, :253-262). This slice builds
// two of its three record kinds; the texture-streaming record and the two red materials are steps_dot_field_texmat.go,
// and the script-image edits (the .rdata scene-set constant, the flag bytes, LINKS: fix_dot_field.py:189-233) are
// H-DOTF, which this tool does not do. The red model's own files are built by no step (KNOWN-WRONG #2242, see
// steps_dot_field_texmat.go).
//
//  1. Model table (CModelCRWin10, fix_dot_field.py:140-166). The root has six arrays: the model records (+0 component,
//     +8 actor), three bitsets, the per-model entries (+0 model asset, +0x10 component) and the per-model instances (+0
//     component, +8 actor, +0x20 model asset, +0x30 component); records, entries and instances are parallel. The arc
//     bubble's record, entry and instance are each copied to the end of their array with the component set to FIELD, the
//     actor to DOT_ACTOR and the model asset to RED_MODEL (CSymbol64("fx_dot_field_bubble"), fix_dot_field.py:79; his
//     tool makes the asset as a copy of the arc's model, which no step here does), and the new model's bit in each bitset is the arc
//     bubble's (a bitset word is added when the new index passes the last word).
//  2. Transform table (CTransformCRWin10, fix_dot_field.py:169-186, :256). A copy of the arc bubble's record is appended
//     with the component set to FIELD, the actor (+8, and +0x58 where the arc's is there) to DOT_ACTOR, and its scale
//     (+0x3C, three floats, which are 1 on the arc's: the bubble model is a unit sphere) to the DOT's radius.
//
// Bytes written: per combat level, in the model table the root's headers for arrays 0, 2 and 5 (byte size, count,
// capacity), one appended element in each, with the values above, the new model's bit in arrays 1, 3 and 4 (and a zero
// word appended to each when the index needs it); in the transform table the header (byte size, count, capacity) and one
// appended row: +0, +8, +0x58 when the arc's is there, +0x3C..+0x48.
//
// Declared fields (Step.Writes, what the stage82 oracle compares): in each level of droneLevels, the model table's FIELD
// record, entry and instance of the DOT actor and its bit in each of the three bitsets (readFieldModel), and the
// transform table's FIELD row of the DOT actor, whole (readFieldTransform). The array headers are not declared: their
// counts also hold whatever other model or transform records a build adds, which are not this step's.
//
// Order: the transform table is the CTransformCR table steps_dot_damp.go edits (transformType). That step runs first
// (file-name order, pinned by TestTheProductionStepsAreTheEchoCombatOnesInOrder), as his build runs fix_dot_dampener.py
// before fix_dot_field.py (deploy_and_launch.py:345, :351), so the appended row keeps the arc's translation; the
// dampener's declared field (readDotTranslations) reads the DOT rows other than this FIELD row, which is this step's.
//
// Base: his tool reads whichever table the build staged (fix_dot_field.py:242-244); these steps read the player's stock
// table, and refuse when the arc bubble is not found exactly once or the table already holds a FIELD record. Whether his
// gun patch changed these two tables is not settled here (#2242); no gun-patch value is used here.
const (
	modelTableType uint64 = 0xea51a0d76eb90142 // CSymbol64("CModelCRWin10"), fix_dot_field.py:54
	arcActor       uint64 = 0x0b53172e221366bd // fix_dot_field.py:68
	arcBubble      uint64 = 0xf775b4bf32c0bf2f // fix_dot_field.py:69
	fieldComponent uint64 = 0x51b9f7653ecac7cb // fix_dot_field.py:70
	arcModel       uint64 = 0x07e824dad4cd4ef3 // fix_dot_field.py:74
	redModel       uint64 = 0x3d938e95af4af8bb // CSymbol64("fx_dot_field_bubble"), fix_dot_field.py:79

	idOff = 0x00 // a model or transform record's component, then its actor at keyOff (+8)

	modelEntryModelOff = 0x00 // per-model entry: the model asset; fix_dot_field.py:155
	modelEntryIDOff    = 0x10 // per-model entry: the component
	modelInstIDOff     = 0x00 // per-model instance: component (fix_dot_field.py:156)
	modelInstModelOff  = 0x20 // ... model asset
	modelInstID2Off    = 0x30 // ... component again

	transformActor2Off = 0x58 // a second actor field, swapped only where it holds the arc's actor (fix_dot_field.py:256)
	transformScaleOff  = 0x3c // three float32, fix_dot_field.py:90
	transformScaleLen  = 12

	u64Len = 8 // bytes of a u64 field

	// dotFieldRadius is grenade_properties.dot.radius (balance_values.h, he_is_the_cat's config/mp_weapon_settings.json,
	// which fix_dot_field.py:250 reads); a test compares it with the embedded value.
	dotFieldRadius float32 = 3.5

	// dotFieldResourcesPerStep is the resources each DOT FIELD step changes over the shipped data: one table per combat
	// level (stock_steps_test.go adds the steps' terms to its total).
	dotFieldResourcesPerStep = 5
)

// dotFieldTransformStep is the FIELD transform step; the map terminal after it names it in After.
const dotFieldTransformStep = "DOT FIELD red bubble: transform records"

// The actor is dotMagazineActor (steps_dot_stick.go), the same id as fix_dot_field.py:67's DOT_ACTOR; the transform
// table type is transformType (steps_dot_damp.go), fix_dot_field.py:55's TRANSFORM_CR.
func init() {
	steps = append(steps,
		Step{Name: "DOT FIELD red bubble: model records", Apply: forEachLevelBytes(modelTableType, addFieldModel),
			Writes: []Field{{Name: "FIELD model record, entry and instance of the DOT actor, and its bit in the three bitsets",
				Set: Sets[0], Type: modelTableType, Records: droneLevels, Read: readFieldModel}}},
		// After the dampener: the dampener refuses the transform row this step appends (dotDampStep, steps_dot_damp.go).
		Step{Name: dotFieldTransformStep, After: []string{dotDampStep}, Apply: forEachLevelBytes(transformType,
			func(b []byte) ([]byte, error) { return addFieldTransform(b, dotFieldRadius) }),
			Writes: []Field{{Name: "FIELD transform row of the DOT actor, whole", Set: Sets[0], Type: transformType,
				Records: droneLevels, Read: readFieldTransform}}},
	)
}

// isFieldRow is a transform or model record of the DOT's FIELD component.
func isFieldRow(raw []byte) bool {
	id, actor, ok := modelKey(raw)
	return ok && id == fieldComponent && actor == dotMagazineActor
}

// readFieldModel is the model step's declared field: the one FIELD record of the DOT actor, the entry and the instance
// at its index, then one byte per bitset (crModelBitsA, B, C) holding its bit.
func readFieldModel(b []byte) ([]byte, error) {
	root, err := crParse(b, crModelRoot)
	if err != nil {
		return nil, err
	}
	if len(root.arrays) != crModelArrays {
		return nil, fmt.Errorf("the model table's root has %d arrays, expected %d", len(root.arrays), crModelArrays)
	}
	recs := root.arrays[crModelRecords]
	at := -1
	for i, e := range recs.elems {
		if isFieldRow(e.raw) {
			if at >= 0 {
				return nil, fmt.Errorf("two FIELD model records of the DOT actor (%d and %d)", at, i)
			}
			at = i
		}
	}
	if at < 0 {
		return nil, fmt.Errorf("no FIELD model record of the DOT actor among %d", len(recs.elems))
	}
	entries, insts := root.arrays[crModelEntries], root.arrays[crModelInsts]
	if at >= len(entries.elems) || at >= len(insts.elems) {
		return nil, fmt.Errorf("model %d has no entry or instance (%d entries, %d instances)", at, len(entries.elems), len(insts.elems))
	}
	out := append(append(append([]byte(nil), recs.elems[at].raw...), entries.elems[at].raw...), insts.elems[at].raw...)
	for _, bi := range []int{crModelBitsA, crModelBitsB, crModelBitsC} {
		bits := root.arrays[bi]
		if bits.esize != crBitsetWord || at/crBitsPerWord >= len(bits.elems) {
			return nil, fmt.Errorf("bitset array %d has %d words of %d bytes, none for model %d", bi, len(bits.elems), bits.esize, at)
		}
		out = append(out, byte(binary.LittleEndian.Uint64(bits.elems[at/crBitsPerWord].raw)>>(at%crBitsPerWord)&1))
	}
	return out, nil
}

// readFieldTransform is the transform step's declared field: the one FIELD row of the DOT actor, whole.
func readFieldTransform(b []byte) ([]byte, error) { return readFieldRow(b, "transform") }

// readFieldRow is the one FIELD row of the DOT actor, whole, of a row table of the named kind (the transform table here,
// the texture-streaming table in steps_dot_field_texmat.go).
func readFieldRow(b []byte, kind string) ([]byte, error) {
	t, err := radpack.ParseRowTable(b)
	if err != nil {
		return nil, err
	}
	if t.Size < keyOff+u64Len {
		return nil, fmt.Errorf("%s rows of %d bytes end before the actor at +%#x", kind, t.Size, keyOff)
	}
	var row []byte
	for i, r := range t.Rows {
		if isFieldRow(r) {
			if row != nil {
				return nil, fmt.Errorf("two FIELD %s rows of the DOT actor (row %d is the second)", kind, i)
			}
			row = r
		}
	}
	if row == nil {
		return nil, fmt.Errorf("no FIELD %s row of the DOT actor among %d", kind, len(t.Rows))
	}
	return append([]byte(nil), row...), nil
}

// expectU64 refuses a record whose u64 at off is not the stock value this step is built over.
func expectU64(raw []byte, off int, want uint64, record, field string) error {
	if off < 0 || off+u64Len > len(raw) {
		return fmt.Errorf("%s of %d bytes ends before %s at +%#x", record, len(raw), field, off)
	}
	if got := binary.LittleEndian.Uint64(raw[off:]); got != want {
		return fmt.Errorf("%s %s at +%#x is %016x, expected %016x", record, field, off, got, want)
	}
	return nil
}

// swapU64 writes v over the u64 at off after checking it holds want.
func swapU64(raw []byte, off int, want, v uint64, record, field string) error {
	if err := expectU64(raw, off, want, record, field); err != nil {
		return err
	}
	binary.LittleEndian.PutUint64(raw[off:], v)
	return nil
}

// modelKey is a model record's (component, actor).
func modelKey(raw []byte) (uint64, uint64, bool) {
	if len(raw) < keyOff+u64Len {
		return 0, 0, false
	}
	return binary.LittleEndian.Uint64(raw[idOff:]), binary.LittleEndian.Uint64(raw[keyOff:]), true
}

// addFieldModel is fix_dot_field.py:140-166 over a stock CModelCRWin10 table.
func addFieldModel(blob []byte) ([]byte, error) {
	root, err := crParse(blob, crModelRoot)
	if err != nil {
		return nil, err
	}
	if len(root.arrays) != crModelArrays {
		return nil, fmt.Errorf("the model table's root has %d arrays, expected %d", len(root.arrays), crModelArrays)
	}
	recs, entries, insts := root.arrays[crModelRecords], root.arrays[crModelEntries], root.arrays[crModelInsts]
	arcAt := -1
	for i, e := range recs.elems {
		id, actor, ok := modelKey(e.raw)
		if !ok {
			return nil, fmt.Errorf("model record %d of %d bytes ends before its actor", i, len(e.raw))
		}
		if id == fieldComponent && actor == dotMagazineActor {
			return nil, fmt.Errorf("the model table already has the DOT's FIELD record (model %d)", i)
		}
		if id == arcBubble && actor == arcActor {
			if arcAt >= 0 {
				return nil, fmt.Errorf("two arc bubble model records (%d and %d), expected one", arcAt, i)
			}
			arcAt = i
		}
	}
	if arcAt < 0 {
		return nil, fmt.Errorf("no arc bubble model record (%016x of actor %016x) among %d", arcBubble, arcActor, len(recs.elems))
	}
	n := recs.count
	if len(entries.elems) != n || len(insts.elems) != n {
		return nil, fmt.Errorf("the per-model arrays are not parallel: %d records, %d entries, %d instances", n, len(entries.elems), len(insts.elems))
	}
	rec, entry, inst := crCopy(recs.elems[arcAt]), crCopy(entries.elems[arcAt]), crCopy(insts.elems[arcAt])
	for _, s := range []struct {
		raw         []byte
		off         int
		want, to    uint64
		record, key string
	}{
		{rec.raw, idOff, arcBubble, fieldComponent, "model record", "component"},
		{rec.raw, keyOff, arcActor, dotMagazineActor, "model record", "actor"},
		{entry.raw, modelEntryIDOff, arcBubble, fieldComponent, "model entry", "component"},
		{entry.raw, modelEntryModelOff, arcModel, redModel, "model entry", "model asset"},
		{inst.raw, modelInstIDOff, arcBubble, fieldComponent, "model instance", "component"},
		{inst.raw, modelInstID2Off, arcBubble, fieldComponent, "model instance", "second component"},
		{inst.raw, keyOff, arcActor, dotMagazineActor, "model instance", "actor"},
		{inst.raw, modelInstModelOff, arcModel, redModel, "model instance", "model asset"},
	} {
		if err := swapU64(s.raw, s.off, s.want, s.to, s.record, s.key); err != nil {
			return nil, err
		}
	}
	// The bitsets: read the arc bubble's bit before any array grows, then give the new model the same.
	for _, bi := range []int{crModelBitsA, crModelBitsB, crModelBitsC} {
		bits := root.arrays[bi]
		if bits.esize != crBitsetWord {
			return nil, fmt.Errorf("bitset array %d has elements of %d bytes, expected %d", bi, bits.esize, crBitsetWord)
		}
		if arcAt/crBitsPerWord >= len(bits.elems) {
			return nil, fmt.Errorf("bitset array %d has %d words, none for model %d", bi, len(bits.elems), arcAt)
		}
	}
	for _, add := range []struct {
		array int
		elem  *crNode
	}{{crModelRecords, rec}, {crModelEntries, entry}, {crModelInsts, inst}} {
		if err := crAdd(root, add.array, add.elem); err != nil {
			return nil, err
		}
	}
	for _, bi := range []int{crModelBitsA, crModelBitsB, crModelBitsC} {
		bits := root.arrays[bi]
		on := binary.LittleEndian.Uint64(bits.elems[arcAt/crBitsPerWord].raw) >> (arcAt % crBitsPerWord) & 1
		for len(bits.elems)*crBitsPerWord <= n {
			if err := crAdd(root, bi, &crNode{raw: make([]byte, crBitsetWord)}); err != nil {
				return nil, err
			}
		}
		w := bits.elems[n/crBitsPerWord].raw
		binary.LittleEndian.PutUint64(w, binary.LittleEndian.Uint64(w)&^(1<<(n%crBitsPerWord))|on<<(n%crBitsPerWord))
	}
	return crBuild(root), nil
}

// addFieldTransform is fix_dot_field.py:169-186 over a stock CTransformCRWin10 table (a RowTable: one array of fixed
// records with nothing after them), the new row's scale set to radius.
func addFieldTransform(blob []byte, radius float32) ([]byte, error) {
	t, err := radpack.ParseRowTable(blob)
	if err != nil {
		return nil, err
	}
	if len(t.Tail) != 0 {
		return nil, fmt.Errorf("the transform table has %d bytes after its rows (nested data), which this step does not copy", len(t.Tail))
	}
	if t.Size < transformScaleOff+transformScaleLen {
		return nil, fmt.Errorf("transform rows of %d bytes end before the scale at +%#x", t.Size, transformScaleOff)
	}
	arcAt := -1
	for i, r := range t.Rows {
		id, actor := binary.LittleEndian.Uint64(r[idOff:]), binary.LittleEndian.Uint64(r[keyOff:])
		if id == fieldComponent && actor == dotMagazineActor {
			return nil, fmt.Errorf("the transform table already has the DOT's FIELD row (row %d)", i)
		}
		if id == arcBubble && actor == arcActor {
			if arcAt >= 0 {
				return nil, fmt.Errorf("two arc bubble transform rows (%d and %d), expected one", arcAt, i)
			}
			arcAt = i
		}
	}
	if arcAt < 0 {
		return nil, fmt.Errorf("no arc bubble transform row (%016x of actor %016x) among %d", arcBubble, arcActor, len(t.Rows))
	}
	row := append([]byte(nil), t.Rows[arcAt]...)
	if err := swapU64(row, idOff, arcBubble, fieldComponent, "transform row", "component"); err != nil {
		return nil, err
	}
	if err := swapU64(row, keyOff, arcActor, dotMagazineActor, "transform row", "actor"); err != nil {
		return nil, err
	}
	if t.Size >= transformActor2Off+u64Len && binary.LittleEndian.Uint64(row[transformActor2Off:]) == arcActor {
		binary.LittleEndian.PutUint64(row[transformActor2Off:], dotMagazineActor)
	}
	var unit [transformScaleLen]byte
	for i := 0; i < transformScaleLen; i += floatLen {
		binary.LittleEndian.PutUint32(unit[i:], math.Float32bits(1))
	}
	if !bytes.Equal(row[transformScaleOff:transformScaleOff+transformScaleLen], unit[:]) {
		return nil, fmt.Errorf("transform row scale at +%#x is % x, expected (1, 1, 1) % x", transformScaleOff,
			row[transformScaleOff:transformScaleOff+transformScaleLen], unit[:])
	}
	for i := 0; i < transformScaleLen; i += floatLen {
		binary.LittleEndian.PutUint32(row[transformScaleOff+i:], math.Float32bits(radius))
	}
	t.Rows = append(t.Rows, row)
	return t.Bytes(), nil
}
