package main

// The stage82 oracle (PLAN-v5 10.3, piece G-ORACLE): each step's declared fields (Step.Writes), compared with the same
// records of he_is_the_cat's EchoCombat build (nevr-pack 82ad7ba, its stage82 output: one file per resource,
// `<dir>/<type in hex>/<name in hex>`, the resource's bytes). His output is read as data, only for the fields a step
// declares; nothing of it is copied into this tree (ADR 0028, ADR 0035 rule 4: the directory is an environment input,
// never pinned or shipped).

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

const (
	stage82Env   = "NEVR_CAT_STAGE82_DIR"
	stockDataEnv = "NEVR_STOCK_DATA"
)

// oracleInputs are the two directories the oracle needs, or the reason it does not run: it is skipped without the
// stage82 directory, and refused (not skipped) with it but no stock data, since the steps have nothing to run over.
func oracleInputs(getenv func(string) string) (stage, stock, skip string, err error) {
	stage, stock = getenv(stage82Env), getenv(stockDataEnv)
	if stage == "" {
		return "", "", stage82Env + " is not set (his stage82 output: <type>/<name> files)", nil
	}
	if stock == "" {
		return "", "", "", fmt.Errorf("%s is set but %s is not: the steps need a stock PC data folder to run over", stage82Env, stockDataEnv)
	}
	return stage, stock, "", nil
}

// oracleDiffs runs list over p, then compares each declared field of each step with his record of the same name. A
// difference, a missing record or a record a reader cannot read is one line naming the step, the record and the field.
func oracleDiffs(list []Step, p *Pack, stage string) ([]string, error) {
	for _, st := range list {
		if err := st.Apply(p); err != nil {
			return nil, fmt.Errorf("step %s: %w", st.Name, err)
		}
	}
	var diffs []string
	for _, st := range list {
		for _, f := range st.Writes {
			for _, rec := range f.Records {
				where := fmt.Sprintf("step %q record %016x.%016x field %q", st.Name, f.Type, rec, f.Name)
				ours, err := p.Read(f.Set, int64(f.Type), int64(rec))
				if err != nil {
					diffs = append(diffs, where+": ours cannot be read: "+err.Error())
					continue
				}
				mine, err := f.Read(ours)
				if err != nil {
					diffs = append(diffs, where+": ours cannot be read: "+err.Error())
					continue
				}
				theirs, err := os.ReadFile(filepath.Join(stage, fmt.Sprintf("%x", f.Type), fmt.Sprintf("%x", rec)))
				if errors.Is(err, fs.ErrNotExist) {
					diffs = append(diffs, where+": the stage82 output has no such record")
					continue
				} else if err != nil {
					return nil, err
				}
				his, err := f.Read(theirs)
				if err != nil {
					diffs = append(diffs, where+": his cannot be read: "+err.Error())
					continue
				}
				if !bytes.Equal(mine, his) {
					diffs = append(diffs, fmt.Sprintf("%s: differs at byte %d (ours %d bytes, his %d bytes)", where, firstDiff(mine, his), len(mine), len(his)))
				}
			}
		}
	}
	return diffs, nil
}

func firstDiff(a, b []byte) int {
	i := 0
	for i < len(a) && i < len(b) && a[i] == b[i] {
		i++
	}
	return i
}

func TestStepsMatchStage82(t *testing.T) {
	t.Parallel()
	stage, stock, skip, err := oracleInputs(os.Getenv)
	if err != nil {
		t.Fatal(err)
	}
	if skip != "" {
		t.Skip(skip)
	}
	s, _, err := radpack.OpenSet(stock, Sets[0])
	if err != nil {
		t.Fatal(err)
	}
	p := &Pack{gameDir: filepath.Join(stock, "..", "..", "..", ".."), sets: map[string]*radpack.Set{Sets[0]: s},
		changes: map[string]map[radpack.Key][]byte{}, scripts: map[string][]byte{}}
	diffs, err := oracleDiffs(steps, p, stage)
	if err != nil {
		t.Fatal(err)
	}
	for _, d := range diffs {
		t.Error(d)
	}
}

func TestTheOracleIsSkippedWithoutItsDirectoryAndRefusedWithoutStockData(t *testing.T) {
	t.Parallel()
	env := func(m map[string]string) func(string) string { return func(k string) string { return m[k] } }
	if _, _, skip, err := oracleInputs(env(map[string]string{stockDataEnv: "/stock"})); skip == "" || err != nil || !strings.Contains(skip, stage82Env) {
		t.Errorf("unset: skip %q, err %v", skip, err)
	}
	if _, _, skip, err := oracleInputs(env(map[string]string{stage82Env: "/s"})); skip != "" || err == nil {
		t.Errorf("no stock data: skip %q, err %v", skip, err)
	}
	if stage, stock, skip, err := oracleInputs(env(map[string]string{stage82Env: "/s", stockDataEnv: "/d"})); stage != "/s" || stock != "/d" || skip != "" || err != nil {
		t.Errorf("both set: %q %q %q %v", stage, stock, skip, err)
	}
}

