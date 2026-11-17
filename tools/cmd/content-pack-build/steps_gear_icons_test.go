package main

// The gear-icon atlas step over synthetic atlases (steps_gear_icons.go). No byte of the game's and no PNG of Cat's is in the
// tree: the art is eleven PNGs the test makes with image/png, the atlases are two tiny BC7 DDS files of a pattern the test
// writes, and the pins are the sha-256 of those PNGs. The sizes and offsets are the ones the three stock atlases measure
// (the constants and the comment on each test name them).

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"image"
	"image/color"
	"image/png"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/bc7"
	"github.com/EchoTools/nevr-runtime/tools/internal/gearicons"
	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

const (
	iconSide      = 16 // the fixture PNGs' side
	iconAlphaBase = 40 // the alpha of the fixture icon of cell c is iconAlphaBase + iconAlphaStep*c
	iconAlphaStep = 15
	encodeBound   = 11 // the largest alpha difference the mode-6 encoder leaves on any block (internal/bc7's index spacing)
	own           = 11 // gear items with a cell, and with a PNG
	atlasBlocks   = 16 // the fixture stock blocks' bytes
	descBytes     = 256
	vecW, vecH    = 8, 4 // V10's stock atlas
	vecLen        = 212  // V10's output: 148 + 64
	vecLinear     = 64   // V10's linear size: (16/4)(4/4)16
	// V10's right half block: all (255,255,255,0), encoded by the tree's mode-6 encoder.
	vecRightBlock = "c0ffffffffff01800100000000000000"
)

// fixtureAtlases are two tiny atlases: stock names 0xa1 and 0xa2, 40x40 and 48x40 (multiples of 4, and large enough for a
// five-by-five grid).
func fixtureAtlases() []gearicons.Atlas {
	return []gearicons.Atlas{{Stock: 0xa1, Texture: "test_gear_icons_one", W: 40, H: 40}, {Stock: 0xa2, Texture: "test_gear_icons_two", W: 48, H: 40}}
}

// fixtureArt writes the eleven PNGs into a new folder and returns it with the pins that match them. The icon of cell c is a
// filled square of alpha iconAlphaBase + iconAlphaStep*c, written as NRGBA so the encoder uses colour type 6.
func fixtureArt(t *testing.T) (string, map[string]string) {
	t.Helper()
	dir := t.TempDir()
	pins := map[string]string{}
	for name, cell := range gearicons.OwnCells() {
		img := image.NewNRGBA(image.Rect(0, 0, iconSide, iconSide))
		a := uint8(iconAlphaBase + iconAlphaStep*cell)
		for y := 0; y < iconSide; y++ {
			for x := 0; x < iconSide; x++ {
				img.SetNRGBA(x, y, color.NRGBA{R: 9, G: 9, B: 9, A: a})
			}
		}
		var buf bytes.Buffer
		if err := png.Encode(&buf, img); err != nil {
			t.Fatal(err)
		}
		file := gearicons.IconFiles[name]
		if err := os.WriteFile(filepath.Join(dir, file), buf.Bytes(), testFileMode); err != nil {
			t.Fatal(err)
		}
		sum := sha256.Sum256(buf.Bytes())
		pins[file] = hex.EncodeToString(sum[:])
	}
	return dir, pins
}

// stockAtlas is the stock DDS file and descriptor of a: the header the step checks, and blocks of a pattern.
func stockAtlas(a gearicons.Atlas) (dds, desc []byte) {
	dds = make([]byte, ddsLen(a.W, a.H))
	copy(dds, ddsMagic)
	setU32(dds, ddsHeightOff, a.H)
	setU32(dds, ddsWidthOff, a.W)
	setU32(dds, ddsLinearOff, len(dds)-ddsHeaderSize)
	setU32(dds, ddsMipsOff, atlasMips)
	setU32(dds, ddsFourCCOff, ddsDX10FourCC)
	setU32(dds, ddsDXGIOff, dxgiBC7SRGB)
	setU32(dds, ddsDimOff, dimTexture2D)
	setU32(dds, ddsArrayOff, 1)
	for i := ddsHeaderSize; i < len(dds); i++ {
		dds[i] = byte(i % 251)
	}
	desc = make([]byte, descBytes)
	for off, v := range map[int]int{descStreamOff: 1, descMaxWOff: a.W, descMaxHOff: a.H, descMaxMipsOff: atlasMips, descWOff: a.W,
		descHOff: a.H, descMipsOff: atlasMips, descResMemOff: len(dds), descPitchOff: len(dds) - ddsHeaderSize} {
		setU32(desc, off, v)
	}
	return dds, desc
}

