package main

// The MEDIGUN data rules (steps_medigun.go) over synthetic records of the shipped layout; no record of the game's is used.
// stock_steps_test.go runs the steps over the shipped data when a machine has it.

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"slices"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

// wantMedigun is ability_properties.medigun as fix_medigun.py's `tuning` yields it for heal_amt 114 and heal_duration 12 and
// the config section at config/mp_weapon_settings.json:91-110 (82ad7ba): heal_per_second = 114 / 12 * 0.85 = 8.075,
// heal_amt = 8.075 * 15 = 121.125, heal_radius = 20 + (20 + 500) / 100000 = 20.0052. Written out here from his values, not
// from the step's constants.
const wantMedigun = `{"enabled":false,"heal_scale":0.85,"beam_seconds":15,"overheat_seconds":4,"cool_seconds":5,` +
	`"cone_angle":20,"range":20,"cone_slack":5,"hand":"auto","hand_offset":"0.064,0.000,0.203","hand_offset_left":"0.124,0,0.203",` +
	`"beam_bone":"ToolAttach1","aim":"gun","heal_per_second":8.075,"heal_amt":121.125,"heal_duration":15,"heal_radius":20.0052,` +
	`"cap_healing":false,"max_healing":0,"scale_healing":false,"min_heal_mult":1,"time_to_max_heal":0,"self_heal_mult":0,` +
	`"unique_healing_only":true}`

const stockSettings = `{"ability_properties":{"buff":{"dr":0.09},"heal":{"heal_amt":114.0,"heal_duration":12.0,"other":3}},"after":[1]}`

func TestTheWeaponSettingsGetTheMedigunTuning(t *testing.T) {
	in := jsonResource(stockSettings)
	out, err := addMedigunTuning(in)
	if err != nil {
		t.Fatal(err)
	}
	want := `{"ability_properties":{"buff":{"dr":0.09},"heal":{"heal_amt":114.0,"heal_duration":12.0,"other":3},"medigun":` +
		wantMedigun + `},"after":[1]}`
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
	// Once is enough: a resource that has the row is refused.
	if _, err := addMedigunTuning(out); err == nil || !strings.Contains(err.Error(), "already has a medigun row") {
		t.Errorf("a second run was not refused: %v", err)
	}
}

func TestAWeaponSettingsResourceTheRuleCannotTrustIsRefused(t *testing.T) {
	wrongLength := jsonResource(stockSettings)
	binary.LittleEndian.PutUint64(wrongLength[jsonLengthOff:], uint64(len(stockSettings)-1))
	cases := []struct {
		name string
		b    []byte
		want string
	}{
		{"a length that is not the body's", wrongLength, "not a JSON resource"},
		{"no ability_properties", jsonResource(`{"x":1}`), "no ability_properties"},
		{"no heal row", jsonResource(`{"ability_properties":{"buff":{}}}`), "has no heal_amt and heal_duration"},
		{"a heal row without its duration", jsonResource(`{"ability_properties":{"heal":{"heal_amt":114.0}}}`), "has no heal_amt and heal_duration"},
		{"another heal_amt", jsonResource(`{"ability_properties":{"heal":{"heal_amt":115.0,"heal_duration":12.0}}}`), "heal_amt 115 heal_duration 12, the stock row's are 114 and 12"},
		{"another heal_duration", jsonResource(`{"ability_properties":{"heal":{"heal_amt":114.0,"heal_duration":10.0}}}`), "heal_amt 114 heal_duration 10"},
		{"a medigun row already there", jsonResource(`{"ability_properties":{"heal":{"heal_amt":114.0,"heal_duration":12.0},"medigun":{}}}`), "already has a medigun row"},
		{"ability_properties that is a list", jsonResource(`{"ability_properties":[1]}`), "json"},
	}
	for _, c := range cases {
		_, err := addMedigunTuning(c.b)
		if err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: %v, want an error naming %q", c.name, err, c.want)
		}
	}
}

