package main

import (
	"bytes"
	"os"
	"path/filepath"
	"slices"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
	"github.com/EchoTools/nevr-runtime/tools/internal/scriptgate"
)

// The UMBRAL VEIL attach over a synthetic global level table (table, row and parse from steps_drone_test.go). Its stock
// barrier script is a made-up file in a temporary game folder; no byte of the game's is used.
var veilStockBytes = []byte("synthetic stock barrier script")

const (
	veilRowMark      = 7      // a value at +0 of the fixture rows, kept as it is by the attach
	veilOtherActor   = 0x1234 // an actor that is not a player's
	veilOtherScript  = 0x5555 // a script that is neither the barrier, the copy nor the point script
	veilDirMode      = 0o755  // the fixture's game folder
	veilFileMode     = 0o644  // the fixture's stock barrier script
	veilWrongSize    = scriptgate.RowStride + 8
	veilCopyOfA      = 1   // the row index of player A's copy in the table of TestTheVeilFieldReadsWhatTheStepWrites
	veilCopyOfB      = 4   // and of player B's
	veilPoolLastSlot = 672 // the twelfth pool slot, 56 + 11*56 = 0x2a0, written out so a change of the stride shows here
)

func veilRow(actor, script uint64) []byte {
	return row(scriptgate.RowStride, map[int]uint64{0: veilRowMark, veilActorOff: actor, scriptgate.ScriptOffset: script})
}

// veilPack is a pack whose global level CScriptCR table is in = the stock table, and whose game folder holds the stock
// barrier script (or none, when withStock is false).
func veilAttachPack(t *testing.T, in []byte, withStock bool) *Pack {
	t.Helper()
	dir := t.TempDir()
	scripts := filepath.Join(dir, "bin", "win10", "scripts")
	if err := os.MkdirAll(scripts, veilDirMode); err != nil {
		t.Fatal(err)
	}
	if withStock {
		if err := os.WriteFile(filepath.Join(scripts, veilBarrierName+".dll"), veilStockBytes, veilFileMode); err != nil {
			t.Fatal(err)
		}
	}
	var typ uint64 = scriptTable
	key := radpack.Key{Type: int64(typ), Name: int64(globalLevel)}
	// the set is named so Put accepts it; Read and Put use the staged changes, so no radpack.Set is read
	return &Pack{gameDir: dir, sets: map[string]*radpack.Set{Sets[0]: nil},
		changes: map[string]map[radpack.Key][]byte{Sets[0]: {key: in}}, scripts: map[string][]byte{}}
}

func veilTable(rows ...[]byte) []byte {
	return table(scriptgate.RowStride, []byte("pool bytes"), rows...)
}

func veilChanged(t *testing.T, p *Pack) []byte {
	t.Helper()
	var typ uint64 = scriptTable
	return p.changes[Sets[0]][radpack.Key{Type: int64(typ), Name: int64(globalLevel)}]
}

// veilWithCopy is the row a barrier row becomes after the attach: its +0 and +32 are the copy's name.
func veilWithCopy(r []byte) []byte {
	n := append([]byte(nil), r...)
	radpack.SetU64(n, 0, veilCopy)
	radpack.SetU64(n, scriptgate.ScriptOffset, veilCopy)
	return n
}

