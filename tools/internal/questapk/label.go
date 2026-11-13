package questapk

import (
	"encoding/binary"
	"fmt"
	"strings"
)

// The launcher label is one string of the manifest's string pool, referenced by android:label on <application>. It is
// overwritten in place: the new text must be no longer than the old one (tools/quest_apk_label.py's docstring).
const (
	resIDLabel      uint32 = 0x01010001
	typeStringValue uint8  = 0x03
	labelPoolAt            = 16
	labelShortMax          = 0x80
	labelLongFlag          = 0x8000
	labelAppElement        = "application"
	labelAttrNSPos         = 0
)

// LabelError is a manifest the label editor does not accept, or an edit that did not land as intended.
type LabelError struct{ Msg string }

func (e *LabelError) Error() string { return e.Msg }

func labelErr(format string, args ...any) error {
	return &LabelError{Msg: fmt.Sprintf(format, args...)}
}

// labelPool is where the string pool is: its chunk position, its header size, its strings' offset and its encoding.
type labelPool struct {
	pos, headerSize, stringsStart int
	utf8                          bool
}

// walkedManifest is what the label editor reads from one pass over a manifest.
type walkedManifest struct {
	strs  []string
	pool  labelPool
	label int
	refs  map[uint32]int
	data  []byte
}

// walkLabel reads the string pool and the application's label, counting every string-typed reference to a pool string.
func walkLabel(data []byte) (walkedManifest, error) {
	if len(data) < chunkHeader {
		return walkedManifest{}, labelErr("not a compiled XML document: shorter than a chunk header")
	}
	kind, headerSize, size, err := chunkFields(data, 0)
	if err != nil {
		return walkedManifest{}, labelErr("%s", err)
	}
	if kind != resXML || headerSize != chunkHeader || size != len(data) {
		return walkedManifest{}, labelErr("not a compiled XML document: type 0x%04x, header %d, size %d of %d bytes", kind, headerSize, size, len(data))
	}
	var (
		w        = walkedManifest{refs: map[uint32]int{}, data: data, label: -1}
		havePool bool
		ids      []uint32
		haveIDs  bool
	)
	for pos := chunkHeader; pos < len(data); {
		c, err := readChunk(data, pos, len(data))
		if err != nil {
			return walkedManifest{}, labelErr("%s", err)
		}
		switch c.kind {
		case resStringPool:
			if havePool {
				return walkedManifest{}, labelErr("a second string pool")
			}
			if w.strs, err = stringPoolStrings(data, pos, c.headerSize, c.size); err != nil {
				return walkedManifest{}, labelErr("%s", err)
			}
			count, styles, flags, stringsStart, err := poolFields(data, pos)
			if err != nil {
				return walkedManifest{}, labelErr("%s", err)
			}
			_ = count
			if styles != 0 {
				return walkedManifest{}, labelErr("the string pool carries styles: this tool does not edit such a pool")
			}
			w.pool = labelPool{pos: pos, headerSize: c.headerSize, stringsStart: stringsStart, utf8: flags&utf8Flag != 0}
			havePool = true
		case resXMLResourceMap:
			if ids, err = readUint32s(data, pos+resMapHeader, (c.size-resMapHeader)/wordBytes); err != nil {
				return walkedManifest{}, labelErr("%s", err)
			}
			haveIDs = true
		case resXMLStartElement:
			if !havePool || !haveIDs {
				return walkedManifest{}, labelErr("an element before the string pool or the resource map")
			}
			if err := walkElement(&w, ids, pos, c); err != nil {
				return walkedManifest{}, err
			}
		}
		pos += c.size
	}
	if !havePool || w.label < 0 {
		return walkedManifest{}, labelErr("no string pool, or no android:label on <application>")
	}
	return w, nil
}

// poolFields reads the five words after a string pool's header: the count, the styles, the flags, the strings' offset and
// the styles' offset.
func poolFields(data []byte, pos int) (int, int, uint32, int, error) {
	var f [poolFieldCount]uint32
	for i := range f {
		v, err := u32At(data, pos+chunkHeader+poolLenBytes*i)
		if err != nil {
			return 0, 0, 0, 0, err
		}
		f[i] = v
	}
	return int(f[0]), int(f[1]), f[2], int(f[3]), nil
}

