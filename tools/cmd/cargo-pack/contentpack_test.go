package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

const (
	testSet                = "48037dc70b0ecab2"
	secondSet              = "2b47aab238f60515"
	typeA, typeB           = 1, 2
	replacedName           = 10
	addedName              = 5
	otherType              = 3
	recordsOfThePack       = 2       // the replacement and the added record
	objectsOfTwoPlatforms  = 2       // one record on two platforms
	stockPackagesAbovePack = 2       // more stock packages than the pack has, so none of its packages is its own
	unlistedPackage        = 2       // a package number content.json does not list
	untouchedName          = 20      // a stock record the pack leaves alone
	otherSetName           = 99      // the stock record of the second set
	chunkNotThere          = 1 << 20 // a chunk index past any table
	otherAlign             = 4096    // an alignment other than the one the builder writes
)

func shaOf(b []byte) string {
	h := sha256.Sum256(b)
	return hex.EncodeToString(h[:])
}

// makePack builds a pack the way content-pack-build does: the stock manifest with two changes applied and the one new
// package, in a folder that holds no stock package. It returns the folder; stock records are in the manifest only.
func makePack(t *testing.T, listed func(files map[string]string)) string {
	t.Helper()
	stockDir := t.TempDir()
	stock := map[radpack.Key][]byte{
		{Type: typeA, Name: replacedName}:  []byte("stock one"),
		{Type: typeB, Name: untouchedName}: []byte("stock two"),
	}
	if err := radpack.WriteStockSet(stockDir, testSet, []map[radpack.Key][]byte{stock}); err != nil {
		t.Fatal(err)
	}
	s, _, err := radpack.OpenSet(stockDir, testSet)
	if err != nil {
		t.Fatal(err)
	}
	manifest, pkg, err := radpack.Build(s.Manifest, map[radpack.Key][]byte{
		{Type: typeA, Name: replacedName}:  []byte("the pack's replacement"),
		{Type: otherType, Name: addedName}: []byte("added by the pack"),
	})
	if err != nil {
		t.Fatal(err)
	}
	dir := t.TempDir()
	for rel, body := range map[string][]byte{"manifests/" + testSet: manifest, "packages/" + testSet + "_1": pkg} {
		if err := os.MkdirAll(filepath.Dir(filepath.Join(dir, rel)), dirPerm); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(dir, rel), body, filePerm); err != nil {
			t.Fatal(err)
		}
	}
	files := map[string]string{"manifests/" + testSet: shaOf(manifest), "packages/" + testSet + "_1": shaOf(pkg)}
	if listed != nil {
		listed(files)
	}
	doc := contentDoc{Format: contentFormat, ID: "testpack", Files: files, Manifests: []contentManifest{{
		Package: testSet, StockPackages: 1, File: "manifests/" + testSet, Packages: []string{"packages/" + testSet + "_1"}}}}
	raw, err := json.Marshal(doc)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, contentJSON), raw, filePerm); err != nil {
		t.Fatal(err)
	}
	return dir
}

func TestContentPackRecordsAreTheOnesItsOwnPackagesStore(t *testing.T) {
	recs, err := contentPackRecords(makePack(t, nil), "combat_content", "windows-x86_64", quiet())
	if err != nil {
		t.Fatal(err)
	}
	got := map[string]string{}
	for _, r := range recs {
		got[r.typ+r.name] = string(r.data)
		if r.label != "combat_content" || r.platform != "windows-x86_64" || r.align == 0 {
			t.Errorf("record = %+v", r)
		}
	}
	want := map[string]string{
		"0x0000000000000001" + "0x000000000000000a": "the pack's replacement",
		"0x0000000000000003" + "0x0000000000000005": "added by the pack",
	}
	if len(got) != len(want) {
		t.Fatalf("records = %v, want %v (the stock record that the pack left alone is not one of them)", got, want)
	}
	for k, v := range want {
		if got[k] != v {
			t.Errorf("record %s = %q, want %q", k, got[k], v)
		}
	}
}

