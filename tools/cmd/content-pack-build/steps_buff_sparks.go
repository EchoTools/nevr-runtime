package main

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"math"
)

// OVERCHARGE's sparks texture (PLAN-v5 inventory row 14, piece G-BUFF-FX-3), from he_is_the_cat's
// tools/rm_preview/buff_assets.py at 82ad7ba, read as text and never run. The beam's sparks colour texture is copied
// under CSymbol64("fx_overcharge_sparks_tex") and the copy's pixels go purple by brightness. The base is his extract,
// which is the stock record (S-BASE, the table of records: 4a4c32c49300b8a0 and beac1969cb7b8861 of 3f39c6f7d0747367
// have no gun-patch copy), so no gun-patch value is imported. The beam that names the copy is G-BUFF-FX-2's.
//
// Two records are written, each the copy of the stock record of the same type:
//   - the descriptor, type 4a4c32c49300b8a0, copied as it is (buff_assets.py:46 TEX_META, :263).
//   - the pixel payload, type beac1969cb7b8861, recoloured (buff_assets.py:262, recolour_bc1 at :175-197): every BC1
//     block from byte 148 on, all its mips, has its two RGB565 endpoints multiplied by the purple matrix and the
//     indices kept, with the endpoint order fixed when the new colours compare the other way (see recolourBC1).
//
// The purple matrix is purple_matrix (:170-172): gain * outer(SPARKS_PURPLE, luma), SPARKS_PURPLE = (0.55, 0.12, 1.0)
// (:48), SPARKS_GAIN = 1.6 (:49), luma = (0.299, 0.587, 0.114). Each endpoint is decoded to 0..255 per channel
// (dec: (c >> 11) * 255 / 31, ((c >> 5) & 63) * 255 / 63, (c & 31) * 255 / 31), the matrix applied, clipped to 0..255
// and encoded back by Python's round (half to even: math.RoundToEven).
//
// Checked first, and the step refuses (naming the record, the field, the expected and the actual) and writes nothing
// when they differ:
//   - the descriptor is 256 bytes (tools/cmd/levelpack/set.go:423, the 256-byte descriptor).
//   - the payload is a DDS file with the 124-byte header and the DX10 extension, so its header is 148 bytes:
//     magic "DDS ", dwSize 124 at +4, fourCC "DX10" at +84, dxgiFormat 71 (DXGI_FORMAT_BC1_UNORM) at +128. The 148 is
//     his range(148, ...) (:181), and docs/engine/asset-system.md:59 says the payload is a DDS. The DDS field
//     offsets are the DDS file format's, not his text's.
//   - after the header the payload is one or more whole 8-byte BC1 blocks.
//
// Declared fields (Step.Writes, what the stage82 oracle compares): the two copies, whole, in Sets[0].
const (
	sparksTex      uint64 = 0x3f39c6f7d0747367 // the beam's sparks colour texture, buff_assets.py:45 SPARKS_TEX
	sparksDescType uint64 = 0x4a4c32c49300b8a0 // CGTextureResourceWin10: the 256-byte descriptor (levelpack set.go:423)
	sparksGPUType  uint64 = 0xbeac1969cb7b8861 // CGTextureResourceWin10GPU: the pixel payload (levelpack set.go:424)
	sparksDescSize        = 256                // the descriptor's size (levelpack set.go:423)

	ddsHeaderSize  = 148        // the DDS header with the DX10 extension: 4 + 124 + 20 (buff_assets.py:181 starts at 148)
	ddsMagic       = "DDS "     // the file's first four bytes
	ddsSizeOff     = 4          // DDS_HEADER dwSize
	ddsSizeField   = 124        // DDS_HEADER dwSize, the header without the DX10 extension
	ddsFourCCOff   = 84         // DDS_PIXELFORMAT dwFourCC
	ddsDX10FourCC  = 0x30315844 // "DX10" as a little-endian u32
	ddsDXGIOff     = 128        // DDS_HEADER_DXT10 dxgiFormat
	dxgiBC1UNORM   = 71         // DXGI_FORMAT_BC1_UNORM
	bc1BlockSize   = 8          // one BC1 block: two u16 endpoints and a u32 of sixteen 2-bit indices (buff_assets.py:181)
	bc1Color1Off   = 2          // the second endpoint within a block
	bc1IndexOff    = 4          // the indices within a block
	bc1Pixels      = 16         // indices per block
	bc1IndexBits   = 2          // bits per index
	bc1IndexMask   = 3          // the mask of one index
	bc1Indices     = 4          // the index values a block can hold
	rgbChannels    = 3          // red, green, blue
	channelMax     = 255        // an 8-bit channel's largest value (buff_assets.py dec and enc)
	r565Max        = 31         // a 5-bit channel's largest value
	g565Max        = 63         // a 6-bit channel's largest value
	r565Shift      = 11         // the red bits of an RGB565 word
	g565Shift      = 5          // the green bits of an RGB565 word
	u16Max         = 0xFFFF     // the largest u16, the clamp of an endpoint that a +1 would overflow (buff_assets.py:197)
	sparksStep     = "OVERCHARGE's sparks texture is a purple copy of the beam's"
	sparksCopyName = "fx_overcharge_sparks_tex"
	sparksPurpleR  = 0.55 // SPARKS_PURPLE (buff_assets.py:48)
	sparksPurpleG  = 0.12
	sparksPurpleB  = 1.0
	sparksGain     = 1.6   // SPARKS_GAIN (buff_assets.py:49)
	lumaR          = 0.299 // purple_matrix's luma weights (buff_assets.py:172)
	lumaG          = 0.587
	lumaB          = 0.114
)

