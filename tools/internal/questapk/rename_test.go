package questapk

import (
	"bytes"
	"encoding/binary"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/axmlfix"
	"github.com/EchoTools/nevr-runtime/tools/internal/questapkfix"
)

// The Go twin of tools/tests/test_quest_apk_rename_package.py: the reference APK's package rename, with every count
// asserted (#1737). The fixture APK is built here; the real-file cases run when NEVR_REFERENCE_APK names the reference APK.

const (
	referenceLabel   = "Echo Combat (port)"
	projectLabel     = "Echo VR"
	labelTooLong     = referenceLabel + " and more"
	otherPackage     = "com.other.packageXXX"
	realLibr15Prefix = "936aa6a3" // kLibR15ReferenceSha256[0], src/quest/patch/libr15_patch_table.h
	libr15Name       = "lib/arm64-v8a/libr15.so"
	assetPatchName   = "lib/arm64-v8a/libassetpatch.so"
	libvrapiName     = "lib/arm64-v8a/libvrapi.so"
	realEnv          = "NEVR_REFERENCE_APK"
	ascii5           = 5
	ascii2           = 2
	ascii4           = 4
	ascii6           = 6
	ascii3           = 3
	metaInfCount     = 3
	keptEntries      = 7
)

// The fixture builders live in questapkfix, which the command tests share.
type (
	fixtureEntry = questapkfix.Entry
	readEntry    = questapkfix.ReadEntry
)

const (
	deflate = questapkfix.Deflate
	stored  = questapkfix.Stored
)

// wide is text as UTF-16 little-endian bytes.
func wide(text string) []byte { return questapkfix.Wide(text) }

// afkManifest is the reference build's manifest (UTF-16 unless utf8).
func afkManifest(utf8 bool) []byte { return questapkfix.AfkManifest(utf8) }

// referenceEntries is the fixture reference APK's entries.
func referenceEntries(libr15Count, assetPatchCount int, manifest []byte, extra ...fixtureEntry) []fixtureEntry {
	return questapkfix.ReferenceEntries(libr15Count, assetPatchCount, manifest, extra...)
}

// writeFixture writes entries as an APK at path.
func writeFixture(t *testing.T, path string, entries []fixtureEntry) {
	questapkfix.Write(t, path, entries)
}

// readFixture reads an APK's entries, in archive order and by name.
func readFixture(t *testing.T, path string) ([]readEntry, map[string]readEntry) {
	return questapkfix.Read(t, path)
}

// labelFixtureManifest is the reference build's manifest shape: one raw label and the package.
func labelFixtureManifest() []byte {
	return questapkfix.LabelledManifest(DefaultFrom, referenceLabel, false)
}

// fixtureTest is one test's directory with the input and output paths of the fixture APK.
type fixtureTest struct {
	dir, src, out string
}

// newFixtureTest is a fixture directory for one test.
func newFixtureTest(t *testing.T) fixtureTest {
	dir := t.TempDir()
	return fixtureTest{dir: dir, src: filepath.Join(dir, "in.apk"), out: filepath.Join(dir, "out.apk")}
}

// make writes the reference fixture with the counts given and returns its entries.
func (f fixtureTest) make(t *testing.T, libr15Count, assetPatchCount int, manifest []byte, extra ...fixtureEntry) []fixtureEntry {
	t.Helper()
	entries := referenceEntries(libr15Count, assetPatchCount, manifest, extra...)
	writeFixture(t, f.src, entries)
	return entries
}

// refused asserts that a rename is refused with a message, that nothing was written and no partial file is left, and
// returns the message.
func (f fixtureTest) refused(t *testing.T, src, out, old, new string, label *string) string {
	t.Helper()
	_, err := RenameApk(src, out, old, new, label)
	if err == nil {
		t.Fatal("the rename was not refused")
	}
	var re *RenameError
	if !errors.As(err, &re) {
		t.Fatalf("the refusal is not a RenameError: %T %v", err, err)
	}
	if _, statErr := os.Stat(out); statErr == nil {
		t.Fatal("a refusal wrote the output")
	}
	if _, statErr := os.Stat(out + partSuffix); statErr == nil {
		t.Fatal("a refusal left its partial file")
	}
	return err.Error()
}

