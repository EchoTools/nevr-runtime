// Package gltf reads the meshes of a binary glTF 2.0 file (.glb), for the content-pack build's drone model step
// (PLAN-v5 10.1, G-MODEL-1). Cat's fix_drone_model.py (`read_glb`) reads, per mesh primitive, the POSITION, NORMAL and
// TEXCOORD_0 accessors and the index accessor, and the mesh name and material index; that is all this reads. Materials'
// textures, node transforms and animations are not read.
//
// A GLB is a 12-byte header (magic "glTF", version 2, total length), then chunks of an 8-byte header (length, type) and
// a payload padded to 4 bytes: the first is JSON, an optional second is the BIN buffer that buffer 0 names. Every offset,
// count and stride is checked against the chunk it points into before any byte is read, and a malformed file is an error,
// never a panic or a partial result.
package gltf

import (
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"os"
)

const (
	magic          = 0x46546C67 // "glTF", little endian
	versionTwo     = 2
	headerSize     = 12
	chunkHeader    = 8
	chunkAlign     = 4
	chunkTypeJSON  = 0x4E4F534A // "JSON"
	chunkTypeBin   = 0x004E4942 // "BIN\0"
	componentU8    = 5121
	componentU16   = 5123
	componentU32   = 5125
	componentF32   = 5126
	typeScalar     = "SCALAR"
	typeVec2       = "VEC2"
	typeVec3       = "VEC3"
	floatBytes     = 4
	modeTriangles  = 4
	noMaterial     = -1
	attrPosition   = "POSITION"
	attrNormal     = "NORMAL"
	attrTexcoord0  = "TEXCOORD_0"
	maxByteStride  = 252 // glTF 2.0 bufferView.byteStride (bufferView.schema.json): minimum 4, maximum 252, multiple of 4
	strideMultiple = 4
	minByteStride  = 4
	offVersion     = 4
	offLength      = 8
	offChunkType   = 4
	vec3Size       = 3
	vec2Size       = 2
	u8Bytes        = 1
	u16Bytes       = 2
	u32Bytes       = 4
	triangleSize   = 3
)

// File is the meshes of one GLB, in file order.
type File struct {
	Meshes []Mesh
}

// Mesh is one glTF mesh and its primitives.
type Mesh struct {
	Name       string
	Primitives []Primitive
}

// Primitive is one triangle list. Normals and UVs are nil when the primitive has no such attribute; Positions and
// Indices are always present. Every index is below len(Positions), and len(Indices) is a multiple of 3.
type Primitive struct {
	Positions [][3]float32
	Normals   [][3]float32
	UVs       [][2]float32
	Indices   []uint32
	Material  int // the glTF material index, -1 when the primitive names none
}

type accessorJSON struct {
	BufferView    *int            `json:"bufferView"`
	ByteOffset    int64           `json:"byteOffset"`
	ComponentType int             `json:"componentType"`
	Count         int64           `json:"count"`
	Type          string          `json:"type"`
	Sparse        json.RawMessage `json:"sparse"`
}

type viewJSON struct {
	Buffer     int   `json:"buffer"`
	ByteOffset int64 `json:"byteOffset"`
	ByteLength int64 `json:"byteLength"`
	ByteStride int64 `json:"byteStride"`
}

type primitiveJSON struct {
	Attributes map[string]int `json:"attributes"`
	Indices    *int           `json:"indices"`
	Material   *int           `json:"material"`
	Mode       *int           `json:"mode"`
}

type docJSON struct {
	Asset struct {
		Version string `json:"version"`
	} `json:"asset"`
	Buffers []struct {
		ByteLength int64  `json:"byteLength"`
		URI        string `json:"uri"`
	} `json:"buffers"`
	BufferViews []viewJSON     `json:"bufferViews"`
	Accessors   []accessorJSON `json:"accessors"`
	Meshes      []struct {
		Name       string          `json:"name"`
		Primitives []primitiveJSON `json:"primitives"`
	} `json:"meshes"`
}

