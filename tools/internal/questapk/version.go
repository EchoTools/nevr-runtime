package questapk

import (
	"archive/zip"
	"bytes"
	"encoding/binary"
	"fmt"
	"io"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
)

// The numbering rule (the reasons are in the Python original's docstring, tools/quest_apk_version.py):
//
//	versionCode = major * 10^7 + minor * 10^5 + patch * 10^3 + slot
//	a release       X.Y.Z                  slot 999
//	a development   X.Y.Z-dev.N+<sha>      slot N (N = commits since the highest reachable release tag, 0..998)
const (
	majorScale   = 10_000_000
	minorScale   = 100_000
	patchScale   = 1_000
	releaseSlot  = 999
	maxDevN      = releaseSlot - 1
	maxMajor     = 209
	maxMinor     = 99
	maxPatch     = 99
	warnDevN     = 900
	warnMinorPat = 90
	// MaxVersionCode is Android's documented ceiling for a versionCode (the Play Store's 2,100,000,000).
	MaxVersionCode = 2_100_000_000
)

// The compiled manifest's versionCode attribute and the names the reader compares.
const (
	resIDVersionCode      uint32 = 0x0101021B
	resIDVersionCodeMajor uint32 = 0x01010576
	androidNS                    = "http://schemas.android.com/apk/res/android"
	manifestEntry                = "AndroidManifest.xml"
	releaseTagPattern            = `^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$`
	versionPattern               = `^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-dev\.(0|[1-9][0-9]*)(?:-(?:store|combat))?\+[0-9a-f]{7,40})?$`
	nodeNameManifest             = "manifest"
	versionCodeAttr              = "versionCode"
	versionCodeMajorAttr         = "versionCodeMajor"
	attrValueSize                = 8
	patchByteWidth               = 4
	elementAttrOffset            = 16
	rootChunkPrefix              = 8
	attrStartPos                 = 8
	attrSizePos                  = 10
	attrCountPos                 = 12
	attrNamespaceEnd             = 4
	attrDataPos                  = 16
	attrNameOff                  = 4
	attrRawOff                   = 8
	attrSizeOff                  = 12
	attrRes0Off                  = 14
	attrTypeOff                  = 15
	versionParts                 = 3
)

var (
	versionRE = regexp.MustCompile(versionPattern)
	releaseRE = regexp.MustCompile(releaseTagPattern)
	// buildVersionRE is the kVersion line of nevr_build_info.h (cmake/nevr_build_info.h.in).
	buildVersionRE = regexp.MustCompile(`\bkVersion\s*=\s*"([^"]*)";`)
)

// VersionError is a version string the numbering rule does not accept, or a stamp the floor refuses.
type VersionError struct{ Msg string }

func (e *VersionError) Error() string { return e.Msg }

func versionErr(format string, args ...any) error {
	return &VersionError{Msg: fmt.Sprintf(format, args...)}
}

// parts is a version string split into its fields. release is true for X.Y.Z; otherwise n is the development distance.
type parts struct {
	major, minor, patch int
	release             bool
	n                   int
}

// parseParts splits version, or reports that it is neither X.Y.Z nor X.Y.Z-dev.N+<sha>.
func parseParts(version string) (parts, bool) {
	m := versionRE.FindStringSubmatch(version)
	if m == nil {
		return parts{}, false
	}
	p := parts{major: satAtoi(m[1]), minor: satAtoi(m[2]), patch: satAtoi(m[3]), release: m[4] == ""}
	if !p.release {
		p.n = satAtoi(m[4])
	}
	return p, true
}

// satAtoi is a decimal field's value, saturated at the largest int, so a field too large to count is refused by its limit.
func satAtoi(s string) int {
	v, err := strconv.ParseInt(s, 10, 64)
	if err != nil {
		return math.MaxInt
	}
	return int(v)
}

// fieldLimit is one part of a version and its limit.
type fieldLimit struct {
	name         string
	value, limit int
}

// limitedFields is the parts of version the rule limits, in the order the messages name them.
func limitedFields(p parts) []fieldLimit {
	fields := []fieldLimit{{"major", p.major, maxMajor}, {"minor", p.minor, maxMinor}, {"patch", p.patch, maxPatch}}
	if !p.release {
		fields = append(fields, fieldLimit{"development distance N", p.n, maxDevN})
	}
	return fields
}

