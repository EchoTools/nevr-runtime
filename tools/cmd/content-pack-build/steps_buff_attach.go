package main

import (
	"fmt"
	"slices"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
	"github.com/EchoTools/nevr-runtime/tools/internal/scriptgate"
)

// OVERCHARGE's script copy, attached (#2242, PLAN-v7 12.3 G-BUFF-ATTACH; PLAN-v5 inventory row 14). The copy is
// 4b65d1b0088d2682 (fix_buff.py:76 BUFF_SCRIPT, CSymbol64("mp_combat_ability_buff")), made from the stock heal script
// 6a0f689e5b91c666 (fix_buff.py:75 HEAL_SCRIPT). The step does what Cat's attach_buff_script (fix_buff.py:453-480) does,
// in three parts, and refuses before it writes anything:
//
//  1. CScriptCR rows (scriptTable) of the global level: for each OVERCHARGE actor (ocActors, fix_buff.py:89), a copy of its
//     heal-script row is put at its place in healAttachOrder (steps_attach_order.go: heal, medigun, buff), with +0 and +32
//     set to the copy. The copy's +8 (the actor) and the rest of the row are the heal row's. The helper is insertOrderedRow,
//     shared with the point and veil attaches.
//  2. The copy's CScriptResourceWin10 entry (scriptgate.ScriptType): four zero bytes (fix_buff.py:566-567 writes it for each
//     copy), so the level resolves the script before it loads the DLL.
//  3. The copy is listed with Pack.Rekey, so the label's scripts list names it and G2 finds it delivered. No script byte
//     is written here; the copy's behaviour is H-BUFF's hooks.
//
// Refused, writing nothing: a table that is not scriptgate.RowStride bytes; a table that already runs the copy (Cat skips it,
// fix_buff.py:465-466, this step refuses so a second run is named); an OVERCHARGE actor without exactly one heal row, or a
// heal row whose +0 is not its +32; a donor heal row with a non-empty descriptor (size, capacity or count of any of the
// twelve nested descriptors at +48+56k, which the copy would share; Cat's tool checks only the size, fix_buff.py:467-469);
// an entry already in the stock set. Cat's tool and this step agree on every table the stock build has (S-ATTACH 0.2).
//
// Declared fields (Step.Writes, what the stage82 oracle compares): the global level's CScriptCR rows of the copy, and the
// copy's CScriptResource entry.
//
// Order: the rows of the copy are placed by healAttachOrder, not by the order the steps run in. The step writes the same
// global table as the point and veil attaches (steps_point_attach.go, steps_veil_attach.go), and each of them places its
// rows by its own anchor's order, so the three are OrderFree with each other: TestBuffAndTheAttachOrdersAgree shows both
// orders give the same bytes.
const (
	buffAttachScript = 0x4b65d1b0088d2682 // fix_buff.py:76 BUFF_SCRIPT, CSymbol64("mp_combat_ability_buff")
	buffAttachStep   = "OVERCHARGE attaches its script copy to each combat player's heal row"
)

func init() {
	steps = append(steps, Step{Name: buffAttachStep, Apply: buffAttach,
		OrderFree: []string{pointAttachStep, veilAttachStep},
		Writes: []Field{
			{Name: "CScriptCR rows of the OVERCHARGE script copy, global level", Set: Sets[0], Type: scriptTable,
				Records: []uint64{globalLevel}, Read: readRowsWhere(scriptgate.ScriptOffset, buffAttachScript)},
			{Name: "CScriptResourceWin10 entry of the OVERCHARGE script copy, four zero bytes", Set: Sets[0], Type: scriptgate.ScriptType,
				Records: []uint64{buffAttachScript}, Read: readScriptResourceEntry},
		}})
}

