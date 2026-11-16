package main

import (
	"bytes"
	"fmt"
	"math"
	"strconv"
	"sync"
)

// The post-match terminal (PLAN-v5 inventory row 30, G-MAPTERM). The celebration room's podium sits in each Combat level's
// own data. he_is_the_cat's tools/fix_map_terminal.py (nevr-pack 82ad7ba) copies the REMATCH button's four-actor subtree
// three times into a row of smaller map buttons (every Combat map but the level's own), shrinks REMATCH and LOBBY into a
// row under them, and gives each copy the press event mapswitch_<map> and the map's name as its label. What a press does
// is VeilNet's (src/runtime/combat/veilnet.cpp, MAP SWITCH); this step writes data records only and touches no script.
//
// The step reads the player's stock level tables (Pack.Read), checks what it expects of them first (the actors it moves, no
// map button yet, REMATCH's press event and label) and refuses, naming record, field, expected and actual, when they
// differ. Base: stock plus the drone's script row (steps_drone.go rule 3); his tool edits the gun patch's copy of the
// script table, which S-BASE 1 shows is stock plus that rule. The step takes no value of the gun patch.
//
// Fields written, per level (the declared fields, Step.Writes, are below the list: mapTermWrites):
//   - the actor table 347869ce492dc7da: three copies of the REMATCH subtree appended (nodeid, name, prefab, transform,
//     parent), and the transforms of the REMATCH and LOBBY subtrees (position at +16, scale at +28);
//   - every component table that owns a row of the REMATCH subtree gains one row per copy (owner and the words that name
//     a subtree actor remapped; CTransformCR +0x30 and +0x3C, CR15ButtonInteractCR +0x80 moved with the copy), and the
//     CTransformCR and CR15ButtonInteractCR rows of the REMATCH and LOBBY subtrees are moved;
//   - CScriptCR d99f6bbd8009c92c: each copy's press event (table 5, row +16) and label (table 8, row +16, 64 bytes).
//
// Levels are the four Combat maps. mpl_lobby_b_combat is not one of them (his LEVELS, fix_map_terminal.py:26-31).
const (
	mapTermStepName = "post-match terminal gets three map buttons"
	// his LEVELS comment: each level's only prp_itt_screen_button_logic pair sending left_ / right_button_press.
	mapTermLeftPress   = 0x052C36430F4094CD // LEFT_BUTTON_PRESS, fix_map_terminal.py:57
	mapTermPressTable  = 5                  // PRESS_TABLE, fix_map_terminal.py:56
	mapTermLabelTable  = 8                  // LABEL_TABLE, fix_map_terminal.py:56
	mapTermPressStride = 24                 // a press row holds its symbol at +16 (fix_map_terminal.py:49-52)
	mapTermSymbolAt    = 16
	mapTermLabelStride = 80 // a label row: a 64-byte string at +16 (fix_map_terminal.py:49-52)
	mapTermLabelLen    = 64
	mapTermStockLabel  = "LEFT"
	mapTermLevelCount  = 4
	mapTermMapY        = 0.055  // MAP_Y, fix_map_terminal.py:45
	mapTermMapScale    = 0.7    // MAP_SCALE, fix_map_terminal.py:45
	mapTermStockY      = -0.085 // STOCK_Y, fix_map_terminal.py:46
	mapTermStockScale  = 0.5    // STOCK_SCALE, fix_map_terminal.py:46
	mapTermTypeBase    = 16     // a type is written as 16 hex digits (mtTypeNames)
	mapTermTypeBits    = 64
)

// mapTermMaps is MAPS (fix_map_terminal.py:37); mapTermLabels is LABELS (:44): Gauss is called SURGE in game.
var (
	// mapTermScript is CScriptCR's type, steps_drone.go's scriptTable, as the level-file key the readers use.
	mapTermScript = mtTypeKey(scriptTable)
	mapTermMaps   = []string{"dyson", "combustion", "fission", "gauss"}
	mapTermLabels = map[string]string{"dyson": "DYSON", "combustion": "COMBUSTION", "fission": "FISSION", "gauss": "SURGE"}
	// mapTermX is MAP_X (:42): three buttons across.
	mapTermX = []float64{-0.125, 0.0, 0.125}
	// mapTermStockX is STOCK_X (:47).
	mapTermStockX = map[string]float64{"rematch": -0.07, "lobby": 0.07}
)