func TestContentPackFileThatIsNotTheListedOneIsRefused(t *testing.T) {
	dir := makePack(t, func(files map[string]string) { files["packages/"+testSet+"_1"] = shaOf([]byte("another file")) })
	if _, err := contentPackRecords(dir, "combat_content", "windows-x86_64", quiet()); err == nil {
		t.Fatal("a package that hashes to something else was read")
	}
	dir = makePack(t, func(files map[string]string) { delete(files, "manifests/"+testSet) })
	if _, err := contentPackRecords(dir, "combat_content", "windows-x86_64", quiet()); err == nil {
		t.Fatal("a manifest content.json does not list was read")
	}
}

func TestARecordOfZeroBytesIsListedAsEmptyAndHasNoObject(t *testing.T) {
	recs := []record{
		{label: "l", platform: "windows-x86_64", typ: "0x0000000000000001", name: "0x0000000000000001", align: testAlign, data: body(smallBytes, seedA)},
		{label: "l", platform: "windows-x86_64", typ: "0x0000000000000001", name: "0x0000000000000002", align: testAlign},
	}
	out := t.TempDir()
	sum, err := build(recs, out, testWindow, quiet())
	if err != nil {
		t.Fatal(err)
	}
	if len(sum.Objects) != 1 || sum.EmptyRecords != 1 {
		t.Fatalf("objects %d, empty %d", len(sum.Objects), sum.EmptyRecords)
	}
	mframe, err := os.ReadFile(filepath.Join(out, manifestName))
	if err != nil {
		t.Fatal(err)
	}
	var top struct {
		Labels map[string]struct {
			Empty []emptyDoc `json:"empty"`
		} `json:"labels"`
		Empty json.RawMessage `json:"empty"`
	}
	if err := json.Unmarshal(decode(t, mframe), &top); err != nil {
		t.Fatal(err)
	}
	// The record belongs to its label, with its platform; the top-level list is written empty.
	if string(top.Empty) != "[]" {
		t.Errorf("top-level empty = %s, want []", top.Empty)
	}
	got := top.Labels["l"].Empty
	if len(got) != 1 || got[0] != (emptyDoc{Platform: "windows-x86_64", Type: "0x0000000000000001", Name: "0x0000000000000002", Align: testAlign}) {
		t.Errorf("label l empty = %+v, want one windows-x86_64 record named 0x...2", got)
	}
}

// twoSetPack is makePack with a second set that stores one record the first set also stores.
func twoSetPack(t *testing.T, second []byte) string {
	t.Helper()
	dir := makePack(t, nil)
	const setB = secondSet
	stockDir := t.TempDir()
	if err := radpack.WriteStockSet(stockDir, setB, []map[radpack.Key][]byte{{{Type: typeB, Name: otherSetName}: []byte("stock b")}}); err != nil {
		t.Fatal(err)
	}
	s, _, err := radpack.OpenSet(stockDir, setB)
	if err != nil {
		t.Fatal(err)
	}
	manifest, pkg, err := radpack.Build(s.Manifest, map[radpack.Key][]byte{{Type: typeA, Name: replacedName}: second})
	if err != nil {
		t.Fatal(err)
	}
	for rel, body := range map[string][]byte{"manifests/" + setB: manifest, "packages/" + setB + "_1": pkg} {
		if err := os.WriteFile(filepath.Join(dir, rel), body, filePerm); err != nil {
			t.Fatal(err)
		}
	}
	var doc contentDoc
	raw, err := os.ReadFile(filepath.Join(dir, contentJSON))
	if err != nil {
		t.Fatal(err)
	}
	if err := json.Unmarshal(raw, &doc); err != nil {
		t.Fatal(err)
	}
	doc.Files["manifests/"+setB], doc.Files["packages/"+setB+"_1"] = shaOf(manifest), shaOf(pkg)
	doc.Manifests = append(doc.Manifests, contentManifest{Package: setB, StockPackages: 1, File: "manifests/" + setB, Packages: []string{"packages/" + setB + "_1"}})
	if raw, err = json.Marshal(doc); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, contentJSON), raw, filePerm); err != nil {
		t.Fatal(err)
	}
	return dir
}

