package main

// The OVERCHARGE sparks texture over synthetic records in the shipped layout: a 256-byte descriptor and a DDS payload
// with the DX10 extension. No byte of the game's is used. The expected blocks are worked out by hand from buff_assets.py's
// arithmetic, each with its sum in a comment, and the DDS layout is written with the test's own constants, not
// steps_buff_sparks.go's, so a mutant of the code's offsets or the code's arithmetic cannot match by construction.

import (
	"bytes"
	"encoding/binary"
	"math"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

// The DDS layout and the sizes, as the test's own copy.
const (
	testDescLen   = 256
	testHeaderLen = 148
	testSizeOff   = 4
	testSizeValue = 124
	testFourCCOff = 84
	testDX10      = 0x30315844
	testDXGIOff   = 128
	testBC1       = 71
	testBlockLen  = 8
	testOtherType = 5
	testOtherName = 5
	testFourCCBad = 0
	testDXGIBad   = 72
	testSizeBad   = 128
	testColor1Off = 2
	testIndexOff  = 4
)

// The three blocks of the test payload and what each must become. The arithmetic is buff_assets.py's, worked by hand.
const (
	// White: c0 0xFFFF is (255, 255, 255), c1 0x0000 is black. m @ (255, 255, 255) = (224.4, 48.96, 408), clipped to
	// (224.4, 48.96, 255), times (31, 63, 31) / 255 = (27.28, 12.09, 31), rounded (27, 12, 31): 27<<11 | 12<<5 | 31 = 0xD99F.
	// Black stays black. 0xFFFF > 0 and 0xD99F > 0, so nothing flips and the indices are kept.
	whiteC0    = 0xFFFF
	whiteC1    = 0x0000
	whiteOutC0 = 0xD99F
	whiteIdx   = 0x12345678

	// Blue: c0 0x001F is (0, 0, 255), c1 0x0020 is (0, 4.05, 0). m @ (0, 0, 255) = (25.58, 5.58, 46.51), which encodes to
	// (3, 1, 6) = 3<<11 | 1<<5 | 6 = 6182 (0x1826). m @ (0, 4.05, 0) = (2.09, 0.46, 3.80), which encodes to 0. The
	// colours compare the other way (31 > 32 is false, 6182 > 0 is true): three-colour, so the endpoints swap and the
	// indices 0 and 1 swap, and 2 and 3 stay. Index 0 is in fourteen slots and becomes 1 (bits 01); the input 0x0000000E
	// has slot 0 = 2 and slot 1 = 3, which stay: 14 x 01 + 2 + 3<<2 = 0x5555555E.
	blueC0     = 0x001F
	blueC1     = 0x0020
	blueOutC0  = 0x0000
	blueOutC1  = 0x1826
	blueIdx    = 0x0000000E
	blueOutIdx = 0x5555555E

	// Equal: c0 0x0020 is (0, 4.05, 0) and encodes to 0, and so does c1 0x0000. 0x20 > 0 but 0 > 0 is false: four-colour,
	// so the endpoints swap and the indices 0 and 1 swap, 2 and 3 swap (0x0000000E: slot 0 = 2 becomes 3, slot 1 = 3
	// becomes 2): fourteen slots 0 -> 1 give 14 x 01, then 3 + 2<<2 = 0x5555555B. Since the new endpoints are equal c0
	// takes one more step, so it stays the larger: c0 1, c1 0.
	eqC0     = 0x0020
	eqC1     = 0x0000
	eqOutC0  = 0x0001
	eqOutC1  = 0x0000
	eqIdx    = 0x0000000E
	eqOutIdx = 0x5555555B
)

func sparksKey(typ, name uint64) radpack.Key {
	return radpack.Key{Type: int64(typ), Name: int64(name)}
}

// sparksPack is a pack whose set 0 lists the stock descriptor and payload, and whose set 1 lists one other resource.
func sparksPack(t *testing.T, desc, payload []byte) *Pack {
	t.Helper()
	return rulesPack(t,
		map[radpack.Key][]byte{sparksKey(sparksDescType, sparksTex): desc, sparksKey(sparksGPUType, sparksTex): payload},
		map[radpack.Key][]byte{sparksKey(testOtherType, testOtherName): []byte("another resource")})
}

// testDDS is a DDS header of the test's layout with the given size field, fourCC and dxgiFormat.
func testDDS(size, fourCC, dxgi uint32) []byte {
	h := make([]byte, testHeaderLen)
	copy(h, "DDS ")
	binary.LittleEndian.PutUint32(h[testSizeOff:], size)
	binary.LittleEndian.PutUint32(h[testFourCCOff:], fourCC)
	binary.LittleEndian.PutUint32(h[testDXGIOff:], dxgi)
	return h
}

func testBlock(c0, c1 uint16, idx uint32) []byte {
	b := make([]byte, testBlockLen)
	binary.LittleEndian.PutUint16(b[0:], c0)
	binary.LittleEndian.PutUint16(b[testColor1Off:], c1)
	binary.LittleEndian.PutUint32(b[testIndexOff:], idx)
	return b
}

// validPayload is a stock-shaped payload: the DX10 BC1 header and the three test blocks.
func validPayload() ([]byte, [][]byte) {
	blocks := [][]byte{
		testBlock(whiteC0, whiteC1, whiteIdx),
		testBlock(blueC0, blueC1, blueIdx),
		testBlock(eqC0, eqC1, eqIdx),
	}
	return bytes.Join(append([][]byte{testDDS(testSizeValue, testDX10, testBC1)}, blocks...), nil), blocks
}

func validDesc() []byte {
	d := make([]byte, testDescLen)
	for i := range d {
		d[i] = byte(i)
	}
	return d
}

func TestTheSparksCopyNameIsItsCSymbol64Hash(t *testing.T) {
	// CSymbol64Hash("fx_overcharge_sparks_tex") from sdk/include/nevr/symbol_hash.h, compiled with g++ -std=c++17 and run.
	const want = 0x6ecd271f1b348eae
	if sparksCopy != want {
		t.Errorf("sparksCopy is %016x, the engine hash of its name is %016x", sparksCopy, uint64(want))
	}
}

func TestTheSparksStepWritesTheTwoCopiesInSetZeroAndNothingElse(t *testing.T) {
	desc := validDesc()
	payload, _ := validPayload()
	p := sparksPack(t, desc, payload)
	if err := sparksTexture(p); err != nil {
		t.Fatal(err)
	}
	if n := len(p.changes[Sets[0]]); n != 2 {
		t.Fatalf("set 0 has %d changes, want the two copies", n)
	}
	if n := len(p.changes[Sets[1]]); n != 0 {
		t.Fatalf("set 1 has %d changes, want none", n)
	}
	if got := p.changes[Sets[0]][sparksKey(sparksDescType, sparksCopy)]; !bytes.Equal(got, desc) {
		t.Errorf("the descriptor copy is not the stock descriptor")
	}
	got := p.changes[Sets[0]][sparksKey(sparksGPUType, sparksCopy)]
	if len(got) != len(payload) {
		t.Fatalf("payload copy of %d bytes, want %d", len(got), len(payload))
	}
	if !bytes.Equal(got[:testHeaderLen], payload[:testHeaderLen]) {
		t.Error("the DDS header of the copy changed")
	}
}

func TestTheRecoloredBlocksAreTheHandWorkedOnes(t *testing.T) {
	payload, _ := validPayload()
	p := sparksPack(t, validDesc(), payload)
	if err := sparksTexture(p); err != nil {
		t.Fatal(err)
	}
	got := p.changes[Sets[0]][sparksKey(sparksGPUType, sparksCopy)]
	want := []struct{ c0, c1, idx uint32 }{
		{whiteOutC0, whiteC1, whiteIdx},
		{blueOutC0, blueOutC1, blueOutIdx},
		{eqOutC0, eqOutC1, eqOutIdx},
	}
	for i, w := range want {
		o := testHeaderLen + i*testBlockLen
		c0 := binary.LittleEndian.Uint16(got[o:])
		c1 := binary.LittleEndian.Uint16(got[o+testColor1Off:])
		idx := binary.LittleEndian.Uint32(got[o+testIndexOff:])
		if uint32(c0) != w.c0 || uint32(c1) != w.c1 || idx != w.idx {
			t.Errorf("block %d: c0 %04x c1 %04x idx %08x, want c0 %04x c1 %04x idx %08x",
				i, c0, c1, idx, w.c0, w.c1, w.idx)
		}
	}
}

// A payload or descriptor that is not the stock shape is refused, the refusal names the field, and nothing is written.
func TestAStockRecordThatIsNotTheExpectedOneIsRefusedAndNothingIsWritten(t *testing.T) {
	good, _ := validPayload()
	withTail := func(b []byte, n int) []byte { return append(append([]byte(nil), b...), make([]byte, n)...) }
	cases := []struct {
		name    string
		desc    []byte
		payload []byte
		want    string
	}{
		{"descriptor one byte short", make([]byte, testDescLen-1), good, "descriptor"},
		{"dxgiFormat is not BC1", validDesc(), validDDSWith(testDXGIBad), "dxgiFormat"},
		{"no DX10 fourCC", validDesc(), validDDSWith(testFourCCBad), "fourCC"},
		{"header size is not 124", validDesc(), validDDSWithSize(testSizeBad), "header size"},
		{"not a DDS file", validDesc(), append([]byte("DDX "), good[testSizeOff:]...), "magic"},
		{"three stray bytes after the blocks", validDesc(), withTail(good, 3), "whole"},
		{"a payload with no blocks", validDesc(), good[:testHeaderLen], "whole"},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			p := sparksPack(t, c.desc, c.payload)
			err := sparksTexture(p)
			if err == nil || !strings.Contains(err.Error(), c.want) {
				t.Fatalf("err %v, want one naming %q", err, c.want)
			}
			if n := len(p.changes[Sets[0]]); n != 0 {
				t.Errorf("a refused step wrote %d records", n)
			}
		})
	}
}