// mapTermLevel is one row of his LEVELS (fix_map_terminal.py:26-31) with LEVEL_MAP (:39-40): the level, its own map
// (REMATCH replays it, so it gets no button), the button to copy (REMATCH) and the other stock button (LOBBY).
type mapTermLevel struct {
	level, rematch, lobby uint64
	own                   string
}

var mapTermLevels = [mapTermLevelCount]mapTermLevel{
	{0x43e2da7914642604, 0xA33EA7534AD8A095, 0x4F4012B16746E848, "dyson"},      // mpl_combat_dyson
	{0x42670f2bed45703c, 0x7D982FF3D83BC503, 0x91E69A11F5A58DDE, "combustion"}, // mpl_combat_combustion
	{0xdf5ca7b7dfa383d4, 0x3330E9CA0147CC25, 0xDF4E5C282CD984F8, "fission"},    // mpl_combat_fission
	{0x43e2da7a0c623a19, 0x075E6ED4E2BE6885, 0xEB20DB36CF202058, "gauss"},      // mpl_combat_gauss
}

func init() {
	// After the FIELD transform and the drone script: the transform and script tables of the four map levels are records
	// both steps write.
	steps = append(steps, Step{Name: mapTermStepName, Apply: mapTerminal, PastGunPatch: true, Writes: mapTermWrites(),
		After: []string{dotFieldTransformStep, droneScriptStep}})
}

// mapTermWrites declares what the step adds, in each of the four map levels: the map buttons' actors and the REMATCH and
// LOBBY subtrees' actor transforms (readMapTermActors), the buttons' press events and labels in CScriptCR
// (readMapTermScript), and the rows the buttons' actors own in each component table of actor_copy.py's TYPE_NAMES
// (readMapTermRows; CInstanceModelCR through its own reader). The REMATCH and LOBBY rows the step moves inside the
// component tables are not a declared field: their owners are those subtrees' child actors, which only the actor table
// names, and a reader is given one record.
func mapTermWrites() []Field {
	var levels []uint64
	for _, l := range mapTermLevels {
		levels = append(levels, l.level)
	}
	// optional: the field is changed only when the stock data holds a button's row in that table (Field.Optional).
	field := func(name string, typ string, read func([]byte) ([]byte, error), optional bool) Field {
		// A type that does not parse is Type 0, which the declared fields' well-formedness test refuses.
		t, _ := strconv.ParseUint(typ, mapTermTypeBase, mapTermTypeBits)
		return Field{Name: name, Set: Sets[0], Type: t, Records: levels, Read: read, Optional: optional}
	}
	out := []Field{
		field("map button actors and the REMATCH and LOBBY transforms", mtActorsType, readMapTermActors, false),
		field("map buttons' press events and labels", mapTermScript, readMapTermScript, false),
	}
	for _, t := range mtSortedNames() {
		name := mtTypeNames[t]
		switch t {
		case mapTermScript:
			continue
		case mtInstanceType:
			out = append(out, field("map buttons' "+name+" records", t, readMapTermInstanceRows, true))
		default:
			out = append(out, field("map buttons' "+name+" rows", t, readMapTermRows(name), true))
		}
	}
	return out
}

// mapTermButtonIds are the ids a map button's actors take, mapswitch_<map>_<part> (fix_map_terminal.py:121), for every
// part an actor table can hold: a reader is given one record, and only the actor table says how many parts a button has.
var mapTermButtonIds = sync.OnceValue(func() map[uint64]bool {
	ids := make(map[uint64]bool, len(mapTermMaps)*mtMaxActors)
	for _, m := range mapTermMaps {
		for j := 0; j < mtMaxActors; j++ {
			ids[csymbol64(fmt.Sprintf("mapswitch_%s_%d", m, j))] = true
		}
	}
	return ids
})

// readMapTermActors reads, from an actor table, each map button's actors (nodeid, name, prefab, parent, transform) and
// the transforms of the level's REMATCH and LOBBY subtrees, in table order.
func readMapTermActors(record []byte) ([]byte, error) {
	table, err := mtReadActorTable(record)
	if err != nil {
		return nil, err
	}
	stock := map[uint64]bool{}
	for _, l := range mapTermLevels {
		stock[l.rematch], stock[l.lobby] = true, true
	}
	moved := map[int]bool{}
	for i, a := range table.actors {
		if stock[a.nodeid] {
			for _, k := range table.subtree(i) {
				moved[k] = true
			}
		}
	}
	ids := mapTermButtonIds()
	var out []byte
	for i, a := range table.actors {
		switch {
		case ids[a.nodeid]:
			out = le.AppendUint64(out, a.nodeid)
			out = le.AppendUint64(out, a.name)
			out = le.AppendUint64(out, a.prefabid)
			out = le.AppendUint16(out, a.parent)
			out = append(out, a.transform...)
		case moved[i]:
			out = le.AppendUint64(out, a.nodeid)
			out = append(out, a.transform...)
		}
	}
	return out, nil
}

