package gearicons

import (
	"math"
	"reflect"
	"testing"
)

// Expected values were measured against Cat's tool (he_is_the_cat's gear_icons.py, read as text, nevr-pack 82ad7ba).
const (
	// The three stock atlases: width and height from each DDS header.
	atlasWGear, atlasHGear = 1024, 996
	atlasWUI, atlasHUI     = 1800, 1800
	atlasWSpec, atlasHSpec = 1920, 700

	// V2: CellSquare(cell, w, h) = {X, Y, Side}.
	sqFirstX, sqFirstY, sqFirstSide     = 22, 20, 159
	sqRowTwoX, sqRowTwoY, sqRowTwoSide  = 22, 418, 160
	sqEmptyX, sqEmptyY, sqEmptySide     = 842, 817, 159
	sqUIFirstX, sqUIFirstY, sqUIFirstSd = 36, 36, 288
	sqSpecX, sqSpecY, sqSpecSide        = 136, 14, 112

	cellRowTwo   = 10
	cellEmptyIdx = 24
	grid         = 5
	ownCount     = 11
	stockCount   = 12
	itemCount    = 23
	atlasCount   = 3
	canvasCount  = 13
	ownFirstCell = 0
	ownLastCell  = 10

	// V3: CellUV bits as float32.
	uvFirst0, uvFirst1, uvFirst2, uvFirst3 = 0x3f02c000, 0x3ca47f7c, 0x3f16a000, 0x3e38083a
	uvUI0, uvUI1, uvUI2, uvUI3             = 0x3f028f5c, 0x3ca3d70a, 0x3f170a3d, 0x3e3851ec

	// Atlas names: CSymbol64 is the step's, not this package's; the names are the record's own strings.
	nameGearTexture = "echocombat_gear_icons"
	nameUITexture   = "echocombat_gear_icons_ui"
	nameSpecTexture = "echocombat_gear_icons_spectator"
)

func TestCellSquareMatchesCatsGeometry(t *testing.T) {
	cases := []struct {
		name       string
		cell, w, h int
		x, y, side int
	}{
		{"first gear cell", 0, atlasWGear, atlasHGear, sqFirstX, sqFirstY, sqFirstSide},
		{"second row cell rounds the inner box", cellRowTwo, atlasWGear, atlasHGear, sqRowTwoX, sqRowTwoY, sqRowTwoSide},
		{"empty cell, last column and row", cellEmptyIdx, atlasWGear, atlasHGear, sqEmptyX, sqEmptyY, sqEmptySide},
		{"ui sheet first cell", 0, atlasWUI, atlasHUI, sqUIFirstX, sqUIFirstY, sqUIFirstSd},
		{"spectator sheet first cell is square", 0, atlasWSpec, atlasHSpec, sqSpecX, sqSpecY, sqSpecSide},
	}
	for _, c := range cases {
		got := CellSquare(c.cell, c.w, c.h)
		want := Square{X: c.x, Y: c.y, Side: c.side}
		if got != want {
			t.Errorf("%s: CellSquare(%d, %d, %d) = %+v, want %+v", c.name, c.cell, c.w, c.h, got, want)
		}
	}
}

func TestCellUVMatchesCatsBits(t *testing.T) {
	cases := []struct {
		name       string
		cell, w, h int
		want       [4]uint32
	}{
		{"gear sheet cell 0", 0, atlasWGear, atlasHGear, [4]uint32{uvFirst0, uvFirst1, uvFirst2, uvFirst3}},
		{"ui sheet cell 0", 0, atlasWUI, atlasHUI, [4]uint32{uvUI0, uvUI1, uvUI2, uvUI3}},
	}
	for _, c := range cases {
		got := CellUV(c.cell, c.w, c.h)
		for i := range got {
			if bits := math.Float32bits(got[i]); bits != c.want[i] {
				t.Errorf("%s: CellUV[%d] bits = %#x, want %#x (%v)", c.name, i, bits, c.want[i], got[i])
			}
		}
	}
}

func TestOwnCellsAreTheElevenInCatsOrder(t *testing.T) {
	want := map[string]int{
		"smoke": 0, "buff": 1, "point": 2, "loc": 3, "dot": 4, "drone": 5,
		"magnum": 6, "smg": 7, "chain": 8, "rifle": 9, "medigun": 10,
	}
	if got := OwnCells(); !reflect.DeepEqual(got, want) {
		t.Fatalf("OwnCells() = %v, want %v", got, want)
	}
	if len(want) != ownCount || ownLastCell != ownCount-1 || ownFirstCell != 0 {
		t.Fatalf("test constants disagree with the eleven own cells")
	}
}

