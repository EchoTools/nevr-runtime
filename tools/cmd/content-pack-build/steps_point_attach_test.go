package main

// The NEUTRON script attach over synthetic CScriptCR rows of the shipped 720-byte layout; no row of the game's is used.

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
	"github.com/EchoTools/nevr-runtime/tools/internal/scriptgate"
)

const (
	testOtherScript   = 0x1234 // the row's +0 (fix_point.py leaves it alone)
	testOtherG2Script = 0x9abc // the row's script at +32, the one G2 reads (scriptgate.ScriptOffset)
	testOtherActor    = 0x5678

	stockRowCount               = 3   // the stock shape: the other row, then a barrier row per player actor
	rowsAfterPointAttach        = 5   // the stock shape, and one point row per player actor
	rowsAfterPointAndVeilAttach = 7   // the stock shape, and a point and a veil row per player actor
	otherRowSize                = 128 // a row size that is not the shipped scriptgate.RowStride
	testDirMode                 = 0o755
	testFileMode                = 0o644

	// Indexes into the table after the point attach of stockGlobalCRShape (the stock shape with a point row per player actor).
	rowOther         = 0
	rowFirstBarrier  = 1
	rowFirstPoint    = 2
	rowSecondBarrier = 3
	rowSecondPoint   = 4
)

func barrierAttachRow(actor uint64) []byte {
	return row(scriptgate.RowStride, map[int]uint64{0: barrierAttachScript, keyOff: actor, scriptgate.ScriptOffset: barrierAttachScript})
}

func otherAttachRow() []byte {
	return row(scriptgate.RowStride, map[int]uint64{0: testOtherScript, keyOff: testOtherActor, scriptgate.ScriptOffset: testOtherG2Script})
}

func stockGlobalCR(rows ...[]byte) []byte {
	return table(scriptgate.RowStride, nil, rows...)
}

// The stock shape: another row, then each player actor's barrier row.
func stockGlobalCRShape() []byte {
	return stockGlobalCR(otherAttachRow(), barrierAttachRow(playerActorIDs[0]), barrierAttachRow(playerActorIDs[1]))
}

// attachedCopy is the point script's row fix_point.py writes after a barrier row: the same row with the point script at +0
// and +32.
func attachedCopy(barrier []byte) []byte {
	c := append([]byte(nil), barrier...)
	radpack.SetU64(c, 0, pointAttachScript)
	radpack.SetU64(c, scriptgate.ScriptOffset, pointAttachScript)
	return c
}

func TestThePointRowFollowsEachPlayerActorsBarrierRow(t *testing.T) {
	tb := parse(t, stockGlobalCRShape())
	if err := attachPointRows(tb); err != nil {
		t.Fatal(err)
	}
	out := parse(t, tb.Bytes())
	if len(out.Rows) != rowsAfterPointAttach {
		t.Fatalf("%d rows, want %d (the stock three and one point row per player actor)", len(out.Rows), rowsAfterPointAttach)
	}
	if !bytes.Equal(out.Rows[rowOther], otherAttachRow()) || !bytes.Equal(out.Rows[rowFirstBarrier], barrierAttachRow(playerActorIDs[0])) ||
		!bytes.Equal(out.Rows[rowSecondBarrier], barrierAttachRow(playerActorIDs[1])) {
		t.Error("a stock row changed or moved")
	}
	if want := attachedCopy(barrierAttachRow(playerActorIDs[0])); !bytes.Equal(out.Rows[rowFirstPoint], want) {
		t.Errorf("the first player actor's point row\n got %x\nwant %x", out.Rows[rowFirstPoint], want)
	}
	if want := attachedCopy(barrierAttachRow(playerActorIDs[1])); !bytes.Equal(out.Rows[rowSecondPoint], want) {
		t.Errorf("the second player actor's point row\n got %x\nwant %x", out.Rows[rowSecondPoint], want)
	}
	if got := radpack.U64(out.Rows[rowFirstPoint], keyOff); got != playerActorIDs[0] {
		t.Errorf("point row actor %016x, want %016x", got, playerActorIDs[0])
	}
}

