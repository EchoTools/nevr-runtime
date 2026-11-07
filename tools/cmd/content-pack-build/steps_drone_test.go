package main

// The DRONE row rules over synthetic tables with the shipped layout (a 0x38-byte header, rows, a tail); no row of the
// game's is used. stock_steps_test.go runs them over the shipped tables when a machine has them.

import (
	"bytes"
	"encoding/binary"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

func table(size int, tail []byte, rows ...[]byte) []byte {
	head := make([]byte, 0x38)
	le := binary.LittleEndian
	le.PutUint64(head[0x08:], uint64(len(rows)*size))
	le.PutUint64(head[0x18:], 1<<32)
	le.PutUint64(head[0x28:], uint64(len(rows)))
	le.PutUint64(head[0x30:], uint64(len(rows)))
	out := head
	for _, r := range rows {
		out = append(out, r...)
	}
	return append(out, tail...)
}

func row(size int, fields map[int]uint64) []byte {
	r := make([]byte, size)
	for off, v := range fields {
		binary.LittleEndian.PutUint64(r[off:], v)
	}
	return r
}

func parse(t *testing.T, b []byte) *radpack.RowTable {
	t.Helper()
	tb, err := radpack.ParseRowTable(b)
	if err != nil {
		t.Fatal(err)
	}
	return tb
}

func TestTheDroneBulletTakesTheAssaultBulletsEffects(t *testing.T) {
	b := table(136, []byte("tail"),
		row(136, map[int]uint64{0: 1, keyOff: assaultBullet, 0x58: 0xaaaa, 0x60: 0xbbbb, 0x68: 7}),
		row(136, map[int]uint64{0: 2, keyOff: 0x5555}),
		row(136, map[int]uint64{0: 3, keyOff: droneBullet, 0x58: ^uint64(0), 0x60: ^uint64(0), 0x68: 9}))
	tb := parse(t, b)
	if err := droneBulletEffects(tb); err != nil {
		t.Fatal(err)
	}
	out := tb.Bytes()
	drone := parse(t, out).Rows[2]
	if radpack.U64(drone, 0x58) != 0xaaaa || radpack.U64(drone, 0x60) != 0xbbbb || radpack.U64(drone, 0x68) != 9 {
		t.Errorf("drone row %x", drone)
	}
	if len(out) != len(b) || !bytes.Equal(out[:0x38], b[:0x38]) || !bytes.HasSuffix(out, []byte("tail")) {
		t.Error("the table's size, header or tail changed")
	}
	if err := droneBulletEffects(parse(t, table(136, nil, row(136, map[int]uint64{keyOff: assaultBullet})))); err == nil {
		t.Error("a table with no drone bullet row was changed")
	}
	if err := droneBulletEffects(parse(t, table(136, nil, row(136, map[int]uint64{keyOff: droneBullet})))); err == nil {
		t.Error("a table with no assault bullet row was changed")
	}
	if err := droneBulletEffects(parse(t, table(0x60, nil, row(0x60, map[int]uint64{keyOff: assaultBullet}), row(0x60, map[int]uint64{keyOff: droneBullet})))); err == nil {
		t.Error("a table whose rows end before the effect ids was changed")
	}
}

func TestTheDroneBulletGetsCopiesOfTheAssaultBulletsParticleRows(t *testing.T) {
	b := table(152, nil,
		row(152, map[int]uint64{0: 0xe1, keyOff: assaultBullet}),
		row(152, map[int]uint64{0: 0xe2, keyOff: assaultBullet}),
		row(152, map[int]uint64{0: 0xe3, keyOff: 0x5555}))
	tb := parse(t, b)
	if err := droneParticleRows(tb); err != nil {
		t.Fatal(err)
	}
	out := parse(t, tb.Bytes())
	if len(out.Rows) != 5 {
		t.Fatalf("%d rows", len(out.Rows))
	}
	for i, want := range []uint64{0xe1, 0xe2} {
		r := out.Rows[3+i]
		if radpack.U64(r, 0) != want || radpack.U64(r, keyOff) != droneBullet {
			t.Errorf("appended row %d: %x", i, r[:16])
		}
	}
	if !bytes.Equal(out.Rows[0], tb.Rows[0]) || radpack.U64(out.Rows[0], keyOff) != assaultBullet {
		t.Error("an assault row changed")
	}
	// Once is enough: a table that already has a drone row is refused, so a step run twice cannot copy twice.
	if err := droneParticleRows(out); err == nil || !strings.Contains(err.Error(), "already") {
		t.Errorf("a second run was not refused: %v", err)
	}
	if err := droneParticleRows(parse(t, table(152, nil, row(152, map[int]uint64{keyOff: 1})))); err == nil {
		t.Error("a table with no assault row was changed")
	}
}

func TestTheDroneActorGetsTheSharedScriptRowBeforeItsOwn(t *testing.T) {
	b := table(720, []byte("tail data"),
		row(720, map[int]uint64{0: 0x1111, keyOff: 0xa1}),
		row(720, map[int]uint64{0: sharedScript, keyOff: 0xa2, 0x100: 42}), // the first row of the script
		row(720, map[int]uint64{0: sharedScript, keyOff: 0xa3, 0x100: 43}),
		row(720, map[int]uint64{0: 0x2222, keyOff: droneActor}), // the drone's own row
		row(720, map[int]uint64{0: 0x3333, keyOff: droneActor}))
	tb := parse(t, b)
	if err := droneScriptRow(tb); err != nil {
		t.Fatal(err)
	}
	out := parse(t, tb.Bytes())
	if len(out.Rows) != 6 {
		t.Fatalf("%d rows", len(out.Rows))
	}
	ins := out.Rows[3]
	if radpack.U64(ins, 0) != sharedScript || radpack.U64(ins, keyOff) != droneActor || radpack.U64(ins, 0x100) != 42 {
		t.Errorf("inserted row %x", ins[:16])
	}
	if radpack.U64(out.Rows[4], 0) != 0x2222 || !bytes.Equal(out.Tail, []byte("tail data")) {
		t.Error("the drone's own row is not right after the inserted one, or the tail changed")
	}
	if err := droneScriptRow(parse(t, table(720, nil, row(720, map[int]uint64{0: sharedScript})))); err == nil {
		t.Error("a table with no drone actor row was changed")
	}
}

func TestARowTableWhoseHeaderDoesNotFitIsRefused(t *testing.T) {
	b := table(16, nil, row(16, nil), row(16, nil))
	binary.LittleEndian.PutUint64(b[0x28:], 5) // capacity above the count: not the layout these tables have
	if _, err := radpack.ParseRowTable(b); err == nil {
		t.Error("a table whose capacity is not its count was read")
	}
	b = table(16, nil, row(16, nil))
	if _, err := radpack.ParseRowTable(b[:0x38+8]); err == nil {
		t.Error("a table shorter than its rows was read")
	}
}

func TestTheDroneStepsChangeTheFiveCombatLevelsAndNoOther(t *testing.T) {
	const other = 0x1234                // a level that is not a combat level
	var scriptType uint64 = scriptTable // not a constant: the id overflows a constant int64 conversion
	res := map[radpack.Key][]byte{}
	for _, lvl := range append(append([]uint64(nil), droneLevels...), other) {
		res[radpack.Key{Type: int64(bulletTableType), Name: int64(lvl)}] = table(136, nil,
			row(136, map[int]uint64{keyOff: assaultBullet, 0x58: lvl}), row(136, map[int]uint64{keyOff: droneBullet}))
		res[radpack.Key{Type: int64(particleTableType), Name: int64(lvl)}] = table(152, nil, row(152, map[int]uint64{keyOff: assaultBullet}))
		res[radpack.Key{Type: int64(scriptType), Name: int64(lvl)}] = table(720, nil,
			row(720, map[int]uint64{0: sharedScript}), row(720, map[int]uint64{keyOff: droneActor}))
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
	for _, st := range steps {
		if err := st.Apply(p); err != nil {
			t.Fatalf("%s: %v", st.Name, err)
		}
	}
	got := p.changes[Sets[0]]
	if len(got) != 15 {
		t.Errorf("%d resources changed, want 15", len(got))
	}
	for _, lvl := range droneLevels {
		b := got[radpack.Key{Type: int64(bulletTableType), Name: int64(lvl)}]
		if b == nil || radpack.U64(parse(t, b).Rows[1], 0x58) != lvl {
			t.Errorf("level %016x: the drone bullet row did not take this level's assault effects", lvl)
		}
		if b := got[radpack.Key{Type: int64(particleTableType), Name: int64(lvl)}]; b == nil || len(parse(t, b).Rows) != 2 {
			t.Errorf("level %016x: the particle table did not get the drone row", lvl)
		}
		if b := got[radpack.Key{Type: int64(scriptType), Name: int64(lvl)}]; b == nil || len(parse(t, b).Rows) != 3 {
			t.Errorf("level %016x: the script table did not get the drone row", lvl)
		}
	}
	for _, typ := range []uint64{bulletTableType, particleTableType, scriptTable} {
		if _, ok := got[radpack.Key{Type: int64(typ), Name: other}]; ok {
			t.Errorf("table %016x of a level that is not a combat level was changed", typ)
		}
	}
}

func TestFindSkipsAnOffsetPastTheRow(t *testing.T) {
	tb := parse(t, table(16, nil, row(16, map[int]uint64{8: 5})))
	if i := tb.Find(16, 5); i != -1 {
		t.Errorf("Find past the row's end is %d, want -1", i)
	}
	if i := tb.Find(8, 5); i != 0 {
		t.Errorf("Find at +8 is %d, want 0", i)
	}
}
