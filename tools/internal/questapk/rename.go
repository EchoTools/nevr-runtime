package questapk

import (
	"archive/zip"
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"unicode/utf16"
)

// The reference build ships as package com.readyatdawn.afk; the sentinel and the reference libr15 use com.readyatdawn.r15
// (tools/quest_apk_rename_package.py's docstring holds the reasons). The rename is in place: the two names have the same
// length, so no offset moves.
const (
	// RenameTag is the log tag of every record the rename writes.
	RenameTag = "NEVR.QUEST.RENAME"
	// DefaultFrom is the reference build's package.
	DefaultFrom = "com.readyatdawn.afk"
	// DefaultTo is the package the sentinel and the reference libr15 use.
	DefaultTo = "com.readyatdawn.r15"

	manifestName    = "AndroidManifest.xml"
	libR15          = "lib/arm64-v8a/libr15.so"
	libAssetPatch   = "lib/arm64-v8a/libassetpatch.so"
	libR15Count     = 5
	assetPatchCount = 2
	metaInfPrefix   = "META-INF/"
	metaInfDir      = "META-INF"
	partSuffix      = ".part"
	dirSuffix       = "/"
	eventRenamed    = "package_renamed"
	eventAlready    = "package_already_renamed"
)

// expectation is one named entry of the rename and the occurrences of the source package it must hold.
type expectation struct {
	name        string
	ascii, wide int
}

// expected is the entries the rename names, in the order the record lists them, with each count asserted.
var expected = []expectation{
	{name: manifestName, ascii: 0, wide: 1},
	{name: libR15, ascii: libR15Count, wide: 0},
	{name: libAssetPatch, ascii: assetPatchCount, wide: 0},
}

// RenameError is a refusal: nothing is written.
type RenameError struct{ Msg string }

func (e *RenameError) Error() string { return e.Msg }

func renameErr(format string, args ...any) error {
	return &RenameError{Msg: fmt.Sprintf(format, args...)}
}

// pairLen is the two counts a pair holds: ASCII and UTF-16LE.
const pairLen = 2

// pair is the (ascii, utf16le) occurrence counts of a string in one entry.
type pair [pairLen]int

// countEntry is one named entry's counts in a record.
type countEntry struct {
	name string
	c    pair
}

// changedEntry is one entry the rename rewrote, with the sha256 of its bytes before and after.
type changedEntry struct {
	name, in, out string
}

// orderedCounts writes its entries as one object, in the order they are held.
type orderedCounts []countEntry

// MarshalJSON writes the counts as an object keyed by entry name.
func (o orderedCounts) MarshalJSON() ([]byte, error) {
	fields := make([]Field, 0, len(o))
	for _, e := range o {
		fields = append(fields, Field{Key: e.name, Value: []int{e.c[0], e.c[1]}})
	}
	line, err := Line(fields...)
	return []byte(line), err
}

// orderedChanged writes its entries as one object, in the order they are held.
type orderedChanged []changedEntry

// MarshalJSON writes the changed entries as an object keyed by entry name.
func (o orderedChanged) MarshalJSON() ([]byte, error) {
	fields := make([]Field, 0, len(o))
	for _, e := range o {
		fields = append(fields, Field{Key: e.name, Value: map[string]string{"sha256_in": e.in, "sha256_out": e.out}})
	}
	line, err := Line(fields...)
	return []byte(line), err
}

// Record is the one line a rename writes: what it read, what it changed and the hash of its output.
type Record struct {
	Apk, Out, From, To string
	DroppedMetaInf     int
	Event              string
	Counts             orderedCounts
	Changed            orderedChanged
	LabelFrom, LabelTo *string
	EntriesOut         int
	Sha256Out          string
}

// Line is the record as one JSON line, keys in the order the Python tool wrote them.
func (r Record) Line() (string, error) {
	fields := []Field{
		{"tag", RenameTag}, {"apk", r.Apk}, {"out", r.Out}, {"from", r.From}, {"to", r.To},
		{"dropped_meta_inf", r.DroppedMetaInf}, {"event", r.Event}, {"counts", r.Counts}, {"changed", r.Changed},
	}
	if r.LabelFrom != nil {
		fields = append(fields, Field{"label_from", *r.LabelFrom}, Field{"label_to", *r.LabelTo})
	}
	fields = append(fields, Field{"entries_out", r.EntriesOut}, Field{"sha256_out", r.Sha256Out})
	return Line(fields...)
}

// sha is the lower-case hex sha256 of data.
func sha(data []byte) string {
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:])
}

// forms is the ASCII and UTF-16LE encodings of an ASCII text.
func forms(text string) ([]byte, []byte) {
	return []byte(text), utf16LE(text)
}

