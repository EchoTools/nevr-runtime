package main

import (
	"encoding/binary"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
	"github.com/EchoTools/nevr-runtime/tools/internal/scriptgate"
)

const (
	gateLabel     = "combat_content"
	gateScript    = 0x94ba7ca39fc97e1a // the copy a label's script record names (the 6.18.1 shape: a 4-byte entry)
	gateStock     = 0x190ed14fe6c74dd3 // a script of the stock game
	gateLevel     = 0x10e40287a9bf5a13
	gatePlatform  = "windows-x86_64"
	gateTableHead = 0x38
	gateStockSet  = "48037dc70b0ecab2"
)

func gateRecord(typ, name uint64, data []byte) record {
	return record{label: gateLabel, platform: gatePlatform, typ: scriptgate.Hex(typ), name: scriptgate.Hex(name), align: 16, data: data, source: "test"}
}

// gateTable is a CScriptCR table whose rows name `scripts` at +32.
func gateTable(scripts ...uint64) []byte {
	t := make([]byte, gateTableHead+len(scripts)*scriptgate.RowStride)
	binary.LittleEndian.PutUint64(t[0x08:], uint64(len(scripts)*scriptgate.RowStride))
	binary.LittleEndian.PutUint64(t[0x28:], uint64(len(scripts)))
	binary.LittleEndian.PutUint64(t[0x30:], uint64(len(scripts)))
	for i, s := range scripts {
		binary.LittleEndian.PutUint64(t[gateTableHead+i*scriptgate.RowStride+scriptgate.ScriptOffset:], s)
	}
	return t
}

func TestLabelNamesUndeliveredScript(t *testing.T) {
	t.Parallel()
	cases := []struct {
		name string
		recs []record
		gate scriptgate.Gate
		want []string // substrings of the refusal; none means the label is accepted
	}{
		{"an entry of 4 bytes, no list, no stock", []record{gateRecord(scriptgate.ScriptType, gateScript, []byte{1, 2, 3, 4})}, scriptgate.Gate{},
			[]string{"label " + gateLabel, "reason=script_undelivered", "script=" + scriptgate.Hex(gateScript), "level=none"}},
		{"a record of 0 bytes", []record{gateRecord(scriptgate.ScriptType, gateScript, nil)}, scriptgate.Gate{},
			[]string{"script=" + scriptgate.Hex(gateScript)}},
		{"a row of a level table", []record{gateRecord(scriptgate.TableType, gateLevel, gateTable(gateStock, gateScript))}, scriptgate.Gate{Stock: scriptgate.Stock{gateStock: true}},
			[]string{"label " + gateLabel, "script=" + scriptgate.Hex(gateScript), "level=" + scriptgate.Hex(gateLevel), "CScriptCR row"}},
		{"a script of the stock game", []record{gateRecord(scriptgate.ScriptType, gateStock, []byte{1}), gateRecord(scriptgate.TableType, gateLevel, gateTable(gateStock))},
			scriptgate.Gate{Stock: scriptgate.Stock{gateStock: true}}, nil},
		{"a script the label lists", []record{gateRecord(scriptgate.ScriptType, gateScript, []byte{1}), gateRecord(scriptgate.TableType, gateLevel, gateTable(gateScript))},
			scriptgate.Gate{Listed: map[string]map[scriptgate.Listed]bool{gateLabel: {{Platform: gatePlatform, Name: gateScript}: true}}}, nil},
		{"a script the label lists for the other platform", []record{gateRecord(scriptgate.ScriptType, gateScript, []byte{1}), gateRecord(scriptgate.TableType, gateLevel, gateTable(gateScript))},
			scriptgate.Gate{Listed: map[string]map[scriptgate.Listed]bool{gateLabel: {{Platform: "android-arm64", Name: gateScript}: true}}},
			[]string{"script=" + scriptgate.Hex(gateScript), "level=none", "level=" + scriptgate.Hex(gateLevel)}},
		{"a script another label lists", []record{gateRecord(scriptgate.ScriptType, gateScript, []byte{1})},
			scriptgate.Gate{Listed: map[string]map[scriptgate.Listed]bool{"other": {{Platform: gatePlatform, Name: gateScript}: true}}}, []string{"script=" + scriptgate.Hex(gateScript)}},
		{"a table with no rows", []record{gateRecord(scriptgate.TableType, gateLevel, nil)}, scriptgate.Gate{}, nil},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			t.Parallel()
			_, err := build(c.recs, t.TempDir(), testWindow, quiet(), c.gate)
			if len(c.want) == 0 {
				if err != nil {
					t.Fatalf("refused: %v", err)
				}
				return
			}
			if err == nil {
				t.Fatal("the label was accepted")
			}
			for _, w := range c.want {
				if !strings.Contains(err.Error(), w) {
					t.Errorf("refusal %q lacks %q", err, w)
				}
			}
		})
	}
}

