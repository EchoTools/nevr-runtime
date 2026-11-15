// Package axmlfix builds binary AndroidManifest.xml documents (Android's compiled XML, "AXML") for the tests of the Quest
// APK tools. It is the Go twin of tools/tests/axml_fixtures.py.
//
// The layout is the one aapt2 writes (frameworks/base/libs/androidfw/include/androidfw/ResourceTypes.h): an XML document
// chunk holding a string pool, a resource map (one resource id per leading string: the id is what Android looks an
// attribute up by, the name is only a label), then the namespace and element chunks. The package keeps its own copies of
// the format's constants, so a builder that misreads the format is not checked against the same misreading: aapt2, a
// reader that is not ours, is the check.
package axmlfix

import (
	"encoding/binary"
	"fmt"
	"unicode/utf16"
)

// AndroidNS is the android namespace URI of an attribute's namespace.
const AndroidNS = "http://schemas.android.com/apk/res/android"

// Resource ids of the attributes the fixtures name (android.R.attr).
const (
	ResVersionCode      uint32 = 0x0101021B
	ResVersionName      uint32 = 0x0101021C
	ResVersionCodeMajor uint32 = 0x01010576
	ResMinSDK           uint32 = 0x0101020C
	ResLabel            uint32 = 0x01010001
)

// Attribute value types and the absent-index marker.
const (
	TypeString uint8  = 0x03
	TypeIntDec uint8  = 0x10
	TypeIntHex uint8  = 0x11
	NoIndex    uint32 = 0xFFFFFFFF
)

// Chunk kinds and sizes the builder writes.
const (
	chunkXML               uint16 = 0x0003
	chunkStringPool        uint16 = 0x0001
	chunkResourceMap       uint16 = 0x0180
	chunkStartNS           uint16 = 0x0100
	chunkEndNS             uint16 = 0x0101
	chunkStartElement      uint16 = 0x0102
	chunkEndElement        uint16 = 0x0103
	chunkHeaderBytes              = 8
	elementHeaderBytes            = 16
	stringPoolHeader              = 28
	wordBytes                     = 4
	attrRecordBytes               = 20
	attrStartOffset               = 0x14
	stringPoolUTF8Flag            = 0x100
	firstLineNumber               = 1
	minSDKValue                   = 29
	versionCodeMajorValue         = 1
	nestedVersionCodeValue        = 7
	longLenFlag8                  = 0x80
	longLenFlag16                 = 0x8000
	byteMask                      = 0xFF
	halfWordMask                  = 0xFFFF
	shift8                        = 8
	shift16                       = 16
	emptyAttrSize                 = 8
	shortLenLimit8                = 0x80
	shortLenLimit16               = 0x8000
	uint16Bytes                   = 2
	packageDefault                = "com.readyatdawn.r15"
	usesSDKName                   = "uses-sdk"
	manifestRoot                  = "manifest"
	defaultVersionCode            = 4987566
)

// Spec is the shape of one manifest. Default is the store APK's: <manifest versionCode versionName package> with a
// <uses-sdk minSdkVersion/> child, UTF-8 pool, one versionCode. The fields plant one defect each for a validator's
// refusal tests.
type Spec struct {
	VersionCode          int64
	UTF8                 bool
	VersionName          string
	VersionCodeType      uint8
	VersionCodeRaw       uint32
	VersionCodeCount     int
	VersionCodeID        uint32
	VersionCodeNSAndroid bool
	WithVersionCodeMajor bool
	WithResourceMap      bool
	Root                 string
	NestedVersionCode    bool
	Trailing             []byte
}

// Default returns the spec of the store APK's manifest, versionCode 4987566.
func Default() Spec {
	return Spec{
		VersionCode:          defaultVersionCode,
		UTF8:                 true,
		VersionName:          fmt.Sprint(defaultVersionCode),
		VersionCodeType:      TypeIntDec,
		VersionCodeRaw:       NoIndex,
		VersionCodeCount:     1,
		VersionCodeID:        ResVersionCode,
		VersionCodeNSAndroid: true,
		WithResourceMap:      true,
		Root:                 manifestRoot,
	}
}

