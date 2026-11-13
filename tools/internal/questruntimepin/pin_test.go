package questruntimepin

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"os"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/questruntimepin/fixture"
)

// The fixture is a synthetic AArch64 ELF built by the fixture package: an executable segment with twelve sites per entry
// and a data segment with twelve lists of three entries. A pin of the fixture must equal the fixture built for the new
// text. One real-file case runs only when NEVR_REFERENCE_LIBVRAPI names the reference APK's libvrapi.so (sha-256
// below), which the lane's expected output (md5 0748d0e3...) is compared with.

const (
	oldMD5       = fixture.OLD
	secondMD5    = fixture.SECOND
	thirdMD5     = fixture.THIRD
	notAnELFSize = 12 // the copies the not-an-ELF case writes: enough for a table
	// The reference APK's libvrapi.so and the lane's expected output.
	referenceSHA256 = "59a3cd2090b29f72cc14b853f1550073c9dd066c87c9cd120eed6d93f84c3839"
	laneMD5         = "0748d0e375e3a6d1956ca057a009bf48"
	lanePinMD5      = "3e89307b9eddca58aa4573953a4477b9"
	laneChangedByte = 828
	// The counts of one entry: twelve copies, twelve sites of four runs, 192 words.
	wantRuns  = 48
	wantWords = 192
	// The runs a missing or extra site leaves.
	runsWithEleven   = 44
	runsWithThirteen = 52
	// The runs of one spliced-in entry.
	splicedRuns = 4
	// The index and entries of the other pins.
	secondIndex = 1
	outsideIdx  = 3
	// shortMD5 is a text that is not 32 characters.
	shortMD5 = "4911ef7f"
	// The width of a text.
	textWidth   = 32
	imm16Offset = 5
	imm16Width  = 0xFFFF
	wordMask    = 3
	stubCodeLen = 8 // the code of a library that is no runtime: two words
	outsideNeg  = -1
)

var (
	newMD5   = fixture.MD5Hex([]byte("a patched libr15"))
	otherMD5 = fixture.MD5Hex([]byte("another libr15"))
)

func runtime(old string, options ...fixture.Option) []byte {
	return fixture.MakeRuntime(old, options...)
}

func mustPin(t *testing.T, data []byte, md5 string, index int, expect string) ([]byte, Report) {
	t.Helper()
	result, report, err := PinData(data, md5, index, expect)
	if err != nil {
		t.Fatalf("PinData(%s): %v", md5, err)
	}
	return result, report
}

// refused checks that err is a PinError whose text holds want, and not a WrongEntryError.
func refused(t *testing.T, err error, want string) {
	t.Helper()
	if err == nil {
		t.Fatalf("no error, want a PinError holding %q", want)
	}
	var pe PinError
	if !errors.As(err, &pe) {
		t.Fatalf("error %T %q is not a PinError", err, err)
	}
	var we WrongEntryError
	if errors.As(err, &we) {
		t.Fatalf("error %q is a WrongEntryError, want a PinError", err)
	}
	if !strings.Contains(err.Error(), want) {
		t.Fatalf("error %q does not hold %q", err, want)
	}
}

func textsOf(t *testing.T, list [][]byte) []string {
	t.Helper()
	out := make([]string, len(list))
	for i, b := range list {
		out[i] = string(b)
	}
	return out
}

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

func runsOf(t *testing.T, data []byte, text string) int {
	t.Helper()
	scan, err := NewScan(data)
	if err != nil {
		t.Fatal(err)
	}
	n := 0
	for _, c := range ChunksOf([]byte(text)) {
		n += len(scan.Runs(c))
	}
	return n
}

// PinDataTest.test_the_pinned_runtime_is_the_runtime_built_for_the_new_md5
func TestPinDataThePinnedRuntimeIsTheRuntimeBuiltForTheNewMD5(t *testing.T) {
	t.Parallel()
	result, report := mustPin(t, runtime(oldMD5), newMD5, 0, StockEntryMD5)
	if !bytes.Equal(result, runtime(newMD5)) {
		t.Error("the pinned runtime is not the runtime built for the new md5")
	}
	if report.ASCIICopies != Sites || report.Sites != Sites || report.ImmediateRuns != wantRuns || report.ImmediateWords != wantWords {
		t.Errorf("report counts = %+v, want %d, %d, %d, %d", report, Sites, Sites, wantRuns, wantWords)
	}
	if report.OldMD5 != oldMD5 {
		t.Errorf("old md5 = %s, want %s", report.OldMD5, oldMD5)
	}
	if !equalStrings(report.Table, []string{oldMD5, secondMD5, thirdMD5}) {
		t.Errorf("table = %v", report.Table)
	}
}

