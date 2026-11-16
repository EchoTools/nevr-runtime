package main

import (
	"bytes"
	"fmt"
	"slices"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

// OVERCHARGE's data (PLAN-v5 inventory row 14, piece G-BUFF), from he_is_the_cat's tools/fix_buff.py at 82ad7ba. Two
// records change; each step checks what it expects of the stock record first and refuses when it differs.
//
//  1. mp_equipment lists OVERCHARGE: gear_table.tacmods gets {"name":"buff","display_name":"OVERCHARGE"} after the stock
//     entries (fix_buff.py:91 BUFF_GEAR, :504-511: his tool renames a "buff" entry that is already there and appends one
//     that is not; the player's stock list has none, so only the append applies, and a list that already has "buff" is
//     refused, because it is not the stock list this step is written over). The list must END with smoke, buff, point
//     (tacModOrder, steps_tacmods.go, from src/runtime/patch/ec_rules.h kNewTacMods), so insertTacmod puts buff in front
//     of a point entry when the list has one and appends it otherwise. The copy of Repair Matrix's script that is
//     OVERCHARGE's own, and its tuning ability_properties.buff (fix_buff.py:512-526), are not this step's: the script is
//     G3's and H-BUFF's, the tuning is main's run-time merge (src/core/combat_balance/balance_values.h, ADR 0039), which
//     writes ability_properties.buff already, so no Go step writes a key of it.
//  2. The heal bubble table of the global level gets OVERCHARGE's own bubble (fix_buff.py:437-450 add_bubbles, :577-581):
//     each of the three actors that carry Repair Matrix's bubble (the two combat players, fix_smoke.py:75 PLAYER_ACTORS,
//     and b113997837c29003, fix_buff.py:89 OC_ACTORS) gets a copy of its row under the component id
//     CSymbol64("echocombat_overcharge_bubble") (fix_buff.py:84), appended after the stock rows, so OVERCHARGE and Repair
//     Matrix never share one bubble's settings (the heal script's handle swap is H-BUFF-b1/b2's).
//
// Base: both records are the stock ones (S-BASE: his gun patch has stock tacmods and no bubble table). Nothing of the
// gun patch is imported.
//
// Declared fields (Step.Writes, what the stage82 oracle compares):
//   - mp_equipment f8d40525c88885d8: the gear_table.tacmods entry named "buff" (readTacmod).
//   - 1ad01a2a91709a1a of 3f9915d3001dc28e: the rows of the OVERCHARGE bubble, the three appended 72-byte rows, each its
//     actor's stock row with the u64 at +0 (the component id) set to the OVERCHARGE bubble (readRowsWhere). The header's
//     byte size (+0x08), capacity (+0x28) and count (+0x30) follow from the rows and are not declared.
//
// globalLevel, healBubbleTable and healBubble are also G-MED's (the heal bubble of the same table).
const (
	healBubbleTable = 0x1ad01a2a91709a1a // CSymbol64("CR15NetHealBubbleCRWin10"), fix_buff.py:86
	globalLevel     = 0x3f9915d3001dc28e // the global level, fix_smoke.py:71 GLOBAL_LEVEL
	healBubble      = 0xb1fda2a83f3debd1 // the stock heal bubble component id, fix_buff.py:83 HEAL_BUBBLE
	ocBubble        = 0xb9833267495be341 // CSymbol64("echocombat_overcharge_bubble"), fix_buff.py:84
	bubbleRowSize   = 72                 // measured on the shipped table: 0xd8 bytes of rows, count 3
	bubbleActorOff  = 0x08               // the actor a row is for
	bubbleIDOff     = 0x00               // the component id of a row
)

// ocActors are the actors that get OVERCHARGE's bubble (fix_buff.py:89 OC_ACTORS). Which rows are copied is this set; the
// order they are appended in is the table's own row order (addOverchargeBubbles).
var ocActors = []uint64{0x91d8de0806964a11, 0xb113997837c29003, 0xf1e70acd0dde0312}

var buffTacmod = gearEntry{Name: "buff", DisplayName: "OVERCHARGE"}

const (
	buffTacmodStep = "equipment lists offer the OVERCHARGE tac mod"
	buffBubbleStep = "each combat player's actor gets OVERCHARGE's own heal bubble"
)

func init() {
	steps = append(steps,
		Step{Name: buffTacmodStep, Apply: buffTacmodList, PastGunPatch: true,
			Writes: []Field{{Name: "gear_table.tacmods: the buff entry", Set: Sets[0], Type: jsonResourceType,
				Records: []uint64{equipmentName}, Read: readTacmod(buffTacmod.Name)}}},
		Step{Name: buffBubbleStep, Apply: buffBubbles,
			Writes: []Field{{Name: "heal bubble rows of the OVERCHARGE bubble", Set: Sets[0], Type: healBubbleTable,
				Records: []uint64{globalLevel}, Read: readRowsWhere(bubbleIDOff, ocBubble)}}},
	)
}

func buffTacmodList(p *Pack) error {
	var name uint64 = equipmentName
	b, err := p.Read(Sets[0], jsonResourceType, int64(name))
	if err != nil {
		return err
	}
	out, err := appendTacmod(b)
	if err != nil {
		return fmt.Errorf("mp_equipment: %w", err)
	}
	return p.Put(Sets[0], jsonResourceType, int64(name), out)
}

// appendTacmod is the resource with OVERCHARGE put into gear_table.tacmods at its rank (insertTacmod). It refuses a
// list without "heal" (Repair Matrix, the tac mod OVERCHARGE is a copy of) or with "buff" already in it.
func appendTacmod(b []byte) ([]byte, error) {
	return insertTacmod(b, buffTacmod, func(names map[string]bool) error {
		if !names["heal"] {
			return fmt.Errorf("gear_table.tacmods lists no heal (REPAIR MATRIX), which OVERCHARGE copies: not the stock list")
		}
		return nil
	})
}

func buffBubbles(p *Pack) error {
	b, err := p.Read(Sets[0], int64(healBubbleTable), int64(globalLevel))
	if err != nil {
		return err
	}
	t, err := radpack.ParseRowTable(b)
	if err != nil {
		return fmt.Errorf("heal bubble table: %w", err)
	}
	if err := addOverchargeBubbles(t); err != nil {
		return fmt.Errorf("heal bubble table: %w", err)
	}
	return p.Put(Sets[0], int64(healBubbleTable), int64(globalLevel), t.Bytes())
}

// addOverchargeBubbles appends, for each OVERCHARGE actor, a copy of its heal bubble row under the OVERCHARGE bubble id,
// in the table's row order: his add_bubbles (fix_buff.py:443-450) walks the stock rows and appends each heal bubble row
// of an OC_ACTORS actor as it meets it. Expected of the stock table: rows of bubbleRowSize bytes, one heal bubble row for
// each of ocActors, and no row of the OVERCHARGE bubble; anything else is refused with the actor and the count found.
func addOverchargeBubbles(t *radpack.RowTable) error {
	if t.Size != bubbleRowSize {
		return fmt.Errorf("rows of %d bytes, want %d", t.Size, bubbleRowSize)
	}
	have := map[uint64]int{}
	var adds [][]byte
	for _, r := range t.Rows {
		actor := radpack.U64(r, bubbleActorOff)
		if !slices.Contains(ocActors, actor) {
			continue
		}
		switch radpack.U64(r, bubbleIDOff) {
		case ocBubble:
			return fmt.Errorf("actor %016x already has an OVERCHARGE bubble row (%016x): not the stock table", actor, uint64(ocBubble))
		case healBubble:
			have[actor]++
			c := bytes.Clone(r)
			radpack.SetU64(c, bubbleIDOff, ocBubble)
			adds = append(adds, c)
		}
	}
	for _, actor := range ocActors {
		if have[actor] != 1 {
			return fmt.Errorf("actor %016x has %d heal bubble rows (%016x), want 1", actor, have[actor], uint64(healBubble))
		}
	}
	t.Rows = append(t.Rows, adds...)
	return nil
}
