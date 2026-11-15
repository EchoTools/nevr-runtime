package questapk

import (
	"bytes"
	"encoding/binary"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/axmlfix"
	"github.com/EchoTools/nevr-runtime/tools/internal/testkit"
)

// The Go twin of tools/tests/test_quest_apk_version.py (the numbering rule and the strict manifest reader and writer, #923).
// The manifests are built by axmlfix; the aapt2 check reads the patched file through a reader that is not ours.

const (
	floorMajor        = 6
	floorTag          = "v6.0.0"
	zeroBytes         = 64
	strayOffset       = 20
	trailingChunkKind = 0x0999
	fourZeroBytes     = 4
	chunkSizeWord     = 8
	chunkKindFirst    = 0x0001
	emptyChunkBytes   = 8
	versionNameShim   = "5.6.1-dev.37+3a35e0b"
	shimOtherVersion  = "5.6.1-dev.3+3a35e0b"
	uint32Limit       = 1 << 32
	int32Bound        = 1 << 31
)

// manifestFor is the store-shaped manifest with the code given (the version name stays the store's).
func manifestFor(code int64, utf8 bool) []byte {
	s := axmlfix.Default()
	s.VersionCode = code
	s.UTF8 = utf8
	return s.Build()
}

// TestNumberingTable is the rule's table, worked out by hand (major*10^7 + minor*10^5 + patch*10^3 + slot).
func TestNumberingTable(t *testing.T) {
	t.Parallel()
	table := []struct{ version, code string }{
		{"5.6.0", "50600999"},
		{"5.6.1-dev.0+3a35e0b", "50601000"},
		{"5.6.1-dev.1+3a35e0b", "50601001"},
		{"5.6.1-dev.37+3a35e0b", "50601037"},
		{"5.6.1-dev.998+3a35e0b", "50601998"},
		{"5.6.1", "50601999"},
		{"5.7.0-dev.1+abcdef0", "50700001"},
		{"5.7.0", "50700999"},
		{"6.0.0", "60000999"},
		{"10.20.30", "102030999"},
		{"5.0.0", "50000999"},
		{"5.0.0-dev.0+abcdef0", "50000000"},
		{"5.99.99", "59999999"},
		{"209.0.0", "2090000999"},
		{"209.99.98-dev.998+abcdef0", "2099998998"},
		{"209.99.99-dev.998+abcdef0", "2099999998"},
		{"209.99.99", "2099999999"},
	}
	for _, row := range table {
		t.Run(row.version, func(t *testing.T) {
			t.Parallel()
			got, err := VersionCodeFor(row.version)
			if err != nil {
				t.Fatal(err)
			}
			if got != mustInt(t, row.code) {
				t.Fatalf("VersionCodeFor(%s) = %d, want %s", row.version, got, row.code)
			}
		})
	}
}

// TestEveryCodeBeatsTheInstallersAndFitsInt32 checks every table version against the installer's build and the ceilings.
func TestEveryCodeBeatsTheInstallersAndFitsInt32(t *testing.T) {
	t.Parallel()
	for _, v := range []string{"5.6.0", "5.6.1-dev.0+3a35e0b", "209.99.99", "5.0.0-dev.0+abcdef0", "10.20.30"} {
		code, err := VersionCodeFor(v)
		if err != nil {
			t.Fatal(err)
		}
		if code <= mustInt(t, installerCodeText) {
			t.Errorf("%s: code %d does not beat the installer's %s", v, code, installerCodeText)
		}
		if int64(code) >= int64(mustInt(t, int32LimitText)) || code >= mustInt(t, playCeilingText) {
			t.Errorf("%s: code %d is at or above the int32 or Play ceiling", v, code)
		}
	}
}

// TestTheLargestCodeIs209_99_99 pins the top of the rule and the ceiling constant.
func TestTheLargestCodeIs209_99_99(t *testing.T) {
	t.Parallel()
	top, err := VersionCodeFor("209.99.99")
	if err != nil {
		t.Fatal(err)
	}
	if top != mustInt(t, "2099999999") {
		t.Fatalf("top = %d", top)
	}
	if top >= mustInt(t, int32LimitText)-1 || top >= mustInt(t, playCeilingText) {
		t.Fatalf("top %d is not under int32 or the Play ceiling", top)
	}
	if MaxVersionCode != mustInt(t, playCeilingText) {
		t.Fatalf("MaxVersionCode = %d", MaxVersionCode)
	}
}

