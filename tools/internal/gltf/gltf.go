// Package gltf reads the meshes of a binary glTF 2.0 file (.glb), for the content-pack build's drone model step
// (PLAN-v5 10.1, G-MODEL-1). The file is decoded by github.com/qmuntal/gltf (BSD-2-Clause): the library reads the GLB
// container, the JSON document and the BIN chunk, and its modeler reads each accessor's values. This package checks what
// the library does not: the GLB header's total length against the file's size, the asset version, the bufferView and
// accessor bounds, and the primitive shape the model step reads (triangles, indexed, a POSITION, and NORMAL and
// TEXCOORD_0 of the same count when present). A malformed file is an error, never a panic or a partial result.
//
// The decoded document is kept in File, unexported, for the steps that read more of it (nodes, materials, textures).
package gltf

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"os"

	"github.com/qmuntal/gltf"
	"github.com/qmuntal/gltf/modeler"
)

const (
	magic          = 0x46546C67 // "glTF", little endian
	headerSize     = 12         // magic, version, total length
	chunkHeader    = 8          // length, type: the first chunk's header follows the GLB header
	offLength      = 8          // the GLB header's total length, a uint32 at this offset
	glbMinimumSize = headerSize + chunkHeader
	assetVersion   = "2.0"
	noMaterial     = -1
	attrPosition   = gltf.POSITION
	attrNormal     = gltf.NORMAL
	attrTexcoord0  = gltf.TEXCOORD_0
	maxByteStride  = 252 // glTF 2.0 bufferView.byteStride (bufferView.schema.json): minimum 4, maximum 252, multiple of 4
	strideMultiple = 4
	minByteStride  = 4
	triangleSize   = 3
)