// sha256Of is the hex sha256 of data (the record's own function, under the test's name).
func sha256Of(data []byte) string { return sha(data) }

// TestRenameExactlyTheThreeEntriesWithTheirCounts.
func TestRenameExactlyTheThreeEntriesWithTheirCounts(t *testing.T) {
	t.Parallel()
	f := newFixtureTest(t)
	items := f.make(t, ascii5, ascii2, nil)
	rec, err := RenameApk(f.src, f.out, DefaultFrom, DefaultTo, nil)
	if err != nil {
		t.Fatal(err)
	}
	before := map[string][]byte{}
	for _, e := range items {
		before[e.Name] = e.Data
	}
	_, after := readFixture(t, f.out)
	if rec.Event != eventRenamed {
		t.Fatalf("event = %s", rec.Event)
	}
	wantCounts := []countEntry{{manifestName, pair{0, 1}}, {libr15Name, pair{ascii5, 0}}, {assetPatchName, pair{ascii2, 0}}}
	if len(rec.Counts) != len(wantCounts) {
		t.Fatalf("counts = %+v", rec.Counts)
	}
	for i := range wantCounts {
		if rec.Counts[i] != wantCounts[i] {
			t.Fatalf("counts[%d] = %+v, want %+v", i, rec.Counts[i], wantCounts[i])
		}
	}
	if len(rec.Changed) != len(wantCounts) {
		t.Fatalf("changed = %+v", rec.Changed)
	}
	manifest := after[manifestName].Data
	if len(manifest) != len(before[manifestName]) {
		t.Fatal("the manifest's length moved")
	}
	if bytes.Count(manifest, wide(DefaultFrom)) != 0 {
		t.Fatal("the source is still in the manifest")
	}
	if !bytes.Equal(manifest, bytes.ReplaceAll(before[manifestName], wide(DefaultFrom), wide(DefaultTo))) {
		t.Fatal("the manifest is not the original with the substitution")
	}
	pool, err := poolStrings(manifest)
	if err != nil {
		t.Fatal(err)
	}
	if !containsString(pool, DefaultTo) || containsString(pool, DefaultFrom) {
		t.Fatalf("pool = %v", pool)
	}
	for name, n := range map[string]int{libr15Name: ascii5, assetPatchName: ascii2} {
		data := after[name].Data
		if len(data) != len(before[name]) {
			t.Fatalf("%s: length moved", name)
		}
		if bytes.Count(data, []byte(DefaultFrom)) != 0 {
			t.Fatalf("%s: source remains", name)
		}
		if !bytes.Equal(data, bytes.ReplaceAll(before[name], []byte(DefaultFrom), []byte(DefaultTo))) {
			t.Fatalf("%s: not the substitution", name)
		}
		if bytes.Count(data, []byte(DefaultTo)) < n {
			t.Fatalf("%s: fewer than %d targets", name, n)
		}
	}
	for _, c := range rec.Changed {
		if c.in != sha256Of(before[c.name]) || c.out != sha256Of(after[c.name].Data) || c.in == c.out {
			t.Fatalf("changed %s: hashes do not match its bytes", c.name)
		}
	}
}

// containsString reports whether a string slice holds s.
func containsString(strs []string, s string) bool {
	for _, x := range strs {
		if x == s {
			return true
		}
	}
	return false
}