// TestSemverPrecedenceIsCodePrecedence: semver item 11 is the order the codes must keep.
func TestSemverPrecedenceIsCodePrecedence(t *testing.T) {
	t.Parallel()
	order := []string{"5.6.0", "5.6.1-dev.1+aaaaaaa", "5.6.1-dev.2+bbbbbbb", "5.6.1-dev.10+ccccccc", "5.6.1-dev.998+ccccccc",
		"5.6.1", "5.7.0-dev.1+ddddddd", "5.7.0", "6.0.0-dev.3+eeeeeee", "6.0.0"}
	prev := -1
	for _, v := range order {
		code, err := VersionCodeFor(v)
		if err != nil {
			t.Fatal(err)
		}
		if code <= prev {
			t.Fatalf("%s = %d is not above the code before it, %d", v, code, prev)
		}
		prev = code
	}
}

// TestSameDistanceDifferentShaGivesTheSameCode: the code carries no commit.
func TestSameDistanceDifferentShaGivesTheSameCode(t *testing.T) {
	t.Parallel()
	a, _ := VersionCodeFor("5.6.1-dev.37+aaaaaaa")
	b, _ := VersionCodeFor("5.6.1-dev.37+bbbbbbb")
	if a != b {
		t.Fatalf("%d != %d", a, b)
	}
}

// TestABaseSuffixStampsTheSameCodeAsTheUnsuffixedVersion: -store and -combat stamp the code of the plain version.
func TestABaseSuffixStampsTheSameCodeAsTheUnsuffixedVersion(t *testing.T) {
	t.Parallel()
	plain, _ := VersionCodeFor("6.16.5-dev.58+aa977701")
	for _, base := range []string{"store", "combat"} {
		t.Run(base, func(t *testing.T) {
			t.Parallel()
			got, err := VersionCodeFor("6.16.5-dev.58-" + base + "+aa977701")
			if err != nil || got != plain {
				t.Fatalf("got %d, %v; want %d", got, err, plain)
			}
		})
	}
}

// TestASuffixOtherThanTheTwoBasesIsRefused.
func TestASuffixOtherThanTheTwoBasesIsRefused(t *testing.T) {
	t.Parallel()
	for _, bad := range []string{"6.16.5-dev.58-foo+aa977701", "6.16.5-dev.58-store-combat+aa977701", "6.16.5-dev.58-+aa977701", "6.16.5-store+aa977701"} {
		t.Run(bad, func(t *testing.T) {
			t.Parallel()
			if _, err := VersionCodeFor(bad); err == nil {
				t.Fatalf("%q was accepted", bad)
			}
		})
	}
}

// TestRefusals: every string the rule does not accept.
func TestRefusals(t *testing.T) {
	t.Parallel()
	for _, bad := range []string{"", "5.6", "5.6.0.1", "v5.6.0", "5.6.0-rc.1", "5.6.0-dev.1", "5.6.0-dev.+abcdef0", "05.6.0",
		"5.6.0-dev.01+abcdef0", "5.6.0-dev.1+XYZ", "-1.0.0", "5.6.0 ", "5.6.1-dev.1+abcdef0\n"} {
		t.Run(bad, func(t *testing.T) {
			t.Parallel()
			if _, err := VersionCodeFor(bad); err == nil {
				t.Fatalf("%q was accepted", bad)
			}
		})
	}
}

// TestEachPartIsRefusedJustAboveItsLimitAndNamed.
func TestEachPartIsRefusedJustAboveItsLimitAndNamed(t *testing.T) {
	t.Parallel()
	cases := []struct{ version, message string }{
		{"210.0.0", "major 210 is above 209"},
		{"5.100.0", "minor 100 is above 99"},
		{"5.6.100", "patch 100 is above 99"},
		{"5.6.1-dev.999+abcdef0", "development distance N 999 is above 998"},
		{"5.6.1-dev.1000+abcdef0", "development distance N 1000 is above 998"},
		{"5.6.1-dev.99999+abcdef0", "development distance N 99999 is above 998"},
		{"999.0.0", "major 999 is above 209"},
	}
	for _, c := range cases {
		t.Run(c.version, func(t *testing.T) {
			t.Parallel()
			_, err := VersionCodeFor(c.version)
			if err == nil || !strings.Contains(err.Error(), c.message) {
				t.Fatalf("VersionCodeFor(%s) error = %v, want %q", c.version, err, c.message)
			}
		})
	}
}

