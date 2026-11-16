package main

// OVERCHARGE's purple hand effect, the arrow mesh (PLAN-v5 10.1, piece G-BUFF-FX-1), from he_is_the_cat's
// tools/rm_preview/buff_assets.py at 82ad7ba (read as text; his tool is never run here). The stock cross mesh of Repair
// Matrix, c90ff12470c68d3b, is listed under four types (buff_assets.py:54, MESH_TYPES). Three of its records are copied
// under CSymbol64("fx_overcharge_arrow") unchanged; the mesh-data record (e7a8ab5ceaef49cb) is regenerated as the arrow in
// the cross's own layout (buff_assets.py:121-146, :255-257).
//
// Stock facts checked before anything is written (measured in the build's scratch extraction of the shipped data, not in
// a test): the cross's mesh-data record is 12648 bytes (248 vertices of 20 + 28 bytes, then 372 u16 indices); every
// vertex's colour pair is ff0000ff then ffffffff; every index is below 248. A set that already lists the arrow name is
// refused: it is not the stock set this step is written over.
//
// The stock tangent's w is +1 or -1 by vertex (measured: 124 each); buff_assets.py:141 writes +1 for every arrow vertex.
// That is his choice and is implemented as written; it is a difference from the stock layout's own signs, reported and not
// corrected here.
//
// Declared fields (Step.Writes, what the stage82 oracle compares): the four arrow records, whole.
//
// Numbers are float64 package variables, not constants, so that the arithmetic runs at run time in the order
// buff_assets.py does it (a constant expression would be folded exactly, and his float64 products would differ from it).

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"math"
)

const (
	arrowVertices = 248                                                        // NV, buff_assets.py:56: the cross's vertex count
	arrowIndices  = 372                                                        // NI, buff_assets.py:56: the cross's index count
	arrowStream0  = 20                                                         // a vertex's stream 0: colours, uv float, uv u16 (buff_assets.py:135-136)
	arrowStream1  = 28                                                         // a vertex's stream 1: position, normal, tangent (buff_assets.py:137-141)
	arrowBytes    = arrowVertices*(arrowStream0+arrowStream1) + arrowIndices*2 // 12648: the cross's mesh-data record
)

var (
	arrowName     uint64 = 0x40a15300d0c79969 // CSymbol64("fx_overcharge_arrow"), buff_assets.py:52
	crossName     uint64 = 0xc90ff12470c68d3b // Repair Matrix's cross mesh, buff_assets.py:51
	arrowMeshData uint64 = 0xe7a8ab5ceaef49cb // the mesh-data type, buff_assets.py:55

	// arrowMeshTypes are the four types the cross mesh is listed under, in buff_assets.py:54's order.
	arrowMeshTypes = []uint64{0x37102e4b27955a14, 0x73d312a620da3824, 0xc2434c5a99e139ce, 0xe7a8ab5ceaef49cb}
)

// The arrow's outline, metres, counter-clockwise seen from +Z: the cross spans +-0.039 (buff_assets.py:60-62).
var (
	arrowApex     float64 = 0.039
	arrowHeadW    float64 = 0.037
	arrowHeadBase float64 = 0.0
	arrowShaftW   float64 = 0.0145
	arrowBottom   float64 = -0.039
	arrowHalfT    float64 = 0.01219653
	arrowBevel    float64 = 0.004
	arrowUVHi     float64 = 0.97726667 // buff_assets.py:130, the cross's uv extent
	arrowUVLo     float64 = 0.02273312
	arrowSnorm    float64 = 32767 // a normalised component in a 16-bit signed short (buff_assets.py:140)
	arrowSideCut  float64 = 0.9   // the tangent's axis choice (buff_assets.py:137)
)

// arrowCapTris is buff_assets.py:70's CAP_TRIS, over the seven inset outline points.
var arrowCapTris = [][3]int{{0, 1, 2}, {0, 2, 5}, {0, 5, 6}, {2, 3, 4}, {2, 4, 5}}

// arrowMesh is the bevelled arrow before it is encoded: positions, normals and triangles (buff_assets.py:89-118).
type arrowMesh struct {
	pos, nrm [][3]float64
	tris     [][3]int
}

type point2 struct{ x, y float64 }