func gearKey(typ uint64, name uint64) radpack.Key {
	return radpack.Key{Type: int64(typ), Name: int64(name)}
}

// gearPack is a pack over a stock Sets[0] holding the atlases (each changed by edit, when it is set) and the extra records.
func gearPack(t *testing.T, atlases []gearicons.Atlas, edit func(a gearicons.Atlas, dds, desc []byte) ([]byte, []byte), extra map[radpack.Key][]byte) *Pack {
	t.Helper()
	res := map[radpack.Key][]byte{}
	for _, a := range atlases {
		dds, desc := stockAtlas(a)
		if edit != nil {
			dds, desc = edit(a, dds, desc)
		}
		res[gearKey(sparksGPUType, a.Stock)], res[gearKey(sparksDescType, a.Stock)] = dds, desc
	}
	for k, v := range extra {
		res[k] = v
	}
	data := t.TempDir()
	if err := radpack.WriteStockSet(data, Sets[0], []map[radpack.Key][]byte{res}); err != nil {
		t.Fatal(err)
	}
	s, _, err := radpack.OpenSet(data, Sets[0])
	if err != nil {
		t.Fatal(err)
	}
	return &Pack{sets: map[string]*radpack.Set{Sets[0]: s}, changes: map[string]map[radpack.Key][]byte{}, scripts: map[string][]byte{}}
}

// V10: an 8x4 stock atlas of two blocks A, B and an all-(255,255,255,0) right half give header, A, B, E, E.
func TestTheDDSIsTheStockHeaderThenEachBlockRowStockThenRight(t *testing.T) {
	stock := make([]byte, ddsLen(vecW, vecH))
	copy(stock, ddsMagic)
	setU32(stock, ddsHeightOff, vecH)
	setU32(stock, ddsWidthOff, vecW)
	setU32(stock, ddsLinearOff, vecLinear/2)
	setU32(stock, ddsMipsOff, atlasMips)
	setU32(stock, ddsFourCCOff, ddsDX10FourCC)
	setU32(stock, ddsDXGIOff, dxgiBC7SRGB)
	blockA, blockB := bytes.Repeat([]byte{0x11}, atlasBlocks), bytes.Repeat([]byte{0x22}, atlasBlocks)
	copy(stock[ddsHeaderSize:], blockA)
	copy(stock[ddsHeaderSize+atlasBlocks:], blockB)
	right := bytes.Repeat([]byte{255, 255, 255, 0}, vecW*vecH)
	out, err := gearIconDDS(stock, right, vecW, vecH)
	if err != nil {
		t.Fatal(err)
	}
	e, _ := hex.DecodeString(vecRightBlock)
	if enc, err := bc7.Encode(right[:vecW*vecH*bc7.Channels], vecW, vecH); err != nil || !bytes.Equal(enc[:atlasBlocks], e) {
		t.Fatalf("the encoder's block for an all-(255,255,255,0) block is %x, %v; the vector says %s", enc[:atlasBlocks], err, vecRightBlock)
	}
	if len(out) != vecLen {
		t.Fatalf("%d bytes, want %d", len(out), vecLen)
	}
	if u32(out, ddsWidthOff) != 2*vecW || u32(out, ddsLinearOff) != vecLinear || u32(out, ddsHeightOff) != vecH {
		t.Errorf("width %d, linear %d, height %d, want %d, %d, %d", u32(out, ddsWidthOff), u32(out, ddsLinearOff), u32(out, ddsHeightOff), 2*vecW, vecLinear, vecH)
	}
	if !bytes.Equal(out[ddsHeaderSize:], append(append(append(append([]byte{}, blockA...), blockB...), e...), e...)) {
		t.Errorf("the blocks are not A, B, E, E: %x", out[ddsHeaderSize:])
	}
	if !bytes.Equal(out[:ddsWidthOff], stock[:ddsWidthOff]) || !bytes.Equal(out[ddsLinearOff+4:ddsHeaderSize], stock[ddsLinearOff+4:ddsHeaderSize]) {
		t.Error("a header byte other than the width and the linear size changed")
	}
	// The descriptor of it: @0xC4 = @0xE8 = 16, @0xC8 = @0xEC = 4, @0xF4 = 212, @0xF8 = 64, @0xC0 = @0xCC = @0xF0 = 1.
	stockDesc := bytes.Repeat([]byte{0x5a}, descBytes)
	d, err := gearIconDescriptor(stockDesc, len(out), 2*vecW, vecH)
	if err != nil {
		t.Fatal(err)
	}
	for off, want := range map[int]int{descStreamOff: 1, descMaxWOff: 2 * vecW, descMaxHOff: vecH, descMaxMipsOff: 1, descWOff: 2 * vecW,
		descHOff: vecH, descMipsOff: 1, descResMemOff: vecLen, descPitchOff: vecLinear} {
		if got := u32(d, off); got != want {
			t.Errorf("descriptor @%#x is %d, want %d", off, got, want)
		}
	}
	changed := 0
	for i := range d {
		if d[i] != stockDesc[i] {
			changed++
		}
	}
	if want := 9 * 4; changed > want { // nine u32 fields; a field whose value has a byte equal to 0x5a changes fewer bytes
		t.Errorf("%d descriptor bytes changed, at most %d (nine u32 fields)", changed, want)
	}
	if _, err := gearIconDescriptor(stockDesc[:descBytes-1], len(out), 2*vecW, vecH); err == nil {
		t.Error("a 255-byte descriptor was used")
	}
}