// ReadFile reads the GLB at path.
func ReadFile(path string) (*File, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	f, err := Read(b)
	if err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	return f, nil
}

// Read parses a GLB held in b.
func Read(b []byte) (*File, error) {
	jsonChunk, bin, err := chunks(b)
	if err != nil {
		return nil, err
	}
	var doc docJSON
	if err := json.Unmarshal(jsonChunk, &doc); err != nil {
		return nil, fmt.Errorf("glb JSON chunk: %w", err)
	}
	if doc.Asset.Version != "2.0" {
		return nil, fmt.Errorf("glb asset version %q, want 2.0", doc.Asset.Version)
	}
	if len(doc.Buffers) > 0 {
		buf := doc.Buffers[0]
		if buf.URI != "" {
			return nil, errors.New("glb buffer 0 has a uri; only the BIN chunk is read")
		}
		if buf.ByteLength < 0 || buf.ByteLength > int64(len(bin)) {
			return nil, fmt.Errorf("glb buffer 0 byteLength %d exceeds the BIN chunk's %d bytes", buf.ByteLength, len(bin))
		}
		bin = bin[:buf.ByteLength]
	}
	r := reader{doc: &doc, bin: bin}
	out := &File{}
	for mi, m := range doc.Meshes {
		mesh := Mesh{Name: m.Name}
		for pi, p := range m.Primitives {
			prim, err := r.primitive(p)
			if err != nil {
				return nil, fmt.Errorf("glb mesh %d (%q) primitive %d: %w", mi, m.Name, pi, err)
			}
			mesh.Primitives = append(mesh.Primitives, prim)
		}
		out.Meshes = append(out.Meshes, mesh)
	}
	return out, nil
}

// chunks splits a GLB into its JSON chunk and BIN chunk (nil when there is none).
func chunks(b []byte) (jsonChunk, bin []byte, err error) {
	if len(b) < headerSize {
		return nil, nil, fmt.Errorf("glb is %d bytes, shorter than its %d-byte header", len(b), headerSize)
	}
	if binary.LittleEndian.Uint32(b) != magic {
		return nil, nil, errors.New("not a glb: bad magic")
	}
	if v := binary.LittleEndian.Uint32(b[offVersion:]); v != versionTwo {
		return nil, nil, fmt.Errorf("glb container version %d, want %d", v, versionTwo)
	}
	if total := binary.LittleEndian.Uint32(b[offLength:]); uint64(total) != uint64(len(b)) {
		return nil, nil, fmt.Errorf("glb header says %d bytes, file has %d (truncated or padded)", total, len(b))
	}
	pos := uint64(headerSize)
	for n := 0; pos < uint64(len(b)); n++ {
		if uint64(len(b))-pos < chunkHeader {
			return nil, nil, fmt.Errorf("glb chunk %d header at %d runs past the end", n, pos)
		}
		length := uint64(binary.LittleEndian.Uint32(b[pos:]))
		kind := binary.LittleEndian.Uint32(b[pos+offChunkType:])
		start := pos + chunkHeader
		if length > uint64(len(b))-start {
			return nil, nil, fmt.Errorf("glb chunk %d claims %d bytes at %d, file ends at %d", n, length, start, len(b))
		}
		payload := b[start : start+length]
		switch {
		case n == 0:
			if kind != chunkTypeJSON {
				return nil, nil, fmt.Errorf("glb first chunk has type %#x, want JSON", kind)
			}
			jsonChunk = payload
		case kind == chunkTypeBin && bin == nil:
			bin = payload
		}
		pos = start + length
		if pad := pos % chunkAlign; pad != 0 {
			pos += chunkAlign - pad
		}
	}
	if jsonChunk == nil {
		return nil, nil, errors.New("glb has no JSON chunk")
	}
	return jsonChunk, bin, nil
}

type reader struct {
	doc *docJSON
	bin []byte
}

