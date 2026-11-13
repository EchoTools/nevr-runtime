package questruntimepin

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/questruntimepin/fixture"
)

// These tests are not cases of the Python file: they pin the parts of the pin the Python tests reach only through a
// whole runtime (the shape of a run, the span of a site, the verification of the result), each with a hand-built input.

const (
	// The registers of the hand-built runs.
	regA = 8
	regB = 9
	// opcodes of the words the tests write (the same encodings as fixture).
	movzWord = 0xD2800000
	movkWord = 0xF2800000
	movnWord = 0x92800000
	hwStep   = 1 << 21
	immStep  = 1 << 5
	// sparsePad is the number of unrelated words between the runs of one site, over SiteSpan bytes; nearPad is within it.
	sparsePad = SiteSpan/4 + 8
	nearPad   = 2
	stubCode  = 8
	// hexLen is the width of an md5 text.
	hexLen = 32
)

func word(op uint32, rd uint32, imm uint32, hw uint32) uint32 {
	return op | hw*hwStep | imm*immStep | rd
}

// scanOf is the Scan of an ELF holding the words as its code.
func scanOf(t *testing.T, words []uint32) *Scan {
	t.Helper()
	s, err := NewScan(fixture.MakeElf(fixture.WordBytes(words), nil))
	if err != nil {
		t.Fatal(err)
	}
	return s
}

// The chunk 0x4444333322221111 as its four 16-bit parts.
const (
	chunkValue = 0x4444333322221111
	half0      = 0x1111
	half1      = 0x2222
	half2      = 0x3333
	half3      = 0x4444
	wrongHalf  = 0x9999
	otherImm   = 0x5555
	halfMask   = 0xFFFF
	// the shifts of the 16-bit parts of a chunk, and the hw field values that select them
	shift1, shift2, shift3 = 16, 32, 48
	hw1, hw2, hw3          = 1, 2, 3
	// two runs of two registers, interleaved
	interleaved = 2
	// the offsets of the first program header's fields in an ELF (type, flags, filesz)
	phdr0         = 64
	phFlagsAt     = phdr0 + 4
	phFilesizeAt  = phdr0 + 32
	elfClassAt    = 4
	elfDataAt     = 5
	shortHeader   = 80
	elfClass32    = 1
	elfBigEndian  = 2
	flagsNoExec   = 4
	phentsizeAt   = 0x36
	tinyPhentsize = 10
	hugeSize      = 0xFF
	hugeAt        = 3 // the top byte of the size
)

func chunkWords(rd uint32) []uint32 {
	return []uint32{word(movzWord, rd, half0, 0), word(movkWord, rd, half1, hw1), word(movkWord, rd, half2, hw2), word(movkWord, rd, half3, hw3)}
}

func TestScanRunsFindsAWholeRun(t *testing.T) {
	t.Parallel()
	runs := scanOf(t, chunkWords(regA)).Runs(chunkValue)
	if len(runs) != 1 || len(runs[0]) != WordsPerRun {
		t.Fatalf("runs = %v, want one run of %d words", runs, WordsPerRun)
	}
}

func TestScanRunsRefusesABrokenRun(t *testing.T) {
	t.Parallel()
	whole := chunkWords(regA)
	cases := map[string][]uint32{
		"a movk repeated for one half":   {whole[0], whole[1], whole[1], whole[2]},
		"a movk with the wrong half":     {whole[0], whole[1], word(movkWord, regA, wrongHalf, hw2), whole[3]},
		"the register rewritten by movz": {whole[0], whole[1], word(movzWord, regA, otherImm, 0), whole[2], whole[3]},
		"the register rewritten by movn": {whole[0], whole[1], word(movnWord, regA, otherImm, 0), whole[2], whole[3]},
		"the last half missing":          whole[:3],
		"the run too far from its movz":  append(append([]uint32{whole[0]}, make([]uint32, Lookahead)...), whole[1:]...),
	}
	for name, words := range cases {
		t.Run(name, func(t *testing.T) {
			t.Parallel()
			if runs := scanOf(t, words).Runs(chunkValue); len(runs) != 0 {
				t.Errorf("runs = %v, want none", runs)
			}
		})
	}
}

func TestScanRunsIgnoresWordsOfOtherRegisters(t *testing.T) {
	t.Parallel()
	a, b := chunkWords(regA), chunkWords(regB)
	words := []uint32{a[0], b[0], a[1], b[1], a[2], b[2], a[3], b[3]}
	if runs := scanOf(t, words).Runs(chunkValue); len(runs) != interleaved {
		t.Errorf("%d runs, want the interleaved ones", len(runs))
	}
}

