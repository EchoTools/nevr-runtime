package main

// The POINT (NEUTRON) data rules over synthetic records with the shipped layout; no text or row of the game's is used.

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"math"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

const stockPointSettings = `{"ability_properties":{"heal":{"x":1},"point":{"radius":2.5,"charge_required":45,"charge_up_time":1.25,"stun_duration":1.25},"z":{}},"after":[1]}`

// tacmods is an mp_equipment body whose tac mod list holds the named entries, in order.
func tacmods(names ...string) string {
	var list []string
	for _, n := range names {
		list = append(list, `{"name":"`+n+`","display_name":"`+strings.ToUpper(n)+`"}`)
	}
	return `{"gear_table":{"weapons":[],"tacmods":[` + strings.Join(list, ",") + `]},"after":[1]}`
}

// tacmodNames are the names of gear_table.tacmods in the resource, in order.
func tacmodNames(t *testing.T, b []byte) []string {
	t.Helper()
	if binary.LittleEndian.Uint64(b[jsonLengthOff:]) != uint64(len(b)-jsonHeader) {
		t.Fatal("the header's length is not the body's")
	}
	var v struct {
		GearTable struct {
			Tacmods []gearEntry `json:"tacmods"`
		} `json:"gear_table"`
	}
	if err := json.Unmarshal(b[jsonHeader:], &v); err != nil {
		t.Fatal(err)
	}
	var names []string
	for _, e := range v.GearTable.Tacmods {
		names = append(names, e.Name)
	}
	return names
}

func TestTheTacmodListOffersPointAfterTheStockEntries(t *testing.T) {
	in := jsonResource(`{"gear_table":{"weapons":[],"tacmods":[{"name":"heal","display_name":"REPAIR MATRIX"},{"name":"wraith","display_name":"PHASE SHIFT"}]},"after":[1]}`)
	out, err := addPointTacmod(in)
	if err != nil {
		t.Fatal(err)
	}
	want := `{"gear_table":{"weapons":[],"tacmods":[{"name":"heal","display_name":"REPAIR MATRIX"},{"name":"wraith","display_name":"PHASE SHIFT"},{"name":"point","display_name":"NEUTRON"}]},"after":[1]}`
	if got := string(out[jsonHeader:]); got != want {
		t.Errorf("body\n got %s\nwant %s", got, want)
	}
	if binary.LittleEndian.Uint64(out[jsonLengthOff:]) != uint64(len(want)) || !bytes.Equal(out[:jsonLengthOff], in[:jsonLengthOff]) {
		t.Error("the header's length is not the new body's, or another header byte changed")
	}
	if _, err := addPointTacmod(out); err == nil || !strings.Contains(err.Error(), "already lists") {
		t.Errorf("a list that offers point was changed: %v", err)
	}
}

// The runtime's tac-mod hide (src/runtime/patch/ec_rules.h kNewTacMods, OursAtEnd) needs the list to end smoke, buff, point
// (TestTheTacModOrderIsTheRuntimes pins tacModOrder to the header). point is placed by rank (insertTacmod), after any smoke
// and buff already listed, whichever of them is listed and whatever their place.
func TestPointLandsLastWhateverOfSmokeAndBuffIsAlreadyListed(t *testing.T) {
	for _, c := range [][]string{
		{"heal", "sensor", "shield", "wraith", "smoke", "buff"},
		{"heal", "sensor", "shield", "wraith", "smoke"},
		{"heal", "sensor", "shield", "wraith", "buff"},
		{"heal", "sensor", "shield", "wraith"},
		{"smoke", "buff"},
		{},
	} {
		out, err := addPointTacmod(jsonResource(tacmods(c...)))
		if err != nil {
			t.Fatalf("%v: %v", c, err)
		}
		want := strings.Join(append(append([]string(nil), c...), "point"), ",")
		if got := strings.Join(tacmodNames(t, out), ","); got != want {
			t.Errorf("%v: tacmods %s, want %s", c, got, want)
		}
	}
}