// Build returns the compiled manifest the spec describes.
func (s Spec) Build() []byte {
	names := []string{"versionCode", "versionName", "minSdkVersion", "versionCodeMajor"}
	ids := []uint32{s.VersionCodeID, ResVersionName, ResMinSDK, ResVersionCodeMajor}
	strs := append(append([]string{}, names...), "android", AndroidNS, s.Root, "package", packageDefault, usesSDKName, s.VersionName)
	idx := indexOf(strs)
	nsURI := NoIndex
	if s.VersionCodeNSAndroid {
		nsURI = idx[AndroidNS]
	}
	var attrs [][]byte
	for i := 0; i < s.VersionCodeCount; i++ {
		attrs = append(attrs, attr(nsURI, idx["versionCode"], s.VersionCodeRaw, s.VersionCodeType, uint32(s.VersionCode)))
	}
	attrs = append(attrs, attr(idx[AndroidNS], idx["versionName"], idx[s.VersionName], TypeString, idx[s.VersionName]))
	if s.WithVersionCodeMajor {
		attrs = append(attrs, attr(idx[AndroidNS], idx["versionCodeMajor"], NoIndex, TypeIntDec, versionCodeMajorValue))
	}
	attrs = append(attrs, attr(NoIndex, idx["package"], idx[packageDefault], TypeString, idx[packageDefault]))
	childAttrs := [][]byte{attr(idx[AndroidNS], idx["minSdkVersion"], NoIndex, TypeIntDec, minSDKValue)}
	if s.NestedVersionCode {
		childAttrs = append(childAttrs, attr(idx[AndroidNS], idx["versionCode"], NoIndex, TypeIntDec, nestedVersionCodeValue))
	}
	var body []byte
	body = append(body, stringPool(strs, s.UTF8)...)
	if s.WithResourceMap {
		body = append(body, resourceMap(ids)...)
	}
	body = append(body, startNS(idx["android"], idx[AndroidNS])...)
	body = append(body, startElement(NoIndex, idx[s.Root], attrs)...)
	body = append(body, startElement(NoIndex, idx[usesSDKName], childAttrs)...)
	body = append(body, endElement(NoIndex, idx[usesSDKName])...)
	body = append(body, endElement(NoIndex, idx[s.Root])...)
	body = append(body, endNS(idx["android"], idx[AndroidNS])...)
	body = append(body, s.Trailing...)
	return document(body)
}

// LabelSpec is a manifest with a UTF-16 string pool holding Package once and Label once:
// <manifest package><application android:label="LABEL"/></manifest>, the label a raw string attribute that no other
// attribute references (the shape the label editor edits in place). LabelID is the resource id the map gives the
// label attribute's name; ShareLabel adds a second string attribute on <manifest> that references the same pool
// string, so editing the label would rename that too. Both are for the refusal tests.
type LabelSpec struct {
	Package    string
	Label      string
	LabelID    uint32
	ShareLabel bool
}

// LabelManifest returns the label spec of the reference build's shape: the label id is android:label's.
func LabelManifest(pkg, label string) LabelSpec {
	return LabelSpec{Package: pkg, Label: label, LabelID: ResLabel}
}

// Build returns the compiled manifest the label spec describes.
func (l LabelSpec) Build() []byte {
	strs := []string{"label", "android", AndroidNS, manifestRoot, "package", l.Package, "application", l.Label}
	idx := indexOf(strs)
	rootAttrs := [][]byte{attr(NoIndex, idx["package"], idx[l.Package], TypeString, idx[l.Package])}
	if l.ShareLabel {
		rootAttrs = append(rootAttrs, attr(NoIndex, idx["label"], idx[l.Label], TypeString, idx[l.Label]))
	}
	appAttrs := [][]byte{attr(idx[AndroidNS], idx["label"], idx[l.Label], TypeString, idx[l.Label])}
	var body []byte
	body = append(body, stringPool(strs, false)...)
	body = append(body, resourceMap([]uint32{l.LabelID})...)
	body = append(body, startNS(idx["android"], idx[AndroidNS])...)
	body = append(body, startElement(NoIndex, idx[manifestRoot], rootAttrs)...)
	body = append(body, startElement(NoIndex, idx["application"], appAttrs)...)
	body = append(body, endElement(NoIndex, idx["application"])...)
	body = append(body, endElement(NoIndex, idx[manifestRoot])...)
	body = append(body, endNS(idx["android"], idx[AndroidNS])...)
	return document(body)
}

// document is the XML document chunk around a body of chunks.
func document(body []byte) []byte {
	out := binary.LittleEndian.AppendUint16(nil, chunkXML)
	out = binary.LittleEndian.AppendUint16(out, chunkHeaderBytes)
	out = binary.LittleEndian.AppendUint32(out, uint32(chunkHeaderBytes+len(body)))
	return append(out, body...)
}

// indexOf maps each string to its index; a repeated string takes its last index, as a Python dict comprehension does.
func indexOf(strs []string) map[string]uint32 {
	idx := make(map[string]uint32, len(strs))
	for i, s := range strs {
		idx[s] = uint32(i)
	}
	return idx
}

// chunk is one chunk: its kind, its header (header size bytes, of which the first eight are the chunk header) and its body.
func chunk(kind uint16, headerSize int, body, extra []byte) []byte {
	if headerSize != chunkHeaderBytes+len(extra) {
		panic(fmt.Sprintf("axmlfix: header size %d does not hold %d extra bytes", headerSize, len(extra)))
	}
	size := headerSize + len(body)
	if size%wordBytes != 0 {
		panic(fmt.Sprintf("axmlfix: chunk of %d bytes is not 4-aligned", size))
	}
	out := binary.LittleEndian.AppendUint16(nil, kind)
	out = binary.LittleEndian.AppendUint16(out, uint16(headerSize))
	out = binary.LittleEndian.AppendUint32(out, uint32(size))
	out = append(out, extra...)
	return append(out, body...)
}

