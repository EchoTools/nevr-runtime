package main

import (
	"encoding/binary"
	"slices"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

// The UMBRAL VEIL rule over a synthetic mp_equipment (jsonResource, steps_equipment_test.go); no text of the game's is used.
const syntheticTacmods = `{"gear_table":{"weapons":[{"name":"w1"}],"tacmods":[{"name":"t1","display_name":"T1"},{"name":"t2"}]},"after":[1]}`

func TestTheVeilIsAppendedToTheTacmodsListAndNothingElseChanges(t *testing.T) {
	in := jsonResource(syntheticTacmods)
	out, err := placeVeil(in)
	if err != nil {
		t.Fatal(err)
	}
	want := `{"gear_table":{"weapons":[{"name":"w1"}],"tacmods":[{"name":"t1","display_name":"T1"},{"name":"t2"},` +
		`{"name":"smoke","display_name":"UMBRAL VEIL"}]},"after":[1]}`
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
}

func TestAnEmptyTacmodsListGetsNoLeadingComma(t *testing.T) {
	out, err := placeVeil(jsonResource(`{"gear_table":{"tacmods":[]}}`))
	if err != nil {
		t.Fatal(err)
	}
	if got, want := string(out[jsonHeader:]), `{"gear_table":{"tacmods":[{"name":"smoke","display_name":"UMBRAL VEIL"}]}}`; got != want {
		t.Errorf("body %s, want %s", got, want)
	}
}

// The tac-mod hide (src/runtime/patch/ec_rules.h kNewTacMods, OursAtEnd) acts only when the list's last three are smoke,
// buff, point in that order, so the veil goes before buff or point when a step has already listed either.
func TestTheVeilGoesBeforeBuffAndPointWhenTheyAreListed(t *testing.T) {
	cases := []struct{ name, in, want string }{
		{"buff and point listed",
			`{"gear_table":{"tacmods":[{"name":"t1"},{"name":"buff","display_name":"B"},{"name":"point","display_name":"P"}]}}`,
			`{"gear_table":{"tacmods":[{"name":"t1"},{"name":"smoke","display_name":"UMBRAL VEIL"},{"name":"buff","display_name":"B"},{"name":"point","display_name":"P"}]}}`},
		{"only point listed",
			`{"gear_table":{"tacmods":[{"name":"t1"}, {"name":"point"}]}}`,
			`{"gear_table":{"tacmods":[{"name":"t1"}, {"name":"smoke","display_name":"UMBRAL VEIL"},{"name":"point"}]}}`},
		{"only buff listed, first",
			`{"gear_table":{"tacmods":[{"name":"buff"},{"name":"t1"}]}}`,
			`{"gear_table":{"tacmods":[{"name":"smoke","display_name":"UMBRAL VEIL"},{"name":"buff"},{"name":"t1"}]}}`},
	}
	for _, c := range cases {
		out, err := placeVeil(jsonResource(c.in))
		if err != nil {
			t.Errorf("%s: %v", c.name, err)
			continue
		}
		if got := string(out[jsonHeader:]); got != c.want {
			t.Errorf("%s: body\n got %s\nwant %s", c.name, got, c.want)
		}
		if binary.LittleEndian.Uint64(out[jsonLengthOff:]) != uint64(len(c.want)) {
			t.Errorf("%s: the header's length is not the new body's", c.name)
		}
	}
}

// A stock record that is not what the rule expects is refused, naming the field, never edited (the step adds the record's
// name: TestTheVeilStepNamesTheRecordItRefuses).
func TestAnEquipmentRecordTheVeilRuleDoesNotExpectIsRefused(t *testing.T) {
	good := jsonResource(syntheticTacmods)
	if _, err := placeVeil(good); err != nil {
		t.Fatalf("the base case is refused: %v", err)
	}
	wrongLength := jsonResource(syntheticTacmods)
	binary.LittleEndian.PutUint64(wrongLength[jsonLengthOff:], uint64(len(syntheticTacmods)-1))
	cases := []struct {
		name string
		b    []byte
		want []string
	}{
		{"a length that is not the body's", wrongLength, []string{"not a JSON resource"}},
		{"no tacmods list", jsonResource(`{"gear_table":{"weapons":[]}}`), []string{`has no key "tacmods"`}},
		{"tacmods that is an object", jsonResource(`{"gear_table":{"tacmods":{"name":"t1"}}}`), []string{"gear_table.tacmods is not a list"}},
		{"the veil already listed", jsonResource(`{"gear_table":{"tacmods":[{"name":"smoke","display_name":"UMBRAL VEIL"}]}}`),
			[]string{"gear_table.tacmods", "already lists smoke", "not the stock list"}},
	}
	for _, c := range cases {
		_, err := placeVeil(c.b)
		if err == nil {
			t.Errorf("%s: not refused", c.name)
			continue
		}
		for _, w := range c.want {
			if !strings.Contains(err.Error(), w) {
				t.Errorf("%s: %q does not name %q", c.name, err, w)
			}
		}
	}
}

func TestTheVeilStepPutsTheEditedEquipmentIntoThePack(t *testing.T) {
	var name uint64 = equipmentName // not a constant: the id overflows a constant int64 conversion
	key := radpack.Key{Type: jsonResourceType, Name: int64(name)}
	data := t.TempDir()
	if err := radpack.WriteStockSet(data, Sets[0], []map[radpack.Key][]byte{{key: jsonResource(syntheticTacmods)}}); err != nil {
		t.Fatal(err)
	}
	s, _, err := radpack.OpenSet(data, Sets[0])
	if err != nil {
		t.Fatal(err)
	}
	p := &Pack{sets: map[string]*radpack.Set{Sets[0]: s}, changes: map[string]map[radpack.Key][]byte{}, scripts: map[string][]byte{}}
	var ran int
	for _, st := range steps {
		if st.Name != veilStepName {
			continue
		}
		ran++
		if err := st.Apply(p); err != nil {
			t.Fatal(err)
		}
	}
	if ran != 1 {
		t.Fatalf("the production steps hold %d steps named %q, want 1", ran, veilStepName)
	}
	got := p.changes[Sets[0]]
	if len(got) != 1 || !strings.Contains(string(got[key][jsonHeader:]), `{"name":"smoke","display_name":"UMBRAL VEIL"}]`) {
		t.Errorf("changes %v: mp_equipment does not carry the veil", got)
	}
}

// veilPack is a pack over a stock set that holds one mp_equipment resource, body.
func veilPack(t *testing.T, body string) (*Pack, radpack.Key) {
	t.Helper()
	var name uint64 = equipmentName // not a constant: the id overflows a constant int64 conversion
	key := radpack.Key{Type: jsonResourceType, Name: int64(name)}
	data := t.TempDir()
	if err := radpack.WriteStockSet(data, Sets[0], []map[radpack.Key][]byte{{key: jsonResource(body)}}); err != nil {
		t.Fatal(err)
	}
	s, _, err := radpack.OpenSet(data, Sets[0])
	if err != nil {
		t.Fatal(err)
	}
	return &Pack{sets: map[string]*radpack.Set{Sets[0]: s}, changes: map[string]map[radpack.Key][]byte{}, scripts: map[string][]byte{}}, key
}

func TestTheVeilStepNamesTheRecordItRefuses(t *testing.T) {
	p, _ := veilPack(t, `{"gear_table":{"tacmods":[{"name":"smoke"}]}}`)
	for _, st := range steps {
		if st.Name != veilStepName {
			continue
		}
		if err := st.Apply(p); err == nil || !strings.HasPrefix(err.Error(), "mp_equipment: ") {
			t.Errorf("error %v, want one that starts with the record's name", err)
		}
	}
}

// The declared field (Step.Writes) reads the entry the step writes, and only it.
func TestTheVeilFieldReadsTheVeilEntry(t *testing.T) {
	out, err := placeVeil(jsonResource(syntheticTacmods))
	if err != nil {
		t.Fatal(err)
	}
	var declared int
	for _, st := range steps {
		if st.Name != veilStepName {
			continue
		}
		for _, f := range st.Writes {
			declared++
			if len(f.Records) != 1 || f.Records[0] != equipmentName || f.Type != jsonResourceType || f.Set != Sets[0] {
				t.Errorf("field %q is not mp_equipment's", f.Name)
			}
			got, err := f.Read(out)
			if want := `{"name":"smoke","display_name":"UMBRAL VEIL"}` + "\n"; err != nil || string(got) != want {
				t.Errorf("field %q read %q, %v; want %q", f.Name, got, err, want)
			}
			if got, err := f.Read(jsonResource(syntheticTacmods)); err != nil || len(got) != 0 {
				t.Errorf("field %q read %q, %v from the stock record, want nothing", f.Name, got, err)
			}
		}
	}
	if declared != 1 {
		t.Errorf("the veil step declares %d fields, want 1", declared)
	}
}

// tacmodTailFixture is a stock-shaped mp_equipment: two tac mods of the game's, REPAIR MATRIX ("heal", which the
// OVERCHARGE step requires) among them; no text of the game's is used.
const tacmodTailFixture = `{"gear_table":{"weapons":[{"name":"w1"}],"tacmods":[{"name":"heal","display_name":"H"},{"name":"t2"}]}}`

// The runtime hides EchoCombat's tac mods only when the list ENDS with smoke, buff, point (src/runtime/patch/ec_rules.h
// kNewTacMods, OursAtEnd): the production tac-mod steps, in the order they are registered and in every other order, leave
// exactly that tail over a stock list.
func TestTheProductionTacmodStepsEndTheListWithSmokeBuffPoint(t *testing.T) {
	var tac []Step
	for _, st := range steps {
		for _, f := range st.Writes {
			if len(f.Records) == 1 && f.Records[0] == equipmentName && strings.HasPrefix(f.Name, "gear_table.tacmods") {
				tac = append(tac, st)
				break
			}
		}
	}
	if len(tac) != len(tacModOrder) {
		t.Fatalf("%d production steps write gear_table.tacmods, want %d (one per tac mod of %v)", len(tac), len(tacModOrder), tacModOrder)
	}
	want := append([]string{"heal", "t2"}, tacModOrder...)
	run := func(label string, order []Step) {
		p, key := veilPack(t, tacmodTailFixture)
		for _, st := range order {
			if err := st.Apply(p); err != nil {
				t.Fatalf("%s: %s: %v", label, st.Name, err)
			}
		}
		b, err := p.Read(Sets[0], key.Type, key.Name)
		if err != nil {
			t.Fatal(err)
		}
		if got := tacmodNames(t, b); !slices.Equal(got, want) { // steps_point_test.go
			t.Errorf("%s: tacmods %v, want %v", label, got, want)
		}
	}
	run("production order", tac)
	for _, perm := range [][]int{{0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}} {
		var order []Step
		var names []string
		for _, i := range perm {
			order = append(order, tac[i])
			names = append(names, tac[i].Name)
		}
		run(strings.Join(names, ", then "), order)
	}
}
