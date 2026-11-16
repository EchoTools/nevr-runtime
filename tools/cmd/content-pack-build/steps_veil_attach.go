package main

import (
	"fmt"
	"slices"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
	"github.com/EchoTools/nevr-runtime/tools/internal/scriptgate"
)

// UMBRAL VEIL, the attach (PLAN-v7 12.3 G-VEIL-ATTACH: "the label names the copy, passes G2"; PLAN-v5 row 11). Two parts, both
// from he_is_the_cat's tools/fix_smoke.py at 82ad7ba, read as text:
//   - the copy: 94ba7ca09dcf7b0b, a copy of the stock barrier script b87ba2039b049d5e (fix_smoke.py:73-74). Pack.Rekey lists it
//     in the label's `scripts` list (10.2); no script byte goes into the pack.
//   - the rows: in the global level's CScriptCR table 3f9915d3001dc28e (fix_smoke.py:71, :411-414), each row of a player actor
//     (PLAYER_ACTORS, fix_smoke.py:75) that runs the barrier script gets a copy placed after it (and after the point's row when the table has one: attachOrder), running the veil copy
//     (attach_smoke_script, fix_smoke.py:272-299): the copy's +0 and +32 are the copy's name, the rest of the row is kept.
//
// The controller patch (patch_controller, fix_smoke.py:266) and the barrier and wall script images are not here: they are
// script-image patches, the H pieces (10.1: H-CTRL, H-BARR, H-WALL-a).
//
// The step refuses, before it writes anything (4.4), when: the table is not rows of scriptgate.RowStride bytes; a row already
// runs the copy; a barrier row of a player actor carries pool data (Cat's tool refuses the same); or no barrier row of a
// player actor is there, so the attach would add nothing. A missing stock barrier script refuses through Rekey.
//
// The table is the same layout as the other CScriptCR tables (radpack.RowTable: a 0x38-byte header, rows, a tail kept as it
// is), and scriptgate reads the rows this step writes (G2: the label's scripts list holds the copy, so the rows pass).
//
// Declared fields (Step.Writes, what the stage82 oracle compares): the global level's CScriptCR rows whose script is the copy.
const (
	veilCopy        = veilAttachScript // fix_smoke.py:74, SMOKE_SCRIPT (CSymbol64("mp_combat_ability_smoke")), steps_attach_order.go
	veilCopyName    = "94ba7ca09dcf7b0b"
	veilBarrier     = barrierAttachScript // fix_smoke.py:73, BARRIER_SCRIPT: the stock script the copy is made from
	veilBarrierName = "b87ba2039b049d5e"
	veilActorOff    = keyOff // the row's actor, Cat's attach_smoke_script reads u64(e, 8)
)

// veilPlayerActors is fix_smoke.py:75 PLAYER_ACTORS (steps_point_attach.go).
var veilPlayerActors = playerActorIDs

// The pool's slots: fix_smoke.py's attach_smoke_script checks +8 of each of the pool's twelve slots (t in 48, 104, ..., 664,
// read at t+8), so a barrier row must leave u64 at veilPoolFirst + i*veilPoolStride zero, for i below veilPoolCount.
const (
	veilPoolFirst  = 56 // t = 48 read at t+8
	veilPoolStride = 56 // the next slot is 56 bytes on
	veilPoolCount  = 12
)

// veilPoolSlots is the list of the pool's offsets a barrier row must leave zero.
func veilPoolSlots() []int {
	s := make([]int, veilPoolCount)
	for i := range s {
		s[i] = veilPoolFirst + i*veilPoolStride
	}
	return s
}

const veilAttachStep = "UMBRAL VEIL attaches its script copy in the global level"

// The veil's row and the point's row are written to the same table. Both steps place their rows by attachOrder and
// insertAttachRow, so the table is the same whichever runs first: OrderFree says so, and TestThePointAndVeilAttaches...
// shows both orders.
func init() {
	steps = append(steps, Step{Name: veilAttachStep, Apply: veilAttach, OrderFree: []string{pointAttachStep},
		Writes: []Field{{Name: "CScriptCR of the global level: the rows that run the veil copy", Set: Sets[0], Type: scriptTable,
			Records: []uint64{globalLevel}, Read: readRowsWhere(scriptgate.ScriptOffset, veilCopy)}}})
}

// veilAttach reads the global level's CScriptCR table, attaches the copy (attachVeil), and then lists the copy in the
// label's scripts list (Rekey). The table is refused before any change; Rekey refuses before the table is written.
func veilAttach(p *Pack) error {
	var typ uint64 = scriptTable // not a constant: the table id overflows a constant int64 conversion (steps_veil.go)
	b, err := p.Read(Sets[0], int64(typ), int64(globalLevel))
	if err != nil {
		return err
	}
	out, err := attachVeil(b)
	if err != nil {
		return fmt.Errorf("CScriptCR of the global level %016x: %w", uint64(globalLevel), err)
	}
	if err := p.Rekey(veilCopyName, veilBarrierName); err != nil {
		return err
	}
	return p.Put(Sets[0], int64(typ), int64(globalLevel), out)
}

// attachVeil is the CScriptCR table with a copy of the veil row for each player actor, put at its place in attachOrder
// (steps_attach_order.go: after the barrier row and after the point row when there is one), the header and the
// tail kept and the count set from the rows. A table that does not fit the rule is refused, naming what it expected and what
// it found, and no bytes are returned.
func attachVeil(b []byte) ([]byte, error) {
	t, err := radpack.ParseRowTable(b)
	if err != nil {
		return nil, fmt.Errorf("not a row table: %w", err)
	}
	if t.Size != scriptgate.RowStride {
		return nil, fmt.Errorf("rows of %d bytes, expected %d", t.Size, scriptgate.RowStride)
	}
	if i := t.Find(scriptgate.ScriptOffset, veilCopy); i >= 0 {
		return nil, fmt.Errorf("row %d already runs the veil copy %016x; expected the stock table, which does not", i, uint64(veilCopy))
	}
	for i, r := range t.Rows {
		if !slices.Contains(veilPlayerActors, radpack.U64(r, veilActorOff)) || radpack.U64(r, scriptgate.ScriptOffset) != veilBarrier {
			continue
		}
		for _, off := range veilPoolSlots() {
			if v := radpack.U64(r, off); v != 0 {
				return nil, fmt.Errorf("row %d (barrier of actor %016x) carries pool data at +%#x: %016x, expected 0",
					i, radpack.U64(r, veilActorOff), off, v)
			}
		}
	}
	rows, added := t.Rows, 0
	for _, actor := range veilPlayerActors {
		var ok bool
		if rows, ok, err = insertAttachRow(rows, actor, veilCopy); err != nil {
			return nil, err
		}
		if ok {
			added++
		}
	}
	if added == 0 {
		return nil, fmt.Errorf("no row of %d runs barrier script %016x for a player actor: expected at least one, found 0", len(t.Rows), uint64(veilBarrier))
	}
	t.Rows = rows
	return t.Bytes(), nil
}