// readMapTermScript reads, from CScriptCR, each map button's press events and its labels (64 bytes each), in map order.
func readMapTermScript(record []byte) ([]byte, error) {
	s, err := mtWalk("CScriptCR", record)
	if err != nil {
		return nil, err
	}
	owned := map[uint64]bool{}
	for _, o := range s.owners {
		owned[o] = true
	}
	var out []byte
	for _, m := range mapTermMaps {
		root := csymbol64("mapswitch_" + m + "_0")
		if !owned[root] {
			continue
		}
		btn, err := mapTermScriptRows(record, root)
		if err != nil {
			return nil, err
		}
		for _, e := range btn.press {
			out = le.AppendUint64(out, e)
		}
		for _, at := range btn.labelAt {
			out = append(out, record[at:at+mapTermLabelLen]...)
		}
	}
	return out, nil
}

// readMapTermRows reads, from a component table, the rows (and their nested data) that a map button's actors own.
func readMapTermRows(typeName string) func([]byte) ([]byte, error) {
	return func(record []byte) ([]byte, error) {
		s, err := mtWalk(typeName, record)
		if err != nil {
			return nil, err
		}
		ids := mapTermButtonIds()
		var out []byte
		for k, o := range s.owners {
			if ids[o] {
				out = append(out, record[s.rows[k][0]:s.rows[k][1]]...)
				out = append(out, record[s.nested[k][0]:s.nested[k][1]]...)
			}
		}
		return out, nil
	}
}

// readMapTermInstanceRows reads, from CInstanceModelCR, the per-record rows of each record a map button's actor owns.
func readMapTermInstanceRows(record []byte) ([]byte, error) {
	m, err := mtReadInstanceModels(record)
	if err != nil {
		return nil, err
	}
	ids := mapTermButtonIds()
	var out []byte
	for k, o := range m.owners() {
		if ids[o] {
			for _, sl := range mtInstPerRecord {
				out = append(out, m.slots[sl][k]...)
			}
		}
	}
	return out, nil
}

func mapTerminal(p *Pack) error {
	for _, l := range mapTermLevels {
		if err := mapTerminalLevel(p, l); err != nil {
			return fmt.Errorf("level %016x: %w", l.level, err)
		}
	}
	return nil
}

// mapTermFiles is actor_copy.py:172-179 level_files: every table of the level, the pack's current bytes.
func mapTermFiles(p *Pack, level uint64) (map[string][]byte, error) {
	types := map[int64]bool{}
	if s := p.sets[Sets[0]]; s != nil {
		for _, r := range s.Manifest.Resources {
			if r.Name == int64(level) {
				types[r.Type] = true
			}
		}
	}
	for k := range p.changes[Sets[0]] {
		if k.Name == int64(level) {
			types[k.Type] = true
		}
	}
	files := map[string][]byte{}
	for typ := range types {
		b, err := p.Read(Sets[0], typ, int64(level))
		if err != nil {
			return nil, err
		}
		files[fmt.Sprintf("%016x", uint64(typ))] = b
	}
	return files, nil
}

// mapTermButton is what a button's CScriptCR rows hold: where each press event and label is, and their values.
type mapTermButton struct {
	pressAt, labelAt []int
	press            []uint64
	labels           []string
}

