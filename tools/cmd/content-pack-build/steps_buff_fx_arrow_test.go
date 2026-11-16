package main

// The OVERCHARGE arrow mesh over a synthetic stock cross mesh of the shipped layout (20-byte stream 0, 28-byte stream 1,
// u16 indices); no byte of the game's is used. The stock-check values (the colours of every vertex, the index range)
// are the shipped record's facts, measured in the build's scratch extraction and cited in steps_buff_fx_arrow.go.

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"math"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

// syntheticCross is a cross-shaped buffer of the layout with the stock colours and an index table inside the vertex count.
func syntheticCross() []byte {
	b := make([]byte, arrowBytes)
	for k := 0; k < arrowVertices; k++ {
		copy(b[k*arrowStream0:], []byte{255, 0, 0, 255, 255, 255, 255, 255})
	}
	for i := 0; i < arrowIndices; i++ {
		binary.LittleEndian.PutUint16(b[arrowVertices*(arrowStream0+arrowStream1)+2*i:], uint16(i%arrowVertices))
	}
	return b
}

// arrowPack is a pack over a stock set that lists the cross mesh under each of its four types; the mesh-data type holds cross.
func arrowPack(t *testing.T, cross []byte) *Pack {
	t.Helper()
	res := map[radpack.Key][]byte{}
	for _, typ := range arrowMeshTypes {
		body := bytes.Repeat([]byte{byte(typ)}, 44)
		if typ == arrowMeshData {
			body = cross
		}
		res[radpack.Key{Type: int64(typ), Name: int64(crossName)}] = body
	}
	dir := t.TempDir()
	if err := radpack.WriteStockSet(dir, Sets[0], []map[radpack.Key][]byte{res}); err != nil {
		t.Fatal(err)
	}
	s, _, err := radpack.OpenSet(dir, Sets[0])
	if err != nil {
		t.Fatal(err)
	}
	return &Pack{sets: map[string]*radpack.Set{Sets[0]: s}, changes: map[string]map[radpack.Key][]byte{}, scripts: map[string][]byte{}}
}

func arrowStep(t *testing.T) Step {
	t.Helper()
	var found []Step
	for _, st := range steps {
		if st.Name == buffArrowStep {
			found = append(found, st)
		}
	}
	if len(found) != 1 {
		t.Fatalf("the production steps hold %d steps named %q, want 1", len(found), buffArrowStep)
	}
	return found[0]
}

func TestTheArrowStepIsRegisteredOnceAndDeclaresItsFourRecords(t *testing.T) {
	st := arrowStep(t)
	if len(st.Writes) != len(arrowMeshTypes) {
		t.Fatalf("the step declares %d fields, want %d", len(st.Writes), len(arrowMeshTypes))
	}
	for i, f := range st.Writes {
		body := []byte{1, 2, 3}
		got, err := f.Read(body)
		if err != nil || !bytes.Equal(got, body) {
			t.Errorf("field %d reads %x (%v), want its record unchanged", i, got, err)
		}
		if f.Set != Sets[0] || f.Type != arrowMeshTypes[i] || len(f.Records) != 1 || f.Records[0] != arrowName || f.Name == "" {
			t.Errorf("field %d: set %s type %016x records %x, want set 0, type %016x, record %016x",
				i, f.Set, f.Type, f.Records, arrowMeshTypes[i], arrowName)
		}
	}
}

func TestTheArrowNameIsItsHashAndNotTheCrossMesh(t *testing.T) {
	if arrowName == crossName || arrowName != 0x40a15300d0c79969 {
		t.Fatalf("arrowName %016x, crossName %016x: want the CSymbol64 of fx_overcharge_arrow, 40a15300d0c79969", arrowName, crossName)
	}
}

func TestTheArrowMeshGetsTheCrossRecordsUnderItsNameAndOnlyTheBufferChanges(t *testing.T) {
	cross := syntheticCross()
	p := arrowPack(t, cross)
	if err := buffArrowMeshes(p); err != nil {
		t.Fatal(err)
	}
	got := p.changes[Sets[0]]
	if len(got) != len(arrowMeshTypes) || len(p.scripts) != 0 {
		t.Fatalf("%d resources and %d scripts changed, want %d resources and no script", len(got), len(p.scripts), len(arrowMeshTypes))
	}
	for _, typ := range arrowMeshTypes {
		body := got[radpack.Key{Type: int64(typ), Name: int64(arrowName)}]
		if typ == arrowMeshData {
			if len(body) != arrowBytes {
				t.Errorf("the arrow buffer is %d bytes, want %d", len(body), arrowBytes)
			}
			if bytes.Equal(body, cross) {
				t.Error("the arrow buffer is the cross's own bytes: the step did not regenerate it")
			}
			continue
		}
		if !bytes.Equal(body, bytes.Repeat([]byte{byte(typ)}, 44)) {
			t.Errorf("type %016x: the arrow record is not the cross's record copied", typ)
		}
	}
}

