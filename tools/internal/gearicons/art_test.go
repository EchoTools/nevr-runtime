package gearicons

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"image"
	"image/color"
	"image/png"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// Vector values worked by hand from the resample and crop rules, and the fixture sizes of the reader tests.
const (
	alphaZero        = 0
	alphaOne         = 1
	alphaFull        = 255
	alphaMid         = 100
	alphaLow         = 50
	alphaSmall       = 90
	alphaOut         = 30
	alphaHigh        = 200
	alphaSeventySev  = 77
	alphaBoxBase     = 40
	alphaBoxStep     = 15
	rgbPadding       = 9
	boxSmall         = 10
	boxLarge         = 100
	boxBigSide       = 106
	bboxX0, bboxY0   = 2, 3
	bboxX1, bboxY1   = 6, 5
	cropVX0, cropVY0 = 2, 2
	cropVX1, cropVY1 = 6, 6
	cropSide         = 4
	padCropX0        = -3
	padCropY0        = -2
	padCropX1        = 103
	padCropY1        = 104
	topCropY0        = -4
	topCropY1        = 6
	fixtureSide      = 10
	fixtureFlipAt    = 40
	rightHalfSide    = 40
	sideTwo          = 2
	sideThree        = 3
	sideFour         = 4
	litRow           = 50
	rgbChannels      = 3
	fileMode         = 0o644
	litBandTop       = 4
	litBandBottom    = 6
)

// alphaAt returns the alpha of (x, y) in a: the test's view of Alpha.
func alphaAt(a Alpha, x, y int) uint8 { return a.A[y*a.W+x] }

func filled(w, h int, v uint8) Alpha {
	a := Alpha{W: w, H: h, A: make([]uint8, w*h)}
	for i := range a.A {
		a.A[i] = v
	}
	return a
}

func rows(w, h int, vals ...uint8) Alpha {
	return Alpha{W: w, H: h, A: append([]uint8(nil), vals...)}
}

func TestResizeIsAnExactBoxAverage(t *testing.T) {
	// V4: 4x4 -> 2x2, each cell 2x2 source pixels.
	v4 := rows(sideFour, sideFour,
		alphaZero, alphaZero, alphaFull, alphaFull,
		alphaZero, alphaZero, alphaFull, alphaFull,
		alphaMid, alphaMid, alphaLow, alphaLow,
		alphaMid, alphaMid, alphaLow, alphaLow)
	if got := Resize(v4, sideTwo); !bytes.Equal(got.A, []uint8{alphaZero, alphaFull, alphaMid, alphaLow}) {
		t.Errorf("V4: got %v", got.A)
	}
	// V5: 3x3 -> 2x2, every row [0 90 255]; weights 2,1 and 1,2; D = 9.
	v5 := rows(sideThree, sideThree,
		alphaZero, alphaSmall, alphaFull,
		alphaZero, alphaSmall, alphaFull,
		alphaZero, alphaSmall, alphaFull)
	if got := Resize(v5, sideTwo); !bytes.Equal(got.A, []uint8{alphaOut, alphaHigh, alphaOut, alphaHigh}) {
		t.Errorf("V5: got %v", got.A)
	}
	// V6: rounding (half up) and upscaling.
	if got := Resize(rows(sideTwo, sideTwo, alphaOne, alphaOne, alphaZero, alphaZero), 1); got.A[0] != alphaOne {
		t.Errorf("V6 first: got %d, want 1", got.A[0])
	}
	if got := Resize(rows(sideTwo, sideTwo, alphaZero, alphaOne, alphaZero, alphaZero), 1); got.A[0] != alphaZero {
		t.Errorf("V6 second: got %d, want 0", got.A[0])
	}
	got := Resize(rows(1, 1, alphaSeventySev), sideThree)
	for i, v := range got.A {
		if v != alphaSeventySev {
			t.Fatalf("V6 upscale pixel %d = %d, want %d", i, v, alphaSeventySev)
		}
	}
}

