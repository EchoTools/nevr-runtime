package gltf

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"math"
	"strings"
	"testing"
)

const (
	fixtureVertices = 4
	fixtureIndices  = 6
	fixtureMeshName = "quad"
	fixtureMaterial = 7
	posBytes        = 12
	uvBytes         = 8
	idxBytes        = 2
	badIndex        = 9
	lastVertex      = 3
	secondVertex    = 1
	uvScale         = 0.25
	posXStep        = 1.5
	twoVertices     = 2
	thirdVertex     = 2
	attrSets        = 2
	padByte         = ' '
	negativeCount   = -1
	modeLines       = 1
	secondBuffer    = 1
	shortCount      = 3
	chunkLengthHigh = 3 // the high byte of the first chunk's little-endian length
	gapBytes        = 4 // the unread bytes between interleaved vertices
	oddStride       = 26
	smallStride     = 8
	junkByte        = 0xAB
)

// rawChunk is a GLB chunk the test places before or after the BIN chunk.
type rawChunk struct {
	kind    uint32
	payload []byte
}

// fixture is a one-mesh GLB: a quad with positions, normals and UVs, each in its own tightly packed view, and six u16
// indices. before and after are extra chunks around the BIN chunk.
type fixture struct {
	doc    map[string]any
	bin    []byte
	mess   func(doc map[string]any)
	before []rawChunk
	after  []rawChunk
}

func (f *fixture) accessors() []any { return f.doc["accessors"].([]any) }

func (f *fixture) prim() map[string]any {
	return f.doc["meshes"].([]any)[0].(map[string]any)["primitives"].([]any)[0].(map[string]any)
}

// addView appends data to the BIN buffer, 4-aligned, adds a bufferView over it and returns the view's index.
func (f *fixture) addView(data []byte, stride int) int {
	for len(f.bin)%chunkAlign != 0 {
		f.bin = append(f.bin, 0)
	}
	off := len(f.bin)
	f.bin = append(f.bin, data...)
	for len(f.bin)%chunkAlign != 0 {
		f.bin = append(f.bin, 0)
	}
	v := map[string]any{"buffer": 0, "byteOffset": off, "byteLength": len(data)}
	if stride != 0 {
		v["byteStride"] = stride
	}
	f.doc["bufferViews"] = append(f.doc["bufferViews"].([]any), v)
	f.doc["buffers"].([]any)[0].(map[string]any)["byteLength"] = len(f.bin)
	return len(f.doc["bufferViews"].([]any)) - 1
}

// addAccessor adds an accessor and returns its index.
func (f *fixture) addAccessor(a map[string]any) int {
	f.doc["accessors"] = append(f.accessors(), a)
	return len(f.accessors()) - 1
}

func chunkBytes(out *bytes.Buffer, kind uint32, payload []byte) {
	_ = binary.Write(out, binary.LittleEndian, uint32(len(payload)))
	_ = binary.Write(out, binary.LittleEndian, kind)
	out.Write(payload)
}