// TestEachPartIsAcceptedAtItsLimit.
func TestEachPartIsAcceptedAtItsLimit(t *testing.T) {
	t.Parallel()
	for _, v := range []string{"209.0.0", "5.99.0", "5.0.99", "5.6.1-dev.998+abcdef0", "5.6.1-dev.0+abcdef0"} {
		if _, err := VersionCodeFor(v); err != nil {
			t.Fatalf("%s: %v", v, err)
		}
	}
}

// TestAReleaseAndTheDevSlotDoNotCollide: dev N tops out at 998 so slot 999 is only ever a release.
func TestAReleaseAndTheDevSlotDoNotCollide(t *testing.T) {
	t.Parallel()
	dev, _ := VersionCodeFor("5.6.1-dev.998+abcdef0")
	rel, _ := VersionCodeFor("5.6.1")
	if dev >= rel {
		t.Fatalf("dev.998 %d is not below the release %d", dev, rel)
	}
}

// TestWarningsStartOnePastTheThresholdAndNameTheFieldAndLimit: dev N > 900, minor > 90, patch > 90.
func TestWarningsStartOnePastTheThresholdAndNameTheFieldAndLimit(t *testing.T) {
	t.Parallel()
	if w := FieldWarnings("5.6.1-dev.900+abcdef0"); len(w) != 0 {
		t.Fatalf("900 warned: %v", w)
	}
	w := FieldWarnings("5.6.1-dev.901+abcdef0")
	if len(w) != 1 {
		t.Fatalf("901: %v", w)
	}
	contains(t, w[0], "development distance N 901")
	contains(t, w[0], "998")
	if w := FieldWarnings("5.90.0"); len(w) != 0 {
		t.Fatalf("minor 90 warned: %v", w)
	}
	w = FieldWarnings("5.91.0")
	if len(w) != 1 {
		t.Fatalf("minor 91: %v", w)
	}
	contains(t, w[0], "minor 91")
	contains(t, w[0], "99")
	if w := FieldWarnings("5.6.90"); len(w) != 0 {
		t.Fatalf("patch 90 warned: %v", w)
	}
	w = FieldWarnings("5.6.91")
	if len(w) != 1 {
		t.Fatalf("patch 91: %v", w)
	}
	contains(t, w[0], "patch 91")
	if n := len(FieldWarnings("5.91.91-dev.901+abcdef0")); n != 3 {
		t.Fatalf("three warnings wanted, got %d", n)
	}
	if w := FieldWarnings("5.6.1"); len(w) != 0 {
		t.Fatalf("a release has a distance: %v", w)
	}
	if w := FieldWarnings("not a version"); len(w) != 0 {
		t.Fatalf("a warning on garbage: %v", w)
	}
}

// TestFieldProblemsNameEveryPartOutOfRangeNotOnlyTheFirst.
func TestFieldProblemsNameEveryPartOutOfRangeNotOnlyTheFirst(t *testing.T) {
	t.Parallel()
	problems := FieldProblems("210.100.100-dev.999+abcdef0")
	if len(problems) != 4 {
		t.Fatalf("problems = %v", problems)
	}
	for _, field := range []string{"major 210", "minor 100", "patch 100", "development distance N 999"} {
		found := false
		for _, p := range problems {
			if strings.Contains(p, field) {
				found = true
			}
		}
		if !found {
			t.Errorf("no problem names %q", field)
		}
	}
	if p := FieldProblems("209.99.99-dev.998+abcdef0"); len(p) != 0 {
		t.Fatalf("the limits themselves: %v", p)
	}
}

// TestTheRuleAloneHasNoFloor: the rule is arithmetic; the floor applies at a stamp.
func TestTheRuleAloneHasNoFloor(t *testing.T) {
	t.Parallel()
	if c, _ := VersionCodeFor("1.0.0"); c != mustInt(t, "10000999") {
		t.Fatalf("1.0.0 = %d", c)
	}
	if c, _ := VersionCodeFor("0.1.0"); c != mustInt(t, "100999") {
		t.Fatalf("0.1.0 = %d", c)
	}
}