func (r *reader) primitive(p primitiveJSON) (Primitive, error) {
	prim := Primitive{Material: noMaterial}
	if p.Mode != nil && *p.Mode != modeTriangles {
		return prim, fmt.Errorf("mode %d, only triangles (%d) are read", *p.Mode, modeTriangles)
	}
	if p.Material != nil {
		prim.Material = *p.Material
	}
	pos, ok := p.Attributes[attrPosition]
	if !ok {
		return prim, errors.New("no POSITION attribute")
	}
	v3, err := r.floats(pos, typeVec3, vec3Size)
	if err != nil {
		return prim, fmt.Errorf("POSITION: %w", err)
	}
	for i := 0; i < len(v3); i += vec3Size {
		prim.Positions = append(prim.Positions, [3]float32(v3[i:i+vec3Size]))
	}
	if n, ok := p.Attributes[attrNormal]; ok {
		f, err := r.floats(n, typeVec3, vec3Size)
		if err != nil {
			return prim, fmt.Errorf("NORMAL: %w", err)
		}
		for i := 0; i < len(f); i += vec3Size {
			prim.Normals = append(prim.Normals, [3]float32(f[i:i+vec3Size]))
		}
		if len(prim.Normals) != len(prim.Positions) {
			return prim, fmt.Errorf("NORMAL has %d elements, POSITION %d", len(prim.Normals), len(prim.Positions))
		}
	}
	if t, ok := p.Attributes[attrTexcoord0]; ok {
		f, err := r.floats(t, typeVec2, vec2Size)
		if err != nil {
			return prim, fmt.Errorf("TEXCOORD_0: %w", err)
		}
		for i := 0; i < len(f); i += vec2Size {
			prim.UVs = append(prim.UVs, [2]float32(f[i:i+vec2Size]))
		}
		if len(prim.UVs) != len(prim.Positions) {
			return prim, fmt.Errorf("TEXCOORD_0 has %d elements, POSITION %d", len(prim.UVs), len(prim.Positions))
		}
	}
	if p.Indices == nil {
		return prim, errors.New("not indexed: no indices accessor")
	}
	prim.Indices, err = r.indices(*p.Indices)
	if err != nil {
		return prim, fmt.Errorf("indices: %w", err)
	}
	if len(prim.Indices)%triangleSize != 0 {
		return prim, fmt.Errorf("%d indices are not whole triangles", len(prim.Indices))
	}
	for i, ix := range prim.Indices {
		if uint64(ix) >= uint64(len(prim.Positions)) {
			return prim, fmt.Errorf("index %d is %d, but there are %d vertices", i, ix, len(prim.Positions))
		}
	}
	return prim, nil
}

