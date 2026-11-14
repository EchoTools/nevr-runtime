package afkdefaulton

import (
	"os"
	"regexp"
	"slices"
	"strings"
	"testing"
)

// The list the repack ships, relative to this package.
const shippedList = "../../quest-afk-default-on.txt"

// The names of the reference build's own list (its QA trial qa-1104-aj4-w2z22, the two safety switches first), as the base APK
// carried it: the list a fresh install used to turn on.
var referenceList = strings.Fields(`fxsndguard.on fxfireguard.on depthrt.on droneguard.on dronekill2.on dronefx.on dronemuzzle.on ecplayo.on
fxchassisfork.on fxchassisforklobby.on fxchassisforksoc.on fxdecal.on fxoutlinefix2.on fxpool18.on fxsprite.on fxvfx.on g40guard.on
hudmeter.on hzlobby90.on neutronfx.on olgate.on peerclamp.on pfx.on pfxlowq.off pfxmesh.on pfxmeshwarm.on pfxnoflow.on pfxribspan.on
pfxtburst.on pfxtev.on pfxtkid.on pfxtmesh.on pfxtovc.on pfxtpar.on pfxtphase.on pfxwarm.on powerboost.on pfxtcomet.on pfxmeteorflow.on
ffr.off drrc.on pvs.off pfxveillow.off p18add.on drrlow.on sensorwd.on drrq2.on texresq2.on olnoteam.on drrhead.on hz72q2.on drrq3.on
pfxargsclear.on volpfx.on pfxgpubar.on`)

// The switch that hangs the GPU in the social lobby on its own (a 5-round bisect on a Quest 2), and the others the task named.
var namedByTheOwner = []string{"pfxwarm.on", "pfxmeshwarm.on", "pfxtev.on", "pfxtkid.on", "pfxtmesh.on", "pfxtovc.on", "pfxtpar.on", "pfxtphase.on", "pfxtburst.on", "pfxtcomet.on"}

func shipped(t *testing.T) []string {
	t.Helper()
	text, err := os.ReadFile(shippedList)
	if err != nil {
		t.Fatal(err)
	}
	names, err := Parse(string(text))
	if err != nil {
		t.Fatal(err)
	}
	return names
}

// The list the repack ships carries no switch the reference source marks TEST.
func TestTheShippedListHasNoTestOnlySwitch(t *testing.T) {
	t.Parallel()
	names := shipped(t)
	if problems := Problems(names); len(problems) > 0 {
		t.Fatalf("the shipped list has TEST-only switches:\n%s", strings.Join(problems, "\n"))
	}
	for _, n := range namedByTheOwner {
		for _, got := range names {
			if got == n {
				t.Errorf("%s is in the shipped list", n)
			}
		}
	}
}

func TestTheShippedListKeepsWhatIsNotMarkedTestOnly(t *testing.T) {
	t.Parallel()
	have := map[string]bool{}
	for _, n := range shipped(t) {
		have[n] = true
	}
	for _, n := range referenceList {
		if _, bad := TestOnly[n]; !bad && !have[n] {
			t.Errorf("%s is not marked TEST in the source and is not in the shipped list", n)
		}
	}
}

// The reference build's own list is refused, naming every switch the owner named and each source line.
func TestTheReferenceListIsRefused(t *testing.T) {
	t.Parallel()
	problems := strings.Join(Problems(referenceList), "\n")
	for _, n := range namedByTheOwner {
		if !strings.Contains(problems, n+" is TEST only (runtime/native/afkxr/afk_pfx.cpp:") {
			t.Errorf("%s is not refused with its source line:\n%s", n, problems)
		}
	}
}

// The switches left out on the module's header or a neighbour's mark, not a line of their own: each is in the table, so a list that
// carries one is refused.
func TestTheJudgedSwitchesAreRefused(t *testing.T) {
	t.Parallel()
	for _, n := range []string{"depthrt.on", "ecplayo.on", "afklog.on", "fxchassisforklobby.on", "pfxtcomet.on", "dronekill2.on", "dronefx.on"} {
		if len(Problems([]string{n})) != 1 {
			t.Errorf("%s is not refused", n)
		}
	}
}

func TestEveryTestOnlyNameCitesASourceLine(t *testing.T) {
	t.Parallel()
	shape := regexp.MustCompile(`^afk_[a-z0-9_]+\.cpp:[1-9][0-9]*$`)
	for name, cite := range TestOnly {
		if !nameShape.MatchString(name) || !shape.MatchString(cite) {
			t.Errorf("%q: %q is not a switch name with a file:line", name, cite)
		}
	}
}

func TestAnEmptyListIsRefused(t *testing.T) {
	t.Parallel()
	if len(Problems(nil)) != 1 {
		t.Fatal("an empty list is not refused")
	}
}

func TestParseSkipsCommentsAndRefusesBadLines(t *testing.T) {
	t.Parallel()
	names, err := Parse("# a list\n\nfxdecal.on\r\nhudmeter.on\n")
	if err != nil || !slices.Equal(names, []string{"fxdecal.on", "hudmeter.on"}) {
		t.Fatalf("Parse = %v, %v", names, err)
	}
	// The runtime strips a trailing CR and nothing else, so a line with spaces around the name would be a bad name there.
	for _, bad := range []string{"fxdecal.on\nfxdecal.on\n", "FxDecal.on\n", "fxdecal\n", "../x.on\n", " hudmeter.on\n", "hudmeter.on \n"} {
		if _, err := Parse(bad); err == nil {
			t.Errorf("Parse(%q) accepted it", bad)
		}
	}
}
