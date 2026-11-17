package main

import (
	"encoding/binary"
	"errors"
	"fmt"

	"github.com/EchoTools/nevr-runtime/tools/internal/bc7"
	"github.com/EchoTools/nevr-runtime/tools/internal/gearicons"
	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

// The gear-icon atlases (#2242, PLAN-v7 12.3 G-ICONS-1). he_is_the_cat's tools/gear_icons.py (at 82ad7ba) makes, for each of
// three stock atlases, a new texture twice as wide: each block row is the stock atlas's block row, then a row of the right
// half, which is w x h RGBA with RGB 255 and the gear items' icon alpha in their cells (gearicons.Cells). The descriptor is the
// stock one with nine fields set (descriptor :278-286). The step writes the three new textures' two records each (the DDS
// file, CGTextureResourceWin10GPU, and the 256-byte descriptor, CGTextureResourceWin10) into Sets[0], named
// CSymbol64(atlas.Texture). It changes no stock record.
//
// The icons' art is an outside input (ADR 0035 rule 4): -gear-icons-dir, read by name and checked against sha-256 pins
// (gearicons.Pins). The step REFUSES when the flag is absent, naming it: nothing of Cat's ships as a subset, and there is no
// switch that lets a build go without. The right half's bytes are not Cat's (gearicons: box resample, this repository's
// encoder), so the step declares only the DDS header with the stock half and the descriptor (Step.Writes).
//
// Refused, writing nothing: no -gear-icons-dir; an art file missing, with another hash or not 8-bit RGBA (gearicons.ReadArt);
// a stock atlas record that is not the one measured (checkStockAtlas: DDS magic, DX10, dxgi 99, dimension 3, array 1, one mip,
// the atlas's width and height, the linear size, the file length, the 256-byte descriptor and its nine fields); a stock or
// earlier-step record of a new texture's name.
const (
	gearIconsStepName = "gear icons: three atlases get a cell for each new gear item"

	ddsHeightOff   = 12  // DDS_HEADER dwHeight
	ddsWidthOff    = 16  // DDS_HEADER dwWidth
	ddsLinearOff   = 20  // DDS_HEADER dwPitchOrLinearSize
	ddsMipsOff     = 28  // DDS_HEADER dwMipMapCount
	ddsDimOff      = 132 // DDS_HEADER_DXT10 resourceDimension
	ddsArrayOff    = 140 // DDS_HEADER_DXT10 arraySize
	dxgiBC7SRGB    = 99  // DXGI_FORMAT_BC7_UNORM_SRGB, the three atlases' format
	dimTexture2D   = 3   // D3D10_RESOURCE_DIMENSION_TEXTURE2D
	atlasMips      = 1   // every stock atlas has one mip (measured), so the new texture has one
	arrayOne       = 1   // DDS_HEADER_DXT10 arraySize of a single texture
	streamingOff   = 1   // gear_icons.py:282, the descriptor's streamingdisabled: the stock records already hold 1
	widthFactor    = 2   // the new texture is twice the stock's width: the right half is a second block column (gear_icons.py:251-254)
	bc7BlockBytes  = bc7.BlockSize
	blockDim       = bc7.BlockDim
	descStreamOff  = 0xC0 // gear_icons.py:177 D_STREAMINGDISABLED
	descMaxWOff    = 0xC4 // D_MAXW
	descMaxHOff    = 0xC8 // D_MAXH
	descMaxMipsOff = 0xCC // D_MAXMIPS
	descWOff       = 0xE8 // D_W
	descHOff       = 0xEC // D_H
	descMipsOff    = 0xF0 // D_MIPS
	descResMemOff  = 0xF4 // D_RESMEM
	descPitchOff   = 0xF8 // D_PITCH
)

func init() {
	steps = append(steps, Step{Name: gearIconsStepName, Apply: gearIconsApply(gearicons.Pins, gearicons.Atlases[:]),
		Writes: gearIconsWrites(gearicons.Atlases[:])})
}

// gearAtlasName is the new texture's name: CSymbol64 of the atlas's Texture.
func gearAtlasName(a gearicons.Atlas) uint64 { return csymbol64(a.Texture) }

// recordName is a record's type and name as the messages write them, the way the pack keys them.
func recordName(typ, name uint64) string { return fmt.Sprintf("%016x.%016x", typ, name) }

// gearIconsWrites are the declared fields: the DDS header with the stock half, and the descriptor, of each new texture.
func gearIconsWrites(atlases []gearicons.Atlas) []Field {
	names := make([]uint64, len(atlases))
	for i, a := range atlases {
		names[i] = gearAtlasName(a)
	}
	return []Field{
		{Name: "gear icon textures: DDS header and the stock half", Set: Sets[0], Type: sparksGPUType, Records: names, Read: readGearIconStockHalf},
		{Name: "gear icon textures: descriptor", Set: Sets[0], Type: sparksDescType, Records: names,
			Read: func(b []byte) ([]byte, error) { return append([]byte(nil), b...), nil }},
	}
}

func u32(b []byte, off int) int { return int(binary.LittleEndian.Uint32(b[off:])) }

func setU32(b []byte, off, v int) { binary.LittleEndian.PutUint32(b[off:], uint32(v)) }

// ddsLen is the length of a one-mip BC7 DDS file of w x h: the header and the blocks.
func ddsLen(w, h int) int { return ddsHeaderSize + (w/blockDim)*(h/blockDim)*bc7BlockBytes }

// checkStockAtlas refuses, naming the record and the field with its expected and actual value, a stock atlas that is not the one
// measured: the DDS format, the one-mip shape, the sizes, and the nine descriptor fields of the three stock atlases.
func checkStockAtlas(dds, desc []byte, a gearicons.Atlas) error {
	if len(dds) < ddsHeaderSize {
		return fmt.Errorf("atlas %016x: the DDS record has %d bytes, the header alone is %d", a.Stock, len(dds), ddsHeaderSize)
	}
	if string(dds[:len(ddsMagic)]) != ddsMagic {
		return fmt.Errorf("atlas %016x: the DDS record does not start with %q", a.Stock, ddsMagic)
	}
	checks := []struct {
		field     string
		got, want int
	}{
		{"DX10 fourCC @84", u32(dds, ddsFourCCOff), ddsDX10FourCC},
		{"dxgiFormat @128", u32(dds, ddsDXGIOff), dxgiBC7SRGB},
		{"resourceDimension @132", u32(dds, ddsDimOff), dimTexture2D},
		{"arraySize @140", u32(dds, ddsArrayOff), arrayOne},
		{"height @12", u32(dds, ddsHeightOff), a.H},
		{"width @16", u32(dds, ddsWidthOff), a.W},
		{"mip count @28", u32(dds, ddsMipsOff), atlasMips},
		{"linear size @20", u32(dds, ddsLinearOff), (a.W / blockDim) * (a.H / blockDim) * bc7BlockBytes},
		{"record length", len(dds), ddsLen(a.W, a.H)},
	}
	for _, c := range checks {
		if c.got != c.want {
			return fmt.Errorf("atlas %016x: DDS %s is %d, expected %d", a.Stock, c.field, c.got, c.want)
		}
	}
	if len(desc) != sparksDescSize {
		return fmt.Errorf("atlas %016x: the descriptor has %d bytes, expected %d", a.Stock, len(desc), sparksDescSize)
	}
	dchecks := []struct {
		field     string
		off, want int
	}{
		{"streamingdisabled", descStreamOff, streamingOff}, {"max width", descMaxWOff, a.W}, {"max height", descMaxHOff, a.H},
		{"max mips", descMaxMipsOff, atlasMips}, {"width", descWOff, a.W}, {"height", descHOff, a.H},
		{"mips", descMipsOff, atlasMips}, {"resident memory", descResMemOff, len(dds)}, {"pitch", descPitchOff, len(dds) - ddsHeaderSize},
	}
	for _, c := range dchecks {
		if got := u32(desc, c.off); got != c.want {
			return fmt.Errorf("atlas %016x: descriptor %s @%#x is %d, expected %d", a.Stock, c.field, c.off, got, c.want)
		}
	}
	return nil
}

// gearIconDDS is the new texture's DDS file: the stock header with the width doubled and the linear size set, then, for each
// block row, the stock atlas's row and the right half's row (bc7.Encode of right, w x h RGBA). w and h are the stock atlas's.
func gearIconDDS(stock, right []byte, w, h int) ([]byte, error) {
	if len(stock) != ddsLen(w, h) {
		return nil, fmt.Errorf("stock atlas has %d bytes, a %dx%d one-mip BC7 DDS has %d", len(stock), w, h, ddsLen(w, h))
	}
	enc, err := bc7.Encode(right, w, h)
	if err != nil {
		return nil, fmt.Errorf("right half: %w", err)
	}
	row := (w / blockDim) * bc7BlockBytes // one block row of the stock atlas, and of the right half
	out := make([]byte, 0, ddsLen(widthFactor*w, h))
	out = append(out, stock[:ddsHeaderSize]...)
	setU32(out, ddsWidthOff, widthFactor*w)
	setU32(out, ddsLinearOff, ((widthFactor*w)/blockDim)*(h/blockDim)*bc7BlockBytes)
	for by := 0; by < h/blockDim; by++ {
		out = append(out, stock[ddsHeaderSize+by*row:ddsHeaderSize+(by+1)*row]...)
		out = append(out, enc[by*row:(by+1)*row]...)
	}
	return out, nil
}

// gearIconDescriptor is the stock descriptor with the nine fields set for the new texture (gear_icons.py:278-286); w is the new
// width, ddsBytes the new DDS file's length.
func gearIconDescriptor(stock []byte, ddsBytes, w, h int) ([]byte, error) {
	if len(stock) != sparksDescSize {
		return nil, fmt.Errorf("stock descriptor has %d bytes, expected %d", len(stock), sparksDescSize)
	}
	d := append([]byte(nil), stock...)
	for off, v := range map[int]int{descStreamOff: streamingOff, descMaxWOff: w, descMaxHOff: h, descMaxMipsOff: atlasMips, descWOff: w, descHOff: h,
		descMipsOff: atlasMips, descResMemOff: ddsBytes, descPitchOff: ddsBytes - ddsHeaderSize} {
		setU32(d, off, v)
	}
	return d, nil
}

// readGearIconStockHalf is the reader of a new texture's DDS record: its header and, of each block row, the left half (the stock
// atlas's blocks), so the right half, which is not Cat's bytes, is not compared. It refuses a record that is shorter than its
// header or whose length is not the header's size.
func readGearIconStockHalf(record []byte) ([]byte, error) {
	if len(record) < ddsHeaderSize {
		return nil, errors.New("gear icon DDS record too short for its header")
	}
	w, h := u32(record, ddsWidthOff), u32(record, ddsHeightOff)
	if w%(widthFactor*blockDim) != 0 || h%blockDim != 0 || len(record) != ddsLen(w, h) {
		return nil, fmt.Errorf("gear icon DDS record too short or long: %d bytes for a %dx%d one-mip BC7 file (%d)", len(record), w, h, ddsLen(w, h))
	}
	row := (w / blockDim) * bc7BlockBytes
	half := row / widthFactor // the stock atlas's block row: the left half of the new one's
	out := append([]byte(nil), record[:ddsHeaderSize]...)
	for by := 0; by < h/blockDim; by++ {
		out = append(out, record[ddsHeaderSize+by*row:ddsHeaderSize+by*row+half]...)
	}
	return out, nil
}

// gearIconsBuilt says whether all six new records are in the pack's changes (G-ICONS-2 checks it before it points a canvas at
// them).
func gearIconsBuilt(p *Pack, atlases []gearicons.Atlas) bool {
	for _, a := range atlases {
		for _, typ := range []uint64{sparksGPUType, sparksDescType} {
			if _, ok := p.changes[Sets[0]][radpack.Key{Type: int64(typ), Name: int64(gearAtlasName(a))}]; !ok {
				return false
			}
		}
	}
	return true
}

// checkNewName refuses a new texture's record when a step already wrote it or the stock set already lists it: the step never
// replaces a record another step or the stock set owns.
func checkNewName(p *Pack, typ, name uint64, a gearicons.Atlas) error {
	key := radpack.Key{Type: int64(typ), Name: int64(name)}
	if _, ok := p.changes[Sets[0]][key]; ok {
		return fmt.Errorf("a step already wrote %s of %s", recordName(typ, name), a.Texture)
	}
	if _, found, err := p.sets[Sets[0]].Read(key.Type, key.Name); err != nil {
		return err
	} else if found {
		return fmt.Errorf("the stock set already lists %s of %s", recordName(typ, name), a.Texture)
	}
	return nil
}

// gearIconsApply is the step over a pins map and an atlas list: production passes gearicons.Pins and gearicons.Atlases, a test its
// own. Every check runs before the first Put, so a refusal leaves no record and no input.
func gearIconsApply(pins map[string]string, atlases []gearicons.Atlas) func(*Pack) error {
	return func(p *Pack) error {
		if p.gearIconsDir == "" {
			return errors.New("-gear-icons-dir is not set: the gear icons' art (he_is_the_cat's ICONS/hd) is required, and a pack is not built without it")
		}
		art, inputs, err := gearicons.ReadArt(p.gearIconsDir, pins)
		if err != nil {
			return err
		}
		type newRecord struct {
			typ, name uint64
			body      []byte
		}
		// The types are variables: int64 of a constant above 2^63 does not compile, and both sparks types are above it.
		gpuType, descType := sparksGPUType, sparksDescType
		var made []newRecord
		for _, a := range atlases {
			name := gearAtlasName(a)
			dds, err := p.Read(Sets[0], int64(gpuType), int64(a.Stock))
			if err != nil {
				return fmt.Errorf("atlas %016x: %w", a.Stock, err)
			}
			desc, err := p.Read(Sets[0], int64(descType), int64(a.Stock))
			if err != nil {
				return fmt.Errorf("atlas %016x: %w", a.Stock, err)
			}
			if err := checkStockAtlas(dds, desc, a); err != nil {
				return err
			}
			for _, typ := range []uint64{gpuType, descType} {
				if err := checkNewName(p, typ, name, a); err != nil {
					return err
				}
			}
			right, err := gearicons.Cells(a.W, a.H, art)
			if err != nil {
				return err
			}
			newDDS, err := gearIconDDS(dds, right, a.W, a.H)
			if err != nil {
				return fmt.Errorf("atlas %016x: %w", a.Stock, err)
			}
			newDesc, err := gearIconDescriptor(desc, len(newDDS), widthFactor*a.W, a.H)
			if err != nil {
				return fmt.Errorf("atlas %016x: %w", a.Stock, err)
			}
			made = append(made, newRecord{gpuType, name, newDDS}, newRecord{descType, name, newDesc})
		}
		for _, r := range made {
			if err := p.Put(Sets[0], int64(r.typ), int64(r.name), r.body); err != nil {
				return err
			}
		}
		for _, in := range inputs {
			p.inputs = append(p.inputs, PinnedInput{Step: gearIconsStepName, File: in.File, Sha256: in.Sha256})
		}
		return nil
	}
}
