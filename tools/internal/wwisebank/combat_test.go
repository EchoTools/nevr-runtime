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

// The expected bytes are hand-derived from PLAN-v5 S-SND 4.2 (its test vector, computed from the layouts of section 1);
// the inputs are synthetic. No byte of a game bank appears here.
const (
	cbAttID        = 0x600
	cbLocEvent     = 0x700
	cbLocAction    = 0x710
	cbLocSound     = 0x720
	cbLocSoundBank = 0x11223344
	cbPointEvent   = 0x800
	cbPointAction  = 0x810
	cbPointSound   = 0x820
	cbPointMedia   = 0x830
	cbVOEvent      = 0x900
	cbVOAction     = 0x910
	cbVOSound      = 0x920
	cbVOMedia      = 0x930
	cbCopies       = 2
	cbNoCopies     = 0
	cbOverCopies   = maxEventActions + 1
	cbMissingMedia = 0x101
	cbWrongSource  = 0x101
	cbPlantAction  = 0x811
	cbCopyDiffers  = "eeef"
	cbAttBody      = "eeee"
	cbVeilPCM      = "01000200"
	cbPointPCM     = "0300"
	cbVOPCM        = "0500"
	// cbDataAt401 and the media offsets that follow are the 16-byte alignment of PLAN-v5 S-SND 1.8, from the vector.
	cbDataAt401    = 80
	cbDataPoint    = 96
	cbDataVO       = 176
	cbDataTailZero = 0x7a
	cbOutBytes     = 744
	cbOutSHA256    = "4e32e8e990c459919e6f0ab92376f3b5ae81f3212756e6e3fb93164ab7b9e9e0"
	cbTrailer      = "00000000 de13fbc2"
	cbTrailerBytes = 8
	// The no-VO list is the vector's objects with the callout block taken out: cbObjects[:cbNoVOHead], the mixer B
	// object, then cbObjects[cbNoVONext:cbNoVOEnd]. cbNoVOObjects is its length.
	cbNoVOHead    = 5
	cbNoVONext    = 7
	cbNoVOEnd     = 13
	cbNoVOObjects = 12
	cbDIDXFull    = "00010000 00000000 44000000 01040000 50000000 01000000 30080000 60000000 42000000 30090000 b0000000 42000000"
	cbDIDXNoVO    = "00010000 00000000 44000000 01040000 50000000 01000000 30080000 60000000 42000000"
)

// cbObjects is the 15 objects of the vector, in the order of the spec's table (# 0..14), each as its encoded bytes.
var cbObjects = []string{
	"0e 06000000 00060000 eeee",
	"02 27000000 00020000 01000100 00 00010000 44000000 01 000000 00000000 00030000 00 01 46 00060000 00 03 0a",
	"02 27000000 20080000 01000100 00 30080000 42000000 01 000000 00000000 00030000 00 01 46 00060000 00 03 0a",
	"07 12000000 00030000 0505 02000000 00020000 20080000",
	"02 21000000 00040000 01000400 00 01040000 01000000 01 000000 00000000 00050000 00 000000",
	"02 21000000 20090000 01000100 00 30090000 42000000 01 000000 00000000 00050000 00 000000",
	"07 12000000 00050000 0707 02000000 00040000 20090000",
	"03 12000000 10070000 0304 20070000 00 00 00 04 44332211",
	"03 12000000 11070000 0304 20070000 00 00 00 04 44332211",
	"04 0d000000 00070000 02 10070000 11070000",
	"03 12000000 10080000 0304 20080000 00 00 00 04 de13fbc2",
	"03 12000000 11080000 0304 20080000 00 00 00 04 de13fbc2",
	"04 0d000000 00080000 02 10080000 11080000",
	"03 12000000 10090000 0304 20090000 00 00 00 04 de13fbc2",
	"04 09000000 00090000 01 10090000",
}

// cbMixerBUnchanged is the template's parent without the VO sound: the object 0x500 of the input, unchanged.
const cbMixerBUnchangedObject = "07 0e000000 00050000 0707 01000000 00040000"

