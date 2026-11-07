package main

import (
	"encoding/binary"
	"strings"
	"testing"
)

// The equipment rule over a synthetic mp_equipment of the shipped layout; no text of the game's is used.
func jsonResource(body string) []byte {
	b := make([]byte, jsonHeader, jsonHeader+len(body))
	for i := range b {
		b[i] = byte(i) // header bytes the rule must keep
	}
	binary.LittleEndian.PutUint64(b[jsonLengthOff:], uint64(len(body)))
	return append(b, body...)
}

func TestTheEquipmentListsGetTheGamesUnlistedGearAfterTheStockEntries(t *testing.T) {
	in := jsonResource(`{"gear_table":{"weapons":[{"name":"w1","display_name":"W1","aliases":["x"]}],"ordnances":[{"name":"o1","display_name":"O1"}],"tacmods":[{"name":"t1"}]},"after":[1]}`)
	out, err := appendGear(in)
	if err != nil {
		t.Fatal(err)
	}
	want := `{"gear_table":{"weapons":[{"name":"w1","display_name":"W1","aliases":["x"]},` +
		`{"name":"magnum","display_name":"PROTOSTAR"},{"name":"smg","display_name":"QUARK"},{"name":"chain","display_name":"BLAZAR"},{"name":"rifle","display_name":"STARBURST"}],` +
		`"ordnances":[{"name":"o1","display_name":"O1"},{"name":"loc","display_name":"TRANSLOCATOR"},{"name":"dot","display_name":"DOT FIELD"},{"name":"drone","display_name":"DRONE"}],` +
		`"tacmods":[{"name":"t1"}]},"after":[1]}`
	if got := string(out[jsonHeader:]); got != want {
		t.Errorf("body\n got %s\nwant %s", got, want)
	}
	if binary.LittleEndian.Uint64(out[jsonLengthOff:]) != uint64(len(want)) {
		t.Error("the header's length is not the new body's")
	}
	for i := 0; i < jsonHeader; i++ {
		if i >= jsonLengthOff && i < jsonLengthOff+8 {
			continue
		}
		if out[i] != in[i] {
			t.Fatalf("header byte %#x changed", i)
		}
	}
	// Once is enough: a list that already offers one of the entries is refused.
	if _, err := appendGear(out); err == nil || !strings.Contains(err.Error(), "already lists") {
		t.Errorf("a second run was not refused: %v", err)
	}
}

func TestAnEmptyListGetsNoLeadingComma(t *testing.T) {
	out, err := appendGear(jsonResource(`{"gear_table":{"ordnances":[],"weapons":[]}}`))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.HasPrefix(string(out[jsonHeader:]), `{"gear_table":{"ordnances":[{"name":"loc"`) {
		t.Errorf("body %s", out[jsonHeader:])
	}
}

func TestAnEquipmentResourceTheRuleCannotReadIsRefused(t *testing.T) {
	good := `{"gear_table":{"weapons":[{"name":"w1"}],"ordnances":[{"name":"o1"}]}}`
	if _, err := appendGear(jsonResource(good)); err != nil {
		t.Fatalf("the base case is refused: %v", err)
	}
	// Each case is the base case with one fault, so each refusal is that fault's.
	wrongLength := jsonResource(good)
	binary.LittleEndian.PutUint64(wrongLength[jsonLengthOff:], uint64(len(good)-1))
	short := make([]byte, jsonHeader-8)
	binary.LittleEndian.PutUint64(short[jsonLengthOff:], ^uint64(7)) // what len-0x220 wraps to
	cases := []struct {
		name string
		b    []byte
		want string
	}{
		{"a length that is not the body's", wrongLength, "not a JSON resource"},
		{"shorter than the header", short, "not a JSON resource"},
		{"a gear table that is a list", jsonResource(`{"gear_table":[{"weapons":[]}]}`), `"gear_table" is not an object`},
		{"no ordnances list", jsonResource(`{"gear_table":{"weapons":[{"name":"w1"}]}}`), `has no key "ordnances"`},
		{"a list that is an object", jsonResource(`{"gear_table":{"weapons":{"name":"w1"},"ordnances":[]}}`), "gear_table.weapons is not a list"},
		{"an entry that is not an object", jsonResource(`{"gear_table":{"weapons":["w1"],"ordnances":[]}}`), "gear_table.weapons: json"},
	}
	for _, c := range cases {
		_, err := appendGear(c.b)
		if err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: %v, want an error naming %q", c.name, err, c.want)
		}
	}
}