// TestStripsMetaInfAndKeepsEveryOtherEntryByteIdentical.
func TestStripsMetaInfAndKeepsEveryOtherEntryByteIdentical(t *testing.T) {
	t.Parallel()
	f := newFixtureTest(t)
	items := f.make(t, ascii5, ascii2, nil)
	rec, err := RenameApk(f.src, f.out, DefaultFrom, DefaultTo, nil)
	if err != nil {
		t.Fatal(err)
	}
	order, after := readFixture(t, f.out)
	if rec.DroppedMetaInf != metaInfCount {
		t.Fatalf("dropped = %d", rec.DroppedMetaInf)
	}
	for name := range after {
		if strings.HasPrefix(name, metaInfPrefix) {
			t.Fatalf("%s survived", name)
		}
	}
	var kept []fixtureEntry
	for _, e := range items {
		if !strings.HasPrefix(e.Name, metaInfPrefix) {
			kept = append(kept, e)
		}
	}
	if len(order) != len(kept) {
		t.Fatalf("entry count %d, want %d", len(order), len(kept))
	}
	for i, e := range kept {
		if order[i].Name != e.Name {
			t.Fatalf("entry order: %s at %d, want %s", order[i].Name, i, e.Name)
		}
		got := after[e.Name]
		if _, named := expectationOf(e.Name); named {
			if got.Method != e.Method {
				t.Fatalf("%s: compression", e.Name)
			}
			continue
		}
		if !bytes.Equal(got.Data, e.Data) {
			t.Fatalf("%s: bytes", e.Name)
		}
		if got.Method != e.Method {
			t.Fatalf("%s: compression", e.Name)
		}
		if !got.When.Equal(questapkfix.Date) {
			t.Fatalf("%s: timestamp %s", e.Name, got.When)
		}
	}
	if n := bytes.Count(after["classes.dex"].Data, []byte(DefaultTo)); n != 1 {
		t.Fatalf("classes.dex holds the target %d times", n)
	}
	if n := bytes.Count(after["resources.arsc"].Data, wide(DefaultTo)); n != 1 {
		t.Fatalf("resources.arsc holds the target %d times", n)
	}
}

// TestRecordCarriesTheOutputShaAndNames.
func TestRecordCarriesTheOutputShaAndNames(t *testing.T) {
	t.Parallel()
	f := newFixtureTest(t)
	f.make(t, ascii5, ascii2, nil)
	rec, err := RenameApk(f.src, f.out, DefaultFrom, DefaultTo, nil)
	if err != nil {
		t.Fatal(err)
	}
	if rec.Apk != "in.apk" || rec.Out != "out.apk" || rec.From != DefaultFrom || rec.To != DefaultTo {
		t.Fatalf("names: %+v", rec)
	}
	out, err := os.ReadFile(f.out)
	if err != nil {
		t.Fatal(err)
	}
	if rec.Sha256Out != sha256Of(out) {
		t.Fatal("sha256_out is not the output's hash")
	}
	if rec.EntriesOut != keptEntries {
		t.Fatalf("entries_out = %d", rec.EntriesOut)
	}
}

// TestRefusesALibr15CountThatDiffers: 4, 6 and 0 are not 5 (0 is a partial state, not already renamed).
func TestRefusesALibr15CountThatDiffers(t *testing.T) {
	t.Parallel()
	for _, n := range []int{ascii4, ascii6, 0} {
		t.Run(itoa(n), func(t *testing.T) {
			t.Parallel()
			f := newFixtureTest(t)
			f.make(t, n, ascii2, nil)
			if msg := f.refused(t, f.src, f.out, DefaultFrom, DefaultTo, nil); !strings.Contains(msg, libr15Name) {
				t.Fatalf("message does not name libr15: %s", msg)
			}
		})
	}
}

// TestRefusesAnAssetPatchCountThatDiffers.
func TestRefusesAnAssetPatchCountThatDiffers(t *testing.T) {
	t.Parallel()
	f := newFixtureTest(t)
	f.make(t, ascii5, ascii3, nil)
	if msg := f.refused(t, f.src, f.out, DefaultFrom, DefaultTo, nil); !strings.Contains(msg, "expected (2, 0)") {
		t.Fatalf("message = %s", msg)
	}
}

// TestRefusesASecondManifestString: a second UTF-16 copy of the source inside the chunk size is a count of 2.
func TestRefusesASecondManifestString(t *testing.T) {
	t.Parallel()
	data := afkManifest(false)
	data = append(append(data, wide(DefaultFrom)...), 0, 0)
	binary.LittleEndian.PutUint32(data[4:], uint32(len(data)))
	f := newFixtureTest(t)
	f.make(t, ascii5, ascii2, data)
	if msg := f.refused(t, f.src, f.out, DefaultFrom, DefaultTo, nil); !strings.Contains(msg, manifestName) {
		t.Fatalf("message = %s", msg)
	}
}

// TestRefusesAUTF8ManifestPool.
func TestRefusesAUTF8ManifestPool(t *testing.T) {
	t.Parallel()
	f := newFixtureTest(t)
	f.make(t, ascii5, ascii2, afkManifest(true))
	if msg := f.refused(t, f.src, f.out, DefaultFrom, DefaultTo, nil); !strings.Contains(msg, manifestName) {
		t.Fatalf("message = %s", msg)
	}
}