// cbInput is the vector's input: the combat bank's ids and PCM on the 223-byte resource (resInput, resource_test.go).
func cbInput() CombatBank {
	return CombatBank{
		BankID:      resBankID,
		Media:       resIDMedia,
		Sound:       resIDSound,
		PCM:         resHex(cbVeilPCM),
		Attenuation: Object{Type: TypeAttenuation, ID: cbAttID, Body: resHex(cbAttBody)},
		Loc: CombatLoc{
			Event: cbLocEvent, FirstAction: cbLocAction, Sound: cbLocSound, SoundBank: cbLocSoundBank, Copies: cbCopies,
		},
		Point: CombatPoint{
			Event: cbPointEvent, FirstAction: cbPointAction, Sound: cbPointSound, Media: cbPointMedia,
			PCM: resHex(cbPointPCM), Copies: cbCopies,
		},
		VOTemplate: resIDSoundVT,
		VO: []CombatVO{{
			Event: cbVOEvent, Action: cbVOAction, Sound: cbVOSound, Media: cbVOMedia, PCM: resHex(cbVOPCM),
		}},
	}
}

// cbStock is the stock index of the vector: the input resource itself, and a record holding the attenuation (14, 0x600,
// eeee). extra records are added under their own names.
func cbStock(t *testing.T, extra map[string][]byte) *StockIndex {
	t.Helper()
	records := map[string][]byte{
		colNameA: resInput(),
		colNameB: colRecord(t, []Object{{Type: TypeAttenuation, ID: cbAttID, Body: resHex(cbAttBody)}}, nil),
	}
	for name, rec := range extra {
		records[name] = rec
	}
	idx, err := IndexStock(records)
	if err != nil {
		t.Fatalf("IndexStock: %v", err)
	}
	return idx
}

// cbPCM is the PCMWEM of a PCM hex literal, for the expected media bytes.
func cbPCM(t *testing.T, s string) []byte {
	t.Helper()
	wem, err := PCMWEM(resHex(s))
	if err != nil {
		t.Fatalf("PCMWEM(%s): %v", s, err)
	}
	return wem
}

// cbPadTo appends zeros to b until it is off bytes long.
func cbPadTo(b []byte, off int) []byte {
	return append(b, make([]byte, off-len(b))...)
}

// cbExpectedData is the DATA of the vector: the veil at 0, the 0x401 byte at 80, the point at 96, and (with callouts) the
// first callout at 176; each media 16-byte aligned with zero padding before it and none after the last (S-SND 1.8).
func cbExpectedData(t *testing.T, withVO bool) []byte {
	t.Helper()
	out := cbPCM(t, cbVeilPCM)
	out = cbPadTo(out, cbDataAt401)
	out = append(out, cbDataTailZero)
	out = cbPadTo(out, cbDataPoint)
	out = append(out, cbPCM(t, cbPointPCM)...)
	if withVO {
		out = cbPadTo(out, cbDataVO)
		out = append(out, cbPCM(t, cbVOPCM)...)
	}
	return out
}

func cbOutObjects(t *testing.T, out []byte) ([]Object, *Bank) {
	t.Helper()
	r, err := ParseResource(out)
	if err != nil {
		t.Fatalf("output is not a resource: %v", err)
	}
	objs, err := ParseHIRC(r.Bank.Chunk(hircTag).Payload)
	if err != nil {
		t.Fatalf("output HIRC: %v", err)
	}
	return objs, r.Bank
}

func cbCompareObjects(t *testing.T, got []Object, want []string) {
	t.Helper()
	if len(got) != len(want) {
		t.Fatalf("HIRC holds %d objects, want %d", len(got), len(want))
	}
	for i := range want {
		if g, w := encEncode(got[i]), encHex(t, want[i]); !bytes.Equal(g, w) {
			t.Errorf("object %d: got %x, want %x", i, g, w)
		}
	}
}

func TestBuildCombatBankMatchesTheLayout(t *testing.T) {
	out, err := BuildCombatBank(resInput(), cbInput(), cbStock(t, nil))
	if err != nil {
		t.Fatalf("BuildCombatBank: %v", err)
	}
	objs, bank := cbOutObjects(t, out)
	cbCompareObjects(t, objs, cbObjects)
	if got, want := bank.Chunk(didxTag).Payload, encHex(t, cbDIDXFull); !bytes.Equal(got, want) {
		t.Errorf("DIDX: got %x, want %x", got, want)
	}
	if got, want := bank.Chunk(dataTag).Payload, cbExpectedData(t, true); !bytes.Equal(got, want) {
		t.Errorf("DATA: got %d bytes %x, want %d bytes %x", len(got), got, len(want), want)
	}
	if len(out) != cbOutBytes {
		t.Errorf("resource is %d bytes, want %d", len(out), cbOutBytes)
	}
	if sum := sha256.Sum256(out); hex.EncodeToString(sum[:]) != cbOutSHA256 {
		t.Errorf("sha256 %x, want %s", sum, cbOutSHA256)
	}
	if got, want := out[len(out)-cbTrailerBytes:], encHex(t, cbTrailer); !bytes.Equal(got, want) {
		t.Errorf("trailer %x, want %x", got, want)
	}
}

