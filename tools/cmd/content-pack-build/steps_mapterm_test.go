package main

// The post-match terminal step over synthetic level tables with the shipped layouts (an actor table, flat component
// tables, CScriptCR with nested press and label tables, CInstanceModelCR); no record of the game's is used.

import (
	"bytes"
	"fmt"
	"math"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

const (
	tNodeRoot, tNodePress, tNodeLabel, tNodeModel, tNodeLobbyKid = 0x8000000000000011, 0x22, 0x33, 0x44, 0x55
	tPrefab                                                      = 0x7777
	tPressOther                                                  = 0x5555
	tLobbyLabel                                                  = "RIGHT"
	tUnknownType                                                 = "aaaaaaaaaaaaaaaa"
	tUnrelatedType                                               = "1111111111111111"
	tTransformStride, tButtonStride                              = 0x50, 0x90
	tTolerance                                                   = 1e-5
	// tResourcesPerLevel: the actor table, CScriptCR, CTransformCR, CR15ButtonInteractCR, CInstanceModelCR and the
	// unknown flat table that names a subtree actor change; the unrelated table does not.
	tResourcesPerLevel = 6
)

type fixOpts struct {
	alreadyPatched, noLobby bool
	press                   uint64
	label                   string
}

func stockOpts() fixOpts { return fixOpts{press: mapTermLeftPress, label: mapTermStockLabel} }

func tDescTable(stride int, rows [][]byte, tail []byte) []byte {
	d := mtDesc{size: uint64(len(rows) * stride), align: 8, flags: 1, capacity: uint64(len(rows)), count: uint64(len(rows))}
	out := d.pack()
	for _, r := range rows {
		out = append(out, r...)
	}
	return append(out, tail...)
}

func tRow(stride int, owner uint64, fields map[int][]byte) []byte {
	r := make([]byte, stride)
	le.PutUint64(r[mtOwnerAt:], owner)
	for at, b := range fields {
		copy(r[at:], b)
	}
	return r
}

func tVec(x, y, z float32) []byte {
	var b [mtVecLen]byte
	mtPutVec(b[:], 0, [3]float64{float64(x), float64(y), float64(z)})
	return b[:]
}

type scriptRow struct {
	owner  uint64
	press  []uint64
	labels []string
}

func tScriptTable(rows []scriptRow) []byte {
	const stride = mtScriptTablesAt + mtDescLen*mtScriptTables
	var packed [][]byte
	for _, r := range rows {
		row := tRow(stride, r.owner, nil)
		for t, n := range map[int][2]int{mapTermPressTable: {len(r.press), mapTermPressStride}, mapTermLabelTable: {len(r.labels), mapTermLabelStride}} {
			d := mtDesc{size: uint64(n[0] * n[1]), align: 8, flags: 1, capacity: uint64(n[0]), count: uint64(n[0])}
			copy(row[mtScriptTablesAt+mtDescLen*t:], d.pack())
		}
		packed = append(packed, row)
	}
	out := tDescTable(stride, packed, nil)
	pad := func() { out = append(out, make([]byte, mtAlignUp(len(out), mtWordLen)-len(out))...) }
	for _, r := range rows {
		if len(r.press) > 0 {
			pad()
			for _, s := range r.press {
				out = append(out, tRow(mapTermPressStride, 0, map[int][]byte{mapTermSymbolAt: le.AppendUint64(nil, s)})...)
			}
		}
		if len(r.labels) > 0 {
			pad()
			for _, s := range r.labels {
				row := make([]byte, mapTermLabelStride)
				copy(row[mapTermSymbolAt:], s)
				out = append(out, row...)
			}
		}
	}
	return out
}

func tActorTable(l mapTermLevel, o fixOpts) []byte {
	id := func(n uint64) mtActor {
		return mtActor{nodeid: n, name: n + 1, prefabid: mtUnset, numpooled: 0xffff}
	}
	tf := func(x, y, z float32) []byte {
		b := make([]byte, mtTransformLen)
		le.PutUint32(b[12:], math.Float32bits(1)) // quaternion w
		copy(b[mtPosAt:], tVec(x, y, z))
		copy(b[mtScaleAt:], tVec(1, 1, 1))
		return b
	}
	a := []mtActor{id(tNodeRoot), id(l.rematch), id(tNodePress), id(tNodeLabel), id(tNodeModel), id(l.lobby), id(tNodeLobbyKid)}
	parents := []uint16{0, 0, 1, 2, 1, 0, 5}
	pos := [][3]float32{{0, 1, 2}, {0.1, 1, 2}, {0.1, 1, 2.01}, {0.1, 1, 2.02}, {0.1, 1.02, 2}, {0.3, 1, 2}, {0.3, 1, 2.05}}
	for i := range a {
		a[i].parent, a[i].transform = parents[i], tf(pos[i][0], pos[i][1], pos[i][2])
	}
	a[4].prefabid, a[6].prefabid, a[1].bits[2] = tPrefab, tPrefab, true
	if o.noLobby {
		a[5].nodeid = 0x999
	}
	if o.alreadyPatched {
		extra := id(csymbol64("mapswitch_dyson_0"))
		extra.transform = a[0].transform
		a = append(a, extra)
	}
	t := &mtActorTable{header: make([]byte, mtActorHeader), groupTemplate: mtDesc{align: 8, flags: 1}, actors: a,
		aliases: [][2]uint64{{0x4242, tNodeRoot}}}
	for i := range t.descs {
		t.descs[i] = mtDesc{align: 8, flags: 1}
	}
	b, err := t.write()
	if err != nil {
		panic(err)
	}
	return b
}

func tInstanceModels(l mapTermLevel) []byte {
	m := &mtInstanceModels{header: make([]byte, mtInstHeader), nbits: 2}
	for _, sl := range mtInstSlots {
		le.PutUint32(m.header[sl[0]+mtDescAlignAt:], 8)
		le.PutUint32(m.header[sl[0]+mtDescAlignAt+4:], 1)
	}
	m.slots = make([][][]byte, len(mtInstSlots))
	for i, sl := range mtInstSlots {
		switch i {
		case mtInstBits:
			m.slots[i] = [][]byte{le.AppendUint64(nil, 0b01)} // record 0 has its bit set
		case 2:
			m.slots[i] = [][]byte{make([]byte, sl[1])}
		default:
			m.slots[i] = [][]byte{make([]byte, sl[1]), make([]byte, sl[1])}
		}
	}
	m.slots[mtInstLookup][0] = tRow(24, 0, map[int][]byte{16: le.AppendUint64(nil, l.rematch)})
	m.slots[mtInstOwnerRow][0] = tRow(72, l.rematch, nil)
	m.slots[mtInstOwnerRow][1] = tRow(72, 0xdead, nil)
	return m.write()
}

func tLevel(l mapTermLevel, o fixOpts) map[string][]byte {
	rows := func(stride int, owners ...uint64) [][]byte {
		var out [][]byte
		for _, ow := range owners {
			out = append(out, tRow(stride, ow, map[int][]byte{mtTransformPosAt: tVec(0.1, 1, 2), mtTransformScaleA: tVec(1, 1, 1)}))
		}
		return out
	}
	button := tRow(tButtonStride, tNodeRoot, map[int][]byte{mtButtonPosAt: tVec(0.1, 1, 2)})
	return map[string][]byte{
		mtActorsType:       tActorTable(l, o),
		mapTermScript:      tScriptTable([]scriptRow{{l.rematch, []uint64{o.press}, []string{o.label}}, {l.lobby, []uint64{tPressOther}, []string{tLobbyLabel}}, {0xdead, nil, nil}}),
		"92abd3e1432bf5e8": tDescTable(tTransformStride, rows(tTransformStride, l.rematch, tNodePress, l.lobby, 0xdead), nil),
		"e9b24ea816dece48": tDescTable(tButtonStride, [][]byte{tRow(tButtonStride, l.rematch, map[int][]byte{mtButtonPosAt: tVec(0.1, 1, 2)}), button}, nil),
		mtInstanceType:     tInstanceModels(l),
		tUnknownType:       tDescTable(16, [][]byte{tRow(16, tNodeLabel, nil)}, nil),
		tUnrelatedType:     tDescTable(16, [][]byte{tRow(16, 0xbeef, nil)}, nil),
	}
}

func tHex(s string) int64 {
	var v uint64
	fmt.Sscanf(s, "%x", &v)
	return int64(v)
}

func tPack(t *testing.T, opts func(mapTermLevel) fixOpts) *Pack {
	t.Helper()
	dir := t.TempDir()
	res := map[radpack.Key][]byte{}
	for _, l := range mapTermLevels {
		for typ, b := range tLevel(l, opts(l)) {
			res[radpack.Key{Type: tHex(typ), Name: int64(l.level)}] = b
		}
	}
	if err := radpack.WriteStockSet(dir, Sets[0], []map[radpack.Key][]byte{res}); err != nil {
		t.Fatal(err)
	}
	s, _, err := radpack.OpenSet(dir, Sets[0])
	if err != nil {
		t.Fatal(err)
	}
	return &Pack{gameDir: dir, sets: map[string]*radpack.Set{Sets[0]: s}, changes: map[string]map[radpack.Key][]byte{}, scripts: map[string][]byte{}}
}

func tStock(mapTermLevel) fixOpts { return stockOpts() }

func tNear(t *testing.T, what string, got, want [3]float64) {
	t.Helper()
	for i := range got {
		if math.Abs(got[i]-want[i]) > tTolerance {
			t.Errorf("%s: %v, want %v", what, got, want)
			return
		}
	}
}

func TestSymbol64IsTheEnginesHashVeilNetHears(t *testing.T) {
	// src/runtime/combat/veilnet.cpp EVT_MAPSWITCH and MAP_BUTTON_ACTOR: the names his tool hashes.
	for i, m := range mapTermMaps {
		if got, want := csymbol64("mapswitch_"+m), []uint64{0xc60e17c86d1f3df0, 0x63093755a2c24ebe, 0x3d77042c79604528, 0xc60e17cb751921ed}[i]; got != want {
			t.Errorf("mapswitch_%s = %016x, want %016x", m, got, want)
		}
		if got, want := csymbol64("mapswitch_"+m+"_0"), []uint64{0x3d75142c65677576, 0xba2b43af12ba468a, 0xc8cc27933f907292, 0x3d760c2a797a7576}[i]; got != want {
			t.Errorf("mapswitch_%s_0 = %016x, want %016x", m, got, want)
		}
	}
}

func TestTheHashSignExtendsAByteAboveAscii(t *testing.T) {
	// sdk/include/nevr/symbol_hash.h's vectors for bytes >= 0x80 (the game's MOVSX at echovr.exe 0x1400ce199).
	for name, want := range map[string]uint64{"caf\xc3\xa9": 0xcd0714df62754f5f, "\xff\xfe": 0x1f17d45f0697bb92} {
		if got := csymbol64(name); got != want {
			t.Errorf("csymbol64(%q) = %016x, want %016x", name, got, want)
		}
	}
}

func TestTheDeclaredFieldsReadTheMapButtonsTheStepWrites(t *testing.T) {
	p := tPack(t, tStock)
	if err := mapTerminal(p); err != nil {
		t.Fatal(err)
	}
	var st *Step
	for i := range steps {
		if steps[i].Name == mapTermStepName {
			st = &steps[i]
		}
	}
	if st == nil {
		t.Fatalf("no step named %q", mapTermStepName)
	}
	byType := map[uint64]Field{}
	for _, f := range st.Writes {
		byType[f.Type] = f
		if len(f.Records) != mapTermLevelCount || f.Set != Sets[0] {
			t.Errorf("field %q: records %x, set %s", f.Name, f.Records, f.Set)
		}
	}
	if len(byType) != len(st.Writes) || len(st.Writes) != len(mtTypeNames)+1 {
		t.Errorf("%d fields over %d types, want the actor table and each of the %d TYPE_NAMES", len(st.Writes), len(byType), len(mtTypeNames))
	}
	read := func(l mapTermLevel, typ string, after bool) []byte {
		f, ok := byType[uint64(tHex(typ))]
		if !ok {
			t.Fatalf("no field declared for %s", typ)
		}
		b := tLevel(l, stockOpts())[typ]
		if after {
			b = p.changes[Sets[0]][radpack.Key{Type: tHex(typ), Name: int64(l.level)}]
		}
		out, err := f.Read(b)
		if err != nil {
			t.Fatalf("level %016x %s: %v", l.level, typ, err)
		}
		return out
	}
	for _, l := range mapTermLevels {
		var shown []string
		for _, m := range mapTermMaps {
			if m != l.own {
				shown = append(shown, m)
			}
		}
		// The script field: nothing in the stock table, each shown button's press event and label after the step.
		if b := read(l, mapTermScript, false); len(b) != 0 {
			t.Errorf("level %016x: the stock script table reads %x", l.level, b)
		}
		var want []byte
		for _, m := range shown {
			want = le.AppendUint64(want, csymbol64("mapswitch_"+m))
			label := make([]byte, mapTermLabelLen)
			copy(label, wantLabels[m])
			want = append(want, label...)
		}
		if got := read(l, mapTermScript, true); !bytes.Equal(got, want) {
			t.Errorf("level %016x: the script field reads %x, want %x", l.level, got, want)
		}
		// The actor field: the stock table holds only the REMATCH and LOBBY subtrees' transforms; after the step, those
		// transforms have moved and the buttons' actors are there too.
		before, after := read(l, mtActorsType, false), read(l, mtActorsType, true)
		at, err := mtReadActorTable(tLevel(l, stockOpts())[mtActorsType])
		if err != nil {
			t.Fatal(err)
		}
		idx := map[uint64]int{}
		for i, a := range at.actors {
			idx[a.nodeid] = i
		}
		moved := len(at.subtree(idx[l.rematch])) + len(at.subtree(idx[l.lobby]))
		parts := len(at.subtree(idx[l.rematch]))
		const movedLen, buttonLen = mtU64Len + mtTransformLen, 3*mtU64Len + mtU16Len + mtTransformLen
		if len(before) != moved*movedLen || len(after) != moved*movedLen+len(shown)*parts*buttonLen {
			t.Errorf("level %016x: the actor field reads %d bytes before and %d after, want %d and %d", l.level, len(before), len(after),
				moved*movedLen, moved*movedLen+len(shown)*parts*buttonLen)
		}
		if bytes.Equal(before[:movedLen], after[:movedLen]) {
			t.Errorf("level %016x: the REMATCH transform reads the same before and after the step", l.level)
		}
		// The component fields: no button row in a stock table; after the step, rows owned by the buttons' actors.
		for _, typ := range []string{"92abd3e1432bf5e8", "e9b24ea816dece48", mtInstanceType} {
			if b := read(l, typ, false); len(b) != 0 {
				t.Errorf("level %016x %s: the stock table reads %x", l.level, typ, b)
			}
			if b := read(l, typ, true); len(b) == 0 {
				t.Errorf("level %016x %s: no map button row after the step", l.level, typ)
			}
		}
		if b := read(l, "92abd3e1432bf5e8", true); !mapTermButtonIds()[le.Uint64(b[mtOwnerAt:])] {
			t.Errorf("level %016x: the transform field's first row is owned by %016x, not a map button", l.level, le.Uint64(b[mtOwnerAt:]))
		}
	}
}

func TestTheMapTerminalsResourcesAreNamedTerms(t *testing.T) {
	var added []radpack.Key
	for _, l := range mapTermLevels {
		added = append(added, radpack.Key{Type: tHex(mtActorsType), Name: int64(l.level)}, radpack.Key{Type: tHex("e9b24ea816dece48"), Name: int64(l.level)})
	}
	if n, err := mapTermResourcesNamed(added); err != nil || n != mapTermLevelCount {
		t.Errorf("an actor table and a button table per level: %d component tables, %v", n, err)
	}
	for _, c := range []struct {
		what string
		keys []radpack.Key
		want string
	}{
		{"a script table, counted by the drone step", append(added[:len(added):len(added)], radpack.Key{Type: tHex(mapTermScript), Name: int64(mapTermLevels[0].level)}), "a resource no term names"},
		{"a transform table, counted by the dampener step", append(added[:len(added):len(added)], radpack.Key{Type: tHex("92abd3e1432bf5e8"), Name: int64(mapTermLevels[0].level)}), "a resource no term names"},
		{"a type outside TYPE_NAMES", append(added[:len(added):len(added)], radpack.Key{Type: tHex(tUnknownType), Name: int64(mapTermLevels[0].level)}), "a resource no term names"},
		{"the lobby level", append(added[:len(added):len(added)], radpack.Key{Type: tHex(mtActorsType), Name: int64(droneLevels[3])}), "which is not a map level's"},
		{"a level without its actor table", added[1:], "actor table 0 times"},
	} {
		if _, err := mapTermResourcesNamed(c.keys); err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: %v, want %q", c.what, err, c.want)
		}
	}
}

