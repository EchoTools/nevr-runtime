package combatwiring

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// #2289: VeilNet's MAP SWITCH and END MATCH gates act on the session game type the runtime stores
// (nevr_arena_round_time::SessionGameType), through combat/match_type.h, which test_combat_match_type.cpp drives. These
// pin that the gates veilnet.cpp runs are the ones that read it, and that the game's session log line, which echovr.exe
// logs and the module does not watch, is no longer a source.

func root(t *testing.T) string {
	t.Helper()
	wd, err := os.Getwd()
	if err != nil {
		t.Fatal(err)
	}
	r := filepath.Join(wd, "..", "..", "..")
	if _, err := os.Stat(filepath.Join(r, "tools", "go.mod")); err != nil {
		t.Fatalf("%s is not the repository root: %v", r, err)
	}
	return r
}

var blockComment = regexp.MustCompile(`(?s)/\*.*?\*/`)
var lineComment = regexp.MustCompile(`(?m)^\s*//[^\n]*|\s//[^\n]*`)

// code reads a source file with its comments removed, so the prose that explains a rule cannot satisfy its check.
func code(t *testing.T, path string) string {
	t.Helper()
	b, err := os.ReadFile(filepath.Join(root(t), path))
	if err != nil {
		t.Fatal(err)
	}
	return lineComment.ReplaceAllString(blockComment.ReplaceAllString(string(b), ""), "")
}

// body returns the text of the braced block whose opening line starts with sig.
func body(t *testing.T, src, sig string) string {
	t.Helper()
	at := strings.Index(src, sig)
	if at < 0 {
		t.Fatalf("no %q", sig)
	}
	open := at + strings.Index(src[at:], "{")
	depth := 0
	for i := open; i < len(src); i++ {
		switch src[i] {
		case '{':
			depth++
		case '}':
			depth--
			if depth == 0 {
				return src[open : i+1]
			}
		}
	}
	t.Fatalf("%q has no closing brace", sig)
	return ""
}

const veilnet = "src/runtime/combat/veilnet.cpp"

func TestTheMatchTypeComesFromTheStoredSessionGameType(t *testing.T) {
	src := code(t, veilnet)
	privacy := body(t, src, "nevr_combat::MatchPrivacy SessionPrivacy()")
	if !strings.Contains(privacy, "nevr_arena_round_time::SessionGameType()") ||
		!strings.Contains(privacy, "nevr_combat::PrivacyOf(type)") {
		t.Errorf("SessionPrivacy no longer reads the stored session game type through PrivacyOf:\n%s", privacy)
	}
	if !strings.Contains(privacy, "Log(") {
		t.Error("SessionPrivacy no longer logs the match type it acts on")
	}
}

func TestBothGatesAskSessionPrivacy(t *testing.T) {
	src := code(t, veilnet)
	for _, gate := range []string{
		"if (p.type == 'C' && (p.id & 0xF0000000) == WANT_MAP && (p.id & 0xF) < MAP_COUNT)",
		"if (p.type == 'C' && p.id == WANT_END)",
	} {
		b := body(t, src, gate)
		if !strings.Contains(b, "SessionPrivacy()") || !strings.Contains(b, "!= nevr_combat::MatchPrivacy::kPrivate") {
			t.Errorf("the gate %q does not refuse unless SessionPrivacy() says private:\n%s", gate, b)
		}
	}
}

func TestTheServerAsksOnceASecondSoTheChangeLineShows(t *testing.T) {
	src := code(t, veilnet)
	if !regexp.MustCompile(`if \(g_server && GetTickCount64\(\) >= next_privacy\) \{\s*next_privacy = GetTickCount64\(\) \+ 1000;\s*SessionPrivacy\(\);`).MatchString(src) {
		t.Error("the tick loop no longer asks SessionPrivacy() once a second on a server")
	}
}

func TestTheGamesSessionLineIsNoLongerASource(t *testing.T) {
	src := code(t, veilnet)
	for _, gone := range []string{"g_private_match", "kSessionFmt", "Starting session (gametype"} {
		if strings.Contains(src, gone) {
			t.Errorf("%s still names %q: the match type has one source, the stored session game type", veilnet, gone)
		}
	}
}
