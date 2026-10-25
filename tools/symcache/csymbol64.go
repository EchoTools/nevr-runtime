package main

const (
	csymbol64Polynomial = 0x95AC9329AC4BC9B5
	csymbol64Mask       = ^uint64(0)
)

var csymbol64Table = func() [256]uint64 {
	var table [256]uint64
	for i := 0; i < 256; i++ {
		table[i] = carrylessProduct(csymbol64Polynomial, uint64(2*i))
	}
	return table
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

// csymbol64 is Echo VR's 64-bit name hash: a CRC-64 variant over the
// case-folded bytes, seeded with all ones. Mirrors Dualgame/echo-symbols
// hashes/csymbol64.py and echo-symbols' vectors.tsv.
func csymbol64(name string) uint64 {
	hash := csymbol64Mask
	for i := 0; i < len(name); i++ {
		c := name[i]
		if c >= 'A' && c <= 'Z' {
			c += 'a' - 'A'
		}
		hash = (hash << 8) ^ csymbol64Table[hash>>56] ^ uint64(c)
	}
	return hash
}
