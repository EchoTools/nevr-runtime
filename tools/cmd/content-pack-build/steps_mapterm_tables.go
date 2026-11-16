package main

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"math"
	"sort"
)

// The level tables the post-match terminal step edits: a level's actor table (type 347869ce492dc7da), its component
// tables (each a 56-byte descriptor, rows owned by the actor nodeid at row +8, then each row's nested tables), and its
// CInstanceModelCR. The layouts are he_is_the_cat's readers, ported (the layout facts are his; no byte of his data is
// here), each citing its Python source at nevr-pack 82ad7ba:
//
//	actor table   tools/level_actor_table.py (HEADER, DESC_AT, SHAPES, Desc, ActorTable.read/write)
//	CR walker     tools/cr_rows.py (Desc, skip_table, spans, NESTED)
//	instance      tools/instance_model_cr.py (SLOTS, InstanceModels)
//	copy / move   tools/actor_copy.py (Place, shift, remap_words, subtree, append_flat, copy_subtree, move_subtree)
//
// Every reader refuses a table whose walk does not end exactly at its last byte, and says what it expected.

const (
	mtDescLen      = 56  // a RadTable descriptor: p, size, alloc, align, flags, grow, capacity, count
	mtWordLen      = 8   // a nodeid, and the alignment floor of every component table
	mtOwnerAt      = 8   // a component row's owning actor nodeid (cr_rows.py:96)
	mtDescSizeAt   = 8   // a descriptor's byte size (actor_copy.py:122)
	mtDescAlignAt  = 24  // a descriptor's alignment word (instance_model_cr.py:77)
	mtDescCapAt    = 40  // a descriptor's capacity (actor_copy.py:123)
	mtDescCountAt  = 48  // a descriptor's count (actor_copy.py:125)
	mtActorHeader  = 784 // level_actor_table.py:HEADER
	mtGroupLen     = 64  // a prefab group: the prefab id and its inline CTable<u16> descriptor
	mtTransformLen = 48  // an actor's transform: quaternion, position, scale, two pad words
	mtQuatLen      = 16  // the quaternion (x, y, z, w) at the start of a transform
	mtPosAt        = 16  // an actor transform's position (actor_copy.py:90)
	mtScaleAt      = 28  // an actor transform's scale (actor_copy.py:90)
	mtBitSets      = 5   // the actor table's five bitfields
	mtBitWord      = 64  // actors per bitfield word
	mtActorSlots   = 13  // the actor table's descriptors
	mtMaxActors    = 1 << 16
	mtU16Len       = 2
	mtU64Len       = 8
	mtF32Len       = 4
	mtVecLen       = 12 // three f32
	mtFlagsDesc    = 1  // level_actor_table.py:Desc.read requires flags == 1
	mtInstHeader   = 568
	mtInstBitCount = 112
	mtInstOwnerRow = 9 // slot 9 (properties) holds the record's owner at +8
	mtInstLookup   = 0
	mtInstBits     = 1
)

var (
	mtDescAt     = [mtActorSlots]int{0, 64, 128, 184, 240, 296, 352, 408, 464, 528, 592, 656, 720}
	mtBitCountAt = [mtBitSets]int{520, 584, 648, 712, 776}
	// mtShapes are (element size, element alignment) of each actor table descriptor (level_actor_table.py:SHAPES).
	mtShapes = [mtActorSlots][2]int{{16, 8}, {64, 8}, {8, 8}, {8, 8}, {8, 8}, {48, 4}, {2, 2}, {2, 2},
		{8, 8}, {8, 8}, {8, 8}, {8, 8}, {8, 8}}
	// mtInstSlots are (descriptor offset, row size, element alignment) (instance_model_cr.py:SLOTS).
	mtInstSlots = [][3]int{{0, 24, 8}, {56, 8, 8}, {120, 8, 8}, {176, 2, 2}, {232, 8, 8}, {288, 16, 4},
		{344, 2, 2}, {400, 2, 2}, {456, 4, 4}, {512, 72, 8}}
	// mtInstPerRecord are the slots with one row per record (instance_model_cr.py:PER_RECORD).
	mtInstPerRecord = []int{0, 3, 4, 5, 6, 7, 8, 9}
)

var le = binary.LittleEndian

func mtAlignUp(n, a int) int {
	if a <= 1 {
		return n
	}
	return (n + a - 1) / a * a
}

// mtDesc is a RadTable descriptor (`<QQQIIQQQ`).
type mtDesc struct {
	size, alloc  uint64
	align, flags uint32
	grow         uint64
	capacity     uint64
	count        uint64
	ptr          uint64
}