func arrowOutline() []point2 {
	return []point2{
		{0, arrowApex},
		{-arrowHeadW, arrowHeadBase},
		{-arrowShaftW, arrowHeadBase},
		{-arrowShaftW, arrowBottom},
		{arrowShaftW, arrowBottom},
		{arrowShaftW, arrowHeadBase},
		{arrowHeadW, arrowHeadBase},
	}
}

// solveAt is the first unknown of the 2x2 system [[a11 a12] [a21 a22]] [t s] = [b1 b2], solved the way LAPACK's dgesv does
// it for a 2x2 (numpy's solve): partial pivoting on the first column, the pivot's reciprocal scales the multiplier, the
// upper solve divides. Last-bit agreement with numpy's OpenBLAS is not established; see the commit's report.
func solveAt(a11, a12, a21, a22, b1, b2 float64) float64 {
	if math.Abs(a21) > math.Abs(a11) {
		a11, a21 = a21, a11
		a12, a22 = a22, a12
		b1, b2 = b2, b1
	}
	l := a21 * (1 / a11)
	u22 := a22 - l*a12
	y2 := b2 - l*b1
	x2 := y2 / u22
	return (b1 - a12*x2) / a11
}

// arrowInset offsets a counter-clockwise polygon inward by d with mitred corners (buff_assets.py:73-86).
func arrowInset(poly []point2, d float64) []point2 {
	n := len(poly)
	out := make([]point2, 0, n)
	for i := 0; i < n; i++ {
		p0, p1, p2 := poly[(i-1+n)%n], poly[i], poly[(i+1)%n]
		e0 := point2{p1.x - p0.x, p1.y - p0.y}
		e1 := point2{p2.x - p1.x, p2.y - p1.y}
		n0 := arrowNormal2(e0)
		n1 := arrowNormal2(e1)
		// the offset lines p0 + n0 d + t e0 and p1 + n1 d + s e1 meet where [e0 -e1] [t s]' = (p1 + n1 d) - (p0 + n0 d)
		t := solveAt(e0.x, -e1.x, e0.y, -e1.y,
			(p1.x+n1.x*d)-(p0.x+n0.x*d), (p1.y+n1.y*d)-(p0.y+n0.y*d))
		out = append(out, point2{p0.x + n0.x*d + t*e0.x, p0.y + n0.y*d + t*e0.y})
	}
	return out
}

// arrowNormal2 is the left normal of e, unit length: (-e.y, e.x) / |e| (buff_assets.py:80-81).
func arrowNormal2(e point2) point2 {
	l := math.Sqrt(e.x*e.x + e.y*e.y)
	return point2{-e.y / l, e.x / l}
}

func arrowSub(a, b [3]float64) [3]float64 { return [3]float64{a[0] - b[0], a[1] - b[1], a[2] - b[2]} }

// arrowCross is numpy's cross product of two 3-vectors, in its component order.
func arrowCross(a, b [3]float64) [3]float64 {
	return [3]float64{a[1]*b[2] - a[2]*b[1], a[2]*b[0] - a[0]*b[2], a[0]*b[1] - a[1]*b[0]}
}

func arrowUnit(v [3]float64) [3]float64 {
	l := math.Sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2])
	return [3]float64{v[0] / l, v[1] / l, v[2] / l}
}

