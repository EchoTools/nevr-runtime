package main

// OVERCHARGE's script copy attach over synthetic CScriptCR rows of the shipped 720-byte layout (steps_buff_attach.go). No row
// of the game's is used: the fixtures are the vectors of S-ATTACH section 5.2, hand-derived and checked by an independent
// script.

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
	"github.com/EchoTools/nevr-runtime/tools/internal/scriptgate"
)

const (
	buffTestTail    = "pool" // the table's tail, kept byte for byte (S-ATTACH 5)
	buffRowsIn      = 5      // the vector's input: heal P1, barrier P1, heal P2, heal B113, other
	buffRowsOut     = 8      // the vector's output: each heal row, then its copy, the barrier and other rows kept
	buffTableHeader = 0x38
	buffTailLen     = 4
	buffLenIn       = buffTableHeader + buffRowsIn*scriptgate.RowStride + buffTailLen
	buffLenOut      = buffTableHeader + buffRowsOut*scriptgate.RowStride + buffTailLen
	// The sha-256 of the vector's output (S-ATTACH 5.2, BUFF row), computed by the independent script and not by this code.
	buffVectorSha = "01c261aa79277ca7cc4a130890705dffabd1e6521ee094a038db14839fd917df"

	buffTagOff     = 0x10 // the u16 the loader overwrites with the actor's index (S-ATTACH 3.1); the fixtures carry a tag there
	buffTagP1      = 0xaffff
	buffTagP2      = 0xffffffff
	buffTagB113    = 0xfffff
	buffOtherValue = 0x1111 // the other row's script at +0 and +32, the row the spec's vector keeps after the heal rows
	buffFlags      = 1 << 32
	buffGrow       = 0x20
	buffFlagsOff   = mtScriptTablesAt + 24 // descriptor 0's flags (S-ATTACH 0.2: 1<<32 at +24)
	buffGrowOff    = mtScriptTablesAt + 32 // descriptor 0's grow (0x20 at +32)

	buffDesc3       = mtScriptTablesAt + mtDescLen*3 // the fourth nested descriptor of a row
	buffDesc3Size   = buffDesc3 + mtDescSizeAt
	buffDesc3Cap    = buffDesc3 + mtDescCapAt
	buffDesc3Count  = buffDesc3 + mtDescCountAt
	buffPoolSize    = 48
	buffPoolCap     = 1
	buffPoolCount   = 2
	buffOtherRowLen = 136 // a row size that is not scriptgate.RowStride

	// The order test's table: the other row, a heal row and a barrier row per player actor, a heal row for B113 and a second
	// player actor's heal row (see buffOrderRows). With the point and veil attaches, 6 rows grow to 13.
	buffRowsAfterAll = 13
	buffCopies       = 3 // the copies the buff attach adds: one per OVERCHARGE actor
	buffRekeyContent = "MZ stock heal"
	buffSpecHeadRows = 2 // the first two rows of the spec's vector: heal P1 and the barrier P1 (no P2 or B113 heal row)
	buffWrites       = 2 // the step's declared fields: the copy rows and the entry
)

// buffHealRow is a stock heal row of actor, with its tag at buffTagOff.
func buffHealRow(actor, tag uint64) []byte {
	return row(scriptgate.RowStride, map[int]uint64{0: healAttachScript, keyOff: actor, buffTagOff: tag, scriptgate.ScriptOffset: healAttachScript})
}

// buffBarrierP1 is the barrier row of P1 with descriptor 0's stock flags and grow (S-ATTACH 5.1, R1).
func buffBarrierP1() []byte {
	return row(scriptgate.RowStride, map[int]uint64{0: barrierAttachScript, keyOff: playerActorIDs[0], buffTagOff: buffTagP1,
		scriptgate.ScriptOffset: barrierAttachScript, buffFlagsOff: buffFlags, buffGrowOff: buffGrow})
}

// buffOtherRow is the row of another script and actor that the buff attach must leave alone (S-ATTACH 5.2, R0).
func buffOtherRow() []byte {
	return row(scriptgate.RowStride, map[int]uint64{0: buffOtherValue, keyOff: playerActorIDs[0], scriptgate.ScriptOffset: buffOtherValue})
}

// buffSpecRows is the vector's rows after the heal rows of P1 and B113 are given: heal P1 (h1), barrier P1, heal P2, the B113
// heal row (h3), other.
func buffSpecRows(h1, h3 []byte) [][]byte {
	return [][]byte{h1, buffBarrierP1(), buffHealRow(playerActorIDs[1], buffTagP2), h3, buffOtherRow()}
}

