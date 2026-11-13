package questruntimepin

import (
	"bytes"
	"crypto/md5" //nolint:gosec // the libr15 md5 is an identifier the runtime's list carries, not a security check
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"os"
	"sort"
	"strings"
)

// The shape of the checksum list in the binary.
const (
	// StockEntryMD5 is the md5 of the stock libr15 that the reference runtime's list opens with (index 0): the entry the
	// pin replaces.
	StockEntryMD5 = "4911ef7f1248580ee2978590d581dbfd"
	// Sites is the number of gated modules: one site and one ASCII copy of an entry per module.
	Sites = 12
	// Chunks is the number of 8-byte compare chunks in a 32-hex text.
	Chunks = 4
	// WordsPerRun is the length of one run: movz + three movk.
	WordsPerRun = 4
	// Runs is the number of immediate runs of one entry.
	Runs = Sites * Chunks
	// Words is the number of immediate words of one entry.
	Words = Runs * WordsPerRun
	// Lookahead is how many words after a movz are searched for the three movk of one register.
	Lookahead = 64
	// SiteSpan is the largest distance in bytes between the four runs of one site.
	SiteSpan = 0x400

	textLen    = 32 // the length of an md5 in hex
	textStride = textLen + 1
	chunkLen   = 8 // bytes of text in one compare chunk
	nul        = 0
)

// ELF, as the pin reads it.
const (
	machineAArch64 = 0xB7
	ptLoad         = 1
	pfExec         = 1
	elfClass64     = 2
	elfDataLE      = 1

	elfHeaderLen      = 64
	elfIdentClass     = 4
	elfIdentData      = 5
	elfOffMachine     = 0x12
	elfOffPhoff       = 0x20
	elfOffPhentsize   = 0x36
	elfPhentsizeMin   = 56
	elfPhdrFlagsAt    = 4
	elfPhdrOffsetAt   = 8
	elfPhdrFilesizeAt = 32
)

// The AArch64 words the scan reads.
const (
	opMask       = 0xFF800000 // the opcode, the width and the shift field above the register and imm16
	opMovz       = 0xD2800000
	opMovn       = 0x92800000
	opMovk       = 0xF2800000
	regMask      = 31
	hwShift      = 21
	hwMask       = 3
	immShift     = 5
	imm16Mask    = 0xFFFF
	imm16Bits    = 16
	imm16Field   = imm16Mask << immShift // the bits a pin may change in a word
	wordLen      = 4
	wordAlign    = wordLen - 1
	hwCount      = 4 // halfwords in a 64-bit chunk
	diffBlockLen = 4096
	hexDigits    = "0123456789abcdef"
	// copyBlockLen is the read size for hashing a file.
	copyBlockLen = 1 << 20
)

// PinError is a refusal: the file is not what the pin is made for, or the arguments are wrong. WrongEntryError is the
// other kind of refusal, and is not a PinError.
type PinError struct{ Msg string }

func (e PinError) Error() string { return e.Msg }

// As lets errors.As find a PinError whether the caller's target is a PinError or a *PinError.
func (e PinError) As(target any) bool {
	switch t := target.(type) {
	case *PinError:
		*t = e
		return true
	case **PinError:
		*t = &e
		return true
	}
	return false
}

func pinErrorf(format string, args ...any) error { return PinError{Msg: fmt.Sprintf(format, args...)} }

// WrongEntryError is the refusal when the entry at the replace index is neither the expected md5 nor the md5 already
// pinned.
type WrongEntryError struct {
	Index    int
	Expected string
	Found    string
}

func (e WrongEntryError) Error() string {
	return fmt.Sprintf("the entry at index %d of the list is %s, expected %s: refusing to pin another entry "+
		"(a damaged list would otherwise retarget the index)", e.Index, e.Found, e.Expected)
}

// As lets errors.As find a WrongEntryError whether the caller's target is a WrongEntryError or a *WrongEntryError.
func (e WrongEntryError) As(target any) bool {
	switch t := target.(type) {
	case *WrongEntryError:
		*t = e
		return true
	case **WrongEntryError:
		*t = &e
		return true
	}
	return false
}

// Report is what a pin did: the table, the entry it replaced, and the counts it asserted.
type Report struct {
	Table          []string
	ReplaceIndex   int
	OldMD5         string
	NewMD5         string
	ASCIICopies    int
	Sites          int
	ImmediateRuns  int
	ImmediateWords int
	ChangedBytes   int
}