// File is the meshes of one GLB, in file order, and the document they were read from.
type File struct {
	Meshes []Mesh
	doc    *gltf.Document
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
	if err := checkContainer(b); err != nil {
		return nil, err
	}
	doc := new(gltf.Document)
	if err := gltf.NewDecoder(bytes.NewReader(b)).Decode(doc); err != nil {
		return nil, fmt.Errorf("glb: %w", err)
	}
	if doc.Asset.Version != assetVersion {
		return nil, fmt.Errorf("glb asset version %q, want %s", doc.Asset.Version, assetVersion)
	}
	if len(doc.Buffers) > 0 && doc.Buffers[0].URI != "" {
		return nil, errors.New("glb buffer 0 has a uri; only the BIN chunk is read")
	}
	r := reader{doc: doc}
	if len(doc.Buffers) > 0 {
		r.bin = doc.Buffers[0].ByteLength
	}
	out := &File{doc: doc}
	for mi, m := range doc.Meshes {
		if m == nil {
			return nil, fmt.Errorf("glb mesh %d is null", mi)
		}
		mesh := Mesh{Name: m.Name}
		for pi, p := range m.Primitives {
			if p == nil {
				return nil, fmt.Errorf("glb mesh %d (%q) primitive %d is null", mi, m.Name, pi)
			}
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

// checkContainer refuses what the library's decoder accepts and this package must not: a file shorter than its header,
// one that is not glTF binary, one too short to hold a first chunk header, and one whose header claims another length
// than the file has. The library checks the chunks themselves (the version, the JSON chunk's type and length, the BIN
// chunk's length against buffer 0).
func checkContainer(b []byte) error {
	if len(b) < headerSize {
		return fmt.Errorf("glb is %d bytes, shorter than its %d-byte header", len(b), headerSize)
	}
	if binary.LittleEndian.Uint32(b) != magic {
		return errors.New("not a glb: bad magic")
	}
	if len(b) < glbMinimumSize {
		return fmt.Errorf("glb is %d bytes, too short for a chunk after its header: no JSON chunk", len(b))
	}
	if total := binary.LittleEndian.Uint32(b[offLength:]); uint64(total) != uint64(len(b)) {
		return fmt.Errorf("glb header says %d bytes, file has %d (truncated or padded)", total, len(b))
	}
	return nil
}

// reader holds the decoded document and the byte length of buffer 0, the BIN chunk every bufferView indexes.
type reader struct {
	doc *gltf.Document
	bin int
}

func (r reader) primitive(p *gltf.Primitive) (Primitive, error) {
	prim := Primitive{Material: noMaterial}
	if p.Mode != gltf.PrimitiveTriangles {
		return prim, fmt.Errorf("mode %s, only triangles (%s) are read", p.Mode, gltf.PrimitiveTriangles)
	}
	if p.Material != nil {
		prim.Material = *p.Material
	}
	pos, ok := p.Attributes[attrPosition]
	if !ok {
		return prim, errors.New("no POSITION attribute")
	}
	acr, err := r.floatAccessor(pos, gltf.AccessorVec3)
	if err != nil {
		return prim, fmt.Errorf("POSITION: %w", err)
	}
	if prim.Positions, err = modeler.ReadPosition(r.doc, acr, nil); err != nil {
		return prim, fmt.Errorf("POSITION: %w", err)
	}
	if n, ok := p.Attributes[attrNormal]; ok {
		acr, err := r.floatAccessor(n, gltf.AccessorVec3)
		if err != nil {
			return prim, fmt.Errorf("NORMAL: %w", err)
		}
		if prim.Normals, err = modeler.ReadNormal(r.doc, acr, nil); err != nil {
			return prim, fmt.Errorf("NORMAL: %w", err)
		}
		if len(prim.Normals) != len(prim.Positions) {
			return prim, fmt.Errorf("NORMAL has %d elements, POSITION %d", len(prim.Normals), len(prim.Positions))
		}
	}
	if t, ok := p.Attributes[attrTexcoord0]; ok {
		acr, err := r.floatAccessor(t, gltf.AccessorVec2)
		if err != nil {
			return prim, fmt.Errorf("TEXCOORD_0: %w", err)
		}
		if prim.UVs, err = modeler.ReadTextureCoord(r.doc, acr, nil); err != nil {
			return prim, fmt.Errorf("TEXCOORD_0: %w", err)
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

// lookup returns accessor ai, after its index is checked against the document.
func (r reader) lookup(ai int) (*gltf.Accessor, error) {
	if ai < 0 || ai >= len(r.doc.Accessors) {
		return nil, fmt.Errorf("accessor %d does not exist (%d accessors)", ai, len(r.doc.Accessors))
	}
	acr := r.doc.Accessors[ai]
	if acr == nil {
		return nil, fmt.Errorf("accessor %d is null", ai)
	}
	return acr, nil
}

// floatAccessor checks that accessor ai is of type want and of float components, and that it lies in its bufferView.
func (r reader) floatAccessor(ai int, want gltf.AccessorType) (*gltf.Accessor, error) {
	acr, err := r.lookup(ai)
	if err != nil {
		return nil, err
	}
	if acr.ComponentType != gltf.ComponentFloat {
		return nil, fmt.Errorf("accessor %d component type %s, want float (%s)", ai, acr.ComponentType, gltf.ComponentFloat)
	}
	return r.accessor(ai, want)
}

// accessor checks accessor ai against the document before any value is read: its type, that it is dense, that its
// bufferView names buffer 0, that the bufferView lies in the BIN chunk, the count, the byte stride, and that every
// element it names lies in the bufferView. The component type is the caller's to check.
func (r reader) accessor(ai int, want gltf.AccessorType) (*gltf.Accessor, error) {
	acr, err := r.lookup(ai)
	if err != nil {
		return nil, err
	}
	if acr.Type != want {
		return nil, fmt.Errorf("accessor %d has type %q, want %q", ai, acr.Type, want)
	}
	if acr.Sparse != nil {
		return nil, fmt.Errorf("accessor %d is sparse, which is not read", ai)
	}
	if acr.BufferView == nil {
		return nil, fmt.Errorf("accessor %d has no bufferView", ai)
	}
	if *acr.BufferView < 0 || *acr.BufferView >= len(r.doc.BufferViews) {
		return nil, fmt.Errorf("accessor %d names bufferView %d of %d", ai, *acr.BufferView, len(r.doc.BufferViews))
	}
	bv := r.doc.BufferViews[*acr.BufferView]
	if bv == nil {
		return nil, fmt.Errorf("bufferView %d is null", *acr.BufferView)
	}
	if bv.Buffer != 0 {
		return nil, fmt.Errorf("bufferView %d is in buffer %d; only buffer 0 (the BIN chunk) is read", *acr.BufferView, bv.Buffer)
	}
	if bv.ByteOffset < 0 || bv.ByteLength < 0 || bv.ByteOffset > r.bin || bv.ByteLength > r.bin-bv.ByteOffset {
		return nil, fmt.Errorf("bufferView %d (offset %d, length %d) is outside the %d-byte BIN buffer", *acr.BufferView, bv.ByteOffset, bv.ByteLength, r.bin)
	}
	elem := gltf.SizeOfElement(acr.ComponentType, want)
	stride := elem
	if bv.ByteStride != 0 {
		if bv.ByteStride < minByteStride || bv.ByteStride > maxByteStride || bv.ByteStride%strideMultiple != 0 || bv.ByteStride < elem {
			return nil, fmt.Errorf("bufferView %d byteStride %d is invalid for %d-byte elements", *acr.BufferView, bv.ByteStride, elem)
		}
		stride = bv.ByteStride
	}
	if acr.ByteOffset < 0 || acr.ByteOffset > bv.ByteLength {
		return nil, fmt.Errorf("accessor %d byteOffset %d is outside its bufferView's %d bytes", ai, acr.ByteOffset, bv.ByteLength)
	}
	room := bv.ByteLength - acr.ByteOffset
	if room < elem || acr.Count-1 > (room-elem)/stride {
		return nil, fmt.Errorf("accessor %d: %d elements of %d bytes at stride %d overrun its bufferView (%d bytes after its offset)", ai, acr.Count, elem, stride, room)
	}
	return acr, nil
}

// indices reads the index accessor ai, which must hold unsigned bytes, shorts or ints.
func (r reader) indices(ai int) ([]uint32, error) {
	acr, err := r.lookup(ai)
	if err != nil {
		return nil, err
	}
	switch acr.ComponentType {
	case gltf.ComponentUbyte, gltf.ComponentUshort, gltf.ComponentUint:
	default:
		return nil, fmt.Errorf("accessor %d component type %s is not an index type", ai, acr.ComponentType)
	}
	if acr, err = r.accessor(ai, gltf.AccessorScalar); err != nil {
		return nil, err
	}
	return modeler.ReadIndices(r.doc, acr, nil)
}
