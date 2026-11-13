// Package questapk holds the Quest APK's packaging rules: the versionCode numbering rule and the strict reader and writer
// of the compiled AndroidManifest.xml (version.go), the launcher label (label.go) and the reference APK's package rename
// (rename.go). Its commands are tools/cmd/quest-apk-version, tools/cmd/quest-apk-label and
// tools/cmd/quest-apk-rename-package. It is the Go port of tools/quest_apk_version.py, tools/quest_apk_label.py and
// tools/quest_apk_rename_package.py (ADR 0044); the Python modules stay for the Python callers that import them.
package questapk

import (
	"encoding/binary"
	"fmt"
	"unicode/utf16"
	"unicode/utf8"
)

// Android compiled XML (frameworks/base/libs/androidfw/include/androidfw/ResourceTypes.h).
const (
	resStringPool      uint16 = 0x0001
	resXML             uint16 = 0x0003
	resXMLStartNS      uint16 = 0x0100
	resXMLEndNS        uint16 = 0x0101
	resXMLStartElement uint16 = 0x0102
	resXMLEndElement   uint16 = 0x0103
	resXMLCDATA        uint16 = 0x0104
	resXMLResourceMap  uint16 = 0x0180

	typeIntDec        uint8  = 0x10
	typeString        uint8  = 0x03
	noIndex           uint32 = 0xFFFFFFFF
	utf8Flag          uint32 = 0x100
	flagKnown         uint32 = utf8Flag | 1
	chunkHeader              = 8
	wordBytes                = 4
	poolHeader               = 28
	elementHdr               = 16
	rootHeader               = 16
	rootMinSize              = 36
	rootAttrStart            = 0x14
	rootAttrSize             = 0x14
	attrRecordSize           = 20
	resMapHeader             = 8
	shortLenFlag             = 0x80
	shortLenMask             = 0x7F
	byteBits                 = 8
	wideLenFlag              = 0x8000
	wideLenMask              = 0x7FFF
	wideLenShift             = 16
	unitBytes                = 2
	int32Limit               = 1 << 31
	poolLenBytes             = 4
	headerSizeOffset         = 2
	chunkSizeOffset          = 4
	surrogateLowStart        = 0xDC00
	surrogateLowEnd          = 0xDFFF
	poolFieldCount           = 5
	poolFlagsOffset          = 16
)

// ManifestError is a manifest the reader does not accept, or a patch that did not land as intended.
type ManifestError struct{ Msg string }

func (e *ManifestError) Error() string { return e.Msg }

func manifestErr(format string, args ...any) error {
	return &ManifestError{Msg: fmt.Sprintf(format, args...)}
}

// chunkInfo is one chunk's header: its kind, its header size and its whole size, all in bytes.
type chunkInfo struct {
	kind             uint16
	headerSize, size int
}

// readChunk reads the chunk header at pos, which must lie before end, and checks that the chunk is whole.
func readChunk(data []byte, pos, end int) (chunkInfo, error) {
	if end-pos < chunkHeader {
		return chunkInfo{}, manifestErr("truncated chunk header at byte %d", pos)
	}
	c := chunkInfo{
		kind:       binary.LittleEndian.Uint16(data[pos:]),
		headerSize: int(binary.LittleEndian.Uint16(data[pos+headerSizeOffset:])),
		size:       int(binary.LittleEndian.Uint32(data[pos+chunkSizeOffset:])),
	}
	if c.headerSize < chunkHeader || c.size < c.headerSize || c.size%wordBytes != 0 || pos+c.size > end {
		return chunkInfo{}, manifestErr("chunk 0x%04x at byte %d has header size %d, size %d (not a whole, 4-aligned chunk inside %d bytes)",
			c.kind, pos, c.headerSize, c.size, end)
	}
	return c, nil
}

// bytesAt is data[pos:pos+n], or a ManifestError when those bytes are not all there.
func bytesAt(data []byte, pos, n int) ([]byte, error) {
	if pos < 0 || n < 0 || pos+n > len(data) {
		return nil, manifestErr("read of %d bytes at byte %d runs past the document (%d bytes)", n, pos, len(data))
	}
	return data[pos : pos+n], nil
}

// u32At reads a little-endian uint32 at pos.
func u32At(data []byte, pos int) (uint32, error) {
	b, err := bytesAt(data, pos, wordBytes)
	if err != nil {
		return 0, err
	}
	return binary.LittleEndian.Uint32(b), nil
}

