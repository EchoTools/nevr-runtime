package main

// The RULES page label rule over synthetic canvases of the shipped text layout (a prefix, then 0x21-byte zero padded text
// slots from "MATCH RULES", then a tail); no byte of the game's is used.

import (
	"bytes"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

var rulesStockTexts = []string{"MATCH RULES", "MINUTES", "05", "SECONDS", "00", "BLUE SCORE", "0", "ORANGE SCORE", "0"}

func rulesCanvas(texts ...string) []byte {
	b := append([]byte("canvas prefix\x01\x02"), bytes.Repeat([]byte{0xAA}, 40)...)
	for _, s := range texts {
		slot := make([]byte, rulesTextSlot)
		copy(slot, s)
		b = append(b, slot...)
	}
	return append(b, []byte("tail bytes")...)
}

func TestTheRulesPageLabelsTheCapturePointsTwoTimes(t *testing.T) {
	in := rulesCanvas(rulesStockTexts...)
	out, err := relabelRulesCanvas(in)
	if err != nil {
		t.Fatal(err)
	}
	if len(out) != len(in) {
		t.Fatalf("length %d, was %d", len(out), len(in))
	}
	base := bytes.Index(in, []byte("MATCH RULES\x00"))
	want := append([]byte(nil), in...)
	for slot, text := range map[int]string{1: "POINT UNLOCK", 3: "CAPTURE TIME"} {
		at := base + slot*rulesTextSlot
		copy(want[at:at+rulesTextSlot], make([]byte, rulesTextSlot))
		copy(want[at:], text)
	}
	if !bytes.Equal(out, want) {
		t.Errorf("canvas\n got %q\nwant %q", out, want)
	}
	if bytes.Equal(in, out) {
		t.Error("the canvas was not changed")
	}
}

func TestARulesCanvasThatIsNotStockIsRefusedAndUnchanged(t *testing.T) {
	stock := append([]string(nil), rulesStockTexts...)
	with := func(slot int, text string) []byte {
		s := append([]string(nil), stock...)
		s[slot] = text
		return rulesCanvas(s...)
	}
	short := rulesCanvas(rulesStockTexts[:4]...) // ends inside slot 3's tail: slot 3 is cut short
	cases := []struct {
		name string
		b    []byte
		want string
	}{
		{"no MATCH RULES text", []byte("a canvas with no rules text at all"), "no \"MATCH RULES\" text slot"},
		{"slot 1 is another text", with(1, "ROUNDS"), `text slot 1`},
		{"slot 3 is another text", with(3, "TICKS"), `text slot 3`},
		{"slot 1 already edited", with(1, "POINT UNLOCK"), `is "POINT UNLOCK", stock is "MINUTES"`},
		{"slot 3 already edited", with(3, "CAPTURE TIME"), `is "CAPTURE TIME", stock is "SECONDS"`},
		{"slot 1 has text past the stock word", with(1, "MINUTES\x00X"), `text slot 1`},
		{"the canvas ends inside the slots", short[:len(short)-len("tail bytes")-0x10], "ends past the canvas"},
	}
	for _, c := range cases {
		in := append([]byte(nil), c.b...)
		out, err := relabelRulesCanvas(in)
		if err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: %v, want an error naming %q", c.name, err, c.want)
		}
		if out != nil || !bytes.Equal(in, c.b) {
			t.Errorf("%s: bytes were returned or changed", c.name)
		}
	}
}

func rulesPack(t *testing.T, sets ...map[radpack.Key][]byte) *Pack {
	t.Helper()
	data := t.TempDir()
	p := &Pack{sets: map[string]*radpack.Set{}, changes: map[string]map[radpack.Key][]byte{}, scripts: map[string][]byte{}}
	for i, res := range sets {
		if err := radpack.WriteStockSet(data, Sets[i], []map[radpack.Key][]byte{res}); err != nil {
			t.Fatal(err)
		}
		s, _, err := radpack.OpenSet(data, Sets[i])
		if err != nil {
			t.Fatal(err)
		}
		p.sets[Sets[i]] = s
	}
	return p
}

