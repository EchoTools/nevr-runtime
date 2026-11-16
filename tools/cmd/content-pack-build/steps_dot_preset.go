package main

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"sort"
	"strings"
)

// The DOT FIELD activation presets (#2242, PLAN-v5 G-DOT-PRESET; he_is_the_cat's tools/fix_dot.py at EchoCombat 82ad7ba).
//
// mp_weapon_settings.grenade_properties.dot carries three booleans that decide how the field arms: activate_on_collision,
// use_remote_trigger and stick_to_geo. As shipped all three are true, so the field arms itself on the first impact, which a
// magazine held in the hand makes at once (fix_dot.py:31-36). His tool picks one of five combinations (PRESETS,
// fix_dot.py:99-124) and writes the chosen one's three values into the resource (fix_dot.py:177-181); its default is
// `trigger` (DEFAULT_PRESET, fix_dot.py:125: clear activate_on_collision, keep the remote trigger and the weld), and that is
// the one this step writes, because a pack carries one.
//
// What main already covers of fix_dot.py, and so what this step leaves out: the ordnance list entry (fix_dot.py:133-139,
// :171-172) is steps_equipment.go's equipmentAdds `dot`; cooldown and radius (fix_dot.py:174-176, :182-184) are
// core/combat_balance/balance_values.h's grenade_properties.dot (radius 3.5, cooldown 30), merged into the resource as the
// game loads it. damage_table.dot (fix_dot.py:185-187) and the drone rows (:189-194) are not this step's.
//
// The base is the player's stock mp_weapon_settings: his tool rebases on whatever file it is given and sets a key only when
// it differs, with no stock check; this step checks first that all three values are the shipped true (his "stock" preset,
// fix_dot.py:121-123, "Exactly as shipped") and refuses otherwise, naming the field. It does not depend on his gun-patch
// base (PLAN-v5 S-BASE), and imports no value of it.
//
// Declared fields (Step.Writes, what the stage82 oracle compares for this step; readDotPresetValue): dotPresetFields below,
// each under grenade_properties.dot of mp_weapon_settings (11a8523ed4724ca7, CJsonResourceWin10), one field per key, read
// as the JSON boolean `true` or `false`. The resource is edited in place, byte for byte outside those three values; the
// header's length is the only other change.
const (
	weaponSettingsName = 0x11a8523ed4724ca7 // CSymbol64("mp_weapon_settings"), core/combat_balance/balance_merge.h
	dotPresetStepName  = "DOT FIELD arms by remote trigger, not on impact"
	dotDefaultPreset   = "trigger" // fix_dot.py:125
)

// dotPresetFields are the keys of grenade_properties.dot a preset sets, in the order of fix_dot.py:102-103 (the field's
// activation, then the weld).
var dotPresetFields = [3]string{"activate_on_collision", "use_remote_trigger", "stick_to_geo"}

// dotPresets are fix_dot.py:99-124's five presets, the values in dotPresetFields order.
var dotPresets = map[string][3]bool{
	"trigger":         {false, true, true},  // fix_dot.py:102-103
	"trigger-nostick": {false, true, false}, // fix_dot.py:110-111
	"impact":          {true, false, true},  // fix_dot.py:115-116
	"impact-nostick":  {true, false, false}, // fix_dot.py:119-120
	"stock":           {true, true, true},   // fix_dot.py:122-123
}

func init() {
	var writes []Field
	for _, field := range dotPresetFields {
		writes = append(writes, Field{Name: "grenade_properties.dot." + field, Set: Sets[0], Type: jsonResourceType,
			Records: []uint64{weaponSettingsName}, Read: readDotPresetValue(field)})
	}
	steps = append(steps, Step{Name: dotPresetStepName, Apply: dotPresetStep, Writes: writes})
}