// FieldProblems is one message per part of version outside the rule's limits, each naming the field. Empty for a version
// that does not parse (VersionCodeFor is what refuses that).
func FieldProblems(version string) []string {
	p, ok := parseParts(version)
	if !ok {
		return nil
	}
	var out []string
	for _, f := range limitedFields(p) {
		if f.value > f.limit {
			out = append(out, fmt.Sprintf("version '%s': %s %d is above %d, the largest the versionCode rule encodes", version, f.name, f.value, f.limit))
		}
	}
	return out
}

// FieldWarnings is one message per part within the limits but close to them (development distance N above 900, minor or
// patch above 90), each naming the field and the limit it approaches. Never fails.
func FieldWarnings(version string) []string {
	p, ok := parseParts(version)
	if !ok {
		return nil
	}
	var out []string
	if !p.release && p.n > warnDevN && p.n <= maxDevN {
		out = append(out, fmt.Sprintf("version '%s': development distance N %d is above %d; the versionCode rule refuses above %d",
			version, p.n, warnDevN, maxDevN))
	}
	for _, f := range []fieldLimit{{"minor", p.minor, maxMinor}, {"patch", p.patch, maxPatch}} {
		if f.value > warnMinorPat && f.value <= f.limit {
			out = append(out, fmt.Sprintf("version '%s': %s %d is above %d; the versionCode rule refuses above %d",
				version, f.name, f.value, warnMinorPat, f.limit))
		}
	}
	return out
}

// VersionCodeFor is the numbering rule's code for a version: its arithmetic and its field limits, and no floor. A stamp
// goes through StampCodeFor, which also refuses a version below the newest release's major.
func VersionCodeFor(version string) (int, error) {
	p, ok := parseParts(version)
	if !ok {
		return 0, versionErr("version '%s' is neither X.Y.Z nor X.Y.Z-dev.N+<sha> (a release tag, or a development build)", version)
	}
	if problems := FieldProblems(version); len(problems) > 0 {
		return 0, versionErr("%s", strings.Join(problems, "; "))
	}
	slot := releaseSlot
	if !p.release {
		slot = p.n
	}
	return p.major*majorScale + p.minor*minorScale + p.patch*patchScale + slot, nil
}

// ReleaseSource is the newest release tag's name and major, read when a stamp needs the floor.
type ReleaseSource func() (string, int, error)

// NewestRelease is the (tag, major) of the newest vX.Y.Z tag git lists in repo. Every release tag the clone knows counts,
// whether or not HEAD descends from it. env is the git command's environment (nil inherits the process's).
func NewestRelease(repo string, env []string) (string, int, error) {
	cmd := exec.Command("git", "-C", repo, "tag", "--list", "v*")
	cmd.Env = env
	var stdout, stderr bytes.Buffer
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr
	if err := cmd.Run(); err != nil {
		if exitErr, ok := err.(*exec.ExitError); ok {
			return "", 0, versionErr("git cannot list the tags of %s (exit %d): %s", repo, exitErr.ExitCode(), strings.TrimSpace(stderr.String()))
		}
		return "", 0, versionErr("cannot run git to list the release tags of %s: %s", repo, err)
	}
	bestName := ""
	var best [versionParts]int
	found := false
	for _, name := range strings.Fields(stdout.String()) {
		m := releaseRE.FindStringSubmatch(name)
		if m == nil {
			continue
		}
		v := [versionParts]int{satAtoi(m[1]), satAtoi(m[2]), satAtoi(m[3])}
		if !found || lessVersion(best, v) {
			bestName, best, found = name, v, true
		}
	}
	if !found {
		return "", 0, versionErr("no release tag (vX.Y.Z) in %s: fetch the tags (git fetch --tags; CI checks out with fetch-depth: 0)", repo)
	}
	return bestName, best[0], nil
}

// lessVersion orders (major, minor, patch) triples.
func lessVersion(a, b [versionParts]int) bool {
	for i := range a {
		if a[i] != b[i] {
			return a[i] < b[i]
		}
	}
	return false
}

// StampCodeFor is VersionCodeFor, refused when the version's major is below the newest release tag's. A build with no git
// reports 1.0.0 (cmake/set_project_version_from_git.cmake), and a stamp below the newest release's major is a downgrade
// for a headset on that release.
func StampCodeFor(version string, newest ReleaseSource) (int, error) {
	code, err := VersionCodeFor(version)
	if err != nil {
		return 0, err
	}
	tag, floor, err := newest()
	if err != nil {
		return 0, err
	}
	p, _ := parseParts(version)
	if p.major < floor {
		return 0, versionErr("version '%s' is below major %d, the major of the newest release tag %s: a stamp that low would be a downgrade for a headset on %s (Android refuses a lower versionCode)",
			version, floor, tag, tag)
	}
	return code, nil
}