// PinDataTest.test_only_the_copies_and_the_immediate_fields_change
func TestPinDataOnlyTheCopiesAndTheImmediateFieldsChange(t *testing.T) {
	t.Parallel()
	before := runtime(oldMD5)
	result, report := mustPin(t, before, newMD5, 0, StockEntryMD5)
	if len(result) != len(before) {
		t.Fatalf("size %d, want %d", len(result), len(before))
	}
	var differing []int
	for i := range before {
		if before[i] != result[i] {
			differing = append(differing, i)
		}
	}
	if len(differing) != report.ChangedBytes {
		t.Errorf("%d bytes differ, the report says %d", len(differing), report.ChangedBytes)
	}
	text := ASCIICopies(before, []byte(oldMD5))
	if len(text) != Sites {
		t.Fatalf("%d copies, want %d", len(text), Sites)
	}
	inside := map[int]bool{}
	for _, o := range text {
		for i := o; i < o+textWidth; i++ {
			inside[i] = true
		}
	}
	for _, i := range differing {
		if inside[i] {
			continue
		}
		word := i &^ wordMask
		x := binary.LittleEndian.Uint32(before[word:]) ^ binary.LittleEndian.Uint32(result[word:])
		if x&^(imm16Width<<imm16Offset) != 0 {
			t.Errorf("byte %#x changed outside the imm16 field of its word", i)
		}
	}
}

// PinDataTest.test_the_new_md5_is_found_twelve_times_in_each_form_and_the_old_one_never
func TestPinDataTheNewMD5IsFoundTwelveTimesInEachFormAndTheOldOneNever(t *testing.T) {
	t.Parallel()
	result, _ := mustPin(t, runtime(oldMD5), newMD5, 0, StockEntryMD5)
	if n := len(ASCIICopies(result, []byte(newMD5))); n != Sites {
		t.Errorf("%d ASCII copies of the new md5, want %d", n, Sites)
	}
	if n := runsOf(t, result, newMD5); n != wantRuns {
		t.Errorf("%d runs of the new md5, want %d", n, wantRuns)
	}
	if n := len(ASCIICopies(result, []byte(oldMD5))); n != 0 {
		t.Errorf("%d ASCII copies of the old md5 remain", n)
	}
	if n := runsOf(t, result, oldMD5); n != 0 {
		t.Errorf("%d runs of the old md5 remain", n)
	}
	if got := textsOf(t, Table(result)); !equalStrings(got, []string{newMD5, secondMD5, thirdMD5}) {
		t.Errorf("table = %v", got)
	}
}

// PinDataTest.test_the_other_entries_keep_their_copies_and_sites
func TestPinDataTheOtherEntriesKeepTheirCopiesAndSites(t *testing.T) {
	t.Parallel()
	result, _ := mustPin(t, runtime(oldMD5), newMD5, 0, StockEntryMD5)
	for _, text := range []string{secondMD5, thirdMD5} {
		t.Run(text, func(t *testing.T) {
			t.Parallel()
			if n := len(ASCIICopies(result, []byte(text))); n != Sites {
				t.Errorf("%d copies, want %d", n, Sites)
			}
			if n := runsOf(t, result, text); n != wantRuns {
				t.Errorf("%d runs, want %d", n, wantRuns)
			}
		})
	}
}

// PinDataTest.test_a_second_pin_with_the_same_md5_changes_nothing
func TestPinDataASecondPinWithTheSameMD5ChangesNothing(t *testing.T) {
	t.Parallel()
	once, _ := mustPin(t, runtime(oldMD5), newMD5, 0, StockEntryMD5)
	twice, report := mustPin(t, once, newMD5, 0, StockEntryMD5)
	if !bytes.Equal(twice, once) {
		t.Error("a second pin changed the file")
	}
	if report.ChangedBytes != 0 {
		t.Errorf("changed bytes = %d, want 0", report.ChangedBytes)
	}
}

// PinDataTest.test_a_file_pinned_to_another_libr15_is_refused_naming_both_entries
func TestPinDataAFilePinnedToAnotherLibr15IsRefusedNamingBothEntries(t *testing.T) {
	t.Parallel()
	once, _ := mustPin(t, runtime(oldMD5), newMD5, 0, StockEntryMD5)
	_, _, err := PinData(once, otherMD5, 0, StockEntryMD5)
	var we WrongEntryError
	if !errors.As(err, &we) {
		t.Fatalf("error %v is not a WrongEntryError", err)
	}
	if we.Expected != oldMD5 || we.Found != newMD5 {
		t.Errorf("expected/found = %s/%s, want %s/%s", we.Expected, we.Found, oldMD5, newMD5)
	}
	var pe PinError
	if errors.As(err, &pe) {
		t.Error("a WrongEntryError is also a PinError; a caller could not tell them apart")
	}
}