// arrowGeometry is buff_assets.py's arrow_geometry: caps at +-HALF_T on the inset outline, then per outline edge a front
// chamfer, a side wall and a back chamfer, each quad flat shaded with its own normal.
func arrowGeometry() arrowMesh {
	o := arrowOutline()
	in := arrowInset(o, arrowBevel)
	var m arrowMesh
	quad := func(a, b, c, d [3]float64) {
		n := arrowUnit(arrowCross(arrowSub(b, a), arrowSub(d, a)))
		k := len(m.pos)
		m.pos = append(m.pos, a, b, c, d)
		m.nrm = append(m.nrm, n, n, n, n)
		m.tris = append(m.tris, [3]int{k, k + 1, k + 2}, [3]int{k, k + 2, k + 3})
	}
	for _, cap := range []struct {
		z    float64
		flip bool
	}{{arrowHalfT, false}, {-arrowHalfT, true}} {
		k := len(m.pos)
		nz := 1.0
		if cap.flip {
			nz = -1.0
		}
		for _, p := range in {
			m.pos = append(m.pos, [3]float64{p.x, p.y, cap.z})
			m.nrm = append(m.nrm, [3]float64{0, 0, nz})
		}
		for _, t := range arrowCapTris {
			if cap.flip {
				m.tris = append(m.tris, [3]int{k + t[0], k + t[2], k + t[1]})
			} else {
				m.tris = append(m.tris, [3]int{k + t[0], k + t[1], k + t[2]})
			}
		}
	}
	zi := arrowHalfT - arrowBevel
	v := func(p point2, z float64) [3]float64 { return [3]float64{p.x, p.y, z} }
	for i := range o {
		j := (i + 1) % len(o)
		o0, o1, i0, i1 := o[i], o[j], in[i], in[j]
		quad(v(o0, zi), v(o1, zi), v(i1, arrowHalfT), v(i0, arrowHalfT))
		quad(v(o0, -zi), v(o1, -zi), v(o1, zi), v(o0, zi))
		quad(v(i0, -arrowHalfT), v(i1, -arrowHalfT), v(o1, -zi), v(o0, -zi))
	}
	return m
}

// arrowSpan is the uv scale: 2 max(APEX, HEAD_W) over the cross's uv extent (buff_assets.py:130).
func arrowSpan() float64 {
	m := arrowApex
	if arrowHeadW > m {
		m = arrowHeadW
	}
	return 2 * m / (arrowUVHi - arrowUVLo)
}

// snorm is buff_assets.py's q: the clamped component times 32767, rounded half to even (Python's round).
func snorm(x float64) int16 {
	c := math.Max(-1, math.Min(1, x))
	return int16(math.RoundToEven(c * arrowSnorm))
}

// arrowBuffer encodes the mesh in the cross's layout (buff_assets.py:121-146): the spare vertices repeat vertex 0, the
// index table is padded with zeros.
func arrowBuffer(m arrowMesh) ([]byte, error) {
	if len(m.pos) > arrowVertices || len(m.tris)*3 > arrowIndices {
		return nil, fmt.Errorf("the arrow has %d vertices and %d indices, the cross's record holds %d and %d",
			len(m.pos), len(m.tris)*3, arrowVertices, arrowIndices)
	}
	span := arrowSpan()
	var s0, s1 []byte
	for k := 0; k < arrowVertices; k++ {
		p, n := m.pos[0], m.nrm[0]
		if k < len(m.pos) {
			p, n = m.pos[k], m.nrm[k]
		}
		s0 = append(s0, 255, 0, 0, 255, 255, 255, 255, 255)
		u := 0.5 + p[0]/span
		w := 0.5 - p[1]/span
		s0 = binary.LittleEndian.AppendUint32(s0, math.Float32bits(float32(u)))
		s0 = binary.LittleEndian.AppendUint32(s0, math.Float32bits(float32(w)))
		s0 = binary.LittleEndian.AppendUint16(s0, uint16(int(u*65535)))
		s0 = binary.LittleEndian.AppendUint16(s0, uint16(int(w*65535)))

		t := [3]float64{1, 0, 0}
		if math.Abs(n[0]) >= arrowSideCut {
			t = [3]float64{0, 0, 1}
		}
		d := t[0]*n[0] + t[1]*n[1] + t[2]*n[2]
		t = arrowUnit([3]float64{t[0] - n[0]*d, t[1] - n[1]*d, t[2] - n[2]*d})
		for _, c := range p {
			s1 = binary.LittleEndian.AppendUint32(s1, math.Float32bits(float32(c)))
		}
		for _, c := range n {
			s1 = binary.LittleEndian.AppendUint16(s1, uint16(snorm(c)))
		}
		s1 = binary.LittleEndian.AppendUint16(s1, 0)
		for _, c := range t {
			s1 = binary.LittleEndian.AppendUint16(s1, uint16(snorm(c)))
		}
		s1 = binary.LittleEndian.AppendUint16(s1, uint16(arrowSnorm))
	}
	idx := make([]byte, 0, arrowIndices*2)
	for _, t := range m.tris {
		for _, i := range t {
			idx = binary.LittleEndian.AppendUint16(idx, uint16(i))
		}
	}
	for len(idx) < arrowIndices*2 {
		idx = binary.LittleEndian.AppendUint16(idx, 0)
	}
	out := append(append(s0, s1...), idx...)
	if len(out) != arrowBytes {
		return nil, fmt.Errorf("the arrow buffer is %d bytes, want %d", len(out), arrowBytes)
	}
	return out, nil
}

