package wwisebank

import (
	"encoding/binary"
	"errors"
	"fmt"
	"sort"
)

var (
	// ErrNotActorMixer is returned when the parent is not an actor-mixer: only type 7 ends with a children list (PLAN-v5
	// S-SND 1.7).
	ErrNotActorMixer = errors.New("wwisebank: the parent is not an actor-mixer")
	// ErrNoChildList is returned when no children list ends the parent's body, or the list is not ascending.
	ErrNoChildList = errors.New("wwisebank: no children list at the end of the parent")
	// ErrChildListed is returned when a child is already in the list or in the bank, or two children share an id.
	ErrChildListed = errors.New("wwisebank: a child is already in the bank or the list")
	// ErrNoKids is returned when there is nothing to add.
	ErrNoKids = errors.New("wwisebank: no children to add")
)

const (
	// childCountBytes is the u32 count that opens a children list; childIDBytes is one u32 child id.
	childCountBytes = 4
	childIDBytes    = 4
)

// AddChildren returns a copy of objs with kids inserted, in order, just before parent, and with the parent's children
// list made the ascending union of its list and the kids' ids (PLAN-v5 S-SND section 3; his tool's add_children,
// veil_sound.py:149-174). The parent is the one object of its id, and it must be an actor-mixer. objs and kids are not
// changed.
func AddChildren(objs []Object, parent uint32, kids []Object) ([]Object, error) {
	if len(kids) == 0 {
		return nil, ErrNoKids
	}
	at, err := soleObjectIndex(objs, parent)
	if err != nil {
		return nil, err
	}
	if objs[at].Type != TypeActorMixer {
		return nil, fmt.Errorf("%w: id %#x type %d", ErrNotActorMixer, parent, objs[at].Type)
	}
	listAt, listed, err := childList(objs[at].Body)
	if err != nil {
		return nil, err
	}

	inBank := make(map[uint32]bool, len(objs))
	for _, o := range objs {
		inBank[o.ID] = true
	}
	member := make(map[uint32]bool, len(listed)+len(kids))
	for _, id := range listed {
		member[id] = true
	}
	for _, k := range kids {
		if member[k.ID] || inBank[k.ID] {
			return nil, fmt.Errorf("%w: id %#x", ErrChildListed, k.ID)
		}
		member[k.ID] = true
	}

	union := make([]uint32, 0, len(member))
	for id := range member {
		union = append(union, id)
	}
	sort.Slice(union, func(i, j int) bool { return union[i] < union[j] })

	newBody := append([]byte(nil), objs[at].Body[:listAt]...)
	newBody = binary.LittleEndian.AppendUint32(newBody, uint32(len(union)))
	for _, id := range union {
		newBody = binary.LittleEndian.AppendUint32(newBody, id)
	}

	out := make([]Object, 0, len(objs)+len(kids))
	for i, o := range objs {
		if i == at {
			for _, k := range kids {
				out = append(out, cloneObject(k))
			}
			out = append(out, Object{Type: o.Type, ID: o.ID, Body: newBody})
			continue
		}
		out = append(out, cloneObject(o))
	}
	return out, nil
}

// soleObjectIndex returns the index of the one object with the id, whatever its type. It refuses with ErrNoObject when
// none has it and with ErrAmbiguous when two do.
func soleObjectIndex(objs []Object, id uint32) (int, error) {
	at, matches := -1, 0
	for i, o := range objs {
		if o.ID == id {
			at = i
			matches++
		}
	}
	switch {
	case matches == 0:
		return -1, fmt.Errorf("%w: id %#x", ErrNoObject, id)
	case matches > 1:
		return -1, fmt.Errorf("%w: id %#x", ErrAmbiguous, id)
	}
	return at, nil
}

// childList finds the children list at the end of an actor-mixer body. It takes the last offset, scanning down, whose u32
// count is non-zero and whose list ends exactly at the end of the body; the ids must be strictly ascending. It returns
// the offset of the count and the ids. Scanning from the end, the largest offset that fits is the list (PLAN-v5 S-SND 1.7).
func childList(body []byte) (int, []uint32, error) {
	for at := len(body) - childCountBytes; at >= 0; at -= childCountBytes {
		count := binary.LittleEndian.Uint32(body[at:])
		if count == 0 || uint64(at)+childCountBytes+childIDBytes*uint64(count) != uint64(len(body)) {
			continue
		}
		ids := make([]uint32, count)
		for k := range ids {
			ids[k] = binary.LittleEndian.Uint32(body[at+childCountBytes+childIDBytes*k:])
		}
		for k := 1; k < len(ids); k++ {
			if ids[k] <= ids[k-1] {
				return 0, nil, fmt.Errorf("%w: ids not ascending at %d", ErrNoChildList, k)
			}
		}
		return at, ids, nil
	}
	return 0, nil, ErrNoChildList
}

// cloneObject copies an object, body included.
func cloneObject(o Object) Object {
	return Object{Type: o.Type, ID: o.ID, Body: append([]byte(nil), o.Body...)}
}