// Sha256Hex is the sha-256 of data in lower-case hex.
func Sha256Hex(data []byte) string {
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:])
}

// MD5OfFile is the md5 of the file at path in lower-case hex.
func MD5OfFile(path string) (string, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := md5.New() //nolint:gosec // see the import
	buf := make([]byte, copyBlockLen)
	for {
		n, err := f.Read(buf)
		h.Write(buf[:n])
		if errors.Is(err, io.EOF) {
			return hex.EncodeToString(h.Sum(nil)), nil
		}
		if err != nil {
			return "", err
		}
	}
}

// Range is an executable PT_LOAD segment: its file offset and size.
type Range struct{ Offset, Size int }

// ExecRanges is the (file offset, size) of every executable PT_LOAD segment of a 64-bit little-endian AArch64 ELF.
func ExecRanges(data []byte) ([]Range, error) {
	if len(data) < elfHeaderLen || !bytes.HasPrefix(data, []byte("\x7fELF")) || data[elfIdentClass] != elfClass64 || data[elfIdentData] != elfDataLE {
		return nil, pinErrorf("not a 64-bit little-endian ELF file")
	}
	le := binary.LittleEndian
	machine := le.Uint16(data[elfOffMachine:])
	if machine != machineAArch64 {
		return nil, pinErrorf("ELF machine 0x%x is not AArch64 (0x%x)", machine, machineAArch64)
	}
	phoff := le.Uint64(data[elfOffPhoff:])
	phentsize := uint64(le.Uint16(data[elfOffPhentsize:]))
	phnum := uint64(le.Uint16(data[elfOffPhentsize+2:]))
	if phentsize < elfPhentsizeMin || phoff > uint64(len(data)) || phoff+phnum*phentsize > uint64(len(data)) {
		return nil, pinErrorf("the program headers do not fit the file")
	}
	var found []Range
	for k := uint64(0); k < phnum; k++ {
		at := phoff + k*phentsize
		kind := le.Uint32(data[at:])
		flags := le.Uint32(data[at+elfPhdrFlagsAt:])
		offset := le.Uint64(data[at+elfPhdrOffsetAt:])
		filesz := le.Uint64(data[at+elfPhdrFilesizeAt:])
		if kind == ptLoad && flags&pfExec != 0 {
			if offset > uint64(len(data)) || filesz > uint64(len(data)) || offset+filesz > uint64(len(data)) {
				return nil, pinErrorf("an executable segment runs past the end of the file")
			}
			found = append(found, Range{Offset: int(offset), Size: int(filesz)})
		}
	}
	if len(found) == 0 {
		return nil, pinErrorf("the ELF file has no executable segment")
	}
	return found, nil
}

type wordAt struct{ segment, index int }

type segment struct {
	start int // file offset of the first word
	words []uint32
}

// Scan is one pass over the file: every 64-bit movz (low half word) of the executable segments, indexed by its immediate.
type Scan struct {
	segments []segment
	movzLow  map[uint32][]wordAt
}

// NewScan reads the executable segments of data.
func NewScan(data []byte) (*Scan, error) {
	ranges, err := ExecRanges(data)
	if err != nil {
		return nil, err
	}
	s := &Scan{movzLow: map[uint32][]wordAt{}}
	for _, r := range ranges {
		start := (r.Offset + wordAlign) &^ wordAlign
		count := (r.Offset + r.Size - start) / wordLen
		if count < 0 {
			count = 0
		}
		words := make([]uint32, count)
		for i := range words {
			words[i] = binary.LittleEndian.Uint32(data[start+i*wordLen:])
		}
		s.segments = append(s.segments, segment{start: start, words: words})
	}
	for si, seg := range s.segments {
		for i, w := range seg.words {
			if w&opMask == opMovz && (w>>hwShift)&hwMask == 0 {
				imm := (w >> immShift) & imm16Mask
				s.movzLow[imm] = append(s.movzLow[imm], wordAt{si, i})
			}
		}
	}
	return s, nil
}