func mtReadDesc(b []byte, at int) (mtDesc, error) {
	if at < 0 || at+mtDescLen > len(b) {
		return mtDesc{}, fmt.Errorf("no table descriptor at %#x: the table ends at %#x", at, len(b))
	}
	d := mtDesc{ptr: le.Uint64(b[at:]), size: le.Uint64(b[at+8:]), alloc: le.Uint64(b[at+16:]), align: le.Uint32(b[at+24:]),
		flags: le.Uint32(b[at+28:]), grow: le.Uint64(b[at+32:]), capacity: le.Uint64(b[at+40:]), count: le.Uint64(b[at+48:])}
	if d.ptr != 0 || d.alloc != 0 || d.capacity < d.count {
		return mtDesc{}, fmt.Errorf("no table descriptor at %#x (pointer %#x, alloc %#x, capacity %d, count %d)", at, d.ptr, d.alloc, d.capacity, d.count)
	}
	return d, nil
}

func (d mtDesc) pack() []byte {
	out := make([]byte, mtDescLen)
	le.PutUint64(out[8:], d.size)
	le.PutUint32(out[24:], d.align)
	le.PutUint32(out[28:], d.flags)
	le.PutUint64(out[32:], d.grow)
	le.PutUint64(out[40:], d.capacity)
	le.PutUint64(out[48:], d.count)
	return out
}

func (d mtDesc) stride() int {
	if d.capacity == 0 {
		return 0
	}
	return int(d.size / d.capacity)
}

// payloadAlign is level_actor_table.py:Desc.payload_align.
func (d mtDesc) payloadAlign(elem int) int {
	if d.count == 0 {
		return 1
	}
	return max(int(d.align), elem)
}

// csymbol64 is CSymbol64, the engine's case-folded 64-bit name hash: sdk/include/nevr/symbol_hash.h's CSymbol64Hash,
// whose table entry i is the carry-less product of the polynomial and 2i and whose byte is sign-extended before it is
// mixed in (the header's (byte ^ 0x80) - 0x80). It is the package's one copy: steps_mapterm.go computes the map buttons'
// ids with it at build time, so it is production code, and the tests of the other steps check their ids with it.
// tools/symcache/csymbol64.go is a `main` package, so it cannot be imported.
func csymbol64(name string) uint64 {
	const (
		poly      = 0x95ac9329ac4bc9b5
		tableLen  = 256
		byteBits  = 8
		highShift = 64 - byteBits
		seed      = ^uint64(0)
	)
	var table [tableLen]uint64
	for i := range table {
		a, b := uint64(poly), uint64(i)<<1
		var r uint64
		for ; b != 0; a, b = a<<1, b>>1 {
			if b&1 != 0 {
				r ^= a
			}
		}
		table[i] = r
	}
	h := seed
	for i := 0; i < len(name); i++ {
		c := name[i]
		if c >= 'A' && c <= 'Z' {
			c += 'a' - 'A'
		}
		h = h<<byteBits ^ table[h>>highShift] ^ uint64(int64(int8(c)))
	}
	return h
}

// ---- the actor table -------------------------------------------------------------------------------------------

type mtActor struct {
	nodeid, name, prefabid uint64
	transform              []byte
	numpooled, parent      uint16
	bits                   [mtBitSets]bool
}

type mtActorTable struct {
	header        []byte
	descs         [mtActorSlots]mtDesc
	groupTemplate mtDesc
	actors        []mtActor
	aliases       [][2]uint64 // alias nodeid, target nodeid
}

const mtUnset = ^uint64(0)