// PinDataTest.test_a_decoy_run_in_a_data_segment_is_not_a_site
func TestPinDataADecoyRunInADataSegmentIsNotASite(t *testing.T) {
	t.Parallel()
	before := runtime(oldMD5, fixture.WithDecoy())
	result, report := mustPin(t, before, newMD5, 0, StockEntryMD5)
	if report.Sites != Sites {
		t.Errorf("sites = %d, want %d", report.Sites, Sites)
	}
	decoy := fixture.WordBytes(fixture.SiteWords(oldMD5))
	if !bytes.HasSuffix(before, decoy) {
		t.Fatal("the fixture does not end with the decoy")
	}
	scan, err := NewScan(before)
	if err != nil {
		t.Fatal(err)
	}
	if n := len(scan.Runs(ChunksOf([]byte(oldMD5))[0])); n != Sites {
		t.Errorf("%d runs of the first chunk, want %d", n, Sites)
	}
	if !bytes.HasSuffix(result, decoy) { // not an executable segment: left as it was
		t.Error("the decoy in the data segment changed")
	}
}

// PinDataTest.test_eleven_sites_are_refused
func TestPinDataElevenSitesAreRefused(t *testing.T) {
	t.Parallel()
	_, _, err := PinData(runtime(oldMD5, fixture.WithSites(Sites-1)), newMD5, 0, StockEntryMD5)
	refused(t, err, fmt.Sprintf("has %d immediate runs", runsWithEleven))
}

// PinDataTest.test_a_duplicated_site_is_refused
func TestPinDataADuplicatedSiteIsRefused(t *testing.T) {
	t.Parallel()
	_, _, err := PinData(runtime(oldMD5, fixture.WithSites(Sites+1)), newMD5, 0, StockEntryMD5)
	refused(t, err, fmt.Sprintf("has %d immediate runs", runsWithThirteen))
}

// PinDataTest.test_eleven_ascii_copies_are_refused
func TestPinDataElevenASCIICopiesAreRefused(t *testing.T) {
	t.Parallel()
	_, _, err := PinData(runtime(oldMD5, fixture.WithCopies(Sites-1)), newMD5, 0, StockEntryMD5)
	refused(t, err, "has 11 ASCII copies, not 12")
}

// PinDataTest.test_a_text_inside_a_longer_hex_string_is_not_a_copy_and_is_left_alone
func TestPinDataATextInsideALongerHexStringIsNotACopyAndIsLeftAlone(t *testing.T) {
	t.Parallel()
	for name, extra := range map[string][]byte{
		"digit before": []byte("0" + oldMD5 + "\x00"),
		"digit after":  []byte(oldMD5 + "0\x00"),
	} {
		t.Run(name, func(t *testing.T) {
			t.Parallel()
			result, report := mustPin(t, runtime(oldMD5, fixture.WithExtra(extra)), newMD5, 0, StockEntryMD5)
			if report.ASCIICopies != Sites {
				t.Errorf("copies = %d, want %d", report.ASCIICopies, Sites)
			}
			if !bytes.Equal(result, runtime(newMD5, fixture.WithExtra(extra))) {
				t.Error("the result is not the fixture built for the new md5 with the same extra bytes")
			}
		})
	}
}

// PinDataTest.test_a_new_md5_that_is_another_entry_of_the_list_is_refused
func TestPinDataANewMD5ThatIsAnotherEntryOfTheListIsRefused(t *testing.T) {
	t.Parallel()
	_, _, err := PinData(runtime(oldMD5), thirdMD5, 0, StockEntryMD5)
	refused(t, err, "already in the file")
}

// PinDataTest.test_a_new_md5_present_once_in_ascii_is_refused
func TestPinDataANewMD5PresentOnceInASCIIIsRefused(t *testing.T) {
	t.Parallel()
	_, _, err := PinData(runtime(oldMD5, fixture.WithExtra([]byte(newMD5+"\x00"))), newMD5, 0, StockEntryMD5)
	refused(t, err, "already in the file (1 ASCII copies, 0 immediate runs")
}