// Runs is the file offsets of the four words of every movz + three movk run that builds the 64-bit chunk.
func (s *Scan) Runs(chunk uint64) [][]int {
	var halves [hwCount]uint32
	for k := range halves {
		halves[k] = uint32(chunk>>(imm16Bits*k)) & imm16Mask
	}
	var found [][]int
	for _, pos := range s.movzLow[halves[0]] {
		seg := s.segments[pos.segment]
		words := seg.words
		rd := words[pos.index] & regMask
		var at [hwCount]int
		var have [hwCount]bool
		at[0], have[0] = pos.index, true
		n := 1
		for j := pos.index + 1; j < min(pos.index+1+Lookahead, len(words)); j++ {
			w := words[j]
			if w&regMask != rd {
				continue
			}
			top := w & opMask
			if top == opMovz || top == opMovn { // the register is rewritten
				break
			}
			if top == opMovk {
				hw := (w >> hwShift) & hwMask
				if have[hw] {
					break
				}
				if (w>>immShift)&imm16Mask != halves[hw] {
					break
				}
				at[hw], have[hw] = j, true
				n++
				if n == hwCount {
					break
				}
			}
		}
		if n == hwCount {
			run := make([]int, hwCount)
			for k := range run {
				run[k] = seg.start + wordLen*at[k]
			}
			found = append(found, run)
		}
	}
	return found
}

func isHex(b byte) bool { return strings.IndexByte(hexDigits, b) >= 0 }

// hexTextAt reports whether a whole NUL-terminated 32-hex text starts at at, not inside a longer run of hex digits.
func hexTextAt(data []byte, at int) bool {
	if at+textLen >= len(data) || data[at+textLen] != nul {
		return false
	}
	if at > 0 && isHex(data[at-1]) {
		return false
	}
	for _, b := range data[at : at+textLen] {
		if !isHex(b) {
			return false
		}
	}
	return true
}

// ASCIICopies is the offsets of every copy of text that is a whole NUL-terminated hex string.
func ASCIICopies(data, text []byte) []int {
	var found []int
	for at := 0; ; at++ {
		i := bytes.Index(data[at:], text)
		if i < 0 {
			return found
		}
		at += i
		after := at + len(text)
		if after < len(data) && data[after] == nul && !(at > 0 && isHex(data[at-1])) {
			found = append(found, at)
		}
	}
}

// ChunksOf is the four 8-byte little-endian compare chunks of a 32-hex text.
func ChunksOf(text []byte) []uint64 {
	chunks := make([]uint64, Chunks)
	for i := range chunks {
		chunks[i] = binary.LittleEndian.Uint64(text[i*chunkLen:])
	}
	return chunks
}

// hexTexts is the offset of every whole NUL-terminated 32-hex text, in order.
func hexTexts(data []byte) []int {
	var found []int
	for i := 0; i < len(data); {
		if hexTextAt(data, i) {
			found = append(found, i)
			i += textLen
		} else {
			i++
		}
	}
	return found
}

// CarriesTable is true when data holds a checksum table, whole or not: some 32-hex text has exactly Sites copies. A
// library that holds none is not the reference runtime (the store's own libvrapi.so is not).
func CarriesTable(data []byte) bool {
	counts := map[string]int{}
	for _, at := range hexTexts(data) {
		counts[string(data[at:at+textLen])]++
	}
	for _, n := range counts {
		if n == Sites {
			return true
		}
	}
	return false
}

// Table is the table: the 32-hex texts of the first list in the file, each NUL-terminated and followed at once by the
// next (stride 33 bytes), starting at the lowest-offset hex text. Index 0 of the reference runtime is the stock libr15's md5.
func Table(data []byte) [][]byte {
	first := -1
	for i := 0; i < len(data); i++ {
		if hexTextAt(data, i) {
			first = i
			break
		}
	}
	if first < 0 {
		return nil
	}
	var texts [][]byte
	for at := first; hexTextAt(data, at); at += textStride {
		texts = append(texts, data[at:at+textLen])
	}
	return texts
}

// pyIntList writes a list of ints the way Python prints it.
func pyIntList(list []int) string {
	parts := make([]string, len(list))
	for i, n := range list {
		parts[i] = fmt.Sprint(n)
	}
	return "[" + strings.Join(parts, ", ") + "]"
}

