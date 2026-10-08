package quest

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The login hook's translation unit is the GOT thunk's handler side (callback_thunk.h): every
// frame in it must sit under the personality-free "zR" CIE and it must carry no LSDA, so the
// game's original (called from the handler) never runs under a frame that names the sentinel's
// personality. The rewrite itself lives in login_rewrite.cpp, which keeps exceptions and so
// must show a "zPLR" CIE: that is the control showing the parser can see a personality.
const (
	loginHookObject    = "../../build/android-arm64/CMakeFiles/nevr_quest_login.dir/login/login_hook.cpp.o"
	loginRewriteObject = "../../build/android-arm64/CMakeFiles/nevr_quest_login.dir/login/login_rewrite.cpp.o"
)

func objectPath(t *testing.T, rel string) string {
	t.Helper()
	p, err := filepath.Abs(rel)
	if err != nil {
		t.Fatalf("resolve path: %v", err)
	}
	if _, err := os.Stat(p); err != nil {
		t.Fatalf("object not found at %s (run `just build-android`): %v", p, err)
	}
	return p
}

// cieAugmentations returns the augmentation string of every CIE in the object's .eh_frame.
func cieAugmentations(t *testing.T, path string) (augs []string, fdes int) {
	t.Helper()
	current := false
	for _, line := range strings.Split(run(t, "readelf", "--debug-dump=frames", path), "\n") {
		if cieRe.MatchString(line) {
			current = true
			continue
		}
		if m := augRe.FindStringSubmatch(line); m != nil && current {
			augs = append(augs, m[1])
			current = false
			continue
		}
		if fdeRe.MatchString(line) {
			fdes++
		}
	}
	return augs, fdes
}

func TestLoginHookObjectCarriesNoPersonality(t *testing.T) {
	hook := objectPath(t, loginHookObject)
	augs, fdes := cieAugmentations(t, hook)
	if len(augs) == 0 || fdes == 0 {
		t.Fatalf("parsed no CIE/FDE from %s; the parser is blind", hook)
	}
	for _, aug := range augs {
		if aug != `"zR"` {
			t.Errorf("login_hook.cpp.o has a CIE with augmentation %s, want only \"zR\" (no personality, no LSDA)", aug)
		}
	}
	if sections := run(t, "readelf", "-S", "-W", hook); strings.Contains(sections, ".gcc_except_table") {
		t.Errorf("login_hook.cpp.o carries a .gcc_except_table (an LSDA): the handler TU must be built -fno-exceptions")
	}

	// Control: the rewrite object is built with exceptions and must show a personality.
	rewrite := objectPath(t, loginRewriteObject)
	rewriteAugs, _ := cieAugmentations(t, rewrite)
	sawPersonality := false
	for _, aug := range rewriteAugs {
		sawPersonality = sawPersonality || strings.Contains(aug, "P")
	}
	if !sawPersonality {
		t.Errorf("login_rewrite.cpp.o shows no personality CIE (%v): the check cannot tell the two apart", rewriteAugs)
	}
}