func TestTheSameRecordInTwoSetsIsOneRecordWhenTheBytesAreTheSame(t *testing.T) {
	recs, err := contentPackRecords(twoSetPack(t, []byte("the pack's replacement")), "combat_content", "windows-x86_64", quiet())
	if err != nil {
		t.Fatal(err)
	}
	if len(recs) != recordsOfThePack {
		t.Fatalf("records = %d, want the replacement and the added one once each", len(recs))
	}
}

func TestTheSameRecordInTwoSetsWithDifferentBytesIsRefused(t *testing.T) {
	if _, err := contentPackRecords(twoSetPack(t, []byte("a different replacement")), "combat_content", "windows-x86_64", quiet()); err == nil {
		t.Fatal("two copies of one record with different bytes were accepted")
	}
}

// editManifest rewrites the pack's manifest of set `set` through fn and the sha-256 content.json lists for it.
func editManifest(t *testing.T, dir, set string, fn func(m *radpack.Manifest)) {
	t.Helper()
	path := filepath.Join(dir, "manifests", set)
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	m, err := radpack.DecodeFile(raw)
	if err != nil {
		t.Fatal(err)
	}
	fn(m)
	edited, err := m.EncodeFile()
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, edited, filePerm); err != nil {
		t.Fatal(err)
	}
	var doc contentDoc
	cj, err := os.ReadFile(filepath.Join(dir, contentJSON))
	if err != nil {
		t.Fatal(err)
	}
	if err := json.Unmarshal(cj, &doc); err != nil {
		t.Fatal(err)
	}
	doc.Files["manifests/"+set] = shaOf(edited)
	if cj, err = json.Marshal(doc); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, contentJSON), cj, filePerm); err != nil {
		t.Fatal(err)
	}
}

// addedIndex is the index of the pack's record `name` in the manifest.
func addedIndex(t *testing.T, m *radpack.Manifest, name int64) int {
	t.Helper()
	for i, r := range m.Resources {
		if r.Name == name && m.Chunks[r.Chunk].Package >= 1 {
			return i
		}
	}
	t.Fatalf("no added record named %d", name)
	return 0
}

func TestAnAddedRecordOfZeroBytesComesOutEmptyAndOneThatRunsPastItsChunkIsRefused(t *testing.T) {
	dir := makePack(t, nil)
	editManifest(t, dir, testSet, func(m *radpack.Manifest) { m.Resources[addedIndex(t, m, addedName)].Size = 0 })
	recs, err := contentPackRecords(dir, "combat_content", "windows-x86_64", quiet())
	if err != nil {
		t.Fatal(err)
	}
	empty := 0
	for _, r := range recs {
		if len(r.data) == 0 {
			empty++
		}
	}
	if empty != 1 || len(recs) != recordsOfThePack {
		t.Errorf("records = %d, empty = %d, want 2 and 1", len(recs), empty)
	}
	dir = makePack(t, nil)
	editManifest(t, dir, testSet, func(m *radpack.Manifest) { m.Resources[addedIndex(t, m, addedName)].Size = largeBytes * largeBytes })
	if _, err := contentPackRecords(dir, "combat_content", "windows-x86_64", quiet()); err == nil {
		t.Fatal("a record that runs past its chunk was read")
	}
}

func TestTheSameKeyOnTwoPlatformsIsTwoRecords(t *testing.T) {
	r := record{label: "l", typ: "0x0000000000000001", name: "0x0000000000000001", align: testAlign, data: body(smallBytes, seedB)}
	r2 := r
	r2.platform, r.platform = "android-arm64", "windows-x86_64"
	sum, err := build([]record{r, r2}, t.TempDir(), testWindow, quiet())
	if err != nil || len(sum.Objects) != objectsOfTwoPlatforms {
		t.Fatalf("objects = %v, err %v", sum, err)
	}
}

