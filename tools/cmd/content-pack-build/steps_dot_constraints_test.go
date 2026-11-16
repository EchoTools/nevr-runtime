package main

import (
	"bytes"
	"fmt"
	"slices"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

// The DOT constraint rule over synthetic tables with the shipped layout; no row of the game's is used.

const (
	constraintRow = 96 // a synthetic row size; the real ones' size is the table's own
	shieldActor   = 0x3d8b8ca4d5292926
)

func constraintTable(tail []byte, actors ...uint64) []byte {
	rows := make([][]byte, len(actors))
	for i, a := range actors {
		rows[i] = row(constraintRow, map[int]uint64{0: uint64(0x100 + i), keyOff: a, 0x18: uint64(i + 1)})
	}
	return table(constraintRow, tail, rows...)
}

func TestTheDotMagazinesConstraintRowIsDroppedAndTheShieldsStays(t *testing.T) {
	b := constraintTable(nil, 0x7777, dotMagazineActor, shieldActor)
	tb := parse(t, b)
	if err := dropDotConstraint(tb); err != nil {
		t.Fatal(err)
	}
	out := parse(t, tb.Bytes())
	if len(out.Rows) != 2 || out.Find(keyOff, dotMagazineActor) >= 0 {
		t.Fatalf("%d rows, DOT row at %d", len(out.Rows), out.Find(keyOff, dotMagazineActor))
	}
	if radpack.U64(out.Rows[0], keyOff) != 0x7777 || radpack.U64(out.Rows[1], keyOff) != shieldActor || radpack.U64(out.Rows[1], 0x18) != 3 {
		t.Errorf("the other rows changed: %x / %x", out.Rows[0][:16], out.Rows[1][:16])
	}
	if got := len(tb.Bytes()); got != len(b)-constraintRow {
		t.Errorf("table is %d bytes, want %d", got, len(b)-constraintRow)
	}
}

func TestADotConstraintTableThatIsNotTheStockOneIsRefused(t *testing.T) {
	for name, c := range map[string]struct {
		table []byte
		want  string
	}{
		"no DOT row (already removed)": {constraintTable(nil, 0x7777, shieldActor), "has exactly 1"},
		"two DOT rows":                 {constraintTable(nil, dotMagazineActor, shieldActor, dotMagazineActor), "has exactly 1"},
		"only the DOT row":             {constraintTable(nil, dotMagazineActor), "empty the table"},
		"a tail after the rows":        {constraintTable([]byte("heap"), dotMagazineActor, shieldActor), "tail"},
		"rows too narrow for the id":   {table(8, nil, make([]byte, 8), make([]byte, 8)), "end before the actor id"},
	} {
		tb := parse(t, c.table)
		before := string(tb.Bytes())
		if err := dropDotConstraint(tb); err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: error %v, want one naming %q", name, err, c.want)
		}
		if string(tb.Bytes()) != before {
			t.Errorf("%s: the table was changed before it was refused", name)
		}
	}
}

func TestTheDotConstraintStepsChangeTheTwoTablesOfTheFiveCombatLevelsAndNoOther(t *testing.T) {
	const other = 0x1234
	var rigid, parented uint64 = rigidAttachTable, parentedTable // not constants here: the ids overflow int64 conversions
	res := map[radpack.Key][]byte{}
	for _, lvl := range append(append([]uint64(nil), droneLevels...), other) {
		for _, typ := range []uint64{rigid, parented} {
			res[radpack.Key{Type: int64(typ), Name: int64(lvl)}] = constraintTable(nil, dotMagazineActor, shieldActor)
		}
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
	ran := 0
	for _, st := range steps {
		if !strings.HasPrefix(st.Name, "DOT FIELD magazine drops") {
			continue
		}
		ran++
		if err := st.Apply(p); err != nil {
			t.Fatalf("%s: %v", st.Name, err)
		}
	}
	if ran != 2 {
		t.Fatalf("%d DOT constraint steps registered, want 2", ran)
	}
	got := p.changes[Sets[0]]
	if len(got) != 10 {
		t.Errorf("%d resources changed, want 10", len(got))
	}
	for _, lvl := range droneLevels {
		for _, typ := range []uint64{rigid, parented} {
			b := got[radpack.Key{Type: int64(typ), Name: int64(lvl)}]
			if b == nil {
				t.Errorf("%016x.%016x was not changed", typ, lvl)
				continue
			}
			if tb := parse(t, b); len(tb.Rows) != 1 || radpack.U64(tb.Rows[0], keyOff) != shieldActor {
				t.Errorf("%016x.%016x: %d rows left", typ, lvl, len(tb.Rows))
			}
		}
	}
	for _, typ := range []uint64{rigid, parented} {
		if _, ok := got[radpack.Key{Type: int64(typ), Name: other}]; ok {
			t.Errorf("table %016x of a level that is not a combat level was changed", typ)
		}
	}
}

// Each DOT constraint step declares one field, the DOT magazine's rows of its own table in the five combat levels: the
// stock table reads as the DOT row, and the table the step wrote reads as none.
func TestTheDotConstraintFieldsReadTheDotRowsTheirStepsDrop(t *testing.T) {
	want := map[string]uint64{fmt.Sprintf(dotConstraintName, "rigid-attach"): rigidAttachTable,
		fmt.Sprintf(dotConstraintName, "parented-constraint"): parentedTable}
	for _, st := range steps {
		typ, ok := want[st.Name]
		if !ok {
			continue
		}
		delete(want, st.Name)
		if len(st.Writes) != 1 {
			t.Fatalf("%s: %d fields declared, want 1", st.Name, len(st.Writes))
		}
		f := st.Writes[0]
		if f.Set != Sets[0] || f.Type != typ || !slices.Equal(f.Records, droneLevels) {
			t.Errorf("%s: field %q is set %s type %016x records %x, want %s %016x %x", st.Name, f.Name, f.Set, f.Type,
				f.Records, Sets[0], typ, droneLevels)
		}
		stock := constraintTable(nil, shieldActor, dotMagazineActor)
		tb := parse(t, stock)
		dot := append([]byte(nil), tb.Rows[1]...)
		if got, err := f.Read(stock); err != nil || !bytes.Equal(got, dot) {
			t.Errorf("%s: the stock table reads as %x (%v), want the DOT row", st.Name, got, err)
		}
		if err := dropDotConstraint(tb); err != nil {
			t.Fatal(err)
		}
		if got, err := f.Read(tb.Bytes()); err != nil || len(got) != 0 {
			t.Errorf("%s: the edited table reads as %x (%v), want no bytes", st.Name, got, err)
		}
	}
	if len(want) != 0 {
		t.Errorf("steps not registered: %v", want)
	}
}