func TestTheVeilCopyIsListedAndEachPlayerBarrierRowGetsItsCopyAfterIt(t *testing.T) {
	a, b := veilRow(veilPlayerActors[0], veilBarrier), veilRow(veilPlayerActors[1], veilBarrier)
	other := veilRow(veilOtherActor, veilBarrier)               // a barrier row of an actor that is not a player's: no copy
	notBarrier := veilRow(veilPlayerActors[1], veilOtherScript) // a player's row that runs another script: no copy
	p := veilAttachPack(t, veilTable(a, other, b, notBarrier), true)
	if err := veilAttach(p); err != nil {
		t.Fatal(err)
	}
	got := veilChanged(t, p)
	want := [][]byte{a, veilWithCopy(a), other, b, veilWithCopy(b), notBarrier}
	rows := parse(t, got).Rows
	if len(rows) != len(want) {
		t.Fatalf("%d rows, want %d", len(rows), len(want))
	}
	for i := range want {
		if !bytes.Equal(rows[i], want[i]) {
			t.Errorf("row %d\n got %x\nwant %x", i, rows[i], want[i])
		}
	}
	if !bytes.HasSuffix(got, []byte("pool bytes")) {
		t.Error("the tail is not kept as it is")
	}
	if len(p.rekeys) != 1 || p.rekeys[0].Name != veilCopyName || p.rekeys[0].StockSource != veilBarrierName ||
		p.rekeys[0].StockSha256 != digest(veilStockBytes) {
		t.Errorf("rekeys %+v, want the copy %s of %s with the stock's sha-256", p.rekeys, veilCopyName, veilBarrierName)
	}
}

// The gate (G2, scriptgate.Check) passes the attached table only when the label lists the copy: the rows name the copy, and
// the list is what delivers it. The stock barrier is named as stock, so the only name the gate can refuse is the copy.
func TestTheAttachPassesTheScriptGateOnlyWhileTheLabelListsTheCopy(t *testing.T) {
	p := veilAttachPack(t, veilTable(veilRow(veilPlayerActors[0], veilBarrier)), true)
	if err := veilAttach(p); err != nil {
		t.Fatal(err)
	}
	recs := []scriptgate.Record{{Platform: RekeyPlatform, Type: scriptgate.TableType, Name: uint64(globalLevel), Data: veilChanged(t, p)}}
	stock := scriptgate.Gate{Stock: scriptgate.Stock{veilBarrier: true}}
	const label = "UMBRAL VEIL"

	res, err := stock.WithListed(label, []scriptgate.Listed{{Platform: RekeyPlatform, Name: veilCopy}}).Check(label, recs)
	if err != nil || len(res.Undelivered) != 0 {
		t.Errorf("listed: undelivered %+v, %v; want none", res.Undelivered, err)
	}
	res, err = stock.Check(label, recs)
	if err != nil || len(res.Undelivered) != 1 || res.Undelivered[0].Script != veilCopy || !res.Undelivered[0].Row {
		t.Errorf("unlisted: undelivered %+v, %v; want the copy named by its row", res.Undelivered, err)
	}
	res, err = stock.WithListed(label, []scriptgate.Listed{{Platform: "android-arm64", Name: veilCopy}}).Check(label, recs)
	if err != nil || len(res.Undelivered) != 1 {
		t.Errorf("listed for another platform: undelivered %+v, %v; want the copy", res.Undelivered, err)
	}
}

// A table that does not fit the rule is refused, naming the rule, and the pack changes nothing and lists no copy.
func TestAnAttachThatDoesNotFitTheStockTableIsRefusedAndWritesNothing(t *testing.T) {
	big := func(actor uint64) []byte {
		return row(veilWrongSize, map[int]uint64{veilActorOff: actor, scriptgate.ScriptOffset: veilBarrier})
	}
	cases := []struct {
		name, want string
		in         []byte
		stock      bool
	}{
		{"rows of another size", "rows of 728 bytes, expected 720",
			table(veilWrongSize, nil, big(veilPlayerActors[0])), true},
		{"no barrier row of a player actor", "expected at least one, found 0",
			veilTable(veilRow(veilOtherActor, veilBarrier), veilRow(veilPlayerActors[0], veilOtherScript)), true},
		{"a row already runs the copy", "already runs the veil copy",
			veilTable(veilRow(veilPlayerActors[0], veilBarrier), veilRow(veilPlayerActors[1], veilCopy)), true},
		{"a barrier row carries pool data", "carries pool data at +0x38",
			veilTable(withU64(veilRow(veilPlayerActors[0], veilBarrier), veilPoolFirst, 1)), true},
		{"a barrier row carries pool data in the last slot", "carries pool data at +0x2a0",
			veilTable(withU64(veilRow(veilPlayerActors[0], veilBarrier), veilPoolLastSlot, 1)), true},
		{"the stock barrier script is missing", "the stock script",
			veilTable(veilRow(veilPlayerActors[0], veilBarrier)), false},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			p := veilAttachPack(t, c.in, c.stock)
			err := veilAttach(p)
			if err == nil {
				t.Fatal("refused: no error")
			}
			if !bytes.Contains([]byte(err.Error()), []byte(c.want)) {
				t.Errorf("error %q does not name %q", err, c.want)
			}
			if !bytes.Equal(veilChanged(t, p), c.in) || len(p.rekeys) != 0 {
				t.Errorf("a refused step changed the table or listed a copy (rekeys %d)", len(p.rekeys))
			}
		})
	}
}

