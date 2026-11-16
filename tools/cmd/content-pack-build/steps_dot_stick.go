package main

import (
	"encoding/binary"
	"fmt"
)

// The DOT FIELD's stick-on-contact bit (PLAN-v5 inventory row 8, G-DOT-STICK). Origin: he_is_the_cat's
// tools/fix_dot_stick.py at 82ad7ba (:29-33 constants, :36-52 patch). In the CR15NetMagazineCR table of each combat
// level (5b723f040a3778ee, CSymbol64("CR15NetMagazineCRWin10")), the record of the DOT magazine actor 0c00c9ac34bd2832
// has bit 0x10 of the u32 at +0x18 set; it makes the game register a stick-on-contact listener that welds the magazine
// to the first thing it touches. The step clears that bit and nothing else, so the DOT is handled like every other
// ordnance (his docstring, :3-20). The rigid-attach record is left as it is (his :17-18); the parented constraint is
// G-DOT-CONS and the dampener G-DOT-DAMP, not this step.
//
// Declared field (Step.Writes, what the stage82 oracle compares for this step; readDotStickBit): resource type 5b723f040a3778ee of each level in droneLevels,
// the one 40-byte record whose u64 at +8 is 0c00c9ac34bd2832, bit 0x10 of its u32 at +0x18. Every other byte of the
// table is kept.
//
// Base: his tool reads the table as his build holds it and says only that the DOT is "the only ordnance with the bit
// set" (:5-6); it relies on no gun-patch difference this repository knows of (the gun patch's files this build
// reproduces are the bullet, particle and script tables, stock_steps_test.go), but whether the stock table has the bit
// set is measured by the step itself: it refuses a table where the bit is not set, naming the record and the value.
const (
	magazineTableType = 0x5b723f040a3778ee // CR15NetMagazineCRWin10 on disk
	dotMagazineActor  = 0x0c00c9ac34bd2832 // the DOT magazine's actor, at +8 of its record (fix_dot_stick.py:30)
	stickBit          = 0x10               // +0x18 bit 0x10, fix_dot_stick.py:31
	magazineHead      = 0x38               // fix_dot_stick.py:32
	magazineRow       = 40                 // fix_dot_stick.py:33
	magazineFlagsOff  = 0x18
	magazineBytesOff  = 0x08 // the rows' byte size
	magazineCountOff  = 0x28 // the row count
)

func init() {
	steps = append(steps,
		Step{Name: "DOT magazine does not stick on contact", Apply: forEachLevelBytes(magazineTableType, dotStick),
			Writes: []Field{{Name: "magazine record of the DOT actor, bit 0x10 of the u32 at +0x18", Set: Sets[0],
				Type: magazineTableType, Records: droneLevels, Read: readDotStickBit}}},
	)
}

// forEachLevelBytes is forEachLevel for a rule over the resource's raw bytes, over the same five levels. The step
// does not go through radpack.ParseRowTable: that reader also requires the u64 at +0x30 to equal the capacity at
// +0x28 and keeps a tail, and its Bytes() rewrites +0x08, +0x28 and +0x30. Cat's framing (fix_dot_stick.py:37-38)
// reads only +0x08 and +0x28 and refuses a tail, and +0x30 of this table is not measured here, so the step keeps his
// check and writes only its declared field.
func forEachLevelBytes(typ uint64, rule func([]byte) ([]byte, error)) func(*Pack) error {
	return func(p *Pack) error {
		for _, lvl := range droneLevels {
			b, err := p.Read(Sets[0], int64(typ), int64(lvl))
			if err != nil {
				return err
			}
			out, err := rule(b)
			if err != nil {
				return fmt.Errorf("level %016x: %w", lvl, err)
			}
			if err := p.Put(Sets[0], int64(typ), int64(lvl), out); err != nil {
				return err
			}
		}
		return nil
	}
}

// dotRecord checks the table's framing and returns the offset of the one record of the DOT magazine.
func dotRecord(blob []byte) (int, error) {
	le := binary.LittleEndian
	if len(blob) < magazineHead {
		return 0, fmt.Errorf("CR15NetMagazineCR of %d bytes is shorter than its %#x header", len(blob), magazineHead)
	}
	size, count := le.Uint64(blob[magazineBytesOff:]), le.Uint64(blob[magazineCountOff:])
	// Expected framing (fix_dot_stick.py:37-38): the rows fill the table after the header, no tail.
	if count == 0 || count > uint64(len(blob)) || size != count*magazineRow || uint64(len(blob)) != magazineHead+size {
		return 0, fmt.Errorf("unexpected CR15NetMagazineCR framing: %d bytes, byte size %d, count %d, want rows of %d filling the table", len(blob), size, count, magazineRow)
	}
	at, hits := 0, 0
	for i := uint64(0); i < count; i++ {
		rec := magazineHead + int(i)*magazineRow
		if le.Uint64(blob[rec+keyOff:]) == dotMagazineActor {
			at = rec
			hits++
		}
	}
	if hits != 1 {
		return 0, fmt.Errorf("expected one record of the DOT magazine %016x, found %d", uint64(dotMagazineActor), hits)
	}
	return at, nil
}

// readDotStickBit is the step's declared field: bit 0x10 of the u32 at +0x18 of the DOT magazine's record, as one byte
// (0x10 set, 0 clear; the bit sits in the u32's low byte, little-endian). The other flag bits are not the step's claim.
func readDotStickBit(b []byte) ([]byte, error) {
	rec, err := dotRecord(b)
	if err != nil {
		return nil, err
	}
	return []byte{b[rec+magazineFlagsOff] & stickBit}, nil
}

// dotStick returns the table with the stick bit of the DOT magazine's record cleared.
func dotStick(blob []byte) ([]byte, error) {
	rec, err := dotRecord(blob)
	if err != nil {
		return nil, err
	}
	le := binary.LittleEndian
	flags := le.Uint32(blob[rec+magazineFlagsOff:])
	if flags&stickBit == 0 {
		return nil, fmt.Errorf("record at %#x of actor %016x: flags at +%#x are %#x, want bit %#x set", rec, uint64(dotMagazineActor), magazineFlagsOff, flags, stickBit)
	}
	out := append([]byte(nil), blob...)
	le.PutUint32(out[rec+magazineFlagsOff:], flags&^stickBit)
	return out, nil
}