// counts is the number of ASCII and UTF-16LE occurrences of text in data, without overlap.
func counts(data []byte, text string) pair {
	a, w := forms(text)
	return pair{bytes.Count(data, a), bytes.Count(data, w)}
}

// poolStrings is the manifest's string pool as text. The rename counts UTF-16LE strings, so a UTF-8 pool is refused.
func poolStrings(data []byte) ([]string, error) {
	kind, headerSize, size, err := chunkFields(data, 0)
	if err != nil {
		return nil, renameErr("%s does not parse: %s", manifestName, err)
	}
	if len(data) < chunkHeader || kind != resXML || headerSize != chunkHeader || size != len(data) {
		return nil, renameErr("%s is not a compiled XML document", manifestName)
	}
	for pos := chunkHeader; pos < len(data); {
		c, err := readChunk(data, pos, len(data))
		if err != nil {
			return nil, renameErr("%s does not parse: %s", manifestName, err)
		}
		if c.kind == resStringPool {
			flags, err := u32At(data, pos+poolFlagsOffset)
			if err != nil {
				return nil, renameErr("%s does not parse: %s", manifestName, err)
			}
			if flags&utf8Flag != 0 {
				return nil, renameErr("the manifest's string pool is UTF-8; the rename counts UTF-16LE strings")
			}
			strs, err := stringPoolStrings(data, pos, c.headerSize, c.size)
			if err != nil {
				return nil, renameErr("%s does not parse: %s", manifestName, err)
			}
			return strs, nil
		}
		pos += c.size
	}
	return nil, renameErr("%s has no string pool", manifestName)
}

// renameManifest is the manifest with old replaced by new in its UTF-16LE strings; the pool must read back as the original
// with exactly that substitution, and the length must not move.
func renameManifest(data []byte, old, new string) ([]byte, error) {
	before, err := poolStrings(data)
	if err != nil {
		return nil, err
	}
	out := bytes.ReplaceAll(data, utf16LE(old), utf16LE(new))
	after, err := poolStrings(out)
	if err != nil {
		return nil, err
	}
	if len(out) != len(data) || !equalStrings(after, substituteAll(before, old, new)) {
		return nil, renameErr("the renamed manifest's string pool is not the original with the substitution")
	}
	return out, nil
}