func withU64(r []byte, off int, v uint64) []byte {
	radpack.SetU64(r, off, v)
	return r
}

// The declared field reads the rows that run the copy, which are the rows the step writes.
func TestTheVeilFieldReadsWhatTheStepWrites(t *testing.T) {
	in := veilTable(veilRow(veilPlayerActors[0], veilBarrier), veilRow(veilOtherActor, veilBarrier), veilRow(veilPlayerActors[1], veilBarrier))
	out, err := attachVeil(in)
	if err != nil {
		t.Fatal(err)
	}
	var f Field
	for _, st := range steps {
		if st.Name == veilAttachStep {
			f = st.Writes[0]
		}
	}
	got, err := f.Read(out)
	if err != nil {
		t.Fatal(err)
	}
	// The output's rows: barrier A, its copy, the other row, barrier B, its copy.
	rows := parse(t, out).Rows
	want := append(append([]byte(nil), rows[veilCopyOfA]...), rows[veilCopyOfB]...)
	if !bytes.Equal(got, want) {
		t.Errorf("field %x, want the two copies %x", got, want)
	}
}

// The final order of a player actor's rows, with both attach steps applied: barrier, point, veil, whichever step runs first.
// Each step alone leaves a valid order (a prefix-ordered subsequence of attachOrder). The synthetic stock shape is the
// point test's (steps_point_attach_test.go).
func TestThePointAndVeilAttachesLeaveEachActorsRowsInAttachOrderInEitherApplicationOrder(t *testing.T) {
	point := func(b []byte) []byte {
		tb := parse(t, b)
		if err := attachPointRows(tb); err != nil {
			t.Fatal(err)
		}
		return tb.Bytes()
	}
	veil := func(b []byte) []byte {
		out, err := attachVeil(b)
		if err != nil {
			t.Fatal(err)
		}
		return out
	}
	stock := stockGlobalCRShape()
	pointThenVeil, veilThenPoint := veil(point(stock)), point(veil(stock))
	if !bytes.Equal(pointThenVeil, veilThenPoint) {
		t.Errorf("the table depends on the order the steps ran in\n point, veil: %x\n veil, point: %x", pointThenVeil, veilThenPoint)
	}
	cases := []struct {
		name string
		got  []byte
		want []uint64
	}{
		{"point then veil", pointThenVeil, attachOrder},
		{"veil then point", veilThenPoint, attachOrder},
		{"point alone", point(stock), []uint64{barrierAttachScript, pointAttachScript}},
		{"veil alone", veil(stock), []uint64{barrierAttachScript, veilAttachScript}},
	}
	for _, c := range cases {
		for _, actor := range playerActorIDs {
			if got := actorScripts(t, c.got, actor); !slices.Equal(got, c.want) {
				t.Errorf("%s: actor %016x rows %x, want %x", c.name, actor, got, c.want)
			}
		}
	}
}
