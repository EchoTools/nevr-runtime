package main

import (
	"encoding/binary"
	"fmt"
	"math"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

// The DRONE's health (PLAN-v5 inventory row 3, piece G-DRONE-H). he_is_the_cat's tools/fix_drone_health.py at 82ad7ba
// (the whole file; `patch` is :44-54, the constants :31-40) edits each combat level's CR15NetDamageableCRWin10 table
// (bd1868f576836696): a record per damageable actor, the actor id at +8, its health as a float at +0x24. Every ordnance's
// stock health is 9999, the drone's too, so a drone can never be shot down; his tool sets the drone's to
// grenade_properties.drone.health.
//
// The value is 70, not 60: his config/mp_weapon_settings.json at 82ad7ba line 30 has `"health": 70` under
// grenade_properties.drone, and src/core/combat_balance/balance_values.h (lines 37-39) carries the same table. His tool's
// `get("health", 60)` (:43) is only the fallback for a config without the key, which his build never uses.
//
// Declared field (what this step writes, for the stage82 oracle): in the damageable table of each of the five drone
// levels, the f32 at +0x24 of the one record whose u64 at +8 is the drone actor 10e40287a9bf5a13, and nothing else.
//
// Base: the player's stock table. His tool reads stock rows (not the gun patch's) for this record: it requires the
// stock 9999.0, so no gun-patch value is relied on (PLAN-v5 4.4, S-BASE).
//
// The table is the same layout as the other level tables (a 0x38 header, rows, no tail). His reader also walks arrays
// nested in a record, which would sit in the tail after the rows; a table with a tail is refused rather than edited
// without being understood.
const (
	damageableTable = 0xbd1868f576836696 // CSymbol64("CR15NetDamageableCRWin10"), fix_drone_health.py:31
	healthOff       = 0x24               // fix_drone_health.py:39
	stockHealth     = 9999.0             // fix_drone_health.py:40
	droneHealth     = 70.0               // his config/mp_weapon_settings.json:30; balance_values.h:39
	healthEnd       = healthOff + 4      // a row must reach the end of the f32
)

func init() {
	steps = append(steps, Step{Name: "damageable health of actor 10e40287a9bf5a13 9999 to 70", Apply: forEachLevel(damageableTable, droneHealthRow),
		Writes: []Field{{Name: "damageable record of the drone actor, health f32 +0x24", Set: Sets[0], Type: damageableTable, Records: droneLevels,
			Read: readDroneHealth}}})
}

// readDroneHealth is the f32 at +0x24 of the one damageable record of the drone actor.
func readDroneHealth(b []byte) ([]byte, error) {
	t, err := radpack.ParseRowTable(b)
	if err != nil {
		return nil, err
	}
	if t.Size < healthEnd {
		return nil, fmt.Errorf("damageable rows of %d bytes end before the health at +%#x", t.Size, healthOff)
	}
	at := -1
	for i, r := range t.Rows {
		if radpack.U64(r, keyOff) == droneActor {
			if at >= 0 {
				return nil, fmt.Errorf("more than one damageable record for the drone actor (rows %d and %d), expected 1", at, i)
			}
			at = i
		}
	}
	if at < 0 {
		return nil, fmt.Errorf("no damageable record for the drone actor %016x, expected 1", uint64(droneActor))
	}
	return t.Rows[at][healthOff:healthEnd], nil
}

// droneHealthRow sets the drone's health in one level's damageable table. A drone already at the target is left as it
// is; any other value than the stock one is refused.
func droneHealthRow(t *radpack.RowTable) error {
	if t.Size < healthEnd {
		return fmt.Errorf("damageable rows of %d bytes end before the health at +%#x", t.Size, healthOff)
	}
	if len(t.Tail) != 0 {
		return fmt.Errorf("damageable table has %d bytes after its rows (arrays nested in a record): not the layout this step edits", len(t.Tail))
	}
	at := -1
	for i, r := range t.Rows {
		if radpack.U64(r, keyOff) != droneActor {
			continue
		}
		if at >= 0 {
			return fmt.Errorf("more than one damageable record for the drone actor %016x (rows %d and %d), expected 1", uint64(droneActor), at, i)
		}
		at = i
	}
	if at < 0 {
		return fmt.Errorf("no damageable record for the drone actor %016x, expected 1", uint64(droneActor))
	}
	r := t.Rows[at]
	old := math.Float32frombits(binary.LittleEndian.Uint32(r[healthOff:]))
	switch old {
	case droneHealth:
		return nil
	case stockHealth:
		binary.LittleEndian.PutUint32(r[healthOff:], math.Float32bits(droneHealth))
		return nil
	}
	return fmt.Errorf("the drone's health (+%#x of the record for actor %016x) is %v, not the stock %v: unexpected table", healthOff, uint64(droneActor), old, float32(stockHealth))
}