// TestRefusesASourceStringInAnEntryTheRenameDoesNotName: each planted entry, named with its form.
func TestRefusesASourceStringInAnEntryTheRenameDoesNotName(t *testing.T) {
	t.Parallel()
	cases := []struct {
		name, form string
		data       []byte
	}{
		{"classes.dex", "ascii=1 utf16le=0", append([]byte("dex\x00"), []byte(DefaultFrom)...)},
		{"assets/other.bin", "ascii=0 utf16le=1", wide(DefaultFrom)},
		{libvrapiName, "ascii=2 utf16le=0", []byte(DefaultFrom + DefaultFrom)},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			t.Parallel()
			f := newFixtureTest(t)
			var items []fixtureEntry
			for _, e := range referenceEntries(ascii5, ascii2, nil) {
				if e.Name != c.name {
					items = append(items, e)
				}
			}
			items = append(items, fixtureEntry{Name: c.name, Data: c.data, Method: deflate})
			writeFixture(t, f.src, items)
			msg := f.refused(t, f.src, f.out, DefaultFrom, DefaultTo, nil)
			for _, want := range []string{c.name, c.form, "does not name"} {
				if !strings.Contains(msg, want) {
					t.Fatalf("message lacks %q: %s", want, msg)
				}
			}
		})
	}
}

// TestReportsEveryUnnamedEntryNotOnlyTheFirst.
func TestReportsEveryUnnamedEntryNotOnlyTheFirst(t *testing.T) {
	t.Parallel()
	f := newFixtureTest(t)
	f.make(t, ascii5, ascii2, nil, fixtureEntry{Name: "a.bin", Data: []byte(DefaultFrom), Method: deflate}, fixtureEntry{Name: "b.bin", Data: wide(DefaultFrom), Method: deflate})
	msg := f.refused(t, f.src, f.out, DefaultFrom, DefaultTo, nil)
	if !strings.Contains(msg, "a.bin") || !strings.Contains(msg, "b.bin") {
		t.Fatalf("message = %s", msg)
	}
}

// TestRefusesTheSourceInNeitherFormWhenTheTargetIsNotThereEither.
func TestRefusesTheSourceInNeitherFormWhenTheTargetIsNotThereEither(t *testing.T) {
	t.Parallel()
	f := newFixtureTest(t)
	var items []fixtureEntry
	for _, e := range referenceEntries(ascii5, ascii2, nil) {
		switch e.Name {
		case "classes.dex", "resources.arsc", libvrapiName:
			continue
		case manifestName:
			e.Data = bytes.ReplaceAll(e.Data, wide(DefaultFrom), wide(otherPackage))
		default:
			e.Data = bytes.ReplaceAll(e.Data, []byte(DefaultFrom), []byte(otherPackage))
		}
		items = append(items, e)
	}
	writeFixture(t, f.src, items)
	if msg := f.refused(t, f.src, f.out, DefaultFrom, DefaultTo, nil); !strings.Contains(msg, "neither the source package string nor the target") {
		t.Fatalf("message = %s", msg)
	}
}

// TestRefusesAMissingNamedEntry.
func TestRefusesAMissingNamedEntry(t *testing.T) {
	t.Parallel()
	f := newFixtureTest(t)
	var items []fixtureEntry
	for _, e := range referenceEntries(ascii5, ascii2, nil) {
		if e.Name != assetPatchName {
			items = append(items, e)
		}
	}
	writeFixture(t, f.src, items)
	if msg := f.refused(t, f.src, f.out, DefaultFrom, DefaultTo, nil); !strings.Contains(msg, assetPatchName) {
		t.Fatalf("message = %s", msg)
	}
}

// TestRefusesNamesOfDifferentLengthOrEqualOrNonASCII.
func TestRefusesNamesOfDifferentLengthOrEqualOrNonASCII(t *testing.T) {
	t.Parallel()
	f := newFixtureTest(t)
	f.make(t, ascii5, ascii2, nil)
	cases := [][2]string{{DefaultFrom, "com.readyatdawn.r150"}, {DefaultFrom, DefaultFrom}, {"com.x", "com.x.y"}, {DefaultFrom, "com.readyatdawn.ä15"}}
	for _, c := range cases {
		t.Run(c[1], func(t *testing.T) {
			t.Parallel()
			if _, err := RenameApk(f.src, f.out, c[0], c[1], nil); err == nil {
				t.Fatal("accepted")
			}
			if _, statErr := os.Stat(f.out); statErr == nil {
				t.Fatal("a refusal wrote the output")
			}
		})
	}
}