// A synthetic pack: one stock record (type 2, name 7) of 16 bytes, one step that writes bytes 4..8 and declares only them.
const (
	oracleType, oracleName = 2, 7
	fieldFrom, fieldTo     = 4, 8
)

func oraclePack(t *testing.T) (*Pack, []Step) {
	t.Helper()
	data := filepath.Join(t.TempDir(), filepath.FromSlash(DataDir))
	if err := radpack.WriteStockSet(data, Sets[0], []map[radpack.Key][]byte{{{Type: oracleType, Name: oracleName}: make([]byte, 16)}}); err != nil {
		t.Fatal(err)
	}
	s, _, err := radpack.OpenSet(data, Sets[0])
	if err != nil {
		t.Fatal(err)
	}
	p := &Pack{sets: map[string]*radpack.Set{Sets[0]: s}, changes: map[string]map[radpack.Key][]byte{}, scripts: map[string][]byte{}}
	list := []Step{{
		Name: "set four bytes",
		Apply: func(p *Pack) error {
			b, err := p.Read(Sets[0], oracleType, oracleName)
			if err != nil {
				return err
			}
			copy(b[fieldFrom:fieldTo], []byte{1, 2, 3, 4})
			return p.Put(Sets[0], oracleType, oracleName, b)
		},
		Writes: []Field{{Name: "bytes 4..8", Set: Sets[0], Type: oracleType, Records: []uint64{oracleName},
			Read: func(b []byte) ([]byte, error) { return b[fieldFrom:fieldTo], nil }}},
	}}
	return p, list
}