// u16At reads a little-endian uint16 at pos.
func u16At(data []byte, pos int) (uint16, error) {
	b, err := bytesAt(data, pos, unitBytes)
	if err != nil {
		return 0, err
	}
	return binary.LittleEndian.Uint16(b), nil
}

// readLen8 reads a UTF-8 pool length: one byte, or two when the first byte's high bit is set.
func readLen8(data []byte, pos int) (int, int, error) {
	first, err := bytesAt(data, pos, 1)
	if err != nil {
		return 0, 0, err
	}
	if first[0]&shortLenFlag != 0 {
		second, err := bytesAt(data, pos+1, 1)
		if err != nil {
			return 0, 0, err
		}
		return int(first[0]&shortLenMask)<<byteBits | int(second[0]), pos + unitBytes, nil
	}
	return int(first[0]), pos + 1, nil
}

// clampSlice is data[from:to] with both ends held inside data, as Python slices are.
func clampSlice(data []byte, from, to int) []byte {
	from = min(max(from, 0), len(data))
	to = min(max(to, from), len(data))
	return data[from:to]
}

// decodeUTF16LE decodes UTF-16 little-endian bytes strictly: an odd length or an unpaired surrogate is an error.
func decodeUTF16LE(b []byte) (string, error) {
	if len(b)%unitBytes != 0 {
		return "", fmt.Errorf("odd number of bytes (%d) in a UTF-16LE string", len(b))
	}
	units := make([]uint16, 0, len(b)/unitBytes)
	for i := 0; i < len(b); i += unitBytes {
		units = append(units, binary.LittleEndian.Uint16(b[i:]))
	}
	for i := 0; i < len(units); i++ {
		u := units[i]
		if utf16.IsSurrogate(rune(u)) {
			if u < surrogateLowStart && i+1 < len(units) && units[i+1] >= surrogateLowStart && units[i+1] <= surrogateLowEnd {
				i++
				continue
			}
			return "", fmt.Errorf("unpaired surrogate 0x%04x in a UTF-16LE string", u)
		}
	}
	return string(utf16.Decode(units)), nil
}

// stringPoolStrings reads the string pool chunk at pos: its strings, UTF-8 or UTF-16LE as the flags say.
func stringPoolStrings(data []byte, pos, headerSize, size int) ([]string, error) {
	if headerSize != poolHeader {
		return nil, manifestErr("string pool header size %d, expected 28", headerSize)
	}
	fields := make([]uint32, poolFieldCount)
	for i := range fields {
		v, err := u32At(data, pos+chunkHeader+poolLenBytes*i)
		if err != nil {
			return nil, err
		}
		fields[i] = v
	}
	count, styles, flags, stringsStart := int(fields[0]), int(fields[1]), fields[2], int(fields[3])
	if flags&^flagKnown != 0 {
		return nil, manifestErr("string pool flags 0x%x carry bits this reader does not know", flags)
	}
	if headerSize+poolLenBytes*(count+styles) > size || stringsStart >= size {
		return nil, manifestErr("string pool offsets run past the chunk")
	}
	utf := flags&utf8Flag != 0
	out := make([]string, 0, count)
	for i := 0; i < count; i++ {
		offset, err := u32At(data, pos+headerSize+poolLenBytes*i)
		if err != nil {
			return nil, err
		}
		at := pos + stringsStart + int(offset)
		if at >= pos+size {
			return nil, manifestErr("string %d starts past the pool", i)
		}
		s, err := decodeString(data, at, utf)
		if err != nil {
			return nil, manifestErr("string %d of the pool does not decode: %s", i, err)
		}
		out = append(out, s)
	}
	return out, nil
}

// decodeString reads one pool string at at.
func decodeString(data []byte, at int, utf bool) (string, error) {
	if utf {
		_, next, err := readLen8(data, at)
		if err != nil {
			return "", err
		}
		length, next, err := readLen8(data, next)
		if err != nil {
			return "", err
		}
		raw := clampSlice(data, next, next+length)
		if !utf8.Valid(raw) {
			return "", fmt.Errorf("invalid UTF-8")
		}
		return string(raw), nil
	}
	length16, err := u16At(data, at)
	if err != nil {
		return "", err
	}
	length32 := int(length16)
	next := at + unitBytes
	if length16&wideLenFlag != 0 {
		low, err := u16At(data, next)
		if err != nil {
			return "", err
		}
		length32 = int(length16&wideLenMask)<<wideLenShift | int(low)
		next += unitBytes
	}
	return decodeUTF16LE(clampSlice(data, next, next+unitBytes*length32))
}
