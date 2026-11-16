package wwisebank

import (
	"bytes"
	"encoding/binary"
	"errors"
	"testing"
)

const (
	testBankID      = 0xA1B2C3D4
	testBankVersion = 145
	testIDOne       = 0x11111111
	testIDTwo       = 0x22222222
	testTypeSound   = 2
	testTypeEvent   = 4
	testDataBytes   = 37
	testHIRCCount   = 1
	testObjects     = 2
	testFill        = 0xAB
)

var soundBody = []byte("opaque")

func u32(v uint32) []byte { return binary.LittleEndian.AppendUint32(nil, v) }

func chunkBytes(tag string, payload []byte) []byte {
	return append(append([]byte(tag), u32(uint32(len(payload)))...), payload...)
}

func syntheticHIRC() []byte {
	objs := []Object{
		{Type: testTypeSound, ID: testIDOne, Body: soundBody},
		{Type: testTypeEvent, ID: testIDTwo, Body: nil},
	}
	return EncodeHIRC(objs)
}

func syntheticBank() []byte {
	bkhd := append(u32(testBankVersion), u32(testBankID)...)
	var out []byte
	out = append(out, chunkBytes("BKHD", bkhd)...)
	out = append(out, chunkBytes("DIDX", append(u32(testIDOne), u32(0)...))...)
	out = append(out, chunkBytes("DATA", bytes.Repeat([]byte{testFill}, testDataBytes))...)
	out = append(out, chunkBytes("HIRC", syntheticHIRC())...)
	return out
}

func TestRoundTripIsByteForByte(t *testing.T) {
	t.Parallel()
	in := syntheticBank()
	bank, err := Parse(in)
	if err != nil {
		t.Fatal(err)
	}
	var tags []string
	for _, c := range bank.Chunks {
		tags = append(tags, string(c.Tag[:]))
	}
	if got := len(tags); got != len([]string{"BKHD", "DIDX", "DATA", "HIRC"}) {
		t.Fatalf("tags %v", tags)
	}
	if got := bank.Bytes(); !bytes.Equal(got, in) {
		t.Fatalf("round trip differs:\n got %x\nwant %x", got, in)
	}
}

func TestHIRCObjectsRoundTripOpaque(t *testing.T) {
	t.Parallel()
	bank, err := Parse(syntheticBank())
	if err != nil {
		t.Fatal(err)
	}
	h := bank.Chunk("HIRC")
	if h == nil {
		t.Fatal("no HIRC")
	}
	objs, err := ParseHIRC(h.Payload)
	if err != nil {
		t.Fatal(err)
	}
	if len(objs) != testObjects || objs[0].ID != testIDOne || objs[0].Type != testTypeSound ||
		!bytes.Equal(objs[0].Body, soundBody) || objs[1].ID != testIDTwo || len(objs[1].Body) != 0 {
		t.Fatalf("objects %+v", objs)
	}
	if back := EncodeHIRC(objs); !bytes.Equal(back, h.Payload) {
		t.Fatalf("hirc round trip differs: %x", back)
	}
}

func TestTruncatedChunkIsRefused(t *testing.T) {
	t.Parallel()
	in := syntheticBank()
	for _, cut := range []int{1, testDataBytes, len(in) - 1} {
		if _, err := Parse(in[:len(in)-cut]); !errors.Is(err, ErrTruncated) {
			t.Errorf("cut %d: err %v, want ErrTruncated", cut, err)
		}
	}
	const shortBytes = 4
	if _, err := Parse(in[:shortBytes]); !errors.Is(err, ErrTruncated) {
		t.Errorf("short header: %v", err)
	}
}

func TestMalformedInputsAreRefused(t *testing.T) {
	t.Parallel()
	if _, err := Parse(nil); err == nil {
		t.Error("empty bank accepted")
	}
	if _, err := Parse(chunkBytes("DATA", nil)); !errors.Is(err, ErrNoHeader) {
		t.Errorf("first chunk not BKHD: %v", err)
	}
	good := syntheticHIRC()
	if _, err := ParseHIRC(good[:len(good)-1]); !errors.Is(err, ErrBadHIRC) {
		t.Errorf("truncated object: %v", err)
	}
	if _, err := ParseHIRC(append(append([]byte{}, good...), 0)); !errors.Is(err, ErrBadHIRC) {
		t.Errorf("trailing byte: %v", err)
	}
	const tooSmall = hircIDBytes - 1
	bad := append(u32(testHIRCCount), append([]byte{testTypeSound}, append(u32(tooSmall), 0, 0, 0)...)...)
	if _, err := ParseHIRC(bad); !errors.Is(err, ErrBadHIRC) {
		t.Errorf("object without an id: %v", err)
	}
}