// TestEveryAcceptedCodeBeatsTheInstaller pins the smallest code of major 5.
func TestEveryAcceptedCodeBeatsTheInstaller(t *testing.T) {
	t.Parallel()
	smallest, _ := VersionCodeFor("5.0.0-dev.1+abcdef0")
	if smallest != mustInt(t, "50000001") || smallest <= mustInt(t, installerCodeText) {
		t.Fatalf("smallest = %d", smallest)
	}
	zero, _ := VersionCodeFor("5.0.0-dev.0+abcdef0")
	if zero != mustInt(t, "50000000") {
		t.Fatalf("5.0.0-dev.0 = %d", zero)
	}
}

// fixedRelease is a ReleaseSource that answers one tag and major.
func fixedRelease(tag string, major int) ReleaseSource {
	return func() (string, int, error) { return tag, major, nil }
}

// TestNewestReleaseIsTheHighestByNumberIgnoringPreAndMalformedTags: "v9.9.9" sorts after "v10.0.0" as a string; the number
// decides, and a pre-release never does.
func TestNewestReleaseIsTheHighestByNumberIgnoringPreAndMalformedTags(t *testing.T) {
	t.Parallel()
	repo := testkit.GitRepo(t, "v5.20.0", "v6.0.0", "vv7.0.0", "v7.0.0-rc.1", "v9.9.9", "v10.0.0", "v11.0.0-rc.1")
	tag, major, err := NewestRelease(repo, testkit.Env(t))
	if err != nil {
		t.Fatal(err)
	}
	if tag != "v10.0.0" || major != mustInt(t, "10") {
		t.Fatalf("NewestRelease = %s %d", tag, major)
	}
}

// TestAReleaseTagHEADDoesNotDescendFromStillCounts: every release tag the clone knows is the floor's input.
func TestAReleaseTagHEADDoesNotDescendFromStillCounts(t *testing.T) {
	t.Parallel()
	env := testkit.Env(t)
	repo := testkit.GitRepo(t, floorTag)
	testkit.Git(t, env, repo, "checkout", "-q", "-b", "elsewhere")
	testkit.Git(t, env, repo, "commit", "-q", "--allow-empty", "-m", "later")
	testkit.Git(t, env, repo, "update-ref", "refs/tags/v12.0.0", "HEAD")
	testkit.Git(t, env, repo, "checkout", "-q", "-")
	if listed := strings.TrimSpace(testkit.Git(t, env, repo, "tag", "--merged", "HEAD", "--list", "v12.0.0")); listed != "" {
		t.Fatalf("v12.0.0 is reachable from HEAD: %q", listed)
	}
	tag, major, err := NewestRelease(repo, env)
	if err != nil {
		t.Fatal(err)
	}
	if tag != "v12.0.0" || major != mustInt(t, "12") {
		t.Fatalf("NewestRelease = %s %d", tag, major)
	}
}

// TestNoReleaseTagIsRefusedAndNamesTheFetch.
func TestNoReleaseTagIsRefusedAndNamesTheFetch(t *testing.T) {
	t.Parallel()
	repo := testkit.GitRepo(t, "v5.0.0-rc.1", "vv6.0.0", "release-6")
	_, _, err := NewestRelease(repo, testkit.Env(t))
	if err == nil {
		t.Fatal("no error")
	}
	contains(t, err.Error(), "no release tag")
	contains(t, err.Error(), "fetch")
}

// TestAPathGitCannotReadIsRefusedNotDefaulted: a missing path, not a directory git could walk up from.
func TestAPathGitCannotReadIsRefusedNotDefaulted(t *testing.T) {
	t.Parallel()
	_, _, err := NewestRelease(filepath.Join(t.TempDir(), "not-there"), testkit.Env(t))
	if err == nil {
		t.Fatal("no error")
	}
	contains(t, err.Error(), "git cannot list the tags")
}