// u8Len is a string length in the UTF-8 pool's one- or two-byte form.
func u8Len(n int) []byte {
	if n < shortLenLimit8 {
		return []byte{byte(n)}
	}
	return []byte{byte(longLenFlag8 | (n >> shift8)), byte(n & byteMask)}
}

// u16Len is a string length in the UTF-16 pool's one- or two-unit form.
func u16Len(n int) []byte {
	if n < shortLenLimit16 {
		return binary.LittleEndian.AppendUint16(nil, uint16(n))
	}
	out := binary.LittleEndian.AppendUint16(nil, uint16(longLenFlag16|(n>>shift16)))
	return binary.LittleEndian.AppendUint16(out, uint16(n&halfWordMask))
}

// stringPool is a string pool chunk; UTF-8 when utf8, otherwise UTF-16LE.
func stringPool(strs []string, utf8 bool) []byte {
	var blobs [][]byte
	for _, text := range strs {
		if utf8 {
			raw := []byte(text)
			blob := append(append(u8Len(len(text)), u8Len(len(raw))...), raw...)
			blobs = append(blobs, append(blob, 0))
		} else {
			blob := append(u16Len(len(text)), utf16LE(text)...)
			blobs = append(blobs, append(blob, 0, 0))
		}
	}
	var offsets []byte
	var data []byte
	at := 0
	for _, blob := range blobs {
		offsets = binary.LittleEndian.AppendUint32(offsets, uint32(at))
		data = append(data, blob...)
		at += len(blob)
	}
	for len(data)%wordBytes != 0 {
		data = append(data, 0)
	}
	stringsStart := stringPoolHeader + wordBytes*len(strs)
	var flags uint32
	if utf8 {
		flags = stringPoolUTF8Flag
	}
	head := binary.LittleEndian.AppendUint32(nil, uint32(len(strs)))
	head = binary.LittleEndian.AppendUint32(head, 0)
	head = binary.LittleEndian.AppendUint32(head, flags)
	head = binary.LittleEndian.AppendUint32(head, uint32(stringsStart))
	head = binary.LittleEndian.AppendUint32(head, 0)
	return chunk(chunkStringPool, stringPoolHeader, append(offsets, data...), head)
}

// utf16LE is s as UTF-16 little-endian bytes.
func utf16LE(s string) []byte {
	var out []byte
	for _, unit := range utf16.Encode([]rune(s)) {
		out = binary.LittleEndian.AppendUint16(out, unit)
	}
	return out
}

// resourceMap is the resource map chunk: one id per leading string.
func resourceMap(ids []uint32) []byte {
	var body []byte
	for _, id := range ids {
		body = binary.LittleEndian.AppendUint32(body, id)
	}
	return chunk(chunkResourceMap, chunkHeaderBytes, body, nil)
}

// attr is one attribute record of an element.
func attr(ns, name, raw uint32, dataType uint8, data uint32) []byte {
	out := binary.LittleEndian.AppendUint32(nil, ns)
	out = binary.LittleEndian.AppendUint32(out, name)
	out = binary.LittleEndian.AppendUint32(out, raw)
	out = binary.LittleEndian.AppendUint16(out, emptyAttrSize)
	out = append(out, 0, dataType)
	return binary.LittleEndian.AppendUint32(out, data)
}

// lineExtra is the header tail every node chunk carries: the line number and the comment index.
func lineExtra() []byte {
	out := binary.LittleEndian.AppendUint32(nil, firstLineNumber)
	return binary.LittleEndian.AppendUint32(out, NoIndex)
}

// startElement is an element's start chunk with its attributes.
func startElement(ns, name uint32, attrs [][]byte) []byte {
	var body []byte
	body = binary.LittleEndian.AppendUint32(body, ns)
	body = binary.LittleEndian.AppendUint32(body, name)
	for _, v := range []uint16{attrStartOffset, attrRecordBytes, uint16(len(attrs)), 0, 0, 0} {
		body = binary.LittleEndian.AppendUint16(body, v)
	}
	for _, a := range attrs {
		body = append(body, a...)
	}
	return chunk(chunkStartElement, elementHeaderBytes, body, lineExtra())
}

// endElement is an element's end chunk.
func endElement(ns, name uint32) []byte {
	body := binary.LittleEndian.AppendUint32(nil, ns)
	body = binary.LittleEndian.AppendUint32(body, name)
	return chunk(chunkEndElement, elementHeaderBytes, body, lineExtra())
}

// startNS is a namespace declaration's start chunk.
func startNS(prefix, uri uint32) []byte {
	body := binary.LittleEndian.AppendUint32(nil, prefix)
	body = binary.LittleEndian.AppendUint32(body, uri)
	return chunk(chunkStartNS, elementHeaderBytes, body, lineExtra())
}

// endNS is a namespace declaration's end chunk.
func endNS(prefix, uri uint32) []byte {
	body := binary.LittleEndian.AppendUint32(nil, prefix)
	body = binary.LittleEndian.AppendUint32(body, uri)
	return chunk(chunkEndNS, elementHeaderBytes, body, lineExtra())
}