// mapTermScriptRows reads the press events (table 5) and labels (table 8) of the rows `owner` owns
// (fix_map_terminal.py:73-98 set_press_and_label). The first row of the owner is the one his tool reads.
func mapTermScriptRows(b []byte, owner uint64) (*mapTermButton, error) {
	s, err := mtWalk("CScriptCR", b)
	if err != nil {
		return nil, err
	}
	k := -1
	for i, o := range s.owners {
		if o == owner {
			k = i
			break
		}
	}
	if k < 0 {
		return nil, fmt.Errorf("CScriptCR %s: expected a row owned by %016x, actual none", mapTermScript, owner)
	}
	out := &mapTermButton{}
	cursor := s.nested[k][0]
	for t := 0; t < mtScriptTables; t++ {
		d, err := mtCRDesc(b, s.rows[k][0]+mtScriptTablesAt+mtDescLen*t)
		if err != nil {
			return nil, err
		}
		if d.count == 0 {
			continue
		}
		cursor = mtAlignUp(cursor, max(int(d.align), mtWordLen))
		stride := d.stride()
		for j := 0; j < int(d.count); j++ {
			at := cursor + j*stride
			switch t {
			case mapTermPressTable:
				if stride < mapTermPressStride {
					return nil, fmt.Errorf("CScriptCR %s owner %016x table %d: rows of %d bytes, expected at least %d", mapTermScript, owner, t, stride, mapTermPressStride)
				}
				out.pressAt = append(out.pressAt, at+mapTermSymbolAt)
				out.press = append(out.press, le.Uint64(b[at+mapTermSymbolAt:]))
			case mapTermLabelTable:
				if stride < mapTermLabelStride {
					return nil, fmt.Errorf("CScriptCR %s owner %016x table %d: rows of %d bytes, expected at least %d", mapTermScript, owner, t, stride, mapTermLabelStride)
				}
				out.labelAt = append(out.labelAt, at+mapTermSymbolAt)
				out.labels = append(out.labels, string(bytes.TrimRight(b[at+mapTermSymbolAt:at+mapTermSymbolAt+mapTermLabelLen], "\x00")))
			}
		}
		cursor += int(d.count) * stride
	}
	return out, nil
}

// mapTermStockCheck refuses a level whose records are not the stock ones his tool expects, before anything is written.
func mapTermStockCheck(files map[string][]byte, l mapTermLevel) (*mtActorTable, map[uint64]int, error) {
	raw, ok := files[mtActorsType]
	if !ok {
		return nil, nil, fmt.Errorf("record %s: expected the level's actor table, actual absent", mtActorsType)
	}
	table, err := mtReadActorTable(raw)
	if err != nil {
		return nil, nil, fmt.Errorf("record %s: %w", mtActorsType, err)
	}
	index := map[uint64]int{}
	for i, a := range table.actors {
		index[a.nodeid] = i
	}
	for _, b := range []struct {
		field string
		id    uint64
	}{{"REMATCH button actor", l.rematch}, {"LOBBY button actor", l.lobby}} {
		if _, ok := index[b.id]; !ok {
			return nil, nil, fmt.Errorf("record %s field %s: expected actor %016x, actual absent", mtActorsType, b.field, b.id)
		}
	}
	for _, m := range mapTermMaps {
		if _, ok := index[csymbol64("mapswitch_"+m+"_0")]; ok {
			return nil, nil, fmt.Errorf("record %s field actor mapswitch_%s_0: expected absent (a stock level), actual present (already patched)", mtActorsType, m)
		}
	}
	script, ok := files[mapTermScript]
	if !ok {
		return nil, nil, fmt.Errorf("record %s: expected the level's script table, actual absent", mapTermScript)
	}
	btn, err := mapTermScriptRows(script, l.rematch)
	if err != nil {
		return nil, nil, err
	}
	if !containsU64(btn.press, mapTermLeftPress) {
		return nil, nil, fmt.Errorf("record %s field REMATCH press event (table %d, row +%d): expected %016x, actual %x", mapTermScript, mapTermPressTable, mapTermSymbolAt, uint64(mapTermLeftPress), btn.press)
	}
	if !containsStr(btn.labels, mapTermStockLabel) {
		return nil, nil, fmt.Errorf("record %s field REMATCH label (table %d, row +%d): expected %q, actual %q", mapTermScript, mapTermLabelTable, mapTermSymbolAt, mapTermStockLabel, btn.labels)
	}
	return table, index, nil
}

func containsU64(l []uint64, v uint64) bool {
	for _, x := range l {
		if x == v {
			return true
		}
	}
	return false
}

func containsStr(l []string, v string) bool {
	for _, x := range l {
		if x == v {
			return true
		}
	}
	return false
}

