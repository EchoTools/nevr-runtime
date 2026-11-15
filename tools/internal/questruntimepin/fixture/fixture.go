// Package fixture builds the synthetic AArch64 ELF runtimes the tests of the runtime pin use (#1737): an executable
// segment with twelve sites per list entry, each of sixteen movz/movk words that build the four 8-byte chunks of the
// entry's text (interleaved over four registers, with unrelated instructions between), and a data segment with twelve
// lists of three entries. It is a package, not a test file, so the tests of the library, of the command and of the
// libs tool all build the same runtime.
package fixture

import (
	"crypto/md5" //nolint:gosec // the libr15 md5 is an identifier the runtime's list carries, not a security check
	"encoding/binary"
	"encoding/hex"
	"fmt"
)

// The entries of the reference runtime's list.
const (
	// OLD is the stock libr15's md5: index 0 of the reference runtime's list.
	OLD    = "4911ef7f1248580ee2978590d581dbfd"
	SECOND = "6e63ac1285105a018a82e39cd1e3edf0"
	THIRD  = "0734f86fd23afc03aea76ffadeb7b924"
)

// Instruction words.
const (
	NOP = 0xD503201F
	// EOR_X8 is eor x8, x8, x9: an instruction on register 8 that is neither movz nor movk.
	EOR_X8 = 0xCA090108

	opMovz   = 0xD2800000
	opMovk   = 0xF2800000
	hwShift  = 21
	immShift = 5
	imm16    = 0xFFFF
	// firstReg is the register of the first chunk; the four chunks use x8 to x11.
	firstReg = 8
)

// Sizes of the fixture.
const (
	// Gated is the number of modules of the reference runtime: sites and copies of each entry.
	Gated      = 12
	textLen    = 32
	chunkLen   = 8
	chunkCount = textLen / chunkLen
	halfwords  = 4
	nopsAfter  = 12 // unrelated words after each site
	wordLen    = 4
	halfBits   = 16
)

// ELF layout.
const (
	// EMAArch64 and EMX8664 are the e_machine values of an AArch64 and an x86-64 ELF.
	EMAArch64 = 0xB7
	EMX8664   = 0x3E

	pageSize   = 0x1000
	pageMask   = pageSize - 1
	codeAt     = pageSize
	ehdrLen    = 64
	phdrLen    = 56
	phdrCount  = 2
	eiLen      = 16
	etDyn      = 3
	evCurrent  = 1
	ptLoad     = 1
	pfRX       = 5
	pfR        = 4
	elfClass64 = 2
	elfDataLE  = 1
	elfVersion = 1
)

// MD5Hex is the md5 of data in lower-case hex.
func MD5Hex(data []byte) string {
	sum := md5.Sum(data) //nolint:gosec // see the import
	return hex.EncodeToString(sum[:])
}

func movz(rd, imm uint32) uint32 { return opMovz | imm<<immShift | rd }

func movk(rd, imm, hw uint32) uint32 { return opMovk | hw<<hwShift | imm<<immShift | rd }

// SiteWords is the twenty words one module's compare builds text with: movz of the low half then the three movk, per
// chunk, interleaved over x8..x11 the way the compiler orders them, with an unrelated instruction on x8 after each four
// (sixteen immediates and four unrelated words).
func SiteWords(text string) []uint32 {
	var chunks [chunkCount]uint64
	for i := range chunks {
		chunks[i] = binary.LittleEndian.Uint64([]byte(text)[i*chunkLen:])
	}
	var words []uint32
	for hw := uint32(0); hw < halfwords; hw++ {
		for k, chunk := range chunks {
			half := uint32(chunk>>(halfBits*hw)) & imm16
			if hw == 0 {
				words = append(words, movz(firstReg+uint32(k), half))
			} else {
				words = append(words, movk(firstReg+uint32(k), half, hw))
			}
		}
		words = append(words, EOR_X8)
	}
	return words
}

// WordBytes is words as little-endian bytes: the form a site has in the file.
func WordBytes(words []uint32) []byte {
	var out []byte
	for _, w := range words {
		out = binary.LittleEndian.AppendUint32(out, w)
	}
	return out
}

