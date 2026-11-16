// Package gearicons holds the layout of the gear-icon atlases: which gear items get a cell in the
// right half of each atlas, where each cell's icon square sits, and the icon art's alpha as it is read
// and resampled. It is pure: no file is written and no game record is read; the step that puts the
// textures into a content pack (content-pack-build) reads the art and the stock records.
//
// Origin: he_is_the_cat's tools/gear_icons.py (nevr-pack 82ad7ba, read as text only): ITEMS :83-93,
// STOCK_ICON :94-95, own_cells :121-123, cell_square :258-268, cell_uv :271-275, cells_image :135-172,
// ATLASES :58-74, Grid :117, CELL_MARGIN :173, ICON_PAD :174. The geometry rounds half to even as his
// Python round does (math.RoundToEven); the resample is this package's own exact box average, not his
// LANCZOS resize (see art.go).
package gearicons

import "math"

// Item is one gear item: its element prefix (Kind), its gear name and its display name (gear_icons.py:83-93).
type Item struct{ Kind, Name, Display string }

// ItemCount is the number of gear items, in Cat's ITEMS order.
const ItemCount = 23

// Items is every gear item in Cat's order (gear_icons.py:83-93).
var Items = [ItemCount]Item{
	{"ABILITY", "heal", "REPAIR MATRIX"}, {"ABILITY", "sensor", "THREAT SCANNER"},
	{"ABILITY", "shield", "ENERGY BARRIER"}, {"ABILITY", "wraith", "PHASE SHIFT"},
	{"ABILITY", "smoke", "UMBRAL VEIL"}, {"ABILITY", "buff", "OVERCHARGE"}, {"ABILITY", "point", "NEUTRON"},
	{"GRENADE", "det", "DETONATOR"}, {"GRENADE", "stun", "STUN FIELD"}, {"GRENADE", "arc", "ARC MINE"},
	{"GRENADE", "burst", "INSTANT REPAIR"}, {"GRENADE", "loc", "TRANSLOCATOR"}, {"GRENADE", "dot", "DOT FIELD"},
	{"GRENADE", "drone", "DRONE"},
	{"WEAPON", "assault", "PULSAR"}, {"WEAPON", "blaster", "NOVA"}, {"WEAPON", "scout", "COMET"},
	{"WEAPON", "rocket", "METEOR"}, {"WEAPON", "magnum", "PROTOSTAR"}, {"WEAPON", "smg", "QUARK"},
	{"WEAPON", "chain", "BLAZAR"}, {"WEAPON", "rifle", "STARBURST"}, {"WEAPON", "medigun", "MEDIGUN"},
}

// stockIcons are the items that keep their stock icon and get no cell (STOCK_ICON, gear_icons.py:94-95).
var stockIcons = map[string]bool{
	"heal": true, "sensor": true, "shield": true, "wraith": true, "det": true, "stun": true, "arc": true,
	"burst": true, "assault": true, "blaster": true, "scout": true, "rocket": true,
}

// StockIcon reports whether the gear item keeps its stock icon.
func StockIcon(name string) bool { return stockIcons[name] }

// Grid is the cells per row and per column of a right half (gear_icons.py:117).
const Grid = 5

// EmptyCell is the one cell never painted: fully transparent (gear_icons.py:118).
const EmptyCell = Grid*Grid - 1

// OwnCells maps each gear item that gets a cell to its cell: the items not in StockIcon, numbered in Items
// order (gear_icons.py:121-123).
func OwnCells() map[string]int {
	cells := map[string]int{}
	for _, it := range Items {
		if !StockIcon(it.Name) {
			cells[it.Name] = len(cells)
		}
	}
	return cells
}

// IconFiles names the PNG each own-cell item's icon is read from, in the pinned ICONS/hd set.
var IconFiles = map[string]string{
	"smoke": "smoke.png", "buff": "buff.png", "point": "point.png", "loc": "translocator.png",
	"dot": "dot.png", "drone": "drone.png", "magnum": "protostar.png", "smg": "quark.png",
	"chain": "blazar.png", "rifle": "starburst.png", "medigun": "medigun.png",
}

