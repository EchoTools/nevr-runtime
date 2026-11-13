package questrepack

import (
	"bytes"
	"encoding/binary"
	"encoding/hex"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/questrepack/repackfixture"
)

const (
	gameID  = "b243509c08ce677aeb95fa348016949b3fc45230" // the fixture libr15's build ID (the pinned one, kLibR15BuildId)
	otherID = "0123456789abcdef0123456789abcdef01234567"
)

// nevrNote is C source for a plugin's NEVR_DECLARE_SUPPORTED_BUILD_IDS note, from the real macro.
func nevrNote(t *testing.T, ids string) string {
	return repackfixture.NevrNote(t, repackfixture.RepoRoot(t), ids)
}

// pluginBuildIDTools are the tools the build-ID tests build an AArch64 object with (the Python test's AARCH64_MISSING).
var pluginBuildIDTools = []string{"clang", "ld.lld", "llvm-nm", "llvm-readelf"}

// buildIDFixture is a store libr15 with GAME_ID, in a directory of the test's own.
type buildIDFixture struct {
	t      *testing.T
	dir    string
	libr15 []byte
}

func newBuildIDFixture(t *testing.T) *buildIDFixture {
	t.Helper()
	requireTools(t, pluginBuildIDTools...)
	dir := t.TempDir()
	libr15 := repackfixture.BuildAArch64(t, filepath.Join(dir, "libr15.so"), "int game = 1;\n", gameID)
	return &buildIDFixture{t: t, dir: dir, libr15: libr15}
}

func (f *buildIDFixture) problems(source string) []string {
	f.t.Helper()
	plugin := repackfixture.BuildAArch64(f.t, filepath.Join(f.dir, "libp.so"), source, "")
	return buildIDProblems(f.libr15, plugin, "libp.so")
}

// TestPluginBuildIDs_ListingTheGameBuildPasses is test_a_plugin_listing_the_game_build_passes.
func TestPluginBuildIDs_ListingTheGameBuildPasses(t *testing.T) {
	t.Parallel()
	f := newBuildIDFixture(t)
	if got := f.problems(nevrNote(t, gameID)); len(got) != 0 {
		t.Fatalf("want none, got %q", got)
	}
}

// TestPluginBuildIDs_GameBuildAnywhereInTheList is test_the_game_build_anywhere_in_the_list_passes. The C string holds a
// tab, as the Python test's `\t` does.
func TestPluginBuildIDs_GameBuildAnywhereInTheList(t *testing.T) {
	t.Parallel()
	f := newBuildIDFixture(t)
	ids := otherID + ", " + otherID[:len(otherID)-1] + "9\\t" + gameID
	if got := f.problems(nevrNote(t, ids)); len(got) != 0 {
		t.Fatalf("want none, got %q", got)
	}
}

// TestPluginBuildIDs_NoNoteIsUndeclared is test_a_plugin_with_no_note_is_named_undeclared.
func TestPluginBuildIDs_NoNoteIsUndeclared(t *testing.T) {
	t.Parallel()
	f := newBuildIDFixture(t)
	got := f.problems("int plugin_marker = 1;\n")
	if len(got) != 1 || !strings.Contains(got[0], "build_ids_undeclared") {
		t.Fatalf("got %q", got)
	}
}

// TestPluginBuildIDs_ListWithoutGameBuildIsUnsupported is test_a_list_without_the_game_build_is_named_unsupported.
func TestPluginBuildIDs_ListWithoutGameBuildIsUnsupported(t *testing.T) {
	t.Parallel()
	f := newBuildIDFixture(t)
	got := f.problems(nevrNote(t, otherID))
	if len(got) != 1 || !strings.Contains(got[0], "build_id_unsupported") || !strings.Contains(got[0], gameID) {
		t.Fatalf("got %q", got)
	}
}

// TestPluginBuildIDs_PrefixIsNotTheGameBuild is test_a_prefix_of_the_game_build_is_not_the_game_build.
func TestPluginBuildIDs_PrefixIsNotTheGameBuild(t *testing.T) {
	t.Parallel()
	f := newBuildIDFixture(t)
	for _, ids := range []string{gameID[:len(gameID)-2], gameID + "00"} {
		got := f.problems(nevrNote(t, ids))
		if len(got) == 0 || !strings.Contains(got[0], "build_id_unsupported") {
			t.Fatalf("list %q: got %q", ids, got)
		}
	}
}

