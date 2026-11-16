package wwisebank

import (
	"bytes"
	"encoding/hex"
	"errors"
	"strings"
	"testing"
)

// Expected bodies are hand-derived from the children-list layout in PLAN-v5 S-SND section 1.7 and the test vectors of
// section 3. The inputs are synthetic; no byte of a game bank appears here.
const (
	chlParent     = 0x99
	chlMissing    = 0x77
	chlLow        = 0x10
	chlMid        = 0x20
	chlHigh       = 0x30
	chlTypeSound  = 2
	chlTypeOther  = 5
	chlTypeParent = TypeActorMixer
)

// chlParentBody is a parent with the list {0x10, 0x30} at its end (14 bytes).
const chlParentBody = "aabb 02000000 10000000 30000000"

// chlHex decodes a hex literal that may hold spaces.
func chlHex(t *testing.T, s string) []byte {
	t.Helper()
	b, err := hex.DecodeString(strings.ReplaceAll(s, " ", ""))
	if err != nil {
		t.Fatalf("bad hex literal %q: %v", s, err)
	}
	return b
}

// chlObj is an object of the type and id whose body is the hex literal.
func chlObj(t *testing.T, typ uint8, id uint32, body string) Object {
	t.Helper()
	return Object{Type: typ, ID: id, Body: chlHex(t, body)}
}

// chlCloneObjects copies a list, bodies included, so a test can check the input after a call.
func chlCloneObjects(objs []Object) []Object {
	out := make([]Object, len(objs))
	for i, o := range objs {
		out[i] = Object{Type: o.Type, ID: o.ID, Body: append([]byte(nil), o.Body...)}
	}
	return out
}

// chlSameObjects fails the test at the first object that differs, naming it.
func chlSameObjects(t *testing.T, got, want []Object) {
	t.Helper()
	if len(got) != len(want) {
		t.Fatalf("object count: got %d, want %d", len(got), len(want))
	}
	for i := range want {
		if got[i].Type != want[i].Type || got[i].ID != want[i].ID || !bytes.Equal(got[i].Body, want[i].Body) {
			t.Fatalf("object %d:\n got type %d id %#x body %x\nwant type %d id %#x body %x",
				i, got[i].Type, got[i].ID, got[i].Body, want[i].Type, want[i].ID, want[i].Body)
		}
	}
}

func TestAddChildrenInsertsBeforeTheParentAndSorts(t *testing.T) {
	t.Parallel()
	objs := []Object{
		chlObj(t, chlTypeSound, chlLow, "61"),
		chlObj(t, chlTypeSound, chlHigh, "62"),
		chlObj(t, chlTypeParent, chlParent, chlParentBody),
	}
	got, err := AddChildren(objs, chlParent, []Object{chlObj(t, chlTypeSound, chlMid, "6b")})
	if err != nil {
		t.Fatal(err)
	}
	want := []Object{
		chlObj(t, chlTypeSound, chlLow, "61"),
		chlObj(t, chlTypeSound, chlHigh, "62"),
		chlObj(t, chlTypeSound, chlMid, "6b"),
		chlObj(t, chlTypeParent, chlParent, "aabb 03000000 10000000 20000000 30000000"),
	}
	chlSameObjects(t, got, want)
	if enc := EncodeHIRC(got); !bytes.Equal(enc[:4], chlHex(t, "04000000")) {
		t.Fatalf("HIRC count: got %x, want 04000000", enc[:4])
	}
}

func TestAddChildrenTakesTheLastCandidate(t *testing.T) {
	t.Parallel()
	objs := []Object{chlObj(t, chlTypeParent, chlParent,
		"05000000 aaaaaaaa bbbbbbbb cccccccc 01000000 0000007f")}
	got, err := AddChildren(objs, chlParent, []Object{chlObj(t, chlTypeSound, chlLow, "")})
	if err != nil {
		t.Fatal(err)
	}
	want := []Object{
		chlObj(t, chlTypeSound, chlLow, ""),
		chlObj(t, chlTypeParent, chlParent,
			"05000000 aaaaaaaa bbbbbbbb cccccccc 02000000 10000000 0000007f"),
	}
	chlSameObjects(t, got, want)
}

func TestAddChildrenRefusesOtherParents(t *testing.T) {
	t.Parallel()
	objs := []Object{chlObj(t, chlTypeOther, chlParent, chlParentBody)}
	_, err := AddChildren(objs, chlParent, []Object{chlObj(t, chlTypeSound, chlLow, "")})
	if !errors.Is(err, ErrNotActorMixer) {
		t.Fatalf("type 5 parent: err %v, want ErrNotActorMixer", err)
	}
}

