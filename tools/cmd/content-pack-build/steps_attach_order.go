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

// healAttachOrder is the order of a player actor's rows by the script at scriptgate.ScriptOffset when the copies hang on the
// heal row (G-BUFF-ATTACH, G-MED-ATTACH): the stock heal row first, then MEDIGUN's copy, then OVERCHARGE's. Cat's pipeline runs
// fix_buff.py (attach_buff_script, fix_buff.py:453-473, which appends its row after each heal row) and then fix_medigun.py
// (attach_script, fix_medigun.py:249-258, which inserts directly after the heal row, so in front of the buff row): the actor's
// rows end heal, medigun, buff (tools/deploy_and_launch.py:316-336). That MEDIGUN comes before BUFF is read from Cat's text,
// not from data: stage82 was built without MEDIGUN. Stated as the default, S-ATTACH intent question 4.
const (
	healAttachScript = 0x6a0f689e5b91c666 // fix_buff.py:75 HEAL_SCRIPT, the stock heal script (Repair Matrix)
	mediAttachScript = 0xb874a615800a8d42 // CSymbol64("mp_combat_ability_medigun"), fix_medigun.py:47 MEDI_SCRIPT
)

var healAttachOrder = []uint64{healAttachScript, mediAttachScript, buffAttachScript}

// insertAttachRow is rows with a copy of the actor's barrier row, running script, put at script's place in attachOrder among
// that actor's rows: right after the last of the actor's rows of an earlier place (the barrier row, at the least), so it
// stands in front of any row of a later place, as Cat's second pass over the table leaves it. The copy's +0 and +32 are the
// script, the rest of the row is the barrier row's. An actor with no barrier row gets nothing and added is false. It refuses,
// naming what it found, a script that is not in attachOrder, an actor with more than one barrier row, and an actor that
// already has a row of the script. rows is not changed.
func insertAttachRow(rows [][]byte, actor, script uint64) (out [][]byte, added bool, err error) {
	return insertOrderedRow(rows, actor, script, barrierAttachScript, attachOrder)
}

// insertOrderedRow is insertAttachRow for any anchor script: the copy is of the actor's row whose script at ScriptOffset is
// anchor, and its place is script's in order. order[0] must be anchor. The rule is the same for every anchor: the copy goes
// right after the last of the actor's rows of an earlier place in order, so it is in front of a row of a later place. An
// actor with no anchor row gets nothing and added is false; an actor with two anchor rows, or a script that order does not
// place, or a row of the script already there, is refused.
func insertOrderedRow(rows [][]byte, actor, script, anchor uint64, order []uint64) (out [][]byte, added bool, err error) {
	if len(order) == 0 || order[0] != anchor {
		return nil, false, fmt.Errorf("order %x does not start with its anchor script %016x", order, anchor)
	}
	place := slices.Index(order, script)
	if place < 0 {
		return nil, false, fmt.Errorf("script %016x is not one of the rows the order places (%x)", script, order)
	}
	anchorAt, nAnchor, last := -1, 0, -1
	for i, r := range rows {
		if radpack.U64(r, keyOff) != actor {
			continue
		}
		at := slices.Index(order, radpack.U64(r, scriptgate.ScriptOffset))
		switch {
		case at < 0:
		case at == place:
			return nil, false, fmt.Errorf("actor %016x already has a row of script %016x (row %d)", actor, script, i)
		case at < place:
			last = i
		}
		if radpack.U64(r, scriptgate.ScriptOffset) == anchor {
			anchorAt, nAnchor = i, nAnchor+1
		}
	}
	switch {
	case nAnchor == 0:
		return rows, false, nil
	case nAnchor > 1:
		return nil, false, fmt.Errorf("actor %016x has %d rows of anchor script %016x, expected 1", actor, nAnchor, anchor)
	}
	at := last + 1
	c := slices.Clone(rows[anchorAt])
	radpack.SetU64(c, 0, script)
	radpack.SetU64(c, scriptgate.ScriptOffset, script)
	out = slices.Insert(slices.Clone(rows), at, c)
	return out, true, nil
}