// editContent rewrites the pack's content.json through fn.
func editContent(t *testing.T, dir string, fn func(d *contentDoc)) {
	t.Helper()
	var doc contentDoc
	raw, err := os.ReadFile(filepath.Join(dir, contentJSON))
	if err != nil {
		t.Fatal(err)
	}
	if err := json.Unmarshal(raw, &doc); err != nil {
		t.Fatal(err)
	}
	fn(&doc)
	if raw, err = json.Marshal(doc); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, contentJSON), raw, filePerm); err != nil {
		t.Fatal(err)
	}
}

func TestContentPackRefusesWhatItCannotServe(t *testing.T) {
	const label, plat = "combat_content", "windows-x86_64"
	cases := []struct {
		name  string
		label string
		plat  string
		edit  func(t *testing.T, dir string)
		want  string // what the refusal says
	}{
		{"a label with a capital", "Combat", plat, nil, "label"},
		{"an unknown platform", label, "linux", nil, "platform"},
		{"a content.json over the bound", label, plat, func(t *testing.T, dir string) {
			if err := os.WriteFile(filepath.Join(dir, contentJSON), make([]byte, maxContentJSON+1), filePerm); err != nil {
				t.Fatal(err)
			}
		}, "more than"},
		{"another format", label, plat, func(t *testing.T, dir string) {
			editContent(t, dir, func(d *contentDoc) { d.Format = contentFormat + 1 })
		}, "is not a format"},
		{"no manifest", label, plat, func(t *testing.T, dir string) { editContent(t, dir, func(d *contentDoc) { d.Manifests = nil }) }, "is not a format"},
		{"a set id that is not 16 hex", label, plat, func(t *testing.T, dir string) {
			editContent(t, dir, func(d *contentDoc) {
				d.Manifests[0].Package = "../" + testSet
				d.Manifests[0].File = "manifests/../" + testSet
			})
		}, "is not a set id"},
		{"a manifest file that is not manifests/<set>", label, plat, func(t *testing.T, dir string) {
			editContent(t, dir, func(d *contentDoc) { d.Manifests[0].File = "manifests/other" })
		}, "is not a set id"},
		{"a manifest with no package", label, plat, func(t *testing.T, dir string) {
			editContent(t, dir, func(d *contentDoc) { d.Manifests[0].Packages = nil })
		}, "is not a set id"},
		{"a package path that leaves the pack folder", label, plat, func(t *testing.T, dir string) {
			editContent(t, dir, func(d *contentDoc) {
				d.Manifests[0].Packages = []string{"packages/" + testSet + "_../../outside"}
				d.Files["packages/"+testSet+"_../../outside"] = shaOf(nil)
			})
		}, "is not packages/"},
		{"a package number with a leading zero", label, plat, func(t *testing.T, dir string) {
			editContent(t, dir, func(d *contentDoc) { d.Manifests[0].Packages = []string{"packages/" + testSet + "_01"} })
		}, "is not packages/"},
		{"a stock package number", label, plat, func(t *testing.T, dir string) {
			editContent(t, dir, func(d *contentDoc) { d.Manifests[0].Packages = []string{"packages/" + testSet + "_0"} })
		}, "is not packages/"},
		{"a pack with no record of its own", label, plat, func(t *testing.T, dir string) {
			editContent(t, dir, func(d *contentDoc) {
				d.Manifests[0].StockPackages = stockPackagesAbovePack
				d.Manifests[0].Packages = []string{"packages/" + testSet + fmt.Sprintf("_%d", stockPackagesAbovePack)}
				d.Files["packages/"+testSet+fmt.Sprintf("_%d", stockPackagesAbovePack)] = shaOf(nil)
			})
			if err := os.WriteFile(filepath.Join(dir, "packages", testSet+fmt.Sprintf("_%d", stockPackagesAbovePack)), nil, filePerm); err != nil {
				t.Fatal(err)
			}
		}, "store no record"},
		{"a chunk past the table", label, plat, func(t *testing.T, dir string) {
			editManifest(t, dir, testSet, func(m *radpack.Manifest) { m.Resources[addedIndex(t, m, addedName)].Chunk = chunkNotThere })
		}, "names chunk"},
		{"a record in a package content.json does not list", label, plat, func(t *testing.T, dir string) {
			editManifest(t, dir, testSet, func(m *radpack.Manifest) {
				m.Chunks[m.Resources[addedIndex(t, m, addedName)].Chunk].Package = unlistedPackage
			})
			raw, err := os.ReadFile(filepath.Join(dir, "packages", testSet+"_1"))
			if err != nil {
				t.Fatal(err)
			}
			if err := os.WriteFile(filepath.Join(dir, "packages", testSet+fmt.Sprintf("_%d", unlistedPackage)), raw, filePerm); err != nil {
				t.Fatal(err)
			}
		}, "does not list"},
	}
	for _, c := range cases {
		dir := makePack(t, nil)
		if c.edit != nil {
			c.edit(t, dir)
		}
		_, err := contentPackRecords(dir, c.label, c.plat, quiet())
		if err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: err = %v, want a refusal that says %q", c.name, err, c.want)
		}
	}
}