func mtReadActorTable(b []byte) (*mtActorTable, error) {
	if len(b) < mtActorHeader {
		return nil, fmt.Errorf("actor table of %d bytes is shorter than its %d-byte header", len(b), mtActorHeader)
	}
	t := &mtActorTable{header: append([]byte(nil), b[:mtActorHeader]...)}
	for i, at := range mtDescAt {
		d, err := mtReadDesc(b, at)
		if err != nil {
			return nil, err
		}
		if d.flags != mtFlagsDesc {
			return nil, fmt.Errorf("actor table descriptor %d: flags %d, expected %d", i, d.flags, mtFlagsDesc)
		}
		if d.capacity != 0 && d.size != d.capacity*uint64(mtShapes[i][0]) {
			return nil, fmt.Errorf("actor table descriptor %d: stride %d, expected %d", i, d.size/d.capacity, mtShapes[i][0])
		}
		t.descs[i] = d
	}
	n := int(t.descs[2].count)
	words := (n + mtBitWord - 1) / mtBitWord
	for i := 3; i <= 7; i++ {
		if int(t.descs[i].count) != n {
			return nil, fmt.Errorf("actor table descriptor %d: count %d, expected %d actors", i, t.descs[i].count, n)
		}
	}
	for k := 0; k < mtBitSets; k++ {
		if int(t.descs[8+k].count) != words {
			return nil, fmt.Errorf("actor table descriptor %d: count %d, expected %d words", 8+k, t.descs[8+k].count, words)
		}
	}
	var at [mtActorSlots]int
	type group struct {
		inner mtDesc
		start int
	}
	var groups []group
	cursor := mtActorHeader
	for i, d := range t.descs {
		size, align := mtShapes[i][0], mtShapes[i][1]
		start := mtAlignUp(cursor, d.payloadAlign(align))
		if start > len(b) || (start > cursor && !allZero(b[cursor:start])) {
			return nil, fmt.Errorf("actor table slot %d: padding at %#x is not zero or runs past the table", i, cursor)
		}
		at[i], cursor = start, start+int(d.count)*size
		if i == 1 {
			for g := 0; g < int(d.count); g++ {
				inner, err := mtReadDesc(b, at[1]+g*mtGroupLen+mtDescSizeAt)
				if err != nil {
					return nil, err
				}
				if inner.flags != mtFlagsDesc {
					return nil, fmt.Errorf("actor table prefab group %d: flags %d, expected %d", g, inner.flags, mtFlagsDesc)
				}
				s := mtAlignUp(cursor, inner.payloadAlign(mtU16Len))
				groups = append(groups, group{inner, s})
				cursor = s + int(inner.count)*mtU16Len
			}
		}
	}
	if cursor != len(b) {
		return nil, fmt.Errorf("actor table walk ends at %#x, the table is %#x bytes", cursor, len(b))
	}
	for i := 0; i < n; i++ {
		a := mtActor{
			nodeid:    le.Uint64(b[at[2]+i*mtU64Len:]),
			name:      le.Uint64(b[at[3]+i*mtU64Len:]),
			prefabid:  le.Uint64(b[at[4]+i*mtU64Len:]),
			transform: append([]byte(nil), b[at[5]+i*mtTransformLen:at[5]+(i+1)*mtTransformLen]...),
			numpooled: le.Uint16(b[at[6]+i*mtU16Len:]),
			parent:    le.Uint16(b[at[7]+i*mtU16Len:]),
		}
		for k := 0; k < mtBitSets; k++ {
			a.bits[k] = le.Uint64(b[at[8+k]+i/mtBitWord*mtU64Len:])>>(i%mtBitWord)&1 == 1
		}
		t.actors = append(t.actors, a)
	}
	for k := 0; k < int(t.descs[0].count); k++ {
		node, index := le.Uint64(b[at[0]+k*2*mtU64Len:]), le.Uint64(b[at[0]+k*2*mtU64Len+mtU64Len:])
		if index >= uint64(n) {
			return nil, fmt.Errorf("actor table key %d names actor %d of %d", k, index, n)
		}
		if t.actors[index].nodeid != node {
			t.aliases = append(t.aliases, [2]uint64{node, t.actors[index].nodeid})
		}
	}
	t.groupTemplate = mtDesc{flags: mtFlagsDesc}
	if len(groups) > 0 {
		t.groupTemplate = groups[0].inner
	}
	return t, nil
}

func allZero(b []byte) bool {
	for _, c := range b {
		if c != 0 {
			return false
		}
	}
	return true
}