// arrowVertex is one decoded vertex of the layout: its colours, position, normal and tangent.
type arrowVertex struct {
	pos, nrm, tan [3]float64
}

// decodeArrow reads the buffer back in the layout buff_assets.py writes (stream 0, stream 1, then the indices).
func decodeArrow(b []byte) ([]arrowVertex, []uint16) {
	vs := make([]arrowVertex, arrowVertices)
	for k := range vs {
		s1 := arrowVertices*arrowStream0 + k*arrowStream1
		for c := 0; c < 3; c++ {
			vs[k].pos[c] = float64(math.Float32frombits(binary.LittleEndian.Uint32(b[s1+4*c:])))
		}
		for c := 0; c < 3; c++ {
			vs[k].nrm[c] = float64(int16(binary.LittleEndian.Uint16(b[s1+12+2*c:]))) / 32767
			vs[k].tan[c] = float64(int16(binary.LittleEndian.Uint16(b[s1+20+2*c:]))) / 32767
		}
	}
	idx := make([]uint16, arrowIndices)
	for i := range idx {
		idx[i] = binary.LittleEndian.Uint16(b[arrowVertices*(arrowStream0+arrowStream1)+2*i:])
	}
	return vs, idx
}

func TestTheArrowBufferHasTheCrossLayoutAndAClosedArrowShape(t *testing.T) {
	p := arrowPack(t, syntheticCross())
	if err := buffArrowMeshes(p); err != nil {
		t.Fatal(err)
	}
	b := p.changes[Sets[0]][radpack.Key{Type: int64(arrowMeshData), Name: int64(arrowName)}]
	vs, idx := decodeArrow(b)
	for k := 0; k < arrowVertices; k++ {
		if got := b[k*arrowStream0 : k*arrowStream0+8]; !bytes.Equal(got, []byte{255, 0, 0, 255, 255, 255, 255, 255}) {
			t.Fatalf("vertex %d colours %x, want the stock pair", k, got)
		}
	}
	used := 3 * arrowTriangles()
	if used != 156 {
		t.Fatalf("the arrow has %d indices, want 156", used)
	}
	for i := used; i < arrowIndices; i++ {
		if idx[i] != 0 {
			t.Fatalf("index %d is %d after the arrow's triangles, want the zero padding", i, idx[i])
		}
	}
	for k := arrowVerticesUsed(); k < arrowVertices; k++ {
		if vs[k] != vs[0] {
			t.Fatalf("spare vertex %d is not a copy of vertex 0 (degenerate)", k)
		}
	}
	for i := 0; i < used; i++ {
		if int(idx[i]) >= arrowVerticesUsed() {
			t.Fatalf("index %d names vertex %d of %d", i, idx[i], arrowVerticesUsed())
		}
	}
	bound := arrowApex + 1e-6
	for k := 0; k < arrowVerticesUsed(); k++ {
		v := vs[k]
		if math.Abs(v.pos[0]) > bound || math.Abs(v.pos[1]) > bound || math.Abs(v.pos[2]) > arrowHalfT+1e-6 {
			t.Fatalf("vertex %d at %v lies outside the arrow's box", k, v.pos)
		}
		if n := math.Sqrt(v.nrm[0]*v.nrm[0] + v.nrm[1]*v.nrm[1] + v.nrm[2]*v.nrm[2]); math.Abs(n-1) > 1e-3 {
			t.Fatalf("vertex %d normal length %g, want 1", k, n)
		}
		if d := v.nrm[0]*v.tan[0] + v.nrm[1]*v.tan[1] + v.nrm[2]*v.tan[2]; math.Abs(d) > 1e-3 {
			t.Fatalf("vertex %d tangent is not perpendicular to its normal (dot %g)", k, d)
		}
	}
	for i := 0; i < used; i += 3 {
		a, bb, c := vs[idx[i]].pos, vs[idx[i+1]].pos, vs[idx[i+2]].pos
		g := cross3(sub3(bb, a), sub3(c, a))
		if dot3(g, vs[idx[i]].nrm) <= 0 {
			t.Fatalf("triangle %d faces against its vertex normal", i/3)
		}
	}
}

// arrowTriangles and arrowVerticesUsed are the counts the geometry gives (98 vertices, 52 triangles), read from it so
// the count is not a second copy of the rule's arithmetic.
func arrowTriangles() int             { return len(arrowGeometry().tris) }
func arrowVerticesUsed() int          { return len(arrowGeometry().pos) }
func sub3(a, b [3]float64) [3]float64 { return [3]float64{a[0] - b[0], a[1] - b[1], a[2] - b[2]} }
func dot3(a, b [3]float64) float64    { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2] }
func cross3(a, b [3]float64) [3]float64 {
	return [3]float64{a[1]*b[2] - a[2]*b[1], a[2]*b[0] - a[0]*b[2], a[0]*b[1] - a[1]*b[0]}
}

