package wwisebank

import (
	"encoding/binary"
	"errors"
	"fmt"
)

// ErrWrongTarget is returned when the target bank is not the bank the combat bank edits (its BKHD bank id differs).
var ErrWrongTarget = errors.New("wwisebank: the target is not the bank the combat bank edits")

// ErrBadCombat is returned when a combat input is out of range: a copy count outside 1 to maxEventActions.
var ErrBadCombat = errors.New("wwisebank: a combat input is out of range")

const (
	// soundParentOff and soundParentEnd bound a Sound's directParentID (PLAN-v5 S-SND 1.3, offset 21, u32).
	soundParentOff = 21
	soundParentEnd = 25
	// combatMinCopies is the fewest copies a sound is stacked (1); the most is maxEventActions, the count of one event.
	combatMinCopies = 1
)

// CombatBank is the input of BuildCombatBank: the ids and PCM of the combat sounds (PLAN-v5 S-SND 4.2). Every id is an
// input; none is chosen here.
type CombatBank struct {
	BankID      uint32 // expected BKHD bank id of the target
	Media       uint32 // the replaced media (the veil slot)
	Sound       uint32 // the Sound that plays it
	PCM         []byte // the veil's s16 mono 48 kHz samples
	Attenuation Object // copied from stock (type 14)
	Loc         CombatLoc
	Point       CombatPoint
	VOTemplate  uint32
	VO          []CombatVO // empty: no callouts (INTENT Q1)
}

// CombatLoc is the translocator event: Copies Plays of Sound from SoundBank, all under Event.
type CombatLoc struct {
	Event, FirstAction, Sound, SoundBank uint32
	Copies                               int
}

// CombatPoint is the point sound: its own Sound and media, played Copies times under Event.
type CombatPoint struct {
	Event, FirstAction, Sound, Media uint32
	PCM                              []byte
	Copies                           int
}

// CombatVO is one callout line: its event, action (Play), Sound and media.
type CombatVO struct {
	Event, Action, Sound, Media uint32
	PCM                         []byte
}

