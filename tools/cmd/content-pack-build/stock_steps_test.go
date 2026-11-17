package main

import (
	"cmp"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"maps"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

// The EchoCombat steps against the shipped data, when a machine has it: NEVR_STOCK_DATA names a stock PC data folder
// (`_data/5932408047/rad15/win10` of an install). Each step's output, per level, must be byte for byte the combat gun
// patch's table that he_is_the_cat's pack starts from; only those files' sha-256 values are kept here, no bytes.
// Skipped without the variable.
var gunPatchTables = map[uint64]map[uint64]string{
	jsonResourceType: {equipmentName: "3ae2cbc89ab64daa9b6989eb23feeb45ce9e0b549aa684ba79f974ad3b42f57d"},
	bulletTableType: {
		0x42670f2bed45703c: "0f8c755be03f720edd229a4d4627facc91a295a6c6336447c3f3c85456aa13c6",
		0x43e2da7914642604: "0f8c755be03f720edd229a4d4627facc91a295a6c6336447c3f3c85456aa13c6",
		0x43e2da7a0c623a19: "0f8c755be03f720edd229a4d4627facc91a295a6c6336447c3f3c85456aa13c6",
		0xcb9977f7fc2b4526: "0f8c755be03f720edd229a4d4627facc91a295a6c6336447c3f3c85456aa13c6",
		0xdf5ca7b7dfa383d4: "0f8c755be03f720edd229a4d4627facc91a295a6c6336447c3f3c85456aa13c6",
	},
	particleTableType: {
		0x42670f2bed45703c: "a207cf39b6665642b2cc39851652e7e6e36fc8fb130a475bda911b617ffd4349",
		0x43e2da7914642604: "a207cf39b6665642b2cc39851652e7e6e36fc8fb130a475bda911b617ffd4349",
		0x43e2da7a0c623a19: "7e3cf1acb3ac7d3696e66e8b4e9ae0d1aa158a723f415cc1188a0aa85b08db2a",
		0xcb9977f7fc2b4526: "a207cf39b6665642b2cc39851652e7e6e36fc8fb130a475bda911b617ffd4349",
		0xdf5ca7b7dfa383d4: "a207cf39b6665642b2cc39851652e7e6e36fc8fb130a475bda911b617ffd4349",
	},
	scriptTable: {
		0x42670f2bed45703c: "d3e95f391a72390fa901cc6ccb2e5c070a992779516de61bcd94dc6797589228",
		0x43e2da7914642604: "0144a28b54260ff82e78fc8a8d8bcbd374ad52f3a91b5557313a4c2b5eec05a0",
		0x43e2da7a0c623a19: "86f09e0a297cf1c2cea39c1c8f2841d0814cb6ed8e2563166ed4e59c0e21b122",
		0xcb9977f7fc2b4526: "8c1cb90d102aa94d9e381c6286c5d64685c45480844798786403d11dba3ef652",
		0xdf5ca7b7dfa383d4: "03c1f3e492286d4a0babe4a34b832f38661b8272cf9188f20742c5847cf72593",
	},
}

// The resources the steps change are the records their declared fields name (Step.Writes): checkChanged compares them with
// what a run really changed, so a step that changes a record it does not declare, or declares one it does not change,
// fails there.

// gearIconsEnv names the folder of the gear icons' art (he_is_the_cat's ICONS/hd) for the tests that run every production step
// over the stock data. The gear icons step refuses without it, so a run without it leaves that step out of the list it applies
// and checks (stockSteps) and says so.
const gearIconsEnv = "NEVR_GEAR_ICONS_DIR"

// stockSteps is the production list for a test that applies it over the stock data: the whole list when gearIconsEnv is set,
// else the list without the gear icons step.
func stockSteps(t *testing.T) []Step {
	t.Helper()
	list := mustProductionSteps(t)
	if os.Getenv(gearIconsEnv) != "" {
		return list
	}
	t.Logf("%s is not set: the %q step is left out (it refuses without the art)", gearIconsEnv, gearIconsStepName)
	return slices.DeleteFunc(slices.Clone(list), func(st Step) bool { return st.Name == gearIconsStepName })
}

// newStockPack opens both client sets of the stock data as Build does, with nothing changed yet.
func newStockPack(t *testing.T, data string) *Pack {
	t.Helper()
	p := &Pack{gameDir: filepath.Join(data, "..", "..", "..", ".."), sets: map[string]*radpack.Set{},
		changes: map[string]map[radpack.Key][]byte{}, scripts: map[string][]byte{}, gearIconsDir: os.Getenv(gearIconsEnv)}
	for _, setID := range Sets { // both client sets, as Build opens them: a step may change a resource in either
		s, _, err := radpack.OpenSet(data, setID)
		if err != nil {
			t.Fatal(err)
		}
		p.sets[setID] = s
	}
	return p
}

func TestTheEchoCombatStepsReproduceTheGunPatchFilesFromTheShippedData(t *testing.T) {
	data := os.Getenv("NEVR_STOCK_DATA")
	if data == "" {
		t.Skip("NEVR_STOCK_DATA is not set (a stock PC data folder: manifests/ and packages/)")
	}
	p := newStockPack(t, data)
	// The gun patch's mp_equipment has no OVERCHARGE, NEUTRON or UMBRAL VEIL (the steps that are PastGunPatch add them), and
	// its four map levels' script tables have no map buttons (steps_mapterm.go adds them), so its sha-256 values are
	// compared before those steps.
	for _, st := range stockSteps(t) {
		if st.PastGunPatch {
			continue
		}
		if err := st.Apply(p); err != nil {
			t.Fatalf("%s: %v", st.Name, err)
		}
	}
	for typ, levels := range gunPatchTables {
		for lvl, want := range levels {
			b, ok := p.changes[Sets[0]][radpack.Key{Type: int64(typ), Name: int64(lvl)}]
			if !ok {
				t.Errorf("%016x.%016x was not changed", typ, lvl)
				continue
			}
			sum := sha256.Sum256(b)
			if got := hex.EncodeToString(sum[:]); got != want {
				t.Errorf("%016x.%016x: sha256 %s, the gun patch's is %s", typ, lvl, got, want)
			}
		}
	}
}

// TestTheChangedResourcesAreTheDeclaredOnes is the independent check of the declarations: the expected set is what each
// step's Writes name, the actual set is what Apply changed over the player's stock data, and checkChanged compares them.
func TestTheChangedResourcesAreTheDeclaredOnes(t *testing.T) {
	data := os.Getenv("NEVR_STOCK_DATA")
	if data == "" {
		t.Skip("NEVR_STOCK_DATA is not set (a stock PC data folder: manifests/ and packages/)")
	}
	p := newStockPack(t, data)
	for _, st := range stockSteps(t) {
		if err := st.Apply(p); err != nil {
			t.Fatalf("%s: %v", st.Name, err)
		}
	}
	if err := checkChanged(stockSteps(t), p.changes); err != nil {
		t.Error(err)
	}
	distinct := map[radpack.Key]bool{}
	for _, setID := range Sets {
		for k := range p.changes[setID] {
			distinct[k] = true
		}
	}
	t.Logf("%d resources changed", len(distinct))
}

// declaredResources is the records the steps say they change: every (type, record) of every declared field. required are
// the ones some non-Optional field names; optional the ones only Optional fields name. The set of a field is not part of
// the key (a client set holds the RULES canvas in whichever set lists it, while its field says Sets[0]). names[key] are the
// steps that declare the key, for messages.
func declaredResources(list []Step) (required, optional map[radpack.Key]bool, names map[radpack.Key][]string) {
	required, optional, names = map[radpack.Key]bool{}, map[radpack.Key]bool{}, map[radpack.Key][]string{}
	for _, st := range list {
		for _, f := range st.Writes {
			for _, r := range f.Records {
				k := radpack.Key{Type: int64(f.Type), Name: int64(r)}
				if f.Optional {
					optional[k] = true
				} else {
					required[k] = true
				}
				if !slices.Contains(names[k], st.Name) {
					names[k] = append(names[k], st.Name)
				}
			}
		}
	}
	return required, optional, names
}

// checkChanged compares what a run changed (p.changes) with declaredResources(list). One joined error naming each of:
//
//	"%016x.%016x changed in set %s and no step declares it"                         (changed, not required, not optional)
//	"%016x.%016x is declared by %v and was not changed"                              (required, not changed in any set)
//	"%016x.%016x changed in more than one client set (%v)"                           (a key in two sets)
//
// nil when none.
func checkChanged(list []Step, changes map[string]map[radpack.Key][]byte) error {
	required, optional, names := declaredResources(list)
	changedIn := map[radpack.Key][]string{}
	for _, set := range slices.Sorted(maps.Keys(changes)) {
		for k := range changes[set] {
			changedIn[k] = append(changedIn[k], set)
		}
	}
	var errs []error
	for _, k := range slices.SortedFunc(maps.Keys(changedIn), lessKey) {
		sets := changedIn[k]
		if len(sets) > 1 {
			errs = append(errs, fmt.Errorf("%s changed in more than one client set (%v)", keyText(k), sets))
		}
		if !required[k] && !optional[k] {
			errs = append(errs, fmt.Errorf("%s changed in set %s and no step declares it", keyText(k), sets[0]))
		}
	}
	for _, k := range slices.SortedFunc(maps.Keys(required), lessKey) {
		if _, ok := changedIn[k]; !ok {
			errs = append(errs, fmt.Errorf("%s is declared by %v and was not changed", keyText(k), names[k]))
		}
	}
	return errors.Join(errs...)
}

// keyText is a resource as the messages write it: its type and its record, in hex.
func keyText(k radpack.Key) string {
	return fmt.Sprintf("%016x.%016x", uint64(k.Type), uint64(k.Name))
}

// lessKey orders resources by type, then record, so the messages come in one order.
func lessKey(a, b radpack.Key) int {
	return cmp.Or(cmp.Compare(a.Type, b.Type), cmp.Compare(a.Name, b.Name))
}

// The vectors of checkChanged: three steps, one of them with an Optional field, over two record types and two records.
const (
	tA, tB, tC = 0xa, 0xb, 0xc
	rOne, rTwo = 1, 2
	stepOne    = "S1"
	stepTwo    = "S2"
	stepThree  = "S3"
)

func TestCheckChanged(t *testing.T) {
	key := func(typ, rec uint64) radpack.Key { return radpack.Key{Type: int64(typ), Name: int64(rec)} }
	text := func(typ, rec uint64) string { return fmt.Sprintf("%016x.%016x", typ, rec) }
	s1 := Step{Name: stepOne, Writes: []Field{{Name: "a", Set: Sets[0], Type: tA, Records: []uint64{rOne, rTwo}}}}
	s2 := Step{Name: stepTwo, Writes: []Field{
		{Name: "a again", Set: Sets[0], Type: tA, Records: []uint64{rTwo}},
		{Name: "b, optional", Set: Sets[0], Type: tB, Records: []uint64{rOne}, Optional: true},
	}}
	s3 := Step{Name: stepThree, Writes: []Field{{Name: "b", Set: Sets[0], Type: tB, Records: []uint64{rOne}}}}
	list12, list123 := []Step{s1, s2}, []Step{s1, s2, s3}
	ch := func(by map[string][]radpack.Key) map[string]map[radpack.Key][]byte {
		out := map[string]map[radpack.Key][]byte{}
		for set, keys := range by {
			out[set] = map[radpack.Key][]byte{}
			for _, k := range keys {
				out[set][k] = []byte{}
			}
		}
		return out
	}
	a1, a2, b1, c1 := key(tA, rOne), key(tA, rTwo), key(tB, rOne), key(tC, rOne)
	cases := []struct {
		name    string
		list    []Step
		changes map[string]map[radpack.Key][]byte
		want    []string // substrings of the error; none means nil
	}{
		{"V1 the optional record absent", list12, ch(map[string][]radpack.Key{Sets[0]: {a1, a2}}), nil},
		{"V2 the optional record present", list12, ch(map[string][]radpack.Key{Sets[0]: {a1, a2, b1}}), nil},
		{"V3 an undeclared record", list12, ch(map[string][]radpack.Key{Sets[0]: {a1, a2, c1}}),
			[]string{text(tC, rOne), "no step declares it"}},
		{"V4 a declared record not changed", list12, ch(map[string][]radpack.Key{Sets[0]: {a1}}),
			[]string{text(tA, rTwo), stepOne, "was not changed"}},
		{"V5 a record in two client sets", list12, ch(map[string][]radpack.Key{Sets[0]: {a1, a2}, Sets[1]: {a1}}),
			[]string{"more than one client set"}},
		{"V6 a required record of a later step not changed", list123, ch(map[string][]radpack.Key{Sets[0]: {a1, a2}}),
			[]string{text(tB, rOne), stepThree, "was not changed"}},
		{"V7 the set is not part of the key", list12, ch(map[string][]radpack.Key{Sets[1]: {a1, a2}}), nil},
		{"V8 nothing changed", list12, ch(map[string][]radpack.Key{}),
			[]string{text(tA, rOne), text(tA, rTwo)}},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			err := checkChanged(c.list, c.changes)
			if len(c.want) == 0 {
				if err != nil {
					t.Errorf("want nil, got %v", err)
				}
				return
			}
			if err == nil {
				t.Fatalf("want an error naming %q, got nil", c.want)
			}
			for _, w := range c.want {
				if !strings.Contains(err.Error(), w) {
					t.Errorf("error %q does not name %q", err, w)
				}
			}
		})
	}
}
