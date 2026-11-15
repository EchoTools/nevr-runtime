package main

import (
	"bytes"
	"encoding/json"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/klauspost/compress/zstd"
)

const (
	// prodSha and combatSha are the sha256 of the two frames in testdata: the production manifest cargo-publish serves
	// at v1/manifest.json.zst, and the staging manifest of the EchoCombat set (6f6d9f17..., named by its own hash).
	prodSha     = "cc5efb560bb57b1fcb1f391aad5ab941aaf4922a078547d1bd6f52e5e968c232"
	combatSha   = "6f6d9f170128180ee2599b0775cc99ee324abdbb711cf605ecce91dc50754e6a"
	prodFile    = "testdata/production-manifest.json.zst"
	combatFile  = "testdata/combat-manifest.json.zst"
	combatLabel = "combat_content"
	baseLabel   = "lobby_posters"
	platformPC  = "windows-x86_64"
	// platformQuest is the Quest build's platform.
	platformQuest = "android-arm64"
	// combatEmpties is the number of records the combat manifest lists as empty (its top-level empty list).
	combatEmpties = 275
	// combatEntries is the number of entries the combat label holds, which the merge must leave as they are.
	combatEntries = 3677
	// emptyTypeCombatMost is the type of the largest group of combat empties (242 of the 275).
	emptyTypeCombatMost      = "0x7b9a4c9fd03f37eb"
	emptyTypeCombatMostCount = 242
	// baseLabelEntries is the number of entries lobby_posters holds in the production manifest.
	baseLabelEntries = 11
	hexDigits        = 64
	// scriptRecordType is CScriptResource: the combat label's script records are undelivered (no stock set, no scripts list), so a
	// run over the real manifest drops them to pass the script_undelivered check. Its level tables' blobs are not in testdata,
	// so the run also passes allowUnchecked (-allow-unchecked-tables).
	scriptRecordType = "0x991421b41de58370"
	allowUnchecked   = "-allow-unchecked-tables"
	badSha           = "0000000000000000000000000000000000000000000000000000000000000000"
)

func quiet() *slog.Logger { return slog.New(slog.NewJSONHandler(io.Discard, nil)) }