// validDDSWith is the stock header with its dxgiFormat or its fourCC replaced, followed by the test's three blocks.
func validDDSWith(v uint32) []byte {
	_, blocks := validPayload()
	h := testDDS(testSizeValue, testDX10, testBC1)
	switch v {
	case testDXGIBad:
		binary.LittleEndian.PutUint32(h[testDXGIOff:], testDXGIBad)
	case testFourCCBad:
		binary.LittleEndian.PutUint32(h[testFourCCOff:], testFourCCBad)
	}
	return bytes.Join(append([][]byte{h}, blocks...), nil)
}

func validDDSWithSize(size uint32) []byte {
	_, blocks := validPayload()
	return bytes.Join(append([][]byte{testDDS(size, testDX10, testBC1)}, blocks...), nil)
}

// Every declared field is in the step's output, and its Read gives the bytes of the record it names.
func TestTheSparksFieldsReadWhatTheStepWrites(t *testing.T) {
	payload, _ := validPayload()
	p := sparksPack(t, validDesc(), payload)
	var step *Step
	for i := range steps {
		if steps[i].Name == sparksStep {
			step = &steps[i]
		}
	}
	if step == nil {
		t.Fatal("the sparks step is not registered")
	}
	if err := step.Apply(p); err != nil {
		t.Fatal(err)
	}
	if len(step.Writes) != 2 {
		t.Fatalf("%d declared fields, want the descriptor and the payload", len(step.Writes))
	}
	// The descriptor field, then the pixel payload field, each under its own record type.
	wantTypes := []uint64{sparksDescType, sparksGPUType}
	for i, f := range step.Writes {
		if f.Type != wantTypes[i] {
			t.Errorf("field %d (%s) declares type %016x, want %016x", i, f.Name, f.Type, wantTypes[i])
		}
	}
	for _, f := range step.Writes {
		for _, rec := range f.Records {
			b, ok := p.changes[f.Set][sparksKey(f.Type, rec)]
			if !ok {
				t.Fatalf("field %q: record %016x is not written", f.Name, rec)
			}
			got, err := f.Read(b)
			if err != nil || len(got) == 0 {
				t.Errorf("field %q: read %d bytes, %v", f.Name, len(got), err)
			}
		}
	}
}

