package wwisebank

import (
	"bytes"
	"encoding/hex"
	"errors"
	"strings"
	"testing"
)

// Expected bytes are hand-derived from the layouts in PLAN-v5 S-SND section 1 (the game's own media and objects of
// bank 03f1464db1fe49b5 are the measured shape); the inputs are synthetic. No byte of a game bank appears here.
const (
	encSource     = 0xAABBCCDD
	encSize       = 0x44
	encStreamed   = 2
	encAttenuated = 0x44444444
	encPlayID     = 0x01020304
	encPlayTarget = 0xA0B0C0D0
	encPlayBank   = 0xC2FB13DE
	encEventID    = 0x0A0B0C0D
	encFindLow    = 0x10
	encFindMid    = 0x20
	encFindHigh   = 0x30
	encFindMiss   = 0x40
	encOne        = 1
	encAfterCount = 255

	// encSoundT is a synthetic Sound body with no FX, props or positioning: plugin Vorbis, source 0x100, size 3,
	// sourceBits 1, parent 0x300 (29 bytes).
	encSoundT = "01000400 00 00010000 03000000 01 000000 00000000 00030000 00 000000"
	// encSoundTPositioned is encSoundT after Positioned with attenuation encAttenuated (35 bytes).
	encSoundTPositioned = "0100040000 00010000 03000000 01 000000 00000000 00030000 00 01 46 44444444 00 03 0a"
	// encSoundTPCM is encSoundT after SetSoundSource(PCM, source encSource, size encSize).
	encSoundTPCM = "01000100 00 ddccbbaa 44000000 01 000000 00000000 00030000 00 000000"
	// encPCMRIFF is the PCMWEM of the samples 0100ff7f (68 bytes), matching the game's own PCM media header (1.9).
	encPCMRIFF = "52494646 3c000000 57415645 666d7420 18000000 feff 0100 80bb0000 00770100 0200 1000 0600 0000 01410000 4a554e4b 04000000 00000000 64617461 04000000 0100ff7f"
	// encPlayStock is the stock Play object of the id, target and bank above: the header and the 14-byte body (1.4).
	encPlayStock = "03 12000000 04030201 0304 d0c0b0a0 00 00 00 04 de13fbc2"
	// encEventTwo is an Event with two actions (13 bytes).
	encEventTwo = "04 0d000000 0d0c0b0a 02 01000000 04030201"
)

// encHex decodes a hex literal that may hold spaces.
func encHex(t *testing.T, s string) []byte {
	t.Helper()
	b, err := hex.DecodeString(strings.ReplaceAll(s, " ", ""))
	if err != nil {
		t.Fatalf("bad hex literal %q: %v", s, err)
	}
	return b
}

// encClone copies b, so a test can change one byte of its input.
func encClone(b []byte) []byte { return append([]byte(nil), b...) }

// encEncode writes one object as a HIRC payload and drops the object count, leaving the object's own bytes.
func encEncode(o Object) []byte { return EncodeHIRC([]Object{o})[hircCountBytes:] }

func TestPCMWEMIsTheGamesHeader(t *testing.T) {
	t.Parallel()
	got, err := PCMWEM(encHex(t, "0100ff7f"))
	if err != nil {
		t.Fatal(err)
	}
	if want := encHex(t, encPCMRIFF); !bytes.Equal(got, want) {
		t.Fatalf("PCMWEM:\n got %x\nwant %x", got, want)
	}
}

func TestPCMWEMRefusesEmptyAndOdd(t *testing.T) {
	t.Parallel()
	for name, pcm := range map[string][]byte{"empty": nil, "odd": encHex(t, "010203")} {
		if _, err := PCMWEM(pcm); !errors.Is(err, ErrBadPCM) {
			t.Errorf("%s: err %v, want ErrBadPCM", name, err)
		}
	}
}

func TestSetSoundSourceKeepsEverythingElse(t *testing.T) {
	t.Parallel()
	got, err := SetSoundSource(encHex(t, encSoundT), PluginPCM, encSource, encSize)
	if err != nil {
		t.Fatal(err)
	}
	if want := encHex(t, encSoundTPCM); !bytes.Equal(got, want) {
		t.Fatalf("SetSoundSource:\n got %x\nwant %x", got, want)
	}
}

func TestSetSoundSourceRefusesAStreamedOrShortBody(t *testing.T) {
	t.Parallel()
	streamed := encClone(encHex(t, encSoundT))
	streamed[soundStreamOff] = encStreamed
	short := encHex(t, encSoundT)[:soundSourceBytes-encOne]
	for name, body := range map[string][]byte{"streamed": streamed, "short": short} {
		if _, err := SetSoundSource(body, PluginPCM, encSource, encSize); !errors.Is(err, ErrBadSound) {
			t.Errorf("%s: err %v, want ErrBadSound", name, err)
		}
	}
}

