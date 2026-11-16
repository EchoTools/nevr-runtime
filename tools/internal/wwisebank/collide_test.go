package wwisebank

import (
	"bytes"
	"encoding/binary"
	"errors"
	"strings"
	"testing"
)

const (
	colBankID         = 0x0BADF00D
	colNameA          = "aaaaaaaaaaaaaaaa"
	colNameB          = "bbbbbbbbbbbbbbbb"
	colNameC          = "cccccccccccccccc"
	colNameD          = "dddddddddddddddd"
	colPlantAction    = 0x710
	colPlantMedia     = 0x830
	colPlantSource    = 0x931
	colFreeID         = 0x700
	colCopyID         = 0x600
	colCopyAbsent     = 0x601
	colCopyType       = 7
	colActionType     = 3
	colSoundBody      = 14
	colCopyBody       = "eeee"
	colCopyBodyOff    = "eeef"
	colSmallID        = 0x700
	colRaggedBytes    = 5
	colShortSoundBody = 4
	colBadHIRCCount   = 1
)

// colNotAResource is a stock record that is not a resource: three bytes, too short for a header.
var colNotAResource = []byte{1, 2, 3}

// colSound is a Sound body with the given sourceID at +5 (PLAN-v5 S-SND 1.3) and zeros elsewhere.
func colSound(source uint32) []byte {
	body := make([]byte, colSoundBody)
	binary.LittleEndian.PutUint32(body[soundSourceOff:], source)
	return body
}

// colRecord is a stock resource with the objects given and a DIDX that lists the media ids.
func colRecord(t *testing.T, objs []Object, media []uint32) []byte {
	t.Helper()
	bkhd := binary.LittleEndian.AppendUint32(nil, resBKHDVersion)
	bkhd = binary.LittleEndian.AppendUint32(bkhd, colBankID)
	var didx []byte
	for _, id := range media {
		didx = binary.LittleEndian.AppendUint32(didx, id)
		didx = binary.LittleEndian.AppendUint32(didx, 0)
		didx = binary.LittleEndian.AppendUint32(didx, 0)
	}
	var bank []byte
	bank = append(bank, resChunk("BKHD", bkhd)...)
	bank = append(bank, resChunk("DIDX", didx)...)
	bank = append(bank, resChunk("HIRC", EncodeHIRC(objs))...)
	return resWrap(bank, 0, colBankID)
}

func colHex(s string) []byte { return resHex(s) }

func TestCheckCreatedRefusesAPlantedID(t *testing.T) {
	t.Parallel()
	records := map[string][]byte{
		colNameA: colRecord(t, []Object{{Type: colActionType, ID: colPlantAction, Body: colHex("aa")}}, nil),
		colNameB: colRecord(t, nil, []uint32{colPlantMedia}),
		colNameC: colRecord(t, []Object{{Type: TypeSound, ID: colSmallID + 1, Body: colSound(colPlantSource)}}, nil),
	}
	stock, err := IndexStock(records)
	if err != nil {
		t.Fatal(err)
	}

	err = stock.CheckCreated([]uint32{colFreeID, colPlantAction})
	if !errors.Is(err, ErrIDCollision) || !strings.Contains(err.Error(), "0x710") ||
		!strings.Contains(err.Error(), "object type 3") || !strings.Contains(err.Error(), colNameA) {
		t.Fatalf("planted object id: err = %v", err)
	}
	err = stock.CheckCreated([]uint32{colPlantMedia})
	if !errors.Is(err, ErrIDCollision) || !strings.Contains(err.Error(), "media") {
		t.Fatalf("stock DIDX media id: err = %v", err)
	}
	err = stock.CheckCreated([]uint32{colPlantSource})
	if !errors.Is(err, ErrIDCollision) || !strings.Contains(err.Error(), "media") {
		t.Fatalf("stock Sound source id: err = %v", err)
	}
	if err := stock.CheckCreated([]uint32{colFreeID, colFreeID}); !errors.Is(err, ErrIDCollision) {
		t.Fatalf("a repeated created id: err = %v", err)
	}
	if err := stock.CheckCreated([]uint32{colFreeID}); err != nil {
		t.Fatalf("a free id: %v", err)
	}
	if err := stock.CheckCreated(nil); err != nil {
		t.Fatalf("no ids: %v", err)
	}
}

