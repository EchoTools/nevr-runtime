package wwisebank

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"math"
)

// The HIRC object types and the Sound property this package writes (PLAN-v5 S-SND section 1, measured on the stock banks).
const (
	TypeSound       uint8 = 2
	TypeAction      uint8 = 3
	TypeEvent       uint8 = 4
	TypeActorMixer  uint8 = 7
	TypeAttenuation uint8 = 14

	PluginPCM    uint32 = 0x00010001
	PluginVorbis uint32 = 0x00040001
	ActionPlay   uint16 = 0x0403

	PropAttenuationID uint8 = 0x46
)

var (
	// ErrBadSound is returned when a Sound body is not the shape the edit takes.
	ErrBadSound = errors.New("wwisebank: sound body is not the shape this edit takes")
	// ErrBadPCM is returned when PCM is empty, odd or too long for its RIFF header.
	ErrBadPCM = errors.New("wwisebank: PCM is empty, odd or too long")
	// ErrTooMany is returned when an event would hold more actions than its u8 count allows.
	ErrTooMany = errors.New("wwisebank: an event holds at most 255 actions")
	// ErrNoObject is returned when no object of the id and type is in the list.
	ErrNoObject = errors.New("wwisebank: no object of that id and type")
	// ErrAmbiguous is returned when more than one object of the id and type is in the list.
	ErrAmbiguous = errors.New("wwisebank: the id names more than one object")
)

const (
	// soundSourceBytes is the length of a Sound body up to and including sourceBits (byte 13), the part SetSoundSource reads.
	soundSourceBytes = 14
	soundPluginOff   = 0
	soundStreamOff   = 4
	soundSourceOff   = 5
	soundSizeOff     = 9
	soundFxCountOff  = 15
	soundByte16Off   = 16
	soundPropsOff    = 26
	soundPropsEnd    = 29

	// positioningBits3D is the positioning byte that sets bit 1 (a bits3D byte follows it): 3D positioning, with
	// bits3DAttenuated as that byte (PLAN-v5 S-SND 1.3; the meaning is the tool's named fact, not verified).
	positioningBits3D = 3
	bits3DAttenuated  = 10

	// playFadeCurve is the Play action's fade byte, 4 in all 1714 stock 14-byte Plays with no props (1.4).
	playFadeCurve = 4
	playBodyBytes = 14

	// maxEventActions is the most actions an event holds: its count is a u8.
	maxEventActions = 255
	eventCountBytes = 1
	actionIDBytes   = 4

	// wemHeaderBytes is the length of the RIFF header before a PCM WEM's samples (1.9); riffHeaderBytes is its
	// "RIFF" tag and size, which the RIFF size does not count.
	wemHeaderBytes  = 64
	riffHeaderBytes = 8
	fmtPayloadBytes = 24
	junkBytes       = 4
	// The fmt fields of a PCM WEM, as the game's own PCM media carry them (1.9).
	wemFormatTag      = 0xFFFE
	wemChannels       = 1
	wemSampleRate     = 48000
	wemAvgBytesPerSec = 96000
	wemBlockAlign     = 2
	wemBitsPerSample  = 16
	wemCbSize         = 6
	wemExtension      = 0
	// wemChannelMask is the channel config word: one channel, front centre.
	wemChannelMask = 0x4101

	// maxPCMBytes keeps the RIFF size, wemHeaderBytes-riffHeaderBytes+len, inside a u32.
	maxPCMBytes = math.MaxUint32 - (wemHeaderBytes - riffHeaderBytes)
)

// PCMWEM wraps s16 mono 48 kHz samples in the RIFF header the game's own PCM media carry (PLAN-v5 S-SND 1.9).
func PCMWEM(pcm []byte) ([]byte, error) {
	if len(pcm) == 0 || len(pcm)%2 != 0 || uint64(len(pcm)) > maxPCMBytes {
		return nil, ErrBadPCM
	}
	n := uint32(len(pcm))
	out := make([]byte, 0, wemHeaderBytes+len(pcm))
	out = append(out, "RIFF"...)
	out = binary.LittleEndian.AppendUint32(out, wemHeaderBytes-riffHeaderBytes+n)
	out = append(out, "WAVE"...)
	out = append(out, "fmt "...)
	out = binary.LittleEndian.AppendUint32(out, fmtPayloadBytes)
	out = binary.LittleEndian.AppendUint16(out, wemFormatTag)
	out = binary.LittleEndian.AppendUint16(out, wemChannels)
	out = binary.LittleEndian.AppendUint32(out, wemSampleRate)
	out = binary.LittleEndian.AppendUint32(out, wemAvgBytesPerSec)
	out = binary.LittleEndian.AppendUint16(out, wemBlockAlign)
	out = binary.LittleEndian.AppendUint16(out, wemBitsPerSample)
	out = binary.LittleEndian.AppendUint16(out, wemCbSize)
	out = binary.LittleEndian.AppendUint16(out, wemExtension)
	out = binary.LittleEndian.AppendUint32(out, wemChannelMask)
	out = append(out, "JUNK"...)
	out = binary.LittleEndian.AppendUint32(out, junkBytes)
	out = append(out, make([]byte, junkBytes)...)
	out = append(out, "data"...)
	out = binary.LittleEndian.AppendUint32(out, n)
	return append(out, pcm...), nil
}