func (t *mtActorTable) write() ([]byte, error) {
	n := len(t.actors)
	if n > mtMaxActors {
		return nil, fmt.Errorf("actor table of %d actors: a parent index is 16 bits", n)
	}
	groupMembers := map[uint64][]int{}
	for i, a := range t.actors {
		if a.prefabid != mtUnset {
			groupMembers[a.prefabid] = append(groupMembers[a.prefabid], i)
		}
	}
	prefabs := make([]uint64, 0, len(groupMembers))
	for p := range groupMembers {
		prefabs = append(prefabs, p)
	}
	sort.Slice(prefabs, func(i, j int) bool { return int64(prefabs[i]) < int64(prefabs[j]) })
	index := map[uint64]int{}
	for i, a := range t.actors {
		index[a.nodeid] = i
	}
	type key struct {
		node uint64
		at   int
	}
	var keys []key
	for i, a := range t.actors {
		keys = append(keys, key{a.nodeid, i})
	}
	for _, al := range t.aliases {
		at, ok := index[al[1]]
		if !ok {
			return nil, fmt.Errorf("alias %016x names actor %016x, which the table does not have", al[0], al[1])
		}
		keys = append(keys, key{al[0], at})
	}
	sort.SliceStable(keys, func(i, j int) bool { return int64(keys[i].node) < int64(keys[j].node) })
	words := (n + mtBitWord - 1) / mtBitWord
	counts := []int{len(keys), len(prefabs), n, n, n, n, n, n}
	for k := 0; k < mtBitSets; k++ {
		counts = append(counts, words)
	}
	rows := make([][]byte, mtActorSlots)
	u64 := func(f func(a mtActor) uint64) []byte {
		var out []byte
		for _, a := range t.actors {
			out = le.AppendUint64(out, f(a))
		}
		return out
	}
	for _, k := range keys {
		rows[0] = le.AppendUint64(le.AppendUint64(rows[0], k.node), uint64(k.at))
	}
	rows[2] = u64(func(a mtActor) uint64 { return a.nodeid })
	rows[3] = u64(func(a mtActor) uint64 { return a.name })
	rows[4] = u64(func(a mtActor) uint64 { return a.prefabid })
	for _, a := range t.actors {
		rows[5] = append(rows[5], a.transform...)
		rows[6] = le.AppendUint16(rows[6], a.numpooled)
		rows[7] = le.AppendUint16(rows[7], a.parent)
	}
	for k := 0; k < mtBitSets; k++ {
		w := make([]uint64, words)
		for i, a := range t.actors {
			if a.bits[k] {
				w[i/mtBitWord] |= 1 << (i % mtBitWord)
			}
		}
		for _, x := range w {
			rows[8+k] = le.AppendUint64(rows[8+k], x)
		}
	}
	header := append([]byte(nil), t.header...)
	descs := t.descs
	for i := range descs {
		d := &descs[i]
		d.count = uint64(counts[i])
		d.capacity = max(d.capacity, d.count)
		d.size = d.capacity * uint64(mtShapes[i][0])
		copy(header[mtDescAt[i]:], d.pack())
	}
	for k := 0; k < mtBitSets; k++ {
		le.PutUint64(header[mtBitCountAt[k]:], uint64(n))
	}
	out := append([]byte(nil), header...)
	put := func(payload []byte, align int) {
		out = append(out, make([]byte, mtAlignUp(len(out), align)-len(out))...)
		out = append(out, payload...)
	}
	for i, d := range descs {
		if i != 1 {
			put(rows[i], d.payloadAlign(mtShapes[i][1]))
			continue
		}
		tpl := t.groupTemplate
		var recs []byte
		var inner []mtDesc
		for _, p := range prefabs {
			cap := uint64(len(groupMembers[p]))
			desc := mtDesc{size: cap * mtU16Len, align: tpl.align, flags: tpl.flags, grow: tpl.grow, capacity: cap, count: cap}
			inner = append(inner, desc)
			recs = le.AppendUint64(recs, p)
			recs = append(recs, desc.pack()...)
		}
		put(recs, d.payloadAlign(mtShapes[1][1]))
		for g, p := range prefabs {
			var members []byte
			for _, m := range groupMembers[p] {
				members = le.AppendUint16(members, uint16(m))
			}
			put(members, inner[g].payloadAlign(mtU16Len))
		}
	}
	return out, nil
}

// subtree is actor_copy.py:subtree: the root and every descendant, breadth first, by parent index.
func (t *mtActorTable) subtree(root int) []int {
	out, todo := []int(nil), []int{root}
	for len(todo) > 0 {
		i := todo[0]
		todo = todo[1:]
		out = append(out, i)
		for j, a := range t.actors {
			if int(a.parent) == i && j != i {
				todo = append(todo, j)
			}
		}
	}
	return out
}

// ---- component tables (cr_rows.py) -----------------------------------------------------------------------------

type mtNest struct {
	off int
	sub []mtNest
}

// mtCRNested is cr_rows.py:NESTED for the types the terminal's subtree uses; any other type is a flat table.
var mtCRNested = func() map[string][]mtNest {
	script := make([]mtNest, 0, mtScriptTables)
	for t := 0; t < mtScriptTables; t++ {
		script = append(script, mtNest{off: mtScriptTablesAt + mtDescLen*t})
	}
	return map[string][]mtNest{
		"CComponentLODCR": {{off: 40, sub: []mtNest{{off: 64}}}},
		"CR15UILayoutCR":  {{off: 40}},
		"CScriptCR":       script,
	}
}()

const (
	mtScriptTablesAt = 48 // cr_rows.py:NESTED CScriptCR, range(48, 665, 56)
	mtScriptTables   = 12
)

type mtSpans struct {
	stride, rowsAt int
	rows, nested   [][2]int
	owners         []uint64
}

func mtCRDesc(b []byte, at int) (mtDesc, error) {
	d, err := mtReadDesc(b, at)
	if err != nil {
		return d, err
	}
	if d.capacity != 0 && d.size%d.capacity != 0 {
		return d, fmt.Errorf("table descriptor at %#x: size %d is not a multiple of capacity %d", at, d.size, d.capacity)
	}
	return d, nil
}