// TestThisCloneReadsTheSameNewestTagAsGitListsIt reads the checkout's own tags; a shallow clone lists none and skips.
func TestThisCloneReadsTheSameNewestTagAsGitListsIt(t *testing.T) {
	t.Parallel()
	root := repoRoot(t)
	listed, err := exec.Command("git", "-C", root, "tag", "--list", "v*").Output()
	if err != nil {
		t.Fatal(err)
	}
	var newest [3]int
	found := false
	for _, name := range strings.Fields(string(listed)) {
		m := releaseRE.FindStringSubmatch(name)
		if m == nil {
			continue
		}
		v := [3]int{satAtoi(m[1]), satAtoi(m[2]), satAtoi(m[3])}
		if !found || lessVersion(newest, v) {
			newest, found = v, true
		}
	}
	if !found {
		t.Skip("this clone lists no release tag (a shallow checkout); the refusal is tested in the throwaway repositories")
	}
	tag, major, err := NewestRelease(root, nil)
	if err != nil {
		t.Fatal(err)
	}
	if tag != "v"+strings.Join([]string{itoa(newest[0]), itoa(newest[1]), itoa(newest[2])}, ".") || major != newest[0] {
		t.Fatalf("NewestRelease = %s %d, git lists %v", tag, major, newest)
	}
}

// itoa is an int as decimal text.
func itoa(n int) string { return strconv.Itoa(n) }

// TestAVersionBelowTheNewestMajorIsRefusedAndNamesTheTag: "1.0.0" is what a build with no git reports.
func TestAVersionBelowTheNewestMajorIsRefusedAndNamesTheTag(t *testing.T) {
	t.Parallel()
	for _, low := range []string{"1.0.0", "0.1.0", "5.20.0", "5.6.1-dev.37+3a35e0b", "5.99.99"} {
		t.Run(low, func(t *testing.T) {
			t.Parallel()
			_, err := StampCodeFor(low, fixedRelease(floorTag, floorMajor))
			if err == nil || !strings.Contains(err.Error(), "below major 6, the major of the newest release tag v6.0.0") {
				t.Fatalf("error = %v", err)
			}
		})
	}
}

// TestTheNewestMajorIsAcceptedWithTheRulesCode.
func TestTheNewestMajorIsAcceptedWithTheRulesCode(t *testing.T) {
	t.Parallel()
	src := fixedRelease(floorTag, floorMajor)
	if c, err := StampCodeFor("6.0.0", src); err != nil || c != mustInt(t, "60000999") {
		t.Fatalf("6.0.0 = %d, %v", c, err)
	}
	if c, err := StampCodeFor("6.0.1-dev.37+3a35e0b", src); err != nil || c != mustInt(t, "60001037") {
		t.Fatalf("6.0.1-dev.37 = %d, %v", c, err)
	}
}

// TestTheFloorFollowsTheNewestReleaseWhenItMoves: a new major raises the floor with it.
func TestTheFloorFollowsTheNewestReleaseWhenItMoves(t *testing.T) {
	t.Parallel()
	src := fixedRelease("v7.0.0", mustInt(t, "7"))
	if _, err := StampCodeFor("6.0.1-dev.1+abcdef0", src); err == nil || !strings.Contains(err.Error(), "below major 7") {
		t.Fatalf("error = %v", err)
	}
	if c, err := StampCodeFor("7.0.0", src); err != nil || c != mustInt(t, "70000999") {
		t.Fatalf("7.0.0 = %d, %v", c, err)
	}
}

// TestAMalformedVersionIsRefusedByTheRuleNotTheFloor.
func TestAMalformedVersionIsRefusedByTheRuleNotTheFloor(t *testing.T) {
	t.Parallel()
	_, err := StampCodeFor("v6.0.0", fixedRelease(floorTag, floorMajor))
	if err == nil || !strings.Contains(err.Error(), "neither X.Y.Z") {
		t.Fatalf("error = %v", err)
	}
}

// TestNoReleaseTagMeansNoStamp.
func TestNoReleaseTagMeansNoStamp(t *testing.T) {
	t.Parallel()
	none := func() (string, int, error) {
		return "", 0, &VersionError{Msg: "no release tag (vX.Y.Z) in repo"}
	}
	_, err := StampCodeFor("6.0.0", none)
	if err == nil || !strings.Contains(err.Error(), "no release tag") {
		t.Fatalf("error = %v", err)
	}
}