// TestRefusesToWriteOverItsInput.
func TestRefusesToWriteOverItsInput(t *testing.T) {
	t.Parallel()
	f := newFixtureTest(t)
	f.make(t, ascii5, ascii2, nil)
	before, err := os.ReadFile(f.src)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := RenameApk(f.src, f.src, DefaultFrom, DefaultTo, nil); err == nil {
		t.Fatal("accepted")
	}
	after, _ := os.ReadFile(f.src)
	if !bytes.Equal(before, after) {
		t.Fatal("the input changed")
	}
}

// TestAnAlreadyRenamedInputIsReportedAndCopied.
func TestAnAlreadyRenamedInputIsReportedAndCopied(t *testing.T) {
	t.Parallel()
	f := newFixtureTest(t)
	f.make(t, ascii5, ascii2, nil)
	if _, err := RenameApk(f.src, f.out, DefaultFrom, DefaultTo, nil); err != nil {
		t.Fatal(err)
	}
	again := filepath.Join(f.dir, "again.apk")
	rec, err := RenameApk(f.out, again, DefaultFrom, DefaultTo, nil)
	if err != nil {
		t.Fatal(err)
	}
	if rec.Event != eventAlready || len(rec.Changed) != 0 {
		t.Fatalf("record = %+v", rec)
	}
	_, a := readFixture(t, f.out)
	_, b := readFixture(t, again)
	assertSameEntries(t, a, b)
}

// assertSameEntries fails unless two entry maps hold the same bytes, methods and timestamps.
func assertSameEntries(t *testing.T, a, b map[string]readEntry) {
	t.Helper()
	if len(a) != len(b) {
		t.Fatalf("entry counts %d and %d", len(a), len(b))
	}
	for name, x := range a {
		y, ok := b[name]
		if !ok || !bytes.Equal(x.Data, y.Data) || x.Method != y.Method || !x.When.Equal(y.When) {
			t.Fatalf("%s differs", name)
		}
	}
}

// TestAnAlreadyRenamedInputKeepsMetaInfDropped: a re-signed copy's META-INF goes again.
func TestAnAlreadyRenamedInputKeepsMetaInfDropped(t *testing.T) {
	t.Parallel()
	f := newFixtureTest(t)
	f.make(t, ascii5, ascii2, nil)
	if _, err := RenameApk(f.src, f.out, DefaultFrom, DefaultTo, nil); err != nil {
		t.Fatal(err)
	}
	order, _ := readFixture(t, f.out)
	signed := filepath.Join(f.dir, "signed.apk")
	var items []fixtureEntry
	for _, e := range order {
		items = append(items, fixtureEntry{Name: e.Name, Data: e.Data, Method: e.Method})
	}
	items = append(items, fixtureEntry{Name: "META-INF/X.SF", Data: []byte("sf"), Method: deflate})
	writeFixture(t, signed, items)
	again := filepath.Join(f.dir, "again.apk")
	rec, err := RenameApk(signed, again, DefaultFrom, DefaultTo, nil)
	if err != nil {
		t.Fatal(err)
	}
	if rec.DroppedMetaInf != 1 {
		t.Fatalf("dropped = %d", rec.DroppedMetaInf)
	}
	_, a := readFixture(t, f.out)
	_, b := readFixture(t, again)
	assertSameEntries(t, a, b)
}

// TestRenameFlagWritesNoLabelFields: without a label the record has no label fields.
func TestWithoutALabelTheManifestLabelAndTheRecordAreAsTheyWere(t *testing.T) {
	t.Parallel()
	f := newFixtureTest(t)
	f.make(t, ascii5, ascii2, labelFixtureManifest())
	rec, err := RenameApk(f.src, f.out, DefaultFrom, DefaultTo, nil)
	if err != nil {
		t.Fatal(err)
	}
	_, after := readFixture(t, f.out)
	if got, _ := LabelOf(after[manifestName].Data); got != referenceLabel {
		t.Fatalf("label = %q", got)
	}
	if rec.LabelFrom != nil || rec.LabelTo != nil {
		t.Fatal("label fields without a label")
	}
}

