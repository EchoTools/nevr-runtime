package gearicons

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"image"
	"image/png"
	"os"
	"path/filepath"
)

// Input is one icon file the art was read from, with the sha-256 it matched (its pin).
type Input struct{ File, Sha256 string }

// Alpha is one icon's alpha channel, row-major, W by H. Only the alpha is kept: the cell texture takes the alpha of
// an icon and nothing else (gear_icons.py:164-165).
type Alpha struct {
	W, H int
	A    []uint8
}

// Art and pixel constants. Each is a fact of the texture format or of Cat's tool (cited where it is used).
const (
	alphaNone    = 0   // a transparent pixel; the bbox takes alpha > 0 (getbbox, gear_icons.py:159)
	alphaOpaque  = 255 // RGB is 255 everywhere in the right half (gear_icons.py:150 and cells_image)
	channels     = 4   // R, G, B, A
	alphaChannel = 3   // index of A within a pixel
	halfDiv      = 2   // rounding: + D/2, and the centre of a box
)

// ownOrder is the own-cell items' names in cell order.
func ownOrder() []string {
	cells := OwnCells()
	order := make([]string, len(cells))
	for name, cell := range cells {
		order[cell] = name
	}
	return order
}

// ReadArt reads the icon file of every own-cell item from dir, in cell order, and checks each one's sha-256 against
// pins (keyed by file name) before it decodes it. A missing file, a different hash (an absent pin counts as
// different), or a PNG that does not decode to 8-bit non-premultiplied RGBA refuses the read: nothing is returned.
// Each icon's alpha comes back in the map by item name; inputs lists the matched files in cell order.
func ReadArt(dir string, pins map[string]string) (map[string]Alpha, []Input, error) {
	order := ownOrder()
	art := make(map[string]Alpha, len(order))
	inputs := make([]Input, 0, len(order))
	for _, name := range order {
		file := IconFiles[name]
		b, err := os.ReadFile(filepath.Join(dir, file))
		if err != nil {
			return nil, nil, fmt.Errorf("gearicons: read %s: %w", file, err)
		}
		sum := sha256.Sum256(b)
		got := hex.EncodeToString(sum[:])
		if want := pins[file]; got != want {
			return nil, nil, fmt.Errorf("gearicons: %s sha256 is %s, the pinned value is %s", file, got, want)
		}
		a, err := DecodeAlpha(b)
		if err != nil {
			return nil, nil, fmt.Errorf("gearicons: %s: %w", file, err)
		}
		art[name] = a
		inputs = append(inputs, Input{File: file, Sha256: got})
	}
	return art, inputs, nil
}

// DecodeAlpha decodes a PNG and keeps its alpha. Only *image.NRGBA (8-bit, non-premultiplied RGBA) is accepted: any
// other decoded type is refused by name.
func DecodeAlpha(b []byte) (Alpha, error) {
	img, err := png.Decode(bytes.NewReader(b))
	if err != nil {
		return Alpha{}, err
	}
	n, ok := img.(*image.NRGBA)
	if !ok {
		return Alpha{}, fmt.Errorf("decoded as %T, want 8-bit non-premultiplied RGBA (*image.NRGBA)", img)
	}
	bounds := n.Bounds()
	a := Alpha{W: bounds.Dx(), H: bounds.Dy(), A: make([]uint8, bounds.Dx()*bounds.Dy())}
	for y := 0; y < a.H; y++ {
		for x := 0; x < a.W; x++ {
			a.A[y*a.W+x] = n.NRGBAAt(bounds.Min.X+x, bounds.Min.Y+y).A
		}
	}
	return a, nil
}

// at is the alpha of (x, y), or alphaNone outside a: a crop pads with zeros.
func at(a Alpha, x, y int) uint8 {
	if x < 0 || y < 0 || x >= a.W || y >= a.H {
		return alphaNone
	}
	return a.A[y*a.W+x]
}