// mtSkipTable is cr_rows.py:skip_table.
func mtSkipTable(b []byte, descAt int, shape []mtNest, cursor int) (int, error) {
	d, err := mtCRDesc(b, descAt)
	if err != nil || d.count == 0 {
		return cursor, err
	}
	cursor = mtAlignUp(cursor, max(int(d.align), mtWordLen))
	rows := cursor
	cursor += int(d.count) * d.stride()
	if cursor > len(b) {
		return 0, fmt.Errorf("nested table at %#x runs past the table's %#x bytes", descAt, len(b))
	}
	for i := 0; i < int(d.count); i++ {
		for _, n := range shape {
			if cursor, err = mtSkipTable(b, rows+i*d.stride()+n.off, n.sub, cursor); err != nil {
				return 0, err
			}
		}
	}
	return cursor, nil
}

// mtWalk is cr_rows.py:spans.
func mtWalk(typeName string, b []byte) (*mtSpans, error) {
	shape := mtCRNested[typeName]
	d, err := mtCRDesc(b, 0)
	if err != nil {
		return nil, fmt.Errorf("%s: %w", typeName, err)
	}
	cursor := mtDescLen
	if d.count != 0 {
		cursor = mtAlignUp(mtDescLen, max(int(d.align), mtWordLen))
	}
	s := &mtSpans{stride: d.stride(), rowsAt: cursor}
	cursor += int(d.count) * s.stride
	if cursor > len(b) {
		return nil, fmt.Errorf("%s: %d rows of %d bytes run past the table's %#x bytes", typeName, d.count, s.stride, len(b))
	}
	if d.count != 0 && s.stride < mtOwnerAt+mtU64Len {
		return nil, fmt.Errorf("%s: rows of %d bytes end before the owner at +%d", typeName, s.stride, mtOwnerAt)
	}
	for i := 0; i < int(d.count); i++ {
		row := s.rowsAt + i*s.stride
		s.rows = append(s.rows, [2]int{row, row + s.stride})
		s.owners = append(s.owners, le.Uint64(b[row+mtOwnerAt:]))
		start := cursor
		for _, n := range shape {
			if cursor, err = mtSkipTable(b, row+n.off, n.sub, cursor); err != nil {
				return nil, fmt.Errorf("%s: %w", typeName, err)
			}
		}
		s.nested = append(s.nested, [2]int{start, cursor})
	}
	if cursor != len(b) {
		return nil, fmt.Errorf("%s: walk ends at %#x, the table is %#x bytes", typeName, cursor, len(b))
	}
	return s, nil
}

// ---- placement (actor_copy.py) ---------------------------------------------------------------------------------

// mtPlace is actor_copy.py:Place: p -> pivot + scale * (p - pivot) + offset.
type mtPlace struct {
	pivot, offset [3]float64
	scale         float64
}

func (pl mtPlace) point(p [3]float64) [3]float64 {
	var out [3]float64
	for i := range p {
		c := pl.pivot[i]
		out[i] = (c + float64(pl.scale*(p[i]-c))) + pl.offset[i]
	}
	return out
}

func mtGetVec(b []byte, at int) [3]float64 {
	var v [3]float64
	for i := range v {
		v[i] = float64(math.Float32frombits(le.Uint32(b[at+i*mtF32Len:])))
	}
	return v
}

func mtPutVec(b []byte, at int, v [3]float64) {
	for i := range v {
		le.PutUint32(b[at+i*mtF32Len:], math.Float32bits(float32(v[i])))
	}
}

// mtShift is actor_copy.py:shift: the world positions at `offsets` move with the place, the scales at `scales` scale.
func mtShift(row []byte, offsets []int, pl mtPlace, scales []int) []byte {
	out := append([]byte(nil), row...)
	for _, at := range offsets {
		mtPutVec(out, at, pl.point(mtGetVec(out, at)))
	}
	for _, at := range scales {
		v := mtGetVec(out, at)
		for i := range v {
			v[i] *= pl.scale
		}
		mtPutVec(out, at, v)
	}
	return out
}

func mtPlaceActor(transform []byte, pl mtPlace) []byte {
	return mtShift(transform, []int{mtPosAt}, pl, []int{mtScaleAt})
}

// mtRemap is actor_copy.py:remap_words: every aligned 8-byte word that names a source actor names its copy.
func mtRemap(data []byte, mapping map[uint64]uint64) []byte {
	out := append([]byte(nil), data...)
	for at := 0; at+mtWordLen <= len(out); at += mtWordLen {
		if to, ok := mapping[le.Uint64(out[at:])]; ok {
			le.PutUint64(out[at:], to)
		}
	}
	return out
}