// buffSpecTable is the vector's input table (S-ATTACH 5.2).
func buffSpecTable() []byte {
	h1 := buffHealRow(playerActorIDs[0], buffTagP1)
	h3 := buffHealRow(ocActors[1], buffTagB113)
	return table(scriptgate.RowStride, []byte(buffTestTail), buffSpecRows(h1, h3)...)
}

// withFields is a copy of r with the u64s of fields set.
func withFields(r []byte, fields map[int]uint64) []byte {
	c := append([]byte(nil), r...)
	for off, v := range fields {
		radpack.SetU64(c, off, v)
	}
	return c
}

func TestTheBuffVectorIsTheMeasuredBytes(t *testing.T) {
	in := buffSpecTable()
	if len(in) != buffLenIn {
		t.Fatalf("input is %d bytes, want %d", len(in), buffLenIn)
	}
	out, err := attachBuffRows(in)
	if err != nil {
		t.Fatal(err)
	}
	if len(out) != buffLenOut {
		t.Fatalf("output is %d bytes, want %d", len(out), buffLenOut)
	}
	sum := sha256.Sum256(out)
	if got := hex.EncodeToString(sum[:]); got != buffVectorSha {
		t.Errorf("output sha-256 %s, want %s", got, buffVectorSha)
	}
}

// Each heal row is followed by its copy, and the rows of each player actor read heal, buff, then what the stock table had
// after the heal row (the barrier, and the other row for P1).
func TestTheBuffCopyFollowsItsHealRowPerActor(t *testing.T) {
	out, err := attachBuffRows(buffSpecTable())
	if err != nil {
		t.Fatal(err)
	}
	cases := []struct {
		actor uint64
		want  []uint64
	}{
		{playerActorIDs[0], []uint64{healAttachScript, buffAttachScript, barrierAttachScript, buffOtherValue}},
		{ocActors[1], []uint64{healAttachScript, buffAttachScript}},
		{playerActorIDs[1], []uint64{healAttachScript, buffAttachScript}},
	}
	for _, c := range cases {
		if got := actorScripts(t, out, c.actor); !slices.Equal(got, c.want) {
			t.Errorf("actor %016x rows %x, want %x", c.actor, got, c.want)
		}
	}
	if !bytes.Equal(parse(t, out).Rows[0], buffHealRow(playerActorIDs[0], buffTagP1)) {
		t.Error("the first stock row changed")
	}
}

// The heal copies' order is Cat's (S-ATTACH 2.2, steps_attach_order.go): the heal row, MEDIGUN's copy, then OVERCHARGE's. The
// names are the CSymbol64 of their scripts.
func TestTheHealCopiesAreInCatsOrder(t *testing.T) {
	if want := []uint64{healAttachScript, mediAttachScript, buffAttachScript}; !slices.Equal(healAttachOrder, want) {
		t.Errorf("healAttachOrder %x, want heal, medigun, buff %x", healAttachOrder, want)
	}
	if got := csymbol64("mp_combat_ability_buff"); got != buffAttachScript {
		t.Errorf("buffAttachScript %016x, want CSymbol64(mp_combat_ability_buff) %016x", uint64(buffAttachScript), got)
	}
	if got := csymbol64("mp_combat_ability_medigun"); got != mediAttachScript {
		t.Errorf("mediAttachScript %016x, want CSymbol64(mp_combat_ability_medigun) %016x", uint64(mediAttachScript), got)
	}
}

// A copy goes behind the rows of an earlier place in the order, whatever its place among the actor's other rows: MEDIGUN's row
// (here a synthetic one) is before the buff copy, which is put after it, not after the heal row.
func TestInsertOrderedRowPutsACopyBehindTheRowsOfEarlierPlaces(t *testing.T) {
	medi := row(scriptgate.RowStride, map[int]uint64{0: mediAttachScript, keyOff: playerActorIDs[0], scriptgate.ScriptOffset: mediAttachScript})
	rows := parse(t, table(scriptgate.RowStride, nil, buffHealRow(playerActorIDs[0], buffTagP1), medi)).Rows
	out, added, err := insertOrderedRow(rows, playerActorIDs[0], buffAttachScript, healAttachScript, healAttachOrder)
	if err != nil || !added {
		t.Fatalf("added %v, %v", added, err)
	}
	var got []uint64
	for _, r := range out {
		if radpack.U64(r, keyOff) == playerActorIDs[0] {
			got = append(got, radpack.U64(r, scriptgate.ScriptOffset))
		}
	}
	if want := []uint64{healAttachScript, mediAttachScript, buffAttachScript}; !slices.Equal(got, want) {
		t.Errorf("actor rows %x, want heal, medigun, buff %x", got, want)
	}
}

