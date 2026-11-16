package wwisebank

import (
	"bytes"
	"encoding/binary"
	"errors"
	"testing"
)

const (
	medEntryBytes     = 12
	medAlign          = 16
	medIDBase         = 0x100
	medIDNew          = 0x401
	medIDSmaller      = 0x050
	medSizeBase       = 3
	medSizeReplaced   = 20
	medOffsetNew      = 16
	medOffsetAfterPad = 32
	medSizeNew        = 1
	medPadToNew       = 13
	medMissingID      = 0x999
	medOverlongSize   = 9
	medDIDXRaggedLen  = 13
)

var (
	medBase   = []byte("abc")
	medNewVal = []byte("z")
)

func medChunk(tag string, payload []byte) Chunk {
	var c Chunk
	copy(c.Tag[:], tag)
	c.Payload = payload
	return c
}

// medDIDX writes entries of (id, offset, size) as the DIDX chunk does.
func medDIDX(entries ...[3]uint32) []byte {
	var out []byte
	for _, e := range entries {
		out = binary.LittleEndian.AppendUint32(out, e[0])
		out = binary.LittleEndian.AppendUint32(out, e[1])
		out = binary.LittleEndian.AppendUint32(out, e[2])
	}
	return out
}

// medBank is a bank of only the DIDX and DATA chunks SetMedia reads.
func medBank(didx, data []byte) *Bank {
	return &Bank{Chunks: []Chunk{medChunk("DIDX", didx), medChunk("DATA", data)}}
}

// medBaseBank is the bank of PLAN-v5 S-SND 4.1: one media 0x100 at 0, size 3, DATA "abc".
func medBaseBank() *Bank {
	return medBank(medDIDX([3]uint32{medIDBase, 0, medSizeBase}), medBase)
}

func medPayload(t *testing.T, b *Bank, tag string) []byte {
	t.Helper()
	c := b.Chunk(tag)
	if c == nil {
		t.Fatalf("no %s chunk", tag)
	}
	return c.Payload
}

func medPad(n int) []byte { return make([]byte, n) }

func TestSetMediaAlignsAndSorts(t *testing.T) {
	t.Parallel()
	b := medBaseBank()
	if err := b.SetMedia(map[uint32][]byte{medIDNew: medNewVal}); err != nil {
		t.Fatal(err)
	}
	wantDIDX := medDIDX([3]uint32{medIDBase, 0, medSizeBase}, [3]uint32{medIDNew, medOffsetNew, medSizeNew})
	if got := medPayload(t, b, "DIDX"); !bytes.Equal(got, wantDIDX) {
		t.Fatalf("DIDX %x, want %x", got, wantDIDX)
	}
	wantData := append(append(append([]byte(nil), medBase...), medPad(medPadToNew)...), medNewVal...)
	if got := medPayload(t, b, "DATA"); !bytes.Equal(got, wantData) {
		t.Fatalf("DATA %x, want %x", got, wantData)
	}

	// A new id below the existing one sorts first: the DIDX is ascending, not in insertion order.
	s := medBaseBank()
	if err := s.SetMedia(map[uint32][]byte{medIDSmaller: medNewVal}); err != nil {
		t.Fatal(err)
	}
	wantDIDX = medDIDX([3]uint32{medIDSmaller, 0, medSizeNew}, [3]uint32{medIDBase, medOffsetNew, medSizeBase})
	if got := medPayload(t, s, "DIDX"); !bytes.Equal(got, wantDIDX) {
		t.Fatalf("sorted DIDX %x, want %x", got, wantDIDX)
	}
	wantData = append(append(append([]byte(nil), medNewVal...), medPad(medAlign-medSizeNew)...), medBase...)
	if got := medPayload(t, s, "DATA"); !bytes.Equal(got, wantData) {
		t.Fatalf("sorted DATA %x, want %x", got, wantData)
	}
}

func TestSetMediaReplacesInPlace(t *testing.T) {
	t.Parallel()
	b := medBaseBank()
	if err := b.SetMedia(map[uint32][]byte{medIDNew: medNewVal}); err != nil {
		t.Fatal(err)
	}
	replaced := bytes.Repeat([]byte{'a'}, medSizeReplaced)
	if err := b.SetMedia(map[uint32][]byte{medIDBase: replaced}); err != nil {
		t.Fatal(err)
	}
	wantDIDX := medDIDX([3]uint32{medIDBase, 0, medSizeReplaced}, [3]uint32{medIDNew, medOffsetAfterPad, medSizeNew})
	if got := medPayload(t, b, "DIDX"); !bytes.Equal(got, wantDIDX) {
		t.Fatalf("DIDX %x, want %x", got, wantDIDX)
	}
	wantData := append(append(append([]byte(nil), replaced...), medPad(medOffsetAfterPad-medSizeReplaced)...), medNewVal...)
	if got := medPayload(t, b, "DATA"); !bytes.Equal(got, wantData) {
		t.Fatalf("DATA %x, want %x", got, wantData)
	}
}