func TestCropBoxAndPadding(t *testing.T) {
	// V7: alpha 200 in x 2..5, y 3..4: bbox (2,3,6,5), box (2,2,6,6).
	v7 := filled(boxSmall, boxSmall, alphaZero)
	for y := bboxY0; y < bboxY1; y++ {
		for x := bboxX0; x < bboxX1; x++ {
			v7.A[y*v7.W+x] = alphaHigh
		}
	}
	x0, y0, x1, y1 := cropBox(v7)
	if x0 != cropVX0 || y0 != cropVY0 || x1 != cropVX1 || y1 != cropVY1 {
		t.Fatalf("V7 box = (%d,%d,%d,%d), want (%d,%d,%d,%d)", x0, y0, x1, y1, cropVX0, cropVY0, cropVX1, cropVY1)
	}
	c := Crop(v7)
	if c.W != cropSide || c.H != cropSide {
		t.Fatalf("V7 crop is %dx%d, want %dx%d", c.W, c.H, cropSide, cropSide)
	}
	for x := 0; x < cropSide; x++ {
		if alphaAt(c, x, 0) != alphaZero || alphaAt(c, x, cropSide-1) != alphaZero {
			t.Fatalf("V7 crop rows 0 and 3 must be alpha 0")
		}
		if alphaAt(c, x, 1) != alphaHigh || alphaAt(c, x, 2) != alphaHigh {
			t.Fatalf("V7 crop rows 1 and 2 must be alpha %d", alphaHigh)
		}
	}

	// V7b: one lit row, box padded by 0.03 of the size: 106x106.
	v7b := filled(boxLarge, boxLarge, alphaZero)
	for x := 0; x < boxLarge; x++ {
		v7b.A[litRow*boxLarge+x] = alphaFull
	}
	x0, y0, x1, y1 = cropBox(v7b)
	if x0 != padCropX0 || y0 != padCropY0 || x1 != padCropX1 || y1 != padCropY1 {
		t.Fatalf("V7b box = (%d,%d,%d,%d), want (%d,%d,%d,%d)", x0, y0, x1, y1, padCropX0, padCropY0, padCropX1, padCropY1)
	}
	if c := Crop(v7b); c.W != boxBigSide || c.H != boxBigSide {
		t.Fatalf("V7b crop is %dx%d, want %dx%d", c.W, c.H, boxBigSide, boxBigSide)
	}

	// V8: a lit band at the top: the box runs above the image, and the crop pads with alpha 0.
	v8 := filled(boxSmall, boxSmall, alphaZero)
	for y := 0; y < sideTwo; y++ {
		for x := 0; x < boxSmall; x++ {
			v8.A[y*boxSmall+x] = alphaFull
		}
	}
	x0, y0, x1, y1 = cropBox(v8)
	if x0 != 0 || y0 != topCropY0 || x1 != boxSmall || y1 != topCropY1 {
		t.Fatalf("V8 box = (%d,%d,%d,%d), want (0,%d,%d,%d)", x0, y0, x1, y1, topCropY0, boxSmall, topCropY1)
	}
	c8 := Crop(v8)
	if c8.W != boxSmall || c8.H != boxSmall {
		t.Fatalf("V8 crop is %dx%d, want %dx%d", c8.W, c8.H, boxSmall, boxSmall)
	}
	for y := 0; y < c8.H; y++ {
		want := uint8(alphaZero)
		if y >= litBandTop && y < litBandBottom {
			want = alphaFull
		}
		if alphaAt(c8, 0, y) != want {
			t.Fatalf("V8 crop row %d alpha %d, want %d", y, alphaAt(c8, 0, y), want)
		}
	}
}

