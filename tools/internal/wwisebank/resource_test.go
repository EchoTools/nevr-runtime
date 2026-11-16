package wwisebank

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"strings"
	"testing"
)

// The G-SND2b input resource of PLAN-v5 S-SND 4.2, built from the layout the spec gives: 223 bytes, sha256 bdf2d4a0...55a9.
const (
	resBankID         = 0xC2FB13DE
	resBKHDVersion    = 134
	resWrongBankID    = 0xC2FB13DF
	resInputBytes     = 223
	resInputSHA256    = "bdf2d4a070e20a71fade38e92e7aa5b57a465d8cd82d6044874797bf4dad55a9"
	resShortBytes     = 23
	resIDSound        = 0x200
	resIDMixer        = 0x300
	resIDSoundVT      = 0x400
	resIDMixerB       = 0x500
	resIDMedia        = 0x100
	resIDMediaVT      = 0x401
	resVTSize         = 1
	resMediaOffsetVT  = 16
	resMediaSizeMain  = 3
	resDataPadBytes   = 13
	resBKHDShortBytes = 4
	resFillByte       = 0xFF
	resOneU64         = 1
	resWrapperFlag    = 0
	resParentOff      = 21
)

// resT is the T body of PLAN-v5 S-SND 2.2 (29 bytes): an embedded Vorbis Sound with source 0x100, size 3, parent 0x300.
const resT = "01000400 00 00010000 03000000 01 000000 00000000 00030000 00 000000"

// resMixer is an actor-mixer body whose children list is the one child 0x200.
const resMixer = "0505 01000000 00020000"

// resMixerB is an actor-mixer body whose children list is the one child 0x400.
const resMixerB = "0707 01000000 00040000"

func resHex(s string) []byte {
	b, err := hex.DecodeString(strings.ReplaceAll(s, " ", ""))
	if err != nil {
		panic(err)
	}
	return b
}

func resChunk(tag string, payload []byte) []byte {
	out := []byte(tag)
	out = binary.LittleEndian.AppendUint32(out, uint32(len(payload)))
	return append(out, payload...)
}

// resWrap writes the 24-byte wrapper of PLAN-v5 S-SND 1.1 around a bank, with the trailer flag and trailer id given.
func resWrap(bank []byte, flag, id uint32) []byte {
	out := binary.LittleEndian.AppendUint64(nil, uint64(len(bank)))
	out = binary.LittleEndian.AppendUint64(out, 0)
	out = append(out, bank...)
	out = binary.LittleEndian.AppendUint32(out, flag)
	return binary.LittleEndian.AppendUint32(out, id)
}

// resBank is the bank of the G-SND2b input: BKHD, DIDX, DATA and HIRC, in that order.
func resBank() []byte {
	t := resHex(resT)
	vt := append([]byte(nil), t...)
	binary.LittleEndian.PutUint32(vt[soundSourceOff:], resIDMediaVT)
	binary.LittleEndian.PutUint32(vt[soundSizeOff:], resVTSize)
	binary.LittleEndian.PutUint32(vt[resParentOff:], resIDMixerB)

	hirc := EncodeHIRC([]Object{
		{Type: TypeSound, ID: resIDSound, Body: t},
		{Type: TypeActorMixer, ID: resIDMixer, Body: resHex(resMixer)},
		{Type: TypeSound, ID: resIDSoundVT, Body: vt},
		{Type: TypeActorMixer, ID: resIDMixerB, Body: resHex(resMixerB)},
	})

	bkhd := binary.LittleEndian.AppendUint32(nil, resBKHDVersion)
	bkhd = binary.LittleEndian.AppendUint32(bkhd, resBankID)
	didx := binary.LittleEndian.AppendUint32(nil, resIDMedia)
	didx = binary.LittleEndian.AppendUint32(didx, 0)
	didx = binary.LittleEndian.AppendUint32(didx, resMediaSizeMain)
	didx = binary.LittleEndian.AppendUint32(didx, resIDMediaVT)
	didx = binary.LittleEndian.AppendUint32(didx, resMediaOffsetVT)
	didx = binary.LittleEndian.AppendUint32(didx, resVTSize)
	data := append([]byte("abc"), make([]byte, resDataPadBytes)...)
	data = append(data, 'z')

	var out []byte
	out = append(out, resChunk("BKHD", bkhd)...)
	out = append(out, resChunk("DIDX", didx)...)
	out = append(out, resChunk("DATA", data)...)
	return append(out, resChunk("HIRC", hirc)...)
}

