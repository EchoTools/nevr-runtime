package main

import (
	"bytes"
	"fmt"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

// The RULES page's row labels in a Combat private match (#2242, EchoCombat slice 7, PLAN-v5 G-RULES-1; he_is_the_cat's
// tools/fix_rules_terminal.py at 82ad7ba, read as text: patch_canvas, :102-115, with the canvas's comment and CANVAS_TYPE,
// RULES_CANVAS, TEXT_SLOT and TEXT_EDITS at :45-53; main stages the canvas at :144-152).
//
// The page's canvas (ui_display_mpl_mm_custom_rules1) is Arena's: rows MINUTES, SECONDS, BLUE SCORE, ORANGE SCORE. Its
// texts are fixed 0x21-byte slots, zero padded, that start at the text "MATCH RULES" (slot 0). In Combat the two time rows
// are the capture point's unlock time and its time to capture (the script half, fix_capture_points.py, is in already), so
// slot 1 MINUTES becomes POINT UNLOCK and slot 3 SECONDS becomes CAPTURE TIME. His tool also lists HIDDEN_ELEMENTS (:54),
// which is empty, and blanks two ":00" text pieces in the podium script (TIME_PIECES :58, blanked at :131-136): that is a script edit, not this step's.
//
// Declared field (Step.Writes, what the stage82 oracle compares for this step; readRulesTextSlots): the bytes of text
// slots 1 and 3 of canvas 59a9bd6e4525ecc4.b655c5c7eaa080d7, at +0x21 and +0x63 from the start of its "MATCH RULES"
// text, each 0x21 bytes; nothing else, and the record's length is unchanged. No other step writes this canvas: his
// fix_rules_pages.py, the other tool of the RULES pages, only reads its page-1 labels (labels_from_canvas, :221-225,
// read at :672-676) and writes no canvas text (:23). The field names Sets[0], the one set
// the oracle opens; which client set lists the canvas on an install is not measured here, and the step itself takes
// whichever set lists it.
//
// Base: stock. His tool checks the slot against the stock text or the new one (so a second run passes); this step takes the
// stock text only, and refuses a canvas that is already edited, as the equipment step does.
const (
	rulesCanvasType = 0x59a9bd6e4525ecc4 // the canvas's type, CANVAS_TYPE
	rulesCanvasName = 0xb655c5c7eaa080d7 // ui_display_mpl_mm_custom_rules1, RULES_CANVAS
	rulesTextSlot   = 0x21               // TEXT_SLOT
	rulesFirstText  = "MATCH RULES"      // slot 0, where his tool finds the slots
)

type rulesLabel struct {
	slot     int
	was, now string
}

// rulesLabels are TEXT_EDITS: slot 1 and slot 3.
var rulesLabels = []rulesLabel{{1, "MINUTES", "POINT UNLOCK"}, {3, "SECONDS", "CAPTURE TIME"}}

func init() {
	steps = append(steps, Step{Name: "RULES page labels the capture point's two times", Apply: rulesPageLabels,
		Writes: []Field{{Name: "text slots 1 and 3 of the RULES canvas, 0x21 bytes each from its MATCH RULES text", Set: Sets[0],
			Type: rulesCanvasType, Records: []uint64{rulesCanvasName}, Read: readRulesTextSlots}}})
}

// readRulesTextSlots is the declared field: the bytes of each slot in rulesLabels, in order, read from the slot the
// canvas's "MATCH RULES" text starts. A canvas with no such text, or one too short for a slot, is refused.
func readRulesTextSlots(b []byte) ([]byte, error) {
	base := bytes.Index(b, append([]byte(rulesFirstText), 0))
	if base < 0 {
		return nil, fmt.Errorf("no %q text slot", rulesFirstText)
	}
	var out []byte
	for _, l := range rulesLabels {
		at := base + l.slot*rulesTextSlot
		if at+rulesTextSlot > len(b) {
			return nil, fmt.Errorf("text slot %d at +%#x ends past the canvas's %d bytes", l.slot, at, len(b))
		}
		out = append(out, b[at:at+rulesTextSlot]...)
	}
	return out, nil
}

// rulesPageLabels reads the canvas from whichever client set lists it, and puts it back in that set.
func rulesPageLabels(p *Pack) error {
	var typ, name uint64 = rulesCanvasType, rulesCanvasName // not constants: the name overflows a constant int64 conversion
	key := radpack.Key{Type: int64(typ), Name: int64(name)}
	for _, set := range Sets {
		s, ok := p.sets[set]
		if !ok {
			continue
		}
		if _, changed := p.changes[set][key]; !changed {
			if _, found, err := s.Read(key.Type, key.Name); err != nil {
				return err
			} else if !found {
				continue
			}
		}
		b, err := p.Read(set, key.Type, key.Name)
		if err != nil {
			return err
		}
		out, err := relabelRulesCanvas(b)
		if err != nil {
			return fmt.Errorf("canvas %016x.%016x: %w", typ, name, err)
		}
		return p.Put(set, key.Type, key.Name, out)
	}
	return fmt.Errorf("no client set lists canvas %016x.%016x", typ, name)
}

// relabelRulesCanvas is the canvas with slots 1 and 3 rewritten, every other byte kept. Every slot is checked before
// any is written.
func relabelRulesCanvas(b []byte) ([]byte, error) {
	base := bytes.Index(b, append([]byte(rulesFirstText), 0))
	if base < 0 {
		return nil, fmt.Errorf("no %q text slot", rulesFirstText)
	}
	out := append([]byte(nil), b...)
	for _, l := range rulesLabels {
		at := base + l.slot*rulesTextSlot
		if at+rulesTextSlot > len(b) {
			return nil, fmt.Errorf("text slot %d at +%#x ends past the canvas's %d bytes", l.slot, at, len(b))
		}
		if len(l.now) >= rulesTextSlot {
			return nil, fmt.Errorf("text slot %d: %q does not fit %d bytes with its NUL", l.slot, l.now, rulesTextSlot)
		}
		if got := bytes.TrimRight(b[at:at+rulesTextSlot], "\x00"); string(got) != l.was {
			return nil, fmt.Errorf("text slot %d at +%#x is %q, stock is %q", l.slot, at, got, l.was)
		}
		copy(out[at:at+rulesTextSlot], append([]byte(l.now), make([]byte, rulesTextSlot-len(l.now))...))
	}
	return out, nil
}
