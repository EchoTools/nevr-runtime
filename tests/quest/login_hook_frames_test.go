package quest

import (
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"testing"
)

// The login hook, on the same rule as TestHookFramesCarryNoPersonality (callback_thunk.h, rule
// 1): a frame that is live while game code runs must sit under the personality-free "zR" CIE.
// The login code is not part of the sentinel .so yet, so this test applies the same walk to a
// probe executable that links the whole nevr_quest_login archive (login_frames_probe): start
// from every hook record's entry and handler (the nevr_hook_records section), follow direct
// bl/b edges and fail on any reachable function under a personality-bearing CIE.
//
// The exemption is the shared one (frames_sensor_test.go): ComposePlan, the exceptions-enabled compose
// phase, calls no game code and has returned before the next game call, so it is marked
// NEVR_OUTSIDE_GAME_CALL and the walk does not enter it. The test requires that it IS reached as an
// annotated function, so the exemption cannot go stale, and that it does sit under a personality, so
// the parser is not blind to one.
const loginProbeRel = "../../build/android-arm64/login_frames_probe"

var composePlanRe = regexp.MustCompile(`^_ZN16nevr_quest_login11ComposePlanE`)

func probePath(t *testing.T) string {
	t.Helper()
	p, err := filepath.Abs(loginProbeRel)
	if err != nil {
		t.Fatalf("resolve path: %v", err)
	}
	if _, err := os.Stat(p); err != nil {
		t.Fatalf("probe executable not found at %s (run `just build-android`): %v", p, err)
	}
	return p
}

func framesOf(t *testing.T, path string) (map[string]string, map[uint64]string) {
	t.Helper()
	cieAug := map[string]string{}
	fdeCIE := map[uint64]string{}
	current := ""
	for _, line := range strings.Split(run(t, "readelf", "--debug-dump=frames", path), "\n") {
		if m := cieRe.FindStringSubmatch(line); m != nil {
			current = m[1]
			continue
		}
		if m := augRe.FindStringSubmatch(line); m != nil && current != "" {
			cieAug[current] = m[1]
			current = ""
			continue
		}
		if m := fdeRe.FindStringSubmatch(line); m != nil {
			pc, err := strconv.ParseUint(m[2], 16, 64)
			if err != nil {
				t.Fatalf("bad pc %q", m[2])
			}
			fdeCIE[pc] = m[1]
		}
	}
	if len(cieAug) == 0 || len(fdeCIE) == 0 {
		t.Fatalf("parsed no CIE/FDE from %s; the parser is blind", path)
	}
	return cieAug, fdeCIE
}

func funcsOf(t *testing.T, path string) []elfFunc {
	t.Helper()
	sections := map[string]string{}
	for _, line := range strings.Split(run(t, "readelf", "-SW", path), "\n") {
		if m := secRe.FindStringSubmatch(line); m != nil {
			sections[m[1]] = m[2]
		}
	}
	var fs []elfFunc
	for _, line := range strings.Split(run(t, "readelf", "-sW", path), "\n") {
		m := symRe.FindStringSubmatch(line)
		if m == nil {
			continue
		}
		addr, _ := strconv.ParseUint(m[1], 16, 64)
		size, _ := strconv.ParseUint(m[2], 10, 64)
		if size == 0 {
			continue
		}
		fs = append(fs, elfFunc{addr: addr, size: size, name: m[4], section: sections[m[3]]})
	}
	if len(fs) == 0 {
		t.Fatalf("no function symbols parsed from %s", path)
	}
	return fs
}

func TestLoginHookFramesCarryNoPersonality(t *testing.T) {
	art := loadFrameGraph(t, probePath(t))
	v := walkHookFrames(art.graph)
	for _, msg := range v.violations {
		t.Error(msg)
	}
	// ComposePlan is the exceptions-enabled compose phase. It is exempt only through its
	// NEVR_OUTSIDE_GAME_CALL annotation; the walk must reach it (so the exemption cannot go stale) and it
	// must sit under a personality (so the parser is not blind to one).
	if !anyMatches(v.annotatedReached, composePlanRe) {
		t.Errorf("ComposePlan was not reached as an annotated function: the annotation or the edge is gone (reached: %v)", v.annotatedReached)
	}
	for addr, name := range art.graph.names {
		if composePlanRe.MatchString(name) && !strings.Contains(art.graph.aug[addr], "P") {
			t.Errorf("ComposePlan sits under CIE augmentation %s, want a personality-bearing CIE", art.graph.aug[addr])
		}
	}
	if v.checked < len(art.graph.roots)+3 {
		t.Errorf("checked only %d functions for %d roots: the walk lost its edges", v.checked, len(art.graph.roots))
	}
	t.Logf("login roots=%d, functions checked=%d", len(art.graph.roots), v.checked)
}