// mapTermSetPressAndLabel is fix_map_terminal.py:73-98: the copy's script rows send mapswitch_<map> and carry its label.
func mapTermSetPressAndLabel(b []byte, owner, event uint64, label string) ([]byte, error) {
	btn, err := mapTermScriptRows(b, owner)
	if err != nil {
		return nil, err
	}
	out := append([]byte(nil), b...)
	pressed, labelled := false, false
	for i, at := range btn.pressAt {
		if btn.press[i] == mapTermLeftPress {
			le.PutUint64(out[at:], event)
			pressed = true
		}
	}
	for i, at := range btn.labelAt {
		if btn.labels[i] == mapTermStockLabel {
			copy(out[at:at+mapTermLabelLen], make([]byte, mapTermLabelLen))
			copy(out[at:at+mapTermLabelLen], label)
			labelled = true
		}
	}
	if !pressed || !labelled {
		return nil, fmt.Errorf("copy %016x: press event found %v, label found %v (expected both)", owner, pressed, labelled)
	}
	return out, nil
}

// mapTermRotate is fix_map_terminal.py:59-66: v by the unit quaternion q = (x, y, z, w).
func mapTermRotate(q [4]float64, v [3]float64) [3]float64 {
	x, y, z, w := q[0], q[1], q[2], q[3]
	tx := 2 * (y*v[2] - z*v[1])
	ty := 2 * (z*v[0] - x*v[2])
	tz := 2 * (x*v[1] - y*v[0])
	return [3]float64{v[0] + w*tx + (y*tz - z*ty), v[1] + w*ty + (z*tx - x*tz), v[2] + w*tz + (x*ty - y*tx)}
}

func mapTerminalLevel(p *Pack, l mapTermLevel) error {
	files, err := mapTermFiles(p, l.level)
	if err != nil {
		return err
	}
	table, index, err := mapTermStockCheck(files, l)
	if err != nil {
		return err
	}
	rematch, lobby := index[l.rematch], index[l.lobby]
	var q [4]float64
	for i := range q {
		q[i] = float64(math.Float32frombits(le.Uint32(table.actors[rematch].transform[i*mtF32Len:])))
	}
	pos := map[int][3]float64{}
	for _, i := range []int{rematch, lobby} {
		pos[i] = mtGetVec(table.actors[i].transform, mtPosAt)
	}
	var middle [3]float64
	for i := range middle {
		middle[i] = (pos[rematch][i] + pos[lobby][i]) / 2
	}
	world := func(x, y float64) [3]float64 {
		r := mapTermRotate(q, [3]float64{x, y, 0})
		return [3]float64{middle[0] + r[0], middle[1] + r[1], middle[2] + r[2]}
	}
	offset := func(i int, x, y float64) [3]float64 {
		w := world(x, y)
		return [3]float64{w[0] - pos[i][0], w[1] - pos[i][1], w[2] - pos[i][2]}
	}
	parts := len(table.subtree(rematch))
	var shown []string
	for _, m := range mapTermMaps {
		if m != l.own {
			shown = append(shown, m)
		}
	}
	var copies []mtCopy
	for k, m := range shown {
		ids := make([]uint64, parts)
		for j := range ids {
			ids[j] = csymbol64(fmt.Sprintf("mapswitch_%s_%d", m, j))
		}
		copies = append(copies, mtCopy{ids, mtPlace{pivot: pos[rematch], offset: offset(rematch, mapTermX[k], mapTermMapY), scale: mapTermMapScale}})
	}
	changed, err := mtCopySubtree(files, rematch, copies)
	if err != nil {
		return err
	}
	apply := func(c map[string][]byte) {
		for t, b := range c {
			files[t], changed[t] = b, b
		}
	}
	apply(changed)
	script := files[mapTermScript]
	for _, m := range shown {
		if script, err = mapTermSetPressAndLabel(script, csymbol64("mapswitch_"+m+"_0"), csymbol64("mapswitch_"+m), mapTermLabels[m]); err != nil {
			return err
		}
	}
	files[mapTermScript], changed[mapTermScript] = script, script
	for _, b := range []struct {
		i   int
		key string
	}{{rematch, "rematch"}, {lobby, "lobby"}} {
		moved, err := mtMoveSubtree(files, b.i, mtPlace{pivot: pos[b.i], offset: offset(b.i, mapTermStockX[b.key], mapTermStockY), scale: mapTermStockScale})
		if err != nil {
			return err
		}
		apply(moved)
	}
	for _, t := range mtSortedTypes(changed) {
		var typ uint64
		if _, err := fmt.Sscanf(t, "%016x", &typ); err != nil {
			return fmt.Errorf("type %q: %w", t, err)
		}
		if err := p.Put(Sets[0], int64(typ), int64(l.level), changed[t]); err != nil {
			return err
		}
	}
	return nil
}