// PinDataTest.test_a_new_md5_present_as_immediate_runs_is_refused
func TestPinDataANewMD5PresentAsImmediateRunsIsRefused(t *testing.T) {
	t.Parallel()
	rt := runtime(oldMD5)
	spliced := bytes.Replace(rt, fixture.WordBytes(fixture.SiteWords(thirdMD5)), fixture.WordBytes(fixture.SiteWords(newMD5)), 1)
	if bytes.Equal(spliced, rt) {
		t.Fatal("the splice changed nothing")
	}
	_, _, err := PinData(spliced, newMD5, 0, StockEntryMD5)
	refused(t, err, fmt.Sprintf("already in the file (0 ASCII copies, %d immediate runs", splicedRuns))
}

// PinDataTest.test_a_file_with_no_list_is_not_the_runtime
func TestPinDataAFileWithNoListIsNotTheRuntime(t *testing.T) {
	t.Parallel()
	plain := fixture.MakeElf(fixture.WordBytes([]uint32{fixture.NOP, fixture.NOP}), []byte("nothing here\x00"))
	if CarriesTable(plain) {
		t.Error("a file with no list carries a table")
	}
	_, _, err := PinData(plain, newMD5, 0, StockEntryMD5)
	refused(t, err, "not the reference runtime")
}

// PinDataTest.test_a_file_that_is_not_an_aarch64_elf_is_refused
func TestPinDataAFileThatIsNotAnAArch64ELFIsRefused(t *testing.T) {
	t.Parallel()
	t.Run("not an ELF", func(t *testing.T) {
		t.Parallel()
		data := append([]byte("not an elf "), bytes.Repeat([]byte(oldMD5+"\x00"), notAnELFSize)...)
		_, _, err := PinData(data, newMD5, 0, StockEntryMD5)
		refused(t, err, "not a 64-bit little-endian ELF")
	})
	t.Run("x86-64", func(t *testing.T) {
		t.Parallel()
		data := fixture.MakeElf(make([]byte, stubCodeLen), bytes.Repeat([]byte(oldMD5+"\x00"), notAnELFSize), fixture.EMX8664)
		_, _, err := PinData(data, newMD5, 0, StockEntryMD5)
		refused(t, err, "is not AArch64")
	})
}

// PinDataTest.test_a_replace_index_outside_the_list_is_refused
func TestPinDataAReplaceIndexOutsideTheListIsRefused(t *testing.T) {
	t.Parallel()
	_, _, err := PinData(runtime(oldMD5), newMD5, outsideIdx, StockEntryMD5)
	refused(t, err, "the table has 3 entries")
	_, _, err = PinData(runtime(oldMD5), newMD5, outsideNeg, StockEntryMD5)
	refused(t, err, "the table has 3 entries")
}

// PinDataTest.test_the_replace_index_and_the_expected_md5_pick_the_entry
func TestPinDataTheReplaceIndexAndTheExpectedMD5PickTheEntry(t *testing.T) {
	t.Parallel()
	result, report := mustPin(t, runtime(oldMD5), newMD5, secondIndex, secondMD5)
	if report.OldMD5 != secondMD5 {
		t.Errorf("old md5 = %s, want %s", report.OldMD5, secondMD5)
	}
	if !bytes.Equal(result, runtime(oldMD5, fixture.WithOthers(newMD5, thirdMD5))) {
		t.Error("the result is not the runtime with the second entry replaced")
	}
}

// PinDataTest.test_a_damaged_first_entry_is_refused_and_does_not_retarget_the_index
func TestPinDataADamagedFirstEntryIsRefusedAndDoesNotRetargetTheIndex(t *testing.T) {
	t.Parallel()
	// the first ASCII copy: the list now opens with SECOND
	damaged := bytes.Replace(runtime(oldMD5), []byte(oldMD5), bytes.Repeat([]byte("x"), textWidth), 1)
	if got := string(Table(damaged)[0]); got != secondMD5 {
		t.Fatalf("table[0] = %s, want %s", got, secondMD5)
	}
	if !CarriesTable(damaged) {
		t.Error("the damaged file no longer carries a table")
	}
	_, _, err := PinData(damaged, newMD5, 0, StockEntryMD5)
	var we WrongEntryError
	if !errors.As(err, &we) {
		t.Fatalf("error %v is not a WrongEntryError", err)
	}
	if we.Index != 0 || we.Expected != oldMD5 || we.Found != secondMD5 {
		t.Errorf("index/expected/found = %d/%s/%s", we.Index, we.Expected, we.Found)
	}
	if !strings.Contains(err.Error(), oldMD5) || !strings.Contains(err.Error(), secondMD5) {
		t.Errorf("error %q does not name both entries", err)
	}
}