// sparksCopy is CSymbol64("fx_overcharge_sparks_tex") (buff_assets.py:244 SPARKS_COPY), the copy's record name. Its value
// is checked against the engine's hash in steps_buff_sparks_test.go.
var sparksCopy = csymbol64(sparksCopyName)

var (
	sparksPurple = [rgbChannels]float64{sparksPurpleR, sparksPurpleG, sparksPurpleB}
	lumaWeights  = [rgbChannels]float64{lumaR, lumaG, lumaB}
	// the two index permutations of recolourBC1: a four-colour block swaps 0 and 1 and 2 and 3 (buff_assets.py:192 `^ 1`),
	// a three-colour block swaps 0 and 1 and keeps 2 and 3 (its {0: 1, 1: 0}.get, :195).
	fourColourRemap  = [bc1Indices]uint32{1, 0, 3, 2}
	threeColourRemap = [bc1Indices]uint32{1, 0, 2, 3}
)

func init() {
	steps = append(steps, Step{Name: sparksStep, Apply: sparksTexture,
		Writes: []Field{
			{Name: "OVERCHARGE sparks texture descriptor, whole (a copy of the beam's)", Set: Sets[0], Type: sparksDescType,
				Records: []uint64{sparksCopy}, Read: wholeRecord},
			{Name: "OVERCHARGE sparks texture pixel payload, whole (the beam's, purple)", Set: Sets[0], Type: sparksGPUType,
				Records: []uint64{sparksCopy}, Read: wholeRecord},
		}})
}

func wholeRecord(b []byte) ([]byte, error) { return b, nil }

// sparksTexture reads the stock descriptor and payload, checks them, and only then adds the two copies. A refusal
// writes nothing.
func sparksTexture(p *Pack) error {
	var descType, gpuType, name uint64 = sparksDescType, sparksGPUType, sparksTex
	desc, err := p.Read(Sets[0], int64(descType), int64(name))
	if err != nil {
		return err
	}
	if err := checkSparksDescriptor(desc); err != nil {
		return err
	}
	payload, err := p.Read(Sets[0], int64(gpuType), int64(name))
	if err != nil {
		return err
	}
	if err := checkSparksPayload(payload); err != nil {
		return err
	}
	if err := p.Put(Sets[0], int64(descType), int64(sparksCopy), desc); err != nil {
		return err
	}
	return p.Put(Sets[0], int64(gpuType), int64(sparksCopy), recolourBC1(payload))
}

// checkSparksDescriptor refuses a descriptor that is not the stock one's size.
func checkSparksDescriptor(b []byte) error {
	if len(b) != sparksDescSize {
		return fmt.Errorf("texture %016x descriptor (%016x): %d bytes, want %d (the stock 256-byte descriptor): not the stock texture",
			sparksTex, sparksDescType, len(b), sparksDescSize)
	}
	return nil
}

// checkSparksPayload refuses a payload that is not a BC1 DDS with the DX10 extension and whole blocks after its header.
func checkSparksPayload(b []byte) error {
	const what = "texture payload"
	if len(b) <= ddsHeaderSize || (len(b)-ddsHeaderSize)%bc1BlockSize != 0 {
		return fmt.Errorf("texture %016x %s (%016x): %d bytes, want a %d-byte DDS header and one or more whole %d-byte BC1 blocks after it",
			sparksTex, what, sparksGPUType, len(b), ddsHeaderSize, bc1BlockSize)
	}
	if string(b[:len(ddsMagic)]) != ddsMagic {
		return fmt.Errorf("texture %016x %s (%016x): magic %q, want %q (a DDS file)", sparksTex, what, sparksGPUType, b[:len(ddsMagic)], ddsMagic)
	}
	if got := binary.LittleEndian.Uint32(b[ddsSizeOff:]); got != ddsSizeField {
		return fmt.Errorf("texture %016x %s (%016x): DDS header size %d at +%d, want %d", sparksTex, what, sparksGPUType, got, ddsSizeOff, ddsSizeField)
	}
	if got := binary.LittleEndian.Uint32(b[ddsFourCCOff:]); got != ddsDX10FourCC {
		return fmt.Errorf("texture %016x %s (%016x): fourCC %08x at +%d, want %08x (DX10: the 148-byte header)", sparksTex, what, sparksGPUType, got, ddsFourCCOff, ddsDX10FourCC)
	}
	if got := binary.LittleEndian.Uint32(b[ddsDXGIOff:]); got != dxgiBC1UNORM {
		return fmt.Errorf("texture %016x %s (%016x): dxgiFormat %d at +%d, want %d (BC1_UNORM)", sparksTex, what, sparksGPUType, got, ddsDXGIOff, dxgiBC1UNORM)
	}
	return nil
}