// sitesOf is the runs of text grouped by site: a list of sites, each a list of four runs (one per chunk, in chunk
// order). A PinError unless the runs are exactly Sites sites of one run per chunk within SiteSpan bytes.
func sitesOf(scan *Scan, text []byte) ([][][]int, error) {
	chunks := ChunksOf(text)
	perChunk := make([][][]int, len(chunks))
	counts := make([]int, len(chunks))
	total := 0
	allSites := true
	for i, c := range chunks {
		perChunk[i] = scan.Runs(c)
		counts[i] = len(perChunk[i])
		total += counts[i]
		if counts[i] != Sites {
			allSites = false
		}
	}
	if !allSites {
		return nil, pinErrorf("%s has %d immediate runs (%s per chunk), not %d (%d per chunk)", text, total, pyIntList(counts), Runs, Sites)
	}
	claimed := map[int]bool{}
	for _, runs := range perChunk {
		for _, run := range runs {
			for _, w := range run {
				if claimed[w] {
					return nil, pinErrorf("%s: two runs share an instruction word", text)
				}
			}
			for _, w := range run {
				claimed[w] = true
			}
		}
	}
	sorted := make([][][]int, len(perChunk))
	for i, runs := range perChunk {
		sorted[i] = append([][]int(nil), runs...)
		sort.SliceStable(sorted[i], func(a, b int) bool { return sorted[i][a][0] < sorted[i][b][0] })
	}
	var sites [][][]int
	for n := range sorted[0] {
		site := make([][]int, len(sorted))
		lo, hi := -1, -1
		for k := range sorted {
			site[k] = sorted[k][n]
			for _, w := range site[k] {
				if lo < 0 || w < lo {
					lo = w
				}
				if w > hi {
					hi = w
				}
			}
		}
		if hi-lo > SiteSpan {
			return nil, pinErrorf("%s: the runs of site %d are %#x bytes apart, over %#x", text, n, hi-lo, SiteSpan)
		}
		sites = append(sites, site)
	}
	return sites, nil
}

func isMD5Text(value string) bool {
	if len(value) != textLen {
		return false
	}
	for i := 0; i < len(value); i++ {
		if !isHex(value[i]) {
			return false
		}
	}
	return true
}

// pyRepr writes a string the way Python's repr does for the plain cases (quotes chosen as Python chooses them).
func pyRepr(s string) string {
	quote := byte('\'')
	if strings.Contains(s, "'") && !strings.Contains(s, `"`) {
		quote = '"'
	}
	var b strings.Builder
	b.WriteByte(quote)
	for _, r := range s {
		switch {
		case r == rune(quote) || r == '\\':
			b.WriteByte('\\')
			b.WriteRune(r)
		case r == '\n':
			b.WriteString(`\n`)
		case r == '\r':
			b.WriteString(`\r`)
		case r == '\t':
			b.WriteString(`\t`)
		default:
			b.WriteRune(r)
		}
	}
	b.WriteByte(quote)
	return b.String()
}

func checkMD5(value string) ([]byte, error) {
	if !isMD5Text(value) {
		return nil, pinErrorf("%s is not 32 lowercase hex characters", pyRepr(value))
	}
	return []byte(value), nil
}

// PinData is data with the table entry at replaceIndex replaced by newMD5 in both forms, and the report of it. The entry
// at replaceIndex must be expectOldMD5 (or newMD5 itself, a file already pinned), found in the ASCII table and with its
// twelve copies and twelve sites; else a WrongEntryError or a PinError. A caller that has no other wish passes 0 and
// StockEntryMD5.
func PinData(data []byte, newMD5 string, replaceIndex int, expectOldMD5 string) ([]byte, Report, error) {
	var report Report
	newText, err := checkMD5(newMD5)
	if err != nil {
		return nil, report, err
	}
	expected, err := checkMD5(expectOldMD5)
	if err != nil {
		return nil, report, err
	}
	if !CarriesTable(data) {
		return nil, report, pinErrorf("the file holds no checksum table (no 32-hex text with exactly %d copies): it is not the reference runtime", Sites)
	}
	entries := Table(data)
	if replaceIndex < 0 || replaceIndex >= len(entries) {
		return nil, report, pinErrorf("--replace-index %d: the table has %d entries", replaceIndex, len(entries))
	}
	old := entries[replaceIndex]
	if !bytes.Equal(old, expected) && !bytes.Equal(old, newText) {
		return nil, report, WrongEntryError{Index: replaceIndex, Expected: expectOldMD5, Found: string(old)}
	}
	oldCopies := ASCIICopies(data, old)
	if len(oldCopies) != Sites {
		return nil, report, pinErrorf("%s has %d ASCII copies, not %d", old, len(oldCopies), Sites)
	}
	scan, err := NewScan(data)
	if err != nil {
		return nil, report, err
	}
	oldSites, err := sitesOf(scan, old)
	if err != nil {
		return nil, report, err
	}
	table := make([]string, len(entries))
	for i, e := range entries {
		table[i] = string(e)
	}
	runs := 0
	for _, site := range oldSites {
		runs += len(site)
	}
	report = Report{Table: table, ReplaceIndex: replaceIndex, OldMD5: string(old), NewMD5: newMD5, ASCIICopies: len(oldCopies),
		Sites: len(oldSites), ImmediateRuns: runs, ImmediateWords: Words}
	if bytes.Equal(newText, old) {
		return data, report, nil
	}
	newCopies := ASCIICopies(data, newText)
	newRuns := 0
	for _, c := range ChunksOf(newText) {
		newRuns += len(scan.Runs(c))
	}
	if len(newCopies) != 0 || newRuns != 0 {
		return nil, report, pinErrorf("%s is already in the file (%d ASCII copies, %d immediate runs) but is not the entry at index %d: refusing to make a second copy",
			newMD5, len(newCopies), newRuns, replaceIndex)
	}
	out := bytes.Clone(data)
	for _, offset := range oldCopies {
		copy(out[offset:offset+textLen], newText)
	}
	newChunks := ChunksOf(newText)
	for _, site := range oldSites {
		for k, run := range site {
			for hw, at := range run {
				w := binary.LittleEndian.Uint32(out[at:])
				w = (w &^ imm16Field) | (uint32(newChunks[k]>>(imm16Bits*hw))&imm16Mask)<<immShift
				binary.LittleEndian.PutUint32(out[at:], w)
			}
		}
	}
	if err := verifyPinned(data, out, newText, old, replaceIndex, oldCopies, oldSites); err != nil {
		return nil, report, err
	}
	report.ChangedBytes = len(diffIndexes(data, out))
	return out, report, nil
}