func baseFixture() fixture {
	var bin bytes.Buffer
	put := func(v ...float32) {
		for _, f := range v {
			_ = binary.Write(&bin, binary.LittleEndian, f)
		}
	}
	for i := 0; i < fixtureVertices; i++ {
		put(float32(i)*posXStep, float32(i), 0)
	}
	for i := 0; i < fixtureVertices; i++ {
		put(0, 0, 1)
	}
	for i := 0; i < fixtureVertices; i++ {
		put(float32(i)*uvScale, 1)
	}
	for _, ix := range []uint16{0, secondVertex, thirdVertex, thirdVertex, secondVertex, lastVertex} {
		_ = binary.Write(&bin, binary.LittleEndian, ix)
	}
	for bin.Len()%chunkAlign != 0 {
		bin.WriteByte(0)
	}
	view := func(off, n int) map[string]any {
		return map[string]any{"buffer": 0, "byteOffset": off, "byteLength": n}
	}
	posLen, uvLen := fixtureVertices*posBytes, fixtureVertices*uvBytes
	doc := map[string]any{
		"asset":   map[string]any{"version": "2.0"},
		"buffers": []any{map[string]any{"byteLength": bin.Len()}},
		"bufferViews": []any{
			view(0, posLen), view(posLen, posLen), view(attrSets*posLen, uvLen), view(attrSets*posLen+uvLen, fixtureIndices*idxBytes),
		},
		"accessors": []any{
			map[string]any{"bufferView": 0, "componentType": componentF32, "count": fixtureVertices, "type": "VEC3"},
			map[string]any{"bufferView": 1, "componentType": componentF32, "count": fixtureVertices, "type": "VEC3"},
			map[string]any{"bufferView": 2, "componentType": componentF32, "count": fixtureVertices, "type": "VEC2"},
			map[string]any{"bufferView": 3, "componentType": componentU16, "count": fixtureIndices, "type": "SCALAR"},
		},
		"meshes": []any{map[string]any{"name": fixtureMeshName, "primitives": []any{map[string]any{
			"attributes": map[string]any{"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
			"indices":    3, "material": fixtureMaterial,
		}}}},
	}
	return fixture{doc: doc, bin: bin.Bytes()}
}

func (f fixture) glb(t *testing.T) []byte {
	t.Helper()
	if f.mess != nil {
		f.mess(f.doc)
	}
	js, err := json.Marshal(f.doc)
	if err != nil {
		t.Fatal(err)
	}
	for len(js)%chunkAlign != 0 {
		js = append(js, padByte)
	}
	var body bytes.Buffer
	chunkBytes(&body, chunkTypeJSON, js)
	for _, c := range f.before {
		chunkBytes(&body, c.kind, c.payload)
	}
	chunkBytes(&body, chunkTypeBin, f.bin)
	for _, c := range f.after {
		chunkBytes(&body, c.kind, c.payload)
	}
	var out bytes.Buffer
	w := func(v uint32) { _ = binary.Write(&out, binary.LittleEndian, v) }
	w(magic)
	w(versionTwo)
	w(uint32(headerSize + body.Len()))
	out.Write(body.Bytes())
	return out.Bytes()
}

func TestReadCountsAndValues(t *testing.T) {
	f, err := Read(baseFixture().glb(t))
	if err != nil {
		t.Fatal(err)
	}
	if len(f.Meshes) != 1 || f.Meshes[0].Name != fixtureMeshName || len(f.Meshes[0].Primitives) != 1 {
		t.Fatalf("meshes: %+v", f.Meshes)
	}
	p := f.Meshes[0].Primitives[0]
	if len(p.Positions) != fixtureVertices || len(p.Normals) != fixtureVertices || len(p.UVs) != fixtureVertices {
		t.Fatalf("vertex counts: %d positions, %d normals, %d uvs", len(p.Positions), len(p.Normals), len(p.UVs))
	}
	if len(p.Indices) != fixtureIndices {
		t.Fatalf("index count %d, want %d", len(p.Indices), fixtureIndices)
	}
	if p.Material != fixtureMaterial {
		t.Fatalf("material %d", p.Material)
	}
	if p.Positions[secondVertex] != [3]float32{posXStep, 1, 0} || p.Normals[0] != [3]float32{0, 0, 1} {
		t.Fatalf("values: %v %v", p.Positions[secondVertex], p.Normals[0])
	}
	if p.UVs[lastVertex] != [2]float32{lastVertex * uvScale, 1} || p.Indices[fixtureIndices-1] != lastVertex {
		t.Fatalf("uv %v last index %d", p.UVs[lastVertex], p.Indices[fixtureIndices-1])
	}
}

func TestOptionalAttributesAbsent(t *testing.T) {
	fx := baseFixture()
	fx.mess = func(d map[string]any) {
		prim := d["meshes"].([]any)[0].(map[string]any)["primitives"].([]any)[0].(map[string]any)
		prim["attributes"] = map[string]any{"POSITION": 0}
		delete(prim, "material")
	}
	f, err := Read(fx.glb(t))
	if err != nil {
		t.Fatal(err)
	}
	p := f.Meshes[0].Primitives[0]
	if p.Normals != nil || p.UVs != nil || p.Material != noMaterial {
		t.Fatalf("%+v", p)
	}
}

func TestEveryTruncationIsRefused(t *testing.T) {
	b := baseFixture().glb(t)
	for n := 0; n < len(b); n++ {
		if _, err := Read(b[:n]); err == nil {
			t.Fatalf("a %d-byte prefix of the %d-byte file was accepted", n, len(b))
		}
	}
}

func TestMalformedIsRefused(t *testing.T) {
	cases := map[string]struct {
		mess func(d map[string]any)
		want string
	}{
		"index out of range": {func(d map[string]any) {
			// the indices name vertex 3, and every vertex accessor is cut to two vertices
			for i := 0; i < lastVertex; i++ {
				d["accessors"].([]any)[i].(map[string]any)["count"] = twoVertices
			}
		}, "vertices"},
		"partial triangle": {func(d map[string]any) {
			d["accessors"].([]any)[lastVertex].(map[string]any)["count"] = fixtureIndices - 1
		}, "triangles"},
		"accessor overruns its view": {func(d map[string]any) {
			d["accessors"].([]any)[0].(map[string]any)["count"] = fixtureVertices + 1
		}, "overrun"},
		"view outside the buffer": {func(d map[string]any) {
			d["bufferViews"].([]any)[0].(map[string]any)["byteLength"] = math.MaxInt32
		}, "outside"},
		"integer positions": {func(d map[string]any) {
			d["accessors"].([]any)[0].(map[string]any)["componentType"] = componentU16
		}, "want float"},
		"wrong type": {func(d map[string]any) {
			d["accessors"].([]any)[1].(map[string]any)["type"] = "VEC2"
		}, "type"},
		"no accessor": {func(d map[string]any) {
			d["meshes"].([]any)[0].(map[string]any)["primitives"].([]any)[0].(map[string]any)["indices"] = badIndex
		}, "does not exist"},
		"negative count": {func(d map[string]any) {
			d["accessors"].([]any)[0].(map[string]any)["count"] = negativeCount
		}, "count"},
		"wrong asset version": {func(d map[string]any) {
			d["asset"] = map[string]any{"version": "1.0"}
		}, "version"},
		"not triangles": {func(d map[string]any) {
			d["meshes"].([]any)[0].(map[string]any)["primitives"].([]any)[0].(map[string]any)["mode"] = modeLines
		}, "only triangles"},
		"sparse accessor": {func(d map[string]any) {
			d["accessors"].([]any)[0].(map[string]any)["sparse"] = map[string]any{"count": 1}
		}, "sparse"},
		"view in another buffer": {func(d map[string]any) {
			d["bufferViews"].([]any)[0].(map[string]any)["buffer"] = secondBuffer
		}, "only buffer 0"},
		"fewer normals than positions": {func(d map[string]any) {
			d["accessors"].([]any)[1].(map[string]any)["count"] = shortCount
		}, "NORMAL has"},
		"fewer uvs than positions": {func(d map[string]any) {
			d["accessors"].([]any)[2].(map[string]any)["count"] = shortCount
		}, "TEXCOORD_0 has"},
		"external buffer": {func(d map[string]any) {
			d["buffers"].([]any)[0].(map[string]any)["uri"] = "quad.bin"
		}, "uri"},
		"buffer longer than the BIN chunk": {func(d map[string]any) {
			b := d["buffers"].([]any)[0].(map[string]any)
			b["byteLength"] = b["byteLength"].(int) + chunkAlign
		}, "exceeds"},
		"view past the buffer's byteLength": {func(d map[string]any) {
			// the BIN chunk holds the index view, but buffer 0 says it ends before it
			b := d["buffers"].([]any)[0].(map[string]any)
			b["byteLength"] = b["byteLength"].(int) - chunkAlign
		}, "outside"},
		"byteStride not a multiple of 4": {func(d map[string]any) {
			d["bufferViews"].([]any)[0].(map[string]any)["byteStride"] = oddStride
		}, "byteStride"},
		"byteStride below the element size": {func(d map[string]any) {
			d["bufferViews"].([]any)[0].(map[string]any)["byteStride"] = smallStride
		}, "byteStride"},
		"accessor with no bufferView": {func(d map[string]any) {
			delete(d["accessors"].([]any)[0].(map[string]any), "bufferView")
		}, "no bufferView"},
		"accessor names a missing bufferView": {func(d map[string]any) {
			d["accessors"].([]any)[0].(map[string]any)["bufferView"] = badIndex
		}, "names bufferView"},
		"accessor byteOffset past its view": {func(d map[string]any) {
			d["accessors"].([]any)[0].(map[string]any)["byteOffset"] = fixtureVertices*posBytes + chunkAlign
		}, "byteOffset"},
		"float indices": {func(d map[string]any) {
			d["accessors"].([]any)[lastVertex].(map[string]any)["componentType"] = componentF32
		}, "not an index type"},
		"no POSITION": {func(d map[string]any) {
			d["meshes"].([]any)[0].(map[string]any)["primitives"].([]any)[0].(map[string]any)["attributes"] = map[string]any{"NORMAL": 1}
		}, "no POSITION"},
		"not indexed": {func(d map[string]any) {
			delete(d["meshes"].([]any)[0].(map[string]any)["primitives"].([]any)[0].(map[string]any), "indices")
		}, "not indexed"},
	}
	for name, c := range cases {
		fx := baseFixture()
		fx.mess = c.mess
		_, err := Read(fx.glb(t))
		if err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: err = %v, want one containing %q", name, err, c.want)
		}
	}
}

