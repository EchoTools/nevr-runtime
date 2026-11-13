// Package questapkfix builds the fixture APKs of the Quest APK tools' tests: the reference APK's shape (the source package
// string where the rename counts it, one raw launcher label) and the reader that compares what an APK holds. It is for
// tests only: nothing in the tools imports it. The fixtures are the Go twin of the Python tests' entries() and read_apk().
package questapkfix

import (
	"archive/zip"
	"bytes"
	"io"
	"os"
	"time"
	"unicode/utf16"

	"github.com/EchoTools/nevr-runtime/tools/internal/axmlfix"
	"github.com/EchoTools/nevr-runtime/tools/internal/testkit"
)

// The reference build's names and the project's.
const (
	From           = "com.readyatdawn.afk"
	To             = "com.readyatdawn.r15"
	ReferenceLabel = "Echo Combat (port)"
	ManifestName   = "AndroidManifest.xml"
	LibR15         = "lib/arm64-v8a/libr15.so"
	LibAssetPatch  = "lib/arm64-v8a/libassetpatch.so"
	LibVRAPI       = "lib/arm64-v8a/libvrapi.so"
	// LibR15Count and AssetPatchCount are the occurrences of From the reference APK holds in each library.
	LibR15Count     = 5
	AssetPatchCount = 2

	deflate         = zip.Deflate
	stored          = zip.Store
	fixtureSalt     = "\x7fELF-r15-"
	assetSalt       = "\x7fELF-ap-"
	tailRepeats     = 3
	byteValues      = 256
	fixtureYear     = 2026
	fixtureMonth    = 10
	fixtureDay      = 30
	fixtureHour     = 12
	allBytesRepeats = 4
)

// Date is the timestamp every fixture entry carries.
var Date = time.Date(fixtureYear, fixtureMonth, fixtureDay, fixtureHour, 0, 0, 0, time.UTC)

// Entry is one entry of a fixture APK: its name, its bytes and its compression method.
type Entry struct {
	Name   string
	Data   []byte
	Method uint16
}

// ReadEntry is one entry of an APK as it reads back: its bytes, method and timestamp.
type ReadEntry struct {
	Name   string
	Data   []byte
	Method uint16
	When   time.Time
}

// Deflate and Stored are the two compression methods the fixtures use.
const (
	Deflate = deflate
	Stored  = stored
)

// Wide is text as UTF-16 little-endian bytes.
func Wide(text string) []byte {
	var out []byte
	for _, u := range utf16.Encode([]rune(text)) {
		out = append(out, byte(u), byte(u>>8))
	}
	return out
}

// AfkManifest is the reference build's manifest: the fixture manifest with its target string swapped for the source
// (UTF-16 by default, as the Python fixture is).
func AfkManifest(utf8 bool) []byte {
	s := axmlfix.Default()
	s.UTF8 = utf8
	data := s.Build()
	swapped := bytes.ReplaceAll(data, Wide(To), Wide(From))
	if utf8 {
		swapped = bytes.ReplaceAll(data, []byte(To), []byte(From))
	}
	if bytes.Equal(swapped, data) {
		panic("questapkfix: the target string is not in the fixture manifest")
	}
	return swapped
}

// LibWith is a library holding old count times after a salt, each occurrence in its own record.
func LibWith(old string, count int, salt string) []byte {
	body := []byte(salt)
	for i := 0; i < count; i++ {
		body = append(body, []byte("\x00/data/data/"+old+"/files\x00pad")...)
		body = append(body, byte(i))
	}
	return append(body, bytes.Repeat([]byte("\x00tail"), tailRepeats)...)
}

// allBytes is the byte values in order.
func allBytes() []byte {
	out := make([]byte, byteValues)
	for i := range out {
		out[i] = byte(i)
	}
	return out
}

// ReferenceEntries is the fixture reference APK's entries in a fixed order: libr15 holding the source libr15Count times,
// assetpatch assetPatchCount times. A nil manifest is AfkManifest(false); extra entries follow.
func ReferenceEntries(libr15Count, assetPatchCount int, manifest []byte, extra ...Entry) []Entry {
	if manifest == nil {
		manifest = AfkManifest(false)
	}
	out := []Entry{
		{"META-INF/MANIFEST.MF", []byte("Manifest-Version: 1.0\n"), deflate},
		{"META-INF/ECHOVRTE.SF", []byte("Signature-Version: 1.0\n"), deflate},
		{"META-INF/ECHOVRTE.RSA", []byte("\x30\x82rsa"), stored},
		{ManifestName, manifest, deflate},
		{"classes.dex", concat([]byte("dex\n035\x00"), []byte(To+"\x00"), bytes.Repeat(allBytes(), allBytesRepeats)), deflate},
		{"resources.arsc", concat([]byte("arsc"), Wide(ReferenceLabel), Wide(To)), stored},
		{LibR15, append(LibWith(From, libr15Count, fixtureSalt), []byte(To)...), deflate},
		{LibVRAPI, []byte("\x7fELF-vrapi" + To + "\x00" + To), deflate},
		{LibAssetPatch, LibWith(From, assetPatchCount, assetSalt), deflate},
		{"assets/afk_default_on.txt", []byte("ecplayo.on\ndronekill2.on\n"), stored},
	}
	return append(out, extra...)
}

// concat joins byte slices into a new one.
func concat(parts ...[]byte) []byte {
	var out []byte
	for _, p := range parts {
		out = append(out, p...)
	}
	return out
}

// Write writes entries as an APK at path, each with the fixture timestamp.
func Write(t testkit.T, path string, entries []Entry) {
	t.Helper()
	out, err := os.Create(path)
	if err != nil {
		t.Fatalf("questapkfix.Write: %v", err)
	}
	defer out.Close()
	zw := zip.NewWriter(out)
	for _, e := range entries {
		w, err := zw.CreateHeader(&zip.FileHeader{Name: e.Name, Method: e.Method, Modified: Date})
		if err != nil {
			t.Fatalf("questapkfix.Write: %v", err)
		}
		if _, err := w.Write(e.Data); err != nil {
			t.Fatalf("questapkfix.Write: %v", err)
		}
	}
	if err := zw.Close(); err != nil {
		t.Fatalf("questapkfix.Write: %v", err)
	}
}

// Read reads an APK's entries: in archive order, and by name.
func Read(t testkit.T, path string) ([]ReadEntry, map[string]ReadEntry) {
	t.Helper()
	zr, err := zip.OpenReader(path)
	if err != nil {
		t.Fatalf("questapkfix.Read: %v", err)
	}
	defer zr.Close()
	var order []ReadEntry
	byName := map[string]ReadEntry{}
	for _, f := range zr.File {
		r, err := f.Open()
		if err != nil {
			t.Fatalf("questapkfix.Read: %v", err)
		}
		data, err := io.ReadAll(r)
		r.Close()
		if err != nil {
			t.Fatalf("questapkfix.Read: %v", err)
		}
		e := ReadEntry{Name: f.Name, Data: data, Method: f.Method, When: f.Modified.UTC()}
		order = append(order, e)
		byName[f.Name] = e
	}
	return order, byName
}

// LabelledManifest is the reference build's manifest with its one raw label, the shape the label editor edits; shareLabel
// adds a second attribute that references the same string.
func LabelledManifest(pkg, label string, shareLabel bool) []byte {
	spec := axmlfix.LabelManifest(pkg, label)
	spec.ShareLabel = shareLabel
	return spec.Build()
}