// V11: the real atlases' new records are 2039956, 6480148 and 2688148 bytes (148 + twice the stock blocks).
func TestTheShippedAtlasesNewRecordsHaveTheMeasuredSizes(t *testing.T) {
	want := []int{2039956, 6480148, 2688148}
	for i, a := range gearicons.Atlases {
		if got := ddsLen(2*a.W, a.H); got != want[i] {
			t.Errorf("%s: %d bytes, want %d", a.Texture, got, want[i])
		}
		if got := ddsLen(2*a.W, a.H) - ddsHeaderSize; got != 2*(ddsLen(a.W, a.H)-ddsHeaderSize) {
			t.Errorf("%s: pitch %d is not twice the stock's", a.Texture, got)
		}
	}
	// V1: the new textures' names (the tree's CSymbol64 rule).
	names := []uint64{0xa9f48bfe2a346543, 0x6e75e05decc99ad1, 0xf9d452940b0cc34a}
	for i, a := range gearicons.Atlases {
		if got := gearAtlasName(a); got != names[i] {
			t.Errorf("%s: name %016x, want %016x", a.Texture, got, names[i])
		}
	}
}

func TestTheStepWritesSixRecordsThatDecodeToTheRightHalf(t *testing.T) {
	dir, pins := fixtureArt(t)
	atlases := fixtureAtlases()
	p := gearPack(t, atlases, nil, nil)
	p.gearIconsDir = dir
	if err := gearIconsApply(pins, atlases)(p); err != nil {
		t.Fatal(err)
	}
	if n := len(p.changes[Sets[0]]); n != 2*len(atlases) {
		t.Fatalf("%d records put, want %d", n, 2*len(atlases))
	}
	if !gearIconsBuilt(p, atlases) {
		t.Error("gearIconsBuilt says a record is missing")
	}
	art, wantInputs, err := gearicons.ReadArt(dir, pins)
	if err != nil {
		t.Fatal(err)
	}
	for _, a := range atlases {
		stockDDS, stockDesc := stockAtlas(a)
		got := p.changes[Sets[0]][gearKey(sparksGPUType, gearAtlasName(a))]
		if len(got) != ddsLen(2*a.W, a.H) {
			t.Fatalf("%s: %d bytes, want %d", a.Texture, len(got), ddsLen(2*a.W, a.H))
		}
		// The left half of every block row is the stock atlas's.
		stockHalf, err := readGearIconStockHalf(got)
		if err != nil {
			t.Fatal(err)
		}
		wantHalf := append([]byte(nil), got[:ddsHeaderSize]...)
		row := (a.W / blockDim) * bc7BlockBytes
		for by := 0; by < a.H/blockDim; by++ {
			wantHalf = append(wantHalf, stockDDS[ddsHeaderSize+by*row:ddsHeaderSize+(by+1)*row]...)
		}
		if !bytes.Equal(stockHalf, wantHalf) {
			t.Errorf("%s: the stock half is not the stock atlas's blocks", a.Texture)
		}
		// The right half decodes to the step's own unencoded right half, within the encoder's bound, with RGB exactly 255.
		var rightBlocks []byte
		for by := 0; by < a.H/blockDim; by++ {
			at := ddsHeaderSize + by*2*row + row
			rightBlocks = append(rightBlocks, got[at:at+row]...)
		}
		dec, err := bc7.Decode(rightBlocks, a.W, a.H)
		if err != nil {
			t.Fatal(err)
		}
		want, err := gearicons.Cells(a.W, a.H, art)
		if err != nil {
			t.Fatal(err)
		}
		worst := 0
		for i := 0; i < len(want); i += bc7.Channels {
			for c := 0; c < alphaStart; c++ {
				if dec[i+c] != 255 {
					t.Fatalf("%s: pixel %d channel %d is %d, want 255", a.Texture, i/bc7.Channels, c, dec[i+c])
				}
			}
			if d := absDiff(dec[i+alphaStart], want[i+alphaStart]); d > worst {
				worst = d
			}
		}
		if worst > encodeBound {
			t.Errorf("%s: the decoded alpha differs from the step's by %d, bound %d", a.Texture, worst, encodeBound)
		}
		// The descriptor is the stock one with the nine fields.
		gotDesc := p.changes[Sets[0]][gearKey(sparksDescType, gearAtlasName(a))]
		wantDesc, _ := gearIconDescriptor(stockDesc, len(got), 2*a.W, a.H)
		if !bytes.Equal(gotDesc, wantDesc) || u32(gotDesc, descWOff) != 2*a.W || u32(gotDesc, descResMemOff) != len(got) {
			t.Errorf("%s: descriptor %x", a.Texture, gotDesc)
		}
	}
	if len(p.inputs) != own {
		t.Fatalf("%d inputs, want %d", len(p.inputs), own)
	}
	for i, in := range wantInputs {
		if p.inputs[i] != (PinnedInput{Step: gearIconsStepName, File: in.File, Sha256: in.Sha256}) {
			t.Errorf("input %d is %+v, want %+v", i, p.inputs[i], in)
		}
	}
	st := Step{Name: gearIconsStepName, Apply: nil, Writes: gearIconsWrites(atlases)}
	if err := checkChanged([]Step{st}, p.changes); err != nil {
		t.Errorf("what the step changed is not what it declares: %v", err)
	}
}

