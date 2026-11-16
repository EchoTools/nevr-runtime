package wwisebank

import (
	"encoding/binary"
	"errors"
	"fmt"
	"math"
	"slices"
)

// ErrNoMedia is returned when the bank has no DIDX or no DATA chunk, so it holds no media to name.
var ErrNoMedia = errors.New("wwisebank: the bank has no DIDX or DATA, or not the media named")

// ErrBadMedia is returned when a DIDX is not whole entries, an entry runs past DATA or an id repeats, or a media value
// given to SetMedia is empty or too large for a u32 offset.
var ErrBadMedia = errors.New("wwisebank: DIDX or DATA is malformed, or a media value is empty")

const (
	// didxEntryBytes is one DIDX entry: a u32 media id, offset into DATA and size (PLAN-v5 S-SND 1.8).
	didxEntryBytes = 12
	// mediaAlign is the alignment of each media's offset in DATA (PLAN-v5 S-SND 1.8).
	mediaAlign = 16
	// didxOffsetOff and didxSizeOff are the offset and size within an entry.
	didxOffsetOff = 4
	didxSizeOff   = 8
	didxIDOff     = 0
	dataTag       = "DATA"
	didxTag       = "DIDX"
)

// mediaEntry is one DIDX entry with the offset and size checked against DATA.
type mediaEntry struct {
	id     uint32
	offset uint32
	size   uint32
}

// mediaEntries reads the DIDX entries of b and the DATA payload they index. It refuses with ErrNoMedia when either chunk
// is absent, and with ErrBadMedia when DIDX is not whole entries, an id repeats, or an entry runs past DATA.
func (b *Bank) mediaEntries() ([]mediaEntry, []byte, error) {
	didx, data := b.Chunk(didxTag), b.Chunk(dataTag)
	if didx == nil || data == nil {
		return nil, nil, ErrNoMedia
	}
	if len(didx.Payload)%didxEntryBytes != 0 {
		return nil, nil, fmt.Errorf("%w: DIDX holds %d bytes, not whole entries", ErrBadMedia, len(didx.Payload))
	}
	n := len(didx.Payload) / didxEntryBytes
	entries := make([]mediaEntry, 0, n)
	seen := make(map[uint32]bool, n)
	for i := 0; i < n; i++ {
		p := didx.Payload[i*didxEntryBytes:]
		e := mediaEntry{
			id:     binary.LittleEndian.Uint32(p[didxIDOff:]),
			offset: binary.LittleEndian.Uint32(p[didxOffsetOff:]),
			size:   binary.LittleEndian.Uint32(p[didxSizeOff:]),
		}
		if seen[e.id] {
			return nil, nil, fmt.Errorf("%w: media id %#x listed twice in DIDX", ErrBadMedia, e.id)
		}
		seen[e.id] = true
		if uint64(e.offset)+uint64(e.size) > uint64(len(data.Payload)) {
			return nil, nil, fmt.Errorf("%w: media id %#x runs past DATA", ErrBadMedia, e.id)
		}
		entries = append(entries, e)
	}
	return entries, data.Payload, nil
}

// Media returns a copy of the media of id, and whether the bank names it. It refuses with ErrNoMedia when the bank has no
// DIDX or DATA.
func (b *Bank) Media(id uint32) ([]byte, bool, error) {
	entries, data, err := b.mediaEntries()
	if err != nil {
		return nil, false, err
	}
	for _, e := range entries {
		if e.id == id {
			return append([]byte(nil), data[e.offset:e.offset+e.size]...), true, nil
		}
	}
	return nil, false, nil
}

// placeMedia returns the offset of a media of size bytes that follows the media ending at end: the next multiple of
// mediaAlign. It reports false when the media would end past a u32, the largest offset DIDX holds.
func placeMedia(end, size uint64) (uint64, bool) {
	offset := (end + mediaAlign - 1) / mediaAlign * mediaAlign
	if offset+size > math.MaxUint32 {
		return 0, false
	}
	return offset, true
}

// SetMedia replaces the media of the named ids and adds the ids the bank does not hold, then rebuilds DIDX and DATA: the
// entries in ascending id order, each media at an offset that is a multiple of mediaAlign, with zero padding before it
// and none after the last (PLAN-v5 S-SND 1.8). A nil map rebuilds the bank's own media. It refuses with ErrNoMedia when
// the bank has no DIDX or DATA, and with ErrBadMedia on an empty value, a malformed DIDX, or a total past a u32 offset;
// a refused call changes nothing. The values are copied into the bank.
func (b *Bank) SetMedia(media map[uint32][]byte) error {
	entries, data, err := b.mediaEntries()
	if err != nil {
		return err
	}
	values := make(map[uint32][]byte, len(entries)+len(media))
	for _, e := range entries {
		values[e.id] = data[e.offset : e.offset+e.size]
	}
	for id, v := range media {
		if len(v) == 0 {
			return fmt.Errorf("%w: media id %#x is empty", ErrBadMedia, id)
		}
		values[id] = v
	}
	ids := make([]uint32, 0, len(values))
	for id := range values {
		ids = append(ids, id)
	}
	slices.Sort(ids)

	var didxOut, dataOut []byte
	var end uint64
	for _, id := range ids {
		v := values[id]
		offset, ok := placeMedia(end, uint64(len(v)))
		if !ok {
			return fmt.Errorf("%w: media id %#x does not fit a u32 offset", ErrBadMedia, id)
		}
		dataOut = append(dataOut, make([]byte, offset-end)...)
		dataOut = append(dataOut, v...)
		didxOut = binary.LittleEndian.AppendUint32(didxOut, id)
		didxOut = binary.LittleEndian.AppendUint32(didxOut, uint32(offset))
		didxOut = binary.LittleEndian.AppendUint32(didxOut, uint32(len(v)))
		end = offset + uint64(len(v))
	}
	b.Chunk(didxTag).Payload = didxOut
	b.Chunk(dataTag).Payload = dataOut
	return nil
}