// purpleMatrix is buff_assets.py's purple_matrix: gain * outer(purple, luma), each element gain * (purple[i] * luma[j]).
func purpleMatrix() [rgbChannels][rgbChannels]float64 {
	var m [rgbChannels][rgbChannels]float64
	for i := range m {
		for j := range m[i] {
			m[i][j] = sparksGain * (sparksPurple[i] * lumaWeights[j])
		}
	}
	return m
}

// mulVec is numpy's m @ v, summed in index order. Each product is converted before it is added, so no fused
// multiply-add changes a result's last bit.
func mulVec(m [rgbChannels][rgbChannels]float64, v [rgbChannels]float64) [rgbChannels]float64 {
	var out [rgbChannels]float64
	for i := range out {
		var s float64
		for j := 0; j < rgbChannels; j++ {
			s += float64(m[i][j] * v[j])
		}
		out[i] = s
	}
	return out
}

// dec565 is buff_assets.py's dec: an RGB565 word as three channels in 0..255.
func dec565(c uint16) [rgbChannels]float64 {
	return [rgbChannels]float64{
		float64(c>>r565Shift) * channelMax / r565Max,
		float64((c>>g565Shift)&g565Max) * channelMax / g565Max,
		float64(c&r565Max) * channelMax / r565Max,
	}
}

// enc565 is buff_assets.py's enc: each channel clipped to 0..255, rounded half to even (Python's round) to its bits.
func enc565(v [rgbChannels]float64) uint16 {
	var c [rgbChannels]float64
	for i := range v {
		c[i] = math.Max(0, math.Min(channelMax, v[i]))
	}
	r := uint16(math.RoundToEven(c[0]*r565Max/channelMax)) << r565Shift
	g := uint16(math.RoundToEven(c[1]*g565Max/channelMax)) << g565Shift
	b := uint16(math.RoundToEven(c[2] * r565Max / channelMax))
	return r | g | b
}

// remapIndices maps each of a block's sixteen 2-bit indices through perm.
func remapIndices(idx uint32, perm [bc1Indices]uint32) uint32 {
	var out uint32
	for i := 0; i < bc1Pixels; i++ {
		v := (idx >> (bc1IndexBits * i)) & bc1IndexMask
		out |= perm[v] << (bc1IndexBits * i)
	}
	return out
}

// recolourBC1 is buff_assets.py's recolour_bc1: every BC1 block from the header's end on, all mips, copied with its two
// endpoints recoloured by the purple matrix. The block's mode depends on c0 > c1, so when the new endpoints compare the
// other way the endpoints swap and the indices are remapped; when a four-colour block's new endpoints are equal, c0 gets
// one more step so it stays the larger.
func recolourBC1(payload []byte) []byte {
	out := bytes.Clone(payload)
	m := purpleMatrix()
	for o := ddsHeaderSize; o+bc1BlockSize <= len(out); o += bc1BlockSize {
		c0 := binary.LittleEndian.Uint16(out[o:])
		c1 := binary.LittleEndian.Uint16(out[o+bc1Color1Off:])
		idx := binary.LittleEndian.Uint32(out[o+bc1IndexOff:])
		n0 := enc565(mulVec(m, dec565(c0)))
		n1 := enc565(mulVec(m, dec565(c1)))
		if (c0 > c1) != (n0 > n1) || (c0 > c1 && n0 == n1) {
			n0, n1 = n1, n0
			if c0 > c1 {
				idx = remapIndices(idx, fourColourRemap)
			} else {
				idx = remapIndices(idx, threeColourRemap)
			}
			if c0 > c1 && n0 == n1 {
				if n0 < u16Max {
					n0++
				}
			}
		}
		binary.LittleEndian.PutUint16(out[o:], n0)
		binary.LittleEndian.PutUint16(out[o+bc1Color1Off:], n1)
		binary.LittleEndian.PutUint32(out[o+bc1IndexOff:], idx)
	}
	return out
}