// The G2 check reads the script of each row at ScriptOffset (scriptgate.RowScripts): the point script is named there.
func TestTheAttachedRowsNameThePointScriptToG2(t *testing.T) {
	tb := parse(t, stockGlobalCRShape())
	if err := attachPointRows(tb); err != nil {
		t.Fatal(err)
	}
	scripts, err := scriptgate.RowScripts(tb.Bytes())
	if err != nil {
		t.Fatal(err)
	}
	want := []uint64{testOtherG2Script, barrierAttachScript, pointAttachScript, barrierAttachScript, pointAttachScript}
	if len(scripts) != len(want) {
		t.Fatalf("scripts %x, want %x", scripts, want)
	}
	for i := range want {
		if scripts[i] != want[i] {
			t.Errorf("row %d names script %016x, want %016x", i, scripts[i], want[i])
		}
	}
}

func TestATableThatIsNotTheExpectedStockIsRefusedAndAnAttachedOneIsNotAttachedAgain(t *testing.T) {
	attached := parse(t, stockGlobalCRShape())
	if err := attachPointRows(attached); err != nil {
		t.Fatal(err)
	}
	cases := map[string][]byte{
		"rows of another size": table(otherRowSize, nil, row(otherRowSize, map[int]uint64{keyOff: playerActorIDs[0]})),
		"stock barrier rows of another size": table(otherRowSize, nil,
			row(otherRowSize, map[int]uint64{0: barrierAttachScript, keyOff: playerActorIDs[0], scriptgate.ScriptOffset: barrierAttachScript}),
			row(otherRowSize, map[int]uint64{0: barrierAttachScript, keyOff: playerActorIDs[1], scriptgate.ScriptOffset: barrierAttachScript})),
		"no barrier row for an actor":     stockGlobalCR(otherAttachRow(), barrierAttachRow(playerActorIDs[0])),
		"two barrier rows for an actor":   stockGlobalCR(barrierAttachRow(playerActorIDs[0]), barrierAttachRow(playerActorIDs[0]), barrierAttachRow(playerActorIDs[1])),
		"already attached (a second run)": attached.Bytes(),
		"a point row of another actor":    stockGlobalCR(append(parse(t, stockGlobalCRShape()).Rows, attachedCopy(barrierAttachRow(testOtherActor)))...),
	}
	for name, b := range cases {
		if err := attachPointRows(parse(t, b)); err == nil {
			t.Errorf("%s: changed", name)
		}
	}
	if err := attachPointRows(parse(t, attached.Bytes())); err == nil || !strings.Contains(err.Error(), "already") {
		t.Errorf("a second run was not refused as already attached: %v", err)
	}
}

// attachStep is the registered attach step, by name.
func attachStep(t *testing.T) Step {
	t.Helper()
	for _, st := range steps {
		if st.Name == pointAttachStep {
			return st
		}
	}
	t.Fatalf("step %q is not registered", pointAttachStep)
	return Step{}
}

// stockPack is a pack over a stock global CScriptCR table holding table, with the barrier script on disk for Rekey.
func stockPack(t *testing.T, rows []byte) *Pack {
	t.Helper()
	var table, level uint64 = scriptTable, globalLevel // not constants: the ids overflow a constant int64 conversion
	data := t.TempDir()
	res := map[radpack.Key][]byte{{Type: int64(table), Name: int64(level)}: rows}
	if err := radpack.WriteStockSet(data, Sets[0], []map[radpack.Key][]byte{res}); err != nil {
		t.Fatal(err)
	}
	s, _, err := radpack.OpenSet(data, Sets[0])
	if err != nil {
		t.Fatal(err)
	}
	game := t.TempDir()
	scripts := filepath.Join(game, "bin", "win10", "scripts")
	if err := os.MkdirAll(scripts, testDirMode); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(scripts, fmt.Sprintf("%016x.dll", uint64(barrierAttachScript))), []byte("MZ stock barrier"), testFileMode); err != nil {
		t.Fatal(err)
	}
	return &Pack{gameDir: game, sets: map[string]*radpack.Set{Sets[0]: s}, changes: map[string]map[radpack.Key][]byte{}, scripts: map[string][]byte{}}
}