// Pins maps each icon file to the sha-256 (hex) of the file as the art was measured.
// The bytes are never in the tree.
var Pins = map[string]string{
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

// Square is the square, in a right half, that one cell's icon is drawn in: its top-left corner and its side.
type Square struct{ X, Y, Side int }

// Geometry constants (gear_icons.py:173-174). Products with them are wrapped in float64() so the compiler cannot
// fuse them into a fused multiply-add.
const (
	cellMargin = 0.1
	iconPad    = 0.03
	halfBase   = 0.5
	// cropScale is the box's half-size factor: half = max(side) * (0.5 + ICON_PAD) (gear_icons.py:160).
	cropScale = halfBase + iconPad
)

// texWidthFact is the new texture's width over the stock width: the right half is added beside the stock half.
const texWidthFact = 2

// roundHalfEven is Python 3's round for a float: halves go to the even integer.
func roundHalfEven(v float64) int { return int(math.RoundToEven(v)) }

// CellSquare is the square cell's icon is drawn in: centred in the cell's inner box, square so a wide sheet does
// not stretch it (gear_icons.py:258-268).
func CellSquare(cell, w, h int) Square {
	cx, cy := cell%Grid, cell/Grid
	x0 := roundHalfEven(float64(cx*w) / float64(Grid))
	x1 := roundHalfEven(float64((cx+1)*w) / float64(Grid))
	y0 := roundHalfEven(float64(cy*h) / float64(Grid))
	y1 := roundHalfEven(float64((cy+1)*h) / float64(Grid))
	ix0 := roundHalfEven(float64(x0) + float64(float64(x1-x0)*cellMargin))
	ix1 := roundHalfEven(float64(x1) - float64(float64(x1-x0)*cellMargin))
	iy0 := roundHalfEven(float64(y0) + float64(float64(y1-y0)*cellMargin))
	iy1 := roundHalfEven(float64(y1) - float64(float64(y1-y0)*cellMargin))
	side := min(ix1-ix0, iy1-iy0)
	return Square{X: ix0 + (ix1-ix0-side)/halfDiv, Y: iy0 + (iy1-iy0-side)/halfDiv, Side: side}
}

// CellUV is the UV rectangle (u0, v0, u1, v1) of the cell's square in the new texture, which is 2w x h: the
// right half starts at u = 0.5 (gear_icons.py:271-275). Computed in float64, stored as float32.
func CellUV(cell, w, h int) [4]float32 {
	s := CellSquare(cell, w, h)
	u0 := halfBase + float64(s.X)/float64(texWidthFact*w)
	v0 := float64(s.Y) / float64(h)
	u1 := halfBase + float64(s.X+s.Side)/float64(texWidthFact*w)
	v1 := float64(s.Y+s.Side) / float64(h)
	return [4]float32{float32(u0), float32(v0), float32(u1), float32(v1)}
}

// AtlasCount is the number of atlases the gear icons are added to.
const AtlasCount = 3

// Atlas is one stock atlas and the new texture that extends it: the stock name, the new texture's name before it
// is hashed (the step hashes it), the stock size (the right half has the same size), the canvases that show it
// (gear_icons.py:58-74) and whether it is the spectator sheet (SPECTATOR_ATLASES, gear_icons.py:77).
type Atlas struct {
	Stock     uint64
	Texture   string
	W, H      int
	Canvases  []uint64
	Spectator bool
}

// Atlases are the three atlases, in Cat's order (gear_icons.py:58-74).
var Atlases = [AtlasCount]Atlas{
	{Stock: 0xd8b84ca5707d3bd2, Texture: "echocombat_gear_icons", W: 1024, H: 996, Canvases: []uint64{
		0x1e5d2a97184cad81, 0x2262b1c4c25c734e, 0x6083f456d52ef5f1, 0xb138c20e60e662a1}},
	{Stock: 0x9ec57c5df16fef80, Texture: "echocombat_gear_icons_ui", W: 1800, H: 1800, Canvases: []uint64{
		0x13361c4773e07b3e, 0x2d06e14794a11e31, 0x8fcec01d07930956, 0xf4cc172339347e8b}},
	{Stock: 0x7da6dd100d552a00, Texture: "echocombat_gear_icons_spectator", W: 1920, H: 700, Spectator: true, Canvases: []uint64{
		0x5546fc45ea60c419, 0x6705b33841b53ba3, 0x8839cd82f4f101e7, 0x6705ba254cb733b1, 0xd97d0f6bb809966d}},
}
