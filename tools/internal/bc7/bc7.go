// Package bc7 encodes and decodes BC7 mode 6 blocks: one RGBA endpoint pair
// (7 bits per channel plus one p-bit per endpoint) and sixteen 4-bit indices
// per 4x4 block.
//
// It is written from the BC7 block format (Khronos Data Format Specification,
// section "BPTC"; Direct3D 11 "BC7 Format"), not translated from another
// encoder. The one choice taken from the EchoCombat build's encoder, which was
// read as text, is the mode (mode 6 for every block) and the endpoints (the
// block's per-channel minimum and maximum, each quantised with the p-bit that
// leaves the smaller L1 error). Index selection here is exact: each pixel takes
// the palette entry with the smallest squared error, in integer arithmetic, so
// the output is identical on every machine.
//
// Layout of a block, least significant bit first, 128 bits as two little-endian
// 64-bit words:
//
//	bits   0..6    mode: six zero bits then a one (the value 0x40)
//	bits   7..62   R0 R1 G0 G1 B0 B1 A0 A1, 7 bits each
//	bits  63..64   p-bit of endpoint 0, p-bit of endpoint 1
//	bits  65..67   index of pixel 0 (3 bits: the anchor, its top bit is implied 0)
//	bits  68..127  indices of pixels 1..15, 4 bits each
//
// Pixel k of a block is at x = k mod 4, y = k / 4 (row major). Blocks of an
// image are row major too.
package bc7

import (
	"errors"
	"fmt"
)

const (
	// BlockSize is the encoded size of one 4x4 block in bytes.
	BlockSize = 16
	// BlockDim is the block's width and height in pixels.
	BlockDim = 4
	// BlockPixels is the number of pixels in a block.
	BlockPixels = BlockDim * BlockDim
	// Channels is the number of channels of a pixel: R, G, B, A.
	Channels = 4

	modeBits      = 7
	modeSixValue  = 1 << 6 // bits 0..6 of a mode 6 block: 0b1000000
	endpointBits  = 7
	pBitBits      = 1
	anchorBits    = 3
	indexBits     = 4
	indexLevels   = 1 << indexBits
	anchorLimit   = 1 << anchorBits // the anchor index is below this
	wordBits      = 64
	weightShift   = 6
	weightMax     = 1 << weightShift
	weightRound   = weightMax / 2
	endpointShift = 1 // 7-bit value << 1, then the p-bit, is the 8-bit endpoint
	channelMax    = 255
	qMax          = 127
	pBitChoices   = 2
	bitsPerByte   = 8
	bytesPerWord  = wordBits / bitsPerByte
	wordCount     = BlockSize / bytesPerWord // 64-bit words in one 128-bit block
)

// weights4 is the 4-bit index interpolation table of BPTC (out of 64).
var weights4 = [indexLevels]int{0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64}

// Pixel is one RGBA pixel.
type Pixel [Channels]uint8

// Block is the 16 pixels of a 4x4 block, row major.
type Block [BlockPixels]Pixel

// EncodedBlock is one encoded block.
type EncodedBlock [BlockSize]byte

// ErrNotMode6 is returned by DecodeBlock for a block of any other mode.
var ErrNotMode6 = errors.New("bc7: block is not mode 6")

// palette returns the 16 colours a block's endpoints produce.
func palette(e0, e1 [Channels]int) [indexLevels]Pixel {
	var out [indexLevels]Pixel
	for i, w := range weights4 {
		for c := 0; c < Channels; c++ {
			out[i][c] = uint8(((weightMax-w)*e0[c] + w*e1[c] + weightRound) >> weightShift)
		}
	}
	return out
}

// quantise turns an 8-bit endpoint into its 7-bit values and the p-bit that
// reconstruct it with the smallest summed absolute error (the lower p-bit wins
// a tie).
func quantise(e [Channels]int) (q [Channels]int, p int, rec [Channels]int) {
	bestErr := -1
	for pb := 0; pb < pBitChoices; pb++ {
		var cq, cr [Channels]int
		errSum := 0
		for c := 0; c < Channels; c++ {
			v := (e[c] - pb + 1) >> endpointShift // nearest, ties upward
			if v < 0 {
				v = 0
			}
			if v > qMax {
				v = qMax
			}
			cq[c] = v
			cr[c] = v<<endpointShift | pb
			d := cr[c] - e[c]
			if d < 0 {
				d = -d
			}
			errSum += d
		}
		if bestErr < 0 || errSum < bestErr {
			bestErr, q, p, rec = errSum, cq, pb, cr
		}
	}
	return q, p, rec
}

// EncodeBlock encodes one block as mode 6.
func EncodeBlock(b Block) EncodedBlock {
	var lo, hi [Channels]int
	for c := 0; c < Channels; c++ {
		lo[c], hi[c] = channelMax, 0
	}
	for _, px := range b {
		for c := 0; c < Channels; c++ {
			v := int(px[c])
			if v < lo[c] {
				lo[c] = v
			}
			if v > hi[c] {
				hi[c] = v
			}
		}
	}
	q0, p0, e0 := quantise(lo)
	q1, p1, e1 := quantise(hi)
	pal := palette(e0, e1)

	var idx [BlockPixels]int
	for k, px := range b {
		best, bestErr := 0, -1
		for i, pc := range pal {
			d := 0
			for c := 0; c < Channels; c++ {
				diff := int(px[c]) - int(pc[c])
				d += diff * diff
			}
			if bestErr < 0 || d < bestErr {
				best, bestErr = i, d
			}
		}
		idx[k] = best
	}
	// The anchor index has an implied top bit of 0: when pixel 0's index is
	// 8 or more, swap the endpoints and mirror every index (the table is
	// symmetric: weights4[15-i] == 64-weights4[i]).
	if idx[0] >= anchorLimit {
		q0, q1 = q1, q0
		p0, p1 = p1, p0
		for k := range idx {
			idx[k] = indexLevels - 1 - idx[k]
		}
	}

	var w bitWriter
	w.put(modeSixValue, modeBits)
	for c := 0; c < Channels; c++ {
		w.put(uint64(q0[c]), endpointBits)
		w.put(uint64(q1[c]), endpointBits)
	}
	w.put(uint64(p0), pBitBits)
	w.put(uint64(p1), pBitBits)
	w.put(uint64(idx[0]), anchorBits)
	for k := 1; k < BlockPixels; k++ {
		w.put(uint64(idx[k]), indexBits)
	}
	return w.bytes()
}