// mtTypeKey is a type as a level file's key, 16 hex digits.
func mtTypeKey(typ uint64) string { return fmt.Sprintf("%016x", typ) }

// mtTypeNames are actor_copy.py:TYPE_NAMES, the types a stock button subtree uses.
var mtTypeNames = map[string]string{
	"142026f469321d54":       "COcclusionCullCR",
	"22f9fcb2d5e52e3c":       "CBoundingSphereCR",
	"2464c4ed290f3268":       "CInstanceModelCR",
	"7f49abae39aaf2aa":       "CComponentLODCR",
	"8b91efd51747c21e":       "CR15UILayoutCR",
	mtTypeKey(transformType): "CTransformCR", // steps_dot_damp.go's transformType
	"ca5a03d5a497238c":       "CFrustumCullCR",
	mtTypeKey(scriptTable):   "CScriptCR", // steps_drone.go's scriptTable
	"e9b24ea816dece48":       "CR15ButtonInteractCR",
	"4127ff2ffe6be26a":       "CTextureOverrideCR",
	"822fd4ccb42e8a3c":       "CCanvasUICR",
}

const (
	mtActorsType      = "347869ce492dc7da"
	mtSceneType       = "a388ea69e5108f4c"
	mtInstanceType    = "2464c4ed290f3268"
	mtTransformName   = "CTransformCR"
	mtButtonName      = "CR15ButtonInteractCR"
	mtTransformScaleA = 0x3C // init_scale (actor_copy.py:SCALES)
	mtTransformPosAt  = 0x30 // init_position (actor_copy.py:POSITIONS)
	mtButtonPosAt     = 0x80 // the button's pose (actor_copy.py:POSITIONS)
)

func mtPositions(name string) []int {
	switch name {
	case mtTransformName:
		return []int{mtTransformPosAt}
	case mtButtonName:
		return []int{mtButtonPosAt}
	}
	return nil
}

func mtScales(name string) []int {
	if name == mtTransformName {
		return []int{mtTransformScaleA}
	}
	return nil
}

// mtCopy is one copy of the subtree: the nodeids of its actors in subtree order, and where it goes.
type mtCopy struct {
	nodeids []uint64
	place   mtPlace
}

type mtPlan struct {
	mapping map[uint64]uint64
	place   mtPlace
}

// mtAppendFlat is actor_copy.py:append_flat: rows owned by a source actor are appended once per copy.
func mtAppendFlat(typeName string, b []byte, plans []mtPlan) ([]byte, error) {
	s, err := mtWalk(typeName, b)
	if err != nil {
		return nil, err
	}
	head := append([]byte(nil), b[:s.rowsAt]...)
	var rows, nested [][]byte
	for _, r := range s.rows {
		rows = append(rows, b[r[0]:r[1]])
	}
	for _, r := range s.nested {
		nested = append(nested, b[r[0]:r[1]])
	}
	for _, pl := range plans {
		for k, owner := range s.owners {
			if _, ok := pl.mapping[owner]; ok {
				rows = append(rows, mtShift(mtRemap(rows[k], pl.mapping), mtPositions(typeName), pl.place, mtScales(typeName)))
				nested = append(nested, mtRemap(nested[k], pl.mapping))
			}
		}
	}
	n := uint64(len(rows))
	if n > le.Uint64(head[mtDescCapAt:]) {
		le.PutUint64(head[mtDescSizeAt:], n*uint64(s.stride))
		le.PutUint64(head[mtDescCapAt:], n)
	}
	le.PutUint64(head[mtDescCountAt:], n)
	out := append([]byte(nil), head...)
	for _, r := range rows {
		out = append(out, r...)
	}
	for _, r := range nested {
		out = append(out, r...)
	}
	// A nested table's padding must not have moved (actor_copy.py:131-135).
	again, err := mtWalk(typeName, out)
	if err != nil {
		return nil, fmt.Errorf("%s after the copy: %w", typeName, err)
	}
	for k := len(s.owners); k < len(rows); k++ {
		if again.nested[k][1]-again.nested[k][0] != len(nested[k]) {
			return nil, fmt.Errorf("%s: the padding of copied row %d moved", typeName, k)
		}
	}
	return out, nil
}

// ---- CInstanceModelCR (instance_model_cr.py) -------------------------------------------------------------------

type mtInstanceModels struct {
	header []byte
	slots  [][][]byte
	nbits  uint64
}