// BuildCombatBank returns the target resource with the combat sounds (his tool's replace_sound, veil_sound.py:177-286),
// after the collision check against stock. It returns bytes only on success; target is not changed.
func BuildCombatBank(target []byte, in CombatBank, stock *StockIndex) ([]byte, error) {
	r, err := ParseResource(target)
	if err != nil {
		return nil, err
	}
	id, err := r.Bank.BankID()
	if err != nil {
		return nil, err
	}
	if id != in.BankID {
		return nil, fmt.Errorf("%w: bank id %#x, the target's is %#x", ErrWrongTarget, in.BankID, id)
	}
	if in.Loc.Copies < combatMinCopies || in.Loc.Copies > maxEventActions ||
		in.Point.Copies < combatMinCopies || in.Point.Copies > maxEventActions {
		return nil, fmt.Errorf("%w: copies loc=%d point=%d, each 1 to %d", ErrBadCombat,
			in.Loc.Copies, in.Point.Copies, maxEventActions)
	}

	// Media (his :194-216): the veil's media is replaced, the point's and each callout's are added.
	if _, ok, err := r.Bank.Media(in.Media); err != nil {
		return nil, err
	} else if !ok {
		return nil, fmt.Errorf("%w: media %#x", ErrNoMedia, in.Media)
	}
	veilWEM, err := PCMWEM(in.PCM)
	if err != nil {
		return nil, err
	}
	pointWEM, err := PCMWEM(in.Point.PCM)
	if err != nil {
		return nil, err
	}
	media := map[uint32][]byte{in.Media: veilWEM, in.Point.Media: pointWEM}
	createdMedia := []uint32{in.Point.Media}
	for _, line := range in.VO {
		wem, err := PCMWEM(line.PCM)
		if err != nil {
			return nil, err
		}
		media[line.Media] = wem
		createdMedia = append(createdMedia, line.Media)
	}
	if err := r.Bank.SetMedia(media); err != nil {
		return nil, err
	}

	// HIRC (his :218-242): the attenuation goes first; the Sound is positioned and set to the veil's PCM.
	chunk := r.Bank.Chunk(hircTag)
	if chunk == nil {
		return nil, fmt.Errorf("%w: the bank has no HIRC", ErrBadHIRC)
	}
	objs, err := ParseHIRC(chunk.Payload)
	if err != nil {
		return nil, err
	}
	for _, o := range objs {
		if o.ID == in.Attenuation.ID {
			return nil, fmt.Errorf("%w: attenuation %#x is already in the target", ErrIDCollision, o.ID)
		}
	}
	snd, err := FindObject(objs, in.Sound, TypeSound)
	if err != nil {
		return nil, err
	}
	if len(snd.Body) < soundSourceBytes {
		return nil, ErrBadSound
	}
	plugin := binary.LittleEndian.Uint32(snd.Body[soundPluginOff:])
	if binary.LittleEndian.Uint32(snd.Body[soundSourceOff:]) != in.Media || snd.Body[soundStreamOff] != 0 ||
		(plugin != PluginVorbis && plugin != PluginPCM) {
		return nil, fmt.Errorf("%w: Sound %#x is not the embedded media %#x", ErrBadSound, in.Sound, in.Media)
	}
	pos, err := Positioned(snd.Body, in.Attenuation.ID)
	if err != nil {
		return nil, err
	}
	soundBody, err := SetSoundSource(pos, PluginPCM, in.Media, uint32(len(veilWEM)))
	if err != nil {
		return nil, err
	}

	out := []Object{cloneObject(in.Attenuation)}
	for _, o := range objs {
		if o.ID == in.Sound && o.Type == TypeSound {
			out = append(out, Object{Type: TypeSound, ID: in.Sound, Body: soundBody})
			continue
		}
		out = append(out, cloneObject(o))
	}

	// Loc (his :249-253): the plays, then their event.
	locActions := make([]uint32, 0, in.Loc.Copies)
	for k := 0; k < in.Loc.Copies; k++ {
		action := in.Loc.FirstAction + uint32(k)
		locActions = append(locActions, action)
		out = append(out, PlayAction(action, in.Loc.Sound, in.Loc.SoundBank))
	}
	locEvent, err := Event(in.Loc.Event, locActions)
	if err != nil {
		return nil, err
	}
	out = append(out, locEvent)

	// Point (his :254-269): the point sound is the positioned Vorbis body with its own media, the plays, their event,
	// and the sound is listed among its parent's children.
	pointBody, err := SetSoundSource(pos, PluginPCM, in.Point.Media, uint32(len(pointWEM)))
	if err != nil {
		return nil, err
	}
	pointSound := Object{Type: TypeSound, ID: in.Point.Sound, Body: pointBody}
	pointActions := make([]uint32, 0, in.Point.Copies)
	for c := 0; c < in.Point.Copies; c++ {
		action := in.Point.FirstAction + uint32(c)
		pointActions = append(pointActions, action)
		out = append(out, PlayAction(action, in.Point.Sound, in.BankID))
	}
	pointEvent, err := Event(in.Point.Event, pointActions)
	if err != nil {
		return nil, err
	}
	out = append(out, pointEvent)
	parent := binary.LittleEndian.Uint32(pos[soundParentOff:soundParentEnd])
	out, err = AddChildren(out, parent, []Object{pointSound})
	if err != nil {
		return nil, err
	}

	// VO (his :270-282), only when there are callouts: each line is a copy of the template with its own media, a Play and
	// an Event; the copies are listed among the template's parent's children.
	if len(in.VO) > 0 {
		template, err := FindObject(out, in.VOTemplate, TypeSound)
		if err != nil {
			return nil, err
		}
		if len(template.Body) < soundParentEnd {
			return nil, ErrBadSound
		}
		var sounds []Object
		for _, line := range in.VO {
			wem := media[line.Media]
			body, err := SetSoundSource(template.Body, PluginPCM, line.Media, uint32(len(wem)))
			if err != nil {
				return nil, err
			}
			sounds = append(sounds, Object{Type: TypeSound, ID: line.Sound, Body: body})
			out = append(out, PlayAction(line.Action, line.Sound, in.BankID))
			lineEvent, err := Event(line.Event, []uint32{line.Action})
			if err != nil {
				return nil, err
			}
			out = append(out, lineEvent)
		}
		out, err = AddChildren(out, binary.LittleEndian.Uint32(template.Body[soundParentOff:soundParentEnd]), sounds)
		if err != nil {
			return nil, err
		}
	}

	// Collision (his ids are not stock): the copied attenuation matches stock, and every id the build creates is clear
	// of the stock banks and of each other. The replaced media is the stock slot, so it is not a created id; a created
	// media id must not be that slot either.
	if err := stock.CheckCopy(in.Attenuation); err != nil {
		return nil, err
	}
	created := []uint32{in.Loc.Event, in.Point.Sound, in.Point.Event}
	for k := 0; k < in.Loc.Copies; k++ {
		created = append(created, in.Loc.FirstAction+uint32(k))
	}
	for c := 0; c < in.Point.Copies; c++ {
		created = append(created, in.Point.FirstAction+uint32(c))
	}
	created = append(created, createdMedia...)
	for _, line := range in.VO {
		created = append(created, line.Sound, line.Action, line.Event)
	}
	for _, m := range createdMedia {
		if m == in.Media {
			return nil, fmt.Errorf("%w: created media %#x is the replaced media", ErrIDCollision, m)
		}
	}
	if err := stock.CheckCreated(created); err != nil {
		return nil, err
	}

	r.Bank.Chunk(hircTag).Payload = EncodeHIRC(out)
	return r.Bytes(), nil
}