func TestInsertOrderedRowRefusesWhatTheOrderDoesNotPlaceOrAnchorOnTheWrongRow(t *testing.T) {
	rows := parse(t, table(scriptgate.RowStride, nil, buffHealRow(playerActorIDs[0], buffTagP1))).Rows
	if _, _, err := insertOrderedRow(rows, playerActorIDs[0], pointAttachScript, healAttachScript, healAttachOrder); err == nil {
		t.Error("a script the order does not place was placed")
	}
	if _, _, err := insertOrderedRow(rows, playerActorIDs[0], buffAttachScript, barrierAttachScript, healAttachOrder); err == nil {
		t.Error("an order whose first script is not the anchor was used")
	}
}

// A table that is not the stock one is refused with the actor and the cause, and no bytes are returned: the input is unchanged.
func TestAttachBuffRowsRefusesAndChangesNothing(t *testing.T) {
	attached, err := attachBuffRows(buffSpecTable())
	if err != nil {
		t.Fatal(err)
	}
	h1 := buffHealRow(playerActorIDs[0], buffTagP1)
	h3 := buffHealRow(ocActors[1], buffTagB113)
	cases := []struct {
		name string
		in   []byte
		want string
	}{
		{"rows of another size", table(buffOtherRowLen, nil, row(buffOtherRowLen, map[int]uint64{keyOff: playerActorIDs[0]})), fmt.Sprintf("rows of %d bytes", buffOtherRowLen)},
		{"no heal row for an actor", table(scriptgate.RowStride, []byte(buffTestTail), h1, buffBarrierP1(), buffHealRow(playerActorIDs[1], buffTagP2), buffOtherRow()), "no heal-script row"},
		{"two heal rows for an actor", table(scriptgate.RowStride, []byte(buffTestTail), h1, buffHealRow(playerActorIDs[0], buffTagP1), buffBarrierP1(), buffHealRow(playerActorIDs[1], buffTagP2), buffHealRow(ocActors[1], buffTagB113), buffOtherRow()), "rows of anchor script"},
		{"already attached", attached, "already runs the OVERCHARGE copy"},
		{"a heal row whose +0 is not its +32", table(scriptgate.RowStride, []byte(buffTestTail), buffSpecRows(withFields(h1, map[int]uint64{scriptNameOff: testOtherScript}), h3)...), "expected"},
		{"a donor with pool size", table(scriptgate.RowStride, []byte(buffTestTail), buffSpecRows(withFields(h1, map[int]uint64{buffDesc3Size: buffPoolSize}), h3)...), "pool data"},
		{"a donor with pool capacity", table(scriptgate.RowStride, []byte(buffTestTail), buffSpecRows(withFields(h1, map[int]uint64{buffDesc3Cap: buffPoolCap}), h3)...), "pool data"},
		{"a donor with pool count", table(scriptgate.RowStride, []byte(buffTestTail), buffSpecRows(withFields(h1, map[int]uint64{buffDesc3Count: buffPoolCount}), h3)...), "pool data"},
	}
	for _, c := range cases {
		before := bytes.Clone(c.in)
		out, err := attachBuffRows(c.in)
		if err == nil {
			t.Errorf("%s: attached", c.name)
			continue
		}
		if out != nil {
			t.Errorf("%s: returned bytes with the error", c.name)
		}
		if !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: error %q does not name %q", c.name, err, c.want)
		}
		if !bytes.Equal(before, c.in) {
			t.Errorf("%s: the input was changed", c.name)
		}
	}
}

// buffEntryKey is the key of the copy's CScriptResource record. The ids are variables: the copy's type overflows a constant
// int64 conversion.
func buffEntryKey() radpack.Key {
	var typ, name uint64 = scriptgate.ScriptType, buffAttachScript
	return radpack.Key{Type: int64(typ), Name: int64(name)}
}

// buffPack is a pack over a stock global CScriptCR table holding rows, with the barrier and the heal scripts on disk for Rekey,
// and, when entry is set, the stock set's CScriptResource entry of the copy.
func buffPack(t *testing.T, rows []byte, entry bool) *Pack {
	t.Helper()
	var table, level uint64 = scriptTable, globalLevel // not constants: the ids overflow a constant int64 conversion
	data := t.TempDir()
	res := map[radpack.Key][]byte{{Type: int64(table), Name: int64(level)}: rows}
	if entry {
		res[buffEntryKey()] = make([]byte, scriptResourceEntry)
	}
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
	for name, content := range map[uint64]string{barrierAttachScript: "MZ stock barrier", healAttachScript: buffRekeyContent} {
		if err := os.WriteFile(filepath.Join(scripts, fmt.Sprintf("%016x.dll", name)), []byte(content), testFileMode); err != nil {
			t.Fatal(err)
		}
	}
	return &Pack{gameDir: game, sets: map[string]*radpack.Set{Sets[0]: s}, changes: map[string]map[radpack.Key][]byte{}, scripts: map[string][]byte{}}
}

