package main

import (
	"fmt"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
	"github.com/EchoTools/nevr-runtime/tools/internal/scriptgate"
)

// NEUTRON's script attach (#2242, PLAN-v7 12.3 G-POINT-ATTACH; PLAN-v5 10.3 G2): the CScriptCR rows of he_is_the_cat's
// tools/fix_point.py attach_point_script (at 82ad7ba, :83-106). The global level's table (scriptTable, scriptgate's
// RowStride 720 bytes a row) gets, after each player actor's barrier-script row, a copy with the NEUTRON script 94ba7ca39fc97e1a
// at +0 and +32 (the script G2 reads is at +32, scriptgate.ScriptOffset). The copy's script is the barrier script's copy the
// runtime makes on the player's machine: the step names it with Pack.Rekey, so the label's `scripts` list carries it and G2
// (cargo-pack) finds it delivered. No script byte is written here (the copy is made by the runtime, R12).
//
// Refused, writing nothing: a table that is not 720-byte rows; a player actor without exactly one barrier-script row (the
// stock shape fix_point.py ran on); a table that already has a row of the point script (fix_point.py skips a second run,
// this step refuses it, so a double run is seen); a stock set that already lists the copy's CScriptResourceWin10 entry.
//
// Declared fields (Step.Writes): the CScriptCR rows of the point script in the global level (readRowsWhere at +32), and the
// copy's CScriptResourceWin10 entry (scriptgate.ScriptType, four zero bytes: fix_point.py:136-137 writes it, and the
// level resolves the script before its DLL loads). The entry is written after the table and before the copy is listed.
// Order: After names the barrier wall step, so this step runs after it as the old production list had it. No shared record
// needs that edge (TestEverySharedRecordIsOrdered does not require it). The rows' order within an actor is not the order
// steps run in: it is attachOrder (steps_attach_order.go: barrier, point, veil), which insertAttachRow applies, so a sibling
// attach into the same table (G-VEIL-ATTACH) lands at its place whichever step runs first.
const (
	playerActorAID = 0x91d8de0806964a11 // fix_smoke.py:75 PLAYER_ACTORS
	playerActorBID = 0xf1e70acd0dde0312 // fix_smoke.py:75 PLAYER_ACTORS
)

var playerActorIDs = []uint64{playerActorAID, playerActorBID}

const (
	pointAttachScript   = 0x94ba7ca39fc97e1a // fix_point.py:POINT_SCRIPT, CSymbol64("mp_combat_ability_point")
	barrierAttachScript = 0xb87ba2039b049d5e // fix_smoke.py:73 BARRIER_SCRIPT, the stock barrier script the copy is made from
	pointAttachStep     = "point script attaches to the player actors' barrier rows"
)

func init() {
	steps = append(steps, Step{Name: pointAttachStep, Apply: pointAttach, After: []string{pointExplosionStep},
		Writes: []Field{
			{Name: "CScriptCR rows of the point script, global level", Set: Sets[0], Type: scriptTable,
				Records: []uint64{globalLevel}, Read: readRowsWhere(scriptgate.ScriptOffset, pointAttachScript)},
			{Name: "CScriptResourceWin10 entry of the point script copy, four zero bytes", Set: Sets[0], Type: scriptgate.ScriptType,
				Records: []uint64{pointAttachScript}, Read: readScriptResourceEntry},
		}})
}

// pointAttach adds the point script's rows to the global level's CScriptCR table, writes the copy's entry and names the copy
// in the label. The stock set's entry is checked before anything is written, so a refusal leaves the pack as it was.
func pointAttach(p *Pack) error {
	var table, level uint64 = scriptTable, globalLevel                    // not constants: the ids overflow a constant int64 conversion
	var entry, copyName uint64 = scriptgate.ScriptType, pointAttachScript // not constants: the ids overflow a constant int64 conversion
	b, err := p.Read(Sets[0], int64(table), int64(level))
	if err != nil {
		return err
	}
	t, err := radpack.ParseRowTable(b)
	if err != nil {
		return fmt.Errorf("CScriptCR global level: %w", err)
	}
	if err := attachPointRows(t); err != nil {
		return fmt.Errorf("CScriptCR global level: %w", err)
	}
	if _, found, err := p.sets[Sets[0]].Read(int64(entry), int64(copyName)); err != nil {
		return err
	} else if found {
		return fmt.Errorf("the stock set already lists the CScriptResourceWin10 entry of %016x; the copy would replace it", copyName)
	}
	if err := p.Put(Sets[0], int64(table), int64(level), t.Bytes()); err != nil {
		return err
	}
	if err := p.Rekey(fmt.Sprintf("%016x", copyName), fmt.Sprintf("%016x", uint64(barrierAttachScript))); err != nil {
		return err
	}
	return p.Put(Sets[0], int64(entry), int64(copyName), make([]byte, scriptResourceEntry))
}

// attachPointRows inserts the point script's row for each player actor at its place in attachOrder: right after the barrier row, in front of a veil row (fix_point.py:90-101), in place.
func attachPointRows(t *radpack.RowTable) error {
	if t.Size != scriptgate.RowStride {
		return fmt.Errorf("rows of %d bytes, expected %d (fix_point.py:83-106)", t.Size, scriptgate.RowStride)
	}
	for _, r := range t.Rows {
		if radpack.U64(r, scriptgate.ScriptOffset) == pointAttachScript {
			return fmt.Errorf("the table already has the point script's rows")
		}
	}
	rows := t.Rows
	for _, actor := range playerActorIDs {
		var added bool
		var err error
		if rows, added, err = insertAttachRow(rows, actor, pointAttachScript); err != nil {
			return err
		}
		if !added {
			return fmt.Errorf("player actor %016x has no barrier-script row, expected 1 (fix_point.py:90-101)", actor)
		}
	}
	t.Rows = rows
	return nil
}