// labelled writes the labelled fixture the label tests edit.
func (f fixtureTest) labelled(t *testing.T, shareLabel bool) {
	t.Helper()
	spec := axmlfix.LabelManifest(DefaultFrom, referenceLabel)
	spec.ShareLabel = shareLabel
	f.make(t, ascii5, ascii2, spec.Build())
}

// TestARenamedInputGetsThePackageAndTheLabelAndNoOtherEntryChanges.
func TestARenamedInputGetsThePackageAndTheLabelAndNoOtherEntryChanges(t *testing.T) {
	t.Parallel()
	f := newFixtureTest(t)
	f.labelled(t, false)
	items := referenceEntries(ascii5, ascii2, labelFixtureManifest())
	plain := filepath.Join(f.dir, "plain.apk")
	if _, err := RenameApk(f.src, plain, DefaultFrom, DefaultTo, nil); err != nil {
		t.Fatal(err)
	}
	label := projectLabel
	rec, err := RenameApk(f.src, f.out, DefaultFrom, DefaultTo, &label)
	if err != nil {
		t.Fatal(err)
	}
	if rec.Event != eventRenamed || rec.LabelFrom == nil || *rec.LabelFrom != referenceLabel || *rec.LabelTo != projectLabel {
		t.Fatalf("record = %+v", rec)
	}
	order, after := readFixture(t, f.out)
	_, without := readFixture(t, plain)
	manifest := after[manifestName].Data
	if got, _ := LabelOf(manifest); got != projectLabel {
		t.Fatalf("label = %q", got)
	}
	pool, _ := poolStrings(manifest)
	if !containsString(pool, DefaultTo) || containsString(pool, DefaultFrom) {
		t.Fatalf("pool = %v", pool)
	}
	source := items[3].Data
	if len(manifest) != len(source) {
		t.Fatal("length moved")
	}
	want, err := SetLabel(without[manifestName].Data, projectLabel)
	if err != nil || !bytes.Equal(manifest, want) {
		t.Fatalf("manifest is not the plain one relabelled: %v", err)
	}
	if len(order) != len(without) {
		t.Fatal("entry count")
	}
	for name, e := range without {
		if name == manifestName {
			continue
		}
		if !bytes.Equal(after[name].Data, e.Data) || after[name].Method != e.Method {
			t.Fatalf("%s: the label changed another entry", name)
		}
	}
	if len(rec.Changed) != 3 {
		t.Fatalf("changed = %+v", rec.Changed)
	}
	if rec.Changed[0].in != sha256Of(source) || rec.Changed[0].out != sha256Of(manifest) {
		t.Fatal("manifest hashes do not match")
	}
	if rec.Sha256Out != sha256Of(mustRead(t, f.out)) {
		t.Fatal("sha256_out")
	}
}

// mustRead is a file's bytes.
func mustRead(t *testing.T, path string) []byte {
	t.Helper()
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	return data
}

// TestAnAlreadyRenamedInputStillGetsTheLabel.
func TestAnAlreadyRenamedInputStillGetsTheLabel(t *testing.T) {
	t.Parallel()
	f := newFixtureTest(t)
	f.labelled(t, false)
	renamed := filepath.Join(f.dir, "renamed.apk")
	if _, err := RenameApk(f.src, renamed, DefaultFrom, DefaultTo, nil); err != nil {
		t.Fatal(err)
	}
	label := projectLabel
	rec, err := RenameApk(renamed, f.out, DefaultFrom, DefaultTo, &label)
	if err != nil {
		t.Fatal(err)
	}
	if rec.Event != eventAlready || *rec.LabelFrom != referenceLabel || *rec.LabelTo != projectLabel {
		t.Fatalf("record = %+v", rec)
	}
	_, before := readFixture(t, renamed)
	_, after := readFixture(t, f.out)
	manifest := after[manifestName].Data
	if got, _ := LabelOf(manifest); got != projectLabel {
		t.Fatalf("label = %q", got)
	}
	want, err := SetLabel(before[manifestName].Data, projectLabel)
	if err != nil || !bytes.Equal(manifest, want) {
		t.Fatalf("manifest: %v", err)
	}
	if len(rec.Changed) != 1 || rec.Changed[0].name != manifestName {
		t.Fatalf("changed = %+v", rec.Changed)
	}
}

