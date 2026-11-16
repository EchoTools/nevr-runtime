package main

import (
	"fmt"
	"slices"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
	"github.com/EchoTools/nevr-runtime/tools/internal/scriptgate"
)

// The order of a player actor's CScriptCR rows in the global level's table (#2242). The steps that add a row to the table
// (the point attach and the veil attach) run in file-name order, which is not the order the rows belong in, and a row's
// position may matter to the engine, so the order is stated here, once, and each step puts its row at its place with
// insertAttachRow: never left to the order the files initialise in.
//
// The order is he_is_the_cat's build's, the one known to work in the game: its pipeline runs fix_smoke.py (the veil's row,
// attach_smoke_script, fix_smoke.py:272-299) and then fix_point.py (the point's row, attach_point_script, fix_point.py:83-106,
// tools/deploy_and_launch.py:312 then :323), each placing its row right after the actor's barrier row, so the actor's rows
// end barrier, point, veil.
const veilAttachScript = 0x94ba7ca09dcf7b0b // fix_smoke.py:74 SMOKE_SCRIPT, CSymbol64("mp_combat_ability_smoke")

// attachOrder is the order of a player actor's rows by the script at scriptgate.ScriptOffset: the stock barrier row first.
var attachOrder = []uint64{barrierAttachScript, pointAttachScript, veilAttachScript}

// insertAttachRow is rows with a copy of the actor's barrier row, running script, put at script's place in attachOrder among
// that actor's rows: right after the last of the actor's rows of an earlier place (the barrier row, at the least), so it
// stands in front of any row of a later place, as Cat's second pass over the table leaves it. The copy's +0 and +32 are the script, the rest of the row is the barrier
// row's. An actor with no barrier row gets nothing and added is false. It refuses, naming what it found, a script that is
// not in attachOrder, an actor with more than one barrier row, and an actor that already has a row of the script. rows is
// not changed.
func insertAttachRow(rows [][]byte, actor, script uint64) (out [][]byte, added bool, err error) {
	place := slices.Index(attachOrder, script)
	if place < 0 {
		return nil, false, fmt.Errorf("script %016x is not one of the rows the attach order places (%x)", script, attachOrder)
	}
	barrier, nBarrier, last := -1, 0, -1
	for i, r := range rows {
		if radpack.U64(r, keyOff) != actor {
			continue
		}
		at := slices.Index(attachOrder, radpack.U64(r, scriptgate.ScriptOffset))
		switch {
		case at < 0:
		case at == place:
			return nil, false, fmt.Errorf("actor %016x already has a row of script %016x (row %d)", actor, script, i)
		case at < place:
			last = i
		}
		if radpack.U64(r, scriptgate.ScriptOffset) == barrierAttachScript {
			barrier, nBarrier = i, nBarrier+1
		}
	}
	switch {
	case nBarrier == 0:
		return rows, false, nil
	case nBarrier > 1:
		return nil, false, fmt.Errorf("actor %016x has %d barrier-script rows, expected 1", actor, nBarrier)
	}
	at := last + 1
	c := slices.Clone(rows[barrier])
	radpack.SetU64(c, 0, script)
	radpack.SetU64(c, scriptgate.ScriptOffset, script)
	out = slices.Insert(slices.Clone(rows), at, c)
	return out, true, nil
}
