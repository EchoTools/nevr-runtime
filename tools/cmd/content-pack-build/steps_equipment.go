package main

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"strings"
)

// The equipment lists (#600, EchoCombat slice 6). mp_equipment (a CJsonResourceWin10: a 0x220-byte header whose u64 at
// +8 is the body's length, then the minified JSON body; src/runtime/combat/balance_merge.h reads the same layout) lists
// the gear the loadout offers in gear_table.weapons and gear_table.ordnances. The game already defines four more weapons
// and two more ordnances in mp_weapon_settings (weapon_properties magnum, smg, chain, rifle; grenade_properties dot,
// drone) that the stock lists leave out. The combat gun patch the EchoCombat pack starts from appends them, and the
// translocator (loc), after the stock entries, each as {"name", "display_name"}; nothing else in the file changes
// (stock_steps_test.go pins its sha-256). The display names are the gun patch's: the game's data has none of them.
const (
	jsonResourceType = 0x607d858c90268f9a // CJsonResourceWin10
	equipmentName    = 0xf8d40525c88885d8 // mp_equipment
	jsonHeader       = 0x220
	jsonLengthOff    = 0x08
)

type gearEntry struct {
	Name        string `json:"name"`
	DisplayName string `json:"display_name"`
}

// equipmentAdds are appended to each list in this order.
var equipmentAdds = []struct {
	list    string
	entries []gearEntry
}{
	{"weapons", []gearEntry{{"magnum", "PROTOSTAR"}, {"smg", "QUARK"}, {"chain", "BLAZAR"}, {"rifle", "STARBURST"}}},
	{"ordnances", []gearEntry{{"loc", "TRANSLOCATOR"}, {"dot", "DOT FIELD"}, {"drone", "DRONE"}}},
}

func init() {
	steps = append(steps, Step{Name: "equipment lists offer magnum, smg, chain, rifle, loc, dot and drone", Apply: equipmentLists})
}

func equipmentLists(p *Pack) error {
	var name uint64 = equipmentName // not a constant: the id overflows a constant int64 conversion
	b, err := p.Read(Sets[0], jsonResourceType, int64(name))
	if err != nil {
		return err
	}
	out, err := appendGear(b)
	if err != nil {
		return fmt.Errorf("mp_equipment: %w", err)
	}
	return p.Put(Sets[0], jsonResourceType, int64(name), out)
}

// appendGear is the resource with equipmentAdds appended to its lists, the header kept and its length set.
func appendGear(b []byte) ([]byte, error) {
	if len(b) < jsonHeader || binary.LittleEndian.Uint64(b[jsonLengthOff:]) != uint64(len(b)-jsonHeader) {
		return nil, fmt.Errorf("not a JSON resource of %d bytes (a 0x220-byte header whose +8 is the body's length)", len(b))
	}
	body := append([]byte(nil), b[jsonHeader:]...)
	for _, add := range equipmentAdds {
		end, names, err := arrayEnd(body, "gear_table", add.list)
		if err != nil {
			return nil, err
		}
		var text bytes.Buffer
		for i, e := range add.entries {
			if names[e.Name] {
				return nil, fmt.Errorf("gear_table.%s already lists %s", add.list, e.Name)
			}
			if len(names) > 0 || i > 0 {
				text.WriteByte(',')
			}
			j, err := json.Marshal(e)
			if err != nil {
				return nil, err
			}
			text.Write(j)
		}
		body = append(body[:end], append(text.Bytes(), body[end:]...)...)
	}
	out := append([]byte(nil), b[:jsonHeader]...)
	binary.LittleEndian.PutUint64(out[jsonLengthOff:], uint64(len(body)))
	return append(out, body...), nil
}

// arrayEnd is the offset of the `]` that closes the array at the object path in body, and the names its entries carry.
func arrayEnd(body []byte, path ...string) (int, map[string]bool, error) {
	dec := json.NewDecoder(bytes.NewReader(body))
	for i, key := range path {
		where := strings.Join(path[:i], ".")
		if t, err := dec.Token(); err != nil || t != json.Delim('{') {
			return 0, nil, fmt.Errorf("%q is not an object", where)
		}
		for {
			if !dec.More() {
				return 0, nil, fmt.Errorf("%q has no key %q", where, key)
			}
			t, err := dec.Token()
			if err != nil {
				return 0, nil, err
			}
			if t == key {
				break
			}
			var skip json.RawMessage
			if err := dec.Decode(&skip); err != nil {
				return 0, nil, err
			}
		}
	}
	where := strings.Join(path, ".")
	if t, err := dec.Token(); err != nil || t != json.Delim('[') {
		return 0, nil, fmt.Errorf("%s is not a list", where)
	}
	names := map[string]bool{}
	for dec.More() {
		var e struct {
			Name string `json:"name"`
		}
		if err := dec.Decode(&e); err != nil {
			return 0, nil, fmt.Errorf("%s: %w", where, err)
		}
		names[e.Name] = true
	}
	if _, err := dec.Token(); err != nil {
		return 0, nil, err
	}
	// The decoder stops right after the list's `]`, so the entries go in just before it.
	return int(dec.InputOffset()) - 1, names, nil
}