// stepNamed is the registered step of that name.
func stepNamed(t *testing.T, name string) Step {
	t.Helper()
	for _, st := range steps {
		if st.Name == name {
			return st
		}
	}
	t.Fatalf("step %q is not registered", name)
	return Step{}
}

// The step writes the global level's table and its entry, and lists the copy for G2 with the heal script's digest.
func TestTheBuffStepWritesTheCopyRowsAndItsEntryAndNamesTheCopy(t *testing.T) {
	p := buffPack(t, buffSpecTable(), false)
	if err := stepNamed(t, buffAttachStep).Apply(p); err != nil {
		t.Fatal(err)
	}
	var table, level uint64 = scriptTable, globalLevel // not constants: the ids overflow a constant int64 conversion
	got, ok := p.changes[Sets[0]][radpack.Key{Type: int64(table), Name: int64(level)}]
	if !ok {
		t.Fatal("the global level's CScriptCR table was not written")
	}
	if n := len(parse(t, got).Rows); n != buffRowsOut {
		t.Errorf("%d rows written, want %d", n, buffRowsOut)
	}
	entry, ok := p.changes[Sets[0]][buffEntryKey()]
	if !ok || !bytes.Equal(entry, make([]byte, scriptResourceEntry)) {
		t.Errorf("the copy's entry %x (written %v), want %d zero bytes", entry, ok, scriptResourceEntry)
	}
	want := RekeyEntry{Name: fmt.Sprintf("%016x", uint64(buffAttachScript)), Platform: RekeyPlatform,
		StockSource: fmt.Sprintf("%016x", uint64(healAttachScript)), StockSha256: digest([]byte(buffRekeyContent))}
	if len(p.rekeys) != 1 || p.rekeys[0] != want {
		t.Errorf("rekeys %+v, want the copy of the heal script %+v", p.rekeys, want)
	}
}

// A refused table writes nothing and names no copy.
func TestTheBuffStepRefusedWritesNothing(t *testing.T) {
	rows := table(scriptgate.RowStride, []byte(buffTestTail), buffSpecRows(buffHealRow(playerActorIDs[0], buffTagP1), buffOtherRow())[:buffSpecHeadRows]...)
	p := buffPack(t, rows, false)
	if err := stepNamed(t, buffAttachStep).Apply(p); err == nil {
		t.Fatal("a table without the B113 heal row was attached")
	}
	if len(p.changes[Sets[0]]) != 0 || len(p.rekeys) != 0 {
		t.Errorf("a refused attach left changes %d and rekeys %+v", len(p.changes[Sets[0]]), p.rekeys)
	}
}

// A stock set that already lists the copy's entry is refused before anything is written, the table included.
func TestTheBuffStepRefusesAStockSetThatAlreadyListsTheCopy(t *testing.T) {
	p := buffPack(t, buffSpecTable(), true)
	if err := stepNamed(t, buffAttachStep).Apply(p); err == nil {
		t.Fatal("a stock set listing the copy's entry was attached")
	}
	if len(p.changes[Sets[0]]) != 0 || len(p.rekeys) != 0 {
		t.Errorf("a refused attach left changes %d and rekeys %+v", len(p.changes[Sets[0]]), p.rekeys)
	}
}

