package main

import (
	"fmt"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

// The DOT FIELD magazine's two leftover constraint rows (#2242, PLAN-v5 G-DOT-CONS; he_is_the_cat's
// tools/fix_dot_constraints.py at 82ad7ba). The DOT magazine's actor carries a rigid-attach and a parented
// constraint row copied from the wall shield, which makes the pouch socket weld the DOT as it never welds the other
// ordnance; his tool removes both rows so the DOT handles like the translocator (`fix_dot_constraints.py:1-21`, the
// reasoning; the two tables, `:33-36`).
//
// Each of the two tables, in each of his five levels (`:37-43`, the same five as droneLevels), is a row table keyed by
// actor at +0x08 (`:58`, `u64(r, 8)`). The step drops the one row whose actor is the DOT magazine (`:58-62`) and
// writes nothing else: the shield's rows stay, so neither table empties (`:19-21`, `:61-62`). The rows have no heap
// and bind by node id, so only the header's byte size, capacity and count change (`:63-67`, which
// radpack.RowTable.Bytes does).
//
// Declared field (Step.Writes, what the stage82 oracle compares for this step; dotConstraintField): in each of the two
// tables of each level in droneLevels, the rows whose u64 at +8 is dotMagazineActor, read by readRowsWhere, which the
// step leaves as none. The header's +0x08, +0x28 and +0x30 change too, but radpack.RowTable.Bytes derives them from the
// rows left; they are not a value the step chooses, so they are not declared. Every other row byte is the stock one.
//
// Base: his tool reads the stock rows (`--src <extract root>`); it names no gun-patch dependency (PLAN-v5 S-BASE: none).
// It tolerates a table that already lacks the DOT row (`:59-60`, "already removed"); this step does not: the stock
// table has the row, so a table without it is not the stock one and is refused, as droneParticleRows refuses a second run.
const (
	rigidAttachTable = 0xdde8fffc5b4d7ad4 // CR15RigidAttachConstraintCRWin10 (`:34`)
	parentedTable    = 0xf2676c58c07bab0c // CR15ParentedConstraintCRWin10 (`:35`)
	// The DOT magazine's actor is dotMagazineActor (steps_dot_stick.go), the same id as his DOT_ACTOR (`:44`).
	dotConstraintName = "DOT FIELD magazine drops its leftover %s rows"
)

func init() {
	steps = append(steps,
		Step{Name: fmt.Sprintf(dotConstraintName, "rigid-attach"), Apply: forEachLevel(rigidAttachTable, dropDotConstraint),
			Writes: []Field{dotConstraintField("rigid-attach", rigidAttachTable)}},
		Step{Name: fmt.Sprintf(dotConstraintName, "parented-constraint"), Apply: forEachLevel(parentedTable, dropDotConstraint),
			Writes: []Field{dotConstraintField("parented-constraint", parentedTable)}},
	)
}

// dotConstraintField is the step's declared field for one table: the DOT magazine's rows, none after the step.
func dotConstraintField(kind string, typ uint64) Field {
	return Field{Name: kind + " rows of the DOT magazine actor (none after the step)", Set: Sets[0], Type: typ,
		Records: droneLevels, Read: readRowsWhere(keyOff, dotMagazineActor)}
}

// dropDotConstraint removes the DOT magazine's one row, after checking the table is shaped as the stock one is: rows
// wide enough to hold the actor id, no tail (his framing check, `:53-55`, requires the table to end with its rows),
// exactly one DOT row and at least one other (`:61-62`).
func dropDotConstraint(t *radpack.RowTable) error {
	if t.Size < keyOff+8 {
		return fmt.Errorf("constraint rows of %d bytes end before the actor id at +%#x", t.Size, keyOff)
	}
	if len(t.Tail) != 0 {
		return fmt.Errorf("the table has a %d-byte tail after its rows; the stock one has none", len(t.Tail))
	}
	at, n := -1, 0
	for i, r := range t.Rows {
		if radpack.U64(r, keyOff) == dotMagazineActor {
			at = i
			n++
		}
	}
	if n != 1 {
		return fmt.Errorf("%d rows of the DOT magazine %016x, the stock table has exactly 1", n, uint64(dotMagazineActor))
	}
	if len(t.Rows) < 2 {
		return fmt.Errorf("the DOT magazine's is the table's only row; removing it would empty the table")
	}
	t.Rows = append(t.Rows[:at:at], t.Rows[at+1:]...)
	return nil
}