func stageWith(t *testing.T, record []byte) string {
	t.Helper()
	dir := t.TempDir()
	if record != nil {
		path := filepath.Join(dir, fmt.Sprintf("%x", oracleType), fmt.Sprintf("%x", oracleName))
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, record, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	return dir
}

func TestAPlantedDifferenceInADeclaredFieldIsNamed(t *testing.T) {
	t.Parallel()
	same := make([]byte, 16)
	copy(same[fieldFrom:], []byte{1, 2, 3, 4})

	// His record equals ours in the declared field and differs everywhere else: not the step's claim, so no difference.
	outside := append([]byte(nil), same...)
	outside[0], outside[fieldTo], outside[15] = 9, 9, 9
	p, list := oraclePack(t)
	if diffs, err := oracleDiffs(list, p, stageWith(t, outside)); err != nil || len(diffs) != 0 {
		t.Errorf("a difference outside the declared field: %v, %v", diffs, err)
	}

	// One byte planted inside the declared field.
	planted := append([]byte(nil), same...)
	planted[fieldFrom+2] = 0xee
	p, list = oraclePack(t)
	diffs, err := oracleDiffs(list, p, stageWith(t, planted))
	if err != nil || len(diffs) != 1 {
		t.Fatalf("planted difference: %v, %v", diffs, err)
	}
	for _, want := range []string{`step "set four bytes"`, "0000000000000002.0000000000000007", `field "bytes 4..8"`, "differs at byte 2"} {
		if !strings.Contains(diffs[0], want) {
			t.Errorf("%q does not name %q", diffs[0], want)
		}
	}

	// His output has no such record.
	p, list = oraclePack(t)
	diffs, err = oracleDiffs(list, p, stageWith(t, nil))
	if err != nil || len(diffs) != 1 || !strings.Contains(diffs[0], "no such record") {
		t.Errorf("absent record: %v, %v", diffs, err)
	}

	// His record is too short for the reader: named, not a panic.
	p, list = oraclePack(t)
	list[0].Writes[0].Read = func(b []byte) ([]byte, error) {
		if len(b) < fieldTo {
			return nil, errors.New("too short")
		}
		return b[fieldFrom:fieldTo], nil
	}
	diffs, err = oracleDiffs(list, p, stageWith(t, same[:fieldFrom]))
	if err != nil || len(diffs) != 1 || !strings.Contains(diffs[0], "his cannot be read: too short") {
		t.Errorf("unreadable record: %v, %v", diffs, err)
	}
}

// shippedFields: the three drone steps, the drone health step, the DOT stick step, the two DOT constraint steps, the two
// DOT FIELD red bubble model and transform steps, its texture-streaming step and the RULES label step declare one field
// each, the equipment step one per list, the DOT preset step one per activation key (3), the DOT dampener step one per
// table (2), the DOT FIELD red materials step one per material (2), and the two OVERCHARGE steps one each (the buff
// tac-mod entry, the OVERCHARGE bubble rows).
const shippedFields = 22

func TestTheDeclaredFieldsOfTheShippedStepsAreWellFormed(t *testing.T) {
	t.Parallel()
	declared := 0
	for _, st := range steps {
		if len(st.Writes) == 0 {
			t.Errorf("step %q declares no field: the oracle would compare nothing it writes", st.Name)
		}
		for _, f := range st.Writes {
			declared++
			if f.Name == "" || f.Read == nil || len(f.Records) == 0 || (f.Set != Sets[0] && f.Set != Sets[1]) || f.Type == 0 {
				t.Errorf("step %q field %+v is not complete", st.Name, f)
			}
		}
	}
	if declared < shippedFields {
		t.Errorf("%d fields declared over %d steps, want at least %d", declared, len(steps), shippedFields)
	}
}

func TestTheDroneFieldsReadWhatTheirStepsWrite(t *testing.T) {
	t.Parallel()
	const size = 136
	bullets := parse(t, table(size, nil,
		row(size, map[int]uint64{0: 1, keyOff: assaultBullet, effectsOff: 0xaaaa, effectsOff + 8: 0xbbbb}),
		row(size, map[int]uint64{0: 3, keyOff: droneBullet, effectsOff: 1, effectsOff + 8: 2})))
	if err := droneBulletEffects(bullets); err != nil {
		t.Fatal(err)
	}
	got, err := readDroneBulletEffects(bullets.Bytes())
	if err != nil || len(got) != effectsLen || radpack.U64(got, 0) != 0xaaaa || radpack.U64(got, 8) != 0xbbbb {
		t.Errorf("effects %x, %v", got, err)
	}

	particles := parse(t, table(size, nil,
		row(size, map[int]uint64{0: 1, keyOff: assaultBullet}), row(size, map[int]uint64{0: 2, keyOff: 0x5555})))
	if err := droneParticleRows(particles); err != nil {
		t.Fatal(err)
	}
	rows, err := readRowsWhere(keyOff, droneBullet)(particles.Bytes())
	if want := particles.Rows[2]; err != nil || !bytes.Equal(rows, want) {
		t.Errorf("particle rows %x, want %x (%v)", rows, want, err)
	}

	scripts := parse(t, table(size, nil,
		row(size, map[int]uint64{0: sharedScript, keyOff: 0x77}), row(size, map[int]uint64{0: 5, keyOff: droneActor})))
	if err := droneScriptRow(scripts); err != nil {
		t.Fatal(err)
	}
	rows, err = readScriptRowsOfDrone(scripts.Bytes())
	if want := scripts.Rows[1]; err != nil || !bytes.Equal(rows, want) || radpack.U64(rows, 0) != sharedScript {
		t.Errorf("script rows %x, want %x (%v)", rows, want, err)
	}
	// A table with no such row reads as no bytes for the particle and script fields, and is refused for the bullet one.
	empty := table(size, nil, row(size, map[int]uint64{0: 1, keyOff: 2}))
	if b, err := readRowsWhere(keyOff, droneBullet)(empty); err != nil || len(b) != 0 {
		t.Errorf("no drone particle rows: %x, %v", b, err)
	}
	if _, err := readDroneBulletEffects(empty); err == nil {
		t.Error("a table with no drone bullet row was read")
	}
}

func TestTheEquipmentFieldsReadOnlyTheAddedEntries(t *testing.T) {
	t.Parallel()
	in := jsonResource(`{"gear_table":{"weapons":[{"name":"w1","display_name":"W1"}],"ordnances":[{"name":"o1","display_name":"O1"}],"tacmods":["x"]}}`)
	out, err := appendGear(in)
	if err != nil {
		t.Fatal(err)
	}
	weapons, err := readAddedGear("weapons")(out)
	want := `{"name":"magnum","display_name":"PROTOSTAR"}` + "\n" + `{"name":"smg","display_name":"QUARK"}` + "\n" +
		`{"name":"chain","display_name":"BLAZAR"}` + "\n" + `{"name":"rifle","display_name":"STARBURST"}` + "\n"
	if err != nil || string(weapons) != want {
		t.Errorf("weapons %q, %v", weapons, err)
	}
	ordnances, err := readAddedGear("ordnances")(out)
	if err != nil || strings.Contains(string(ordnances), "o1") || strings.Count(string(ordnances), "\n") != 3 {
		t.Errorf("ordnances %q, %v", ordnances, err)
	}
	if _, err := readAddedGear("nothing")(out); err == nil {
		t.Error("a list the table lacks was read")
	}
	if _, err := readAddedGear("weapons")(in[:jsonHeader-1]); err == nil {
		t.Error("a short resource was read")
	}
	wrongLength := append([]byte(nil), out...)
	binary.LittleEndian.PutUint64(wrongLength[jsonLengthOff:], uint64(len(out)-jsonHeader-1))
	if _, err := readAddedGear("weapons")(wrongLength); err == nil {
		t.Error("a resource whose header length is not its body's was read")
	}
}