func mtReadInstanceModels(b []byte) (*mtInstanceModels, error) {
	if len(b) < mtInstHeader {
		return nil, fmt.Errorf("CInstanceModelCR of %d bytes is shorter than its %d-byte header", len(b), mtInstHeader)
	}
	m := &mtInstanceModels{header: append([]byte(nil), b[:mtInstHeader]...)}
	cursor := mtInstHeader
	for _, sl := range mtInstSlots {
		d, err := mtReadDesc(b, sl[0])
		if err != nil {
			return nil, err
		}
		if d.capacity != 0 && d.size != d.capacity*uint64(sl[1]) {
			return nil, fmt.Errorf("CInstanceModelCR slot at %d: size %d, expected %d", sl[0], d.size, d.capacity*uint64(sl[1]))
		}
		start := cursor
		if d.count != 0 {
			start = mtAlignUp(cursor, max(int(d.align), sl[2]))
			if start > len(b) || !allZero(b[cursor:start]) {
				return nil, fmt.Errorf("CInstanceModelCR slot at %d: padding at %#x is not zero or runs past the table", sl[0], cursor)
			}
		}
		end := start + int(d.count)*sl[1]
		if end > len(b) {
			return nil, fmt.Errorf("CInstanceModelCR slot at %d runs past the table", sl[0])
		}
		var rows [][]byte
		for i := 0; i < int(d.count); i++ {
			rows = append(rows, b[start+i*sl[1]:start+(i+1)*sl[1]])
		}
		m.slots = append(m.slots, rows)
		cursor = end
	}
	if cursor != len(b) {
		return nil, fmt.Errorf("CInstanceModelCR walk ends at %#x, the table is %#x bytes", cursor, len(b))
	}
	m.nbits = le.Uint64(b[mtInstBitCount:])
	return m, nil
}

func (m *mtInstanceModels) owners() []uint64 {
	var out []uint64
	for _, r := range m.slots[mtInstOwnerRow] {
		out = append(out, le.Uint64(r[mtOwnerAt:]))
	}
	return out
}

// copyRecord appends a copy of record i; remap rewrites the nodeids of the lookup and property rows.
func (m *mtInstanceModels) copyRecord(i int, remap func([]byte) []byte) {
	n := len(m.slots[mtInstOwnerRow])
	words := m.slots[mtInstBits]
	bit := le.Uint64(words[i/mtBitWord])>>(i%mtBitWord)&1 == 1
	for _, s := range mtInstPerRecord {
		row := m.slots[s][i]
		if s == mtInstLookup || s == mtInstOwnerRow {
			row = remap(row)
		}
		m.slots[s] = append(m.slots[s], row)
	}
	for len(words)*mtBitWord < n+1 {
		words = append(words, make([]byte, mtU64Len))
	}
	if bit {
		w := le.Uint64(words[n/mtBitWord]) | 1<<(n%mtBitWord)
		words[n/mtBitWord] = le.AppendUint64(nil, w)
	}
	m.slots[mtInstBits] = words
	m.nbits = uint64(n + 1)
}

func (m *mtInstanceModels) write() []byte {
	out := append([]byte(nil), m.header...)
	le.PutUint64(out[mtInstBitCount:], m.nbits)
	for i, sl := range mtInstSlots {
		d, _ := mtReadDesc(out, sl[0])
		d.count = uint64(len(m.slots[i]))
		d.capacity = max(d.capacity, d.count)
		d.size = d.capacity * uint64(sl[1])
		copy(out[sl[0]:], d.pack())
	}
	for i, sl := range mtInstSlots {
		align := int(le.Uint32(out[sl[0]+mtDescAlignAt:]))
		if len(m.slots[i]) > 0 {
			out = append(out, make([]byte, mtAlignUp(len(out), max(align, sl[2]))-len(out))...)
		}
		out = append(out, bytes.Join(m.slots[i], nil)...)
	}
	return out
}

// ---- copy and move (actor_copy.py:copy_subtree, move_subtree) --------------------------------------------------

func mtSortedTypes(files map[string][]byte) []string {
	out := make([]string, 0, len(files))
	for t := range files {
		out = append(out, t)
	}
	sort.Strings(out)
	return out
}