// walkElement reads one start element: its attributes count the string references, and <application>'s label is noted.
func walkElement(w *walkedManifest, ids []uint32, pos int, c chunkInfo) error {
	base := pos + c.headerSize
	ns, err := u32At(w.data, base)
	if err != nil {
		return labelErr("%s", err)
	}
	_ = ns
	name, err := u32At(w.data, base+attrNamespaceEnd)
	if err != nil {
		return labelErr("%s", err)
	}
	attrStart, err := u16At(w.data, base+attrStartPos)
	if err != nil {
		return labelErr("%s", err)
	}
	attrSize, err := u16At(w.data, base+attrSizePos)
	if err != nil {
		return labelErr("%s", err)
	}
	attrCount, err := u16At(w.data, base+attrCountPos)
	if err != nil {
		return labelErr("%s", err)
	}
	if int(attrStart) != rootAttrStart || int(attrSize) != rootAttrSize || c.headerSize+int(attrStart)+int(attrSize)*int(attrCount) > c.size {
		return labelErr("an element's attributes do not fit its chunk")
	}
	element := ""
	if int(name) < len(w.strs) {
		element = w.strs[name]
	}
	for i := 0; i < int(attrCount); i++ {
		at := base + int(attrStart) + int(attrSize)*i
		rec, err := readAttr(w.data, at)
		if err != nil {
			return labelErr("%s", err)
		}
		if rec.typ == typeStringValue {
			seen := map[uint32]bool{}
			for _, index := range []uint32{rec.raw, rec.data} {
				if index != noIndex && !seen[index] {
					seen[index] = true
					w.refs[index]++
				}
			}
		}
		isLabel := int(rec.name) < len(ids) && ids[rec.name] == resIDLabel
		if element == labelAppElement && isLabel {
			if w.label >= 0 {
				return labelErr("two android:label attributes on <application>")
			}
			if rec.typ != typeStringValue || rec.raw != rec.data || int(rec.data) >= len(w.strs) {
				return labelErr("the label is not a plain string attribute (type 0x%02x, raw %#x, data %#x)", rec.typ, rec.raw, rec.data)
			}
			w.label = int(rec.data)
		}
	}
	return nil
}

// LabelOf is the application's launcher label in a manifest.
func LabelOf(data []byte) (string, error) {
	w, err := walkLabel(data)
	if err != nil {
		return "", err
	}
	return w.strs[w.label], nil
}

// labelSlot is the label string's bytes, from its length prefix through its terminator, and the characters they hold.
func labelSlot(data []byte, w walkedManifest) (int, int, int, error) {
	pool := w.pool
	offset, err := u32At(data, pool.pos+pool.headerSize+wordBytes*w.label)
	if err != nil {
		return 0, 0, 0, labelErr("%s", err)
	}
	at := pool.pos + pool.stringsStart + int(offset)
	if pool.utf8 {
		chars, after, err := readLen8(data, at)
		if err != nil {
			return 0, 0, 0, labelErr("%s", err)
		}
		length, after, err := readLen8(data, after)
		if err != nil {
			return 0, 0, 0, labelErr("%s", err)
		}
		if after-at != unitBytes || chars != length || chars >= labelShortMax {
			return 0, 0, 0, labelErr("the label is not a short ASCII string of a UTF-8 pool")
		}
		return at, after + length + 1, length, nil
	}
	first, err := u16At(data, at)
	if err != nil {
		return 0, 0, 0, labelErr("%s", err)
	}
	if first&labelLongFlag != 0 {
		return 0, 0, 0, labelErr("the label's length uses the long form")
	}
	return at, at + unitBytes + unitBytes*int(first) + unitBytes, int(first), nil
}

// SetLabel is data with the application label replaced by text, in place; a LabelError unless every rule holds.
func SetLabel(data []byte, text string) ([]byte, error) {
	if text == "" || !isASCII(text) || strings.ContainsRune(text, 0) {
		return nil, labelErr("the new label %q must be non-empty ASCII", text)
	}
	w, err := walkLabel(data)
	if err != nil {
		return nil, err
	}
	if w.refs[uint32(w.label)] != 1 {
		return nil, labelErr("the label string is referenced by %d string attributes; editing it would rename the others too", w.refs[uint32(w.label)])
	}
	start, end, capacity, err := labelSlot(data, w)
	if err != nil {
		return nil, err
	}
	if len(text) > capacity {
		return nil, labelErr("the new label has %d characters, the old one %d: this tool only edits in place", len(text), capacity)
	}
	var body []byte
	if w.pool.utf8 {
		body = append([]byte{byte(len(text)), byte(len(text))}, text...)
		body = append(body, 0)
	} else {
		body = binary.LittleEndian.AppendUint16(nil, uint16(len(text)))
		body = append(body, utf16LE(text)...)
		body = append(body, 0, 0)
	}
	out := append([]byte(nil), data...)
	copy(out[start:end], append(body, make([]byte, end-start-len(body))...))
	if len(out) != len(data) {
		return nil, labelErr("the edited manifest does not read back with the new label")
	}
	if got, err := LabelOf(out); err != nil || got != text {
		return nil, labelErr("the edited manifest does not read back with the new label")
	}
	first, last := -1, -1
	for i := range data {
		if data[i] != out[i] {
			if first < 0 {
				first = i
			}
			last = i
		}
	}
	if first >= 0 && (first < start || last >= end) {
		return nil, labelErr("the edit changed a byte outside the label string's slot")
	}
	return out, nil
}

// isASCII reports whether s holds only 7-bit characters.
func isASCII(s string) bool {
	for i := 0; i < len(s); i++ {
		if s[i] >= labelShortMax {
			return false
		}
	}
	return true
}