// checkCrossBuffer refuses a stock cross mesh that is not the shipped layout: its size, every vertex's colour pair, and
// every index's range. The error names the record and the field, the expected and the actual.
func checkCrossBuffer(b []byte) error {
	if len(b) != arrowBytes {
		return fmt.Errorf("record %016x.%016x (the cross mesh's buffer), size: %d bytes, want %d",
			arrowMeshData, crossName, len(b), arrowBytes)
	}
	want0 := []byte{255, 0, 0, 255}
	want1 := []byte{255, 255, 255, 255}
	for k := 0; k < arrowVertices; k++ {
		if got := b[k*arrowStream0 : k*arrowStream0+4]; !bytes.Equal(got, want0) {
			return fmt.Errorf("record %016x.%016x (the cross mesh's buffer), vertex %d colour 1: %x, want %x",
				arrowMeshData, crossName, k, got, want0)
		}
		if got := b[k*arrowStream0+4 : k*arrowStream0+8]; !bytes.Equal(got, want1) {
			return fmt.Errorf("record %016x.%016x (the cross mesh's buffer), vertex %d colour 0: %x, want %x",
				arrowMeshData, crossName, k, got, want1)
		}
	}
	base := arrowVertices * (arrowStream0 + arrowStream1)
	for i := 0; i < arrowIndices; i++ {
		if v := binary.LittleEndian.Uint16(b[base+2*i:]); int(v) >= arrowVertices {
			return fmt.Errorf("record %016x.%016x (the cross mesh's buffer), index %d: %d, want below %d",
				arrowMeshData, crossName, i, v, arrowVertices)
		}
	}
	return nil
}

// buffArrowMeshes lists the arrow mesh: the cross's three records copied, the buffer regenerated, each under the arrow name.
func buffArrowMeshes(p *Pack) error {
	stock := p.sets[Sets[0]]
	if stock == nil {
		return fmt.Errorf("set %s is not one of the client's sets", Sets[0])
	}
	for _, typ := range arrowMeshTypes {
		if _, found, err := stock.Read(int64(typ), int64(arrowName)); err != nil {
			return err
		} else if found {
			return fmt.Errorf("type %016x already lists %016x (fx_overcharge_arrow): not the stock set", typ, arrowName)
		}
	}
	bodies := make(map[uint64][]byte, len(arrowMeshTypes))
	for _, typ := range arrowMeshTypes {
		b, err := p.Read(Sets[0], int64(typ), int64(crossName))
		if err != nil {
			return err
		}
		if typ != arrowMeshData {
			bodies[typ] = b
			continue
		}
		if err := checkCrossBuffer(b); err != nil {
			return err
		}
		arrow, err := arrowBuffer(arrowGeometry())
		if err != nil {
			return err
		}
		bodies[typ] = arrow
	}
	for _, typ := range arrowMeshTypes {
		if err := p.Put(Sets[0], int64(typ), int64(arrowName), bodies[typ]); err != nil {
			return err
		}
	}
	return nil
}

// arrowFields declares the four arrow records, whole: each is compared as the bytes the step wrote.
func arrowFields() []Field {
	whole := func(b []byte) ([]byte, error) { return b, nil }
	fs := make([]Field, 0, len(arrowMeshTypes))
	for _, typ := range arrowMeshTypes {
		fs = append(fs, Field{Name: fmt.Sprintf("the arrow mesh record of type %016x", typ), Set: Sets[0], Type: typ,
			Records: []uint64{arrowName}, Read: whole})
	}
	return fs
}

const buffArrowStep = "OVERCHARGE's purple arrow mesh: the cross's four records under fx_overcharge_arrow"

func init() {
	steps = append(steps, Step{Name: buffArrowStep, Apply: buffArrowMeshes, Writes: arrowFields()})
}