// DecodeBlock decodes one mode 6 block.
func DecodeBlock(enc EncodedBlock) (Block, error) {
	r := newBitReader(enc)
	if r.get(modeBits) != modeSixValue {
		return Block{}, ErrNotMode6
	}
	var q0, q1 [Channels]int
	for c := 0; c < Channels; c++ {
		q0[c] = int(r.get(endpointBits))
		q1[c] = int(r.get(endpointBits))
	}
	p0 := int(r.get(pBitBits))
	p1 := int(r.get(pBitBits))
	var e0, e1 [Channels]int
	for c := 0; c < Channels; c++ {
		e0[c] = q0[c]<<endpointShift | p0
		e1[c] = q1[c]<<endpointShift | p1
	}
	pal := palette(e0, e1)
	var out Block
	out[0] = pal[r.get(anchorBits)]
	for k := 1; k < BlockPixels; k++ {
		out[k] = pal[r.get(indexBits)]
	}
	return out, nil
}

func checkSize(n, w, h, perPixel int) error {
	if w <= 0 || h <= 0 || w%BlockDim != 0 || h%BlockDim != 0 {
		return fmt.Errorf("bc7: size %dx%d is not a positive multiple of %d", w, h, BlockDim)
	}
	if n != w*h*perPixel {
		return fmt.Errorf("bc7: %d bytes given, %dx%d needs %d", n, w, h, w*h*perPixel)
	}
	return nil
}

// Encode encodes a w x h RGBA image (row major, 4 bytes a pixel) as mode 6
// blocks, row major. w and h must be positive multiples of 4.
func Encode(rgba []byte, w, h int) ([]byte, error) {
	if err := checkSize(len(rgba), w, h, Channels); err != nil {
		return nil, err
	}
	bw, bh := w/BlockDim, h/BlockDim
	out := make([]byte, 0, bw*bh*BlockSize)
	for by := 0; by < bh; by++ {
		for bx := 0; bx < bw; bx++ {
			var blk Block
			for y := 0; y < BlockDim; y++ {
				for x := 0; x < BlockDim; x++ {
					o := ((by*BlockDim+y)*w + bx*BlockDim + x) * Channels
					copy(blk[y*BlockDim+x][:], rgba[o:o+Channels])
				}
			}
			enc := EncodeBlock(blk)
			out = append(out, enc[:]...)
		}
	}
	return out, nil
}

// Decode decodes blocks produced by Encode back to a w x h RGBA image.
func Decode(blocks []byte, w, h int) ([]byte, error) {
	if w <= 0 || h <= 0 || w%BlockDim != 0 || h%BlockDim != 0 {
		return nil, fmt.Errorf("bc7: size %dx%d is not a positive multiple of %d", w, h, BlockDim)
	}
	bw, bh := w/BlockDim, h/BlockDim
	if len(blocks) != bw*bh*BlockSize {
		return nil, fmt.Errorf("bc7: %d bytes given, %dx%d needs %d", len(blocks), w, h, bw*bh*BlockSize)
	}
	out := make([]byte, w*h*Channels)
	for by := 0; by < bh; by++ {
		for bx := 0; bx < bw; bx++ {
			var enc EncodedBlock
			copy(enc[:], blocks[(by*bw+bx)*BlockSize:])
			blk, err := DecodeBlock(enc)
			if err != nil {
				return nil, fmt.Errorf("bc7: block (%d,%d): %w", bx, by, err)
			}
			for y := 0; y < BlockDim; y++ {
				for x := 0; x < BlockDim; x++ {
					o := ((by*BlockDim+y)*w + bx*BlockDim + x) * Channels
					copy(out[o:o+Channels], blk[y*BlockDim+x][:])
				}
			}
		}
	}
	return out, nil
}

// bitWriter packs fields least significant bit first into 128 bits.
type bitWriter struct {
	word [wordCount]uint64
	pos  int
}

func (w *bitWriter) put(v uint64, bits int) {
	for b := 0; b < bits; b++ {
		if v>>uint(b)&1 == 1 {
			w.word[w.pos/wordBits] |= 1 << uint(w.pos%wordBits)
		}
		w.pos++
	}
}

func (w *bitWriter) bytes() EncodedBlock {
	var out EncodedBlock
	for i := 0; i < BlockSize; i++ {
		out[i] = byte(w.word[i/bytesPerWord] >> uint(bitsPerByte*(i%bytesPerWord)))
	}
	return out
}

type bitReader struct {
	enc EncodedBlock
	pos int
}

func newBitReader(e EncodedBlock) *bitReader { return &bitReader{enc: e} }

func (r *bitReader) get(bits int) uint64 {
	var v uint64
	for b := 0; b < bits; b++ {
		bit := r.enc[r.pos/bitsPerByte] >> uint(r.pos%bitsPerByte) & 1
		v |= uint64(bit) << uint(b)
		r.pos++
	}
	return v
}