func TestTheSameRecordInTwoSetsAtDifferentAlignmentsIsRefused(t *testing.T) {
	dir := twoSetPack(t, []byte("the pack's replacement"))
	editManifest(t, dir, secondSet, func(m *radpack.Manifest) { m.Resources[addedIndex(t, m, replacedName)].Alignment = otherAlign })
	if _, err := contentPackRecords(dir, "combat_content", "windows-x86_64", quiet()); err == nil {
		t.Fatal("two copies of one record at two alignments were accepted")
	}
}

func TestAnAlignmentThatIsNotAPowerOfTwoIsRefusedInAPackToo(t *testing.T) {
	for _, align := range []uint32{0, notPowerOfTwo, aboveMaxAlign} {
		r := record{label: "l", platform: "windows-x86_64", typ: "0x0000000000000001", name: "0x0000000000000001", align: align, data: body(smallBytes, seedA)}
		if _, err := build([]record{r}, t.TempDir(), testWindow, quiet()); err == nil {
			t.Errorf("alignment %d accepted", align)
		}
	}
}

func TestRunWantsOneInputAndTheLabelAndPlatformOfAPack(t *testing.T) {
	for _, args := range [][]string{
		{"-spec", "s.json", "-content-pack", "p", "-label", "l", "-platform", "windows-x86_64", "-out", "o", "-max-window", "1024"},
		{"-content-pack", "p", "-platform", "windows-x86_64", "-out", "o", "-max-window", "1024"},
		{"-content-pack", "p", "-label", "l", "-out", "o", "-max-window", "1024"},
		{"-out", "o", "-max-window", "1024"},
	} {
		var stdout, stderr bytes.Buffer
		if code := run(args, &stdout, &stderr, quiet()); code != exitUsage {
			t.Errorf("%v: exit %d, want %d (usage)", args, code, exitUsage)
		}
	}
}

const (
	copyName    = "00000000000000c1"
	copySource  = "190ed14fe6c74dd3"
	otherCopy   = "00000000000000c2"
	otherSource = "94ba7ca39fc97e1a"
)

// withRekeys rewrites the pack's content.json with the given `rekeys` list, the way content-pack-build writes it.
func withRekeys(t *testing.T, dir string, rekeys []scriptDoc) {
	t.Helper()
	path := filepath.Join(dir, contentJSON)
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	var doc contentDoc
	if err := json.Unmarshal(raw, &doc); err != nil {
		t.Fatal(err)
	}
	doc.Rekeys = rekeys
	if raw, err = json.Marshal(doc); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, raw, filePerm); err != nil {
		t.Fatal(err)
	}
}

func fixtureRekey(name, source, platformName string) scriptDoc {
	return scriptDoc{Name: name, Platform: platformName, StockSource: source, StockSha256: shaOf([]byte("stock " + source))}
}