// ManifestVersion is the versionCode of a compiled manifest and the offset of its 4 bytes.
type ManifestVersion struct {
	VersionCode int
	Offset      int
}

// packCode writes a versionCode into the manifest bytes at off. It is a variable so a test can plant a stray write.
var packCode = func(b []byte, off int, code uint32) { binary.LittleEndian.PutUint32(b[off:], code) }

// ParseManifest is the versionCode of a compiled AndroidManifest.xml and where its 4 bytes sit; a ManifestError unless
// the field is exactly where Android reads it.
func ParseManifest(data []byte) (ManifestVersion, error) {
	if len(data) < chunkHeader {
		return ManifestVersion{}, manifestErr("not a compiled XML document: shorter than a chunk header")
	}
	kind, headerSize, size, err := chunkFields(data, 0)
	if err != nil {
		return ManifestVersion{}, err
	}
	if kind != resXML || headerSize != chunkHeader || size != len(data) {
		return ManifestVersion{}, manifestErr("not a compiled XML document: type 0x%04x, header %d, size %d of %d bytes",
			kind, headerSize, size, len(data))
	}
	doc, err := scanDocument(data)
	if err != nil {
		return ManifestVersion{}, err
	}
	return rootVersionCode(data, doc)
}

// document is what ParseManifest finds in one pass over the chunks: the strings, the resource ids and the root element.
type document struct {
	strs      []string
	ids       []uint32
	root      chunkInfo
	rootPos   int
	haveRoot  bool
	haveStrs  bool
	haveIDMap bool
}

// scanDocument reads the chunks after the document header, in order, and refuses a layout the reader does not know.
func scanDocument(data []byte) (document, error) {
	var doc document
	for pos := chunkHeader; pos < len(data); {
		c, err := readChunk(data, pos, len(data))
		if err != nil {
			return document{}, err
		}
		switch c.kind {
		case resStringPool:
			if doc.haveStrs || doc.haveRoot {
				return document{}, manifestErr("a second string pool, or one after the first element")
			}
			if doc.strs, err = stringPoolStrings(data, pos, c.headerSize, c.size); err != nil {
				return document{}, err
			}
			doc.haveStrs = true
		case resXMLResourceMap:
			if doc.haveIDMap || doc.haveRoot {
				return document{}, manifestErr("a second resource map, or one after the first element")
			}
			if c.headerSize != resMapHeader {
				return document{}, manifestErr("resource map header size %d, expected 8", c.headerSize)
			}
			if doc.ids, err = readUint32s(data, pos+resMapHeader, (c.size-resMapHeader)/wordBytes); err != nil {
				return document{}, err
			}
			doc.haveIDMap = true
		case resXMLStartElement:
			if !doc.haveRoot {
				doc.root, doc.rootPos, doc.haveRoot = c, pos, true
			}
		case resXMLStartNS, resXMLEndNS, resXMLEndElement, resXMLCDATA:
		default:
			return document{}, manifestErr("unknown chunk type 0x%04x at byte %d", c.kind, pos)
		}
		pos += c.size
	}
	if !doc.haveStrs || !doc.haveIDMap || !doc.haveRoot {
		return document{}, manifestErr("no string pool, resource map or element: the version attribute cannot be located by resource id")
	}
	return doc, nil
}

// chunkFields reads a chunk's kind, header size and size at pos, with no check that the chunk is whole.
func chunkFields(data []byte, pos int) (uint16, int, int, error) {
	kind, err := u16At(data, pos)
	if err != nil {
		return 0, 0, 0, err
	}
	headerSize, err := u16At(data, pos+headerSizeOffset)
	if err != nil {
		return 0, 0, 0, err
	}
	size, err := u32At(data, pos+chunkSizeOffset)
	if err != nil {
		return 0, 0, 0, err
	}
	return kind, int(headerSize), int(size), nil
}

// nameOf is the pool string an index names; the absent index names nothing.
func nameOf(strs []string, index uint32) (string, error) {
	if index == noIndex {
		return "", nil
	}
	if int(index) >= len(strs) {
		return "", manifestErr("string index %d is outside the pool of %d", index, len(strs))
	}
	return strs[index], nil
}