func TestTheRulesStepChangesTheCanvasInTheSetThatListsItAndNoOther(t *testing.T) {
	var typ, name uint64 = rulesCanvasType, rulesCanvasName
	key := radpack.Key{Type: int64(typ), Name: int64(name)}
	other := radpack.Key{Type: 5, Name: 5}
	p := rulesPack(t,
		map[radpack.Key][]byte{other: []byte("another resource")},
		map[radpack.Key][]byte{key: rulesCanvas(rulesStockTexts...)})
	if err := rulesPageLabels(p); err != nil {
		t.Fatal(err)
	}
	if len(p.changes[Sets[0]]) != 0 || len(p.changes[Sets[1]]) != 1 {
		t.Fatalf("changes %d in set 0, %d in set 1; want 0 and 1", len(p.changes[Sets[0]]), len(p.changes[Sets[1]]))
	}
	got := p.changes[Sets[1]][key]
	if !bytes.Contains(got, append([]byte("POINT UNLOCK"), 0)) || !bytes.Contains(got, append([]byte("CAPTURE TIME"), 0)) {
		t.Errorf("canvas %q", got)
	}
	// A second run reads the pack's own bytes, which are no longer stock: it is refused.
	if err := rulesPageLabels(p); err == nil || !strings.Contains(err.Error(), "stock is") {
		t.Errorf("a second run: %v", err)
	}
}

func TestTheRulesStepRefusesASetThatHasNoCanvasOrAStockOneThatDiffers(t *testing.T) {
	var typ, name uint64 = rulesCanvasType, rulesCanvasName
	key := radpack.Key{Type: int64(typ), Name: int64(name)}
	none := rulesPack(t, map[radpack.Key][]byte{{Type: 5, Name: 5}: []byte("another resource")})
	if err := rulesPageLabels(none); err == nil || !strings.Contains(err.Error(), "no client set lists canvas") {
		t.Errorf("no canvas: %v", err)
	}
	texts := append([]string(nil), rulesStockTexts...)
	texts[3] = "TICKS"
	differs := rulesPack(t, map[radpack.Key][]byte{key: rulesCanvas(texts...)})
	err := rulesPageLabels(differs)
	if err == nil || !strings.Contains(err.Error(), "b655c5c7eaa080d7") || !strings.Contains(err.Error(), `stock is "SECONDS"`) {
		t.Errorf("a canvas whose slot 3 differs: %v", err)
	}
	if len(differs.changes[Sets[0]]) != 0 {
		t.Error("a refused canvas was put")
	}
}

func TestTheRulesFieldReadsTheTwoSlotsTheStepWrites(t *testing.T) {
	in := rulesCanvas(rulesStockTexts...)
	out, err := relabelRulesCanvas(in)
	if err != nil {
		t.Fatal(err)
	}
	slot := func(s string) []byte { b := make([]byte, rulesTextSlot); copy(b, s); return b }
	for _, c := range []struct {
		canvas []byte
		want   string
	}{{in, "MINUTES/SECONDS"}, {out, "POINT UNLOCK/CAPTURE TIME"}} {
		got, err := readRulesTextSlots(c.canvas)
		words := strings.Split(c.want, "/")
		if want := append(slot(words[0]), slot(words[1])...); err != nil || !bytes.Equal(got, want) {
			t.Errorf("%s: field %q, want %q (%v)", c.want, got, want, err)
		}
	}
	// A byte outside the two slots does not change the field.
	other := append([]byte(nil), out...)
	other[bytes.Index(other, []byte(rulesFirstText))+2*rulesTextSlot] = 'X'
	if a, b := mustRead(t, out), mustRead(t, other); !bytes.Equal(a, b) {
		t.Errorf("slot 2 changed the field: %q, %q", a, b)
	}
	// A canvas of full length whose first text is not MATCH RULES: no slot can be found.
	if _, err := readRulesTextSlots(rulesCanvas(append([]string{"MATCH"}, rulesStockTexts[1:]...)...)); err == nil {
		t.Error("a canvas with no MATCH RULES text was read")
	}
	short := in[:bytes.Index(in, []byte(rulesFirstText))+3*rulesTextSlot]
	if _, err := readRulesTextSlots(short); err == nil {
		t.Error("a canvas that ends before slot 3 was read")
	}
}

func mustRead(t *testing.T, b []byte) []byte {
	t.Helper()
	got, err := readRulesTextSlots(b)
	if err != nil {
		t.Fatal(err)
	}
	return got
}
