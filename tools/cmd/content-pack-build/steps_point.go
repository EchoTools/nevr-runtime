package main

import (
	"encoding/binary"
	"encoding/json"
	"fmt"
	"math"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

// NEUTRON, the POINT tac mod (#2242, PLAN-v5 G-POINT): the data records of he_is_the_cat's tools/fix_point.py (at 82ad7ba).
// The game has the name `point` and its tuning (ability_properties.point) but no script reads them, and the equipment list
// does not offer it. Three records change, each from the player's own stock record (his tool starts from the combat gun
// patch, which is equal to stock in all three on these fields: S-BASE 2.2 lists no difference in tacmods or
// ability_properties; the rows the gun patch changes are not read here):
//
//  1. mp_equipment gear_table.tacmods gets {"name":"point","display_name":"NEUTRON"} (fix_point.py:44, :113-116), placed
//     by rank in tacModOrder (steps_tacmods.go, the runtime's kNewTacMods order): after any smoke and buff entry, so last.
//  2. mp_weapon_settings ability_properties.point gets required_charge, the key the engine reads for the recharge, equal to
//     charge_required (fix_point.py:123-126). The tool also copies radius, charge_required, charge_up_time and stun_duration
//     from his config (config/mp_weapon_settings.json:79-83); stock already holds those four values, so this step checks
//     them and writes none (and the run-time merge of src/core/combat_balance/balance_values.h owns the point keys it names,
//     ADR 0039: required_charge is not among them, so no field is written by both).
//  3. The explosion tables 4af215b19b297c34, one per combat level, get a copy of the stun mine's row for the barrier wall,
//     near and far radius = POINT's radius (fix_point.py:52-75, :144-150).
//
// Declared fields (Step.Writes, what the stage82 oracle compares):
//   - mp_equipment f8d40525c88885d8: the gear_table.tacmods entry named "point" (readTacmod).
//   - mp_weapon_settings 11a8523ed4724ca7: ability_properties.point.required_charge, by value (readPointRequiredCharge).
//   - 4af215b19b297c34 of each drone level: the rows whose actor (+8) is the barrier wall, the one appended 96-byte row
//     (readRowsWhere). The header's size and count follow from the rows and are not declared.
//
// Not here: the CScriptCR row (fix_point.py:83-106, G-POINT-ATTACH), the script copy and every script patch (H pieces).
const (
	explosionTable  = 0x4af215b19b297c34
	stunMineActor   = 0x7435764d2c648580 // fix_point.py:46
	barrierWall     = 0x3d8b8ca4d5292926 // fix_point.py:47, R15NetDynamicCover
	stunExplosion   = 0x25905b54e61d3f10 // veil_fx.py:537, the stun mine's explosion component (flags 0x890)
	explosionStride = 96                 // fix_point.py:49
	explosionMask   = 16                 // the row's mask u64, fix_point.py:67
	explosionRadius = 32                 // near radius f32; far radius at +36, fix_point.py:68
	explosionFar    = 36
	explosionLink   = 80 // the companion component, fix_point.py:69
	pointRadius     = 2.5
	pointCharge     = 45
	pointChargeUp   = 1.25
	pointStun       = 1.25
)

// The wall's records carry ffff1f.. in the mask (the grenades' ffff0f..), and +50h names the wall's own companion
// component: fix_point.py:51-54.
var (
	explosionWallMask = [8]byte{0xff, 0xff, 0x1f}
	explosionWallLink = [8]byte{0x98, 0x52, 0x7d, 0x4c, 0x8d, 0x8e, 0xd5, 0x2f}
)

const (
	pointTacmodStep    = "equipment tacmods offer point (NEUTRON)"
	pointSettingsStep  = "point settings get required_charge"
	pointExplosionStep = "barrier wall gets the stun mine's explosion row"
)

func init() {
	steps = append(steps,
		Step{Name: pointTacmodStep, Apply: pointTacmod, PastGunPatch: true,
			Writes: []Field{{Name: "gear_table.tacmods: the point entry", Set: Sets[0], Type: jsonResourceType,
				Records: []uint64{equipmentName}, Read: readTacmod(pointEntry.Name)}}},
		Step{Name: pointSettingsStep, Apply: pointSettings,
			Writes: []Field{{Name: "ability_properties.point.required_charge", Set: Sets[0], Type: jsonResourceType,
				Records: []uint64{weaponSettingsName}, Read: readPointRequiredCharge}}},
		Step{Name: pointExplosionStep, Apply: forEachLevel(explosionTable, pointExplosionRow),
			Writes: []Field{{Name: "explosion rows of the barrier wall actor", Set: Sets[0], Type: explosionTable,
				Records: droneLevels, Read: readRowsWhere(keyOff, barrierWall)}}},
	)
}

// pointEntry is the tac mod list entry (fix_point.py:44, :113-116).
var pointEntry = gearEntry{"point", "NEUTRON"}

func pointTacmod(p *Pack) error {
	var name uint64 = equipmentName
	b, err := p.Read(Sets[0], jsonResourceType, int64(name))
	if err != nil {
		return err
	}
	out, err := addPointTacmod(b)
	if err != nil {
		return fmt.Errorf("mp_equipment: %w", err)
	}
	return p.Put(Sets[0], jsonResourceType, int64(name), out)
}

// addPointTacmod is the mp_equipment resource with NEUTRON put into gear_table.tacmods at its rank (insertTacmod,
// steps_tacmods.go): point ranks last in tacModOrder, so it goes after any smoke and buff entry, at the list's end. A list
// that already has point is refused.
func addPointTacmod(b []byte) ([]byte, error) {
	return insertTacmod(b, pointEntry, nil)
}

func pointSettings(p *Pack) error {
	var name uint64 = weaponSettingsName // not a constant: the id overflows a constant int64 conversion
	b, err := p.Read(Sets[0], jsonResourceType, int64(name))
	if err != nil {
		return err
	}
	out, err := addRequiredCharge(b)
	if err != nil {
		return fmt.Errorf("mp_weapon_settings: %w", err)
	}
	return p.Put(Sets[0], jsonResourceType, int64(name), out)
}

// addRequiredCharge checks the stock point tuning, then adds required_charge at the end of ability_properties.point.
func addRequiredCharge(b []byte) ([]byte, error) {
	if len(b) < jsonHeader || binary.LittleEndian.Uint64(b[jsonLengthOff:]) != uint64(len(b)-jsonHeader) {
		return nil, fmt.Errorf("not a JSON resource of %d bytes (a 0x220-byte header whose +8 is the body's length)", len(b))
	}
	body := append([]byte(nil), b[jsonHeader:]...)
	end, fields, err := objectEnd(body, "ability_properties", "point")
	if err != nil {
		return nil, err
	}
	if _, ok := fields["required_charge"]; ok {
		return nil, fmt.Errorf("ability_properties.point already has required_charge")
	}
	for _, want := range []struct {
		key string
		val float64
	}{{"radius", pointRadius}, {"charge_required", pointCharge}, {"charge_up_time", pointChargeUp}, {"stun_duration", pointStun}} {
		var got float64
		if err := json.Unmarshal(fields[want.key], &got); err != nil || got != want.val {
			return nil, fmt.Errorf("ability_properties.point.%s is %s, expected %v", want.key, fields[want.key], want.val)
		}
	}
	add := fmt.Sprintf(`"required_charge":%d`, pointCharge)
	if len(fields) > 0 {
		add = "," + add
	}
	body = append(body[:end], append([]byte(add), body[end:]...)...)
	out := append([]byte(nil), b[:jsonHeader]...)
	binary.LittleEndian.PutUint64(out[jsonLengthOff:], uint64(len(body)))
	return append(out, body...), nil
}

// objectEnd is the offset of the `}` that closes the object at the path in body, and its members' values. The path walk is
// valueSpan's (steps_dot_preset.go).
func objectEnd(body []byte, path ...string) (int, map[string]json.RawMessage, error) {
	start, end, err := valueSpan(body, path...)
	if err != nil {
		return 0, nil, err
	}
	var fields map[string]json.RawMessage
	if err := json.Unmarshal(body[start:end], &fields); err != nil {
		return 0, nil, fmt.Errorf("%v is not an object: %w", path, err)
	}
	// The value ends with the object's `}`, so a member goes in just before it.
	return end - 1, fields, nil
}

// readPointRequiredCharge is a reader of ability_properties.point.required_charge of a settings resource, as the JSON text
// of its number, so two builds that lay the record out differently compare by value. A value that is not a number is
// refused.
func readPointRequiredCharge(b []byte) ([]byte, error) {
	if len(b) < jsonHeader || binary.LittleEndian.Uint64(b[jsonLengthOff:]) != uint64(len(b)-jsonHeader) {
		return nil, fmt.Errorf("not a JSON resource of %d bytes (a 0x220-byte header whose +8 is the body's length)", len(b))
	}
	body := b[jsonHeader:]
	start, end, err := valueSpan(body, "ability_properties", "point", "required_charge")
	if err != nil {
		return nil, err
	}
	var v float64
	if err := json.Unmarshal(body[start:end], &v); err != nil {
		return nil, fmt.Errorf("ability_properties.point.required_charge is not a number: %w", err)
	}
	return json.Marshal(v)
}

// pointExplosionRow appends the barrier wall's copy of the stun mine's explosion row (fix_point.py:52-75).
func pointExplosionRow(t *radpack.RowTable) error {
	if t.Size != explosionStride || len(t.Tail) != 0 {
		return fmt.Errorf("explosion rows of %d bytes with a %d-byte tail, expected %d bytes and no tail", t.Size, len(t.Tail), explosionStride)
	}
	if t.Find(keyOff, barrierWall) >= 0 {
		return fmt.Errorf("the table already has the barrier wall's row")
	}
	var stun []int
	for i, r := range t.Rows {
		if radpack.U64(r, keyOff) == stunMineActor {
			stun = append(stun, i)
		}
	}
	if len(stun) != 1 || radpack.U64(t.Rows[stun[0]], 0) != stunExplosion {
		return fmt.Errorf("%d rows for the stun mine actor %#x (expected 1, with explosion %#x)", len(stun), uint64(stunMineActor), uint64(stunExplosion))
	}
	c := append([]byte(nil), t.Rows[stun[0]]...)
	radpack.SetU64(c, keyOff, barrierWall)
	copy(c[explosionMask:], explosionWallMask[:])
	binary.LittleEndian.PutUint32(c[explosionRadius:], math.Float32bits(pointRadius))
	binary.LittleEndian.PutUint32(c[explosionFar:], math.Float32bits(pointRadius))
	copy(c[explosionLink:], explosionWallLink[:])
	t.Rows = append(t.Rows, c)
	return nil
}
