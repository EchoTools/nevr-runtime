package main

// The OVERCHARGE rules over synthetic records of the shipped layouts; no row or text of the game's is used.
// stock_steps_test.go runs them over the shipped data when a machine has it.

import (
	"bytes"
	"encoding/binary"
	"slices"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

const stockTacmods = `{"gear_table":{"weapons":[{"name":"w1"}],"tacmods":[{"name":"heal","display_name":"REPAIR MATRIX"},{"name":"wraith","display_name":"PHASE SHIFT"}]},"after":[1]}`

func TestOverchargeIsAppendedToTheTacmodsAfterTheStockEntries(t *testing.T) {
	in := jsonResource(stockTacmods)
	out, err := appendTacmod(in)
	if err != nil {
		t.Fatal(err)
	}
	want := `{"gear_table":{"weapons":[{"name":"w1"}],"tacmods":[{"name":"heal","display_name":"REPAIR MATRIX"},` +
		`{"name":"wraith","display_name":"PHASE SHIFT"},{"name":"buff","display_name":"OVERCHARGE"}]},"after":[1]}`
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

// ec_rules.h kNewTacMods: the list must end smoke, buff, point. A list that already has point (steps_point runs after this
// step, but a pack built over one that has) gets buff in front of it, not after it.
func TestOverchargeGoesBeforeAnExistingPointEntry(t *testing.T) {
	for _, c := range []struct{ name, in, want string }{
		{"point last", `{"gear_table":{"tacmods":[{"name":"heal"},{"name":"smoke"},{"name":"point","display_name":"NEUTRON"}]},"x":{"tacmods":1}}`,
			`{"gear_table":{"tacmods":[{"name":"heal"},{"name":"smoke"},{"name":"buff","display_name":"OVERCHARGE"},{"name":"point","display_name":"NEUTRON"}]},"x":{"tacmods":1}}`},
		{"point first after a space", `{"gear_table":{"tacmods":[ {"name":"point"},{"name":"heal"}]}}`,
			`{"gear_table":{"tacmods":[ {"name":"buff","display_name":"OVERCHARGE"},{"name":"point"},{"name":"heal"}]}}`},
	} {
		out, err := appendTacmod(jsonResource(c.in))
		if err != nil {
			t.Fatalf("%s: %v", c.name, err)
		}
		if got := string(out[jsonHeader:]); got != c.want {
			t.Errorf("%s\n got %s\nwant %s", c.name, got, c.want)
		}
		if binary.LittleEndian.Uint64(out[jsonLengthOff:]) != uint64(len(c.want)) {
			t.Errorf("%s: the header's length is not the new body's", c.name)
		}
	}
}

func TestATacmodListThatIsNotTheStockOneIsRefused(t *testing.T) {
	cases := []struct {
		name string
		b    []byte
		want string
	}{
		{"no heal to copy", jsonResource(`{"gear_table":{"tacmods":[{"name":"wraith"}]}}`), "lists no heal"},
		{"buff already listed", jsonResource(`{"gear_table":{"tacmods":[{"name":"heal"},{"name":"buff","display_name":"POWER BOOST"}]}}`), "already lists buff"},
		{"no tacmods list", jsonResource(`{"gear_table":{"weapons":[]}}`), `has no key "tacmods"`},
		{"a list that is an object", jsonResource(`{"gear_table":{"tacmods":{"name":"heal"}}}`), "gear_table.tacmods is not a list"},
		{"a length that is not the body's", jsonResource(`{"gear_table":{"tacmods":[{"name":"heal"}]}}`)[:jsonHeader+5], "not a JSON resource"},
	}
	for _, c := range cases {
		if _, err := appendTacmod(c.b); err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: %v, want an error naming %q", c.name, err, c.want)
		}
	}
}

// bubbleRow is a heal bubble row: +0 the component, +8 the actor, the rest settings the rule must carry over.
func bubbleRow(component, actor uint64, mark byte) []byte {
	r := row(bubbleRowSize, map[int]uint64{bubbleIDOff: component, bubbleActorOff: actor})
	for i := 0x10; i < bubbleRowSize; i++ {
		r[i] = mark + byte(i)
	}
	return r
}

func stockBubbles() [][]byte {
	return [][]byte{bubbleRow(healBubble, ocActors[0], 1), bubbleRow(healBubble, ocActors[1], 2), bubbleRow(healBubble, ocActors[2], 3)}
}

func TestEachOverchargeActorGetsACopyOfItsBubbleRowUnderTheOverchargeBubble(t *testing.T) {
	rows := stockBubbles()
	in := table(bubbleRowSize, []byte("tail"), rows...)
	tb := parse(t, in)
	if err := addOverchargeBubbles(tb); err != nil {
		t.Fatal(err)
	}
	out := parse(t, tb.Bytes())
	if len(out.Rows) != 6 {
		t.Fatalf("%d rows, want the 3 stock rows and 3 more", len(out.Rows))
	}
	for i, r := range rows {
		if !bytes.Equal(out.Rows[i], r) {
			t.Errorf("stock row %d changed", i)
		}
		want := bytes.Clone(r)
		radpack.SetU64(want, bubbleIDOff, ocBubble)
		if !bytes.Equal(out.Rows[3+i], want) {
			t.Errorf("row %d: %x, want the stock row with the OVERCHARGE bubble id: %x", 3+i, out.Rows[3+i], want)
		}
	}
	if !bytes.HasSuffix(tb.Bytes(), []byte("tail")) {
		t.Error("the tail changed")
	}
	if n := binary.LittleEndian.Uint64(tb.Bytes()[0x30:]); n != 6 {
		t.Errorf("header count %d", n)
	}
}

func TestABubbleTableThatIsNotTheStockOneIsRefused(t *testing.T) {
	ok := func(rows ...[]byte) *radpack.RowTable { return parse(t, table(bubbleRowSize, nil, rows...)) }
	s := stockBubbles()
	withOwn := append(stockBubbles(), bubbleRow(ocBubble, ocActors[1], 9))
	twice := append(stockBubbles(), bubbleRow(healBubble, ocActors[0], 9))
	cases := []struct {
		name string
		tb   *radpack.RowTable
		want string
	}{
		{"rows of another size", parse(t, table(80, nil, row(80, nil))), "rows of 80 bytes"},
		{"an actor without a heal bubble", ok(s[0], s[1]), "actor f1e70acd0dde0312 has 0 heal bubble rows"},
		{"an actor with two", ok(twice...), "actor 91d8de0806964a11 has 2 heal bubble rows"},
		{"an actor that already has one", ok(withOwn...), "actor b113997837c29003 already has an OVERCHARGE bubble"},
	}
	for _, c := range cases {
		if err := addOverchargeBubbles(c.tb); err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: %v, want an error naming %q", c.name, err, c.want)
		}
	}
}