func TestAnUndeliveredScriptWritesNothing(t *testing.T) {
	t.Parallel()
	out := filepath.Join(t.TempDir(), "out")
	recs := []record{gateRecord(scriptgate.ScriptType, gateScript, []byte{1, 2, 3, 4}), gateRecord(1, 2, []byte("poster"))}
	if _, err := build(recs, out, testWindow, quiet(), scriptgate.Gate{}); err == nil {
		t.Fatal("accepted")
	}
	if _, err := os.Stat(out); !os.IsNotExist(err) {
		t.Errorf("a refused set wrote %s: %v", out, err)
	}
}

// The stock flags read the scripts of the stock sets, so a label that names one is accepted through run.
func TestRunReadsTheStockScriptsFromTheStockSets(t *testing.T) {
	t.Parallel()
	data := t.TempDir()
	if err := radpack.WriteStockSet(data, gateStockSet, []map[radpack.Key][]byte{{{Type: asInt64(scriptgate.ScriptType), Name: asInt64(gateStock)}: {1}}}); err != nil {
		t.Fatal(err)
	}
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "s.bin"), nil, 0o644); err != nil {
		t.Fatal(err)
	}
	spec := func(name uint64) string {
		p := filepath.Join(dir, "spec.json")
		body := `{"entries":[{"label":"` + gateLabel + `","platform":"` + gatePlatform + `","type":"` + scriptgate.Hex(scriptgate.ScriptType) +
			`","name":"` + scriptgate.Hex(name) + `","file":"s.bin","align":16}]}`
		if err := os.WriteFile(p, []byte(body), 0o644); err != nil {
			t.Fatal(err)
		}
		return p
	}
	var so, se strings.Builder
	args := func(name uint64, extra ...string) []string {
		return append([]string{"-spec", spec(name), "-out", t.TempDir(), "-max-window", "4096"}, extra...)
	}
	stockArgs := []string{"-stock-data", data, "-stock-set", gateStockSet}
	if code := run(args(gateStock, stockArgs...), &so, &se, quiet()); code != 0 {
		t.Errorf("a stock script: exit %d: %s", code, se.String())
	}
	se.Reset()
	if code := run(args(gateScript, stockArgs...), &so, &se, quiet()); code != 1 || !strings.Contains(se.String(), "script_undelivered") {
		t.Errorf("an undelivered script: exit %d: %s", code, se.String())
	}
	se.Reset()
	if code := run(args(gateStock, "-stock-data", data), &so, &se, quiet()); code != exitUsage {
		t.Errorf("-stock-data alone: exit %d, want %d", code, exitUsage)
	}
}

// asInt64 is a 64-bit name as the stock manifest stores it (signed).
func asInt64(v uint64) int64 { return int64(v) }

// A label's `scripts` list is what the label delivers (PLAN-v5 G2 wired to G3): a record that names a script the list holds, for
// the record's own platform, passes; an entry whose name is not 0x and hex refuses the set, never reads as an empty list.
func TestTheLabelsScriptsListDeliversItsScripts(t *testing.T) {
	t.Parallel()
	recs := []record{gateRecord(scriptgate.ScriptType, gateScript, []byte{1}), gateRecord(scriptgate.TableType, gateLevel, gateTable(gateScript))}
	entry := func(name, platformName string) map[string][]scriptDoc {
		return map[string][]scriptDoc{gateLabel: {{Name: name, Platform: platformName, StockSource: scriptgate.Hex(gateStock), StockSha256: shaOf([]byte("stock"))}}}
	}
	if _, err := buildLabeled(recs, entry(scriptgate.Hex(gateScript), gatePlatform), t.TempDir(), testWindow, quiet(), scriptgate.Gate{}); err != nil {
		t.Errorf("a script the label's list holds: refused: %v", err)
	}
	if _, err := buildLabeled(recs, entry(scriptgate.Hex(gateScript), "android-arm64"), t.TempDir(), testWindow, quiet(), scriptgate.Gate{}); err == nil ||
		!strings.Contains(err.Error(), scriptgate.Reason) {
		t.Errorf("a list entry of the other platform: %v, want %s", err, scriptgate.Reason)
	}
	bare := strings.TrimPrefix(scriptgate.Hex(gateScript), manifestHexPrefix)
	if _, err := buildLabeled(recs, entry(bare, gatePlatform), t.TempDir(), testWindow, quiet(), scriptgate.Gate{}); err == nil ||
		!strings.Contains(err.Error(), "scripts entry "+bare) {
		t.Errorf("a list entry with a bare name: %v, want an error naming it", err)
	}
	// The caller's gate is not changed by the label's list.
	gate := scriptgate.Gate{Listed: map[string]map[scriptgate.Listed]bool{}}
	if _, err := buildLabeled(recs, entry(scriptgate.Hex(gateScript), gatePlatform), t.TempDir(), testWindow, quiet(), gate); err != nil || len(gate.Listed) != 0 {
		t.Errorf("err %v, caller's gate now %v", err, gate.Listed)
	}
}
