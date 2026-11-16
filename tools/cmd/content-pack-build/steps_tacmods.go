package main

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"slices"
)

// The tac-mod list's tail, shared by the three steps that add EchoCombat's tac mods to mp_equipment's gear_table.tacmods
// (steps_buff.go now; steps_point.go and steps_veil.go reuse it). The runtime hides or shows EchoCombat's tac mods only
// when they are the list's LAST three entries in this order: src/runtime/patch/ec_rules.h kNewTacMods, read by OursAtEnd
// (any other tail makes PlanTacMods return an empty plan). The steps run in file-name order (steps_buff, steps_point,
// steps_veil), not in this order, so each one inserts its entry by rank: in front of the first entry of a later rank,
// else at the list's end. TestTheTacModOrderIsTheRuntimes pins this list to the header.
var tacModOrder = []string{"smoke", "buff", "point"}

const kindTacmods = "tacmods"

// insertTacmod is the mp_equipment resource with e put into gear_table.tacmods at its rank (tacModOrder), the header kept
// and its length set. check, when not nil, is the step's own test of the stock list's names, run first. A name that is
// not one of tacModOrder, or one the list already has, is refused.
func insertTacmod(b []byte, e gearEntry, check func(names map[string]bool) error) ([]byte, error) {
	rank := slices.Index(tacModOrder, e.Name)
	if rank < 0 {
		return nil, fmt.Errorf("%s is not one of the tac mods the runtime orders (%v)", e.Name, tacModOrder)
	}
	if len(b) < jsonHeader || binary.LittleEndian.Uint64(b[jsonLengthOff:]) != uint64(len(b)-jsonHeader) {
		return nil, fmt.Errorf("not a JSON resource of %d bytes (a 0x220-byte header whose +8 is the body's length)", len(b))
	}
	body := append([]byte(nil), b[jsonHeader:]...)
	end, names, err := arrayEnd(body, "gear_table", kindTacmods)
	if err != nil {
		return nil, err
	}
	if check != nil {
		if err := check(names); err != nil {
			return nil, err
		}
	}
	if names[e.Name] {
		return nil, fmt.Errorf("gear_table.tacmods already lists %s: not the stock list", e.Name)
	}
	j, err := json.Marshal(e)
	if err != nil {
		return nil, err
	}
	if at, found, err := entryStart(body, kindTacmods, tacModOrder[rank+1:]); err != nil {
		return nil, err
	} else if found {
		// A later tac mod is already there: the entry goes in front of it, with a comma after it.
		body = append(body[:at], append(append(append([]byte(nil), j...), ','), body[at:]...)...)
	} else {
		sep := []byte{','}
		if len(names) == 0 {
			sep = nil
		}
		body = append(body[:end], append(append(sep, j...), body[end:]...)...)
	}
	out := append([]byte(nil), b[:jsonHeader]...)
	binary.LittleEndian.PutUint64(out[jsonLengthOff:], uint64(len(body)))
	return append(out, body...), nil
}

// entryStart is the offset of the `{` that opens the first entry of the gear_table list `list` whose name is one of
// names, and whether the list has one. arrayEnd has checked the list is there.
func entryStart(body []byte, list string, names []string) (int, bool, error) {
	dec := json.NewDecoder(bytes.NewReader(body))
	for {
		t, err := dec.Token()
		if err != nil {
			return 0, false, fmt.Errorf("gear_table.%s: %w", list, err)
		}
		if t != list {
			continue
		}
		if d, err := dec.Token(); err != nil || d != json.Delim('[') {
			continue // the same word as a value elsewhere, not the list's key
		}
		for dec.More() {
			from := int(dec.InputOffset())
			var e struct {
				Name string `json:"name"`
			}
			if err := dec.Decode(&e); err != nil {
				return 0, false, fmt.Errorf("gear_table.%s: %w", list, err)
			}
			if slices.Contains(names, e.Name) {
				return from + bytes.IndexByte(body[from:], '{'), true, nil
			}
		}
		return 0, false, nil
	}
}

// readTacmod is a reader of the gear_table.tacmods entry named name (name and display_name, as one JSON line): the field
// a tac-mod step declares (Step.Writes).
func readTacmod(name string) func([]byte) ([]byte, error) {
	return readGearNamed(kindTacmods, map[string]bool{name: true})
}