func TestItemsAndStockIcons(t *testing.T) {
	if len(Items) != itemCount || ItemCount != itemCount {
		t.Fatalf("Items has %d entries (ItemCount %d), want %d", len(Items), ItemCount, itemCount)
	}
	if Items[0] != (Item{Kind: "ABILITY", Name: "heal", Display: "REPAIR MATRIX"}) {
		t.Fatalf("Items[0] = %+v", Items[0])
	}
	stock := 0
	for _, it := range Items {
		if StockIcon(it.Name) {
			stock++
		}
	}
	if stock != stockCount {
		t.Fatalf("%d items are stock icons, want %d", stock, stockCount)
	}
	if StockIcon("smoke") || !StockIcon("heal") {
		t.Fatalf("StockIcon(smoke) or StockIcon(heal) is wrong")
	}
	if Grid != grid || EmptyCell != cellEmptyIdx {
		t.Fatalf("Grid %d, EmptyCell %d, want %d and %d", Grid, EmptyCell, grid, cellEmptyIdx)
	}
}

func TestAtlasRecordsAndCanvases(t *testing.T) {
	if len(Atlases) != atlasCount || AtlasCount != atlasCount {
		t.Fatalf("Atlases has %d entries, want %d", len(Atlases), atlasCount)
	}
	names := []string{nameGearTexture, nameUITexture, nameSpecTexture}
	sizes := [][2]int{{atlasWGear, atlasHGear}, {atlasWUI, atlasHUI}, {atlasWSpec, atlasHSpec}}
	canvases := 0
	for i, a := range Atlases {
		if a.Texture != names[i] || a.W != sizes[i][0] || a.H != sizes[i][1] {
			t.Errorf("Atlases[%d] = %q %dx%d, want %q %dx%d", i, a.Texture, a.W, a.H, names[i], sizes[i][0], sizes[i][1])
		}
		canvases += len(a.Canvases)
	}
	if canvases != canvasCount {
		t.Errorf("the atlases list %d canvases, want %d", canvases, canvasCount)
	}
	if !Atlases[2].Spectator || Atlases[0].Spectator || Atlases[1].Spectator {
		t.Errorf("only the spectator atlas is marked Spectator")
	}
}

// wantIconFiles is the file each own-cell item reads, written out here so IconFiles is checked against a list the
// code does not supply (the names of the measured ICONS/hd set).
var wantIconFiles = map[string]string{
	"smoke": "smoke.png", "buff": "buff.png", "point": "point.png", "loc": "translocator.png",
	"dot": "dot.png", "drone": "drone.png", "magnum": "protostar.png", "smg": "quark.png",
	"chain": "blazar.png", "rifle": "starburst.png", "medigun": "medigun.png",
}

func TestIconFilesAndPinsAgree(t *testing.T) {
	if !reflect.DeepEqual(IconFiles, wantIconFiles) {
		t.Fatalf("IconFiles = %v, want %v", IconFiles, wantIconFiles)
	}
	if len(IconFiles) != ownCount || len(Pins) != ownCount {
		t.Fatalf("IconFiles has %d and Pins %d entries, want %d each", len(IconFiles), len(Pins), ownCount)
	}
	for item, file := range IconFiles {
		if _, ok := Pins[file]; !ok {
			t.Errorf("item %s names %s, which has no pin", item, file)
		}
	}
}

// specPins are the eleven icon files' sha-256 values as the spec's section 2 table records them (the art as measured),
// written out here so Pins is checked against a list the code does not supply: a typo in a pin is a red test.
var specPins = map[string]string{
	"smoke.png":        "864c0dc72a7350f8d56ad3ee7c8d91468f02e605e0beb448b4b95ad1810bc09d",
	"buff.png":         "07b3b9141f67b00e81430b26a3ed29ce23900d88e5b1d2ab292473c00621a145",
	"point.png":        "1e25ab4265f14cbf12b4450c573290fa37910bd942699d785427acf2b5c76ea1",
	"translocator.png": "fa6971da3151f2188eb7174022e73ee26a95400a00e114aa130e35a130a744d4",
	"dot.png":          "60ae5d2953056a6d79ab2b8f5c04d0cf5cdcabf03f01ea6a32419d6ba790ce00",
	"drone.png":        "98e9b4fb368c519110d22fa60688f6387a3898865b427449ce8ecbc466a501d7",
	"protostar.png":    "d251b20f13d00b658187fd4621f4d64a83d7bb5aa1469dff8e1e1461bd3e94eb",
	"quark.png":        "60645737582811a6472d2c4e58e5f912b82b116b4d09f3fc6bb15c4f2acf9483",
	"blazar.png":       "c37cf471db7d2eda912a7ce47fb24e6727e0475c4e3861a39abd053c037850a6",
	"starburst.png":    "f472e8dac8cc8423df48ef3ebd2326e35d5a7c662c574977c7c885d5d3c9c296",
	"medigun.png":      "85fe6d1d2c960f357d520c04c01127a97a545c4954bc3684a206b0e9e50d9462",
}

func TestPinsMatchTheSpecTable(t *testing.T) {
	if len(Pins) != len(specPins) || len(specPins) != ownCount {
		t.Fatalf("Pins has %d entries, the spec table %d, want %d", len(Pins), len(specPins), ownCount)
	}
	for file, want := range specPins {
		if got, ok := Pins[file]; !ok || got != want {
			t.Errorf("Pins[%s] = %q, want %q", file, got, want)
		}
	}
}