func TestBBoxIgnoresColourUnderZeroAlpha(t *testing.T) {
	// V9: a pixel with RGB (9,9,9) and alpha 0 outside V7's rectangle leaves the bbox where it was.
	img := image.NewNRGBA(image.Rect(0, 0, boxSmall, boxSmall))
	for y := bboxY0; y < bboxY1; y++ {
		for x := bboxX0; x < bboxX1; x++ {
			img.SetNRGBA(x, y, color.NRGBA{A: alphaHigh})
		}
	}
	img.SetNRGBA(0, 0, color.NRGBA{R: rgbPadding, G: rgbPadding, B: rgbPadding, A: alphaZero})
	a, err := DecodeAlpha(encodePNG(t, img))
	if err != nil {
		t.Fatalf("V9 decode: %v", err)
	}
	x0, y0, x1, y1 := cropBox(a)
	if x0 != cropVX0 || y0 != cropVY0 || x1 != cropVX1 || y1 != cropVY1 {
		t.Fatalf("V9 box = (%d,%d,%d,%d), want (%d,%d,%d,%d)", x0, y0, x1, y1, cropVX0, cropVY0, cropVX1, cropVY1)
	}
}

func TestDecodeAlphaRefusesNonRGBA(t *testing.T) {
	grey := image.NewGray(image.Rect(0, 0, fixtureSide, fixtureSide))
	if _, err := DecodeAlpha(encodePNG(t, grey)); err == nil || !strings.Contains(err.Error(), "*image.Gray") {
		t.Fatalf("grey PNG: err = %v, want a refusal naming *image.Gray", err)
	}
}

// fixtureArt draws the eleven icon PNGs of the fixture: each one fully opaque-ish with alpha 40+15*cell, plus
// one transparent pixel so the encoder writes colour type 6.
func fixtureArt(t *testing.T) map[string][]byte {
	t.Helper()
	files := map[string][]byte{}
	for name, cell := range OwnCells() {
		img := image.NewNRGBA(image.Rect(0, 0, fixtureSide, fixtureSide))
		v := uint8(alphaBoxBase + alphaBoxStep*cell)
		for y := 0; y < fixtureSide; y++ {
			for x := 0; x < fixtureSide; x++ {
				img.SetNRGBA(x, y, color.NRGBA{R: alphaFull, G: alphaFull, B: alphaFull, A: v})
			}
		}
		img.SetNRGBA(0, 0, color.NRGBA{A: alphaZero})
		files[IconFiles[name]] = encodePNG(t, img)
	}
	return files
}

func pinsOf(files map[string][]byte) map[string]string {
	pins := map[string]string{}
	for f, b := range files {
		sum := sha256.Sum256(b)
		pins[f] = hex.EncodeToString(sum[:])
	}
	return pins
}

func writeFiles(t *testing.T, files map[string][]byte) string {
	t.Helper()
	dir := t.TempDir()
	for f, b := range files {
		if err := os.WriteFile(filepath.Join(dir, f), b, fileMode); err != nil {
			t.Fatal(err)
		}
	}
	return dir
}

func TestReadArtReturnsTheElevenInOwnCellOrder(t *testing.T) {
	files := fixtureArt(t)
	dir := writeFiles(t, files)
	art, inputs, err := ReadArt(dir, pinsOf(files))
	if err != nil {
		t.Fatalf("ReadArt: %v", err)
	}
	if len(art) != ownCount || len(inputs) != ownCount {
		t.Fatalf("ReadArt returned %d items and %d inputs, want %d", len(art), len(inputs), ownCount)
	}
	for cell := ownFirstCell; cell <= ownLastCell; cell++ {
		var name string
		for n, c := range OwnCells() {
			if c == cell {
				name = n
			}
		}
		if inputs[cell].File != IconFiles[name] {
			t.Errorf("inputs[%d].File = %s, want %s", cell, inputs[cell].File, IconFiles[name])
		}
		if inputs[cell].Sha256 != pinsOf(files)[IconFiles[name]] {
			t.Errorf("inputs[%d].Sha256 does not match its pin", cell)
		}
	}
}