// TestReadsTheCodeAndItsOffset: the code and the offset of its 4 bytes, in both pool encodings.
func TestReadsTheCodeAndItsOffset(t *testing.T) {
	t.Parallel()
	for _, utf8 := range []bool{true, false} {
		t.Run(map[bool]string{true: "utf8", false: "utf16"}[utf8], func(t *testing.T) {
			t.Parallel()
			data := manifestFor(mustInt64(t, storeCodeText), utf8)
			got, err := ParseManifest(data)
			if err != nil {
				t.Fatal(err)
			}
			if got.VersionCode != mustInt(t, storeCodeText) {
				t.Fatalf("code = %d", got.VersionCode)
			}
			if v := binary.LittleEndian.Uint32(data[got.Offset:]); v != uint32(mustInt(t, storeCodeText)) {
				t.Fatalf("bytes at the offset = %d", v)
			}
		})
	}
}

// mustInt64 parses digits to int64.
func mustInt64(t *testing.T, s string) int64 {
	t.Helper()
	return int64(mustInt(t, s))
}

// TestRefusesEachDefect: one planted defect per case, each refused.
func TestRefusesEachDefect(t *testing.T) {
	t.Parallel()
	cases := map[string]func(*axmlfix.Spec){
		"second versionCode":       func(s *axmlfix.Spec) { s.VersionCodeCount = 2 },
		"string-typed code":        func(s *axmlfix.Spec) { s.VersionCodeType = axmlfix.TypeString },
		"hex code":                 func(s *axmlfix.Spec) { s.VersionCodeType = axmlfix.TypeIntHex },
		"text twin":                func(s *axmlfix.Spec) { s.VersionCodeRaw = 0 },
		"wrong resource id":        func(s *axmlfix.Spec) { s.VersionCodeID = axmlfix.ResMinSDK },
		"no android namespace":     func(s *axmlfix.Spec) { s.VersionCodeNSAndroid = false },
		"versionCodeMajor":         func(s *axmlfix.Spec) { s.WithVersionCodeMajor = true },
		"no resource map":          func(s *axmlfix.Spec) { s.WithResourceMap = false },
		"root is not manifest":     func(s *axmlfix.Spec) { s.Root = "application" },
		"bytes after the document": func(s *axmlfix.Spec) { s.Trailing = make([]byte, fourZeroBytes) },
	}
	for label, mutate := range cases {
		t.Run(label, func(t *testing.T) {
			t.Parallel()
			s := axmlfix.Default()
			mutate(&s)
			if _, err := ParseManifest(s.Build()); err == nil {
				t.Fatal("accepted")
			}
		})
	}
}

// TestAVersionCodeOnAChildElementIsNotTheApps: only the root <manifest> counts.
func TestAVersionCodeOnAChildElementIsNotTheApps(t *testing.T) {
	t.Parallel()
	s := axmlfix.Default()
	s.NestedVersionCode = true
	got, err := ParseManifest(s.Build())
	if err != nil || got.VersionCode != mustInt(t, storeCodeText) {
		t.Fatalf("got %d, %v", got.VersionCode, err)
	}
}

// TestRefusesGarbage: each malformed input is a ManifestError.
func TestRefusesGarbage(t *testing.T) {
	t.Parallel()
	good := axmlfix.Default().Build()
	unknown := axmlfix.Default()
	unknown.Trailing = le(trailingChunkKind, emptyChunkBytes, emptyChunkBytes)
	sizeLie := append(append(append([]byte{}, good[:4]...), le32(len(good)+fourZeroBytes)...), good[chunkSizeWord:]...)
	cases := map[string][]byte{
		"empty":              {},
		"short":              {0x03, 0x00, 0x08, 0x00},
		"wrong type":         le(chunkKindFirst, emptyChunkBytes, emptyChunkBytes),
		"size mismatch":      good[:len(good)-fourZeroBytes],
		"size field lies":    sizeLie,
		"unknown chunk":      unknown.Build(),
		"plain xml":          []byte("<manifest/>"),
		"short chunk header": shortHeaderDocument(),
		"zeros":              make([]byte, zeroBytes),
	}
	for label, data := range cases {
		t.Run(label, func(t *testing.T) {
			t.Parallel()
			if _, err := ParseManifest(data); err == nil {
				t.Fatal("accepted")
			}
		})
	}
}

// le is three little-endian words: a chunk header.
func le(kind, headerSize, size int) []byte {
	out := binary.LittleEndian.AppendUint16(nil, uint16(kind))
	out = binary.LittleEndian.AppendUint16(out, uint16(headerSize))
	return binary.LittleEndian.AppendUint32(out, uint32(size))
}