// The rank rule for smoke and buff is the landed helper's (TestATacModIsInsertedAtItsRank); point's step refuses what is
// not an mp_equipment resource with a tac-mod list of entries.
func TestAPointTacmodListThatIsNotTheExpectedStockIsRefused(t *testing.T) {
	for name, body := range map[string]string{
		"no tacmods":             `{"gear_table":{"weapons":[]}}`,
		"tacmods not a list":     `{"gear_table":{"tacmods":{}}}`,
		"an entry not an object": `{"gear_table":{"tacmods":[1]}}`,
	} {
		if _, err := addPointTacmod(jsonResource(body)); err == nil {
			t.Errorf("%s was changed", name)
		}
	}
	if _, err := addPointTacmod([]byte("short")); err == nil {
		t.Error("a non-resource was changed")
	}
}

func TestThePointSettingsGetRequiredChargeAndNothingElseChanges(t *testing.T) {
	in := jsonResource(stockPointSettings)
	out, err := addRequiredCharge(in)
	if err != nil {
		t.Fatal(err)
	}
	want := strings.Replace(stockPointSettings, `"stun_duration":1.25}`, `"stun_duration":1.25,"required_charge":45}`, 1)
	if got := string(out[jsonHeader:]); got != want {
		t.Errorf("body\n got %s\nwant %s", got, want)
	}
	if binary.LittleEndian.Uint64(out[jsonLengthOff:]) != uint64(len(want)) || !bytes.Equal(out[:jsonLengthOff], in[:jsonLengthOff]) {
		t.Error("the header's length is not the new body's, or another header byte changed")
	}
	if _, err := addRequiredCharge(out); err == nil || !strings.Contains(err.Error(), "already has required_charge") {
		t.Errorf("a second run was not refused: %v", err)
	}
}

func TestPointSettingsThatDifferFromTheExpectedStockAreRefusedNamingTheField(t *testing.T) {
	for _, c := range []struct{ key, old, new string }{
		{"radius", `"radius":2.5`, `"radius":3`},
		{"charge_required", `"charge_required":45`, `"charge_required":40`},
		{"charge_up_time", `"charge_up_time":1.25`, `"charge_up_time":2`},
		{"stun_duration", `"stun_duration":1.25`, `"stun_duration":2`},
	} {
		_, err := addRequiredCharge(jsonResource(strings.Replace(stockPointSettings, c.old, c.new, 1)))
		if err == nil || !strings.Contains(err.Error(), "point."+c.key+" is") || !strings.Contains(err.Error(), "expected") {
			t.Errorf("%s: %v", c.key, err)
		}
	}
	for name, body := range map[string]string{
		"no point object":     `{"ability_properties":{"heal":{}}}`,
		"point not an object": `{"ability_properties":{"point":[1]}}`,
		"a missing field":     `{"ability_properties":{"point":{"radius":2.5}}}`,
	} {
		if _, err := addRequiredCharge(jsonResource(body)); err == nil {
			t.Errorf("%s was changed", name)
		}
	}
	if _, err := addRequiredCharge([]byte("short")); err == nil {
		t.Error("a non-resource was changed")
	}
}

func explosionRow(actor, explosion uint64, fill byte) []byte {
	r := bytes.Repeat([]byte{fill}, explosionStride)
	radpack.SetU64(r, 0, explosion)
	radpack.SetU64(r, keyOff, actor)
	return r
}

func TestTheBarrierWallGetsTheStunMinesExplosionRowWithPointsRadius(t *testing.T) {
	stun := explosionRow(stunMineActor, stunExplosion, 0x11)
	other := explosionRow(0x1234, 0x5678, 0x22)
	tb := parse(t, table(explosionStride, nil, other, stun))
	if err := pointExplosionRow(tb); err != nil {
		t.Fatal(err)
	}
	out := parse(t, tb.Bytes())
	if len(out.Rows) != 3 || !bytes.Equal(out.Rows[0], other) || !bytes.Equal(out.Rows[1], stun) {
		t.Fatalf("%d rows, or a stock row changed", len(out.Rows))
	}
	w := out.Rows[2]
	want := append([]byte(nil), stun...)
	radpack.SetU64(want, keyOff, barrierWall)
	copy(want[explosionMask:], explosionWallMask[:])
	binary.LittleEndian.PutUint32(want[explosionRadius:], math.Float32bits(2.5))
	binary.LittleEndian.PutUint32(want[explosionFar:], math.Float32bits(2.5))
	copy(want[explosionLink:], explosionWallLink[:])
	if !bytes.Equal(w, want) {
		t.Errorf("wall row %x\n want %x", w, want)
	}
	if w[16] != 0xff || w[17] != 0xff || w[18] != 0x1f || w[19] != 0 || w[80] != 0x98 || w[87] != 0x2f {
		t.Errorf("mask or link bytes %x %x", w[16:24], w[80:88])
	}
	if err := pointExplosionRow(out); err == nil || !strings.Contains(err.Error(), "already") {
		t.Errorf("a second run was not refused: %v", err)
	}
}