func TestReadArtRefusals(t *testing.T) {
	t.Run("missing file", func(t *testing.T) {
		files := fixtureArt(t)
		pins := pinsOf(files)
		dir := writeFiles(t, files)
		if err := os.Remove(filepath.Join(dir, IconFiles["loc"])); err != nil {
			t.Fatal(err)
		}
		art, inputs, err := ReadArt(dir, pins)
		if err == nil || !strings.Contains(err.Error(), IconFiles["loc"]) || art != nil || inputs != nil {
			t.Fatalf("missing file: art=%v inputs=%v err=%v", art != nil, inputs != nil, err)
		}
	})
	t.Run("flipped byte", func(t *testing.T) {
		files := fixtureArt(t)
		pins := pinsOf(files)
		name := IconFiles["buff"]
		files[name][fixtureFlipAt] ^= alphaFull
		dir := writeFiles(t, files)
		art, inputs, err := ReadArt(dir, pins)
		if err == nil || art != nil || inputs != nil {
			t.Fatalf("flipped byte: art=%v inputs=%v err=%v", art != nil, inputs != nil, err)
		}
		for _, want := range []string{name, pins[name]} {
			if !strings.Contains(err.Error(), want) {
				t.Errorf("error %q does not name %s", err, want)
			}
		}
	})
	t.Run("grey PNG", func(t *testing.T) {
		files := fixtureArt(t)
		grey := image.NewGray(image.Rect(0, 0, fixtureSide, fixtureSide))
		files[IconFiles["smoke"]] = encodePNG(t, grey)
		dir := writeFiles(t, files)
		art, inputs, err := ReadArt(dir, pinsOf(files))
		if err == nil || !strings.Contains(err.Error(), "*image.Gray") || art != nil || inputs != nil {
			t.Fatalf("grey PNG: art=%v inputs=%v err=%v", art != nil, inputs != nil, err)
		}
	})
}

func TestCellsPaintsEachOwnCellAndNothingElse(t *testing.T) {
	art := map[string]Alpha{}
	for name, cell := range OwnCells() {
		art[name] = filled(fixtureSide, fixtureSide, uint8(alphaBoxBase+alphaBoxStep*cell))
	}
	out, err := Cells(rightHalfSide, rightHalfSide, art)
	if err != nil {
		t.Fatalf("Cells: %v", err)
	}
	if len(out) != rightHalfSide*rightHalfSide*channels {
		t.Fatalf("Cells returned %d bytes, want %d", len(out), rightHalfSide*rightHalfSide*channels)
	}
	inSquare := make([]bool, rightHalfSide*rightHalfSide)
	for name, cell := range OwnCells() {
		sq := CellSquare(cell, rightHalfSide, rightHalfSide)
		want := uint8(alphaBoxBase + alphaBoxStep*cell)
		cx, cy := sq.X+sq.Side/sideTwo, sq.Y+sq.Side/sideTwo
		if got := out[(cy*rightHalfSide+cx)*channels+alphaChannel]; got != want {
			t.Errorf("%s: centre alpha %d, want %d", name, got, want)
		}
		for y := sq.Y; y < sq.Y+sq.Side; y++ {
			for x := sq.X; x < sq.X+sq.Side; x++ {
				inSquare[y*rightHalfSide+x] = true
			}
		}
	}
	for p := 0; p < rightHalfSide*rightHalfSide; p++ {
		for c := 0; c < rgbChannels; c++ {
			if out[p*channels+c] != alphaFull {
				t.Fatalf("pixel %d channel %d = %d, want 255", p, c, out[p*channels+c])
			}
		}
		if !inSquare[p] && out[p*channels+alphaChannel] != alphaZero {
			t.Fatalf("pixel %d outside every square has alpha %d", p, out[p*channels+alphaChannel])
		}
	}
	delete(art, "smoke")
	if _, err := Cells(rightHalfSide, rightHalfSide, art); err == nil {
		t.Fatalf("Cells with a missing item: want a refusal")
	}
}

