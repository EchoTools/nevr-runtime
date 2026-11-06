package logwiring

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// #2245: a line the game logs from its error dialog (echovr.exe 0x1400EA730) is recorded at error. test_error_dialog_depth.cpp
// and test_run_log_record.cpp drive the behaviour; these pin that the pieces those tests drive are the ones the hooks call.

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

// body returns the text of the braced function whose signature starts with sig.
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

func mustContain(t *testing.T, where, text, want string) {
	t.Helper()
	if !strings.Contains(text, want) {
		t.Errorf("%s does not contain %q", where, want)
	}
}

func TestTheFilterHookAsksWhetherTheLineComesFromTheDialog(t *testing.T) {
	hook := body(t, code(t, "src/runtime/log/builtin_filter.cpp"), "static void __fastcall hook_PrintfImpl(")
	mustContain(t, "hook_PrintfImpl", hook, "const bool insideErrorDialog = nevr_error_dialog::Inside();")
	mustContain(t, "hook_PrintfImpl", hook, "const uint32_t effective = nevr_log_level::Effective(level, insideErrorDialog);")
	inside, gate := strings.Index(hook, "nevr_error_dialog::Inside()"), strings.Index(hook, "ShouldSuppress(buf, effective, &entry)")
	if inside < 0 || gate < inside {
		t.Errorf("the level gate does not see the level the line is recorded at: Inside at %d, ShouldSuppress at %d", inside, gate)
	}
	// The record gets the game's level and the mark, so it writes error and game_level.
	mustContain(t, "hook_PrintfImpl", hook, "EmitLine(level, insideErrorDialog, buf, emit_len);")
}

func TestTheLevelGateIsTheOneTheTestsDrive(t *testing.T) {
	should := body(t, code(t, "src/runtime/log/builtin_filter.cpp"), "static bool ShouldSuppress(")
	mustContain(t, "ShouldSuppress", should, "nevr_log_level::DroppedByMinLevel(level, g_config.min_level)")
	if strings.Contains(should, "level < g_config.min_level") {
		t.Error("ShouldSuppress keeps a second copy of the min_level rule")
	}
}

func TestTheFilterInstallsTheDialogHookOnceItsOwnHookIsInPlace(t *testing.T) {
	init := body(t, code(t, "src/runtime/log/builtin_filter.cpp"), "void nevr_builtin_log_filter::Init(")
	install := "nevr_error_dialog::Install(base_addr);"
	if n := strings.Count(init, install); n != 1 {
		t.Fatalf("Init calls %s %d times, want 1", install, n)
	}
	record := strings.Index(init, `nevr_hook_guard::Record(g_hook_target, "CLog::PrintfImpl");`)
	if record < 0 || strings.Index(init, install) < record {
		t.Error("the dialog hook is installed before the CLog hook is in place, when its lines cannot reach the filter")
	}
}

func TestTheDialogHookRunsTheOriginalThroughTheMarkedScope(t *testing.T) {
	hook := body(t, code(t, "src/runtime/log/error_dialog_level.cpp"), "void ErrorDialogHook(")
	mustContain(t, "ErrorDialogHook", hook, "Run([&] { g_original(owner, message, title); });")
	if strings.Contains(hook, "t_depth") {
		t.Error("ErrorDialogHook touches t_depth; only Run's guard sets and clears the mark")
	}
}

// The VEH longjmps out of the game loop without running destructors (crash_recovery.cpp, N70 item 3), so the landing
// branch clears the mark itself, before it logs anything.
func TestTheLongjmpLandingClearsAMarkTheLongjmpLeft(t *testing.T) {
	wrapper := body(t, code(t, "src/runtime/lifecycle/crash_recovery.cpp"), "static INT64 GameMainWrapperHook(")
	at := strings.Index(wrapper, "if (crashCount > 0) {")
	if at < 0 {
		t.Fatal("GameMainWrapperHook has no landing branch")
	}
	landing := wrapper[at:]
	firstLog := strings.Index(landing, "Log(")
	reset := strings.Index(landing, "nevr_error_dialog::ResetAfterLongjmp();")
	if reset < 0 || firstLog < 0 || reset > firstLog {
		t.Errorf("the landing does not clear the mark before it logs: reset at %d, first Log at %d", reset, firstLog)
	}
}
