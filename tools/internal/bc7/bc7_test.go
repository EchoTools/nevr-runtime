package bc7

import (
	"bytes"
	"encoding/hex"
	"errors"
	"math/rand/v2"
	"testing"
)

const (
	// twoColourHex was packed by hand from the format layout (mode 0x40; R0 R1
	// G0 G1 B0 B1 A0 A1 = 0,127 0,127 0,127 127,127; p-bits 0,0; indices 0 for
	// even pixels and 15 for odd ones), not produced by the encoder.
	twoColourHex = "40c01ff007fcff7ff0f0f0f0f0f0f0f0"

	darkChannel   = 0
	lightChannel  = 254
	opaqueEven    = 254 // an even 8-bit value: the 7-bit endpoint plus p-bit 0 is exact
	gradientStep  = 16
	gradientBound = 12 // stated error: half the widest palette step (5/64 of the 254 span) plus quantisation, rounded up
	randomBlocks  = 200
	randomSeedA   = 7
	randomSeedB   = 11
	imageBlocksX  = 3
	imageBlocksY  = 2
	// each image block is one even solid level, a step of 2*gradientStep from the last
	imageLevelStep = 2 * gradientStep
)

// paletteCases are packed by hand with pixel k holding index k; each want row is
// the levels computed from the BPTC weights ((64-w)*e0 + w*e1 + 32) >> 6.
var paletteCases = []struct {
	name string
	hex  string
	want [indexLevels]uint8
}{
	{"endpoints 0 and 254", "40c01ff007fc017f1032547698badcfe",
		[indexLevels]uint8{0, 16, 36, 52, 67, 83, 103, 119, 135, 151, 171, 187, 202, 218, 238, 254}},
	{"endpoints 1 and 254", "40c01ff007fc01ff1032547698badcfe",
		[indexLevels]uint8{1, 17, 37, 52, 68, 84, 104, 120, 135, 151, 171, 187, 203, 218, 238, 254}},
}

// quantisationCases reach choices the solid-block exactness test does not: the
// p-bit tie goes to 0, and a half rounds up.
var quantisationCases = []struct {
	name     string
	in, want Pixel
}{
	{"p-bit tie takes 0", Pixel{1, 1, 0, 0}, Pixel{2, 2, 0, 0}},
	{"a half rounds up", Pixel{4, 5, 5, 5}, Pixel{5, 5, 5, 5}},
}

func twoColourBlock() Block {
	var b Block
	for k := range b {
		v := uint8(darkChannel)
		if k%2 == 1 {
			v = lightChannel
		}
		b[k] = Pixel{v, v, v, opaqueEven}
	}
	return b
}

func maxChannelError(a, b Block) int {
	worst := 0
	for k := range a {
		for c := 0; c < Channels; c++ {
			d := int(a[k][c]) - int(b[k][c])
			if d < 0 {
				d = -d
			}
			if d > worst {
				worst = d
			}
		}
	}
	return worst
}

func TestFixedBlockEncodesToExpectedBytes(t *testing.T) {
	t.Parallel()
	want, err := hex.DecodeString(twoColourHex)
	if err != nil {
		t.Fatal(err)
	}
	got := EncodeBlock(twoColourBlock())
	if !bytes.Equal(got[:], want) {
		t.Fatalf("encoded %x, want %x", got, want)
	}
	back, err := DecodeBlock(got)
	if err != nil {
		t.Fatal(err)
	}
	if back != twoColourBlock() {
		t.Fatalf("two-colour block did not round-trip: %v", back)
	}
}

func TestGradientDecodesWithinStatedError(t *testing.T) {
	t.Parallel()
	var b Block
	for k := range b {
		v := uint8(k * gradientStep)
		b[k] = Pixel{v, v, v, channelMax}
	}
	enc := EncodeBlock(b)
	back, err := DecodeBlock(enc)
	if err != nil {
		t.Fatal(err)
	}
	if e := maxChannelError(b, back); e > gradientBound {
		t.Fatalf("max channel error %d exceeds %d", e, gradientBound)
	}
}

// The p-bit is shared by the four channels of an endpoint, so a solid colour is
// exactly representable when its channels have the same parity; the test covers
// every value of R, B and A (and a parity-matched G) under that condition.
func TestSolidBlocksRoundTripExactly(t *testing.T) {
	t.Parallel()
	for v := 0; v <= channelMax; v++ {
		var b Block
		g := (channelMax-v)&^1 | v&1
		px := Pixel{uint8(v), uint8(g), uint8(v), uint8(v)}
		for k := range b {
			b[k] = px
		}
		back, err := DecodeBlock(EncodeBlock(b))
		if err != nil {
			t.Fatal(err)
		}
		if back != b {
			t.Fatalf("solid %v came back as %v", px, back[0])
		}
	}
}

func squaredDistance(a, b Pixel) int {
	d := 0
	for c := 0; c < Channels; c++ {
		diff := int(a[c]) - int(b[c])
		d += diff * diff
	}
	return d
}

// streamPalette reads the endpoints and p-bits out of enc and returns the
// palette they make, independently of DecodeBlock's pixel indices.
func streamPalette(enc EncodedBlock) [indexLevels]Pixel {
	r := newBitReader(enc)
	r.get(modeBits)
	var e0, e1 [Channels]int
	for c := 0; c < Channels; c++ {
		e0[c] = int(r.get(endpointBits)) << endpointShift
		e1[c] = int(r.get(endpointBits)) << endpointShift
	}
	p0, p1 := int(r.get(pBitBits)), int(r.get(pBitBits))
	for c := 0; c < Channels; c++ {
		e0[c] |= p0
		e1[c] |= p1
	}
	return palette(e0, e1)
}