func TestAnExplosionTableThatIsNotTheExpectedStockIsRefused(t *testing.T) {
	stun := explosionRow(stunMineActor, stunExplosion, 0x11)
	for name, b := range map[string][]byte{
		"no stun mine row":                table(explosionStride, nil, explosionRow(1, 2, 0)),
		"two stun mine rows":              table(explosionStride, nil, stun, stun),
		"a stun row of another explosion": table(explosionStride, nil, explosionRow(stunMineActor, 5, 0)),
		"rows of another size":            table(128, nil, row(128, map[int]uint64{keyOff: stunMineActor})),
		"a tail":                          table(explosionStride, []byte("tail"), stun),
	} {
		if err := pointExplosionRow(parse(t, b)); err == nil {
			t.Errorf("%s: changed", name)
		}
	}
}

// Each NEUTRON step's declared field (Step.Writes) reads what the step wrote, and only that.
func TestThePointFieldsReadWhatTheirStepsWrite(t *testing.T) {
	fields := map[string]Field{}
	for _, st := range steps {
		switch st.Name {
		case pointTacmodStep, pointSettingsStep, pointExplosionStep:
			if len(st.Writes) != 1 {
				t.Fatalf("%s declares %d fields, want 1", st.Name, len(st.Writes))
			}
			fields[st.Name] = st.Writes[0]
		}
	}
	if len(fields) != 3 {
		t.Fatalf("%d NEUTRON steps registered, want 3", len(fields))
	}

	tac := fields[pointTacmodStep]
	if tac.Type != jsonResourceType || len(tac.Records) != 1 || tac.Records[0] != equipmentName {
		t.Errorf("tac-mod field %+v", tac)
	}
	out, err := addPointTacmod(jsonResource(tacmods("heal", "smoke", "buff")))
	if err != nil {
		t.Fatal(err)
	}
	if got, err := tac.Read(out); err != nil || string(got) != `{"name":"point","display_name":"NEUTRON"}`+"\n" {
		t.Errorf("tac-mod field read %q, %v", got, err)
	}
	if got, err := tac.Read(jsonResource(tacmods("heal", "smoke", "buff"))); err != nil || len(got) != 0 {
		t.Errorf("tac-mod field of the stock list read %q, %v", got, err)
	}

	set := fields[pointSettingsStep]
	var settingsName uint64 = weaponSettingsName
	if set.Type != jsonResourceType || len(set.Records) != 1 || set.Records[0] != settingsName {
		t.Errorf("settings field %+v", set)
	}
	out, err = addRequiredCharge(jsonResource(stockPointSettings))
	if err != nil {
		t.Fatal(err)
	}
	if got, err := set.Read(out); err != nil || string(got) != "45" {
		t.Errorf("required_charge read %q, %v", got, err)
	}
	if _, err := set.Read(jsonResource(stockPointSettings)); err == nil {
		t.Error("stock settings without required_charge were read")
	}
	if _, err := set.Read(jsonResource(`{"ability_properties":{"point":{"required_charge":"x"}}}`)); err == nil {
		t.Error("a required_charge that is not a number was read")
	}

	ex := fields[pointExplosionStep]
	if ex.Type != explosionTable || len(ex.Records) != len(droneLevels) {
		t.Errorf("explosion field %+v", ex)
	}
	stun := explosionRow(stunMineActor, stunExplosion, 0x11)
	tb := parse(t, table(explosionStride, nil, explosionRow(0x1234, 0x5678, 0x22), stun))
	if got, err := ex.Read(tb.Bytes()); err != nil || len(got) != 0 {
		t.Errorf("explosion field of the stock table read %x, %v", got, err)
	}
	if err := pointExplosionRow(tb); err != nil {
		t.Fatal(err)
	}
	if got, err := ex.Read(tb.Bytes()); err != nil || !bytes.Equal(got, tb.Rows[2]) {
		t.Errorf("explosion field read %x, want the wall row %x (%v)", got, tb.Rows[2], err)
	}
}