// readFile returns the testdata file, or fails the test.
func readFile(t *testing.T, name string) []byte {
	t.Helper()
	b, err := os.ReadFile(name)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

// frameOf encodes text as a zstd frame and returns the frame with its sha256, as a manifest file on disk would have.
func frameOf(t *testing.T, text string) ([]byte, string) {
	t.Helper()
	enc, err := zstd.NewWriter(nil)
	if err != nil {
		t.Fatal(err)
	}
	defer enc.Close()
	frame := enc.EncodeAll([]byte(text), nil)
	return frame, sha256Hex(frame)
}

// textOf decodes a zstd frame, for checking the merged manifest.
func textOf(t *testing.T, frame []byte) []byte {
	t.Helper()
	dec, err := zstd.NewReader(nil)
	if err != nil {
		t.Fatal(err)
	}
	defer dec.Close()
	out, err := dec.DecodeAll(frame, nil)
	if err != nil {
		t.Fatal(err)
	}
	return out
}

// rawLabel returns the exact bytes of one label's value in a manifest text, found by a token walk over the labels object.
func rawLabel(t *testing.T, text []byte, name string) []byte {
	t.Helper()
	var top struct {
		Labels map[string]json.RawMessage `json:"labels"`
	}
	if err := json.Unmarshal(text, &top); err != nil {
		t.Fatal(err)
	}
	raw, ok := top.Labels[name]
	if !ok {
		t.Fatalf("no label %s", name)
	}
	return raw
}

// labelOrder lists the labels of a manifest text in the order they appear in the document.
func labelOrder(t *testing.T, text []byte) []string {
	t.Helper()
	var order []string
	at := bytes.Index(text, []byte(`"labels":{`))
	if at < 0 {
		t.Fatal("no labels object")
	}
	d := json.NewDecoder(bytes.NewReader(text[at+len(`"labels":`):]))
	if _, err := d.Token(); err != nil {
		t.Fatal(err)
	}
	for d.More() {
		k, err := d.Token()
		if err != nil {
			t.Fatal(err)
		}
		order = append(order, k.(string))
		var skip json.RawMessage
		if err := d.Decode(&skip); err != nil {
			t.Fatal(err)
		}
	}
	return order
}

func TestMergeKeepsBaseLabelsFirstAndByteIdentical(t *testing.T) {
	t.Parallel()
	baseFrame := readFile(t, prodFile)
	addFrame := readFile(t, combatFile)
	res, err := merge(baseFrame, prodSha, addFrame, combatSha)
	if err != nil {
		t.Fatal(err)
	}
	baseText := textOf(t, baseFrame)
	mergedText := textOf(t, res.Frame)

	// lobby_posters is byte-identical to its value in the production manifest, and it is the first label.
	if want, got := rawLabel(t, baseText, baseLabel), rawLabel(t, mergedText, baseLabel); !bytes.Equal(want, got) {
		t.Fatalf("lobby_posters changed: %d bytes became %d", len(want), len(got))
	}
	if order := labelOrder(t, mergedText); len(order) != mergedLabelCount || order[0] != baseLabel || order[1] != combatLabel {
		t.Fatalf("label order = %v, want [%s %s]", order, baseLabel, combatLabel)
	}
	if !bytes.Contains(mergedText, []byte(`"labels":{"`+baseLabel+`":`)) {
		t.Fatal("the merged labels object does not open with lobby_posters")
	}
	if res.Sha256 != sha256Hex(res.Frame) {
		t.Errorf("reported sha256 %s, frame hashes to %s", res.Sha256, sha256Hex(res.Frame))
	}
}

func TestMergeMovesTheCombatEmptiesUnderCombatContent(t *testing.T) {
	t.Parallel()
	res, err := merge(readFile(t, prodFile), prodSha, readFile(t, combatFile), combatSha)
	if err != nil {
		t.Fatal(err)
	}
	var doc struct {
		Labels map[string]struct {
			Entries []json.RawMessage `json:"entries"`
			Empty   []struct {
				Platform string `json:"platform"`
				Type     string `json:"type"`
				Name     string `json:"name"`
				Align    uint32 `json:"align"`
			} `json:"empty"`
		} `json:"labels"`
		Empty json.RawMessage `json:"empty"`
	}
	if err := json.Unmarshal(textOf(t, res.Frame), &doc); err != nil {
		t.Fatal(err)
	}
	combat := doc.Labels[combatLabel]
	if len(combat.Empty) != combatEmpties {
		t.Fatalf("combat_content holds %d empties, want %d", len(combat.Empty), combatEmpties)
	}
	byType := map[string]int{}
	for _, e := range combat.Empty {
		if e.Platform != platformPC {
			t.Fatalf("empty %s %s has platform %q, want %s", e.Type, e.Name, e.Platform, platformPC)
		}
		byType[e.Type]++
	}
	if byType[emptyTypeCombatMost] != emptyTypeCombatMostCount {
		t.Errorf("type %s: %d empties, want %d", emptyTypeCombatMost, byType[emptyTypeCombatMost], emptyTypeCombatMostCount)
	}
	if len(combat.Entries) != combatEntries {
		t.Errorf("combat_content holds %d entries, want %d", len(combat.Entries), combatEntries)
	}
	if len(doc.Labels[baseLabel].Entries) != baseLabelEntries {
		t.Errorf("lobby_posters holds %d entries, want %d", len(doc.Labels[baseLabel].Entries), baseLabelEntries)
	}
	if string(doc.Empty) != "[]" {
		t.Errorf("top-level empty = %s, want []", doc.Empty)
	}
}

func TestMergeRefusesAShaMismatchOnEitherInput(t *testing.T) {
	t.Parallel()
	base, add := readFile(t, prodFile), readFile(t, combatFile)
	if _, err := merge(base, badSha, add, combatSha); err == nil || !strings.Contains(err.Error(), "sha256") {
		t.Errorf("base with a wrong pin: err = %v", err)
	}
	if _, err := merge(base, prodSha, add, badSha); err == nil || !strings.Contains(err.Error(), "sha256") {
		t.Errorf("add with a wrong pin: err = %v", err)
	}
	// A changed byte inside the frame is refused by its hash, before anything is decoded.
	flipped := append([]byte(nil), add...)
	flipped[len(flipped)/halfDivisor] ^= corruptMask
	if _, err := merge(base, prodSha, flipped, combatSha); err == nil {
		t.Error("a changed frame was merged")
	}
}

// The base's combat_content holds PC records (CModelCRWin10, CGLightMapResourceWin10GPU); the added manifest's holds Quest ones
// (CModelCRAndroid 0x042c7bbb8da4211c, CGLightMapResourceAndroidGPU 0x8cce0f0b134fc0ed, and one record
// whose type and name are a PC record's, on the other platform). The two are one label.
const (
	joinBase = `{"schema":"nevr-cargo-manifest","schema_version":1,"labels":{` +
		`"lobby_posters":{"entries":[{"platform":"windows-x86_64","type":"0x4a4c32c49300b8a0","name":"0x0000000000000001"}]},` +
		`"combat_content":{"entries":[{"platform":"windows-x86_64","type":"0xea51a0d76eb90142","name":"0x00000000000000c1"}],` +
		`"empty":[{"platform":"windows-x86_64","type":"0x7b9a4c9fd03f37eb","name":"0x00000000000000e1","align":16}]}},"empty":[]}`
	joinAdd = `{"schema":"nevr-cargo-manifest","schema_version":1,"labels":{` +
		`"combat_content":{"entries":[{"platform":"android-arm64","type":"0x042c7bbb8da4211c","name":"0x00000000000000c1"},` +
		`{"platform":"android-arm64","type":"0xea51a0d76eb90142","name":"0x00000000000000c1"}],` +
		`"empty":[{"platform":"android-arm64","type":"0x8cce0f0b134fc0ed","name":"0x00000000000000e1","align":16}]},` +
		`"quest_only":{"entries":[{"platform":"android-arm64","type":"0x489bb35d53ca50e9","name":"0x0000000000000002"}]}},"empty":[]}`
	joinedEntries = 3 // the base's one and the added two
	joinedEmpty   = 2 // one per platform
)

func TestASharedLabelIsJoinedAndHoldsBothPlatforms(t *testing.T) {
	t.Parallel()
	base, bsha := frameOf(t, joinBase)
	add, asha := frameOf(t, joinAdd)
	res, err := merge(base, bsha, add, asha)
	if err != nil {
		t.Fatalf("a label in both manifests was refused: %v", err)
	}
	if len(res.Joined) != 1 || res.Joined[0] != combatLabel {
		t.Errorf("joined = %v, want [%s]", res.Joined, combatLabel)
	}
	if got := labelOrder(t, res.Text); strings.Join(got, ",") != "lobby_posters,combat_content,quest_only" {
		t.Errorf("label order = %v: the joined label keeps the base's place, the added manifest's own label follows", got)
	}
	if !bytes.Equal(rawLabel(t, res.Text, baseLabel), rawLabel(t, []byte(joinBase), baseLabel)) {
		t.Error("a base label the added manifest does not carry changed its bytes")
	}
	var joined struct {
		Entries []json.RawMessage `json:"entries"`
		Empty   []emptyDoc        `json:"empty"`
	}
	if err := json.Unmarshal(rawLabel(t, res.Text, combatLabel), &joined); err != nil {
		t.Fatal(err)
	}
	if len(joined.Entries) != joinedEntries || len(joined.Empty) != joinedEmpty {
		t.Fatalf("joined label holds %d entries and %d empty records, want %d and %d", len(joined.Entries), len(joined.Empty), joinedEntries, joinedEmpty)
	}
	var baseBody, addBody struct {
		Entries []json.RawMessage `json:"entries"`
	}
	if err := json.Unmarshal(rawLabel(t, []byte(joinBase), combatLabel), &baseBody); err != nil {
		t.Fatal(err)
	}
	if err := json.Unmarshal(rawLabel(t, []byte(joinAdd), combatLabel), &addBody); err != nil {
		t.Fatal(err)
	}
	want := append(append([]json.RawMessage{}, baseBody.Entries...), addBody.Entries...)
	for i := range want {
		if !bytes.Equal(joined.Entries[i], want[i]) {
			t.Errorf("joined entry %d = %s, want %s (the base's first, then the added ones, each as it was)", i, joined.Entries[i], want[i])
		}
	}
	if joined.Empty[0].Platform != platformPC || joined.Empty[1].Platform != platformQuest {
		t.Errorf("joined empty records = %+v: each keeps its own platform, the base's first", joined.Empty)
	}
	for _, r := range res.Labels {
		if r.Name == combatLabel && (r.Entries != joinedEntries || r.Empty != joinedEmpty) {
			t.Errorf("report for %s = %+v", combatLabel, r)
		}
	}
}

func TestAJoinedLabelRefusesAKeyHeldTwiceOnOnePlatform(t *testing.T) {
	t.Parallel()
	base, bsha := frameOf(t, joinBase)
	for name, text := range map[string]string{
		"an entry":            `{"schema":"nevr-cargo-manifest","schema_version":1,"labels":{"combat_content":{"entries":[{"platform":"windows-x86_64","type":"0xea51a0d76eb90142","name":"0x00000000000000c1"}]}},"empty":[]}`,
		"a record of 0 bytes": `{"schema":"nevr-cargo-manifest","schema_version":1,"labels":{"combat_content":{"entries":[{"platform":"windows-x86_64","type":"0xea51a0d76eb90142","name":"0x00000000000000c2"}],"empty":[{"platform":"windows-x86_64","type":"0x7b9a4c9fd03f37eb","name":"0x00000000000000e1","align":16}]}},"empty":[]}`,
	} {
		add, asha := frameOf(t, text)
		if _, err := merge(base, bsha, add, asha); err == nil || !strings.Contains(err.Error(), "listed twice") {
			t.Errorf("%s the base's label already holds on its platform: err = %v", name, err)
		}
	}
}

// A joined label's `scripts` list is the base's then the added one's, each entry as it was; a script both list on one platform
// is refused (the reader would refuse the label whole as duplicate), and the same name on the other platform is not.
const (
	joinScriptPC    = `{"name":"0x00000000000000c1","platform":"windows-x86_64","stock_source":"0x190ed14fe6c74dd3","stock_sha256":"` + joinStockSha + `"}`
	joinScriptQuest = `{"name":"0x00000000000000c1","platform":"android-arm64","stock_source":"0x190ed14fe6c74dd3","stock_sha256":"` + joinStockSha + `"}`
	joinStockSha    = "1111111111111111111111111111111111111111111111111111111111111111"
	joinScriptsBase = `{"schema":"nevr-cargo-manifest","schema_version":1,"labels":{"combat_content":{"entries":[{"platform":"windows-x86_64","type":"0xea51a0d76eb90142","name":"0x00000000000000c1"}],"scripts":[` + joinScriptPC + `]}},"empty":[]}`
)

func joinScriptsAdd(script string) string {
	return `{"schema":"nevr-cargo-manifest","schema_version":1,"labels":{"combat_content":{"entries":[{"platform":"android-arm64","type":"0x042c7bbb8da4211c","name":"0x00000000000000c1"}],"scripts":[` + script + `]}},"empty":[]}`
}

func TestAJoinedLabelHoldsBothScriptsListsAndRefusesAScriptListedTwice(t *testing.T) {
	t.Parallel()
	base, bsha := frameOf(t, joinScriptsBase)
	add, asha := frameOf(t, joinScriptsAdd(joinScriptQuest))
	res, err := merge(base, bsha, add, asha)
	if err != nil {
		t.Fatalf("a joined label whose lists name the script on two platforms was refused: %v", err)
	}
	var joined struct {
		Scripts []json.RawMessage `json:"scripts"`
	}
	if err := json.Unmarshal(rawLabel(t, res.Text, combatLabel), &joined); err != nil {
		t.Fatal(err)
	}
	if len(joined.Scripts) != 2 || string(joined.Scripts[0]) != joinScriptPC || string(joined.Scripts[1]) != joinScriptQuest {
		t.Errorf("joined scripts = %s, want the base's entry then the added one, each as it was", joined.Scripts)
	}
	add, asha = frameOf(t, joinScriptsAdd(joinScriptPC))
	if _, err := merge(base, bsha, add, asha); err == nil || !strings.Contains(err.Error(), "list it twice") {
		t.Errorf("a script both halves list on one platform: err = %v", err)
	}
}

func TestMergeRefusesACollidingRecordKey(t *testing.T) {
	t.Parallel()
	base, bsha := frameOf(t, `{"schema":"nevr-cargo-manifest","schema_version":1,"labels":{"lobby_posters":{"entries":[{"platform":"windows-x86_64","type":"0x0000000000000001","name":"0x0000000000000001"}]}},"empty":[]}`)
	add, asha := frameOf(t, `{"schema":"nevr-cargo-manifest","schema_version":1,"labels":{"combat_content":{"entries":[{"platform":"windows-x86_64","type":"0x0000000000000001","name":"0x0000000000000001"}]}},"empty":[]}`)
	if _, err := merge(base, bsha, add, asha); err == nil || !strings.Contains(err.Error(), "0x0000000000000001") {
		t.Errorf("a record key in both: err = %v", err)
	}
	// An empty record of the added set collides with an entry of the base on the same platform, type and name.
	add2, asha2 := frameOf(t, `{"schema":"nevr-cargo-manifest","schema_version":1,"labels":{"combat_content":{"entries":[{"platform":"windows-x86_64","type":"0x0000000000000002","name":"0x0000000000000002"}]}},"empty":[{"type":"0x0000000000000001","name":"0x0000000000000001","align":16}]}`)
	if _, err := merge(base, bsha, add2, asha2); err == nil {
		t.Error("an added empty colliding with a base entry was merged")
	}
}

func TestMergeRefusesTopLevelEmptiesThatHaveNoOneLabel(t *testing.T) {
	t.Parallel()
	base, bsha := frameOf(t, `{"schema":"nevr-cargo-manifest","schema_version":1,"labels":{"lobby_posters":{"entries":[]}},"empty":[]}`)
	add, asha := frameOf(t, `{"schema":"nevr-cargo-manifest","schema_version":1,"labels":{"a":{"entries":[{"platform":"windows-x86_64","type":"0x0000000000000002","name":"0x0000000000000002"}]},"b":{"entries":[{"platform":"windows-x86_64","type":"0x0000000000000003","name":"0x0000000000000003"}]}},"empty":[{"type":"0x0000000000000009","name":"0x0000000000000009","align":16}]}`)
	if _, err := merge(base, bsha, add, asha); err == nil || !strings.Contains(err.Error(), "one label") {
		t.Errorf("top-level empties with two labels: err = %v", err)
	}
}

func TestMergeRefusesAnUnknownTopLevelKey(t *testing.T) {
	t.Parallel()
	base, bsha := frameOf(t, `{"schema":"nevr-cargo-manifest","schema_version":1,"labels":{"lobby_posters":{"entries":[]}},"empty":[],"sets":[]}`)
	add, asha := frameOf(t, `{"schema":"nevr-cargo-manifest","schema_version":1,"labels":{"combat_content":{"entries":[]}},"empty":[]}`)
	if _, err := merge(base, bsha, add, asha); err == nil || !strings.Contains(err.Error(), "sets") {
		t.Errorf("an unknown key in the base: err = %v", err)
	}
}

func TestRunWritesTheManifestAndPrintsItsSha(t *testing.T) {
	t.Parallel()
	out := filepath.Join(t.TempDir(), "merged")
	var stdout, stderr bytes.Buffer
	code := run([]string{"-base", prodFile, "-base-sha256", prodSha, "-add", combatFile, "-add-sha256", combatSha, "-exclude-type", scriptRecordType, allowUnchecked, "-out", out},
		&stdout, &stderr, quiet())
	if code != 0 {
		t.Fatalf("exit %d: %s", code, stderr.String())
	}
	frame, err := os.ReadFile(filepath.Join(out, manifestName))
	if err != nil {
		t.Fatal(err)
	}
	var sum struct {
		Sha256 string `json:"manifest_sha256"`
	}
	if err := json.Unmarshal(stdout.Bytes(), &sum); err != nil {
		t.Fatalf("stdout %q: %v", stdout.String(), err)
	}
	if sum.Sha256 != sha256Hex(frame) || len(sum.Sha256) != hexDigits {
		t.Errorf("printed sha256 %s, file hashes to %s", sum.Sha256, sha256Hex(frame))
	}
	// A second run into the same folder does not overwrite the file.
	if code := run([]string{"-base", prodFile, "-base-sha256", prodSha, "-add", combatFile, "-add-sha256", combatSha, "-out", out},
		&bytes.Buffer{}, &bytes.Buffer{}, quiet()); code == 0 {
		t.Error("a second run overwrote the merged manifest")
	}
}

// The printed summary names the labels that were joined.
func TestRunPrintsTheJoinedLabels(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	base, bsha := frameOf(t, joinBase)
	add, asha := frameOf(t, joinAdd)
	baseFile, addFile := filepath.Join(dir, "base.json.zst"), filepath.Join(dir, "add.json.zst")
	for path, frame := range map[string][]byte{baseFile: base, addFile: add} {
		if err := os.WriteFile(path, frame, filePerm); err != nil {
			t.Fatal(err)
		}
	}
	var stdout, stderr bytes.Buffer
	if code := run([]string{"-base", baseFile, "-base-sha256", bsha, "-add", addFile, "-add-sha256", asha, "-out", filepath.Join(dir, "merged")},
		&stdout, &stderr, quiet()); code != 0 {
		t.Fatalf("exit %d: %s", code, stderr.String())
	}
	var sum struct {
		Joined []string `json:"joined"`
	}
	if err := json.Unmarshal(stdout.Bytes(), &sum); err != nil {
		t.Fatalf("stdout %q: %v", stdout.String(), err)
	}
	if len(sum.Joined) != 1 || sum.Joined[0] != combatLabel {
		t.Errorf("printed joined = %v, want [%s]", sum.Joined, combatLabel)
	}
}

func TestMergeRefusesTopLevelEmptiesInTheBase(t *testing.T) {
	t.Parallel()
	base, bsha := frameOf(t, `{"schema":"nevr-cargo-manifest","schema_version":1,"labels":{"lobby_posters":{"entries":[]}},"empty":[{"type":"0x0000000000000009","name":"0x0000000000000009","align":16}]}`)
	add, asha := frameOf(t, `{"schema":"nevr-cargo-manifest","schema_version":1,"labels":{"combat_content":{"entries":[]}},"empty":[]}`)
	if _, err := merge(base, bsha, add, asha); err == nil || !strings.Contains(err.Error(), "base has top-level empty") {
		t.Errorf("top-level empties in the base: err = %v", err)
	}
}

// TestMergedFrameStatesItsContentSize reads the frame header: the manifest is framed as cargo-pack frames one, with its
// content size in the header.
func TestMergedFrameStatesItsContentSize(t *testing.T) {
	t.Parallel()
	res, err := merge(readFile(t, prodFile), prodSha, readFile(t, combatFile), combatSha)
	if err != nil {
		t.Fatal(err)
	}
	var h zstd.Header
	if err := h.Decode(res.Frame); err != nil {
		t.Fatal(err)
	}
	if !h.HasFCS || h.FrameContentSize != uint64(len(res.Text)) {
		t.Errorf("header states content size %d (present %v), the text is %d bytes", h.FrameContentSize, h.HasFCS, len(res.Text))
	}
	if !h.SingleSegment {
		t.Error("the frame is not single-segment: its window is not its content")
	}
}

const (
	// scriptImageType is the type of the 22 script-image records of the combat label: third-party compiled script DLLs,
	// which nevr-runtime does not serve (the production manifest does not carry them).
	scriptImageType = "0x06eace9f64204fd0"
	// mergedLabelCount is the number of labels the merged manifest holds: lobby_posters and combat_content.
	mergedLabelCount = 2
	// excludedTypeCount is the number of types the repeatable -exclude-type test names.
	excludedTypeCount = 2
	// corruptMask flips every bit of the byte the corruption test changes; halfDivisor locates the middle of a frame.
	corruptMask = 0xff
	halfDivisor = 2
	// scriptImageEntries is how many entries of that type the combat manifest holds (counted from the data).
	scriptImageEntries = 22
	// combatEntriesWithoutScripts is combatEntries less the script images.
	combatEntriesWithoutScripts = 3655
)

// entryTypes lists the type of each entry of a label in a merged text.
func entryTypes(t *testing.T, text []byte, label string) map[string]int {
	t.Helper()
	var doc struct {
		Labels map[string]struct {
			Entries []struct {
				Type string `json:"type"`
			} `json:"entries"`
		} `json:"labels"`
	}
	if err := json.Unmarshal(text, &doc); err != nil {
		t.Fatal(err)
	}
	out := map[string]int{}
	for _, e := range doc.Labels[label].Entries {
		out[e.Type]++
	}
	return out
}

func TestExcludeTypeDropsItsEntriesFromTheMergedLabel(t *testing.T) {
	t.Parallel()
	res, err := mergeExcluding(readFile(t, prodFile), prodSha, readFile(t, combatFile), combatSha, []string{scriptImageType}, nil)
	if err != nil {
		t.Fatal(err)
	}
	types := entryTypes(t, textOf(t, res.Frame), combatLabel)
	if n := types[scriptImageType]; n != 0 {
		t.Errorf("%d entries of the excluded type remain", n)
	}
	total := 0
	for _, n := range types {
		total += n
	}
	if total != combatEntriesWithoutScripts {
		t.Errorf("combat_content holds %d entries, want %d", total, combatEntriesWithoutScripts)
	}
	if res.Dropped[scriptImageType] != scriptImageEntries {
		t.Errorf("dropped %d of type %s, want %d", res.Dropped[scriptImageType], scriptImageType, scriptImageEntries)
	}
	if len(res.Dropped) != 1 {
		t.Errorf("dropped has %d types, want 1: %v", len(res.Dropped), res.Dropped)
	}
	// The empties of the label are records of 0 bytes, not entries: the exclusion leaves them.
	var doc struct {
		Labels map[string]struct {
			Empty []json.RawMessage `json:"empty"`
		} `json:"labels"`
	}
	if err := json.Unmarshal(textOf(t, res.Frame), &doc); err != nil {
		t.Fatal(err)
	}
	if len(doc.Labels[combatLabel].Empty) != combatEmpties {
		t.Errorf("combat_content holds %d empties, want %d", len(doc.Labels[combatLabel].Empty), combatEmpties)
	}
}

func TestExcludeTypeLeavesTheBaseLabelAlone(t *testing.T) {
	t.Parallel()
	baseFrame := readFile(t, prodFile)
	// 0x489bb35d53ca50e9 is the Quest payload type of lobby_posters: the exclusion is for the merged-in label only.
	res, err := mergeExcluding(baseFrame, prodSha, readFile(t, combatFile), combatSha, []string{"0x489bb35d53ca50e9"}, nil)
	if err != nil {
		t.Fatal(err)
	}
	if want, got := rawLabel(t, textOf(t, baseFrame), baseLabel), rawLabel(t, textOf(t, res.Frame), baseLabel); !bytes.Equal(want, got) {
		t.Fatal("the base label changed under an exclusion")
	}
	if res.Dropped["0x489bb35d53ca50e9"] != 0 {
		t.Errorf("an exclusion dropped %d base entries", res.Dropped["0x489bb35d53ca50e9"])
	}
	// The base's report still counts every base entry, of the excluded type or not.
	if res.Labels[0].Name != baseLabel || res.Labels[0].Entries != baseLabelEntries {
		t.Errorf("base report = %+v, want %d entries", res.Labels[0], baseLabelEntries)
	}
}

func TestExcludeTypeIsRepeatableAndCountsEveryTypeNamed(t *testing.T) {
	t.Parallel()
	absent := "0x0000000000000001"
	res, err := mergeExcluding(readFile(t, prodFile), prodSha, readFile(t, combatFile), combatSha, []string{scriptImageType, absent}, nil)
	if err != nil {
		t.Fatal(err)
	}
	if res.Dropped[scriptImageType] != scriptImageEntries || res.Dropped[absent] != 0 || len(res.Dropped) != excludedTypeCount {
		t.Errorf("dropped = %v, want %s: %d and %s: 0", res.Dropped, scriptImageType, scriptImageEntries, absent)
	}
}

func TestRunPrintsTheDroppedCountsPerType(t *testing.T) {
	t.Parallel()
	out := filepath.Join(t.TempDir(), "merged")
	var stdout, stderr bytes.Buffer
	code := run([]string{"-base", prodFile, "-base-sha256", prodSha, "-add", combatFile, "-add-sha256", combatSha,
		"-exclude-type", scriptImageType, "-exclude-type", scriptRecordType, allowUnchecked, "-out", out}, &stdout, &stderr, quiet())
	if code != 0 {
		t.Fatalf("exit %d: %s", code, stderr.String())
	}
	var sum struct {
		Dropped map[string]int `json:"dropped"`
	}
	if err := json.Unmarshal(stdout.Bytes(), &sum); err != nil {
		t.Fatalf("stdout %q: %v", stdout.String(), err)
	}
	if sum.Dropped[scriptImageType] != scriptImageEntries {
		t.Errorf("printed dropped = %v, want %d of %s", sum.Dropped, scriptImageEntries, scriptImageType)
	}
}

func TestExcludeTypeRefusesAMalformedType(t *testing.T) {
	t.Parallel()
	for _, bad := range []string{"0x06eace9f64204fd", "06eace9f64204fd0", "0x06EACE9F64204FD0", "0x06eace9f64204fd0x"} {
		var stdout, stderr bytes.Buffer
		code := run([]string{"-base", prodFile, "-base-sha256", prodSha, "-add", combatFile, "-add-sha256", combatSha,
			"-exclude-type", bad, "-out", t.TempDir()}, &stdout, &stderr, quiet())
		if code != exitUsage {
			t.Errorf("-exclude-type %q: exit %d, want %d", bad, code, exitUsage)
		}
	}
}