const alphaStart = 3 // the alpha's index in a pixel

func absDiff(a, b uint8) int {
	if a > b {
		return int(a - b)
	}
	return int(b - a)
}

// Every refusal puts nothing and leaves no input.
func TestTheStepRefusesWhatIsNotTheMeasuredAtlasAndPutsNothing(t *testing.T) {
	dir, pins := fixtureArt(t)
	atlases := fixtureAtlases()
	second := atlases[1].Stock
	cases := []struct {
		name  string
		edit  func(a gearicons.Atlas, dds, desc []byte) ([]byte, []byte)
		extra map[radpack.Key][]byte
		dir   string
		want  string
	}{
		{"no -gear-icons-dir", nil, nil, "", "-gear-icons-dir"}, // an empty directory is the absent flag
		{"dxgi 98", func(a gearicons.Atlas, d, s []byte) ([]byte, []byte) {
			if a.Stock == second {
				setU32(d, ddsDXGIOff, 98)
			}
			return d, s
		}, nil, dir, "dxgiFormat"},
		{"two mips", func(a gearicons.Atlas, d, s []byte) ([]byte, []byte) {
			if a.Stock == second {
				setU32(d, ddsMipsOff, 2)
			}
			return d, s
		}, nil, dir, "mip count"},
		{"width off by four", func(a gearicons.Atlas, d, s []byte) ([]byte, []byte) {
			if a.Stock == second {
				setU32(d, ddsWidthOff, a.W+blockDim)
			}
			return d, s
		}, nil, dir, "width"},
		{"a block short", func(a gearicons.Atlas, d, s []byte) ([]byte, []byte) {
			if a.Stock == second {
				d = d[:len(d)-bc7BlockBytes]
			}
			return d, s
		}, nil, dir, "record length"},
		{"descriptor of 255 bytes", func(a gearicons.Atlas, d, s []byte) ([]byte, []byte) {
			if a.Stock == second {
				s = s[:descBytes-1]
			}
			return d, s
		}, nil, dir, "descriptor has 255"},
		{"descriptor resident memory off by one", func(a gearicons.Atlas, d, s []byte) ([]byte, []byte) {
			if a.Stock == second {
				setU32(s, descResMemOff, u32(s, descResMemOff)+1)
			}
			return d, s
		}, nil, dir, "resident memory"},
		{"a new texture's name already in the stock set", nil,
			map[radpack.Key][]byte{gearKey(sparksGPUType, csymbol64(atlases[1].Texture)): []byte("x")}, dir, "already lists"},
	}
	for _, c := range cases {
		p := gearPack(t, atlases, c.edit, c.extra)
		p.gearIconsDir = c.dir
		err := gearIconsApply(pins, atlases)(p)
		if err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: error %v does not name %q", c.name, err, c.want)
		}
		if len(p.changes[Sets[0]]) != 0 || len(p.inputs) != 0 {
			t.Errorf("%s: a refusal left %d records and %d inputs", c.name, len(p.changes[Sets[0]]), len(p.inputs))
		}
	}
	// An art file with another hash: the error names the file and both hashes.
	p := gearPack(t, atlases, nil, nil)
	p.gearIconsDir = dir
	file := gearicons.IconFiles["smoke"]
	b, err := os.ReadFile(filepath.Join(dir, file))
	if err != nil {
		t.Fatal(err)
	}
	b[len(b)/2] ^= 1
	bent := t.TempDir()
	for _, f := range gearicons.IconFiles {
		src, err := os.ReadFile(filepath.Join(dir, f))
		if err != nil {
			t.Fatal(err)
		}
		if f == file {
			src = b
		}
		if err := os.WriteFile(filepath.Join(bent, f), src, testFileMode); err != nil {
			t.Fatal(err)
		}
	}
	p.gearIconsDir = bent
	if err := gearIconsApply(pins, atlases)(p); err == nil || !strings.Contains(err.Error(), file) || !strings.Contains(err.Error(), pins[file]) {
		t.Errorf("a changed art file: %v", err)
	}
	if len(p.changes[Sets[0]]) != 0 || len(p.inputs) != 0 {
		t.Error("a changed art file left records or inputs")
	}
}