func TestContainerErrors(t *testing.T) {
	good := baseFixture().glb(t)
	bad := func(edit func(b []byte)) []byte {
		b := append([]byte(nil), good...)
		edit(b)
		return b
	}
	cases := map[string][]byte{
		"magic":         bad(func(b []byte) { b[0] = 'X' }),
		"version":       bad(func(b []byte) { b[offVersion] = versionTwo + 1 }),
		"total length":  bad(func(b []byte) { b[offLength]++ }),
		"first chunk":   bad(func(b []byte) { b[headerSize+offChunkType] = 'X' }),
		"chunk too big": bad(func(b []byte) { b[headerSize+chunkLengthHigh] = math.MaxUint8 }),
	}
	// four bytes after the last chunk: too few for a chunk header
	trailing := append(append([]byte(nil), good...), make([]byte, chunkAlign)...)
	binary.LittleEndian.PutUint32(trailing[offLength:], uint32(len(trailing)))
	cases["trailing bytes"] = trailing
	for name, b := range cases {
		if _, err := Read(b); err == nil {
			t.Errorf("%s: accepted", name)
		}
	}
}

// TestHeaderOnlyIsRefused reads a GLB that is a valid header and no chunk at all.
func TestHeaderOnlyIsRefused(t *testing.T) {
	b := make([]byte, headerSize)
	binary.LittleEndian.PutUint32(b, magic)
	binary.LittleEndian.PutUint32(b[offVersion:], versionTwo)
	binary.LittleEndian.PutUint32(b[offLength:], headerSize)
	if _, err := Read(b); err == nil || !strings.Contains(err.Error(), "no JSON chunk") {
		t.Fatalf("err = %v, want one containing %q", err, "no JSON chunk")
	}
}