// TestASecondRunWithTheSameLabelWritesTheSameEntries.
func TestASecondRunWithTheSameLabelWritesTheSameEntries(t *testing.T) {
	t.Parallel()
	f := newFixtureTest(t)
	f.labelled(t, false)
	label := projectLabel
	if _, err := RenameApk(f.src, f.out, DefaultFrom, DefaultTo, &label); err != nil {
		t.Fatal(err)
	}
	again := filepath.Join(f.dir, "again.apk")
	rec, err := RenameApk(f.out, again, DefaultFrom, DefaultTo, &label)
	if err != nil {
		t.Fatal(err)
	}
	if rec.Event != eventAlready || *rec.LabelFrom != projectLabel || *rec.LabelTo != projectLabel || len(rec.Changed) != 0 {
		t.Fatalf("record = %+v", rec)
	}
	_, a := readFixture(t, f.out)
	_, b := readFixture(t, again)
	assertSameEntries(t, a, b)
}

// TestALabelLongerThanTheOldOneIsARefusalAndWritesNothing.
func TestALabelLongerThanTheOldOneIsARefusalAndWritesNothing(t *testing.T) {
	t.Parallel()
	f := newFixtureTest(t)
	f.labelled(t, false)
	label := labelTooLong
	msg := f.refused(t, f.src, f.out, DefaultFrom, DefaultTo, &label)
	if !strings.Contains(msg, manifestName) || !strings.Contains(msg, "only edits in place") {
		t.Fatalf("message = %s", msg)
	}
}

// TestALabelLongerThanTheOldOneOnAnAlreadyRenamedInputWritesNothing.
func TestALabelLongerThanTheOldOneOnAnAlreadyRenamedInputWritesNothing(t *testing.T) {
	t.Parallel()
	f := newFixtureTest(t)
	f.labelled(t, false)
	renamed := filepath.Join(f.dir, "renamed.apk")
	if _, err := RenameApk(f.src, renamed, DefaultFrom, DefaultTo, nil); err != nil {
		t.Fatal(err)
	}
	label := labelTooLong
	if msg := f.refused(t, renamed, f.out, DefaultFrom, DefaultTo, &label); !strings.Contains(msg, "only edits in place") {
		t.Fatalf("message = %s", msg)
	}
}

// TestAnEmptyOrNonASCIILabelIsARefusalAndWritesNothing.
func TestAnEmptyOrNonASCIILabelIsARefusalAndWritesNothing(t *testing.T) {
	t.Parallel()
	f := newFixtureTest(t)
	f.labelled(t, false)
	for _, label := range []string{"", "Echo Vÿ"} {
		t.Run(label, func(t *testing.T) {
			t.Parallel()
			l := label
			if msg := f.refused(t, f.src, f.out, DefaultFrom, DefaultTo, &l); !strings.Contains(msg, "non-empty ASCII") {
				t.Fatalf("message = %s", msg)
			}
		})
	}
}

// TestAManifestWithoutAnApplicationLabelIsARefusalAndWritesNothing.
func TestAManifestWithoutAnApplicationLabelIsARefusalAndWritesNothing(t *testing.T) {
	t.Parallel()
	f := newFixtureTest(t)
	f.make(t, ascii5, ascii2, nil)
	label := projectLabel
	if msg := f.refused(t, f.src, f.out, DefaultFrom, DefaultTo, &label); !strings.Contains(msg, "android:label") {
		t.Fatalf("message = %s", msg)
	}
}

// TestALabelReferencedByAnotherAttributeIsARefusalAndWritesNothing.
func TestALabelReferencedByAnotherAttributeIsARefusalAndWritesNothing(t *testing.T) {
	t.Parallel()
	f := newFixtureTest(t)
	f.labelled(t, true)
	label := projectLabel
	if msg := f.refused(t, f.src, f.out, DefaultFrom, DefaultTo, &label); !strings.Contains(msg, "referenced by") {
		t.Fatalf("message = %s", msg)
	}
}