// A new texture's record that an earlier step already wrote is refused, and the step adds none of its own.
func TestTheStepRefusesANameAnEarlierStepAlreadyWrote(t *testing.T) {
	dir, pins := fixtureArt(t)
	atlases := fixtureAtlases()
	p := gearPack(t, atlases, nil, nil)
	p.gearIconsDir = dir
	p.changes[Sets[0]] = map[radpack.Key][]byte{gearKey(sparksDescType, gearAtlasName(atlases[1])): []byte("earlier")}
	err := gearIconsApply(pins, atlases)(p)
	if err == nil || !strings.Contains(err.Error(), "already wrote") {
		t.Fatalf("error %v does not say that an earlier step wrote the name", err)
	}
	if len(p.changes[Sets[0]]) != 1 || len(p.inputs) != 0 {
		t.Errorf("a refusal left %d records and %d inputs, want the earlier step's one record and no input", len(p.changes[Sets[0]]), len(p.inputs))
	}
}

// The reader keeps the header and the stock half: a record whose right half the test overwrites reads the same.
func TestTheStockHalfReaderIgnoresTheRightHalfAndRefusesAShortRecord(t *testing.T) {
	a := fixtureAtlases()[0]
	stock, _ := stockAtlas(a)
	right := bytes.Repeat([]byte{200, 100, 50, 25}, a.W*a.H)
	rec, err := gearIconDDS(stock, right, a.W, a.H)
	if err != nil {
		t.Fatal(err)
	}
	first, err := readGearIconStockHalf(rec)
	if err != nil {
		t.Fatal(err)
	}
	other := append([]byte(nil), rec...)
	row := (a.W / blockDim) * bc7BlockBytes
	for by := 0; by < a.H/blockDim; by++ {
		for i := 0; i < row; i++ {
			other[ddsHeaderSize+by*2*row+row+i] ^= 0xff
		}
	}
	second, err := readGearIconStockHalf(other)
	if err != nil || !bytes.Equal(first, second) {
		t.Errorf("the reader saw the right half: %v", err)
	}
	if _, err := readGearIconStockHalf(rec[:len(rec)-bc7BlockBytes]); err == nil || !strings.Contains(err.Error(), "too short") {
		t.Errorf("a short record: %v", err)
	}
	if _, err := readGearIconStockHalf(rec[:ddsHeaderSize-1]); err == nil || !strings.Contains(err.Error(), "too short") {
		t.Errorf("a record shorter than its header: %v", err)
	}
}