// diffIndexes is the index of every differing byte of two equal-length buffers; equal blocks are skipped fast.
func diffIndexes(a, b []byte) []int {
	var found []int
	for base := 0; base < len(a); base += diffBlockLen {
		end := min(base+diffBlockLen, len(a))
		if bytes.Equal(a[base:end], b[base:end]) {
			continue
		}
		for i := base; i < end; i++ {
			if a[i] != b[i] {
				found = append(found, i)
			}
		}
	}
	return found
}

func sameEntries(a, b [][]byte) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if !bytes.Equal(a[i], b[i]) {
			return false
		}
	}
	return true
}

func equalInts(a, b []int) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

// verifyPinned re-decodes after and compares it with before; a PinError on any difference from what a pin may change.
func verifyPinned(before, after, newText, old []byte, index int, oldCopies []int, oldSites [][][]int) error {
	if len(after) != len(before) {
		return pinErrorf("the pinned file is not the size of the input")
	}
	entries, was := Table(after), Table(before)
	rest := func(list [][]byte) [][]byte { return append(append([][]byte(nil), list[:index]...), list[index+1:]...) }
	if len(entries) != len(was) || !bytes.Equal(entries[index], newText) || !sameEntries(rest(entries), rest(was)) {
		return pinErrorf("re-reading the result: the table is not the input's with entry %d replaced by %s", index, newText)
	}
	if !equalInts(ASCIICopies(after, newText), oldCopies) {
		return pinErrorf("re-reading the result: the new entry's copies are not where the old ones were")
	}
	scan, err := NewScan(after)
	if err != nil {
		return err
	}
	if _, err := sitesOf(scan, newText); err != nil {
		return err
	}
	stillThere := len(ASCIICopies(after, old)) != 0
	for _, c := range ChunksOf(old) {
		if len(scan.Runs(c)) != 0 {
			stillThere = true
		}
	}
	if stillThere {
		return pinErrorf("re-reading the result: %s is still in the file", old)
	}
	words := map[int]bool{}
	for _, site := range oldSites {
		for _, run := range site {
			for _, at := range run {
				words[at] = true
			}
		}
	}
diffs:
	for _, i := range diffIndexes(before, after) {
		for _, lo := range oldCopies {
			if lo <= i && i < lo+textLen {
				continue diffs
			}
		}
		word := i &^ wordAlign
		if words[word] {
			wa := binary.LittleEndian.Uint32(before[word:])
			wb := binary.LittleEndian.Uint32(after[word:])
			if (wa^wb)&^imm16Field == 0 {
				continue
			}
		}
		return pinErrorf("the pin changed byte %#x, which is neither an ASCII copy nor the imm16 field of a pinned word", i)
	}
	return nil
}