// le32 is a little-endian uint32.
func le32(v int) []byte { return binary.LittleEndian.AppendUint32(nil, uint32(v)) }

// TestTheRealStoreManifestParsesAndAgreesWithAapt2 runs when NEVR_STORE_APK names the store APK and aapt2 is installed.
func TestTheRealStoreManifestParsesAndAgreesWithAapt2(t *testing.T) {
	t.Parallel()
	store := os.Getenv("NEVR_STORE_APK")
	if store == "" {
		t.Skip("NEVR_STORE_APK is not set: name the store APK to run this check")
	}
	if _, err := os.Stat(store); err != nil {
		t.Skipf("the store APK at NEVR_STORE_APK=%s is not on this machine", store)
	}
	if _, err := exec.LookPath("aapt2"); err != nil {
		t.Skip("aapt2 is not on this machine")
	}
	badging, err := exec.Command("aapt2", "dump", "badging", store).Output()
	if err != nil {
		t.Fatal(err)
	}
	expected := strings.SplitN(strings.SplitN(string(badging), "versionCode='", 2)[1], "'", 2)[0]
	got, err := VersionCodeOfAPK(store)
	if err != nil {
		t.Fatal(err)
	}
	if itoa(got) != expected {
		t.Fatalf("our read %d, aapt2 %s", got, expected)
	}
}

// TestAFourByteWriteAndNothingElse: the patched manifest is the document built directly with the new code.
func TestAFourByteWriteAndNothingElse(t *testing.T) {
	t.Parallel()
	for _, utf8 := range []bool{true, false} {
		t.Run(map[bool]string{true: "utf8", false: "utf16"}[utf8], func(t *testing.T) {
			t.Parallel()
			data := manifestFor(mustInt64(t, storeCodeText), utf8)
			out, err := PatchManifest(data, mustInt(t, "50601037"))
			if err != nil {
				t.Fatal(err)
			}
			if len(out) != len(data) {
				t.Fatal("length changed")
			}
			var diff []int
			for i := range data {
				if data[i] != out[i] {
					diff = append(diff, i)
				}
			}
			if len(diff) == 0 || diff[len(diff)-1]-diff[0] >= patchWidth {
				t.Fatalf("diff %v is wider than the 4 bytes", diff)
			}
			if got, _ := ParseManifest(out); got.VersionCode != mustInt(t, "50601037") {
				t.Fatalf("code = %d", got.VersionCode)
			}
			if !bytes.Equal(out, manifestFor(mustInt64(t, "50601037"), utf8)) {
				t.Fatal("not the document built directly with the new code")
			}
		})
	}
}

// patchWidth is the width of a versionCode write.
const patchWidth = 4

// TestOnlyEverRaises: the stamp never lowers or repeats a code.
func TestOnlyEverRaises(t *testing.T) {
	t.Parallel()
	data := manifestFor(mustInt64(t, "50601037"), true)
	for _, code := range []string{"50601037", "50601036", storeCodeText} {
		t.Run(code, func(t *testing.T) {
			t.Parallel()
			_, err := PatchManifest(data, mustInt(t, code))
			if err == nil || !strings.Contains(err.Error(), "only ever raises") {
				t.Fatalf("error = %v", err)
			}
		})
	}
}

// TestRefusesACodeOutOfRange.
func TestRefusesACodeOutOfRange(t *testing.T) {
	t.Parallel()
	data := manifestFor(mustInt64(t, storeCodeText), true)
	for _, code := range []int{0, -1, MaxVersionCode, int32Bound, uint32Limit} {
		t.Run(itoa(code), func(t *testing.T) {
			t.Parallel()
			if _, err := PatchManifest(data, code); err == nil {
				t.Fatal("accepted")
			}
		})
	}
}

// TestRefusesAManifestTheReaderRefuses.
func TestRefusesAManifestTheReaderRefuses(t *testing.T) {
	t.Parallel()
	s := axmlfix.Default()
	s.VersionCodeCount = 2
	if _, err := PatchManifest(s.Build(), mustInt(t, "50601037")); err == nil {
		t.Fatal("accepted")
	}
}