// equalStrings reports whether two string slices hold the same strings in the same order.
func equalStrings(a, b []string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

// substituteAll is each string with old replaced by new.
func substituteAll(strs []string, old, new string) []string {
	out := make([]string, len(strs))
	for i, s := range strs {
		out[i] = strings.ReplaceAll(s, old, new)
	}
	return out
}

// renameLibrary is data with every ASCII old replaced by new; the length must not move.
func renameLibrary(data []byte, old, new string) ([]byte, error) {
	out := bytes.ReplaceAll(data, []byte(old), []byte(new))
	if len(out) != len(data) {
		return nil, renameErr("the renamed library changed length")
	}
	return out, nil
}

// checkNames refuses names that are not one non-empty ASCII string each, of equal length and not the same.
func checkNames(old, new string) error {
	for _, name := range []string{old, new} {
		if name == "" || !isASCII(name) || strings.ContainsRune(name, 0) {
			return renameErr("package name %q must be non-empty ASCII", name)
		}
	}
	if len(old) != len(new) {
		return renameErr("%q and %q differ in length: the rename is in place and never moves a byte", old, new)
	}
	if old == new {
		return renameErr("the source and target package names are the same")
	}
	return nil
}

// inMetaInf reports whether an entry is under META-INF (the signature files, which the repack re-signs).
func inMetaInf(name string) bool {
	return strings.HasPrefix(name, metaInfPrefix) || name == metaInfDir
}

// relabel sets the manifest's launcher label to label and records the label before and after.
func relabel(data []byte, label string, rec *Record) ([]byte, error) {
	before, err := LabelOf(data)
	if err != nil {
		return nil, renameErr("%s: the launcher label was not set to %q: %s", manifestName, label, err)
	}
	out, err := SetLabel(data, label)
	if err != nil {
		return nil, renameErr("%s: the launcher label was not set to %q: %s", manifestName, label, err)
	}
	after, err := LabelOf(out)
	if err != nil {
		return nil, renameErr("%s: the launcher label was not set to %q: %s", manifestName, label, err)
	}
	rec.LabelFrom, rec.LabelTo = &before, &after
	for i := range rec.Changed {
		if rec.Changed[i].name == manifestName {
			rec.Changed[i].out = sha(out)
			return out, nil
		}
	}
	if !bytes.Equal(out, data) {
		rec.Changed = append(rec.Changed, changedEntry{name: manifestName, in: sha(data), out: sha(out)})
	}
	return out, nil
}

// expectationOf is the expectation of a named entry, and whether it is one.
func expectationOf(name string) (expectation, bool) {
	for _, e := range expected {
		if e.name == name {
			return e, true
		}
	}
	return expectation{}, false
}

// RenameApk writes dst from src with the source package renamed, returning the record of what it did; a RenameError (and
// nothing written) unless every rule of the Python tool's docstring holds. label, when set, also sets the manifest's
// launcher label.
func RenameApk(src, dst, old, new string, label *string) (Record, error) {
	if err := checkNames(old, new); err != nil {
		return Record{}, err
	}
	if sameFile(src, dst) {
		return Record{}, renameErr("the output is the input file: the tool never edits an APK in place")
	}
	zr, err := zip.OpenReader(src)
	if err != nil {
		return Record{}, err
	}
	defer zr.Close()
	entries, err := readEntries(zr)
	if err != nil {
		return Record{}, err
	}
	rec := Record{Apk: filepath.Base(src), Out: filepath.Base(dst), From: old, To: new}
	names := make([]string, 0, len(zr.File))
	for _, f := range zr.File {
		names = append(names, f.Name)
		if inMetaInf(f.Name) {
			rec.DroppedMetaInf++
		}
	}
	found, targetFound := map[string]pair{}, map[string]pair{}
	for _, e := range entries {
		hit := counts(e.data, old)
		if hit[0] != 0 || hit[1] != 0 {
			found[e.name] = hit
		}
		if _, ok := expectationOf(e.name); ok {
			targetFound[e.name] = counts(e.data, new)
		}
	}
	if err := checkPresence(names); err != nil {
		return Record{}, err
	}
	if err := checkUnnamed(found); err != nil {
		return Record{}, err
	}
	already := len(found) == 0
	if already {
		if err := checkAlreadyRenamed(targetFound); err != nil {
			return Record{}, err
		}
		rec.Event = eventAlready
		rec.Counts = countsInOrder(targetFound)
	} else {
		for _, e := range expected {
			if found[e.name] != (pair{e.ascii, e.wide}) {
				return Record{}, renameErr("%s holds the source package string ascii/utf16le (%d, %d), expected (%d, %d); nothing written",
					e.name, found[e.name][0], found[e.name][1], e.ascii, e.wide)
			}
		}
		rec.Event = eventRenamed
		rec.Counts = countsInOrder(found)
	}
	if err := writeRenamed(zr, dst, &rec, found, old, new, already, label); err != nil {
		return Record{}, err
	}
	rec.EntriesOut = len(zr.File) - rec.DroppedMetaInf
	out, err := os.ReadFile(dst)
	if err != nil {
		return Record{}, err
	}
	rec.Sha256Out = sha(out)
	return rec, nil
}

// entry is one zip entry's name and bytes; a directory has none.
type entry struct {
	name string
	data []byte
}

// readEntries reads every entry that is not a directory, refusing a duplicate name.
func readEntries(zr *zip.ReadCloser) ([]entry, error) {
	seen := map[string]bool{}
	var out []entry
	for _, f := range zr.File {
		if seen[f.Name] {
			return nil, renameErr("the APK has duplicate entry names")
		}
		seen[f.Name] = true
		if strings.HasSuffix(f.Name, dirSuffix) || inMetaInf(f.Name) {
			continue
		}
		r, err := f.Open()
		if err != nil {
			return nil, err
		}
		data, err := io.ReadAll(r)
		r.Close()
		if err != nil {
			return nil, err
		}
		out = append(out, entry{name: f.Name, data: data})
	}
	return out, nil
}

// checkPresence refuses an APK missing a named entry.
func checkPresence(names []string) error {
	have := map[string]bool{}
	for _, n := range names {
		have[n] = true
	}
	var missing []string
	for _, e := range expected {
		if !have[e.name] {
			missing = append(missing, e.name)
		}
	}
	if len(missing) > 0 {
		return renameErr("the APK has no entry %s", strings.Join(missing, ", "))
	}
	return nil
}

// checkUnnamed refuses the source string in an entry the rename does not name; it lists every such entry.
func checkUnnamed(found map[string]pair) error {
	var names []string
	for n := range found {
		if _, ok := expectationOf(n); !ok {
			names = append(names, n)
		}
	}
	if len(names) == 0 {
		return nil
	}
	sort.Strings(names)
	parts := make([]string, 0, len(names))
	for _, n := range names {
		parts = append(parts, fmt.Sprintf("%s ascii=%d utf16le=%d", n, found[n][0], found[n][1]))
	}
	return renameErr("the source package string is in entries the rename does not name (not renamed, decide each): %s",
		strings.Join(parts, "; "))
}

// checkAlreadyRenamed accepts an input that holds the target at least as often as the source was expected.
func checkAlreadyRenamed(targetFound map[string]pair) error {
	for _, e := range expected {
		have := targetFound[e.name]
		if have[0] < e.ascii || have[1] < e.wide {
			return renameErr("%s holds neither the source package string nor the target at least %d ASCII and %d UTF-16LE times (has ascii=%d utf16le=%d): not a reference APK this tool knows",
				e.name, e.ascii, e.wide, have[0], have[1])
		}
	}
	return nil
}

// countsInOrder is the counts of the named entries, in the order they are listed.
func countsInOrder(m map[string]pair) orderedCounts {
	out := make(orderedCounts, 0, len(expected))
	for _, e := range expected {
		out = append(out, countEntry{name: e.name, c: m[e.name]})
	}
	return out
}

// writeRenamed writes dst through a .part file: every entry but META-INF, renamed where it is named, relabelled when a
// label is asked for. The file replaces dst only when it is whole.
func writeRenamed(zr *zip.ReadCloser, dst string, rec *Record, found map[string]pair, old, new string, already bool, label *string) error {
	tmp := dst + partSuffix
	defer os.Remove(tmp)
	out, err := os.Create(tmp)
	if err != nil {
		return err
	}
	zw := zip.NewWriter(out)
	for _, f := range zr.File {
		if inMetaInf(f.Name) {
			continue
		}
		data, err := readFile(f)
		if err != nil {
			out.Close()
			return err
		}
		if err := renameEntry(f.Name, &data, rec, found, old, new, already); err != nil {
			out.Close()
			return err
		}
		if label != nil && f.Name == manifestName {
			if data, err = relabel(data, *label, rec); err != nil {
				out.Close()
				return err
			}
		}
		if err := writeEntry(zw, f, data); err != nil {
			out.Close()
			return err
		}
	}
	if err := zw.Close(); err != nil {
		out.Close()
		return err
	}
	if err := out.Close(); err != nil {
		return err
	}
	return os.Rename(tmp, dst)
}

// renameEntry rewrites the named entry data in place when the rename is not an already-renamed input, and records it.
func renameEntry(name string, data *[]byte, rec *Record, found map[string]pair, old, new string, already bool) error {
	if _, ok := expectationOf(name); !ok || already {
		return nil
	}
	in := *data
	var out []byte
	var err error
	if name == manifestName {
		out, err = renameManifest(in, old, new)
	} else {
		out, err = renameLibrary(in, old, new)
	}
	if err != nil {
		return err
	}
	left := counts(out, old)
	gained := counts(out, new)
	gainedIdx := 0
	if name == manifestName {
		gainedIdx = 1
	}
	if left != (pair{0, 0}) || gained[gainedIdx] < found[name][0]+found[name][1] {
		return renameErr("%s: the result still holds the source string, or lost the target", name)
	}
	rec.Changed = append(rec.Changed, changedEntry{name: name, in: sha(in), out: sha(out)})
	*data = out
	return nil
}

// writeEntry writes one entry with its name, method, timestamp and attributes, and no extra fields.
func writeEntry(zw *zip.Writer, f *zip.File, data []byte) error {
	fh := &zip.FileHeader{
		Name:           f.Name,
		Method:         f.Method,
		Modified:       f.Modified.UTC(),
		ExternalAttrs:  f.ExternalAttrs,
		CreatorVersion: f.CreatorVersion,
		Comment:        f.Comment,
	}
	w, err := zw.CreateHeader(fh)
	if err != nil {
		return err
	}
	_, err = w.Write(data)
	return err
}

// readFile is an entry's bytes.
func readFile(f *zip.File) ([]byte, error) {
	r, err := f.Open()
	if err != nil {
		return nil, err
	}
	defer r.Close()
	return io.ReadAll(r)
}

// sameFile reports whether two paths name one file, comparing the resolved paths and the file identity.
func sameFile(a, b string) bool {
	absA, errA := filepath.Abs(a)
	absB, errB := filepath.Abs(b)
	if errA == nil && errB == nil && absA == absB {
		return true
	}
	infoA, errA := os.Stat(a)
	infoB, errB := os.Stat(b)
	return errA == nil && errB == nil && os.SameFile(infoA, infoB)
}

// utf16LE is an ASCII text as UTF-16 little-endian bytes.
func utf16LE(s string) []byte {
	out := make([]byte, 0, unitBytes*len(s))
	for _, u := range utf16.Encode([]rune(s)) {
		out = append(out, byte(u), byte(u>>byteBits))
	}
	return out
}
