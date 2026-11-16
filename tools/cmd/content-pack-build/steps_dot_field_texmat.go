package main

import (
	"encoding/binary"
	"fmt"
	"math"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

// The DOT FIELD's red bubble, the texture-streaming and material records (#2242, PLAN-v5 G-DOT-FIELD-2, inventory row
// 9). he_is_the_cat's tool tools/fix_dot_field.py (his echocombat tree at 82ad7ba) gives the DOT FIELD a visible field:
// a red copy of the arc mine's arming bubble, model component arcBubble of arcActor, copied onto the DOT actor
// (dotMagazineActor) as fieldComponent. steps_dot_field.go builds the model and transform records; this file builds the
// texture-streaming records and the two red materials. His script-image edits (fix_dot_field.py:189-233) are H-DOTF,
// not built here, and no script is touched.
//
// The red model's own files are built by no step: KNOWN-WRONG #2242. His tool copies the eight files of the arc's model
// (arcModel, fix_dot_field.py:75-76, :102-121) to redModel and, in the model definition a388ea69e5108f4c, replaces the
// two arc material ids with the red copies made here (:107-111). Neither G-DOT-FIELD-1 nor G-DOT-FIELD-2 builds them, a
// gap of the plan. Until a step does, the model and texture-streaming records name a model asset the pack does not
// serve, and nothing references the two red materials, so the red bubble does not show.
//
//  1. Materials (fix_dot_field.py:78-87, :113-120). The bubble model names two materials, 4a9b00b6ec40fbfa and
//     7ac7845083d5e9a6. Each is copied under a new name, CSymbol64("mat_dot_field_bubble_a") and "..._b", and its light
//     blue colours are replaced by red ones: material a at +0x1a8 and +0x1b8, material b at +0x1a8 (four f32 each,
//     r g b a). His tool refuses a colour that is not light blue (b == 1, a == 1, g > r, :117); so does this step.
//  2. Texture streaming (fix_dot_field.py:56, :169-186, :257). In each of the five combat levels' CTextureStreamingCR
//     table (96832b652460ecde) the record keyed (arcBubble at +0, arcActor at +8) is copied to the end of the table with
//     +0 set to fieldComponent, +8 to dotMagazineActor and the model id at +0x20 from arcModel to redModel. His tool
//     leaves a table that already has the record as it is; this step, like the model and transform steps, refuses it,
//     so a run twice cannot copy twice.
//
// Declared fields (Step.Writes, what the stage82 oracle compares): in each level of droneLevels the texture-streaming
// table's FIELD record of the DOT actor, whole (readFieldTexStream: the arc bubble's record with +0, +8 and +0x20
// changed); in each red material the four f32 at each colour offset above (readRedColours). Nothing else of a table or
// material differs from its source; a table's header counts are not declared, as in steps_dot_field.go.
//
// Base: both are stock records. The gun patch he starts from changes the three drone rules of steps_drone.go and the
// equipment lists, and nothing of a texture-streaming table or a material, so his tool does not rely on a gun-patch
// difference here (PLAN-v5 S-BASE: not measured against his copy on this machine; stock_steps_test.go runs the step
// over the shipped data when NEVR_STOCK_DATA is set). The materials are read from Sets[0] only, not measured against
// the shipped data: a material that set does not list stops the step with the resource named.
const (
	texStreamTableType uint64 = 0x96832b652460ecde // CTextureStreamingCRWin10, fix_dot_field.py:56
	materialType       uint64 = 0xe3e0266f1911dafa // fix_dot_field.py:78
	texModelOff               = 0x20               // the record's model id (model_at=(0x20,), fix_dot_field.py:257)
	colourFloats              = 4                  // r g b a
	colourLen                 = colourFloats * floatLen
)

// dotFieldMaterial is an arc material, its red copy and the colours that copy takes (fix_dot_field.py:82-87).
type dotFieldMaterial struct {
	name     string // the red copy's name, for the declared field
	arc, red uint64
	colours  []dotFieldColour
}

type dotFieldColour struct {
	off  int
	rgba [colourFloats]float32
}

var (
	dotFieldRed      = [colourFloats]float32{1.0, 0.06, 0.04, 1.0} // fix_dot_field.py:84, :86
	dotFieldRedLight = [colourFloats]float32{1.0, 0.55, 0.50, 1.0} // fix_dot_field.py:85
)

const (
	materialColourOff      = 0x1a8 // fix_dot_field.py:84
	materialLightColourOff = 0x1b8 // fix_dot_field.py:85
)

var dotFieldMaterials = []dotFieldMaterial{
	{"mat_dot_field_bubble_a", 0x4a9b00b6ec40fbfa, 0xdb51d60d5644104f,
		[]dotFieldColour{{materialColourOff, dotFieldRed}, {materialLightColourOff, dotFieldRedLight}}},
	{"mat_dot_field_bubble_b", 0x7ac7845083d5e9a6, 0xdb51d60d5644104c,
		[]dotFieldColour{{materialColourOff, dotFieldRed}}},
}

func init() {
	writes := []Field{}
	for _, m := range dotFieldMaterials {
		writes = append(writes, Field{Name: m.name + ": the four f32 at each red colour offset", Set: Sets[0],
			Type: materialType, Records: []uint64{m.red}, Read: readRedColours(m)})
	}
	steps = append(steps,
		Step{Name: "DOT FIELD red bubble: materials", Apply: dotFieldMaterialCopies, Writes: writes},
		Step{Name: "DOT FIELD red bubble: texture streaming records", Apply: forEachLevel(texStreamTableType, dotFieldTexStreamRow),
			Writes: []Field{{Name: "FIELD texture streaming record of the DOT actor, whole", Set: Sets[0],
				Type: texStreamTableType, Records: droneLevels, Read: readFieldTexStream}}},
	)
}

func dotFieldMaterialCopies(p *Pack) error {
	var typ = materialType // not a constant: the id overflows a constant int64 conversion
	for _, m := range dotFieldMaterials {
		src, err := p.Read(Sets[0], int64(typ), int64(m.arc))
		if err != nil {
			return err
		}
		out, err := redMaterial(src, m)
		if err != nil {
			return fmt.Errorf("material %016x: %w", m.arc, err)
		}
		if err := p.Put(Sets[0], int64(typ), int64(m.red), out); err != nil {
			return err
		}
	}
	return nil
}

// colourAt is the four f32 at off, or an error when the material ends before them.
func colourAt(b []byte, off int) ([colourFloats]float32, error) {
	var cur [colourFloats]float32
	if len(b) < off+colourLen {
		return cur, fmt.Errorf("%d bytes end before the colour at +%#x", len(b), off)
	}
	for i := range cur {
		cur[i] = math.Float32frombits(binary.LittleEndian.Uint32(b[off+floatLen*i:]))
	}
	return cur, nil
}

// redMaterial is the arc material with each colour of m replaced, after checking it is the light blue the arc has
// (fix_dot_field.py:116-119).
func redMaterial(src []byte, m dotFieldMaterial) ([]byte, error) {
	out := append([]byte(nil), src...)
	for _, c := range m.colours {
		cur, err := colourAt(out, c.off)
		if err != nil {
			return nil, err
		}
		if !(cur[2] == 1.0 && cur[3] == 1.0 && cur[1] > cur[0]) {
			return nil, fmt.Errorf("no light blue colour at +%#x: r g b a are %v, want b 1, a 1 and g above r", c.off, cur)
		}
		for i, v := range c.rgba {
			binary.LittleEndian.PutUint32(out[c.off+floatLen*i:], math.Float32bits(v))
		}
	}
	return out, nil
}

// readRedColours is a material step's declared field: the bytes of each colour of m, in order.
func readRedColours(m dotFieldMaterial) func([]byte) ([]byte, error) {
	return func(b []byte) ([]byte, error) {
		var out []byte
		for _, c := range m.colours {
			if _, err := colourAt(b, c.off); err != nil {
				return nil, err
			}
			out = append(out, b[c.off:c.off+colourLen]...)
		}
		return out, nil
	}
}

// readFieldTexStream is the texture-streaming step's declared field: the one FIELD record of the DOT actor, whole.
func readFieldTexStream(b []byte) ([]byte, error) { return readFieldRow(b, "texture streaming") }

// dotFieldTexStreamRow appends the DOT's copy of the arc bubble's texture-streaming record (fix_dot_field.py:169-186).
func dotFieldTexStreamRow(t *radpack.RowTable) error {
	if t.Size < texModelOff+u64Len {
		return fmt.Errorf("texture streaming rows of %d bytes end before the model id at +%#x", t.Size, texModelOff)
	}
	if len(t.Tail) != 0 {
		return fmt.Errorf("texture streaming table has %d bytes after its rows; his tool reads a table of plain records", len(t.Tail))
	}
	var arc []int
	for i, r := range t.Rows {
		switch id, actor, _ := modelKey(r); {
		case id == fieldComponent && actor == dotMagazineActor:
			return fmt.Errorf("the table already has the DOT's FIELD record (row %d)", i)
		case id == arcBubble && actor == arcActor:
			arc = append(arc, i)
		}
	}
	if len(arc) != 1 {
		return fmt.Errorf("%d arc bubble records (component %016x, actor %016x), want 1", len(arc), arcBubble, arcActor)
	}
	c := append([]byte(nil), t.Rows[arc[0]]...)
	for _, s := range []struct {
		off      int
		want, to uint64
		field    string
	}{
		{idOff, arcBubble, fieldComponent, "component"},
		{keyOff, arcActor, dotMagazineActor, "actor"},
		{texModelOff, arcModel, redModel, "model id"},
	} {
		if err := swapU64(c, s.off, s.want, s.to, "arc bubble texture streaming record", s.field); err != nil {
			return err
		}
	}
	t.Rows = append(t.Rows, c)
	return nil
}
