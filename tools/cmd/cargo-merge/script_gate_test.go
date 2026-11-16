package main

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/klauspost/compress/zstd"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
	"github.com/EchoTools/nevr-runtime/tools/internal/scriptgate"
)

const (
	gateScript   = 0x94ba7ca39fc97e1a
	gateStock    = 0x190ed14fe6c74dd3
	gateLevel    = 0x10e40287a9bf5a13
	gateStockSet = "48037dc70b0ecab2"
	gateHead     = 0x38
)

// gateBase is a base manifest with one poster label; the added manifests below are built around it.
const gateBase = `{"schema":"nevr-cargo-manifest","schema_version":1,"labels":{"lobby_posters":{"entries":[]}},"empty":[]}`

func gateAdd(entries, empties string) string {
	return `{"schema":"nevr-cargo-manifest","schema_version":1,"labels":{"combat_content":{"entries":[` + entries + `],"empty":[` + empties + `]}},"empty":[]}`
}

func gateEntry(typ, name uint64, sha string) string {
	return fmt.Sprintf(`{"platform":"windows-x86_64","type":%q,"name":%q,"format":"zstd-frame","sha256":%q,"size":1,"bytes":1,"align":16}`,
		scriptgate.Hex(typ), scriptgate.Hex(name), sha)
}

func gateEmpty(typ, name uint64) string {
	return fmt.Sprintf(`{"platform":"windows-x86_64","type":%q,"name":%q,"align":16}`, scriptgate.Hex(typ), scriptgate.Hex(name))
}

func gateMerge(t *testing.T, add string, gate scriptgate.Gate, blobs string) (*mergeResult, error) {
	t.Helper()
	bf, bs := frameOf(t, gateBase)
	af, as := frameOf(t, add)
	return mergeExcluding(bf, bs, af, as, nil, scriptCheck(gate, blobs, false, quiet()))
}

func gateTable(scripts ...uint64) []byte {
	tb := make([]byte, gateHead+len(scripts)*scriptgate.RowStride)
	binary.LittleEndian.PutUint64(tb[0x08:], uint64(len(scripts)*scriptgate.RowStride))
	binary.LittleEndian.PutUint64(tb[0x28:], uint64(len(scripts)))
	binary.LittleEndian.PutUint64(tb[0x30:], uint64(len(scripts)))
	for i, s := range scripts {
		binary.LittleEndian.PutUint64(tb[gateHead+i*scriptgate.RowStride+scriptgate.ScriptOffset:], s)
	}
	return tb
}

// gateBlob writes the table as blobs/sha256/<hash of its frame> and returns the folder and the hash.
func gateBlob(t *testing.T, table []byte) (string, string) {
	t.Helper()
	enc, err := zstd.NewWriter(nil)
	if err != nil {
		t.Fatal(err)
	}
	defer enc.Close()
	frame := enc.EncodeAll(table, nil)
	dir := t.TempDir()
	hash := sha256Hex(frame)
	if err := os.MkdirAll(filepath.Join(dir, "blobs", "sha256"), dirPerm); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "blobs", "sha256", hash), frame, filePerm); err != nil {
		t.Fatal(err)
	}
	return dir, hash
}