func TestAddChildrenRefusesNoListOrUnsorted(t *testing.T) {
	t.Parallel()
	kid := []Object{chlObj(t, chlTypeSound, chlLow, "")}
	cases := map[string]string{
		"count zero":    "01 00000000",
		"not ascending": "aabb 02000000 30000000 10000000",
		"too short":     "01",
	}
	for name, body := range cases {
		objs := []Object{chlObj(t, chlTypeParent, chlParent, body)}
		if _, err := AddChildren(objs, chlParent, kid); !errors.Is(err, ErrNoChildList) {
			t.Errorf("%s: err %v, want ErrNoChildList", name, err)
		}
	}
}

func TestAddChildrenRefusesARepeat(t *testing.T) {
	t.Parallel()
	parent := chlObj(t, chlTypeParent, chlParent, chlParentBody)
	cases := map[string]struct {
		objs []Object
		kids []Object
	}{
		"listed": {
			objs: []Object{parent},
			kids: []Object{chlObj(t, chlTypeSound, chlHigh, "")},
		},
		"object in the bank": {
			objs: []Object{chlObj(t, chlTypeSound, chlLow, "61"), parent},
			kids: []Object{chlObj(t, chlTypeSound, chlLow, "")},
		},
		"two kids with one id": {
			objs: []Object{parent},
			kids: []Object{chlObj(t, chlTypeSound, chlMid, ""), chlObj(t, chlTypeSound, chlMid, "")},
		},
	}
	for name, c := range cases {
		if _, err := AddChildren(c.objs, chlParent, c.kids); !errors.Is(err, ErrChildListed) {
			t.Errorf("%s: err %v, want ErrChildListed", name, err)
		}
	}
}

func TestAddChildrenRefusesAMissingParent(t *testing.T) {
	t.Parallel()
	objs := []Object{chlObj(t, chlTypeParent, chlParent, chlParentBody)}
	_, err := AddChildren(objs, chlMissing, []Object{chlObj(t, chlTypeSound, chlLow, "")})
	if !errors.Is(err, ErrNoObject) {
		t.Fatalf("missing parent: err %v, want ErrNoObject", err)
	}
}

func TestAddChildrenRefusesAnAmbiguousParent(t *testing.T) {
	t.Parallel()
	objs := []Object{
		chlObj(t, chlTypeParent, chlParent, chlParentBody),
		chlObj(t, chlTypeOther, chlParent, chlParentBody),
	}
	_, err := AddChildren(objs, chlParent, []Object{chlObj(t, chlTypeSound, chlMid, "")})
	if !errors.Is(err, ErrAmbiguous) {
		t.Fatalf("two objects of the parent id: err %v, want ErrAmbiguous", err)
	}
}

func TestAddChildrenRefusesNoKids(t *testing.T) {
	t.Parallel()
	objs := []Object{chlObj(t, chlTypeParent, chlParent, chlParentBody)}
	if _, err := AddChildren(objs, chlParent, nil); !errors.Is(err, ErrNoKids) {
		t.Fatalf("no kids: err %v, want ErrNoKids", err)
	}
}

func TestAddChildrenDoesNotChangeItsInput(t *testing.T) {
	t.Parallel()
	objs := []Object{
		chlObj(t, chlTypeSound, chlLow, "61"),
		chlObj(t, chlTypeParent, chlParent, chlParentBody),
	}
	kids := []Object{chlObj(t, chlTypeSound, chlMid, "6b")}
	beforeObjs := chlCloneObjects(objs)
	beforeKids := chlCloneObjects(kids)

	got, err := AddChildren(objs, chlParent, kids)
	if err != nil {
		t.Fatal(err)
	}
	chlSameObjects(t, objs, beforeObjs)
	chlSameObjects(t, kids, beforeKids)

	// The result must not alias the input: writing into it leaves the input as it was.
	for i := range got {
		for j := range got[i].Body {
			got[i].Body[j] = 0xFF
		}
	}
	chlSameObjects(t, objs, beforeObjs)
	chlSameObjects(t, kids, beforeKids)
}

func TestAddChildrenRefusalsLeaveTheInputAlone(t *testing.T) {
	t.Parallel()
	objs := []Object{chlObj(t, chlTypeParent, chlParent, "01 00000000")}
	before := chlCloneObjects(objs)
	if _, err := AddChildren(objs, chlParent, []Object{chlObj(t, chlTypeSound, chlLow, "")}); !errors.Is(err, ErrNoChildList) {
		t.Fatalf("err %v, want ErrNoChildList", err)
	}
	chlSameObjects(t, objs, before)
}