func TestPositionedAddsTheAttenuation(t *testing.T) {
	t.Parallel()
	got, err := Positioned(encHex(t, encSoundT), encAttenuated)
	if err != nil {
		t.Fatal(err)
	}
	if want := encHex(t, encSoundTPositioned); !bytes.Equal(got, want) {
		t.Fatalf("Positioned:\n got %x\nwant %x", got, want)
	}
}

func TestPositionedRefusesFxPropsOrByte16(t *testing.T) {
	t.Parallel()
	fx := encClone(encHex(t, encSoundT))
	fx[soundFxCountOff] = encOne
	byte16 := encClone(encHex(t, encSoundT))
	byte16[soundByte16Off] = encOne
	props := encClone(encHex(t, encSoundT))
	props[soundPropsOff] = encOne
	short := encHex(t, encSoundT)[:soundPropsEnd-encOne]
	for name, body := range map[string][]byte{"fx": fx, "byte16": byte16, "props": props, "short": short} {
		if _, err := Positioned(body, encAttenuated); !errors.Is(err, ErrBadSound) {
			t.Errorf("%s: err %v, want ErrBadSound", name, err)
		}
	}
}

func TestPlayActionIsTheStockShape(t *testing.T) {
	t.Parallel()
	got := encEncode(PlayAction(encPlayID, encPlayTarget, encPlayBank))
	if want := encHex(t, encPlayStock); !bytes.Equal(got, want) {
		t.Fatalf("PlayAction:\n got %x\nwant %x", got, want)
	}
}

func TestEventCountsItsActions(t *testing.T) {
	t.Parallel()
	ev, err := Event(encEventID, []uint32{encOne, encPlayID})
	if err != nil {
		t.Fatal(err)
	}
	if got, want := encEncode(ev), encHex(t, encEventTwo); !bytes.Equal(got, want) {
		t.Fatalf("Event:\n got %x\nwant %x", got, want)
	}
}

func TestEventRefuses256Actions(t *testing.T) {
	t.Parallel()
	ids := make([]uint32, maxEventActions+encOne)
	for i := range ids {
		ids[i] = uint32(i)
	}
	if _, err := Event(encEventID, ids); !errors.Is(err, ErrTooMany) {
		t.Fatalf("256 actions: err %v, want ErrTooMany", err)
	}
	ev, err := Event(encEventID, ids[:encAfterCount])
	if err != nil {
		t.Fatalf("255 actions refused: %v", err)
	}
	if want := eventCountBytes + actionIDBytes*maxEventActions; len(ev.Body) != want {
		t.Fatalf("255 actions: body %d bytes, want %d", len(ev.Body), want)
	}
	if ev.Body[0] != maxEventActions {
		t.Fatalf("255 actions: count byte %d, want %d", ev.Body[0], maxEventActions)
	}
}

func TestEventWithNoActions(t *testing.T) {
	t.Parallel()
	ev, err := Event(encEventID, nil)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(ev.Body, []byte{0}) {
		t.Fatalf("zero actions: body %x, want 00", ev.Body)
	}
}

func TestFindObject(t *testing.T) {
	t.Parallel()
	objs := []Object{
		{Type: TypeSound, ID: encFindLow, Body: []byte("a")},
		{Type: TypeAttenuation, ID: encFindMid, Body: []byte("bb")},
		{Type: TypeAttenuation, ID: encFindHigh, Body: []byte("c")},
		{Type: TypeAttenuation, ID: encFindHigh, Body: []byte("d")},
	}
	got, err := FindObject(objs, encFindMid, TypeAttenuation)
	if err != nil {
		t.Fatal(err)
	}
	if string(got.Body) != "bb" {
		t.Fatalf("found body %q, want bb", got.Body)
	}
	got.Body[0] = 'x'
	if string(objs[1].Body) != "bb" {
		t.Fatalf("FindObject returned the stock slice, not a copy: %q", objs[1].Body)
	}
	if _, err := FindObject(objs, encFindMid, TypeSound); !errors.Is(err, ErrNoObject) {
		t.Errorf("other type: err %v, want ErrNoObject", err)
	}
	if _, err := FindObject(objs, encFindMiss, TypeAttenuation); !errors.Is(err, ErrNoObject) {
		t.Errorf("absent id: err %v, want ErrNoObject", err)
	}
	if _, err := FindObject(objs, encFindHigh, TypeAttenuation); !errors.Is(err, ErrAmbiguous) {
		t.Errorf("two objects: err %v, want ErrAmbiguous", err)
	}
}