func TestTheTuningRefusesARangeOrAnAngleOutsideHisLimits(t *testing.T) {
	// His `tuning` raises for range outside 1..100 or cone_angle outside 1..90 (fix_medigun.py:179-180); the config's
	// values (20, 20) are inside.
	if _, err := tuning(stockHealAmt, stockHealDuration); err != nil {
		t.Fatal(err)
	}
	for _, c := range []struct {
		length, angle int
		ok            bool
	}{
		{1, 1, true}, {100, 90, true}, {20, 20, true},
		{0, 20, false}, {101, 20, false}, {20, 0, false}, {20, 91, false},
	} {
		err := checkCone(c.length, c.angle)
		if c.ok && err != nil {
			t.Errorf("range %d cone_angle %d refused: %v", c.length, c.angle, err)
		}
		if !c.ok && (err == nil || !strings.Contains(err.Error(), "out of range")) {
			t.Errorf("range %d cone_angle %d: %v, want out of range", c.length, c.angle, err)
		}
	}
}

func bubbleTable(tail []byte, rows ...[]byte) []byte { return table(bubbleRowSize, tail, rows...) }

const (
	otherActor  = 0x5555
	otherBubble = 0x7777
)

func bubble(handle, actor uint64, mark byte) []byte {
	r := row(bubbleRowSize, map[int]uint64{0: handle, keyOff: actor})
	r[0x30] = mark // payload the copy must keep
	return r
}

func TestEachPlayerActorsHealBubbleIsCopiedUnderTheMedigunBubble(t *testing.T) {
	// The stock table's order (91d8de0806964a11, b113997837c29003, f1e70acd0dde0312), which is not mediActors' order, with
	// rows between them that are not copied: another actor's heal bubble, and a player actor's row under another handle.
	b := bubbleTable([]byte("tail"),
		bubble(healBubble, 0x91d8de0806964a11, 1), bubble(healBubble, 0xb113997837c29003, 2), bubble(otherBubble, otherActor, 4),
		bubble(healBubble, otherActor, 5), bubble(otherBubble, 0xf1e70acd0dde0312, 6), bubble(healBubble, 0xf1e70acd0dde0312, 3))
	tb := parse(t, b)
	if err := copyBubbles(tb); err != nil {
		t.Fatal(err)
	}
	out := parse(t, tb.Bytes())
	if len(out.Rows) != 9 {
		t.Fatalf("%d rows, want the 6 stock rows and 3 copies", len(out.Rows))
	}
	for i, r := range parse(t, b).Rows { // the stock rows are untouched
		if !bytes.Equal(out.Rows[i], r) {
			t.Errorf("stock row %d changed", i)
		}
	}
	// The copies follow in the stock rows' order, as his loop appends them (fix_medigun.py:230-235): each the stock row with
	// only the handle changed.
	for i, si := range []int{0, 1, 5} {
		want := append([]byte(nil), out.Rows[si]...)
		radpack.SetU64(want, 0, mediBubble)
		if c := out.Rows[6+i]; !bytes.Equal(c, want) {
			t.Errorf("copy %d is %x, want stock row %d under the MEDIGUN bubble %x", i, c, si, want)
		}
	}
	if !bytes.HasSuffix(tb.Bytes(), []byte("tail")) {
		t.Error("the table's tail changed")
	}
}

func TestABubbleTableTheRuleCannotTrustIsRefused(t *testing.T) {
	full := func(extra ...[]byte) []byte {
		rows := [][]byte{bubble(healBubble, mediActors[0], 1), bubble(healBubble, mediActors[1], 2), bubble(healBubble, mediActors[2], 3)}
		return bubbleTable(nil, append(rows, extra...)...)
	}
	if err := copyBubbles(parse(t, full())); err != nil {
		t.Fatalf("the base case is refused: %v", err)
	}
	cases := []struct {
		name string
		tb   *radpack.RowTable
		want string
	}{
		{"an actor with no heal bubble", parse(t, bubbleTable(nil, bubble(healBubble, mediActors[0], 1), bubble(healBubble, mediActors[1], 2))), "no heal-bubble row"},
		{"the actor's row under another handle", parse(t, bubbleTable(nil, bubble(healBubble, mediActors[0], 1), bubble(healBubble, mediActors[1], 2), bubble(otherBubble, mediActors[2], 3))), "no heal-bubble row"},
		{"two heal bubbles for one actor", parse(t, full(bubble(healBubble, mediActors[1], 9))), "two heal-bubble rows"},
		{"a medigun bubble already there", parse(t, full(bubble(mediBubble, mediActors[1], 9))), "already has a MEDIGUN bubble row"},
		{"rows too short for an actor", parse(t, table(8, nil, row(8, nil))), "end before the actor"},
	}
	for _, c := range cases {
		before := len(c.tb.Rows)
		err := copyBubbles(c.tb)
		if err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: %v, want an error naming %q", c.name, err, c.want)
		}
		if len(c.tb.Rows) != before {
			t.Errorf("%s: a refused table was changed", c.name)
		}
	}
}