// TestTheFixtureManifestReadsAsTheReferenceBuildDoes.
func TestTheFixtureManifestReadsAsTheReferenceBuildDoes(t *testing.T) {
	t.Parallel()
	manifest := axmlfix.LabelManifest(DefaultFrom, referenceLabel).Build()
	if got, _ := LabelOf(manifest); got != referenceLabel {
		t.Fatalf("label = %q", got)
	}
	if n := bytes.Count(manifest, wide(DefaultFrom)); n != 1 {
		t.Fatalf("source count = %d", n)
	}
}

// TestRealReferenceApkRenamesWithTheLaneCountsAndThePinnedLibr15Sha runs when NEVR_REFERENCE_APK names the reference APK.
func TestRealReferenceApkRenamesWithTheLaneCountsAndThePinnedLibr15Sha(t *testing.T) {
	t.Parallel()
	src := referenceAPK(t)
	dir := t.TempDir()
	out := filepath.Join(dir, "ref-r15.apk")
	rec, err := RenameApk(src, out, DefaultFrom, DefaultTo, nil)
	if err != nil {
		t.Fatal(err)
	}
	if rec.Event != eventRenamed {
		t.Fatalf("event = %s", rec.Event)
	}
	_, after := readFixture(t, out)
	libr15 := after[libr15Name].Data
	if !strings.HasPrefix(sha256Of(libr15), realLibr15Prefix) {
		t.Fatalf("libr15 sha = %s", sha256Of(libr15))
	}
	if sha256Of(libr15) != rec.Changed[1].out {
		t.Fatal("record's libr15 hash is not the output's")
	}
	if bytes.Count(libr15, []byte(DefaultFrom)) != 0 || bytes.Count(after[assetPatchName].Data, []byte(DefaultFrom)) != 0 {
		t.Fatal("source remains")
	}
	again := filepath.Join(dir, "again.apk")
	second, err := RenameApk(out, again, DefaultFrom, DefaultTo, nil)
	if err != nil {
		t.Fatal(err)
	}
	if second.Event != eventAlready {
		t.Fatalf("second run event = %s", second.Event)
	}
}

// TestRealReferenceApkLabelReadsEchoVRAndNothingElseMoves runs when NEVR_REFERENCE_APK names the reference APK.
func TestRealReferenceApkLabelReadsEchoVRAndNothingElseMoves(t *testing.T) {
	t.Parallel()
	src := referenceAPK(t)
	dir := t.TempDir()
	plain, labelled := filepath.Join(dir, "plain.apk"), filepath.Join(dir, "labelled.apk")
	if _, err := RenameApk(src, plain, DefaultFrom, DefaultTo, nil); err != nil {
		t.Fatal(err)
	}
	label := projectLabel
	rec, err := RenameApk(src, labelled, DefaultFrom, DefaultTo, &label)
	if err != nil {
		t.Fatal(err)
	}
	if *rec.LabelFrom != referenceLabel || *rec.LabelTo != projectLabel {
		t.Fatalf("labels %q -> %q", *rec.LabelFrom, *rec.LabelTo)
	}
	orderP, plainEntries := readFixture(t, plain)
	orderL, labelledEntries := readFixture(t, labelled)
	if len(orderP) != len(orderL) {
		t.Fatal("entry counts differ")
	}
	manifest := labelledEntries[manifestName].Data
	if got, _ := LabelOf(manifest); got != projectLabel {
		t.Fatalf("label = %q", got)
	}
	want, err := SetLabel(plainEntries[manifestName].Data, projectLabel)
	if err != nil || !bytes.Equal(manifest, want) {
		t.Fatalf("manifest: %v", err)
	}
	for name, e := range plainEntries {
		if name != manifestName && sha256Of(labelledEntries[name].Data) != sha256Of(e.Data) {
			t.Fatalf("%s moved", name)
		}
	}
}

// referenceAPK is the reference APK named by NEVR_REFERENCE_APK, or a skip when it is not set or not a file.
func referenceAPK(t *testing.T) string {
	t.Helper()
	src := os.Getenv(realEnv)
	if src == "" {
		t.Skip("NEVR_REFERENCE_APK does not name the reference APK")
	}
	if _, err := os.Stat(src); err != nil {
		t.Skip("NEVR_REFERENCE_APK does not name the reference APK")
	}
	return src
}