// The purple matrix's elements are products of buff_assets.py's constants, gain 1.6 (:49), purple (0.55, 0.12, 1.0) (:48)
// and luma (0.299, 0.587, 0.114) (:172), worked by hand: element [i][j] is 1.6 * purple[i] * luma[j].
const (
	matrixTol = 1e-9
	m00       = 0.26312  // 1.6 * 0.55 * 0.299
	m01       = 0.51656  // 1.6 * 0.55 * 0.587
	m02       = 0.10032  // 1.6 * 0.55 * 0.114
	m10       = 0.057408 // 1.6 * 0.12 * 0.299
	m11       = 0.112704 // 1.6 * 0.12 * 0.587
	m12       = 0.021888 // 1.6 * 0.12 * 0.114
	m20       = 0.4784   // 1.6 * 1.0 * 0.299
	m21       = 0.9392   // 1.6 * 1.0 * 0.587
	m22       = 0.1824   // 1.6 * 1.0 * 0.114
)

func TestThePurpleMatrixIsTheProductsOfTheToolsConstants(t *testing.T) {
	got := purpleMatrix()
	want := [rgbChannels][rgbChannels]float64{{m00, m01, m02}, {m10, m11, m12}, {m20, m21, m22}}
	for i := range want {
		for j := range want[i] {
			if math.Abs(got[i][j]-want[i][j]) > matrixTol {
				t.Errorf("element [%d][%d] is %.9f, want %.9f", i, j, got[i][j], want[i][j])
			}
		}
	}
}
