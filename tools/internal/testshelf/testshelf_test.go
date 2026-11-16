package testshelf

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

const (
	repoRoot       = "../../.."
	ownerCeilingS  = 30
	validEntryJSON = `{"id":"pytest:test_x","reason":"median 40 s over 5 runs","measured":"junit","date":"2026-11-16"}`
)

func shelfJSON(ceiling string, entries ...string) []byte {
	return []byte(`{"schema":1,"about":[],"ceiling_s":` + ceiling + `,"entries":[` + strings.Join(entries, ",") + `]}`)
}

func TestTheRepoShelfIsTheOwnersCeilingAndEveryEntryExists(t *testing.T) {
	s, err := Load(filepath.Join(repoRoot, DefaultPath))
	if err != nil {
		t.Fatal(err)
	}
	if s.CeilingS != ownerCeilingS || s.Ceiling() != ownerCeilingS*time.Second || s.CeilingArg() != "30s" {
		t.Errorf("ceiling = %d s (%v, %q), want the owner's %d s", s.CeilingS, s.Ceiling(), s.CeilingArg(), ownerCeilingS)
	}
	if len(s.Entries) == 0 {
		t.Fatal("the shelf lists nothing: the tests over the ceiling are not shelved")
	}
	if miss := s.Missing(repoRoot); len(miss) != 0 {
		t.Errorf("shelf entries whose test does not exist:\n%s", strings.Join(miss, "\n"))
	}
}

func TestAnEntryForATestThatIsNotThereIsCaught(t *testing.T) {
	ids := []string{
		"pytest:test_no_such_file", "pytest:test_run_tool_tests#NoSuchClass", "go:internal/nosuchpkg",
		"go:internal/testshelf#TestNoSuchTest", "gtest:test_no_such_binary", "ctest:no_such_ctest",
		"just:no-such-recipe", "target:no-such-target",
		"gtest:test_env_windows#EnvWindowsHttp.NoSuchTest",
	}
	var entries []string
	for _, id := range ids {
		entries = append(entries, `{"id":"`+id+`","reason":"r","measured":"m","date":"2026-11-16"}`)
	}
	s, err := Parse(shelfJSON("30", entries...))
	if err != nil {
		t.Fatal(err)
	}
	miss := s.Missing(repoRoot)
	if len(miss) != len(ids) {
		t.Errorf("Missing found %d of the %d absent tests:\n%s", len(miss), len(ids), strings.Join(miss, "\n"))
	}
}

func TestAPresentTestIsNotReportedMissing(t *testing.T) {
	ids := []string{
		"pytest:test_run_tool_tests#RunToolTestsRecord", "go:internal/testshelf#TestSplit", "target:host-seams",
		"just:test-android", "ctest:resource_request_hook_test",
		"gtest:test_env_windows#EnvWindowsHttp.ATransferUnderTheLowSpeedFloorTimesOut",
	}
	var entries []string
	for _, id := range ids {
		entries = append(entries, `{"id":"`+id+`","reason":"r","measured":"m","date":"2026-11-16"}`)
	}
	s, err := Parse(shelfJSON("30", entries...))
	if err != nil {
		t.Fatal(err)
	}
	if miss := s.Missing(repoRoot); len(miss) != 0 {
		t.Errorf("present tests reported missing:\n%s", strings.Join(miss, "\n"))
	}
}