// PinDataTest.test_a_wrong_expected_md5_is_refused
func TestPinDataAWrongExpectedMD5IsRefused(t *testing.T) {
	t.Parallel()
	_, _, err := PinData(runtime(oldMD5), newMD5, 0, secondMD5)
	var we WrongEntryError
	if !errors.As(err, &we) {
		t.Fatalf("error %v is not a WrongEntryError", err)
	}
	if we.Expected != secondMD5 || we.Found != oldMD5 {
		t.Errorf("expected/found = %s/%s, want %s/%s", we.Expected, we.Found, secondMD5, oldMD5)
	}
}

// PinDataTest.test_an_expected_md5_that_is_not_32_lowercase_hex_is_refused
func TestPinDataAnExpectedMD5ThatIsNot32LowercaseHexIsRefused(t *testing.T) {
	t.Parallel()
	_, _, err := PinData(runtime(oldMD5), newMD5, 0, shortMD5)
	refused(t, err, "is not 32 lowercase hex characters")
}

// PinDataTest.test_the_expected_entry_must_have_its_immediate_sites_too
func TestPinDataTheExpectedEntryMustHaveItsImmediateSitesToo(t *testing.T) {
	t.Parallel()
	// the ASCII table says OLD is at index 0 and it has 12 copies; one of its sites is gone, so the pin must not go on
	rt := runtime(oldMD5)
	words := fixture.SiteWords(oldMD5)
	nops := make([]uint32, len(words))
	for i := range nops {
		nops[i] = fixture.NOP
	}
	damaged := bytes.Replace(rt, fixture.WordBytes(words), fixture.WordBytes(nops), 1)
	if bytes.Equal(damaged, rt) {
		t.Fatal("the damage changed nothing")
	}
	_, _, err := PinData(damaged, newMD5, 0, StockEntryMD5)
	refused(t, err, fmt.Sprintf("has %d immediate runs", runsWithEleven))
}

// PinDataTest.test_an_md5_that_is_not_32_lowercase_hex_is_refused
func TestPinDataAnMD5ThatIsNot32LowercaseHexIsRefused(t *testing.T) {
	t.Parallel()
	for name, bad := range map[string]string{
		"empty":     "",
		"uppercase": strings.ToUpper(lanePinMD5),
		"short":     newMD5[:len(newMD5)-1],
		"long":      newMD5 + "0",
		"not hex":   strings.Repeat("g", textWidth),
	} {
		t.Run(name, func(t *testing.T) {
			t.Parallel()
			_, _, err := PinData(runtime(oldMD5), bad, 0, StockEntryMD5)
			refused(t, err, "is not 32 lowercase hex characters")
		})
	}
}

// PinDataTest.test_the_store_style_library_carries_no_list
func TestPinDataTheStoreStyleLibraryCarriesNoList(t *testing.T) {
	t.Parallel()
	data := append([]byte("vrapi\x00"), bytes.Repeat([]byte(oldMD5+"\x00"), 3)...)
	if CarriesTable(fixture.MakeElf(make([]byte, stubCodeLen), data)) {
		t.Error("the store-style library carries a list")
	}
}

// RealReferenceRuntimeTest.test_the_pin_reproduces_the_lane_file_byte_for_byte
func TestRealReferenceRuntime(t *testing.T) {
	t.Parallel()
	path := os.Getenv("NEVR_REFERENCE_LIBVRAPI") // an input outside the tree, like NEVR_STOCK_DATA in internal/radpack
	if path == "" {
		t.Skip("set NEVR_REFERENCE_LIBVRAPI to the reference APK's lib/arm64-v8a/libvrapi.so")
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if got := Sha256Hex(data); got != referenceSHA256 {
		t.Fatalf("sha-256 = %s, want %s", got, referenceSHA256)
	}
	if got := textsOf(t, Table(data)); !equalStrings(got, []string{oldMD5, secondMD5, thirdMD5}) {
		t.Fatalf("table = %v", got)
	}
	result, report := mustPin(t, data, lanePinMD5, 0, StockEntryMD5)
	if report.ASCIICopies != Sites || report.Sites != Sites || report.ImmediateRuns != wantRuns || report.ImmediateWords != wantWords {
		t.Errorf("report counts = %+v", report)
	}
	if report.ChangedBytes != laneChangedByte {
		t.Errorf("changed bytes = %d, want %d", report.ChangedBytes, laneChangedByte)
	}
	if got := fixture.MD5Hex(result); got != laneMD5 {
		t.Errorf("md5 of the pin = %s, want %s", got, laneMD5)
	}
	again, _ := mustPin(t, result, lanePinMD5, 0, StockEntryMD5)
	if !bytes.Equal(again, result) {
		t.Error("a second pin changed the file")
	}
}
