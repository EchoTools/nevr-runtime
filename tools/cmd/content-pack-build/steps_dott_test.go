package main

import (
	"bytes"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
	"github.com/EchoTools/nevr-runtime/tools/internal/scriptgate"
)

// The DOT trigger attach over synthetic tables with the shipped layout: no row or script of the game's is used.

const trigOtherActor = 0x7777

func trigScriptRow(script, actor, at uint64) []byte {
	return row(scriptgate.RowStride, map[int]uint64{scriptNameOff: script, keyOff: actor, scriptgate.ScriptOffset: at})
}

// dotOwnRow is the DOT field's own script row, which the trigger row goes after.
func dotOwnRow() []byte { return trigScriptRow(dotFieldScript, dotMagazineActor, dotFieldScript) }

// donorRow is the detonator script's row, which the trigger row copies (its actor is some other one).
func donorRow() []byte { return trigScriptRow(dotDetonatorScript, trigOtherActor, dotDetonatorScript) }

// resourceKey is the radpack key of a resource; the ids above the int64 range are converted at run time.
func resourceKey(typ, name uint64) radpack.Key {
	return radpack.Key{Type: int64(typ), Name: int64(name)}
}

// dotTriggerFixture is a Pack over a synthetic stock set holding the CScriptCR table of each level in tables, plus the
// extra resources, and a game folder with the detonator's stock script. It returns the pack and that script's bytes.
func dotTriggerFixture(t *testing.T, tables map[uint64][]byte, extra map[radpack.Key][]byte) (*Pack, []byte) {
	t.Helper()
	res := map[radpack.Key][]byte{}
	for lvl, b := range tables {
		res[resourceKey(uint64(scriptTable), lvl)] = b
	}
	for k, v := range extra {
		res[k] = v
	}
	data := t.TempDir()
	if err := radpack.WriteStockSet(data, Sets[0], []map[radpack.Key][]byte{res}); err != nil {
		t.Fatal(err)
	}
	s, _, err := radpack.OpenSet(data, Sets[0])
	if err != nil {
		t.Fatal(err)
	}
	gameDir := t.TempDir()
	stock := []byte("synthetic stock detonator script")
	scripts := filepath.Join(gameDir, "bin", "win10", "scripts")
	if err := os.MkdirAll(scripts, os.ModePerm); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(scripts, "190ed14fe3da5ed1.dll"), stock, os.ModePerm); err != nil {
		t.Fatal(err)
	}
	p := &Pack{gameDir: gameDir, sets: map[string]*radpack.Set{Sets[0]: s}, changes: map[string]map[radpack.Key][]byte{}, scripts: map[string][]byte{}}
	return p, stock
}

func dotTriggerStep(t *testing.T) Step {
	t.Helper()
	for _, st := range steps {
		if st.Name == dotTriggerStepName {
			return st
		}
	}
	t.Fatalf("step %q is not registered", dotTriggerStepName)
	return Step{}
}

func TestTheDotTriggerRowIsACopyOfTheDetonatorsRowForTheDotActorAfterTheDotsOwnRow(t *testing.T) {
	shield := trigScriptRow(0x3, shieldActor, 0x3)
	tb := parse(t, table(scriptgate.RowStride, nil, row(scriptgate.RowStride, map[int]uint64{keyOff: trigOtherActor}), donorRow(), dotOwnRow(), shield))
	if err := insertDotTriggerRow(tb); err != nil {
		t.Fatal(err)
	}
	if len(tb.Rows) != 5 {
		t.Fatalf("%d rows, want 5", len(tb.Rows))
	}
	want := donorRow()
	radpack.SetU64(want, scriptNameOff, dotTriggerScript)
	radpack.SetU64(want, keyOff, dotMagazineActor)
	radpack.SetU64(want, scriptgate.ScriptOffset, dotTriggerScript)
	if !bytes.Equal(tb.Rows[3], want) {
		t.Errorf("trigger row %x, want %x", tb.Rows[3], want)
	}
	if !bytes.Equal(tb.Rows[2], dotOwnRow()) || !bytes.Equal(tb.Rows[4], shield) || !bytes.Equal(tb.Rows[1], donorRow()) {
		t.Error("another row changed")
	}
	out := parse(t, tb.Bytes())
	if len(out.Rows) != 5 || out.Find(keyOff, dotMagazineActor) != 2 {
		t.Errorf("written table has %d rows, the DOT rows at %d", len(out.Rows), out.Find(keyOff, dotMagazineActor))
	}
}