func TestParseRefusesWhatAShelfMustNotHold(t *testing.T) {
	cases := map[string][]byte{
		"a ceiling of 0":          shelfJSON("0"),
		"a ceiling past a minute": shelfJSON("600"),
		"an unknown kind":         shelfJSON("30", `{"id":"perl:t","reason":"r","measured":"m","date":"2026-11-16"}`),
		"no reason":               shelfJSON("30", `{"id":"pytest:t","reason":" ","measured":"m","date":"2026-11-16"}`),
		"no measured-where":       shelfJSON("30", `{"id":"pytest:t","reason":"r","measured":"","date":"2026-11-16"}`),
		"a date not a day":        shelfJSON("30", `{"id":"pytest:t","reason":"r","measured":"m","date":"2026-13-45"}`),
		"a date in words":         shelfJSON("30", `{"id":"pytest:t","reason":"r","measured":"m","date":"today"}`),
		"a case on a recipe":      shelfJSON("30", `{"id":"just:t#c","reason":"r","measured":"m","date":"2026-11-16"}`),
		"an empty case":           shelfJSON("30", `{"id":"go:t#","reason":"r","measured":"m","date":"2026-11-16"}`),
		"the same id twice":       shelfJSON("30", validEntryJSON, validEntryJSON),
		"an unknown field":        []byte(`{"schema":1,"ceiling_s":30,"entries":[],"extra":1}`),
		"another schema":          []byte(`{"schema":2,"ceiling_s":30,"entries":[]}`),
	}
	for name, data := range cases {
		if _, err := Parse(data); err == nil {
			t.Errorf("%s: Parse accepted it", name)
		}
	}
	if _, err := Parse(shelfJSON("30", validEntryJSON)); err != nil {
		t.Errorf("a valid shelf: %v", err)
	}
}

func TestSplit(t *testing.T) {
	kind, unit, c, err := Split("gtest:test_a#Suite.Name")
	if err != nil || kind != KindGTest || unit != "test_a" || c != "Suite.Name" {
		t.Errorf("Split = %q %q %q %v", kind, unit, c, err)
	}
	if _, _, _, err := Split("nokind"); err == nil {
		t.Error("an id with no kind was accepted")
	}
}

func TestShelvedReadsAUnitLevelIdAndABareTarget(t *testing.T) {
	s, err := Parse(shelfJSON("30",
		`{"id":"target:host-seams","reason":"r","measured":"m","date":"2026-11-16"}`,
		`{"id":"just:test-android","reason":"r","measured":"m","date":"2026-11-16"}`))
	if err != nil {
		t.Fatal(err)
	}
	for _, id := range []string{"host-seams", "target:host-seams", "just:test-android"} {
		if _, ok := s.Shelved(id); !ok {
			t.Errorf("%s is not shelved", id)
		}
	}
	for _, id := range []string{"build", "just:test-quest-shared", "pytest:host-seams"} {
		if _, ok := s.Shelved(id); ok {
			t.Errorf("%s is shelved", id)
		}
	}
}

func TestGoSkipAndGTestExclusionNameOnlyTheShelvedCasesOfTheUnit(t *testing.T) {
	s, err := Parse(shelfJSON("30",
		`{"id":"go:internal/a#TestOne","reason":"r","measured":"m","date":"2026-11-16"}`,
		`{"id":"go:internal/a#TestTwo","reason":"r","measured":"m","date":"2026-11-16"}`,
		`{"id":"go:internal/b","reason":"r","measured":"m","date":"2026-11-16"}`,
		`{"id":"gtest:test_x#S.A","reason":"r","measured":"m","date":"2026-11-16"}`,
		`{"id":"gtest:test_x#S.B","reason":"r","measured":"m","date":"2026-11-16"}`))
	if err != nil {
		t.Fatal(err)
	}
	if got := s.GoSkip("internal/a"); got != "^(TestOne|TestTwo)$" {
		t.Errorf("GoSkip(a) = %q", got)
	}
	if got := s.GoSkip("internal/b"); got != "" {
		t.Errorf("GoSkip(b) = %q: a whole-package entry has no cases", got)
	}
	if got := s.GTestExclusion("test_x"); got != "S.A:S.B" {
		t.Errorf("GTestExclusion = %q", got)
	}
	if got := s.GTestExclusion("test_y"); got != "" {
		t.Errorf("GTestExclusion(other) = %q", got)
	}
}

func TestLoadNamesAMissingFile(t *testing.T) {
	if _, err := Load(filepath.Join(t.TempDir(), "none.json")); err == nil || !os.IsNotExist(unwrap(err)) {
		t.Errorf("Load of a missing file: %v", err)
	}
}

func unwrap(err error) error {
	for {
		u, ok := err.(interface{ Unwrap() error })
		if !ok {
			return err
		}
		err = u.Unwrap()
	}
}
