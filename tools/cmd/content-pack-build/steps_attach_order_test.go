package main

import (
	"slices"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
	"github.com/EchoTools/nevr-runtime/tools/internal/scriptgate"
)

// The pinned attach order: the scripts of he_is_the_cat's build, in the order the game is known to work with.
const (
	wantBarrierScript uint64 = 0xb87ba2039b049d5e // fix_smoke.py:73 BARRIER_SCRIPT
	wantPointScript   uint64 = 0x94ba7ca39fc97e1a // fix_point.py POINT_SCRIPT
	wantVeilScript    uint64 = 0x94ba7ca09dcf7b0b // fix_smoke.py:74 SMOKE_SCRIPT
)

// The order of a player actor's CScriptCR rows is stated in code (attachOrder), not left to the order the attach steps'
// files initialise in. Rows here are synthetic (the helpers of steps_point_attach_test.go).

// actorScripts is the scripts of the table's rows of the actor, in table order.
func actorScripts(t *testing.T, b []byte, actor uint64) []uint64 {
	t.Helper()
	var s []uint64
	for _, r := range parse(t, b).Rows {
		if radpack.U64(r, keyOff) == actor {
			s = append(s, radpack.U64(r, scriptgate.ScriptOffset))
		}
	}
	return s
}

// veilAttachedCopy is the veil's row: the barrier row's copy running the veil script.
func veilAttachedCopy(barrier []byte) []byte {
	c := append([]byte(nil), barrier...)
	radpack.SetU64(c, 0, veilAttachScript)
	radpack.SetU64(c, scriptgate.ScriptOffset, veilAttachScript)
	return c
}

func TestTheAttachOrderIsBarrierPointVeil(t *testing.T) {
	want := []uint64{wantBarrierScript, wantPointScript, wantVeilScript}
	if !slices.Equal(attachOrder, want) {
		t.Errorf("attachOrder %x, want barrier, point, veil %x", attachOrder, want)
	}
}

// After the point step, each player actor's rows run in attachOrder: the point row right after the barrier row when the
// actor has no veil row, and in front of the veil row when the veil's attach has already run.
func TestEachPlayerActorsRowsAreInAttachOrderAfterThePointStep(t *testing.T) {
	stock := stockGlobalCRShape()
	withVeil := stockGlobalCR(otherAttachRow(),
		barrierAttachRow(playerActorIDs[0]), veilAttachedCopy(barrierAttachRow(playerActorIDs[0])),
		barrierAttachRow(playerActorIDs[1]), veilAttachedCopy(barrierAttachRow(playerActorIDs[1])))
	cases := []struct {
		name string
		in   []byte
		want []uint64
	}{
		{"no veil row", stock, []uint64{barrierAttachScript, pointAttachScript}},
		{"a veil row already there", withVeil, []uint64{barrierAttachScript, pointAttachScript, veilAttachScript}},
	}
	for _, c := range cases {
		tb := parse(t, c.in)
		if err := attachPointRows(tb); err != nil {
			t.Fatalf("%s: %v", c.name, err)
		}
		for _, actor := range playerActorIDs {
			if got := actorScripts(t, tb.Bytes(), actor); !slices.Equal(got, c.want) {
				t.Errorf("%s: actor %016x rows %x, want %x", c.name, actor, got, c.want)
			}
		}
	}
}

func TestInsertAttachRowRefusesWhatTheOrderDoesNotPlace(t *testing.T) {
	rows := parse(t, stockGlobalCRShape()).Rows
	if _, _, err := insertAttachRow(rows, playerActorIDs[0], testOtherScript); err == nil {
		t.Error("a script that is not in attachOrder was placed")
	}
	out, added, err := insertAttachRow(rows, playerActorIDs[0], pointAttachScript)
	if err != nil || !added {
		t.Fatalf("first insert: added %v, %v", added, err)
	}
	if _, _, err := insertAttachRow(out, playerActorIDs[0], pointAttachScript); err == nil {
		t.Error("a second row of the same script was placed")
	}
	if len(rows) != stockRowCount {
		t.Errorf("the input rows were changed: %d rows", len(rows))
	}
	if out, added, err := insertAttachRow(rows, testOtherActor, pointAttachScript); err != nil || added || len(out) != len(rows) {
		t.Errorf("an actor with no barrier row: added %v, %d rows, %v", added, len(out), err)
	}
}

// The rows land in attachOrder whichever of the point and the veil row is put first: the same rows either way, and the
// helper alone (not the file order of the steps) decides it.
func TestInsertAttachRowPlacesPointAndVeilInTheSameOrderWhicheverIsFirst(t *testing.T) {
	stock := parse(t, stockGlobalCRShape()).Rows
	place := func(scripts ...uint64) [][]byte {
		rows := stock
		for _, s := range scripts {
			for _, actor := range playerActorIDs {
				var err error
				if rows, _, err = insertAttachRow(rows, actor, s); err != nil {
					t.Fatal(err)
				}
			}
		}
		return rows
	}
	a, b := place(pointAttachScript, veilAttachScript), place(veilAttachScript, pointAttachScript)
	if len(a) != len(b) || len(a) != rowsAfterPointAndVeilAttach {
		t.Fatalf("%d and %d rows, want %d (the stock three and a point and a veil row per player actor)", len(a), len(b), rowsAfterPointAndVeilAttach)
	}
	for i := range a {
		if !slices.Equal(a[i], b[i]) {
			t.Errorf("row %d differs by the order the rows were put in", i)
		}
	}
	for _, actor := range playerActorIDs {
		var got []uint64
		for _, r := range a {
			if radpack.U64(r, keyOff) == actor {
				got = append(got, radpack.U64(r, scriptgate.ScriptOffset))
			}
		}
		if !slices.Equal(got, attachOrder) {
			t.Errorf("actor %016x rows %x, want attachOrder %x", actor, got, attachOrder)
		}
	}
}

func TestInsertAttachRowRefusesAnActorWithTwoBarrierRows(t *testing.T) {
	rows := parse(t, stockGlobalCR(barrierAttachRow(playerActorIDs[0]), barrierAttachRow(playerActorIDs[0]))).Rows
	if _, _, err := insertAttachRow(rows, playerActorIDs[0], pointAttachScript); err == nil {
		t.Error("an actor with two barrier rows was given a row")
	}
}