// TestInterleavedStride reads POSITION and NORMAL from one view at a stride wider than both, with a gap of unread
// bytes after each vertex, the layout of an exported model's interleaved vertex buffer.
func TestInterleavedStride(t *testing.T) {
	fx := baseFixture()
	var data bytes.Buffer
	for i := 0; i < fixtureVertices; i++ {
		for _, v := range []float32{float32(i) * posXStep, float32(i), 0, 0, float32(i), 1} {
			_ = binary.Write(&data, binary.LittleEndian, v)
		}
		data.Write(bytes.Repeat([]byte{junkByte}, gapBytes))
	}
	stride := attrSets*posBytes + gapBytes
	view := fx.addView(data.Bytes(), stride)
	posA := fx.addAccessor(map[string]any{"bufferView": view, "componentType": componentF32, "count": fixtureVertices, "type": "VEC3"})
	nrmA := fx.addAccessor(map[string]any{"bufferView": view, "byteOffset": posBytes, "componentType": componentF32, "count": fixtureVertices, "type": "VEC3"})
	fx.prim()["attributes"] = map[string]any{"POSITION": posA, "NORMAL": nrmA}
	f, err := Read(fx.glb(t))
	if err != nil {
		t.Fatal(err)
	}
	p := f.Meshes[0].Primitives[0]
	for i := 0; i < fixtureVertices; i++ {
		wantPos := [3]float32{float32(i) * posXStep, float32(i), 0}
		wantNrm := [3]float32{0, float32(i), 1}
		if p.Positions[i] != wantPos || p.Normals[i] != wantNrm {
			t.Fatalf("vertex %d: position %v normal %v, want %v %v", i, p.Positions[i], p.Normals[i], wantPos, wantNrm)
		}
	}
}