// TestPluginBuildIDs_EmptyListIsUnsupported is test_an_empty_list_is_unsupported.
func TestPluginBuildIDs_EmptyListIsUnsupported(t *testing.T) {
	t.Parallel()
	f := newBuildIDFixture(t)
	got := f.problems(nevrNote(t, ""))
	if len(got) == 0 || !strings.Contains(got[0], "an empty list") {
		t.Fatalf("got %q", got)
	}
}

// TestPluginBuildIDs_LibR15WithNoBuildIDChecksNothing is test_a_libr15_with_no_build_id_checks_nothing.
func TestPluginBuildIDs_LibR15WithNoBuildIDChecksNothing(t *testing.T) {
	t.Parallel()
	f := newBuildIDFixture(t)
	bare := repackfixture.BuildAArch64(t, filepath.Join(f.dir, "bare.so"), "int game = 1;\n", "")
	plugin := repackfixture.BuildAArch64(t, filepath.Join(f.dir, "libp.so"), nevrNote(t, gameID), "")
	got := buildIDProblems(bare, plugin, "libp.so")
	if len(got) != 1 || !strings.Contains(got[0], "has no GNU build ID") {
		t.Fatalf("got %q", got)
	}
}

// TestNoteDesc_NotA64BitELFHasNoNotes is test_a_file_that_is_not_a_64_bit_elf_has_no_notes.
func TestNoteDesc_NotA64BitELFHasNoNotes(t *testing.T) {
	t.Parallel()
	if _, ok := noteDesc([]byte("\x7fELF the game\x00"+strings.Repeat("\x7fELF the game\x00", 49)), []byte("GNU\x00"), ntGNUBuildID); ok {
		t.Fatal("a non-64-bit file has a note")
	}
	if _, ok := noteDesc(nil, []byte("GNU\x00"), ntGNUBuildID); ok {
		t.Fatal("an empty file has a note")
	}
}

// TestPluginBuildIDs_ReadAsReadelfReadsIt is test_the_build_id_is_read_as_readelf_reads_it.
func TestPluginBuildIDs_ReadAsReadelfReadsIt(t *testing.T) {
	t.Parallel()
	f := newBuildIDFixture(t)
	out, err := exec.Command("llvm-readelf", "-n", filepath.Join(f.dir, "libr15.so")).Output()
	if err != nil {
		t.Fatalf("llvm-readelf: %v", err)
	}
	if !strings.Contains(string(out), "Build ID: "+gameID) {
		t.Fatalf("readelf: %s", out)
	}
	desc, ok := noteDesc(f.libr15, []byte("GNU\x00"), ntGNUBuildID)
	if !ok || !bytes.Equal(desc, mustHex(t, gameID)) {
		t.Fatalf("note_desc: %x, %v", desc, ok)
	}
}

// TestCheckPluginBuildIDs_StoreAPKWithoutLibR15 is test_a_store_apk_without_libr15_is_named.
func TestCheckPluginBuildIDs_StoreAPKWithoutLibR15(t *testing.T) {
	t.Parallel()
	f := newBuildIDFixture(t)
	apk := zipOf(t, filepath.Join(f.dir, "store.apk"), entry{"lib/arm64-v8a/libother.so", []byte("x")})
	repackfixture.BuildAArch64(t, filepath.Join(f.dir, "libp.so"), nevrNote(t, gameID), "")
	got, err := checkPluginBuildIDs(apk, filepath.Join(f.dir, "libp.so"))
	if err != nil || len(got) != 1 || got[0] != "the store APK has no lib/arm64-v8a/libr15.so" {
		t.Fatalf("got %q, %v", got, err)
	}
}

// mustHex decodes lower-case hex, failing the test on bad input.
func mustHex(t *testing.T, s string) []byte {
	t.Helper()
	out, err := hex.DecodeString(s)
	if err != nil {
		t.Fatalf("not hex: %q: %v", s, err)
	}
	return out
}