// fix_medigun.py:284-286: MEDIGUN_SHOWN is False, so gear_table.weapons never lists it. Nothing in this build adds it.
func TestNoStepListsMedigunOnATerminal(t *testing.T) {
	for _, add := range equipmentAdds {
		for _, e := range add.entries {
			if strings.EqualFold(e.Name, "medigun") {
				t.Errorf("equipmentAdds lists %q in gear_table.%s", e.Name, add.list)
			}
		}
	}
	out, err := appendGear(jsonResource(`{"gear_table":{"weapons":[{"name":"w1"}],"ordnances":[]}}`))
	if err != nil || bytes.Contains(bytes.ToLower(out), []byte("medigun")) {
		t.Errorf("the equipment step's output names medigun: %v", err)
	}
}

// The declared fields read what the two steps write: ability_properties.medigun by value, and the three MEDIGUN rows; the
// OVERCHARGE field of the same bubble table (steps_buff.go) reads none of them, and this one none of OVERCHARGE's.
func TestTheMedigunFieldsReadWhatTheirStepsWrite(t *testing.T) {
	fields := map[string]Field{}
	for _, st := range steps {
		if st.Name == medigunSettingsStep || st.Name == medigunBubblesStep || st.Name == buffBubbleStep {
			if len(st.Writes) != 1 {
				t.Fatalf("%s declares %d fields, want 1", st.Name, len(st.Writes))
			}
			fields[st.Name] = st.Writes[0]
		}
	}
	out, err := addMedigunTuning(jsonResource(stockSettings))
	if err != nil {
		t.Fatal(err)
	}
	f := fields[medigunSettingsStep]
	got, err := f.Read(out)
	var v map[string]any
	if err := json.Unmarshal([]byte(wantMedigun), &v); err != nil {
		t.Fatal(err)
	}
	want, _ := json.Marshal(v)
	if err != nil || !bytes.Equal(got, want) || f.Type != jsonResourceType || !slices.Equal(f.Records, []uint64{weaponSettingsName}) {
		t.Errorf("settings field %s, want %s (%v; type %016x, records %x)", got, want, err, f.Type, f.Records)
	}
	for _, bad := range []string{stockSettings, `{"ability_properties":{"medigun":1}}`, `{"ability_properties":{"medigun":null}}`} {
		if b, err := f.Read(jsonResource(bad)); err == nil {
			t.Errorf("%s read as %s, want refused", bad, b)
		}
	}
	short := bytes.Clone(out)
	binary.LittleEndian.PutUint64(short[jsonLengthOff:], uint64(len(out)-jsonHeader-1))
	if b, err := f.Read(short); err == nil || !strings.Contains(err.Error(), "not a JSON resource") {
		t.Errorf("a resource whose length is not its body's read as %s, %v", b, err)
	}

	tb := parse(t, bubbleTable(nil, bubble(healBubble, mediActors[0], 1), bubble(healBubble, mediActors[2], 3),
		bubble(healBubble, mediActors[1], 2)))
	if err := addOverchargeBubbles(tb); err != nil {
		t.Fatal(err)
	}
	if err := copyBubbles(tb); err != nil {
		t.Fatal(err)
	}
	f = fields[medigunBubblesStep]
	rows, err := f.Read(tb.Bytes())
	var medi []byte
	for _, r := range tb.Rows[6:] {
		medi = append(medi, r...)
	}
	if err != nil || !bytes.Equal(rows, medi) || f.Type != healBubbleTable || !slices.Equal(f.Records, []uint64{globalLevel}) {
		t.Errorf("bubble field %x, want %x (%v; type %016x, records %x)", rows, medi, err, f.Type, f.Records)
	}
	oc, err := fields[buffBubbleStep].Read(tb.Bytes())
	if err != nil || len(oc) != 3*bubbleRowSize || bytes.Contains(oc, medi[:bubbleRowSize]) || bytes.Contains(rows, oc[:bubbleRowSize]) {
		t.Errorf("the OVERCHARGE and MEDIGUN fields share a row: %x / %x (%v)", oc, rows, err)
	}
}