func TestBuildCombatBankWithoutVO(t *testing.T) {
	in := cbInput()
	in.VO = nil
	out, err := BuildCombatBank(resInput(), in, cbStock(t, nil))
	if err != nil {
		t.Fatalf("BuildCombatBank: %v", err)
	}
	want := append(append([]string{}, cbObjects[:cbNoVOHead]...), cbMixerBUnchangedObject)
	want = append(want, cbObjects[cbNoVONext:cbNoVOEnd]...)
	if len(want) != cbNoVOObjects {
		t.Fatalf("test table holds %d objects, want %d", len(want), cbNoVOObjects)
	}
	objs, bank := cbOutObjects(t, out)
	cbCompareObjects(t, objs, want)
	if got, w := bank.Chunk(didxTag).Payload, encHex(t, cbDIDXNoVO); !bytes.Equal(got, w) {
		t.Errorf("DIDX without VO: got %x, want %x", got, w)
	}
	if got, w := bank.Chunk(dataTag).Payload, cbExpectedData(t, false); !bytes.Equal(got, w) {
		t.Errorf("DATA without VO: got %d bytes, want %d", len(got), len(w))
	}
}

func TestBuildCombatBankRefusesAPlantedCollision(t *testing.T) {
	plantedObject := colRecord(t, []Object{{Type: TypeAction, ID: cbPlantAction, Body: make([]byte, colSoundBody)}}, nil)
	stock := cbStock(t, map[string][]byte{colNameC: plantedObject})
	_, err := BuildCombatBank(resInput(), cbInput(), stock)
	if !errors.Is(err, ErrIDCollision) || !strings.Contains(err.Error(), "0x811") ||
		!strings.Contains(err.Error(), colNameC) {
		t.Fatalf("planted object 0x811: got %v, want ErrIDCollision naming 0x811 and record %s", err, colNameC)
	}

	plantedMedia := colRecord(t, nil, []uint32{cbVOMedia})
	stock = cbStock(t, map[string][]byte{colNameD: plantedMedia})
	_, err = BuildCombatBank(resInput(), cbInput(), stock)
	if !errors.Is(err, ErrIDCollision) || !strings.Contains(err.Error(), "media") ||
		!strings.Contains(err.Error(), colNameD) {
		t.Fatalf("planted media 0x930: got %v, want ErrIDCollision naming media in record %s", err, colNameD)
	}

	plantedVOAction := colRecord(t, []Object{{Type: TypeAction, ID: cbVOAction, Body: make([]byte, colSoundBody)}}, nil)
	stock = cbStock(t, map[string][]byte{colNameC: plantedVOAction})
	_, err = BuildCombatBank(resInput(), cbInput(), stock)
	if !errors.Is(err, ErrIDCollision) || !strings.Contains(err.Error(), "0x910") {
		t.Fatalf("planted VO action 0x910: got %v, want ErrIDCollision naming 0x910", err)
	}
}

func TestBuildCombatBankRefusesACopyThatDiffers(t *testing.T) {
	differs := colRecord(t, []Object{{Type: TypeAttenuation, ID: cbAttID, Body: resHex(cbCopyDiffers)}}, nil)
	stock := cbStock(t, map[string][]byte{colNameC: differs})
	if _, err := BuildCombatBank(resInput(), cbInput(), stock); !errors.Is(err, ErrCopyDiffers) {
		t.Fatalf("attenuation body differs: got %v, want ErrCopyDiffers", err)
	}
}

// cbTargetWith returns the vector's target resource with its HIRC objects changed by edit.
func cbTargetWith(t *testing.T, edit func([]Object) []Object) []byte {
	t.Helper()
	r, err := ParseResource(resInput())
	if err != nil {
		t.Fatalf("ParseResource: %v", err)
	}
	objs, err := ParseHIRC(r.Bank.Chunk(hircTag).Payload)
	if err != nil {
		t.Fatalf("ParseHIRC: %v", err)
	}
	r.Bank.Chunk(hircTag).Payload = EncodeHIRC(edit(objs))
	return r.Bytes()
}