// MakeElf is an AArch64 ELF64 with an R+X segment holding code and an R segment holding data. machine is the e_machine
// field; it is EMAArch64 when omitted.
func MakeElf(code, data []byte, machine ...uint16) []byte {
	m := uint16(EMAArch64)
	if len(machine) > 0 {
		m = machine[0]
	}
	dataAt := codeAt + ((len(code) + pageMask) &^ pageMask)
	le := binary.LittleEndian
	ident := append([]byte("\x7fELF"), elfClass64, elfDataLE, elfVersion)
	ident = append(ident, make([]byte, eiLen-len(ident))...)
	header := append([]byte(nil), ident...)
	header = le.AppendUint16(header, etDyn)
	header = le.AppendUint16(header, m)
	header = le.AppendUint32(header, evCurrent)
	header = le.AppendUint64(header, 0)       // e_entry
	header = le.AppendUint64(header, ehdrLen) // e_phoff
	header = le.AppendUint64(header, 0)       // e_shoff
	header = le.AppendUint32(header, 0)       // e_flags
	header = le.AppendUint16(header, ehdrLen)
	header = le.AppendUint16(header, phdrLen)
	header = le.AppendUint16(header, phdrCount)
	header = le.AppendUint16(header, ehdrLen) // e_shentsize
	header = le.AppendUint16(header, 0)       // e_shnum
	header = le.AppendUint16(header, 0)       // e_shstrndx
	phdr := func(flags uint32, at, size int) []byte {
		var p []byte
		p = le.AppendUint32(p, ptLoad)
		p = le.AppendUint32(p, flags)
		for range 3 { // p_offset, p_vaddr, p_paddr
			p = le.AppendUint64(p, uint64(at))
		}
		p = le.AppendUint64(p, uint64(size)) // p_filesz
		p = le.AppendUint64(p, uint64(size)) // p_memsz
		return le.AppendUint64(p, pageSize)  // p_align
	}
	image := append(header, phdr(pfRX, codeAt, len(code))...)
	image = append(image, phdr(pfR, dataAt, len(data))...)
	image = append(image, make([]byte, codeAt-len(image))...)
	image = append(image, code...)
	image = append(image, make([]byte, dataAt-len(image))...)
	return append(image, data...)
}

type config struct {
	sites, copies int
	others        []string
	decoy         bool
	extra         []byte
}

// Option changes one thing of the runtime MakeRuntime builds.
type Option func(*config)

// WithSites sets the number of immediate sites of the first entry (Gated in the reference runtime).
func WithSites(n int) Option { return func(c *config) { c.sites = n } }

// WithCopies sets the number of ASCII copies of the first entry (Gated in the reference runtime).
func WithCopies(n int) Option { return func(c *config) { c.copies = n } }

// WithOthers sets the entries after the first (SECOND and THIRD by default); each has Gated sites and copies.
func WithOthers(others ...string) Option { return func(c *config) { c.others = others } }

// WithDecoy adds a run of the first entry's site words in the non-executable segment.
func WithDecoy() Option { return func(c *config) { c.decoy = true } }

// WithExtra appends bytes to the data segment.
func WithExtra(extra []byte) Option { return func(c *config) { c.extra = extra } }

// MakeRuntime is a runtime fixture whose list is old then the other entries (SECOND and THIRD unless WithOthers says
// otherwise). It has WithSites sites and WithCopies ASCII copies of old (Gated each in the reference runtime); the others
// always have Gated of each.
func MakeRuntime(old string, options ...Option) []byte {
	c := config{sites: Gated, copies: Gated, others: []string{SECOND, THIRD}}
	for _, o := range options {
		o(&c)
	}
	entries := append([]string{old}, c.others...)
	wanted := func(i, first int) int {
		if i == 0 {
			return first
		}
		return Gated
	}
	var code []byte
	for m := 0; m < max(c.sites, Gated); m++ {
		for i, e := range entries {
			if m < wanted(i, c.sites) {
				code = append(code, WordBytes(SiteWords(e))...)
				code = append(code, WordBytes(nops())...)
			}
		}
	}
	var data []byte
	for m := 0; m < max(c.copies, Gated); m++ {
		for i, e := range entries {
			if m < wanted(i, c.copies) {
				data = append(append(data, e...), 0)
			}
		}
		data = append(data, fmt.Sprintf("module %d: %%.8s is none of the known builds\x00", m)...)
	}
	if c.decoy {
		data = append(data, make([]byte, (-len(data))&(wordLen-1))...) // a word-aligned run, as a compiler would leave it
		data = append(data, WordBytes(SiteWords(old))...)
	}
	data = append(data, c.extra...)
	return MakeElf(code, data)
}

func nops() []uint32 {
	words := make([]uint32, nopsAfter)
	for i := range words {
		words[i] = NOP
	}
	return words
}