func TestADotTriggerTableThatIsNotTheStockOneIsRefusedAndUnchanged(t *testing.T) {
	poolDonor := donorRow()
	radpack.SetU64(poolDonor, 48+keyOff, 1)
	narrow := row(scriptgate.RowStride-8, map[int]uint64{keyOff: dotMagazineActor})
	for name, c := range map[string]struct {
		table []byte
		want  string
	}{
		"no DOT row":                {table(scriptgate.RowStride, nil, donorRow(), row(scriptgate.RowStride, map[int]uint64{keyOff: trigOtherActor})), "want exactly 1"},
		"two DOT rows":              {table(scriptgate.RowStride, nil, dotOwnRow(), donorRow(), dotOwnRow()), "want exactly 1"},
		"no detonator donor":        {table(scriptgate.RowStride, nil, dotOwnRow(), row(scriptgate.RowStride, map[int]uint64{keyOff: trigOtherActor})), "no row of the detonator script"},
		"donor with pool data":      {table(scriptgate.RowStride, nil, poolDonor, dotOwnRow()), "pool data"},
		"trigger row already there": {table(scriptgate.RowStride, nil, dotOwnRow(), trigScriptRow(dotTriggerScript, dotMagazineActor, dotTriggerScript), donorRow()), "already has"},
		"rows of another size":      {table(scriptgate.RowStride-8, nil, narrow, narrow), "want 720"},
	} {
		tb := parse(t, c.table)
		before := tb.Bytes()
		err := insertDotTriggerRow(tb)
		if err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: error %v, want one naming %q", name, err, c.want)
		}
		if !bytes.Equal(tb.Bytes(), before) {
			t.Errorf("%s: the table was changed before it was refused", name)
		}
	}
}

func TestTheDotTriggerStepChangesTheFiveCombatLevelsAndTheEntryAndListsTheCopy(t *testing.T) {
	tables := map[uint64][]byte{}
	for _, lvl := range droneLevels {
		tables[lvl] = table(scriptgate.RowStride, nil, donorRow(), dotOwnRow())
	}
	p, stock := dotTriggerFixture(t, tables, nil)
	if err := dotTriggerStep(t).Apply(p); err != nil {
		t.Fatal(err)
	}
	got := p.changes[Sets[0]]
	if len(got) != len(droneLevels)+1 {
		t.Errorf("%d resources changed, want %d (the five script tables and the entry)", len(got), len(droneLevels)+1)
	}
	for _, lvl := range droneLevels {
		b := got[resourceKey(uint64(scriptTable), lvl)]
		if tb := parse(t, b); len(tb.Rows) != 3 || tb.Find(keyOff, dotMagazineActor) != 1 {
			t.Errorf("level %016x: %d rows after the step", lvl, len(tb.Rows))
		}
	}
	entry := got[resourceKey(uint64(scriptgate.ScriptType), dotTriggerScript)]
	if !bytes.Equal(entry, make([]byte, scriptResourceEntry)) {
		t.Errorf("entry %x, want %d zero bytes", entry, scriptResourceEntry)
	}
	if len(p.rekeys) != 1 || p.rekeys[0].Name != dotTriggerName || p.rekeys[0].StockSource != dotDetonatorName ||
		p.rekeys[0].StockSha256 != digest(stock) || p.rekeys[0].Platform != RekeyPlatform {
		t.Errorf("rekeys %+v, want the trigger copied from the detonator script", p.rekeys)
	}
	if len(p.scripts) != 0 {
		t.Errorf("%d script bytes in the pack; the copy carries none", len(p.scripts))
	}
}