// mtCopySubtree is actor_copy.py:copy_subtree. files holds a level's tables by type; the changed tables are returned.
func mtCopySubtree(files map[string][]byte, root int, copies []mtCopy) (map[string][]byte, error) {
	table, err := mtReadActorTable(files[mtActorsType])
	if err != nil {
		return nil, fmt.Errorf("actor table: %w", err)
	}
	src := table.subtree(root)
	var plans []mtPlan
	for _, c := range copies {
		if len(c.nodeids) != len(src) {
			return nil, fmt.Errorf("a copy names %d actors for a subtree of %d", len(c.nodeids), len(src))
		}
		taken := map[uint64]bool{}
		for _, a := range table.actors {
			taken[a.nodeid] = true
		}
		mapping := map[uint64]uint64{}
		for k, i := range src {
			if taken[c.nodeids[k]] {
				return nil, fmt.Errorf("actor table: nodeid %016x of the copy is already an actor (expected none)", c.nodeids[k])
			}
			mapping[table.actors[i].nodeid] = c.nodeids[k]
		}
		index := map[int]int{}
		for k, i := range src {
			a := table.actors[i]
			parent := int(a.parent)
			if p, ok := index[parent]; ok {
				parent = p
			}
			index[i] = len(table.actors)
			if index[i] >= mtMaxActors {
				return nil, fmt.Errorf("actor table: a copy would be actor %d; a parent index is 16 bits", index[i])
			}
			table.actors = append(table.actors, mtActor{nodeid: c.nodeids[k], name: a.name, prefabid: a.prefabid,
				transform: mtPlaceActor(a.transform, c.place), numpooled: a.numpooled, parent: uint16(parent), bits: a.bits})
		}
		plans = append(plans, mtPlan{mapping, c.place})
	}
	actors, err := table.write()
	if err != nil {
		return nil, err
	}
	out := map[string][]byte{mtActorsType: actors}
	owners := map[uint64]bool{}
	for _, pl := range plans {
		for o := range pl.mapping {
			owners[o] = true
		}
	}
	for _, t := range mtSortedTypes(files) {
		b := files[t]
		if t == mtActorsType || t == mtSceneType {
			continue
		}
		if t == mtInstanceType {
			m, err := mtReadInstanceModels(b)
			if err != nil {
				return nil, fmt.Errorf("type %s: %w", t, err)
			}
			own, hit := m.owners(), false
			for _, pl := range plans {
				mapping := pl.mapping
				for k, o := range own {
					if _, ok := mapping[o]; ok {
						m.copyRecord(k, func(row []byte) []byte { return mtRemap(row, mapping) })
						hit = true
					}
				}
			}
			if hit {
				out[t] = m.write()
			}
			continue
		}
		name, known := mtTypeNames[t]
		if !known {
			named := false
			for o := range owners {
				if bytes.Contains(b, le.AppendUint64(nil, o)) {
					named = true
					break
				}
			}
			if !named {
				continue
			}
			s, err := mtWalk("?", b)
			if err != nil {
				return nil, fmt.Errorf("type %s names a source actor and is not a flat table: %w", t, err)
			}
			hit := false
			for _, o := range s.owners {
				hit = hit || owners[o]
			}
			if !hit {
				continue
			}
			name = "?"
		}
		s, err := mtWalk(name, b)
		if err != nil {
			return nil, fmt.Errorf("type %s: %w", t, err)
		}
		hit := false
		for _, o := range s.owners {
			hit = hit || owners[o]
		}
		if hit {
			if out[t], err = mtAppendFlat(name, b, plans); err != nil {
				return nil, fmt.Errorf("type %s: %w", t, err)
			}
		}
	}
	return out, nil
}

// mtMoveSubtree is actor_copy.py:move_subtree: the subtree at `root`, and the rows its actors own, move and scale in place.
func mtMoveSubtree(files map[string][]byte, root int, pl mtPlace) (map[string][]byte, error) {
	table, err := mtReadActorTable(files[mtActorsType])
	if err != nil {
		return nil, fmt.Errorf("actor table: %w", err)
	}
	src := table.subtree(root)
	owners := map[uint64]bool{}
	for _, i := range src {
		table.actors[i].transform = mtPlaceActor(table.actors[i].transform, pl)
		owners[table.actors[i].nodeid] = true
	}
	actors, err := table.write()
	if err != nil {
		return nil, err
	}
	out := map[string][]byte{mtActorsType: actors}
	for _, t := range mtSortedNames() {
		name := mtTypeNames[t]
		b, ok := files[t]
		if !ok || (len(mtPositions(name)) == 0 && len(mtScales(name)) == 0) {
			continue
		}
		s, err := mtWalk(name, b)
		if err != nil {
			return nil, fmt.Errorf("type %s: %w", t, err)
		}
		moved, hit := append([]byte(nil), b...), false
		for k, o := range s.owners {
			if owners[o] {
				r := s.rows[k]
				copy(moved[r[0]:r[1]], mtShift(b[r[0]:r[1]], mtPositions(name), pl, mtScales(name)))
				hit = true
			}
		}
		if hit {
			out[t] = moved
		}
	}
	return out, nil
}

func mtSortedNames() []string {
	out := make([]string, 0, len(mtTypeNames))
	for t := range mtTypeNames {
		out = append(out, t)
	}
	sort.Strings(out)
	return out
}
