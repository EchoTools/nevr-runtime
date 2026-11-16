package wwisebank

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"slices"
	"strings"
)

// ErrIDCollision is returned when an id the build creates is already in the stock banks, or is listed twice.
var ErrIDCollision = errors.New("wwisebank: an id the build creates is already used by a stock bank")

// ErrCopyDiffers is returned when a copied object is in no stock record, or a stock object of its id differs from it.
var ErrCopyDiffers = errors.New("wwisebank: a copied object differs from the stock object of its id")

const (
	whatMedia = "media"
	hircTag   = "HIRC"
	// soundSourceEnd is the end of a Sound's sourceID (PLAN-v5 S-SND 1.3): the source is read from +5 to +9.
	soundSourceEnd = soundSourceOff + resU32Bytes
)

// IDUse is one place a stock id is used: the record's name and what the id names there ("object type <n>" or "media").
type IDUse struct {
	Record string
	What   string
}

// stockObject is a stock HIRC object with the record it came from.
type stockObject struct {
	record string
	obj    Object
}

// StockIndex is every object and media id of the stock bank records (PLAN-v5 S-SND 1.10): the uses of each id, and the
// stock objects of each id.
type StockIndex struct {
	uses    map[uint32][]IDUse
	objects map[uint32][]stockObject
}

// IndexStock reads the stock resource records, keyed by name, into an index. An id is used by a HIRC object (its
// type), by a DIDX entry (media) or as a Sound's sourceID (media). It refuses (ErrBadResource, naming the record) a
// record that does not parse. The index keeps copies; records is not changed.
func IndexStock(records map[string][]byte) (*StockIndex, error) {
	names := make([]string, 0, len(records))
	for name := range records {
		names = append(names, name)
	}
	slices.Sort(names)

	idx := &StockIndex{uses: make(map[uint32][]IDUse), objects: make(map[uint32][]stockObject)}
	for _, name := range names {
		r, err := ParseResource(records[name])
		if err != nil {
			return nil, fmt.Errorf("%w: record %s: %v", ErrBadResource, name, err)
		}
		if didx := r.Bank.Chunk(didxTag); didx != nil {
			if len(didx.Payload)%didxEntryBytes != 0 {
				return nil, fmt.Errorf("%w: record %s: DIDX is not whole entries", ErrBadResource, name)
			}
			for p := didx.Payload; len(p) >= didxEntryBytes; p = p[didxEntryBytes:] {
				idx.add(binary.LittleEndian.Uint32(p), IDUse{Record: name, What: whatMedia})
			}
		}
		hirc := r.Bank.Chunk(hircTag)
		if hirc == nil {
			continue
		}
		objs, err := ParseHIRC(hirc.Payload)
		if err != nil {
			return nil, fmt.Errorf("%w: record %s: %v", ErrBadResource, name, err)
		}
		for _, o := range objs {
			idx.add(o.ID, IDUse{Record: name, What: fmt.Sprintf("object type %d", o.Type)})
			idx.objects[o.ID] = append(idx.objects[o.ID], stockObject{record: name, obj: o})
			if o.Type == TypeSound {
				if len(o.Body) < soundSourceEnd {
					return nil, fmt.Errorf("%w: record %s: Sound %#x is too short for its source", ErrBadResource, name, o.ID)
				}
				idx.add(binary.LittleEndian.Uint32(o.Body[soundSourceOff:]), IDUse{Record: name, What: whatMedia})
			}
		}
	}
	return idx, nil
}

func (s *StockIndex) add(id uint32, use IDUse) {
	s.uses[id] = append(s.uses[id], use)
}

// CheckCreated refuses (ErrIDCollision) an id the stock banks use, naming every use, and an id listed twice. The ids the
// build creates are checked as a set, so an id repeated among them is refused too.
func (s *StockIndex) CheckCreated(ids []uint32) error {
	seen := make(map[uint32]bool, len(ids))
	for _, id := range ids {
		if seen[id] {
			return fmt.Errorf("%w: id %#x (%d) is listed twice", ErrIDCollision, id, id)
		}
		seen[id] = true
		if uses := s.uses[id]; len(uses) > 0 {
			return fmt.Errorf("%w: id %#x (%d) is in stock: %s", ErrIDCollision, id, id, describeUses(uses))
		}
	}
	return nil
}

// CheckCopy refuses (ErrCopyDiffers) an object copied from stock unless every stock object with its id has the same type
// and body. An id no stock record holds is refused too. o is not changed.
func (s *StockIndex) CheckCopy(o Object) error {
	objs := s.objects[o.ID]
	if len(objs) == 0 {
		return fmt.Errorf("%w: id %#x (%d) is in no stock record", ErrCopyDiffers, o.ID, o.ID)
	}
	for _, so := range objs {
		if so.obj.Type != o.Type || !bytes.Equal(so.obj.Body, o.Body) {
			return fmt.Errorf("%w: id %#x in record %s is type %d with a different body (the copy is type %d)",
				ErrCopyDiffers, o.ID, so.record, so.obj.Type, o.Type)
		}
	}
	return nil
}

func describeUses(uses []IDUse) string {
	parts := make([]string, 0, len(uses))
	for _, u := range uses {
		parts = append(parts, fmt.Sprintf("record %s %s", u.Record, u.What))
	}
	return strings.Join(parts, "; ")
}