func encodePNG(t *testing.T, img image.Image) []byte {
	t.Helper()
	var buf bytes.Buffer
	if err := png.Encode(&buf, img); err != nil {
		t.Fatal(err)
	}
	return buf.Bytes()
}

// ownArt returns, for every own-cell item, a w by h alpha filled with that item's own value (alphaBoxBase + 15*cell).
func ownArt(w, h int) map[string]Alpha {
	art := map[string]Alpha{}
	for name, cell := range OwnCells() {
		art[name] = filled(w, h, uint8(alphaBoxBase+alphaBoxStep*cell))
	}
	return art
}

func TestCellsRefusesAnEmptyArt(t *testing.T) {
	art := ownArt(fixtureSide, fixtureSide)
	art["smoke"] = Alpha{}
	if _, err := Cells(rightHalfSide, rightHalfSide, art); err == nil || !strings.Contains(err.Error(), "smoke") {
		t.Fatalf("empty art for smoke: err = %v, want a refusal naming smoke", err)
	}
}

func TestCellsRefusesACellWithNoSquare(t *testing.T) {
	// In a 1x1 half, cell 0's square is 0 px (CellSquare), so there is nothing to draw the icon in.
	if _, err := Cells(1, 1, ownArt(fixtureSide, fixtureSide)); err == nil || !strings.Contains(err.Error(), "square is 0") {
		t.Fatalf("1x1 half: err = %v, want a refusal naming the zero square", err)
	}
}

func TestDecodeAlphaRefusesBytesThatAreNotAPNG(t *testing.T) {
	_, err := DecodeAlpha([]byte("this is not a PNG file"))
	var format png.FormatError
	if !errors.As(err, &format) {
		t.Fatalf("garbage bytes: err = %v, want the PNG decoder's FormatError", err)
	}
}

func TestResizeOfAnEmptyAlphaIsTheZeroAlpha(t *testing.T) {
	if got := Resize(Alpha{}, sideThree); got.W != 0 || got.H != 0 || len(got.A) != 0 {
		t.Fatalf("Resize of an empty Alpha = %+v, want the zero Alpha", got)
	}
}

func TestResizeCropAndCellsLeaveTheirInputUnchanged(t *testing.T) {
	// Crop: the V7 fixture, whose pixel (0, 0) lies outside the crop box.
	v7 := filled(boxSmall, boxSmall, alphaZero)
	for y := bboxY0; y < bboxY1; y++ {
		for x := bboxX0; x < bboxX1; x++ {
			v7.A[y*v7.W+x] = alphaHigh
		}
	}
	before := append([]uint8(nil), v7.A...)
	Crop(v7)
	if !bytes.Equal(v7.A, before) {
		t.Errorf("Crop changed its input")
	}

	// Resize: the V5 fixture.
	v5 := rows(sideThree, sideThree,
		alphaZero, alphaSmall, alphaFull,
		alphaZero, alphaSmall, alphaFull,
		alphaZero, alphaSmall, alphaFull)
	before = append([]uint8(nil), v5.A...)
	Resize(v5, sideTwo)
	if !bytes.Equal(v5.A, before) {
		t.Errorf("Resize changed its input")
	}

	// Cells: every own item's art is copied before the call and compared after it.
	art := ownArt(fixtureSide, fixtureSide)
	snap := map[string][]uint8{}
	for name, a := range art {
		snap[name] = append([]uint8(nil), a.A...)
	}
	if _, err := Cells(rightHalfSide, rightHalfSide, art); err != nil {
		t.Fatalf("Cells: %v", err)
	}
	for name, a := range art {
		if !bytes.Equal(a.A, snap[name]) {
			t.Errorf("Cells changed the art of %s", name)
		}
	}
}