// gearGame is a game folder whose Sets[0] holds the atlases and whose Sets[1] holds one record, as Build opens them.
func gearGame(t *testing.T, atlases []gearicons.Atlas) string {
	t.Helper()
	game := filepath.Join(t.TempDir(), "echovr")
	data := filepath.Join(game, filepath.FromSlash(DataDir))
	res := map[radpack.Key][]byte{}
	for _, a := range atlases {
		res[gearKey(sparksGPUType, a.Stock)], res[gearKey(sparksDescType, a.Stock)] = stockAtlas(a)
	}
	if err := radpack.WriteStockSet(data, Sets[0], []map[radpack.Key][]byte{res}); err != nil {
		t.Fatal(err)
	}
	if err := radpack.WriteStockSet(data, Sets[1], []map[radpack.Key][]byte{{{Type: 4, Name: 4}: []byte("stock bank")}}); err != nil {
		t.Fatal(err)
	}
	return game
}

// Build with -gear-icons-dir logs one input_pinned record per input and lists them in the summary; without the flag it refuses,
// naming it, and writes no pack.
func TestBuildPinsTheArtAndRefusesWithoutIt(t *testing.T) {
	dir, pins := fixtureArt(t)
	atlases := fixtureAtlases()
	list := []Step{{Name: gearIconsStepName, Apply: gearIconsApply(pins, atlases), Writes: gearIconsWrites(atlases)}}
	game := gearGame(t, atlases)
	var logs bytes.Buffer
	log := slog.New(slog.NewJSONHandler(&logs, nil))
	out := filepath.Join(t.TempDir(), "gear")
	res, err := Build(list, game, out, "gear", "1", Inputs{GearIconsDir: dir}, log)
	if err != nil {
		t.Fatal(err)
	}
	if len(res.Inputs) != own || strings.Count(logs.String(), `"msg":"input_pinned"`) != own {
		t.Errorf("%d inputs in the result, %d input_pinned records; want %d each", len(res.Inputs), strings.Count(logs.String(), `"msg":"input_pinned"`), own)
	}
	line, _ := json.Marshal(res)
	if !strings.Contains(string(line), `"inputs":[{"step":`) {
		t.Errorf("the summary line %s lists no inputs", line)
	}
	out2 := filepath.Join(t.TempDir(), "none")
	if _, err := Build(list, game, out2, "none", "1", Inputs{}, log); err == nil || !strings.Contains(err.Error(), "-gear-icons-dir") {
		t.Errorf("a build without the flag: %v", err)
	}
	if _, err := os.Lstat(out2); err == nil {
		t.Error("a refused build left a pack")
	}
}

// The flag reaches the step through the command line, and its absence is exit 1 with the flag named on stderr.
func TestTheFlagReachesTheStep(t *testing.T) {
	dir, pins := fixtureArt(t)
	atlases := fixtureAtlases()
	list := []Step{{Name: gearIconsStepName, Apply: gearIconsApply(pins, atlases), Writes: gearIconsWrites(atlases)}}
	game := gearGame(t, atlases)
	env := map[string]string{"XDG_STATE_HOME": t.TempDir()}
	f := fixture{game: game, root: t.TempDir(), env: env}
	code, stdout, stderr := f.run(list, "-game-dir", game, "-out", f.out("gear"), "-id", "gear", "-version", "1", "-gear-icons-dir", dir)
	if code != 0 || !strings.Contains(stdout, `"inputs"`) {
		t.Errorf("with the flag: exit %d, stdout %s, stderr %s", code, stdout, stderr)
	}
	code, _, stderr = f.run(list, "-game-dir", game, "-out", f.out("gear2"), "-id", "gear2", "-version", "1")
	if code != 1 || !strings.Contains(stderr, "-gear-icons-dir") {
		t.Errorf("without the flag: exit %d, stderr %s", code, stderr)
	}
}

// The production list holds the step, with the real atlases' three new textures in both declared fields.
func TestTheGearIconsStepIsRegisteredWithTheRealAtlases(t *testing.T) {
	st := stepNamed(t, gearIconsStepName)
	if len(st.Writes) != 2 {
		t.Fatalf("%d fields declared, want the DDS header and the descriptor", len(st.Writes))
	}
	want := []uint64{gearAtlasName(gearicons.Atlases[0]), gearAtlasName(gearicons.Atlases[1]), gearAtlasName(gearicons.Atlases[2])}
	for i, typ := range []uint64{sparksGPUType, sparksDescType} {
		f := st.Writes[i]
		if f.Set != Sets[0] || f.Type != typ || len(f.Records) != len(want) {
			t.Fatalf("field %d is %+v, want %d records of type %016x in %s", i, f, len(want), typ, Sets[0])
		}
		for j, r := range f.Records {
			if r != want[j] {
				t.Errorf("field %d record %d is %016x, want %016x", i, j, r, want[j])
			}
		}
	}
}