func TestLabelNamesUndeliveredScript(t *testing.T) {
	t.Parallel()
	zero := strings.Repeat("0", sha256HexLen)
	stock := scriptgate.Gate{Stock: scriptgate.Stock{gateStock: true}}
	dir, hash := gateBlob(t, gateTable(gateStock, gateScript))
	cases := []struct {
		name   string
		add    string
		gate   scriptgate.Gate
		blobs  string
		want   []string
		wantOK bool
	}{
		{name: "a 4-byte entry of the 6.18.1 shape", add: gateAdd(gateEntry(scriptgate.ScriptType, gateScript, zero), ""),
			want: []string{"label combat_content", "script=" + scriptgate.Hex(gateScript), "level=none", "script_undelivered"}},
		{name: "a record of 0 bytes", add: gateAdd("", gateEmpty(scriptgate.ScriptType, gateScript)),
			want: []string{"script=" + scriptgate.Hex(gateScript)}},
		{name: "a row of a level table, read from its blob", add: gateAdd(gateEntry(scriptgate.TableType, gateLevel, hash), ""), gate: stock, blobs: dir,
			want: []string{"script=" + scriptgate.Hex(gateScript), "level=" + scriptgate.Hex(gateLevel)}},
		{name: "a stock script", add: gateAdd(gateEntry(scriptgate.ScriptType, gateStock, zero), gateEmpty(scriptgate.ScriptType, gateStock+1)), gate: scriptgate.Gate{Stock: scriptgate.Stock{gateStock: true, gateStock + 1: true}}, wantOK: true},
		{name: "a script the label lists", add: gateAdd(gateEntry(scriptgate.ScriptType, gateScript, zero), ""),
			gate: scriptgate.Gate{Listed: map[string]map[scriptgate.Listed]bool{"combat_content": {{Platform: "windows-x86_64", Name: gateScript}: true}}}, wantOK: true},
		{name: "a record of 0 bytes the label lists", add: gateAdd("", gateEmpty(scriptgate.ScriptType, gateScript)),
			gate: scriptgate.Gate{Listed: map[string]map[scriptgate.Listed]bool{"combat_content": {{Platform: "windows-x86_64", Name: gateScript}: true}}}, wantOK: true},
		{name: "a script the label lists for the other platform", add: gateAdd(gateEntry(scriptgate.ScriptType, gateScript, zero), ""),
			gate: scriptgate.Gate{Listed: map[string]map[scriptgate.Listed]bool{"combat_content": {{Platform: "android-arm64", Name: gateScript}: true}}},
			want: []string{"script=" + scriptgate.Hex(gateScript), "script_undelivered"}},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			t.Parallel()
			res, err := gateMerge(t, c.add, c.gate, c.blobs)
			if c.wantOK {
				if err != nil {
					t.Fatalf("refused: %v", err)
				}
				if res == nil {
					t.Fatal("no result")
				}
				return
			}
			if err == nil {
				t.Fatal("the label was merged")
			}
			for _, w := range c.want {
				if !strings.Contains(err.Error(), w) {
					t.Errorf("refusal %q lacks %q", err, w)
				}
			}
		})
	}
}

// A level table whose blob is not supplied cannot be read: the merge refuses, naming it, unless -allow-unchecked-tables is
// given; then it merges, lists the table in the summary and writes a Warning naming it, and never counts its rows as delivered.
func TestLevelTablesWithoutBlobsRefuseUnlessAllowed(t *testing.T) {
	t.Parallel()
	zero := strings.Repeat("0", sha256HexLen)
	want := "combat_content/" + scriptgate.Hex(gateLevel)
	dir := t.TempDir()
	write := func(name string, b []byte) string {
		p := filepath.Join(dir, name)
		if err := os.WriteFile(p, b, filePerm); err != nil {
			t.Fatal(err)
		}
		return p
	}
	bf, bs := frameOf(t, gateBase)
	af, as := frameOf(t, gateAdd(gateEntry(scriptgate.TableType, gateLevel, zero), ""))
	basePath, addPath := write("base", bf), write("add", af)
	try := func(extra ...string) (int, string, string, string) {
		var so, se, logged bytes.Buffer
		args := append([]string{"-base", basePath, "-base-sha256", bs, "-add", addPath, "-add-sha256", as, "-out", filepath.Join(t.TempDir(), "merged")}, extra...)
		code := run(args, &so, &se, slog.New(slog.NewJSONHandler(&logged, nil)))
		return code, so.String(), se.String(), logged.String()
	}
	code, _, se, _ := try()
	if code != exitRefused || !strings.Contains(se, want) || !strings.Contains(se, "-allow-unchecked-tables") {
		t.Errorf("no -add-blobs: exit %d, stderr %q; want a refusal naming %s", code, se, want)
	}
	code, so, se, logged := try("-allow-unchecked-tables")
	if code != 0 {
		t.Fatalf("-allow-unchecked-tables: exit %d: %s", code, se)
	}
	if !strings.Contains(so, `"script_tables_unchecked":["`+want+`"]`) {
		t.Errorf("summary %q does not list %s", so, want)
	}
	if !strings.Contains(logged, `"level":"WARN"`) || !strings.Contains(logged, want) {
		t.Errorf("run log %q has no Warning naming %s", logged, want)
	}
}