// element locates an accessor's data: base offset into bin, stride, count, after every bounds check.
func (r *reader) element(ai int, wantType string, componentSize int, wantComponents ...int) (base, stride, count uint64, a accessorJSON, err error) {
	if ai < 0 || ai >= len(r.doc.Accessors) {
		return 0, 0, 0, a, fmt.Errorf("accessor %d does not exist (%d accessors)", ai, len(r.doc.Accessors))
	}
	a = r.doc.Accessors[ai]
	if a.Type != wantType {
		return 0, 0, 0, a, fmt.Errorf("accessor %d has type %q, want %q", ai, a.Type, wantType)
	}
	if len(a.Sparse) != 0 && string(a.Sparse) != "null" {
		return 0, 0, 0, a, fmt.Errorf("accessor %d is sparse, which is not read", ai)
	}
	if a.BufferView == nil {
		return 0, 0, 0, a, fmt.Errorf("accessor %d has no bufferView", ai)
	}
	if *a.BufferView < 0 || *a.BufferView >= len(r.doc.BufferViews) {
		return 0, 0, 0, a, fmt.Errorf("accessor %d names bufferView %d of %d", ai, *a.BufferView, len(r.doc.BufferViews))
	}
	bv := r.doc.BufferViews[*a.BufferView]
	if bv.Buffer != 0 {
		return 0, 0, 0, a, fmt.Errorf("bufferView %d is in buffer %d; only buffer 0 (the BIN chunk) is read", *a.BufferView, bv.Buffer)
	}
	if bv.ByteOffset < 0 || bv.ByteLength < 0 || bv.ByteOffset > int64(len(r.bin)) || bv.ByteLength > int64(len(r.bin))-bv.ByteOffset {
		return 0, 0, 0, a, fmt.Errorf("bufferView %d (offset %d, length %d) is outside the %d-byte BIN buffer", *a.BufferView, bv.ByteOffset, bv.ByteLength, len(r.bin))
	}
	if a.Count <= 0 {
		return 0, 0, 0, a, fmt.Errorf("accessor %d count %d", ai, a.Count)
	}
	elem := uint64(componentSize)
	for _, c := range wantComponents {
		elem *= uint64(c)
	}
	st := elem
	if bv.ByteStride != 0 {
		if bv.ByteStride < minByteStride || bv.ByteStride > maxByteStride || bv.ByteStride%strideMultiple != 0 || uint64(bv.ByteStride) < elem {
			return 0, 0, 0, a, fmt.Errorf("bufferView %d byteStride %d is invalid for %d-byte elements", *a.BufferView, bv.ByteStride, elem)
		}
		st = uint64(bv.ByteStride)
	}
	if a.ByteOffset < 0 || a.ByteOffset > bv.ByteLength {
		return 0, 0, 0, a, fmt.Errorf("accessor %d byteOffset %d is outside its bufferView's %d bytes", ai, a.ByteOffset, bv.ByteLength)
	}
	room := uint64(bv.ByteLength - a.ByteOffset)
	if room < elem || uint64(a.Count-1) > (room-elem)/st {
		return 0, 0, 0, a, fmt.Errorf("accessor %d: %d elements of %d bytes at stride %d overrun its bufferView (%d bytes after its offset)", ai, a.Count, elem, st, room)
	}
	return uint64(bv.ByteOffset + a.ByteOffset), st, uint64(a.Count), a, nil
}

// floats reads a float accessor of `comps` components, flattened.
func (r *reader) floats(ai int, typ string, comps int) ([]float32, error) {
	base, stride, count, a, err := r.element(ai, typ, floatBytes, comps)
	if err != nil {
		return nil, err
	}
	if a.ComponentType != componentF32 {
		return nil, fmt.Errorf("accessor %d component type %d, want float (%d)", ai, a.ComponentType, componentF32)
	}
	out := make([]float32, 0, count*uint64(comps))
	for e := uint64(0); e < count; e++ {
		for c := 0; c < comps; c++ {
			bits := binary.LittleEndian.Uint32(r.bin[base+e*stride+uint64(c)*floatBytes:])
			out = append(out, math.Float32frombits(bits))
		}
	}
	return out, nil
}

func (r *reader) indices(ai int) ([]uint32, error) {
	if ai < 0 || ai >= len(r.doc.Accessors) {
		return nil, fmt.Errorf("accessor %d does not exist (%d accessors)", ai, len(r.doc.Accessors))
	}
	size := 0
	switch r.doc.Accessors[ai].ComponentType {
	case componentU8:
		size = u8Bytes
	case componentU16:
		size = u16Bytes
	case componentU32:
		size = u32Bytes
	default:
		return nil, fmt.Errorf("accessor %d component type %d is not an index type", ai, r.doc.Accessors[ai].ComponentType)
	}
	base, stride, count, _, err := r.element(ai, typeScalar, size)
	if err != nil {
		return nil, err
	}
	out := make([]uint32, count)
	for e := uint64(0); e < count; e++ {
		o := base + e*stride
		switch size {
		case u8Bytes:
			out[e] = uint32(r.bin[o])
		case u16Bytes:
			out[e] = uint32(binary.LittleEndian.Uint16(r.bin[o:]))
		default:
			out[e] = binary.LittleEndian.Uint32(r.bin[o:])
		}
	}
	return out, nil
}