func TestTheActorTableRoundTripsAndKeepsItsGroups(t *testing.T) {
	b := tActorTable(mapTermLevels[0], stockOpts())
	at, err := mtReadActorTable(b)
	if err != nil {
		t.Fatal(err)
	}
	if len(at.actors) != 7 || len(at.aliases) != 1 {
		t.Errorf("%d actors, %d aliases", len(at.actors), len(at.aliases))
	}
	again, err := at.write()
	if err != nil || !bytes.Equal(again, b) {
		t.Errorf("the table does not round trip (%v)", err)
	}
	if _, err := mtReadActorTable(b[:len(b)-1]); err == nil {
		t.Error("a table one byte short was read")
	}
}

func TestTheTerminalGetsThreeMapButtonsPerLevelAndSmallerStockButtons(t *testing.T) {
	p := tPack(t, tStock)
	if err := mapTerminal(p); err != nil {
		t.Fatal(err)
	}
	if n := len(p.changes[Sets[0]]); n != tResourcesPerLevel*mapTermLevelCount {
		t.Errorf("%d resources changed, want %d", n, tResourcesPerLevel*mapTermLevelCount)
	}
	for _, l := range mapTermLevels {
		get := func(typ string) []byte {
			b, ok := p.changes[Sets[0]][radpack.Key{Type: tHex(typ), Name: int64(l.level)}]
			if !ok {
				t.Fatalf("level %016x: %s was not changed", l.level, typ)
			}
			return b
		}
		if _, ok := p.changes[Sets[0]][radpack.Key{Type: tHex(tUnrelatedType), Name: int64(l.level)}]; ok {
			t.Errorf("level %016x: a table that names no subtree actor was changed", l.level)
		}
		table, err := mtReadActorTable(get(mtActorsType))
		if err != nil {
			t.Fatal(err)
		}
		const parts, shownCount = 4, 3
		wantTr, _ := mtWalk("CTransformCR", get("92abd3e1432bf5e8"))
		for _, m := range mapTermMaps {
			has := containsU64(wantTr.owners, csymbol64("mapswitch_"+m+"_0"))
			if has == (m == l.own) {
				t.Errorf("level %016x: transform row for the %s button: present %v", l.level, m, has)
			}
		}
		for k, o := range wantTr.owners {
			if o == csymbol64("mapswitch_"+mapTermMaps[len(mapTermMaps)-1]+"_0") || o == csymbol64("mapswitch_combustion_0") {
				// a copy's CTransformCR init_position moved with the copy: its root is at the middle + (x, MAP_Y)
				if got := mtGetVec(get("92abd3e1432bf5e8")[wantTr.rows[k][0]:], mtTransformPosAt); got[1] < 1+wantMapY-tTolerance || got[1] > 1+wantMapY+tTolerance {
					t.Errorf("copy transform row y %v, want %v", got[1], 1+wantMapY)
				}
			}
		}
		if len(table.actors) != 7+parts*shownCount {
			t.Fatalf("level %016x: %d actors", l.level, len(table.actors))
		}
		idx := map[uint64]int{}
		for i, a := range table.actors {
			idx[a.nodeid] = i
		}
		k := 0
		for _, m := range mapTermMaps {
			if m == l.own {
				if _, ok := idx[csymbol64("mapswitch_"+m+"_0")]; ok {
					t.Errorf("level %016x: its own map %s got a button", l.level, m)
				}
				continue
			}
			root := idx[csymbol64("mapswitch_"+m+"_0")]
			if root == 0 {
				t.Fatalf("level %016x: no button for %s", l.level, m)
			}
			// middle (0.2, 1, 2) + (x, MAP_Y, 0); the quaternion is the identity
			tNear(t, m+" root", mtGetVec(table.actors[root].transform, mtPosAt), [3]float64{0.2 + wantX[k], 1 + wantMapY, 2})
			if got := table.actors[root].parent; got != 0 {
				t.Errorf("%s root keeps the layout parent, got %d", m, got)
			}
			press := idx[csymbol64("mapswitch_"+m+"_1")]
			if int(table.actors[press].parent) != root {
				t.Errorf("%s press part's parent %d, want its own root %d", m, table.actors[press].parent, root)
			}
			sc := mtGetVec(table.actors[root].transform, mtScaleAt)
			tNear(t, m+" scale", sc, [3]float64{wantMapScale, wantMapScale, wantMapScale})
			k++
		}
		// REMATCH at (-0.07, STOCK_Y) of the middle, LOBBY at (0.07, STOCK_Y), both at STOCK_SCALE.
		tNear(t, "rematch", mtGetVec(table.actors[idx[l.rematch]].transform, mtPosAt), [3]float64{0.2 - 0.07, 1 + wantStockY, 2})
		tNear(t, "lobby", mtGetVec(table.actors[idx[l.lobby]].transform, mtPosAt), [3]float64{0.2 + 0.07, 1 + wantStockY, 2})
		tNear(t, "rematch scale", mtGetVec(table.actors[idx[l.rematch]].transform, mtScaleAt), [3]float64{0.5, 0.5, 0.5})
		// a child keeps its offset from the pivot, halved: press part at +0.01 z over REMATCH
		tNear(t, "rematch child", mtGetVec(table.actors[idx[tNodePress]].transform, mtPosAt), [3]float64{0.2 - 0.07, 1 + wantStockY, 2.005})

		s, err := mtWalk("CScriptCR", get(mapTermScript))
		if err != nil {
			t.Fatal(err)
		}
		if len(s.owners) != 3+shownCount {
			t.Fatalf("script rows %d", len(s.owners))
		}
		for _, m := range mapTermMaps {
			if m == l.own {
				continue
			}
			btn, err := mapTermScriptRows(get(mapTermScript), csymbol64("mapswitch_"+m+"_0"))
			if err != nil {
				t.Fatal(err)
			}
			if len(btn.press) != 1 || btn.press[0] != csymbol64("mapswitch_"+m) || len(btn.labels) != 1 || btn.labels[0] != wantLabels[m] {
				t.Errorf("%s button sends %x labelled %q", m, btn.press, btn.labels)
			}
		}
		orig, _ := mapTermScriptRows(get(mapTermScript), l.rematch)
		if orig.press[0] != mapTermLeftPress || orig.labels[0] != mapTermStockLabel {
			t.Errorf("REMATCH's own rows changed: %x %q", orig.press, orig.labels)
		}
		// component rows: one per copy, owners remapped, positions moved
		tr, _ := mtWalk("CTransformCR", get("92abd3e1432bf5e8"))
		if len(tr.owners) != 4+2*shownCount { // two subtree owners per copy: REMATCH and its press part
			t.Errorf("transform rows %d", len(tr.owners))
		}
		// CR15ButtonInteractCR: a copy's pose at +0x80 moves with it, and REMATCH's own pose moves to its new place.
		bt, err := mtWalk("CR15ButtonInteractCR", get("e9b24ea816dece48"))
		if err != nil {
			t.Fatal(err)
		}
		for k, o := range bt.owners {
			pose := mtGetVec(get("e9b24ea816dece48"), bt.rows[k][0]+mtButtonPosAt)
			switch o {
			case l.rematch:
				tNear(t, "rematch button pose", pose, [3]float64{0.2 - 0.07, 1 + wantStockY, 2})
			case csymbol64("mapswitch_" + mapTermMaps[len(mapTermMaps)-1] + "_0"):
				if l.own != "gauss" {
					tNear(t, "gauss button pose", pose, [3]float64{0.2 + wantX[len(wantX)-1], 1 + wantMapY, 2})
				}
			}
		}
		im, err := mtReadInstanceModels(get(mtInstanceType))
		if err != nil || len(im.slots[mtInstOwnerRow]) != 2+shownCount {
			t.Fatalf("instance records %v %v", len(im.slots[mtInstOwnerRow]), err)
		}
		if again := im.write(); !bytes.Equal(again, get(mtInstanceType)) {
			t.Error("instance models do not round trip")
		}
	}
}