func TestTheOverchargeStepsChangeOnlyTheEquipmentListAndTheBubbleTable(t *testing.T) {
	var eqName uint64 = equipmentName // not a constant: the id overflows a constant int64 conversion
	bubbles := table(bubbleRowSize, nil, stockBubbles()...)
	p := &Pack{sets: map[string]*radpack.Set{Sets[0]: {}}, changes: map[string]map[radpack.Key][]byte{Sets[0]: {
		{Type: jsonResourceType, Name: int64(eqName)}:            jsonResource(stockTacmods),
		{Type: int64(healBubbleTable), Name: int64(globalLevel)}: bubbles,
	}}, scripts: map[string][]byte{}}
	for _, st := range steps {
		if st.Name == buffTacmodStep || st.Name == buffBubbleStep {
			if err := st.Apply(p); err != nil {
				t.Fatalf("%s: %v", st.Name, err)
			}
		}
	}
	if n := len(p.changes[Sets[0]]); n != 2 || len(p.scripts) != 0 {
		t.Errorf("%d resources and %d scripts changed, want the two records and no script", n, len(p.scripts))
	}
	eq := p.changes[Sets[0]][radpack.Key{Type: jsonResourceType, Name: int64(eqName)}]
	if !strings.Contains(string(eq), `{"name":"buff","display_name":"OVERCHARGE"}`) {
		t.Errorf("the equipment list has no OVERCHARGE: %s", eq[jsonHeader:])
	}
	if got := len(parse(t, p.changes[Sets[0]][radpack.Key{Type: int64(healBubbleTable), Name: int64(globalLevel)}]).Rows); got != 6 {
		t.Errorf("the bubble table has %d rows, want 6", got)
	}
	bad := &Pack{sets: p.sets, changes: map[string]map[radpack.Key][]byte{Sets[0]: {
		{Type: int64(healBubbleTable), Name: int64(globalLevel)}: table(bubbleRowSize, nil, stockBubbles()[0]),
	}}}
	if err := buffBubbles(bad); err == nil || !strings.Contains(err.Error(), "heal bubble table") {
		t.Errorf("a bubble table without the other actors' rows was not refused: %v", err)
	}
}