// resInput is the 223-byte resource of the spec's test vector.
func resInput() []byte {
	return resWrap(resBank(), resWrapperFlag, resBankID)
}

func resWithU64(b []byte, off int, v uint64) []byte {
	out := append([]byte(nil), b...)
	binary.LittleEndian.PutUint64(out[off:], v)
	return out
}

func TestResourceRoundTrip(t *testing.T) {
	t.Parallel()
	in := resInput()
	if len(in) != resInputBytes {
		t.Fatalf("input is %d bytes, the spec's is %d", len(in), resInputBytes)
	}
	sum := sha256.Sum256(in)
	if got := hex.EncodeToString(sum[:]); got != resInputSHA256 {
		t.Fatalf("input sha256 %s, the spec's %s: the layout is not the spec's", got, resInputSHA256)
	}
	r, err := ParseResource(in)
	if err != nil {
		t.Fatal(err)
	}
	if got := r.Bytes(); !bytes.Equal(got, in) {
		t.Fatalf("Bytes() differs from the input: %d bytes against %d", len(got), len(in))
	}
	if r.TrailerFlag != resWrapperFlag {
		t.Fatalf("TrailerFlag %#x, want %#x", r.TrailerFlag, resWrapperFlag)
	}
	id, err := r.Bank.BankID()
	if err != nil {
		t.Fatal(err)
	}
	if id != resBankID {
		t.Fatalf("BankID %#x, want %#x", id, resBankID)
	}
}

func TestResourceParseCopiesItsInput(t *testing.T) {
	t.Parallel()
	in := resInput()
	want := append([]byte(nil), in...)
	r, err := ParseResource(in)
	if err != nil {
		t.Fatal(err)
	}
	first := r.Bytes()
	for i := range in {
		in[i] = resFillByte
	}
	if got := r.Bytes(); !bytes.Equal(got, want) {
		t.Fatal("the parsed resource shares bytes with its input")
	}
	if !bytes.Equal(first, want) {
		t.Fatal("Bytes() changed between two calls")
	}
	if !bytes.Equal(r.Bank.Bytes(), want[resHeaderBytes:len(want)-resTrailerBytes]) {
		t.Fatal("Bytes() changed the bank it writes")
	}
}

func TestResourceRefusals(t *testing.T) {
	t.Parallel()
	in := resInput()
	bank := in[resHeaderBytes : len(in)-resTrailerBytes]
	bankLen := binary.LittleEndian.Uint64(in)
	cases := []struct {
		name string
		b    []byte
	}{
		{"23 bytes", in[:resShortBytes]},
		{"u64 at 0 one too big", resWithU64(in, 0, bankLen+resOneU64)},
		{"u64 at 0 one too small", resWithU64(in, 0, bankLen-resOneU64)},
		{"u64 at 8 is 1", resWithU64(in, resU64Bytes, resOneU64)},
		{"trailer id off by one", resWrap(bank, resWrapperFlag, resWrongBankID)},
		{"bank truncated by a byte", resWrap(bank[:len(bank)-1], resWrapperFlag, resBankID)},
		{"first chunk not BKHD", resWrap(resChunk("DIDX", nil), resWrapperFlag, resBankID)},
		{"BKHD shorter than its bank id", resWrap(resChunk("BKHD", make([]byte, resBKHDShortBytes)), resWrapperFlag, resBankID)},
	}
	for _, c := range cases {
		_, err := ParseResource(c.b)
		if !errors.Is(err, ErrBadResource) {
			t.Errorf("%s: err = %v, want ErrBadResource", c.name, err)
		}
	}
}