// TestTheAfterWriteComparisonCatchesAWriteThatLandsElsewhereToo plants a stray write beside the code. It replaces the
// package's packCode, so it is not parallel: no other test runs while it does.
func TestTheAfterWriteComparisonCatchesAWriteThatLandsElsewhereToo(t *testing.T) {
	data := manifestFor(mustInt64(t, storeCodeText), true)
	saved := packCode
	t.Cleanup(func() { packCode = saved })
	packCode = func(b []byte, off int, code uint32) {
		binary.LittleEndian.PutUint32(b[off:], code)
		b[off+strayOffset] ^= 0xFF
	}
	_, err := PatchManifest(data, mustInt(t, "50601037"))
	if err == nil || !strings.Contains(err.Error(), "exactly the versionCode bytes") {
		t.Fatalf("error = %v", err)
	}
}

// TestAapt2ReadsThePatchedManifest: a reader that is not ours confirms the write.
func TestAapt2ReadsThePatchedManifest(t *testing.T) {
	t.Parallel()
	for _, tool := range []string{"aapt2", "zip"} {
		if _, err := exec.LookPath(tool); err != nil {
			t.Skipf("%s is missing", tool)
		}
	}
	out, err := PatchManifest(axmlfix.Default().Build(), mustInt(t, "50601037"))
	if err != nil {
		t.Fatal(err)
	}
	apk := filepath.Join(t.TempDir(), "m.apk")
	writeZip(t, apk, map[string][]byte{manifestEntry: out})
	badging, err := exec.Command("aapt2", "dump", "badging", apk).Output()
	if err != nil {
		t.Fatal(err)
	}
	contains(t, string(badging), "versionCode='50601037'")
	got, err := VersionCodeOfAPK(apk)
	if err != nil || got != mustInt(t, "50601037") {
		t.Fatalf("VersionCodeOfAPK = %d, %v", got, err)
	}
}

// TestReadsKVersion.
func TestReadsKVersion(t *testing.T) {
	t.Parallel()
	path := writeHeader(t, `namespace nevr { constexpr const char* kVersion = "`+versionNameShim+`"; }`+"\n")
	got, err := ReadBuildVersion(path)
	if err != nil || got != versionNameShim {
		t.Fatalf("ReadBuildVersion = %q, %v", got, err)
	}
}

// TestRefusesNoKVersionOrTwo, and a missing file.
func TestRefusesNoKVersionOrTwo(t *testing.T) {
	t.Parallel()
	for _, text := range []string{"", `kVersion = "5.6.0"; kVersion = "5.6.1";`} {
		if _, err := ReadBuildVersion(writeHeader(t, text)); err == nil {
			t.Fatalf("%q accepted", text)
		}
	}
	if _, err := ReadBuildVersion(filepath.Join(t.TempDir(), "missing.h")); err == nil {
		t.Fatal("a missing file accepted")
	}
}

// writeHeader writes a build info header with the text in a temporary directory and returns its path.
func writeHeader(t *testing.T, text string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "nevr_build_info.h")
	if err := os.WriteFile(path, []byte(text), 0o644); err != nil {
		t.Fatal(err)
	}
	return path
}

// TestTheShimMustCarryTheWholeVersion: a longer or shorter version is another build.
func TestTheShimMustCarryTheWholeVersion(t *testing.T) {
	t.Parallel()
	version := versionNameShim
	if !ShimCarriesVersion([]byte("\x00"+version+"\x00"), version) {
		t.Fatal("exact match refused")
	}
	if ShimCarriesVersion([]byte("\x00"+version+"9\x00"), version) {
		t.Fatal("a longer string accepted")
	}
	if ShimCarriesVersion([]byte("x"+version+"\x00"), version) {
		t.Fatal("a longer prefix accepted")
	}
	if ShimCarriesVersion([]byte("\x00"+shimOtherVersion+"\x00"), version) {
		t.Fatal("a different version accepted")
	}
}

// shortHeaderDocument is the store manifest followed by a chunk whose header is shorter than a chunk header: a kind, a
// header size of four and a size of eight, the whole of the chunk. Only the header-size check refuses it: its size fits,
// and its kind is a known one.
func shortHeaderDocument() []byte {
	s := axmlfix.Default()
	s.Trailing = le(int(resXMLStartNS), shortChunkHeader, emptyChunkBytes)
	return s.Build()
}

// shortChunkHeader is a chunk header size below the eight bytes every chunk header takes.
const shortChunkHeader = 4
