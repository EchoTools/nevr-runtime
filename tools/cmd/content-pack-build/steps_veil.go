package main

import "fmt"

// UMBRAL VEIL, the data half (PLAN-v5 G-VEIL, row 11). he_is_the_cat's tool fix_smoke.py registers the cut `smoke` tac mod
// as the fifth tac mod: its main() reads mp_equipment and, when gear_table.tacmods has no entry named "smoke", appends
// {"name": "smoke", "display_name": "UMBRAL VEIL"} (fix_smoke.py:76 SMOKE_GEAR, :335-337, at 82ad7ba). That is the only
// edit of the equipment record: fix_smoke.py's other outputs are scripts, the CScriptResource entry and the CScriptCR attach
// (below), and it states that the veil's tuning "is already in ability_properties.smoke" (:20-21), the
// game's own record.
//
// What this step does not write, and why:
//   - mp_weapon_settings: ability_properties.smoke's veil_* look keys (fix_smoke.py:65 VEIL: his config's look, which his build merges
//     into the record) are merged at run time by src/core/combat_balance/balance_values.h; a field that merge writes is never written by a Go step too
//     (ADR 0039). The settings and the explosion row of PLAN-v5 10.1's "veil and point tacmods, settings, explosion row" are
//     NEUTRON's (fix_point.py:123-127 and :60-80, :149-155), G-POINT.
//   - the CScriptCR row that attaches the veil's script copy 94ba7ca09dcf7b0b, the rekey, and every script-image patch
//     (G3, G-VEIL-ATTACH, the H pieces).
//
// Base: the player's stock mp_equipment. his build stages the gun patch's record (fix_dot.py:163, :220-223), but the gun patch's
// tacmods list equals the stock one (measured: the gun patch's mp_equipment differs from the stock one by the weapon and ordnance entries
// steps_equipment.go adds, and its tacmods list is unchanged), so no gun-patch value is imported here.
//
// Declared fields (Step.Writes, what the stage82 oracle compares): mp_equipment f8d40525c88885d8, the gear_table.tacmods
// entry named "smoke" (readTacmod, steps_tacmods.go).
//
// Order: the order is read by the runtime, so it is placed, not left to whichever step runs first. The tac-mod hide on a
// server without EchoCombat (src/runtime/patch/ec_rules.h, kNewTacMods {"smoke","buff","point"}, OursAtEnd and PlanTacMods)
// changes the list only when its last three entries are smoke, buff, point in that order; any other order gives an empty plan
// and the three are never hidden. insertTacmod (steps_tacmods.go) places each tac mod by its rank in tacModOrder: smoke ranks
// first, so it goes in front of the first buff or point entry when either is listed (steps_buff.go and steps_point.go run
// before this file's init, by file name), else last. his own order is the same (fix_smoke.py --with-buff --with-point, at
// 82ad7ba).
const veilStepName = "equipment tacmods offer UMBRAL VEIL"

var veilGear = gearEntry{Name: "smoke", DisplayName: "UMBRAL VEIL"} // fix_smoke.py:76, CSymbol64("smoke") 0xe32dc7c9d4804fb5 (:77)

func init() {
	steps = append(steps, Step{Name: veilStepName, Apply: veilTacmod, PastGunPatch: true,
		Writes: []Field{{Name: "gear_table.tacmods: the smoke entry", Set: Sets[0], Type: jsonResourceType,
			Records: []uint64{equipmentName}, Read: readTacmod(veilGear.Name)}}})
}

func veilTacmod(p *Pack) error {
	var name uint64 = equipmentName // not a constant: the id overflows a constant int64 conversion
	b, err := p.Read(Sets[0], jsonResourceType, int64(name))
	if err != nil {
		return err
	}
	out, err := placeVeil(b)
	if err != nil {
		return fmt.Errorf("mp_equipment: %w", err)
	}
	return p.Put(Sets[0], jsonResourceType, int64(name), out)
}

// placeVeil is the mp_equipment resource with the veil put into gear_table.tacmods at its rank (insertTacmod): before the
// first of buff or point when either is listed, else last, the header kept and its length set. A resource of another
// layout, a record with no tacmods list, or one that already lists the veil is refused, and nothing is edited.
func placeVeil(b []byte) ([]byte, error) {
	return insertTacmod(b, veilGear, nil)
}