// readDotPresetValue is a reader of grenade_properties.dot.<field> of a settings resource, as the JSON text of its boolean
// (`true` or `false`), so two builds that lay the record out differently compare by value. A value that is not a boolean
// is refused.
func readDotPresetValue(field string) func([]byte) ([]byte, error) {
	return func(b []byte) ([]byte, error) {
		if len(b) < jsonHeader || binary.LittleEndian.Uint64(b[jsonLengthOff:]) != uint64(len(b)-jsonHeader) {
			return nil, fmt.Errorf("not a JSON resource of %d bytes (a 0x220-byte header whose +8 is the body's length)", len(b))
		}
		body := b[jsonHeader:]
		start, end, err := valueSpan(body, "grenade_properties", "dot", field)
		if err != nil {
			return nil, err
		}
		var v bool
		if err := json.Unmarshal(body[start:end], &v); err != nil {
			return nil, fmt.Errorf("grenade_properties.dot.%s is not a boolean: %w", field, err)
		}
		return json.Marshal(v)
	}
}

func dotPresetStep(p *Pack) error {
	var name uint64 = weaponSettingsName // not a constant: the id overflows a constant int64 conversion
	b, err := p.Read(Sets[0], jsonResourceType, int64(name))
	if err != nil {
		return err
	}
	out, err := applyDotPreset(b, dotDefaultPreset)
	if err != nil {
		return fmt.Errorf("mp_weapon_settings: %w", err)
	}
	return p.Put(Sets[0], jsonResourceType, int64(name), out)
}

// applyDotPreset is the settings resource with the preset's three values written into grenade_properties.dot. Each of the
// three must be the shipped `true` first.
func applyDotPreset(b []byte, preset string) ([]byte, error) {
	want, ok := dotPresets[preset]
	if !ok {
		return nil, fmt.Errorf("unknown preset %q", preset)
	}
	if len(b) < jsonHeader || binary.LittleEndian.Uint64(b[jsonLengthOff:]) != uint64(len(b)-jsonHeader) {
		return nil, fmt.Errorf("not a JSON resource of %d bytes (a 0x220-byte header whose +8 is the body's length)", len(b))
	}
	body := b[jsonHeader:]
	type edit struct{ start, end int }
	var edits []edit
	for i, field := range dotPresetFields {
		start, end, err := valueSpan(body, "grenade_properties", "dot", field)
		if err != nil {
			return nil, err
		}
		if got := string(body[start:end]); got != "true" {
			return nil, fmt.Errorf("grenade_properties.dot.%s: expected true (as shipped), actual %s", field, got)
		}
		if !want[i] {
			edits = append(edits, edit{start, end})
		}
	}
	sort.Slice(edits, func(i, j int) bool { return edits[i].start > edits[j].start })
	out := append([]byte(nil), body...)
	for _, e := range edits {
		out = append(out[:e.start], append([]byte("false"), out[e.end:]...)...)
	}
	res := append([]byte(nil), b[:jsonHeader]...)
	binary.LittleEndian.PutUint64(res[jsonLengthOff:], uint64(len(out)))
	return append(res, out...), nil
}

// valueSpan is the offsets of the value at the object path in body.
func valueSpan(body []byte, path ...string) (int, int, error) {
	dec := json.NewDecoder(bytes.NewReader(body))
	for i, key := range path {
		where := strings.Join(path[:i], ".")
		if where == "" {
			where = "the resource"
		}
		if t, err := dec.Token(); err != nil || t != json.Delim('{') {
			return 0, 0, fmt.Errorf("%s is not an object", where)
		}
		for {
			if !dec.More() {
				return 0, 0, fmt.Errorf("%s has no key %q", where, key)
			}
			t, err := dec.Token()
			if err != nil {
				return 0, 0, err
			}
			if t == key {
				break
			}
			var skip json.RawMessage
			if err := dec.Decode(&skip); err != nil {
				return 0, 0, err
			}
		}
	}
	// The last key is matched and not entered: the value after it is the one wanted.
	var raw json.RawMessage
	if err := dec.Decode(&raw); err != nil {
		return 0, 0, err
	}
	end := int(dec.InputOffset())
	return end - len(raw), end, nil
}