func TestTheDotTriggerStepRefusesAStockThatListsTheEntryOrATableAndWritesNothing(t *testing.T) {
	good := table(scriptgate.RowStride, nil, donorRow(), dotOwnRow())
	bad := table(scriptgate.RowStride, nil, donorRow())

	tables := map[uint64][]byte{}
	for _, lvl := range droneLevels {
		tables[lvl] = good
	}
	withEntry := map[radpack.Key][]byte{resourceKey(uint64(scriptgate.ScriptType), dotTriggerScript): make([]byte, scriptResourceEntry)}
	p, _ := dotTriggerFixture(t, tables, withEntry)
	if err := dotTriggerStep(t).Apply(p); err == nil || !strings.Contains(err.Error(), "already lists") {
		t.Errorf("a stock that lists the entry: error %v", err)
	}
	if len(p.changes[Sets[0]]) != 0 || len(p.rekeys) != 0 {
		t.Errorf("%d changes and %d rekeys written for a refused stock", len(p.changes[Sets[0]]), len(p.rekeys))
	}

	tables[droneLevels[2]] = bad
	p, _ = dotTriggerFixture(t, tables, nil)
	if err := dotTriggerStep(t).Apply(p); err == nil {
		t.Fatal("a level without its DOT row was attached")
	}
	if len(p.changes[Sets[0]]) != 0 || len(p.rekeys) != 0 {
		t.Errorf("%d changes and %d rekeys written before the refusal", len(p.changes[Sets[0]]), len(p.rekeys))
	}
}

func TestTheDotTriggerFieldsReadWhatTheStepWrites(t *testing.T) {
	st := dotTriggerStep(t)
	if len(st.Writes) != 2 {
		t.Fatalf("%d fields declared, want 2", len(st.Writes))
	}
	rows, entry := st.Writes[0], st.Writes[1]
	if rows.Set != Sets[0] || rows.Type != scriptTable || !slices.Equal(rows.Records, droneLevels) {
		t.Errorf("rows field is set %s type %016x records %x", rows.Set, rows.Type, rows.Records)
	}
	if entry.Set != Sets[0] || entry.Type != scriptgate.ScriptType || !slices.Equal(entry.Records, []uint64{dotTriggerScript}) {
		t.Errorf("entry field is set %s type %016x records %x", entry.Set, entry.Type, entry.Records)
	}

	stock := table(scriptgate.RowStride, nil, donorRow(), dotOwnRow())
	if got, err := rows.Read(stock); err != nil || len(got) != 0 {
		t.Errorf("the stock table reads as %x (%v), want no bytes", got, err)
	}
	tb := parse(t, stock)
	if err := insertDotTriggerRow(tb); err != nil {
		t.Fatal(err)
	}
	if got, err := rows.Read(tb.Bytes()); err != nil || !bytes.Equal(got, tb.Rows[2]) {
		t.Errorf("the edited table reads as %x (%v), want the trigger row %x", got, err, tb.Rows[2])
	}
	if got, err := entry.Read(make([]byte, scriptResourceEntry)); err != nil || len(got) != scriptResourceEntry {
		t.Errorf("entry reads as %x (%v)", got, err)
	}
	if _, err := entry.Read([]byte{1}); err == nil {
		t.Error("a one-byte entry was read")
	}
	if _, err := rows.Read(table(8, nil, make([]byte, 8))); err == nil {
		t.Error("rows narrower than the script id were read")
	}
}