// buffAttach attaches the copy to the global level's table, writes its entry and lists the copy (Rekey). Every check runs
// before the first Rekey or Put, so a refusal leaves the pack as it was.
func buffAttach(p *Pack) error {
	var table, level uint64 = scriptTable, globalLevel // not constants: the ids overflow a constant int64 conversion
	var entry uint64 = scriptgate.ScriptType
	b, err := p.Read(Sets[0], int64(table), int64(level))
	if err != nil {
		return err
	}
	out, err := attachBuffRows(b)
	if err != nil {
		return fmt.Errorf("CScriptCR global level %016x: %w", level, err)
	}
	if _, found, err := p.sets[Sets[0]].Read(int64(entry), int64(buffAttachScript)); err != nil {
		return err
	} else if found {
		return fmt.Errorf("the stock set already lists the CScriptResourceWin10 entry of %016x; the copy would replace it", uint64(buffAttachScript))
	}
	if err := p.Rekey(fmt.Sprintf("%016x", uint64(buffAttachScript)), fmt.Sprintf("%016x", uint64(healAttachScript))); err != nil {
		return err
	}
	if err := p.Put(Sets[0], int64(table), int64(level), out); err != nil {
		return err
	}
	return p.Put(Sets[0], int64(entry), int64(buffAttachScript), make([]byte, scriptResourceEntry))
}

// attachBuffRows is the CScriptCR table with the copy's row for each OVERCHARGE actor, at its place in healAttachOrder. It
// refuses, naming the actor and what it found, and returns no bytes, when the table is not the stock one (the rules above).
// The header and the tail are kept, and the header's counts are set from the rows (radpack.RowTable.Bytes).
func attachBuffRows(b []byte) ([]byte, error) {
	t, err := radpack.ParseRowTable(b)
	if err != nil {
		return nil, fmt.Errorf("not a row table: %w", err)
	}
	if t.Size != scriptgate.RowStride {
		return nil, fmt.Errorf("rows of %d bytes, expected %d", t.Size, scriptgate.RowStride)
	}
	if i := t.Find(scriptgate.ScriptOffset, buffAttachScript); i >= 0 {
		return nil, fmt.Errorf("row %d already runs the OVERCHARGE copy %016x; expected the stock table, which does not", i, uint64(buffAttachScript))
	}
	for i, r := range t.Rows {
		if !slices.Contains(ocActors, radpack.U64(r, keyOff)) || radpack.U64(r, scriptgate.ScriptOffset) != healAttachScript {
			continue
		}
		if id := radpack.U64(r, scriptNameOff); id != healAttachScript {
			return nil, fmt.Errorf("heal row %d of actor %016x has +0 %016x, expected %016x (its +32)", i, radpack.U64(r, keyOff), id, uint64(healAttachScript))
		}
		if off := donorPoolOff(r); off >= 0 {
			return nil, fmt.Errorf("heal row %d of actor %016x carries pool data at +%#x: %016x, expected 0",
				i, radpack.U64(r, keyOff), off, radpack.U64(r, off))
		}
	}
	rows := t.Rows
	for _, actor := range ocActors {
		var added bool
		if rows, added, err = insertOrderedRow(rows, actor, buffAttachScript, healAttachScript, healAttachOrder); err != nil {
			return nil, err
		}
		if !added {
			return nil, fmt.Errorf("actor %016x has no heal-script row (%016x), expected 1", actor, uint64(healAttachScript))
		}
	}
	t.Rows = rows
	return t.Bytes(), nil
}

// donorPoolOff is the offset of the first size, capacity or count of the row's twelve nested descriptors (at
// mtScriptTablesAt + mtDescLen*k) that is not zero, or -1. A copy of the row would share that pool, which neither this
// step nor Cat's writes.
func donorPoolOff(r []byte) int {
	for k := range mtScriptTables {
		d := mtScriptTablesAt + mtDescLen*k
		for _, f := range []int{mtDescSizeAt, mtDescCapAt, mtDescCountAt} {
			if radpack.U64(r, d+f) != 0 {
				return d + f
			}
		}
	}
	return -1
}