func TestSetMediaOnTheCanonicalLayoutIsTheIdentity(t *testing.T) {
	t.Parallel()
	b := medBank(medDIDX([3]uint32{medIDBase, 0, medSizeBase}, [3]uint32{medIDNew, medOffsetNew, medSizeNew}),
		append(append(append([]byte(nil), medBase...), medPad(medPadToNew)...), medNewVal...))
	before := append(append([]byte(nil), medPayload(t, b, "DIDX")...), medPayload(t, b, "DATA")...)
	if err := b.SetMedia(nil); err != nil {
		t.Fatal(err)
	}
	after := append(append([]byte(nil), medPayload(t, b, "DIDX")...), medPayload(t, b, "DATA")...)
	if !bytes.Equal(before, after) {
		t.Fatalf("a rebuild of a canonical layout changed it: %x", after)
	}
}

func TestSetMediaRefusalsLeaveTheBankAlone(t *testing.T) {
	t.Parallel()
	dupDIDX := medDIDX([3]uint32{medIDBase, 0, medSizeBase}, [3]uint32{medIDBase, medOffsetNew, medSizeNew})
	cases := []struct {
		name  string
		b     *Bank
		media map[uint32][]byte
		want  error
	}{
		{"no DIDX", &Bank{Chunks: []Chunk{medChunk("DATA", medBase)}}, map[uint32][]byte{medIDNew: medNewVal}, ErrNoMedia},
		{"no DATA", &Bank{Chunks: []Chunk{medChunk("DIDX", medDIDX())}}, map[uint32][]byte{medIDNew: medNewVal}, ErrNoMedia},
		{"empty value", medBaseBank(), map[uint32][]byte{medIDNew: nil}, ErrBadMedia},
		{"ragged DIDX", medBank(make([]byte, medDIDXRaggedLen), medBase), nil, ErrBadMedia},
		{"duplicate id in DIDX", medBank(dupDIDX, medBase), nil, ErrBadMedia},
		{"entry past DATA", medBank(medDIDX([3]uint32{medIDBase, 0, medOverlongSize}), medBase), nil, ErrBadMedia},
	}
	for _, c := range cases {
		before := c.b.Bytes()
		err := c.b.SetMedia(c.media)
		if !errors.Is(err, c.want) {
			t.Errorf("%s: err = %v, want %v", c.name, err, c.want)
		}
		if !bytes.Equal(c.b.Bytes(), before) {
			t.Errorf("%s: a refused SetMedia changed the bank", c.name)
		}
	}
}

func TestSetMediaCopiesItsValues(t *testing.T) {
	t.Parallel()
	b := medBaseBank()
	val := []byte("q")
	if err := b.SetMedia(map[uint32][]byte{medIDNew: val}); err != nil {
		t.Fatal(err)
	}
	val[0] = resFillByte
	got, found, err := b.Media(medIDNew)
	if err != nil || !found || !bytes.Equal(got, []byte("q")) {
		t.Fatalf("Media after the caller changed its slice: %q found=%v err=%v", got, found, err)
	}
}

func TestMediaReadsTheNamedMedia(t *testing.T) {
	t.Parallel()
	b := medBaseBank()
	if err := b.SetMedia(map[uint32][]byte{medIDNew: medNewVal}); err != nil {
		t.Fatal(err)
	}
	got, found, err := b.Media(medIDBase)
	if err != nil || !found || !bytes.Equal(got, medBase) {
		t.Fatalf("Media(base) = %q found=%v err=%v", got, found, err)
	}
	got, found, err = b.Media(medIDNew)
	if err != nil || !found || !bytes.Equal(got, medNewVal) {
		t.Fatalf("Media(new) = %q found=%v err=%v", got, found, err)
	}
	got, found, err = b.Media(medMissingID)
	if err != nil || found || got != nil {
		t.Fatalf("Media(missing) = %q found=%v err=%v, want nil, false, nil", got, found, err)
	}
	// The returned slice is a copy: changing it leaves the bank as it was.
	got, _, _ = b.Media(medIDBase)
	got[0] = resFillByte
	again, _, _ := b.Media(medIDBase)
	if !bytes.Equal(again, medBase) {
		t.Fatal("Media returned a slice that shares the bank's bytes")
	}
	noMedia := &Bank{Chunks: []Chunk{medChunk("HIRC", nil)}}
	if _, _, err := noMedia.Media(medIDBase); !errors.Is(err, ErrNoMedia) {
		t.Fatalf("Media on a bank with no DIDX: err = %v, want ErrNoMedia", err)
	}
}

func TestPlaceMediaRefusesAnOffsetPastAU32(t *testing.T) {
	t.Parallel()
	const maxU32 = 1<<32 - 1
	if off, ok := placeMedia(0, maxU32); !ok || off != 0 {
		t.Fatalf("a media of u32 size at 0: off=%d ok=%v", off, ok)
	}
	if off, ok := placeMedia(1, maxU32); ok {
		t.Fatalf("a media after one byte, of u32 size, fits a u32: off=%d", off)
	}
	if off, ok := placeMedia(medPadToNew, medSizeNew); !ok || off != medAlign {
		t.Fatalf("the media after 13 bytes: off=%d ok=%v, want %d", off, ok, medAlign)
	}
}