// Every other test builds its fixture from these constants, so a wrong value would pass them all: here each is pinned
// to the value written out (fix_buff.py:83, :86, fix_smoke.py:71, and the hash of fix_buff.py:84's name), and the two
// named ones also to their name's hash.
func TestTheOverchargeIdsAreTheirValues(t *testing.T) {
	for _, c := range []struct {
		what      string
		got, want uint64
	}{
		{"ocBubble", ocBubble, 0xb9833267495be341},
		{"healBubble", healBubble, 0xb1fda2a83f3debd1},
		{"healBubbleTable", healBubbleTable, 0x1ad01a2a91709a1a},
		{"globalLevel", globalLevel, 0x3f9915d3001dc28e},
		{`CSymbol64("echocombat_overcharge_bubble")`, ocBubble, csymbol64("echocombat_overcharge_bubble")},
		{`CSymbol64("CR15NetHealBubbleCRWin10")`, healBubbleTable, csymbol64("CR15NetHealBubbleCRWin10")},
	} {
		if c.got != c.want {
			t.Errorf("%s: the step has %016x, want %016x", c.what, c.got, c.want)
		}
	}
}

// His add_bubbles walks the table's rows and appends each copy as it meets the row, so the copies are in the table's
// order, not ocActors' order; a heal row of an actor that is not an OVERCHARGE actor is not copied.
func TestTheBubbleCopiesAreAppendedInTheTablesRowOrder(t *testing.T) {
	const otherActor = 0x1111222233334444
	rows := [][]byte{
		bubbleRow(healBubble, ocActors[2], 3),
		bubbleRow(healBubble, otherActor, 4),
		bubbleRow(healBubble, ocActors[0], 1),
		bubbleRow(healBubble, ocActors[1], 2),
	}
	tb := parse(t, table(bubbleRowSize, nil, rows...))
	if err := addOverchargeBubbles(tb); err != nil {
		t.Fatal(err)
	}
	copied := []int{0, 2, 3}
	if len(tb.Rows) != len(rows)+len(copied) {
		t.Fatalf("%d rows, want the %d stock rows and %d copies", len(tb.Rows), len(rows), len(copied))
	}
	for i, from := range copied {
		want := bytes.Clone(rows[from])
		radpack.SetU64(want, bubbleIDOff, ocBubble)
		if got := tb.Rows[len(rows)+i]; !bytes.Equal(got, want) {
			t.Errorf("copy %d is for actor %016x, want the copy of stock row %d (actor %016x)",
				i, radpack.U64(got, bubbleActorOff), from, radpack.U64(rows[from], bubbleActorOff))
		}
	}
}

// The declared fields read what the two steps write: the buff entry of the list, and the three OVERCHARGE rows.
func TestTheOverchargeFieldsReadWhatTheirStepsWrite(t *testing.T) {
	fields := map[string]Field{}
	for _, st := range steps {
		if st.Name == buffTacmodStep || st.Name == buffBubbleStep {
			if len(st.Writes) != 1 {
				t.Fatalf("%s declares %d fields, want 1", st.Name, len(st.Writes))
			}
			fields[st.Name] = st.Writes[0]
		}
	}
	out, err := appendTacmod(jsonResource(stockTacmods))
	if err != nil {
		t.Fatal(err)
	}
	f := fields[buffTacmodStep]
	got, err := f.Read(out)
	if want := `{"name":"buff","display_name":"OVERCHARGE"}` + "\n"; err != nil || string(got) != want || f.Type != jsonResourceType ||
		!slices.Equal(f.Records, []uint64{equipmentName}) {
		t.Errorf("tacmod field %q, %v (type %016x, records %x)", got, err, f.Type, f.Records)
	}
	tb := parse(t, table(bubbleRowSize, nil, stockBubbles()...))
	if err := addOverchargeBubbles(tb); err != nil {
		t.Fatal(err)
	}
	f = fields[buffBubbleStep]
	rows, err := f.Read(tb.Bytes())
	var want []byte
	for _, r := range tb.Rows[3:] {
		want = append(want, r...)
	}
	if err != nil || !bytes.Equal(rows, want) || f.Type != healBubbleTable || !slices.Equal(f.Records, []uint64{globalLevel}) {
		t.Errorf("bubble field %x, want %x (%v; type %016x, records %x)", rows, want, err, f.Type, f.Records)
	}
}