// The step's two declared fields read what the step writes: the copy's rows, and its four-byte entry.
func TestTheBuffAttachFieldsReadWhatTheyWrite(t *testing.T) {
	st := stepNamed(t, buffAttachStep)
	if len(st.Writes) != buffWrites {
		t.Fatalf("%d fields declared, want 2", len(st.Writes))
	}
	rows, entry := st.Writes[0], st.Writes[1]
	if rows.Type != scriptTable || len(rows.Records) != 1 || rows.Records[0] != globalLevel {
		t.Errorf("rows field %+v", rows)
	}
	if entry.Type != scriptgate.ScriptType || len(entry.Records) != 1 || entry.Records[0] != buffAttachScript {
		t.Errorf("entry field %+v", entry)
	}
	stock := buffSpecTable()
	if got, err := rows.Read(stock); err != nil || len(got) != 0 {
		t.Errorf("the stock table's copy rows read %x, %v", got, err)
	}
	out, err := attachBuffRows(stock)
	if err != nil {
		t.Fatal(err)
	}
	var want []byte
	for _, r := range parse(t, out).Rows {
		if radpack.U64(r, scriptgate.ScriptOffset) == buffAttachScript {
			want = append(want, r...)
		}
	}
	if got, err := rows.Read(out); err != nil || !bytes.Equal(got, want) || len(got) != buffCopies*scriptgate.RowStride {
		t.Errorf("copy rows read %d bytes, %v; want the %d copies", len(got), err, buffCopies)
	}
	// A row whose +0 is the copy and whose +32 is another script is not a copy: the reader keys on ScriptOffset, not on +0.
	stray := row(scriptgate.RowStride, map[int]uint64{0: buffAttachScript, keyOff: testOtherActor, scriptgate.ScriptOffset: testOtherG2Script})
	withStray := table(scriptgate.RowStride, nil, append(parse(t, out).Rows, stray)...)
	if got, err := rows.Read(withStray); err != nil || !bytes.Equal(got, want) {
		t.Errorf("a row with +0 the copy and +32 another script was read as a copy (%d bytes, %v)", len(got), err)
	}
	if got, err := entry.Read(make([]byte, scriptResourceEntry)); err != nil || len(got) != scriptResourceEntry {
		t.Errorf("a %d-byte entry read %x, %v", scriptResourceEntry, got, err)
	}
	if _, err := entry.Read(make([]byte, scriptResourceEntry+1)); err == nil {
		t.Error("an entry of the wrong length was read")
	}
}

// The three attaches write one global table. Whichever order they run in, the table and the entries are the same bytes (the
// OrderFree declarations of the three steps rest on this), and each player actor's rows read in attach order.
func TestBuffAndTheAttachOrdersAgree(t *testing.T) {
	rows := table(scriptgate.RowStride, nil,
		otherAttachRow(),
		buffHealRow(playerActorIDs[0], buffTagP1),
		barrierAttachRow(playerActorIDs[0]),
		buffHealRow(ocActors[1], buffTagB113),
		buffHealRow(playerActorIDs[1], buffTagP2),
		barrierAttachRow(playerActorIDs[1]))
	run := func(names ...string) []byte {
		p := buffPack(t, rows, false)
		for _, name := range names {
			if err := stepNamed(t, name).Apply(p); err != nil {
				t.Fatalf("%s: %v", name, err)
			}
		}
		var table, level uint64 = scriptTable, globalLevel // not constants: the ids overflow a constant int64 conversion
		return p.changes[Sets[0]][radpack.Key{Type: int64(table), Name: int64(level)}]
	}
	a := run(buffAttachStep, pointAttachStep, veilAttachStep)
	b := run(veilAttachStep, pointAttachStep, buffAttachStep)
	if !bytes.Equal(a, b) {
		t.Fatal("the table differs by the order the attaches run in")
	}
	if n := len(parse(t, a).Rows); n != buffRowsAfterAll {
		t.Fatalf("%d rows, want %d (the six stock rows, and the point, veil and buff rows)", n, buffRowsAfterAll)
	}
	want := []uint64{healAttachScript, buffAttachScript, barrierAttachScript, pointAttachScript, veilAttachScript}
	if got := actorScripts(t, a, playerActorIDs[0]); !slices.Equal(got, want) {
		t.Errorf("the first player actor's rows %x, want %x", got, want)
	}
	if got := actorScripts(t, a, playerActorIDs[1]); !slices.Equal(got, want) {
		t.Errorf("the second player actor's rows %x, want %x", got, want)
	}
	if got := actorScripts(t, a, ocActors[1]); !slices.Equal(got, []uint64{healAttachScript, buffAttachScript}) {
		t.Errorf("the B113 actor's rows %x", got)
	}
}

// G2 reads each row's script at ScriptOffset: the copy is named once per OVERCHARGE actor.
func TestTheBuffRowsNameTheCopyToG2(t *testing.T) {
	out, err := attachBuffRows(buffSpecTable())
	if err != nil {
		t.Fatal(err)
	}
	scripts, err := scriptgate.RowScripts(out)
	if err != nil {
		t.Fatal(err)
	}
	named := 0
	for _, s := range scripts {
		if s == buffAttachScript {
			named++
		}
	}
	if named != buffCopies {
		t.Errorf("the copy is named by %d rows, want %d", named, buffCopies)
	}
}