func TestBuildCombatBankRefusesTheWrongTarget(t *testing.T) {
	soundWrongSource := cbTargetWith(t, func(objs []Object) []Object {
		for i := range objs {
			if objs[i].ID == resIDSound {
				binary.LittleEndian.PutUint32(objs[i].Body[soundSourceOff:], cbWrongSource)
			}
		}
		return objs
	})
	attPresent := cbTargetWith(t, func(objs []Object) []Object {
		return append(objs, Object{Type: TypeAttenuation, ID: cbAttID, Body: resHex(cbAttBody)})
	})

	cases := []struct {
		name   string
		target []byte
		edit   func(*CombatBank)
		want   error
	}{
		{"bank id", resInput(), func(in *CombatBank) { in.BankID = resWrongBankID }, ErrWrongTarget},
		{"media absent", resInput(), func(in *CombatBank) { in.Media = cbMissingMedia }, ErrNoMedia},
		{"sound source differs", soundWrongSource, func(in *CombatBank) {}, ErrBadSound},
		{"attenuation in target", attPresent, func(in *CombatBank) {}, ErrIDCollision},
		{"no loc copies", resInput(), func(in *CombatBank) { in.Loc.Copies = cbNoCopies }, ErrBadCombat},
		{"loc copies over the bound", resInput(), func(in *CombatBank) { in.Loc.Copies = cbOverCopies }, ErrBadCombat},
		{"no point copies", resInput(), func(in *CombatBank) { in.Point.Copies = cbNoCopies }, ErrBadCombat},
		{"point copies over the bound", resInput(), func(in *CombatBank) { in.Point.Copies = cbOverCopies }, ErrBadCombat},
	}
	for _, c := range cases {
		in := cbInput()
		c.edit(&in)
		before := append([]byte(nil), c.target...)
		out, err := BuildCombatBank(c.target, in, cbStock(t, nil))
		if !errors.Is(err, c.want) {
			t.Errorf("%s: got %v, want %v", c.name, err, c.want)
		}
		if out != nil {
			t.Errorf("%s: returned %d bytes with the error", c.name, len(out))
		}
		if !bytes.Equal(before, c.target) {
			t.Errorf("%s: target changed", c.name)
		}
	}
}

// A point or callout media id that is the replaced veil slot would overwrite the veil's media: refused, even when the
// stock index does not hold the target itself (the added check of the build, see combat.go).
func TestBuildCombatBankRefusesTheReplacedMediaAsCreated(t *testing.T) {
	in := cbInput()
	in.Point.Media = in.Media
	// The stock index holds only the attenuation record, not the target, so the veil's media id is not stock.
	idx, err := IndexStock(map[string][]byte{
		colNameB: colRecord(t, []Object{{Type: TypeAttenuation, ID: cbAttID, Body: resHex(cbAttBody)}}, nil),
	})
	if err != nil {
		t.Fatalf("IndexStock: %v", err)
	}
	if _, err := BuildCombatBank(resInput(), in, idx); !errors.Is(err, ErrIDCollision) {
		t.Fatalf("point media equal to the veil slot: got %v, want ErrIDCollision", err)
	}
}

const (
	cbVOEvent2  = 0x901
	cbVOAction2 = 0x911
	cbVOSound2  = 0x921
	cbVOMedia2  = 0x931
)

// Each callout's Play is followed by its Event, line after line: an event listed after all the plays would change the
// order that the one-line vector cannot show.
func TestBuildCombatBankTwoCalloutsKeepTheirOrder(t *testing.T) {
	in := cbInput()
	in.VO = append(in.VO, CombatVO{
		Event: cbVOEvent2, Action: cbVOAction2, Sound: cbVOSound2, Media: cbVOMedia2, PCM: resHex(cbVOPCM),
	})
	out, err := BuildCombatBank(resInput(), in, cbStock(t, nil))
	if err != nil {
		t.Fatalf("BuildCombatBank: %v", err)
	}
	objs, _ := cbOutObjects(t, out)
	callout := map[uint32]bool{cbVOAction: true, cbVOEvent: true, cbVOAction2: true, cbVOEvent2: true}
	var got []uint32
	for _, o := range objs {
		if callout[o.ID] {
			got = append(got, o.ID)
		}
	}
	want := []uint32{cbVOAction, cbVOEvent, cbVOAction2, cbVOEvent2}
	if len(got) != len(want) {
		t.Fatalf("callout objects %x, want %x", got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("callout objects %x, want %x", got, want)
		}
	}
}
