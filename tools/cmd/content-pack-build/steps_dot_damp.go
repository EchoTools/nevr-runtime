package main

import (
	"encoding/binary"
	"fmt"
	"math"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

// The DOT FIELD's dampener and node translations (#2242, PLAN-v5 G-DOT-DAMP; inventory row 8). Origin: he_is_the_cat's
// tools/fix_dot_dampener.py at 82ad7ba (read as text; nothing of his output is used). The DOT magazine actor carries
// level component data the balance JSON cannot reach, in two tables of each combat level, each row bound to its actor
// by the node id at +0x08 (the tool's DOT_ACTOR, fix_dot_dampener.py:44; the binding is docstring :23):
//
//  1. CR15DampenerCR (2b292830e51c4fd2; tool :35, DAMPENER_CR): one row for the DOT. Stock has it enabled at spawn
//     (+0x18 = 1) with damping 10000 at +0x20 and +0x24 (docstring :7), so the body cannot move. The step starts it off
//     (:71) and sets both damping values to the arc's in-flight value, 25 (:72; the tool's --damping default, :67).
//  2. CTransformCR (92abd3e1432bf5e8; tool :36, TRANSFORM_CR): every DOT node carries a local translation of about
//     (-3.01, 0, 1.99) at +0x30 (the prefab was saved off-origin, docstring :17-18). The step zeroes the three floats
//     (:75); w, at +0x3c, is not touched (docstring :21).
//
// The tool's docstring hands CR15ParentedConstraintCR to fix_dot_constraints.py (:14-15), which is G-DOT-CONS's, not
// this step's.
//
// Order: this step must run before G-DOT-FIELD-1's step, which appends a DOT-actor row to the same CTransformCR table
// with a translation that is not the stray stock one; run after it, this step would refuse that row. Go runs file
// init()s in file-name order (steps_dot_damp.go before steps_dot_field.go), and
// TestTheProductionStepsAreTheEchoCombatOnesInOrder pins the resulting list.
//
// The DOT actor is dotMagazineActor (steps_dot_stick.go), the same id as the tool's DOT_ACTOR (:44).
//
// Declared fields (Step.Writes, what the stage82 oracle compares for this step): in each level of droneLevels, the
// CR15DampenerCR row of the DOT actor, its u32 at +0x18 and its two f32 at +0x20 and +0x24 (readDotDampener); and the
// CTransformCR rows of the DOT actor but its FIELD row (steps_dot_field.go's), the three f32 at +0x30 of each, in
// table order (readDotTranslations). This step
// leaves every other byte of both tables as it was. Base: stock rows (the tool reads his extract root; whether that root is stock
// or his gun patch for these two tables is S-BASE's question, PLAN-v5 4.4, and no gun-patch value is imported here).
const (
	dampenerTable = 0x2b292830e51c4fd2 // CSymbol64("CR15DampenerCRWin10")
	transformType = 0x92abd3e1432bf5e8 // CSymbol64("CTransformCRWin10")

	dampenerEnabledOff  = 0x18 // u32, 1 = on
	dampenerEnabledEnd  = dampenerEnabledOff + u32Len
	dampenerDampingOff  = 0x20                          // two f32
	dampenerDampingYOff = dampenerDampingOff + floatLen // the second damping value
	dampenerEnd         = dampenerDampingOff + 2*floatLen
	transformMoveOff    = 0x30                          // local translation, three f32: x
	transformMoveYOff   = transformMoveOff + floatLen   // y
	transformMoveZOff   = transformMoveOff + 2*floatLen // z
	transformMoveEnd    = transformMoveOff + 3*floatLen
	floatLen            = 4
	u32Len              = 4

	stockDampenerEnabled = 1
	stockDampening       = 10000.0 // damping at spawn
	arcDamping           = 25.0    // the arc's in-flight damping (the tool's --damping default, :67; written at :72)
	stockMoveX           = -3.01   // the prefab's stray translation
	stockMoveZ           = 1.99
	moveTolerance        = 0.005 // the tool's docstring gives two decimals
)

func init() {
	steps = append(steps, Step{
		Name:  "DOT FIELD dampener starts off and its nodes sit at the origin",
		Apply: dotDampApply,
		Writes: []Field{
			{Name: "dampener row of the DOT actor, enabled u32 +0x18 and damping f32 +0x20, +0x24", Set: Sets[0],
				Type: dampenerTable, Records: droneLevels, Read: readDotDampener},
			{Name: "transform rows of the DOT actor, translation f32 x3 +0x30", Set: Sets[0], Type: transformType,
				Records: droneLevels, Read: readDotTranslations},
		},
	})
}

func dotDampApply(p *Pack) error {
	if err := forEachLevel(dampenerTable, dotDampener)(p); err != nil {
		return fmt.Errorf("CR15DampenerCR: %w", err)
	}
	if err := forEachLevel(transformType, dotTransforms)(p); err != nil {
		return fmt.Errorf("CTransformCR: %w", err)
	}
	return nil
}

func f32(row []byte, off int) float64 {
	return float64(math.Float32frombits(binary.LittleEndian.Uint32(row[off:])))
}

func setF32(row []byte, off int, v float32) {
	binary.LittleEndian.PutUint32(row[off:], math.Float32bits(v))
}

func near(got, want float64) bool { return math.Abs(got-want) <= moveTolerance }

// dotDampenerRow is the one dampener row of the DOT actor, after checking the rows reach the dampener fields.
func dotDampenerRow(t *radpack.RowTable) ([]byte, error) {
	if t.Size < dampenerEnd {
		return nil, fmt.Errorf("rows of %d bytes end before the dampener fields at +%#x", t.Size, dampenerEnd)
	}
	var hits []int
	for i, r := range t.Rows {
		if radpack.U64(r, keyOff) == dotMagazineActor {
			hits = append(hits, i)
		}
	}
	if len(hits) != 1 { // the tool refuses any other count (:58; expect_one is True for the dampener, :77)
		return nil, fmt.Errorf("%d rows for the DOT actor %016x, want 1", len(hits), uint64(dotMagazineActor))
	}
	return t.Rows[hits[0]], nil
}

// readDotDampener is the dampener step's declared field: the enabled u32 at +0x18 and the two damping f32 at +0x20 and
// +0x24 of the one DOT row, in that order.
func readDotDampener(b []byte) ([]byte, error) {
	t, err := radpack.ParseRowTable(b)
	if err != nil {
		return nil, err
	}
	r, err := dotDampenerRow(t)
	if err != nil {
		return nil, err
	}
	return append(append([]byte(nil), r[dampenerEnabledOff:dampenerEnabledEnd]...), r[dampenerDampingOff:dampenerEnd]...), nil
}

// readDotTranslations is the transform step's declared field: the three translation f32 at +0x30 of every DOT row, in
// table order, but the DOT FIELD row steps_dot_field.go appends after this step (isFieldRow): that row keeps the arc
// bubble's translation, and it is that step's declared field (readFieldTransform), not this one's.
func readDotTranslations(b []byte) ([]byte, error) {
	t, err := radpack.ParseRowTable(b)
	if err != nil {
		return nil, err
	}
	if t.Size < transformMoveEnd {
		return nil, fmt.Errorf("rows of %d bytes end before the translation at +%#x", t.Size, transformMoveEnd)
	}
	var out []byte
	for _, r := range t.Rows {
		if radpack.U64(r, keyOff) == dotMagazineActor && !isFieldRow(r) {
			out = append(out, r[transformMoveOff:transformMoveEnd]...)
		}
	}
	return out, nil
}

// dotDampener edits the one DOT row, after checking it is the stock one.
func dotDampener(t *radpack.RowTable) error {
	r, err := dotDampenerRow(t)
	if err != nil {
		return err
	}
	if en := binary.LittleEndian.Uint32(r[dampenerEnabledOff:]); en != stockDampenerEnabled {
		return fmt.Errorf("DOT row +%#x enabled is %d, stock is %d", dampenerEnabledOff, en, stockDampenerEnabled)
	}
	for _, off := range []int{dampenerDampingOff, dampenerDampingYOff} {
		if v := f32(r, off); v != stockDampening {
			return fmt.Errorf("DOT row +%#x damping is %g, stock is %g", off, v, stockDampening)
		}
	}
	binary.LittleEndian.PutUint32(r[dampenerEnabledOff:], 0)
	setF32(r, dampenerDampingOff, arcDamping)
	setF32(r, dampenerDampingYOff, arcDamping)
	return nil
}

// dotTransforms zeroes the local translation of every DOT node, after checking each holds the stray stock one.
func dotTransforms(t *radpack.RowTable) error {
	if t.Size < transformMoveEnd {
		return fmt.Errorf("rows of %d bytes end before the translation at +%#x", t.Size, transformMoveEnd)
	}
	n := 0
	for i, r := range t.Rows {
		if radpack.U64(r, keyOff) != dotMagazineActor {
			continue
		}
		x, y, z := f32(r, transformMoveOff), f32(r, transformMoveYOff), f32(r, transformMoveZOff)
		if !near(x, stockMoveX) || y != 0 || !near(z, stockMoveZ) {
			return fmt.Errorf("DOT row %d translation at +%#x is (%g, %g, %g), stock is (%g, 0, %g)",
				i, transformMoveOff, x, y, z, stockMoveX, stockMoveZ)
		}
		for off := transformMoveOff; off < transformMoveEnd; off += floatLen {
			setF32(r, off, 0)
		}
		n++
	}
	if n == 0 {
		return fmt.Errorf("no rows for the DOT actor %016x", uint64(dotMagazineActor))
	}
	return nil
}