// note is one ELF note: its owner, its type and its description, each padded to 4 bytes.
func note(owner []byte, nType uint32, desc []byte) []byte {
	out := make([]byte, 12)
	binary.LittleEndian.PutUint32(out[0:], uint32(len(owner)))
	binary.LittleEndian.PutUint32(out[4:], uint32(len(desc)))
	binary.LittleEndian.PutUint32(out[8:], nType)
	out = append(out, pad4(owner)...)
	return append(out, pad4(desc)...)
}

func pad4(b []byte) []byte {
	out := append([]byte(nil), b...)
	for len(out)%4 != 0 {
		out = append(out, 0)
	}
	return out
}

// segment is one program header and its bytes.
type segment struct {
	pType uint32
	body  []byte
}

// elfImage is a 64-bit little-endian ELF: its header, its program headers, then each segment's bytes in order. phentsize
// and phnum (when non-zero) override the header's, as the Python test's elf() does.
func elfImage(segments []segment, phentsize, phnum int) []byte {
	const (
		headerLen = 64
		phdrLen   = 56
	)
	count := len(segments)
	if phnum != 0 {
		count = phnum
	}
	if phentsize == 0 {
		phentsize = phdrLen
	}
	header := make([]byte, headerLen)
	copy(header, []byte("\x7fELF\x02\x01"))
	binary.LittleEndian.PutUint64(header[0x20:], headerLen)
	binary.LittleEndian.PutUint16(header[0x36:], uint16(phentsize))
	binary.LittleEndian.PutUint16(header[0x38:], uint16(count))
	offset := uint64(headerLen + phdrLen*len(segments))
	var phdrs, data []byte
	for _, s := range segments {
		ph := make([]byte, phdrLen)
		binary.LittleEndian.PutUint32(ph[0:], s.pType)
		binary.LittleEndian.PutUint32(ph[4:], 4)
		binary.LittleEndian.PutUint64(ph[8:], offset+uint64(len(data)))
		binary.LittleEndian.PutUint64(ph[32:], uint64(len(s.body)))
		binary.LittleEndian.PutUint64(ph[40:], uint64(len(s.body)))
		binary.LittleEndian.PutUint64(ph[48:], 4)
		phdrs = append(phdrs, ph...)
		data = append(data, s.body...)
	}
	return append(append(header, phdrs...), data...)
}

var buildIDOwner = []byte("GNU\x00")

// readBuildID is note_desc(image, GNU, build ID) as the NoteWalkTest reads it.
func readBuildID(image []byte) ([]byte, bool) {
	return noteDesc(image, buildIDOwner, ntGNUBuildID)
}

// TestNoteWalk_BuildIDAfterOtherNotes is test_the_build_id_after_other_notes_is_found.
func TestNoteWalk_BuildIDAfterOtherNotes(t *testing.T) {
	t.Parallel()
	build := mustHex(t, gameID)
	notes := concat(note(buildIDOwner, 1, make([]byte, 16)), note([]byte("NEVR\x00"), 1, []byte("x")), note(buildIDOwner, 3, build))
	if got, ok := readBuildID(elfImage([]segment{{ptNote, notes}}, 0, 0)); !ok || !bytes.Equal(got, build) {
		t.Fatalf("got %x, %v", got, ok)
	}
}

// TestNoteWalk_NoteSegmentAfterAnother is test_a_note_segment_after_another_segment_is_read.
func TestNoteWalk_NoteSegmentAfterAnother(t *testing.T) {
	t.Parallel()
	build := mustHex(t, gameID)
	image := elfImage([]segment{{1, make([]byte, 32)}, {ptNote, note(buildIDOwner, 3, build)}}, 0, 0)
	if got, ok := readBuildID(image); !ok || !bytes.Equal(got, build) {
		t.Fatalf("got %x, %v", got, ok)
	}
}

// TestNoteWalk_NoteEndingTheFile is test_a_note_that_ends_the_file_is_read.
func TestNoteWalk_NoteEndingTheFile(t *testing.T) {
	t.Parallel()
	build := mustHex(t, gameID)
	image := elfImage([]segment{{ptNote, note(buildIDOwner, 3, build)}}, 0, 0)
	if !bytes.HasSuffix(image, build) {
		t.Fatal("the image does not end with the build ID")
	}
	if got, ok := readBuildID(image); !ok || !bytes.Equal(got, build) {
		t.Fatalf("got %x, %v", got, ok)
	}
}