// rootVersionCode finds the one versionCode attribute of the root <manifest> element.
func rootVersionCode(data []byte, doc document) (ManifestVersion, error) {
	root := doc.root
	if root.headerSize != rootHeader || root.size < rootMinSize {
		return ManifestVersion{}, manifestErr("root element chunk has header %d, size %d", root.headerSize, root.size)
	}
	base := doc.rootPos + root.headerSize
	ns, err := u32At(data, base)
	if err != nil {
		return ManifestVersion{}, err
	}
	name, err := u32At(data, base+attrNamespaceEnd)
	if err != nil {
		return ManifestVersion{}, err
	}
	attrStart, err := u16At(data, base+attrStartPos)
	if err != nil {
		return ManifestVersion{}, err
	}
	attrSize, err := u16At(data, base+attrSizePos)
	if err != nil {
		return ManifestVersion{}, err
	}
	attrCount, err := u16At(data, base+attrCountPos)
	if err != nil {
		return ManifestVersion{}, err
	}
	rootName, err := nameOf(doc.strs, name)
	if err != nil {
		return ManifestVersion{}, err
	}
	if rootName != nodeNameManifest || ns != noIndex {
		return ManifestVersion{}, manifestErr("the first element is '%s', not the root 'manifest'", rootName)
	}
	if int(attrStart) != rootAttrStart || int(attrSize) != rootAttrSize || root.headerSize+int(attrStart)+int(attrSize)*int(attrCount) > root.size {
		return ManifestVersion{}, manifestErr("root attributes: start %d, size %d, count %d do not fit the chunk", attrStart, attrSize, attrCount)
	}
	var found []attrAt
	for i := 0; i < int(attrCount); i++ {
		at := base + int(attrStart) + int(attrSize)*i
		rec, err := readAttr(data, at)
		if err != nil {
			return ManifestVersion{}, err
		}
		if int(rec.name) >= len(doc.strs) {
			return ManifestVersion{}, manifestErr("attribute %d: name index %d is outside the pool", i, rec.name)
		}
		resID := noIndex
		if int(rec.name) < len(doc.ids) {
			resID = doc.ids[rec.name]
		}
		label := doc.strs[rec.name]
		if resID == resIDVersionCodeMajor || label == versionCodeMajorAttr {
			return ManifestVersion{}, manifestErr("the manifest carries versionCodeMajor: the code Android reports would not be the one set here")
		}
		if resID == resIDVersionCode || label == versionCodeAttr {
			found = append(found, attrAt{index: i, at: at, rec: rec, resID: resID})
		}
	}
	if len(found) != 1 {
		return ManifestVersion{}, manifestErr("%d versionCode attributes on the root <manifest> (need exactly 1)", len(found))
	}
	return checkVersionCode(doc.strs, found[0])
}

// attrAt is one attribute of the root element: where its record is and what it holds.
type attrAt struct {
	index int
	at    int
	rec   attrRecord
	resID uint32
}

// checkVersionCode accepts the one versionCode attribute only when it is a plain decimal android:versionCode whose value
// is a positive int32.
func checkVersionCode(strs []string, f attrAt) (ManifestVersion, error) {
	attrNS, err := nameOf(strs, f.rec.ns)
	if err != nil {
		return ManifestVersion{}, err
	}
	label := strs[f.rec.name]
	if label != versionCodeAttr || f.resID != resIDVersionCode || attrNS != androidNS {
		return ManifestVersion{}, manifestErr("attribute %d: name '%s', resource id %d, namespace '%s' are not android:versionCode (0x%08x)",
			f.index, label, f.resID, attrNS, resIDVersionCode)
	}
	if f.rec.typ != typeIntDec || f.rec.size != attrValueSize || f.rec.res0 != 0 || f.rec.raw != noIndex {
		return ManifestVersion{}, manifestErr("versionCode is not a plain decimal integer (type 0x%02x, size %d, raw text index %#x): it has a text twin or another type that a 4-byte write would leave stale",
			f.rec.typ, f.rec.size, f.rec.raw)
	}
	if f.rec.data == 0 || uint64(f.rec.data) >= int32Limit {
		return ManifestVersion{}, manifestErr("versionCode %d is not a positive int32", f.rec.data)
	}
	return ManifestVersion{VersionCode: int(f.rec.data), Offset: f.at + attrDataPos}, nil
}

