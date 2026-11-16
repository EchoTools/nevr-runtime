package scriptgate

import (
	"encoding/binary"
	"strings"
	"testing"
)

const (
	testScript = 0x94ba7ca39fc97e1a
	testLevel  = 0x10e40287a9bf5a13
	// The layout as the EchoCombat pack's attach_point_script reads it, written here apart from the package's constants.
	rowBytes    = 720
	rowScriptAt = 32
)

func table(scripts ...uint64) []byte {
	t := make([]byte, 0x38+len(scripts)*rowBytes)
	binary.LittleEndian.PutUint64(t[0x08:], uint64(len(scripts)*rowBytes))
	binary.LittleEndian.PutUint64(t[0x30:], uint64(len(scripts)))
	for i, s := range scripts {
		binary.LittleEndian.PutUint64(t[0x38+i*rowBytes+rowScriptAt:], s)
	}
	return t
}

func TestRowScriptsReadsTheScriptAtOffset32OfEachRow(t *testing.T) {
	t.Parallel()
	got, err := RowScripts(table(1, 2, 3))
	if err != nil || len(got) != 3 || got[0] != 1 || got[1] != 2 || got[2] != 3 {
		t.Fatalf("got %v, %v", got, err)
	}
}

func TestRowScriptsRefusesATableThatIsNotRowsOf720Bytes(t *testing.T) {
	t.Parallel()
	bad := table(1, 2)
	binary.LittleEndian.PutUint64(bad[countAt:], 3)
	short := table(1)[:0x38+rowBytes-1]
	for name, b := range map[string][]byte{"count 3 over 2 rows": bad, "no header": make([]byte, headerBytes-1), "truncated row": short} {
		if _, err := RowScripts(b); err == nil {
			t.Errorf("%s: accepted", name)
		}
	}
}

func TestACheckNamesTheLevelOfARowAndSkipsAScriptOfZero(t *testing.T) {
	t.Parallel()
	res, err := Gate{}.Check("l", []Record{{Type: TableType, Name: testLevel, Data: table(0, testScript)}})
	if err != nil || len(res.Undelivered) != 1 || res.Undelivered[0] != (Undelivered{Script: testScript, Level: testLevel, Row: true}) {
		t.Fatalf("got %+v, %v", res, err)
	}
	if msg := Refusal("l", res.Undelivered).Error(); !strings.Contains(msg, Hex(testLevel)) {
		t.Errorf("refusal %q does not name the level", msg)
	}
}

func TestATableWithoutItsBytesIsUncheckedNeverDelivered(t *testing.T) {
	t.Parallel()
	res, err := Gate{}.Check("l", []Record{{Type: TableType, Name: testLevel}})
	if err != nil || len(res.Unchecked) != 1 || len(res.Undelivered) != 0 {
		t.Fatalf("got %+v, %v", res, err)
	}
}

func TestARefusalCountsWhatItDoesNotList(t *testing.T) {
	t.Parallel()
	var us []Undelivered
	for i := 0; i < maxListed+3; i++ {
		us = append(us, Undelivered{Script: uint64(i + 1)})
	}
	if msg := Refusal("l", us).Error(); !strings.Contains(msg, "and 3 more") || !strings.Contains(msg, "11 undelivered") {
		t.Errorf("refusal %q", msg)
	}
}

// A list entry delivers its script only to a record of the entry's platform: an android-arm64 entry leaves a windows-x86_64
// script record and a windows-x86_64 table's row of the same script undelivered.
func TestAListEntryDeliversOnlyToARecordOfItsPlatform(t *testing.T) {
	t.Parallel()
	recs := []Record{{Platform: "windows-x86_64", Type: ScriptType, Name: testScript}, {Platform: "windows-x86_64", Type: TableType, Name: testLevel, Data: table(testScript)}}
	other := Gate{Listed: map[string]map[Listed]bool{"l": {{Platform: "android-arm64", Name: testScript}: true}}}
	if res, err := other.Check("l", recs); err != nil || len(res.Undelivered) != 2 {
		t.Errorf("an android-arm64 entry: got %+v, %v; want both undelivered", res, err)
	}
	own := Gate{Listed: map[string]map[Listed]bool{"l": {{Platform: "windows-x86_64", Name: testScript}: true}}}
	if res, err := own.Check("l", recs); err != nil || len(res.Undelivered) != 0 {
		t.Errorf("a windows-x86_64 entry: got %+v, %v; want none undelivered", res, err)
	}
}

// WithListed adds a label's list to what that label delivers, keeps every other label's and the label's own earlier entries,
// and writes nothing into the gate it was called on.
func TestWithListedAddsTheLabelsListAndLeavesTheCallersGate(t *testing.T) {
	const otherScript = testScript + 1
	pc := Listed{Platform: "windows-x86_64", Name: testScript}
	earlier := Listed{Platform: "windows-x86_64", Name: otherScript}
	g := Gate{Listed: map[string]map[Listed]bool{"combat": {earlier: true}, "other": {pc: true}}}
	got := g.WithListed("combat", []Listed{pc})
	if !got.Listed["combat"][pc] || !got.Listed["combat"][earlier] || !got.Listed["other"][pc] {
		t.Errorf("WithListed = %v, want combat {%v %v} and other {%v}", got.Listed, pc, earlier, pc)
	}
	if g.Listed["combat"][pc] || len(g.Listed["combat"]) != 1 {
		t.Errorf("the caller's gate was written: %v", g.Listed)
	}
	if res, err := got.Check("combat", []Record{{Platform: pc.Platform, Type: ScriptType, Name: testScript}}); err != nil || len(res.Undelivered) != 0 {
		t.Errorf("a listed script: %+v, %v", res, err)
	}
}