// TestIndexWidths reads the same triangles as u8 and as u32 indices.
func TestIndexWidths(t *testing.T) {
	want := []uint32{0, secondVertex, thirdVertex, thirdVertex, secondVertex, lastVertex}
	cases := map[string]struct {
		component int
		put       func(b *bytes.Buffer, v uint32)
	}{
		"u8":  {componentU8, func(b *bytes.Buffer, v uint32) { b.WriteByte(byte(v)) }},
		"u32": {componentU32, func(b *bytes.Buffer, v uint32) { _ = binary.Write(b, binary.LittleEndian, v) }},
	}
	for name, c := range cases {
		fx := baseFixture()
		var data bytes.Buffer
		for _, v := range want {
			c.put(&data, v)
		}
		view := fx.addView(data.Bytes(), 0)
		fx.prim()["indices"] = fx.addAccessor(map[string]any{"bufferView": view, "componentType": c.component, "count": len(want), "type": "SCALAR"})
		f, err := Read(fx.glb(t))
		if err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		got := f.Meshes[0].Primitives[0].Indices
		if len(got) != len(want) {
			t.Fatalf("%s: indices %v, want %v", name, got, want)
		}
		for i := range want {
			if got[i] != want[i] {
				t.Fatalf("%s: indices %v, want %v", name, got, want)
			}
		}
	}
}

// TestOnlyTheFirstBinChunkIsRead puts a chunk of another type before the BIN chunk and a second BIN chunk of junk
// after it; the values are the first BIN chunk's.
func TestOnlyTheFirstBinChunkIsRead(t *testing.T) {
	fx := baseFixture()
	junk := bytes.Repeat([]byte{junkByte}, len(fx.bin))
	fx.before = []rawChunk{{kind: chunkTypeJSON, payload: junk}}
	fx.after = []rawChunk{{kind: chunkTypeBin, payload: junk}}
	f, err := Read(fx.glb(t))
	if err != nil {
		t.Fatal(err)
	}
	p := f.Meshes[0].Primitives[0]
	if p.Positions[secondVertex] != [3]float32{posXStep, 1, 0} || p.Indices[fixtureIndices-1] != lastVertex {
		t.Fatalf("values: %v, last index %d", p.Positions[secondVertex], p.Indices[fixtureIndices-1])
	}
}