// cropBox is the box Crop takes (gear_icons.py:159-162, Pillow's getbbox and crop, with Python's round). The bbox is
// the smallest rectangle holding every pixel with alpha > 0, the whole image when there is none; the box is that
// bbox's centre with half-size max(w, h) * (0.5 + ICON_PAD), rounded half to even. It may run past the image.
func cropBox(a Alpha) (x0, y0, x1, y1 int) {
	bx0, by0, bx1, by1 := a.W, a.H, alphaNone, alphaNone
	found := false
	for y := 0; y < a.H; y++ {
		for x := 0; x < a.W; x++ {
			if a.A[y*a.W+x] > alphaNone {
				found = true
				bx0, by0 = min(bx0, x), min(by0, y)
				bx1, by1 = max(bx1, x+1), max(by1, y+1)
			}
		}
	}
	if !found {
		bx0, by0, bx1, by1 = 0, 0, a.W, a.H
	}
	half := float64(max(bx1-bx0, by1-by0)) * cropScale
	mx := float64(bx0+bx1) / halfDiv
	my := float64(by0+by1) / halfDiv
	return roundHalfEven(mx - half), roundHalfEven(my - half), roundHalfEven(mx + half), roundHalfEven(my + half)
}

// Crop returns the alpha inside cropBox, padded with alpha 0 where the box runs past the image.
func Crop(a Alpha) Alpha {
	x0, y0, x1, y1 := cropBox(a)
	c := Alpha{W: x1 - x0, H: y1 - y0}
	c.A = make([]uint8, c.W*c.H)
	for y := 0; y < c.H; y++ {
		for x := 0; x < c.W; x++ {
			c.A[y*c.W+x] = at(a, x0+x, y0+y)
		}
	}
	return c
}

// tap is one source pixel's index on one axis and its weight, in units of 1/(n*S) of the axis.
type tap struct{ j, w int }

// axisTaps gives, for each of the n destination pixels of an axis whose source has length s, the source pixels it
// covers and their weights. Destination pixel i covers [i*s, (i+1)*s) and source pixel j covers [j*n, (j+1)*n), so
// the weights of one i sum to s.
func axisTaps(s, n int) [][]tap {
	taps := make([][]tap, n)
	for i := 0; i < n; i++ {
		lo := i * s / n
		hi := ((i+1)*s - 1) / n
		for j := lo; j <= hi; j++ {
			taps[i] = append(taps[i], tap{j: j, w: min((i+1)*s, (j+1)*n) - max(i*s, j*n)})
		}
	}
	return taps
}

// Resize scales a to side by side with an exact box average: out = (sum + D/2) / D with D = W*H, in integers, no
// rounding in between. Exact halves round up. It works for down- and up-scaling. a must hold W*H alpha bytes
// (len(A) == W*H); an empty a (W*H == 0) has no average, so Resize returns the zero Alpha.
func Resize(a Alpha, side int) Alpha {
	d := a.W * a.H
	if d == 0 {
		return Alpha{}
	}
	xs := axisTaps(a.W, side)
	ys := axisTaps(a.H, side)
	out := Alpha{W: side, H: side, A: make([]uint8, side*side)}
	for k := 0; k < side; k++ {
		for i := 0; i < side; i++ {
			sum := 0
			for _, ty := range ys[k] {
				row := 0
				for _, tx := range xs[i] {
					row += tx.w * int(a.A[ty.j*a.W+tx.j])
				}
				sum += ty.w * row
			}
			out.A[k*side+i] = uint8((sum + d/halfDiv) / d)
		}
	}
	return out
}

// Cells returns the right half, w by h RGBA bytes, row major: RGB 255 everywhere, and in each own cell the alpha of
// its item's icon, cropped and resized into the cell's square. Every other pixel has alpha 0 (gear_icons.py:135-172).
// A missing item's art, or an empty one, refuses the call.
func Cells(w, h int, art map[string]Alpha) ([]byte, error) {
	out := make([]byte, w*h*channels)
	for p := 0; p < w*h; p++ {
		out[p*channels], out[p*channels+1], out[p*channels+2] = alphaOpaque, alphaOpaque, alphaOpaque
	}
	for cell, name := range ownOrder() {
		a, ok := art[name]
		if !ok {
			return nil, fmt.Errorf("gearicons: no art for %s (cell %d)", name, cell)
		}
		if a.W <= alphaNone || a.H <= alphaNone {
			return nil, fmt.Errorf("gearicons: art for %s is %dx%d", name, a.W, a.H)
		}
		sq := CellSquare(cell, w, h)
		if sq.Side < 1 {
			return nil, fmt.Errorf("gearicons: cell %d square is %d px in a %dx%d half", cell, sq.Side, w, h)
		}
		icon := Resize(Crop(a), sq.Side)
		for y := 0; y < sq.Side; y++ {
			for x := 0; x < sq.Side; x++ {
				out[((sq.Y+y)*w+sq.X+x)*channels+alphaChannel] = icon.A[y*sq.Side+x]
			}
		}
	}
	return out, nil
}
