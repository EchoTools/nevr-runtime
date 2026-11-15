// Package csymbol is Echo VR's 64-bit name hash, CSymbol64 (echovr.exe CSymbol64::Lookup 0x1400ce120, libr15.so
// CalculateSymbolValue 0x11cc580; sdk/include/nevr/symbol_hash.h is the C++ form): a CRC-64 variant over the case-folded
// bytes, seeded with all ones. Hashing continues from a seed, so Append(Hash(a), b) equals Hash(a + b), which is how the
// engine's CSymbol64::Make (echovr.exe 0x1400ce0c0) and CSymbol64::Append (libr15.so) build a name from two parts
// (docs/engine/asset-system.md, "Names"). Mirrors Dualgame/echo-symbols hashes/csymbol64.py and its vectors.tsv.
package csymbol

const (
	polynomial = 0x95AC9329AC4BC9B5
	seed       = ^uint64(0) // the hash of the empty name
	tableSize  = 256        // one entry per byte value
	byteBits   = 8
	topShift   = 56 // the top byte of the 64-bit state indexes the table
)

var table = func() [tableSize]uint64 {
	var t [tableSize]uint64
	for i := range t {
		t[i] = carrylessProduct(polynomial, uint64(2*i))
	}
	return t
}()

// carrylessProduct multiplies a by b without carries, truncated to 64 bits.
func carrylessProduct(a, b uint64) uint64 {
	var result uint64
	for b != 0 {
		if b&1 != 0 {
			result ^= a
		}
		a <<= 1
		b >>= 1
	}
	return result
}

// Hash is CSymbol64(name).
func Hash(name string) uint64 { return Append(seed, name) }

// Append continues the hash h over s: Append(Hash(a), b) == Hash(a + b).
func Append(h uint64, s string) uint64 {
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c >= 'A' && c <= 'Z' {
			c += 'a' - 'A'
		}
		h = (h << byteBits) ^ table[h>>topShift] ^ uint64(c)
	}
	return h
}