// Every pool slot of the detonator row must be empty (fix_dot_trigger.py:163). The slots are written out here, so a change
// to scriptPoolOffs that drops one fails this test.
func TestTheDetonatorRowWithPoolDataInAnyPoolSlotIsRefusedAndUnchanged(t *testing.T) {
	wantSlots := []int{48, 104, 160, 216, 272, 328, 384, 440, 496, 552, 608, 664}
	if !slices.Equal(scriptPoolOffs, wantSlots) {
		t.Fatalf("pool slots %v, want %v (fix_dot_trigger.py:163)", scriptPoolOffs, wantSlots)
	}
	for _, off := range wantSlots {
		donor := donorRow()
		radpack.SetU64(donor, off+keyOff, 1)
		tb := parse(t, table(scriptgate.RowStride, nil, donor, dotOwnRow()))
		before := tb.Bytes()
		err := insertDotTriggerRow(tb)
		if err == nil || !strings.Contains(err.Error(), "pool data") {
			t.Errorf("pool data at +%#x: error %v, want one naming \"pool data\"", off+keyOff, err)
		}
		if !bytes.Equal(tb.Bytes(), before) {
			t.Errorf("pool data at +%#x: the table was changed before it was refused", off+keyOff)
		}
	}
}

// trigDroneSource is the row the drone step copies (script 190ed14fe6c74dd3 at +0). trigDroneActor is the drone actor's row,
// which the drone step puts its copy before (steps_drone.go droneScriptRow).
func trigDroneSource() []byte { return trigScriptRow(sharedScript, trigOtherActor, sharedScript) }
func trigDroneActor() []byte  { return trigScriptRow(0x5, droneActor, 0x5) }

// dotTriggerCopyRow is the copy's row as the step writes it: the detonator's row with its two script ids and its actor set.
func dotTriggerCopyRow() []byte {
	want := donorRow()
	radpack.SetU64(want, scriptNameOff, dotTriggerScript)
	radpack.SetU64(want, keyOff, dotMagazineActor)
	radpack.SetU64(want, scriptgate.ScriptOffset, dotTriggerScript)
	return want
}

// trigApplyInOrder runs the DOT step and the drone script step over one fixture of the five tables, the DOT step first or the
// drone step first, and returns each level's table as the pack holds it after both.
func trigApplyInOrder(t *testing.T, tables map[uint64][]byte, dotFirst bool) map[uint64][]byte {
	t.Helper()
	p, _ := dotTriggerFixture(t, tables, nil)
	dott := dotTriggerStep(t).Apply
	drone := forEachLevel(scriptTable, droneScriptRow)
	order := []func(*Pack) error{drone, dott}
	if dotFirst {
		order = []func(*Pack) error{dott, drone}
	}
	for _, apply := range order {
		if err := apply(p); err != nil {
			t.Fatal(err)
		}
	}
	out := map[uint64][]byte{}
	for _, lvl := range droneLevels {
		out[lvl] = p.changes[Sets[0]][resourceKey(uint64(scriptTable), lvl)]
	}
	return out
}