// The anchor field has 3 bits (its top bit is implied 0), so an encoder that
// wrote an index of 8 or more there would lose its top bit and pixel 0 would
// decode to a palette colour that is not its nearest. Pixel 0 must decode to a
// colour as near as the nearest in the block's own palette.
func TestAnchorIndexTopBitIsClear(t *testing.T) {
	t.Parallel()
	rng := rand.New(rand.NewPCG(randomSeedA, randomSeedB))
	for n := 0; n < randomBlocks; n++ {
		var b Block
		for k := range b {
			for c := 0; c < Channels; c++ {
				b[k][c] = uint8(rng.IntN(channelMax + 1))
			}
		}
		enc := EncodeBlock(b)
		back, err := DecodeBlock(enc)
		if err != nil {
			t.Fatal(err)
		}
		nearest := -1
		for _, pc := range streamPalette(enc) {
			if d := squaredDistance(b[0], pc); nearest < 0 || d < nearest {
				nearest = d
			}
		}
		if got := squaredDistance(b[0], back[0]); got != nearest {
			t.Fatalf("block %d: pixel 0 decoded at squared distance %d, nearest palette colour is %d", n, got, nearest)
		}
	}
}

// A block whose pixel 0 is the light colour forces the endpoint swap; it must
// still decode to the same pixels.
func TestAnchorSwapRoundTrips(t *testing.T) {
	t.Parallel()
	b := twoColourBlock()
	for k := range b {
		if b[k][0] == darkChannel {
			b[k] = Pixel{lightChannel, lightChannel, lightChannel, opaqueEven}
		} else {
			b[k] = Pixel{darkChannel, darkChannel, darkChannel, opaqueEven}
		}
	}
	back, err := DecodeBlock(EncodeBlock(b))
	if err != nil {
		t.Fatal(err)
	}
	if back != b {
		t.Fatalf("swapped block did not round-trip: %v", back)
	}
}

func TestDecodeRefusesOtherModes(t *testing.T) {
	t.Parallel()
	var enc EncodedBlock
	enc[0] = 1 // mode 0
	if _, err := DecodeBlock(enc); !errors.Is(err, ErrNotMode6) {
		t.Fatalf("got %v, want ErrNotMode6", err)
	}
}

func TestImageBlockOrderAndSizes(t *testing.T) {
	t.Parallel()
	w, h := imageBlocksX*BlockDim, imageBlocksY*BlockDim
	img := make([]byte, w*h*Channels)
	for by := 0; by < imageBlocksY; by++ {
		for bx := 0; bx < imageBlocksX; bx++ {
			v := byte((by*imageBlocksX + bx) * imageLevelStep &^ 1)
			for y := 0; y < BlockDim; y++ {
				for x := 0; x < BlockDim; x++ {
					o := ((by*BlockDim+y)*w + bx*BlockDim + x) * Channels
					copy(img[o:], []byte{v, v, v, opaqueEven})
				}
			}
		}
	}
	enc, err := Encode(img, w, h)
	if err != nil {
		t.Fatal(err)
	}
	if len(enc) != imageBlocksX*imageBlocksY*BlockSize {
		t.Fatalf("encoded %d bytes", len(enc))
	}
	// block (bx,by) is the (by*imageBlocksX+bx)'th 16 bytes
	second, err := DecodeBlock(EncodedBlock(enc[BlockSize : 2*BlockSize]))
	if err != nil {
		t.Fatal(err)
	}
	if second[0][0] != byte(imageLevelStep&^1) {
		t.Fatalf("block 1 is not the second block row-major: %v", second[0])
	}
	back, err := Decode(enc, w, h)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(back, img) {
		t.Fatal("solid-block image did not round-trip")
	}
	if _, err := Encode(img[:len(img)-1], w, h); err == nil {
		t.Fatal("short input accepted")
	}
	if _, err := Encode(img, w-1, h); err == nil {
		t.Fatal("size not a multiple of 4 accepted")
	}
	if _, err := Decode(enc[:len(enc)-1], w, h); err == nil {
		t.Fatal("short blocks accepted")
	}
}

// The palette of both endpoint pairs (paletteCases).
func TestDecodeHandPackedPalettes(t *testing.T) {
	t.Parallel()
	for _, tc := range paletteCases {
		raw, err := hex.DecodeString(tc.hex)
		if err != nil {
			t.Fatal(err)
		}
		blk, err := DecodeBlock(EncodedBlock(raw))
		if err != nil {
			t.Fatal(err)
		}
		for k, px := range blk {
			if px != (Pixel{tc.want[k], tc.want[k], tc.want[k], tc.want[k]}) {
				t.Fatalf("%s: pixel %d is %v, want level %d", tc.name, k, px, tc.want[k])
			}
		}
	}
}

// The quantisation choices of quantisationCases.
func TestQuantisationChoices(t *testing.T) {
	t.Parallel()
	for _, tc := range quantisationCases {
		var b Block
		for k := range b {
			b[k] = tc.in
		}
		back, err := DecodeBlock(EncodeBlock(b))
		if err != nil {
			t.Fatal(err)
		}
		if back[0] != tc.want {
			t.Fatalf("%s: %v decoded to %v, want %v", tc.name, tc.in, back[0], tc.want)
		}
	}
}