// The arrow buffer is pinned to this rule's own output (the SHA-256 of the bytes the step writes for the synthetic cross,
// which carries no game data). It pins the arithmetic at this commit; it is not Cat's bytes, which only the stage82
// oracle compares, and the oracle is not run here.
const arrowOwnSha256 = "9659faa25d38c98f0fd8bdce32f6dfe2efe56b42c97ee5bd961b88d84ef89d52"

func TestTheArrowBufferIsPinnedToThisRulesOutput(t *testing.T) {
	p := arrowPack(t, syntheticCross())
	if err := buffArrowMeshes(p); err != nil {
		t.Fatal(err)
	}
	b := p.changes[Sets[0]][radpack.Key{Type: int64(arrowMeshData), Name: int64(arrowName)}]
	sum := sha256.Sum256(b)
	if got := hex.EncodeToString(sum[:]); got != arrowOwnSha256 {
		t.Errorf("arrow buffer sha256 %s, want %s", got, arrowOwnSha256)
	}
}

func TestAStockCrossThatIsNotTheShippedLayoutIsRefusedNamingItsRecord(t *testing.T) {
	short := syntheticCross()[:arrowBytes-2]
	colour := syntheticCross()
	colour[3*arrowStream0+4] = 0
	index := syntheticCross()
	binary.LittleEndian.PutUint16(index[arrowVertices*(arrowStream0+arrowStream1):], arrowVertices)
	cases := []struct {
		name  string
		cross []byte
		want  string
	}{
		{"size", short, fmt.Sprintf("%d bytes, want %d", len(short), arrowBytes)},
		{"colour of vertex 3", colour, "vertex 3"},
		{"index past the vertices", index, fmt.Sprintf("%d", arrowVertices)},
	}
	for _, c := range cases {
		p := arrowPack(t, c.cross)
		err := buffArrowMeshes(p)
		if err == nil || !strings.Contains(err.Error(), c.want) || !strings.Contains(err.Error(), fmt.Sprintf("%016x", crossName)) {
			t.Errorf("%s: error %v, want one that names the record and %q", c.name, err, c.want)
		}
		if len(p.changes[Sets[0]]) != 0 {
			t.Errorf("%s: %d records changed after a refusal, want none", c.name, len(p.changes[Sets[0]]))
		}
	}
}

func TestAStockSetThatAlreadyListsTheArrowIsRefused(t *testing.T) {
	p := arrowPack(t, syntheticCross())
	res := map[radpack.Key][]byte{}
	for _, typ := range arrowMeshTypes {
		res[radpack.Key{Type: int64(typ), Name: int64(arrowName)}] = []byte{9}
	}
	dir := t.TempDir()
	if err := radpack.WriteStockSet(dir, Sets[0], []map[radpack.Key][]byte{res}); err != nil {
		t.Fatal(err)
	}
	s, _, err := radpack.OpenSet(dir, Sets[0])
	if err != nil {
		t.Fatal(err)
	}
	p.sets[Sets[0]] = s
	if err := buffArrowMeshes(p); err == nil || !strings.Contains(err.Error(), "already lists") {
		t.Errorf("a set that lists fx_overcharge_arrow was not refused: %v", err)
	}
}

// solveAt against the exact answer, with and without the row swap the partial pivot makes (a21 larger than a11).
func TestSolveAtSolvesTheTwoByTwoInBothPivotOrders(t *testing.T) {
	cases := []struct{ a11, a12, a21, a22, t, s float64 }{
		{2, 1, 1, 3, 1.5, -0.25}, // no swap: |a21| <= |a11|
		{1, 2, 4, 3, -0.5, 0.75}, // swap: |a21| > |a11|
	}
	for _, c := range cases {
		b1 := c.a11*c.t + c.a12*c.s
		b2 := c.a21*c.t + c.a22*c.s
		if got := solveAt(c.a11, c.a12, c.a21, c.a22, b1, b2); math.Abs(got-c.t) > 1e-12 {
			t.Errorf("solveAt for %v = %g, want %g", c, got, c.t)
		}
	}
}

// snorm is Python's round of the clamped component times 32767: half to even, and clamped to +-1.
func TestSnormRoundsHalfToEvenAndClamps(t *testing.T) {
	cases := []struct {
		x    float64
		want int16
	}{
		{0.5 / 32767, 0}, {1.5 / 32767, 2}, {2.5 / 32767, 2}, {-1.5 / 32767, -2},
		{1.5, 32767}, {-1.5, -32767}, {1, 32767}, {-1, -32767},
	}
	for _, c := range cases {
		if got := snorm(c.x); got != c.want {
			t.Errorf("snorm(%g) = %d, want %d", c.x, got, c.want)
		}
	}
}