func TestCheckCopy(t *testing.T) {
	t.Parallel()
	att := Object{Type: TypeAttenuation, ID: colCopyID, Body: colHex(colCopyBody)}
	records := map[string][]byte{
		colNameA: colRecord(t, []Object{att}, nil),
		colNameB: colRecord(t, []Object{att}, nil),
		colNameC: colRecord(t, []Object{{Type: TypeAttenuation, ID: colSmallID, Body: colHex("ee")}}, nil),
		colNameD: colRecord(t, []Object{{Type: TypeAttenuation, ID: colSmallID, Body: colHex("ef")}}, nil),
	}
	stock, err := IndexStock(records)
	if err != nil {
		t.Fatal(err)
	}
	if err := stock.CheckCopy(att); err != nil {
		t.Fatalf("an identical copy: %v", err)
	}
	cases := []struct {
		name string
		o    Object
		want string
	}{
		{"body differs", Object{Type: TypeAttenuation, ID: colCopyID, Body: colHex(colCopyBodyOff)}, colNameA},
		{"type differs", Object{Type: colCopyType, ID: colCopyID, Body: colHex(colCopyBody)}, colNameA},
		{"id absent", Object{Type: TypeAttenuation, ID: colCopyAbsent, Body: colHex(colCopyBody)}, ""},
		{"one of two stock records differs", Object{Type: TypeAttenuation, ID: colSmallID, Body: colHex("ee")}, colNameD},
	}
	for _, c := range cases {
		err := stock.CheckCopy(c.o)
		if !errors.Is(err, ErrCopyDiffers) {
			t.Errorf("%s: err = %v, want ErrCopyDiffers", c.name, err)
			continue
		}
		if c.want != "" && !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: the error %q does not name record %s", c.name, err, c.want)
		}
	}
}

func TestIndexStockLeavesItsRecordsAndCopiesAlone(t *testing.T) {
	t.Parallel()
	rec := colRecord(t, []Object{{Type: TypeAttenuation, ID: colCopyID, Body: colHex(colCopyBody)}}, nil)
	records := map[string][]byte{colNameA: rec}
	snapshot := append([]byte(nil), rec...)

	stock, err := IndexStock(records)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(records[colNameA], snapshot) {
		t.Fatal("IndexStock changed a record it was given")
	}
	// The index is built from copies: changing the records afterwards does not change its answers.
	for i := range rec {
		rec[i] = resFillByte
	}
	copyOf := Object{Type: TypeAttenuation, ID: colCopyID, Body: colHex(colCopyBody)}
	if err := stock.CheckCopy(copyOf); err != nil {
		t.Fatalf("the index shares bytes with its records: %v", err)
	}
	// CheckCopy does not change the object it is given.
	if !bytes.Equal(copyOf.Body, colHex(colCopyBody)) || copyOf.Type != TypeAttenuation {
		t.Fatal("CheckCopy changed its argument")
	}
}

func TestIndexStockRefusesABadRecord(t *testing.T) {
	t.Parallel()
	_, err := IndexStock(map[string][]byte{colNameA: colNotAResource})
	if !errors.Is(err, ErrBadResource) || !strings.Contains(err.Error(), colNameA) {
		t.Fatalf("a record that is not a resource: err = %v", err)
	}
}

// colRaw is a stock resource whose chunks after BKHD are the given payloads, each under its tag.
func colRaw(tagged map[string][]byte, order []string) []byte {
	bkhd := binary.LittleEndian.AppendUint32(nil, resBKHDVersion)
	bkhd = binary.LittleEndian.AppendUint32(bkhd, colBankID)
	bank := resChunk("BKHD", bkhd)
	for _, tag := range order {
		bank = append(bank, resChunk(tag, tagged[tag])...)
	}
	return resWrap(bank, 0, colBankID)
}

func TestIndexStockRefusesABadPiece(t *testing.T) {
	t.Parallel()
	raggedDIDX := colRaw(map[string][]byte{"DIDX": make([]byte, colRaggedBytes)}, []string{"DIDX"})
	shortSound := colRecord(t, []Object{{Type: TypeSound, ID: colFreeID, Body: make([]byte, colShortSoundBody)}}, nil)
	badHIRC := colRaw(map[string][]byte{"HIRC": binary.LittleEndian.AppendUint32(nil, colBadHIRCCount)}, []string{"HIRC"})
	cases := []struct {
		name string
		rec  []byte
	}{
		{"ragged DIDX", raggedDIDX},
		{"Sound too short for its source", shortSound},
		{"HIRC that does not hold its count", badHIRC},
	}
	for _, c := range cases {
		_, err := IndexStock(map[string][]byte{colNameA: c.rec})
		if !errors.Is(err, ErrBadResource) || !strings.Contains(err.Error(), colNameA) {
			t.Errorf("%s: err = %v, want ErrBadResource naming the record", c.name, err)
		}
	}
}

func TestResourceBytesWritesZeroForAShortBKHD(t *testing.T) {
	t.Parallel()
	r := &Resource{Bank: &Bank{Chunks: []Chunk{medChunk("BKHD", make([]byte, colShortSoundBody))}}, TrailerFlag: 1}
	got := r.Bytes()
	if !bytes.Equal(got[len(got)-resU32Bytes:], make([]byte, resU32Bytes)) {
		t.Fatalf("a hand-built resource with a short BKHD: trailer id %x, want zero", got[len(got)-resU32Bytes:])
	}
}