// SetSoundSource returns a copy of a Sound body with pluginID, streamType 0, sourceID and inMemoryMediaSize set; every
// other byte (sourceBits at +13 included) is kept. A body that is not embedded (streamType not 0) is refused.
func SetSoundSource(body []byte, plugin, source, size uint32) ([]byte, error) {
	if len(body) < soundSourceBytes || body[soundStreamOff] != 0 {
		return nil, ErrBadSound
	}
	out := append([]byte(nil), body...)
	binary.LittleEndian.PutUint32(out[soundPluginOff:], plugin)
	out[soundStreamOff] = 0
	binary.LittleEndian.PutUint32(out[soundSourceOff:], source)
	binary.LittleEndian.PutUint32(out[soundSizeOff:], size)
	return out, nil
}

// Positioned returns a copy of a Sound body with no FX, props or positioning, given AttenuationID and 3D positioning
// (PLAN-v5 S-SND 1.3). It refuses a body with an FX array, a byte 16 that is not 0, or a prop count, ranged count or
// positioning byte that is not 0 (the three bytes at 26 to 28 are replaced, so they must be empty).
func Positioned(body []byte, attenuation uint32) ([]byte, error) {
	var zeroProps [soundPropsEnd - soundPropsOff]byte
	if len(body) < soundPropsEnd || body[soundFxCountOff] != 0 || body[soundByte16Off] != 0 ||
		!bytes.Equal(body[soundPropsOff:soundPropsEnd], zeroProps[:]) {
		return nil, ErrBadSound
	}
	out := make([]byte, 0, len(body)+positionedTailBytes)
	out = append(out, body[:soundPropsOff]...)
	out = append(out, 1, PropAttenuationID)
	out = binary.LittleEndian.AppendUint32(out, attenuation)
	out = append(out, 0, positioningBits3D, bits3DAttenuated)
	return append(out, body[soundPropsEnd:]...), nil
}

// positionedTailBytes is the length of the prop count, the prop id, its u32 value, the ranged count, the positioning
// byte and the bits3D byte that Positioned writes where the three zero bytes were.
const positionedTailBytes = 1 + 1 + 4 + 1 + 1 + 1

// PlayAction is a 14-byte Play of target, which bank holds (PLAN-v5 S-SND 1.4).
func PlayAction(id, target, bank uint32) Object {
	body := make([]byte, 0, playBodyBytes)
	body = binary.LittleEndian.AppendUint16(body, ActionPlay)
	body = binary.LittleEndian.AppendUint32(body, target)
	body = append(body, 0, 0, 0, playFadeCurve)
	body = binary.LittleEndian.AppendUint32(body, bank)
	return Object{Type: TypeAction, ID: id, Body: body}
}

// Event is an Event that runs actions in order (PLAN-v5 S-SND 1.5). An event holds at most maxEventActions actions.
func Event(id uint32, actions []uint32) (Object, error) {
	if len(actions) > maxEventActions {
		return Object{}, ErrTooMany
	}
	body := make([]byte, 0, eventCountBytes+actionIDBytes*len(actions))
	body = append(body, byte(len(actions)))
	for _, a := range actions {
		body = binary.LittleEndian.AppendUint32(body, a)
	}
	return Object{Type: TypeEvent, ID: id, Body: body}, nil
}

// FindObject returns a copy of the one object of id and typ. It refuses with ErrNoObject when none has both, and with
// ErrAmbiguous when two do.
func FindObject(objs []Object, id uint32, typ uint8) (Object, error) {
	var hit Object
	matches := 0
	for _, o := range objs {
		if o.ID == id && o.Type == typ {
			hit = o
			matches++
		}
	}
	switch {
	case matches == 0:
		return Object{}, fmt.Errorf("%w: id %#x type %d", ErrNoObject, id, typ)
	case matches > 1:
		return Object{}, fmt.Errorf("%w: id %#x type %d", ErrAmbiguous, id, typ)
	}
	hit.Body = append([]byte(nil), hit.Body...)
	return hit, nil
}