// packedLabel runs cargo-pack over the pack and returns the manifest's label as written.
func packedLabel(t *testing.T, dir, platformName string) (labelDoc, int) {
	t.Helper()
	out := t.TempDir()
	var stdout, stderr bytes.Buffer
	if code := run([]string{"-content-pack", dir, "-label", "combat_content", "-platform", platformName, "-out", out, "-max-window", fmt.Sprint(testWindow)}, &stdout, &stderr, quiet()); code != 0 {
		t.Fatalf("exit %d: %s", code, stderr.String())
	}
	mframe, err := os.ReadFile(filepath.Join(out, manifestName))
	if err != nil {
		t.Fatal(err)
	}
	var top struct {
		Labels map[string]labelDoc `json:"labels"`
	}
	if err := json.Unmarshal(decode(t, mframe), &top); err != nil {
		t.Fatal(err)
	}
	blobs, err := os.ReadDir(filepath.Join(out, "blobs", "sha256"))
	if err != nil {
		t.Fatal(err)
	}
	return top.Labels["combat_content"], len(blobs)
}

func TestARekeyOfThePackIsTheLabelsScriptsListWithItsSha256AndNoScriptByte(t *testing.T) {
	dir := makePack(t, nil)
	withRekeys(t, dir, []scriptDoc{fixtureRekey(otherCopy, otherSource, "windows-x86_64"), fixtureRekey(copyName, copySource, "windows-x86_64")})
	label, blobs := packedLabel(t, dir, "windows-x86_64")
	want := []scriptDoc{fixtureRekey(copyName, copySource, "windows-x86_64"), fixtureRekey(otherCopy, otherSource, "windows-x86_64")}
	if len(label.Scripts) != len(want) || label.Scripts[0] != want[0] || label.Scripts[1] != want[1] {
		t.Fatalf("label scripts = %+v, want %+v (sorted by name, each with the sha-256 of its stock source)", label.Scripts, want)
	}
	if blobs != recordsOfThePack {
		t.Errorf("%d objects for a pack of %d records: a script went into the set", blobs, recordsOfThePack)
	}
}

func TestAPackWithNoRekeyWritesNoScriptsList(t *testing.T) {
	label, _ := packedLabel(t, makePack(t, nil), "windows-x86_64")
	if label.Scripts != nil {
		t.Errorf("label scripts = %+v, want none", label.Scripts)
	}
}

func TestOnlyTheRekeysOfThePlatformAreListed(t *testing.T) {
	dir := makePack(t, nil)
	withRekeys(t, dir, []scriptDoc{fixtureRekey(copyName, copySource, "windows-x86_64")})
	if label, _ := packedLabel(t, dir, "android-arm64"); label.Scripts != nil {
		t.Errorf("an android-arm64 label lists %+v", label.Scripts)
	}
}

func TestAMalformedRekeyIsRefused(t *testing.T) {
	good := fixtureRekey(copyName, copySource, "windows-x86_64")
	cases := map[string][]scriptDoc{
		"short name":       {{Name: "c1", Platform: good.Platform, StockSource: good.StockSource, StockSha256: good.StockSha256}},
		"upper-case name":  {{Name: "00000000000000C1", Platform: good.Platform, StockSource: good.StockSource, StockSha256: good.StockSha256}},
		"bad source":       {{Name: good.Name, Platform: good.Platform, StockSource: "xyz", StockSha256: good.StockSha256}},
		"bad sha":          {{Name: good.Name, Platform: good.Platform, StockSource: good.StockSource, StockSha256: "abc"}},
		"unknown platform": {{Name: good.Name, Platform: "plan9", StockSource: good.StockSource, StockSha256: good.StockSha256}},
		"copy of itself":   {{Name: good.StockSource, Platform: good.Platform, StockSource: good.StockSource, StockSha256: good.StockSha256}},
		"listed twice":     {good, good},
	}
	for what, list := range cases {
		dir := makePack(t, nil)
		withRekeys(t, dir, list)
		if _, err := contentPackScripts(dir, "combat_content", "windows-x86_64", quiet()); err == nil {
			t.Errorf("%s: the rekey was read", what)
		}
	}
}
