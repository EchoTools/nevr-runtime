package repackfixture

import (
	"encoding/binary"
	"unicode/utf8"
)

// The binary AndroidManifest.xml ("AXML") the store-like fixture APK carries, built by hand in the layout aapt2 writes
// (frameworks/base/libs/androidfw/include/androidfw/ResourceTypes.h). It is the default shape of the Python fixture
// (tools/tests/axml_fixtures.py build_manifest): a manifest with versionCode, versionName and package, and a uses-sdk
// child with minSdkVersion 29. The recipe's version stamp reads and rewrites that versionCode.
const (
	androidNS          = "http://schemas.android.com/apk/res/android"
	resVersionCode     = 0x0101021B
	resVersionName     = 0x0101021C
	resVersionCodeMaj  = 0x01010576
	resMinSDK          = 0x0101020C
	typeString         = 0x03
	typeIntDec         = 0x10
	noIndex            = 0xFFFFFFFF
	chunkXMLDoc        = 0x0003
	chunkStringPool    = 0x0001
	chunkResourceMap   = 0x0180
	chunkStartNS       = 0x0100
	chunkEndNS         = 0x0101
	chunkStartElement  = 0x0102
	chunkEndElement    = 0x0103
	chunkHeaderBytes   = 8
	chunkElementHeader = 16
	stringPoolHeader   = 28
	stringPoolUTF8     = 0x100
	attrBytes          = 20
	attrValueSize      = 8
	elementAttrStart   = 0x14
	elementBodyBytes   = 20 // the start element's fixed body: ns, name, attribute start and size, count, and three indices
	minSDKVersion      = 29
	storePackage       = "com.readyatdawn.r15"
	manifestRoot       = "manifest"
)

// StoreManifest is the default manifest the fixture APK carries, with the given versionCode and its versionName.
func StoreManifest(versionCode uint32) []byte {
	versionName := itoa(versionCode)
	names := []string{"versionCode", "versionName", "minSdkVersion", "versionCodeMajor"}
	ids := []uint32{resVersionCode, resVersionName, resMinSDK, resVersionCodeMaj}
	strings := append(append([]string(nil), names...), "android", androidNS, manifestRoot, "package", storePackage,
		"uses-sdk", versionName)
	idx := map[string]uint32{}
	for i, s := range strings {
		idx[s] = uint32(i)
	}
	attrs := [][]byte{
		attr(idx[androidNS], idx["versionCode"], noIndex, typeIntDec, versionCode),
		attr(idx[androidNS], idx["versionName"], idx[versionName], typeString, idx[versionName]),
		attr(noIndex, idx["package"], idx[storePackage], typeString, idx[storePackage]),
	}
	childAttrs := [][]byte{attr(idx[androidNS], idx["minSdkVersion"], noIndex, typeIntDec, minSDKVersion)}
	body := concat(
		stringPool(strings),
		resourceMap(ids),
		startNS(idx["android"], idx[androidNS]),
		startElement(noIndex, idx[manifestRoot], attrs),
		startElement(noIndex, idx["uses-sdk"], childAttrs),
		endElement(noIndex, idx["uses-sdk"]),
		endElement(noIndex, idx[manifestRoot]),
		endNS(idx["android"], idx[androidNS]),
	)
	out := make([]byte, chunkHeaderBytes)
	binary.LittleEndian.PutUint16(out[0:], chunkXMLDoc)
	binary.LittleEndian.PutUint16(out[2:], chunkHeaderBytes)
	binary.LittleEndian.PutUint32(out[4:], uint32(chunkHeaderBytes+len(body)))
	return append(out, body...)
}

// itoa is the decimal text of v.
func itoa(v uint32) string {
	if v == 0 {
		return "0"
	}
	var b [10]byte
	i := len(b)
	for v > 0 {
		i--
		b[i] = byte('0' + v%10)
		v /= 10
	}
	return string(b[i:])
}

func concat(parts ...[]byte) []byte {
	var out []byte
	for _, p := range parts {
		out = append(out, p...)
	}
	return out
}

// chunk is one chunk: its kind, header size, body, and the header's extra bytes after the 8-byte header.
func chunk(kind uint16, headerSize int, body, headerExtra []byte) []byte {
	size := headerSize + len(body)
	out := make([]byte, 0, size)
	var h [chunkHeaderBytes]byte
	binary.LittleEndian.PutUint16(h[0:], kind)
	binary.LittleEndian.PutUint16(h[2:], uint16(headerSize))
	binary.LittleEndian.PutUint32(h[4:], uint32(size))
	out = append(out, h[:]...)
	out = append(out, headerExtra...)
	return append(out, body...)
}

func u32s(values ...uint32) []byte {
	out := make([]byte, 4*len(values))
	for i, v := range values {
		binary.LittleEndian.PutUint32(out[4*i:], v)
	}
	return out
}

// stringPool is a UTF-8 string pool of strings.
func stringPool(strings []string) []byte {
	var blobs [][]byte
	for _, text := range strings {
		raw := []byte(text)
		blob := concat([]byte{byte(utf8.RuneCountInString(text))}, []byte{byte(len(raw))}, raw, []byte{0})
		blobs = append(blobs, blob)
	}
	offsets := make([]uint32, len(blobs))
	var data []byte
	at := uint32(0)
	for i, blob := range blobs {
		offsets[i] = at
		data = append(data, blob...)
		at += uint32(len(blob))
	}
	for len(data)%4 != 0 {
		data = append(data, 0)
	}
	stringsStart := uint32(stringPoolHeader + 4*len(strings))
	head := u32s(uint32(len(strings)), 0, stringPoolUTF8, stringsStart, 0)
	return chunk(chunkStringPool, stringPoolHeader, concat(u32s(offsets...), data), head)
}

func resourceMap(ids []uint32) []byte {
	return chunk(chunkResourceMap, chunkHeaderBytes, u32s(ids...), nil)
}

// attr is one attribute: namespace, name, raw value, the typed value's size, a reserved byte, its data type, its data.
func attr(ns, name, raw uint32, dataType uint8, data uint32) []byte {
	out := make([]byte, attrBytes)
	binary.LittleEndian.PutUint32(out[0:], ns)
	binary.LittleEndian.PutUint32(out[4:], name)
	binary.LittleEndian.PutUint32(out[8:], raw)
	binary.LittleEndian.PutUint16(out[12:], attrValueSize)
	out[15] = dataType
	binary.LittleEndian.PutUint32(out[16:], data)
	return out
}

// startElement opens an element with its attributes; its header holds the line number and the comment, both unset.
func startElement(ns, name uint32, attrs [][]byte) []byte {
	h := make([]byte, elementBodyBytes)
	binary.LittleEndian.PutUint32(h[0:], ns)
	binary.LittleEndian.PutUint32(h[4:], name)
	binary.LittleEndian.PutUint16(h[8:], elementAttrStart)
	binary.LittleEndian.PutUint16(h[10:], elementAttrStart)
	binary.LittleEndian.PutUint16(h[12:], uint16(len(attrs)))
	return chunk(chunkStartElement, chunkElementHeader, concat(h, concat(attrs...)), u32s(1, noIndex))
}

func endElement(ns, name uint32) []byte {
	return chunk(chunkEndElement, chunkElementHeader, u32s(ns, name), u32s(1, noIndex))
}

func startNS(prefix, uri uint32) []byte {
	return chunk(chunkStartNS, chunkElementHeader, u32s(prefix, uri), u32s(1, noIndex))
}

func endNS(prefix, uri uint32) []byte {
	return chunk(chunkEndNS, chunkElementHeader, u32s(prefix, uri), u32s(1, noIndex))
}