// attrRecord is one attribute record's fields (ResourceTypes.h: ns, name, raw value, size, res0, type, data).
type attrRecord struct {
	ns, name, raw uint32
	size          uint16
	res0, typ     uint8
	data          uint32
}

// readAttr reads the attribute record at at.
func readAttr(data []byte, at int) (attrRecord, error) {
	b, err := bytesAt(data, at, attrRecordSize)
	if err != nil {
		return attrRecord{}, err
	}
	return attrRecord{
		ns:   binary.LittleEndian.Uint32(b[0:]),
		name: binary.LittleEndian.Uint32(b[attrNameOff:]),
		raw:  binary.LittleEndian.Uint32(b[attrRawOff:]),
		size: binary.LittleEndian.Uint16(b[attrSizeOff:]),
		res0: b[attrRes0Off],
		typ:  b[attrTypeOff],
		data: binary.LittleEndian.Uint32(b[attrDataPos:]),
	}, nil
}

// readUint32s reads n little-endian uint32s from pos.
func readUint32s(data []byte, pos, n int) ([]uint32, error) {
	out := make([]uint32, n)
	for i := range out {
		v, err := u32At(data, pos+wordBytes*i)
		if err != nil {
			return nil, err
		}
		out[i] = v
	}
	return out, nil
}

// PatchManifest is data with the root versionCode set to code: a 4-byte write, read back and compared.
func PatchManifest(data []byte, code int) ([]byte, error) {
	if code <= 0 || code >= MaxVersionCode {
		return nil, manifestErr("refusing to write versionCode %d", code)
	}
	before, err := ParseManifest(data)
	if err != nil {
		return nil, err
	}
	if code <= before.VersionCode {
		return nil, manifestErr("refusing to write versionCode %d: the manifest already has %d and the stamp only ever raises it",
			code, before.VersionCode)
	}
	out := append([]byte(nil), data...)
	packCode(out, before.Offset, uint32(code))
	after, err := ParseManifest(out)
	if err != nil {
		return nil, err
	}
	var changed []int
	for i := range data {
		if data[i] != out[i] {
			changed = append(changed, i)
		}
	}
	if after.VersionCode != code || after.Offset != before.Offset || len(out) != len(data) || len(changed) == 0 ||
		changed[0] < before.Offset || changed[len(changed)-1] >= before.Offset+patchByteWidth {
		return nil, manifestErr("the patched manifest does not differ from the original in exactly the versionCode bytes")
	}
	return out, nil
}

// ManifestOfAPK is the AndroidManifest.xml entry of an APK, which must be there exactly once.
func ManifestOfAPK(apk string) ([]byte, error) {
	z, err := zip.OpenReader(apk)
	if err != nil {
		return nil, err
	}
	defer z.Close()
	var hits []*zip.File
	for _, f := range z.File {
		if f.Name == manifestEntry {
			hits = append(hits, f)
		}
	}
	if len(hits) != 1 {
		return nil, manifestErr("%s: %d %s entries (need exactly 1)", filepath.Base(apk), len(hits), manifestEntry)
	}
	r, err := hits[0].Open()
	if err != nil {
		return nil, err
	}
	defer r.Close()
	return io.ReadAll(r)
}

// VersionCodeOfAPK is the versionCode in an APK's manifest.
func VersionCodeOfAPK(apk string) (int, error) {
	data, err := ManifestOfAPK(apk)
	if err != nil {
		return 0, err
	}
	m, err := ParseManifest(data)
	if err != nil {
		return 0, err
	}
	return m.VersionCode, nil
}

// ReadBuildVersion is kVersion from the nevr_build_info.h the Quest build generates.
func ReadBuildVersion(path string) (string, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return "", versionErr("cannot read %s: %s", path, errText(err))
	}
	found := buildVersionRE.FindAllStringSubmatch(string(raw), -1)
	if len(found) != 1 {
		return "", versionErr("%s: expected exactly one kVersion, found %d", path, len(found))
	}
	return found[0][1], nil
}

// errText is an OS error's own words, without the path it names.
func errText(err error) string {
	if pe, ok := err.(*os.PathError); ok {
		return pe.Err.Error()
	}
	return err.Error()
}

// ShimCarriesVersion reports whether the shim stores version as its own NUL-bounded string.
func ShimCarriesVersion(shim []byte, version string) bool {
	return bytes.Contains(shim, []byte("\x00"+version+"\x00"))
}