// TestNoteWalk_NoteBytesOutsideANoteSegment is test_note_bytes_outside_a_note_segment_are_not_a_note.
func TestNoteWalk_NoteBytesOutsideANoteSegment(t *testing.T) {
	t.Parallel()
	build := mustHex(t, gameID)
	if _, ok := readBuildID(elfImage([]segment{{1, note(buildIDOwner, 3, build)}}, 0, 0)); ok {
		t.Fatal("a load segment's bytes are a note")
	}
}

// TestNoteWalk_SegmentPastTheFileIsSkipped is test_a_segment_that_runs_past_the_file_is_skipped.
func TestNoteWalk_SegmentPastTheFileIsSkipped(t *testing.T) {
	t.Parallel()
	build := mustHex(t, gameID)
	image := elfImage([]segment{{ptNote, note(buildIDOwner, 3, build)}}, 0, 0)
	if _, ok := readBuildID(image[:len(image)-1]); ok {
		t.Fatal("a segment past the end of the file was read")
	}
}

// TestNoteWalk_HeadersPastTheFileOrTooSmallAreRefused is test_program_headers_past_the_file_or_too_small_are_refused.
func TestNoteWalk_HeadersPastTheFileOrTooSmallAreRefused(t *testing.T) {
	t.Parallel()
	body := note(buildIDOwner, 3, mustHex(t, gameID))
	if _, ok := readBuildID(elfImage([]segment{{ptNote, body}}, 0, 200)); ok {
		t.Fatal("200 program headers past the end were read")
	}
	if _, ok := readBuildID(elfImage([]segment{{ptNote, body}}, 55, 0)); ok {
		t.Fatal("a 55-byte program header was read")
	}
}

// TestNoteWalk_DescriptionPastItsSegmentIsNotReturned is test_a_description_that_runs_past_its_segment_is_not_returned.
func TestNoteWalk_DescriptionPastItsSegmentIsNotReturned(t *testing.T) {
	t.Parallel()
	whole := note(buildIDOwner, 3, mustHex(t, gameID))
	cut := concat(headerOf(4, 64, 3), whole[12:])
	if _, ok := readBuildID(elfImage([]segment{{ptNote, cut}}, 0, 0)); ok {
		t.Fatal("a description past its segment was returned")
	}
}

// headerOf is a note header: namesz, descsz, type.
func headerOf(namesz, descsz, nType uint32) []byte {
	out := make([]byte, 12)
	binary.LittleEndian.PutUint32(out[0:], namesz)
	binary.LittleEndian.PutUint32(out[4:], descsz)
	binary.LittleEndian.PutUint32(out[8:], nType)
	return out
}

// TestNoteWalk_NameRunningPastItStopsTheWalk is test_a_name_that_runs_past_its_segment_stops_the_walk.
func TestNoteWalk_NameRunningPastItStopsTheWalk(t *testing.T) {
	t.Parallel()
	body := concat(headerOf(64, 0, 3), []byte("GNU\x00"))
	if _, ok := readBuildID(elfImage([]segment{{ptNote, body}}, 0, 0)); ok {
		t.Fatal("a name past its segment was read")
	}
}

// TestNoteWalk_OtherClassOrByteOrderHasNoNotes is test_another_class_or_byte_order_has_no_notes.
func TestNoteWalk_OtherClassOrByteOrderHasNoNotes(t *testing.T) {
	t.Parallel()
	image := elfImage([]segment{{ptNote, note(buildIDOwner, 3, mustHex(t, gameID))}}, 0, 0)
	for _, change := range []struct {
		at    int
		value byte
	}{{4, 1}, {5, 2}, {0, 0}} {
		bad := append([]byte(nil), image...)
		bad[change.at] = change.value
		if _, ok := readBuildID(bad); ok {
			t.Fatalf("byte %d = %d still reads a note", change.at, change.value)
		}
	}
}

// concat joins byte slices.
func concat(parts ...[]byte) []byte {
	var out []byte
	for _, p := range parts {
		out = append(out, p...)
	}
	return out
}