// sparseRuntime is a runtime whose sites hold the runs of their four chunks pad words apart.
func sparseRuntime(pad int) []byte {
	var code []byte
	chunks := ChunksOf([]byte(fixture.OLD))
	for site := 0; site < Sites; site++ {
		for k, c := range chunks {
			rd := uint32(regA + k)
			code = append(code, fixture.WordBytes([]uint32{
				word(movzWord, rd, uint32(c)&halfMask, 0), word(movkWord, rd, uint32(c>>shift1)&halfMask, hw1),
				word(movkWord, rd, uint32(c>>shift2)&halfMask, hw2), word(movkWord, rd, uint32(c>>shift3)&halfMask, hw3)})...)
			code = append(code, fixture.WordBytes(make([]uint32, pad))...)
		}
	}
	data := bytes.Repeat([]byte(fixture.OLD+"\x00unrelated\x00"), Sites)
	return fixture.MakeElf(code, data)
}

func TestPinDataTheRunsOfOneSiteMustLieWithinTheSiteSpan(t *testing.T) {
	t.Parallel()
	newText := fixture.MD5Hex([]byte("a patched libr15"))
	if _, _, err := PinData(sparseRuntime(nearPad), newText, 0, StockEntryMD5); err != nil {
		t.Fatalf("runs close together: %v", err)
	}
	_, _, err := PinData(sparseRuntime(sparsePad), newText, 0, StockEntryMD5)
	refused(t, err, fmt.Sprintf("bytes apart, over %#x", SiteSpan))
}

func TestPinDataMoreThanTwelveASCIICopiesAreRefused(t *testing.T) {
	t.Parallel()
	_, _, err := PinData(runtime(fixture.OLD, fixture.WithCopies(Sites+1)), fixture.MD5Hex([]byte("a patched libr15")), 0, StockEntryMD5)
	refused(t, err, "has 13 ASCII copies, not 12")
}

// A list that holds the entry twelve times in a row would have all twelve copies replaced, though the pin means one entry:
// the result's table is not the input's with one entry replaced, which only the re-reading of the result sees.
func TestPinDataATableThatHoldsTheEntryTwelveTimesIsRefusedWhenReRead(t *testing.T) {
	t.Parallel()
	var code []byte
	for range Sites {
		code = append(code, fixture.WordBytes(fixture.SiteWords(fixture.OLD))...)
	}
	rt := fixture.MakeElf(code, bytes.Repeat([]byte(fixture.OLD+"\x00"), Sites))
	_, _, err := PinData(rt, fixture.MD5Hex([]byte("a patched libr15")), 0, StockEntryMD5)
	refused(t, err, "re-reading the result: the table is not the input's with entry 0 replaced")
}

func TestVerifyPinnedSeesWhatAPinMayNotChange(t *testing.T) {
	t.Parallel()
	newText := fixture.MD5Hex([]byte("a patched libr15"))
	before := runtime(fixture.OLD)
	good, _ := mustPin(t, before, newText, 0, StockEntryMD5)
	scan, err := NewScan(before)
	if err != nil {
		t.Fatal(err)
	}
	oldSites, err := sitesOf(scan, []byte(fixture.OLD))
	if err != nil {
		t.Fatal(err)
	}
	copies := ASCIICopies(before, []byte(fixture.OLD))
	verify := func(after []byte) error {
		return verifyPinned(before, after, []byte(newText), []byte(fixture.OLD), 0, copies, oldSites)
	}
	if err := verify(good); err != nil {
		t.Fatalf("a good pin: %v", err)
	}
	tail := func(f func(b []byte) []byte) []byte { return f(bytes.Clone(good)) }
	cases := map[string]struct {
		after []byte
		want  string
	}{
		"a changed size":                      {tail(func(b []byte) []byte { return append(b, 0) }), "not the size of the input"},
		"an untouched input":                  {before, "the table is not the input's with entry 0 replaced"},
		"a stray byte":                        {tail(func(b []byte) []byte { b[len(b)-1] ^= 1; return b }), "neither an ASCII copy nor the imm16 field"},
		"a changed opcode":                    {tail(func(b []byte) []byte { b[oldSites[0][0][0]+3] ^= 1; return b }), "immediate runs"},
		"a copy of the old text in the table": {tail(func(b []byte) []byte { copy(b[copies[0]:], fixture.OLD); return b }), "the table is not the input's"},
		"a copy of the old text left behind":  {tail(func(b []byte) []byte { copy(b[copies[len(copies)-1]:], fixture.OLD); return b }), "the new entry's copies are not where the old ones were"},
		"a copy of the old text added": {tail(func(b []byte) []byte {
			at := bytes.Index(b, []byte("module 5:"))
			copy(b[at:], fixture.OLD+"\x00")
			return b
		}), "is still in the file"},
	}
	for name, c := range cases {
		t.Run(name, func(t *testing.T) {
			t.Parallel()
			refused(t, verify(c.after), c.want)
		})
	}
}

