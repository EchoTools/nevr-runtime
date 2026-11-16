package main

// The drone health rule over synthetic damageable tables; no row of the game's is used.

import (
	"bytes"
	"encoding/binary"
	"math"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

const (
	damageableRow = 0x58 // a size for the synthetic rows; the step needs only that they reach +0x28
	hpAt          = 0x24 // the health's offset as his tool has it (fix_drone_health.py:39), kept apart from the step's own constant
)

func healthRow(actor uint64, health float32) []byte {
	r := row(damageableRow, map[int]uint64{0: 0x77, keyOff: actor})
	binary.LittleEndian.PutUint32(r[hpAt:], math.Float32bits(health))
	return r
}

func healthOf(r []byte) float32 {
	return math.Float32frombits(binary.LittleEndian.Uint32(r[hpAt:]))
}

func TestTheDronesHealthGoesFromTheStockValueToTheConfigsAndNothingElseChanges(t *testing.T) {
	b := table(damageableRow, nil, healthRow(0xa1, stockHealth), healthRow(droneActor, stockHealth), healthRow(0xa2, 12))
	tb := parse(t, b)
	if err := droneHealthRow(tb); err != nil {
		t.Fatal(err)
	}
	out := parse(t, tb.Bytes())
	if got := healthOf(out.Rows[1]); got != 70 {
		t.Errorf("drone health %v, want 70", got)
	}
	if healthOf(out.Rows[0]) != stockHealth || healthOf(out.Rows[2]) != 12 {
		t.Error("another actor's health changed")
	}
	want := parse(t, b)
	want.Rows[1][hpAt] = out.Rows[1][hpAt]
	for i := range want.Rows[1] {
		if i >= hpAt && i < hpAt+4 {
			continue
		}
		if want.Rows[1][i] != out.Rows[1][i] {
			t.Errorf("byte %#x of the drone's record changed", i)
		}
	}
	if !bytes.Equal(tb.Bytes()[:0x38], b[:0x38]) || len(tb.Bytes()) != len(b) {
		t.Error("the header or the size changed")
	}
}

func TestADroneAlreadyAtTheTargetIsLeftAlone(t *testing.T) {
	b := table(damageableRow, nil, healthRow(droneActor, droneHealth))
	tb := parse(t, b)
	if err := droneHealthRow(tb); err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(tb.Bytes(), b) {
		t.Error("a table already at the target changed")
	}
}

func TestADamageableTableThatIsNotTheStockOneIsRefused(t *testing.T) {
	cases := []struct {
		name, want string
		b          []byte
	}{
		{"other health", "not the stock", table(damageableRow, nil, healthRow(droneActor, 55))},
		{"no drone", "no damageable record", table(damageableRow, nil, healthRow(0xa1, stockHealth))},
		{"two drones", "more than one", table(damageableRow, nil, healthRow(droneActor, stockHealth), healthRow(droneActor, stockHealth))},
		{"short rows", "end before the health", table(0x20, nil, row(0x20, map[int]uint64{keyOff: droneActor}))},
		{"a tail", "after its rows", table(damageableRow, []byte{1, 2, 3, 4, 5, 6, 7, 8}, healthRow(droneActor, stockHealth))},
	}
	for _, c := range cases {
		tb := parse(t, c.b)
		before := tb.Bytes()
		err := droneHealthRow(tb)
		if err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: err %v, want one naming %q", c.name, err, c.want)
		}
		if !bytes.Equal(tb.Bytes(), before) {
			t.Errorf("%s: a refused table was changed", c.name)
		}
	}
}

func TestTheDroneHealthStepChangesTheFiveCombatLevelsAndNoOther(t *testing.T) {
	const other = 0x1234
	var damageableType uint64 = damageableTable // not a constant: the id overflows a constant int64 conversion
	res := map[radpack.Key][]byte{}
	for _, lvl := range append(append([]uint64(nil), droneLevels...), other) {
		res[radpack.Key{Type: int64(damageableType), Name: int64(lvl)}] = table(damageableRow, nil, healthRow(droneActor, stockHealth))
	}
	data := t.TempDir()
	if err := radpack.WriteStockSet(data, Sets[0], []map[radpack.Key][]byte{res}); err != nil {
		t.Fatal(err)
	}
	s, _, err := radpack.OpenSet(data, Sets[0])
	if err != nil {
		t.Fatal(err)
	}
	p := &Pack{sets: map[string]*radpack.Set{Sets[0]: s}, changes: map[string]map[radpack.Key][]byte{}, scripts: map[string][]byte{}}
	n := 0
	for _, st := range steps {
		if st.Name != "damageable health of actor 10e40287a9bf5a13 9999 to 70" {
			continue
		}
		n++
		if err := st.Apply(p); err != nil {
			t.Fatal(err)
		}
	}
	if n != 1 {
		t.Fatalf("%d steps named for the drone health, want 1", n)
	}
	got := p.changes[Sets[0]]
	if len(got) != len(droneLevels) {
		t.Errorf("%d resources changed, want %d", len(got), len(droneLevels))
	}
	for _, lvl := range droneLevels {
		b := got[radpack.Key{Type: int64(damageableType), Name: int64(lvl)}]
		if b == nil || healthOf(parse(t, b).Rows[0]) != 70 {
			t.Errorf("level %016x: the drone's health is not 70", lvl)
		}
	}
	if _, ok := got[radpack.Key{Type: int64(damageableType), Name: other}]; ok {
		t.Error("a level that is not a combat level was changed")
	}
}