// The step writes the global level's table and names the copy, so the label lists it for G2.
func TestTheAttachStepWritesTheGlobalLevelAndNamesTheCopy(t *testing.T) {
	p := stockPack(t, stockGlobalCRShape())
	if err := attachStep(t).Apply(p); err != nil {
		t.Fatal(err)
	}
	var table, level uint64 = scriptTable, globalLevel // not constants: the ids overflow a constant int64 conversion
	got, ok := p.changes[Sets[0]][radpack.Key{Type: int64(table), Name: int64(level)}]
	if !ok {
		t.Fatal("the global level's CScriptCR table was not written")
	}
	if out := parse(t, got); len(out.Rows) != rowsAfterPointAttach {
		t.Errorf("%d rows written, want %d", len(out.Rows), rowsAfterPointAttach)
	}
	wantName, wantSource := fmt.Sprintf("%016x", uint64(pointAttachScript)), fmt.Sprintf("%016x", uint64(barrierAttachScript))
	if len(p.rekeys) != 1 || p.rekeys[0].Name != wantName || p.rekeys[0].StockSource != wantSource {
		t.Errorf("rekeys %+v, want the point copy of the barrier script", p.rekeys)
	}
}

// A table that is not the stock one writes nothing and names no copy.
func TestTheAttachStepRefusedWritesNothing(t *testing.T) {
	p := stockPack(t, stockGlobalCR(otherAttachRow()))
	if err := attachStep(t).Apply(p); err == nil {
		t.Fatal("a table with no barrier rows was attached")
	}
	if len(p.changes[Sets[0]]) != 0 || len(p.rekeys) != 0 {
		t.Errorf("a refused attach left changes %d and rekeys %+v", len(p.changes[Sets[0]]), p.rekeys)
	}
}

func TestThePointAttachFieldReadsWhatItWrites(t *testing.T) {
	st := attachStep(t)
	if len(st.Writes) != 1 {
		t.Fatalf("%d fields declared, want 1", len(st.Writes))
	}
	f := st.Writes[0]
	if f.Type != scriptTable || len(f.Records) != 1 || f.Records[0] != globalLevel {
		t.Errorf("field %+v", f)
	}
	stock := stockGlobalCRShape()
	if got, err := f.Read(stock); err != nil || len(got) != 0 {
		t.Errorf("the stock table's point rows read %x, %v", got, err)
	}
	tb := parse(t, stock)
	if err := attachPointRows(tb); err != nil {
		t.Fatal(err)
	}
	if len(tb.Rows) != rowsAfterPointAttach {
		t.Fatalf("%d rows after the attach, want %d", len(tb.Rows), rowsAfterPointAttach)
	}
	got, err := f.Read(tb.Bytes())
	want := append(append([]byte(nil), tb.Rows[rowFirstPoint]...), tb.Rows[rowSecondPoint]...)
	if err != nil || !bytes.Equal(got, want) {
		t.Errorf("point rows %x, want %x (%v)", got, want, err)
	}
}

// Only a player actor's barrier row gets a point row: a barrier row of another actor is left alone.
func TestOnlyThePlayerActorsBarrierRowsGetAPointRow(t *testing.T) {
	tb := parse(t, stockGlobalCR(barrierAttachRow(testOtherActor), barrierAttachRow(playerActorIDs[0]), barrierAttachRow(playerActorIDs[1])))
	if err := attachPointRows(tb); err != nil {
		t.Fatal(err)
	}
	out := parse(t, tb.Bytes())
	if len(out.Rows) != rowsAfterPointAttach || !bytes.Equal(out.Rows[rowOther], barrierAttachRow(testOtherActor)) {
		t.Fatalf("%d rows, or the other actor's barrier row got a copy", len(out.Rows))
	}
	if got := radpack.U64(out.Rows[rowFirstBarrier], keyOff); got != playerActorIDs[0] {
		t.Errorf("row 1 actor %016x, want the first player actor's barrier row", got)
	}
}

// The point attach runs after the barrier wall step by its declaration (After), not by file order alone: with the tie
// between unrelated steps reversed (highest registration index first), the point attach would be ready at once and run
// first without the declaration.
func TestThePointAttachRunsAfterTheBarrierWallUnderAReversedTie(t *testing.T) {
	list, err := orderedWith(steps, func(a, b int) bool { return a > b })
	if err != nil {
		t.Fatal(err)
	}
	pos := map[string]int{}
	for i, st := range list {
		pos[st.Name] = i
	}
	if pos[pointAttachStep] <= pos[pointExplosionStep] {
		t.Errorf("point attach runs at %d, not after the barrier wall step at %d", pos[pointAttachStep], pos[pointExplosionStep])
	}
}