func TestAStockLevelThatDiffersIsRefusedBeforeAnythingIsWritten(t *testing.T) {
	for _, c := range []struct {
		name string
		opts fixOpts
		want []string
	}{
		{"already patched", fixOpts{alreadyPatched: true, press: mapTermLeftPress, label: mapTermStockLabel}, []string{mtActorsType, "mapswitch_dyson_0", "expected absent", "actual present"}},
		{"no lobby button", fixOpts{noLobby: true, press: mapTermLeftPress, label: mapTermStockLabel}, []string{mtActorsType, "LOBBY button actor", "expected actor", "actual absent"}},
		{"another press event", fixOpts{press: 0x1234, label: mapTermStockLabel}, []string{mapTermScript, "REMATCH press event", fmt.Sprintf("%016x", uint64(mapTermLeftPress)), "1234"}},
		{"another label", fixOpts{press: mapTermLeftPress, label: "REMATCH"}, []string{mapTermScript, "REMATCH label", `"LEFT"`, "REMATCH"}},
	} {
		p := tPack(t, func(mapTermLevel) fixOpts { return c.opts })
		err := mapTerminal(p)
		if err == nil {
			t.Errorf("%s: a differing level was changed", c.name)
			continue
		}
		for _, w := range c.want {
			if !strings.Contains(err.Error(), w) {
				t.Errorf("%s: error %q lacks %q", c.name, err, w)
			}
		}
		if len(p.changes[Sets[0]]) != 0 {
			t.Errorf("%s: %d resources were written before the refusal", c.name, len(p.changes[Sets[0]]))
		}
	}
}

func TestTheStepIsRegisteredAndNamed(t *testing.T) {
	for _, s := range steps {
		if s.Name == mapTermStepName {
			return
		}
	}
	t.Errorf("no step named %q", mapTermStepName)
}

// The values his tool sets, written out here so the step's constants are checked against them (fix_map_terminal.py:42-47).
var (
	wantX      = []float64{-0.125, 0, 0.125}
	wantLabels = map[string]string{"dyson": "DYSON", "combustion": "COMBUSTION", "fission": "FISSION", "gauss": "SURGE"}
)

const (
	wantMapY, wantMapScale, wantStockY = 0.055, 0.7, -0.085
)