// The copy's place is directly after the DOT field's own row, in each of the five drone levels (dotTriggerAfter), whichever
// of the two steps runs first, and the bytes are the same either way. The three layouts put the drone actor's row and the
// detonator's row on each side of the DOT row.
func TestTheDotTriggerRowSitsDirectlyAfterTheDotRowInEachDroneLevelWhicheverOrderTheDroneStepRuns(t *testing.T) {
	layouts := []struct {
		name string
		rows [][]byte
	}{
		{"the drone actor's row right after the DOT row", [][]byte{trigDroneSource(), dotOwnRow(), trigDroneActor(), donorRow()}},
		{"the drone actor's row before the DOT row", [][]byte{trigDroneActor(), donorRow(), dotOwnRow(), trigDroneSource()}},
		{"the detonator's row before the DOT row", [][]byte{trigDroneSource(), donorRow(), dotOwnRow(), trigDroneActor()}},
	}
	for _, l := range layouts {
		t.Run(l.name, func(t *testing.T) {
			tables := map[uint64][]byte{}
			for _, lvl := range droneLevels {
				tables[lvl] = table(scriptgate.RowStride, nil, l.rows...)
			}
			dottFirst := trigApplyInOrder(t, tables, true)
			droneFirst := trigApplyInOrder(t, tables, false)
			for _, lvl := range droneLevels {
				if !bytes.Equal(dottFirst[lvl], droneFirst[lvl]) {
					t.Errorf("level %016x: the DOT step before the drone step gives other bytes than after it", lvl)
				}
				rows := parse(t, dottFirst[lvl]).Rows
				own := -1
				copies := 0
				for i, r := range rows {
					if bytes.Equal(r, dotOwnRow()) {
						own = i
					}
					if bytes.Equal(r, dotTriggerCopyRow()) {
						copies++
					}
				}
				if own < 0 || own+1 >= len(rows) || !bytes.Equal(rows[own+1], dotTriggerCopyRow()) {
					t.Errorf("level %016x: the copy is not directly after the DOT row at %d; rows %x", lvl, own, rows)
				}
				if copies != 1 {
					t.Errorf("level %016x: %d copy rows, want 1", lvl, copies)
				}
			}
		})
	}
}

// The attached tables and the copy's entry pass the script gate (G2, scriptgate.Check) only while the label lists the copy,
// for the same platform (PLAN-v7 12.3). The stock names are the DOT field's script and the detonator, which the game has.
func TestTheDotTriggerPassesTheScriptGateOnlyWhileTheLabelListsTheCopy(t *testing.T) {
	tables := map[uint64][]byte{}
	for _, lvl := range droneLevels {
		tables[lvl] = table(scriptgate.RowStride, nil, donorRow(), dotOwnRow())
	}
	p, _ := dotTriggerFixture(t, tables, nil)
	if err := dotTriggerStep(t).Apply(p); err != nil {
		t.Fatal(err)
	}
	recs := []scriptgate.Record{{Platform: RekeyPlatform, Type: scriptgate.ScriptType, Name: dotTriggerScript,
		Data: p.changes[Sets[0]][resourceKey(uint64(scriptgate.ScriptType), dotTriggerScript)]}}
	for _, lvl := range droneLevels {
		recs = append(recs, scriptgate.Record{Platform: RekeyPlatform, Type: scriptgate.TableType, Name: lvl,
			Data: p.changes[Sets[0]][resourceKey(uint64(scriptTable), lvl)]})
	}
	const label = "DOT FIELD"
	stock := scriptgate.Gate{Stock: scriptgate.Stock{dotDetonatorScript: true, dotFieldScript: true}}

	res, err := stock.WithListed(label, []scriptgate.Listed{{Platform: RekeyPlatform, Name: dotTriggerScript}}).Check(label, recs)
	if err != nil || len(res.Undelivered) != 0 {
		t.Errorf("listed: undelivered %+v, %v; want none", res.Undelivered, err)
	}
	// Unlisted, or listed for another platform: the copy is named by the entry and by each of the five rows, six in all.
	for _, c := range []struct {
		name string
		g    scriptgate.Gate
	}{
		{"unlisted", stock},
		{"listed for another platform", stock.WithListed(label, []scriptgate.Listed{{Platform: "android-arm64", Name: dotTriggerScript}})},
	} {
		res, err := c.g.Check(label, recs)
		if err != nil || len(res.Undelivered) != len(droneLevels)+1 {
			t.Errorf("%s: undelivered %+v, %v; want the copy named by the entry and by each of the five rows", c.name, res.Undelivered, err)
		}
		for _, u := range res.Undelivered {
			if u.Script != dotTriggerScript {
				t.Errorf("%s: script %016x undelivered, want only the copy %016x", c.name, u.Script, uint64(dotTriggerScript))
			}
		}
	}
}