func TestARefusedMergeWritesNothingAndRunReadsTheStockFlags(t *testing.T) {
	t.Parallel()
	data := t.TempDir()
	if err := radpack.WriteStockSet(data, gateStockSet, []map[radpack.Key][]byte{{{Type: asInt64(scriptgate.ScriptType), Name: asInt64(gateStock)}: {1}}}); err != nil {
		t.Fatal(err)
	}
	dir := t.TempDir()
	bf, bs := frameOf(t, gateBase)
	write := func(name string, b []byte) string {
		p := filepath.Join(dir, name)
		if err := os.WriteFile(p, b, filePerm); err != nil {
			t.Fatal(err)
		}
		return p
	}
	zero := strings.Repeat("0", sha256HexLen)
	basePath := write("base", bf)
	try := func(script uint64) (int, string, string) {
		af, as := frameOf(t, gateAdd(gateEntry(scriptgate.ScriptType, script, zero), ""))
		out := filepath.Join(t.TempDir(), "merged")
		var so, se bytes.Buffer
		code := run([]string{"-base", basePath, "-base-sha256", bs, "-add", write(fmt.Sprintf("add-%x", script), af), "-add-sha256", as,
			"-stock-data", data, "-stock-set", gateStockSet, "-out", out}, &so, &se, quiet())
		return code, se.String(), out
	}
	if code, se, _ := try(gateStock); code != 0 {
		t.Errorf("a stock script: exit %d: %s", code, se)
	}
	code, se, out := try(gateScript)
	if code != exitRefused || !strings.Contains(se, "script_undelivered") {
		t.Errorf("an undelivered script: exit %d: %s", code, se)
	}
	if _, err := os.Stat(out); !os.IsNotExist(err) {
		t.Errorf("a refused merge wrote %s: %v", out, err)
	}
}

func asInt64(v uint64) int64 { return int64(v) }

// gateAddListed is gateAdd with the label's `scripts` list.
func gateAddListed(entries, scripts string) string {
	return `{"schema":"nevr-cargo-manifest","schema_version":1,"labels":{"combat_content":{"entries":[` + entries + `],"scripts":[` + scripts + `]}},"empty":[]}`
}

func gateListEntry(name, platformName string) string {
	return fmt.Sprintf(`{"name":%q,"platform":%q,"stock_source":%q,"stock_sha256":%q}`, name, platformName, scriptgate.Hex(gateStock), strings.Repeat("d", sha256HexLen))
}

// A label's `scripts` list (PLAN-v5 G2 wired to G3) delivers its scripts to records of the entry's platform, and the merged
// label keeps the list, byte for byte and in order, so the runtime is given the list the check counted on.
func TestTheLabelsScriptsListDeliversAndIsKept(t *testing.T) {
	t.Parallel()
	zero := strings.Repeat("0", sha256HexLen)
	entry := gateEntry(scriptgate.ScriptType, gateScript, zero)
	list := gateListEntry(scriptgate.Hex(gateScript), "windows-x86_64") + "," + gateListEntry(scriptgate.Hex(gateScript), "android-arm64")
	res, err := gateMerge(t, gateAddListed(entry, list), scriptgate.Gate{}, "")
	if err != nil {
		t.Fatalf("a script the label's list holds: refused: %v", err)
	}
	var top struct {
		Labels map[string]struct {
			Scripts json.RawMessage `json:"scripts"`
		} `json:"labels"`
	}
	if err := json.Unmarshal(res.Text, &top); err != nil {
		t.Fatal(err)
	}
	if got := string(top.Labels["combat_content"].Scripts); got != "["+list+"]" {
		t.Errorf("merged scripts = %s, want [%s]", got, list)
	}
	if _, err := gateMerge(t, gateAddListed(entry, gateListEntry(scriptgate.Hex(gateScript), "android-arm64")), scriptgate.Gate{}, ""); err == nil ||
		!strings.Contains(err.Error(), scriptgate.Reason) {
		t.Errorf("a list entry of the other platform: %v, want %s", err, scriptgate.Reason)
	}
}

// A list entry the merge cannot read refuses the merge: the list is never read as empty.
func TestAMalformedScriptsEntryRefusesTheMerge(t *testing.T) {
	t.Parallel()
	entry := gateEntry(scriptgate.ScriptType, gateScript, strings.Repeat("0", sha256HexLen))
	bare := strings.TrimPrefix(scriptgate.Hex(gateScript), "0x")
	cases := map[string]string{
		"a bare name":    gateListEntry(bare, "windows-x86_64"),
		"no platform":    gateListEntry(scriptgate.Hex(gateScript), ""),
		"an unknown key": strings.TrimSuffix(gateListEntry(scriptgate.Hex(gateScript), "windows-x86_64"), "}") + `,"edits":[]}`,
	}
	for what, list := range cases {
		gate := scriptgate.Gate{Listed: map[string]map[scriptgate.Listed]bool{"combat_content": {{Platform: "windows-x86_64", Name: gateScript}: true}}}
		if _, err := gateMerge(t, gateAddListed(entry, list), gate, ""); err == nil {
			t.Errorf("%s: merged", what)
		}
	}
}