func TestTableIgnoresATextInsideALongerHexString(t *testing.T) {
	t.Parallel()
	for name, data := range map[string][]byte{
		"digit before": []byte("0" + fixture.OLD + "\x00"),
		"digit after":  []byte(fixture.OLD + "0\x00"),
		"no NUL":       []byte(fixture.OLD),
		"not hex":      append(bytes.Repeat([]byte("g"), hexLen), 0),
	} {
		t.Run(name, func(t *testing.T) {
			t.Parallel()
			if got := Table(data); len(got) != 0 {
				t.Errorf("table = %q, want empty", got)
			}
			if CarriesTable(bytes.Repeat(data, Sites)) {
				t.Error("a list of such texts carries a table")
			}
		})
	}
	whole := []byte("\x00" + fixture.OLD + "\x00" + fixture.SECOND + "\x00x")
	if got := Table(whole); len(got) != 2 || string(got[0]) != fixture.OLD || string(got[1]) != fixture.SECOND {
		t.Errorf("table = %q, want OLD then SECOND", got)
	}
}

func TestExecRangesRefusals(t *testing.T) {
	t.Parallel()
	elf := fixture.MakeElf(make([]byte, stubCode), nil)
	cases := map[string]struct {
		data []byte
		want string
	}{
		"too short":                 {elf[:hexLen], "not a 64-bit little-endian ELF"},
		"32-bit":                    {mutate(elf, elfClassAt, elfClass32), "not a 64-bit little-endian ELF"},
		"big-endian":                {mutate(elf, elfDataAt, elfBigEndian), "not a 64-bit little-endian ELF"},
		"program headers out":       {elf[:shortHeader], "the program headers do not fit the file"},
		"no executable":             {mutate(elf, phFlagsAt, flagsNoExec), "no executable segment"},
		"segment past end":          {mutate(elf, phFilesizeAt+hugeAt, hugeSize), "runs past the end of the file"},
		"segment ends past the end": {setFilesize(elf, len(elf)-1), "runs past the end of the file"},
		"program header too small":  {mutate(elf, phentsizeAt, tinyPhentsize), "the program headers do not fit the file"},
	}
	for name, c := range cases {
		t.Run(name, func(t *testing.T) {
			t.Parallel()
			_, err := ExecRanges(c.data)
			refused(t, err, c.want)
		})
	}
}

func mutate(data []byte, at int, value byte) []byte {
	out := bytes.Clone(data)
	out[at] = value
	return out
}

// setFilesize is elf with the first program header's p_filesz set to size.
func setFilesize(elf []byte, size int) []byte {
	out := bytes.Clone(elf)
	binary.LittleEndian.PutUint64(out[phFilesizeAt:], uint64(size))
	return out
}

// Two chunks of a text that are equal are built by the same runs: a text whose first two chunks are equal, with six sites
// that build both on two registers each and twelve that build the others, has twelve runs for every chunk and shares
// instruction words between the first two.
func TestPinDataRunsThatShareAnInstructionWordAreRefused(t *testing.T) {
	t.Parallel()
	const text = "0123456701234567" + "89abcdef" + "fedcba98"
	chunks := ChunksOf([]byte(text))
	run := func(c uint64, rd uint32) []uint32 {
		return []uint32{word(movzWord, rd, uint32(c)&halfMask, 0), word(movkWord, rd, uint32(c>>shift1)&halfMask, hw1),
			word(movkWord, rd, uint32(c>>shift2)&halfMask, hw2), word(movkWord, rd, uint32(c>>shift3)&halfMask, hw3)}
	}
	var words []uint32
	for range Sites {
		words = append(words, run(chunks[2], regA+2)...)
		words = append(words, run(chunks[3], regA+3)...)
	}
	for range Sites / 2 {
		words = append(words, run(chunks[0], regA)...)
		words = append(words, run(chunks[1], regB)...)
	}
	data := bytes.Repeat([]byte(text+"\x00unrelated\x00"), Sites)
	_, _, err := PinData(fixture.MakeElf(fixture.WordBytes(words), data), fixture.MD5Hex([]byte("a patched libr15")), 0, text)
	refused(t, err, "two runs share an instruction word")
}
