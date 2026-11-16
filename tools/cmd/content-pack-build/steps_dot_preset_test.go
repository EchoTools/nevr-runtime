package main

// The DOT FIELD preset rule over a synthetic mp_weapon_settings of the shipped layout (a 0x220-byte header, a minified
// JSON body); no text of the game's is used. stock_steps_test.go runs the step over the shipped data when a machine has it.

import (
	"encoding/binary"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

// dotSettings is a settings body whose dot object has the shipped activation values (all three true), with a sibling
// ordnance that carries one of the same keys, which the rule must leave alone.
func dotSettings(dot string) string {
	return `{"damage_table":{"arc":{"head":1}},"grenade_properties":{"arc":{"activate_on_collision":true,"stick_to_geo":true},` +
		`"dot":{` + dot + `}},"after":[1]}`
}

const stockDot = `"radius":2.25,"activate_on_collision":true,"use_remote_trigger":true,"stick_to_geo":true,"z":[1,2]`

func dotWith(a, r, s string) string {
	return `"radius":2.25,"activate_on_collision":` + a + `,"use_remote_trigger":` + r + `,"stick_to_geo":` + s + `,"z":[1,2]`
}

func TestTheDefaultPresetClearsActivateOnCollisionAndNothingElse(t *testing.T) {
	in := jsonResource(dotSettings(stockDot))
	out, err := applyDotPreset(in, dotDefaultPreset)
	if err != nil {
		t.Fatal(err)
	}
	want := dotSettings(dotWith("false", "true", "true"))
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

func TestEachPresetWritesItsThreeValues(t *testing.T) {
	// fix_dot.py:99-124: activate_on_collision, use_remote_trigger, stick_to_geo.
	cases := map[string][3]string{
		"trigger":         {"false", "true", "true"},
		"trigger-nostick": {"false", "true", "false"},
		"impact":          {"true", "false", "true"},
		"impact-nostick":  {"true", "false", "false"},
		"stock":           {"true", "true", "true"},
	}
	if len(cases) != len(dotPresets) {
		t.Fatalf("the rule has %d presets, the test %d", len(dotPresets), len(cases))
	}
	for name, v := range cases {
		out, err := applyDotPreset(jsonResource(dotSettings(stockDot)), name)
		if err != nil {
			t.Errorf("%s: %v", name, err)
			continue
		}
		if got, want := string(out[jsonHeader:]), dotSettings(dotWith(v[0], v[1], v[2])); got != want {
			t.Errorf("%s\n got %s\nwant %s", name, got, want)
		}
	}
	if dotDefaultPreset != "trigger" {
		t.Errorf("the default preset is %q, fix_dot.py:125 makes it trigger", dotDefaultPreset)
	}
}

func TestAStockValueThatDiffersIsRefusedNamingTheFieldTheExpectedAndTheActual(t *testing.T) {
	cases := []struct {
		name, body string
		want       []string
	}{
		{"activate_on_collision already false", dotSettings(dotWith("false", "true", "true")),
			[]string{"grenade_properties.dot.activate_on_collision", "expected true", "actual false"}},
		{"use_remote_trigger a string", dotSettings(dotWith("true", `"yes"`, "true")),
			[]string{"grenade_properties.dot.use_remote_trigger", "expected true", `actual "yes"`}},
		{"stick_to_geo a number", dotSettings(dotWith("true", "true", "0")),
			[]string{"grenade_properties.dot.stick_to_geo", "expected true", "actual 0"}},
		{"stick_to_geo absent", dotSettings(`"activate_on_collision":true,"use_remote_trigger":true`),
			[]string{"grenade_properties.dot", `no key "stick_to_geo"`}},
		{"no dot ordnance", `{"grenade_properties":{"arc":{}}}`, []string{"grenade_properties", `no key "dot"`}},
		{"dot is a list", `{"grenade_properties":{"dot":[]}}`, []string{"grenade_properties.dot", "not an object"}},
	}
	for _, c := range cases {
		_, err := applyDotPreset(jsonResource(c.body), dotDefaultPreset)
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

func TestAResourceTheRuleCannotReadAndAnUnknownPresetAreRefused(t *testing.T) {
	good := dotSettings(stockDot)
	wrongLength := jsonResource(good)
	binary.LittleEndian.PutUint64(wrongLength[jsonLengthOff:], uint64(len(good)-1))
	if _, err := applyDotPreset(wrongLength, dotDefaultPreset); err == nil || !strings.Contains(err.Error(), "not a JSON resource") {
		t.Errorf("a wrong length: %v", err)
	}
	if _, err := applyDotPreset(make([]byte, 8), dotDefaultPreset); err == nil || !strings.Contains(err.Error(), "not a JSON resource") {
		t.Errorf("a short resource: %v", err)
	}
	if _, err := applyDotPreset(jsonResource(good), "nope"); err == nil || !strings.Contains(err.Error(), `unknown preset "nope"`) {
		t.Errorf("an unknown preset: %v", err)
	}
}

// runDotStep runs the registered step over a pack whose mp_weapon_settings has `body`.
func runDotStep(t *testing.T, body string) (*Pack, radpack.Key, error) {
	t.Helper()
	var name uint64 = weaponSettingsName
	key := radpack.Key{Type: jsonResourceType, Name: int64(name)}
	data := t.TempDir()
	if err := radpack.WriteStockSet(data, Sets[0], []map[radpack.Key][]byte{{key: jsonResource(body)}}); err != nil {
		t.Fatal(err)
	}
	s, _, err := radpack.OpenSet(data, Sets[0])
	if err != nil {
		t.Fatal(err)
	}
	p := &Pack{sets: map[string]*radpack.Set{Sets[0]: s}, changes: map[string]map[radpack.Key][]byte{}, scripts: map[string][]byte{}}
	ran := 0
	var applyErr error
	for _, st := range steps {
		if st.Name != dotPresetStepName {
			continue
		}
		ran++
		applyErr = st.Apply(p)
	}
	if ran != 1 {
		t.Fatalf("%d steps are named %q, want 1", ran, dotPresetStepName)
	}
	return p, key, applyErr
}

func TestTheDotPresetStepRefusesAChangedStockNamingTheRecord(t *testing.T) {
	p, _, err := runDotStep(t, dotSettings(dotWith("false", "true", "true")))
	if err == nil || !strings.Contains(err.Error(), "mp_weapon_settings: grenade_properties.dot.activate_on_collision") {
		t.Errorf("not refused naming the record and field: %v", err)
	}
	if len(p.changes[Sets[0]]) != 0 {
		t.Error("a refused step changed the pack")
	}
}

func TestTheDotPresetStepRewritesMpWeaponSettingsInThePack(t *testing.T) {
	p, key, err := runDotStep(t, dotSettings(stockDot))
	if err != nil {
		t.Fatal(err)
	}
	if len(p.changes[Sets[0]]) != 1 {
		t.Errorf("%d resources changed, want only mp_weapon_settings", len(p.changes[Sets[0]]))
	}
	got := p.changes[Sets[0]][key]
	if want := dotSettings(dotWith("false", "true", "true")); len(got) < jsonHeader || string(got[jsonHeader:]) != want {
		t.Errorf("the pack's mp_weapon_settings is not the default preset's")
	}
	if strings.Contains(dotPresetStepName, "drone") {
		t.Error("a step name with \"drone\" is picked up by the five-level drone test")
	}
}

// The step's declared fields (Step.Writes) read the three values it writes, from its own output and by value from a record
// laid out differently, and not the sibling ordnance's keys of the same name.
func TestTheDotPresetFieldsReadWhatTheStepWrites(t *testing.T) {
	p, key, err := runDotStep(t, dotSettings(stockDot))
	if err != nil {
		t.Fatal(err)
	}
	var writes []Field
	for _, st := range steps {
		if st.Name == dotPresetStepName {
			writes = st.Writes
		}
	}
	if len(writes) != len(dotPresetFields) {
		t.Fatalf("%d fields declared, want %d", len(writes), len(dotPresetFields))
	}
	spaced := jsonResource(`{"grenade_properties": {"arc": {"activate_on_collision": true}, "dot": {"activate_on_collision": false,` +
		` "use_remote_trigger": true, "stick_to_geo": true}}}`)
	stock := jsonResource(dotSettings(stockDot))
	want := [3]string{"false", "true", "true"}
	for i, f := range writes {
		if f.Name != "grenade_properties.dot."+dotPresetFields[i] || f.Type != jsonResourceType || len(f.Records) != 1 || f.Records[0] != weaponSettingsName {
			t.Errorf("field %d: %+v", i, f)
		}
		for what, rec := range map[string][]byte{"the step's output": p.changes[Sets[0]][key], "a spaced record": spaced} {
			if got, err := f.Read(rec); err != nil || string(got) != want[i] {
				t.Errorf("%s of %s: %q, %v, want %s", f.Name, what, got, err, want[i])
			}
		}
		if got, err := f.Read(stock); err != nil || string(got) != "true" {
			t.Errorf("%s of the stock record: %q, %v, want true", f.Name, got, err)
		}
	}
	if _, err := writes[0].Read(jsonResource(`{"grenade_properties":{"dot":{"activate_on_collision":"no"}}}`)); err == nil {
		t.Error("a value that is not a boolean was read")
	}
	if _, err := writes[0].Read([]byte("short")); err == nil {
		t.Error("a resource the reader cannot frame was read")
	}
}
