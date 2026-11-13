package logwiring

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// Peer timeout logging: the PC hooks and their call sites (source sensors, rank 3).
//
// The detours run inside echovr.exe, so no test target in this tree drives them. The pure rules are GTest
// (src/runtime/tests/peer_timeout_log_test.cpp). These sensors check what the hook file and its callers say:
//
//   - the server hook sits on CBroadcaster::DestroyPeer and logs every removal at its reason's level;
//   - the hook reads the peer record before the original destroys it, and forwards every argument to the original;
//   - the install checks each prologue before it attaches;
//   - boot installs the hook for the run's role; the NetGame state hook reports in-game entry and exit; the frame tick
//     checks the no-send threshold.

const (
	peerTimeoutHook = "src/runtime/patch/peer_timeout_log_hook.cpp"
)

// stripCppComments removes // line comments and /* */ block comments from C++ (or CMake) source text, tracking string
// and character literals so a literal holding "//" or "/*" (CMakeLists.txt's "installer/src/*.rs" glob) is not read as
// a comment start. A closed block comment leaves one space, as the compiler treats it as white space, so
// "return/**/Call();" does not collapse into "returnCall();". Newlines inside a comment are kept. The regex stripper
// code() uses has neither property, which is why this file carries its own.
func stripCppComments(text string) string {
	var out strings.Builder
	out.Grow(len(text))
	n := len(text)
	inLine, inBlock, inString, inChar := false, false, false, false
	for i := 0; i < n; {
		c := text[i]
		two := ""
		if i+1 < n {
			two = text[i : i+2]
		}
		switch {
		case inLine:
			if c == '\n' {
				inLine = false
				out.WriteByte(c)
			}
			i++
		case inBlock:
			if two == "*/" {
				inBlock = false
				i += 2
				out.WriteByte(' ')
				continue
			}
			if c == '\n' {
				out.WriteByte('\n')
			}
			i++
		case inString || inChar:
			out.WriteByte(c)
			if c == '\\' && i+1 < n {
				out.WriteByte(text[i+1])
				i += 2
				continue
			}
			if inString && c == '"' {
				inString = false
			} else if inChar && c == '\'' {
				inChar = false
			}
			i++
		case two == "//":
			inLine = true
			i += 2
		case two == "/*":
			inBlock = true
			i += 2
		default:
			if c == '"' {
				inString = true
			} else if c == '\'' {
				inChar = true
			}
			out.WriteByte(c)
			i++
		}
	}
	return out.String()
}

// source reads a tree file with its comments removed by stripCppComments.
func source(t *testing.T, path string) string {
	t.Helper()
	b, err := os.ReadFile(filepath.Join(root(t), path))
	if err != nil {
		t.Fatal(err)
	}
	return stripCppComments(string(b))
}

// indexOf is the first (or, with last, the final) offset of what in text; a missing what fails the test.
func indexOf(t *testing.T, where, text, what string, last bool) int {
	t.Helper()
	at := strings.Index(text, what)
	if last {
		at = strings.LastIndex(text, what)
	}
	if at < 0 {
		t.Fatalf("%s does not contain %q", where, what)
	}
	return at
}

// inOrder fails unless a's offset in text is below b's (a's first, b's first or, with lastB, final).
func inOrder(t *testing.T, where, text, a, b string, lastB bool) {
	t.Helper()
	ia, ib := indexOf(t, where, text, a, false), indexOf(t, where, text, b, lastB)
	if ia >= ib {
		t.Errorf("%s: want %q (at %d) before %q (at %d)", where, a, ia, b, ib)
	}
}

func TestPeerTimeoutServerHookIsOnDestroyPeerAtTheReconstructedAddress(t *testing.T) {
	t.Parallel()
	mustContain(t, peerTimeoutHook, source(t, peerTimeoutHook), "nevr_patch_addresses::DESTROY_PEER")
	mustContain(t, "src/runtime/hook/addresses.h", source(t, "src/runtime/hook/addresses.h"), "constexpr uintptr_t DESTROY_PEER = 0xF7BC60;")
}

func TestPeerTimeoutEveryRemovalIsLoggedAtItsReasonsLevel(t *testing.T) {
	t.Parallel()
	hook := source(t, peerTimeoutHook)
	mustContain(t, peerTimeoutHook, hook, "LevelForRemovalReason(removal.reason)")
	mustContain(t, peerTimeoutHook, hook, "FormatPeerRemoved(removal)")
}

func TestPeerTimeoutThePeerRecordIsReadBeforeTheOriginalDestroysIt(t *testing.T) {
	t.Parallel()
	inOrder(t, peerTimeoutHook, source(t, peerTimeoutHook), "FormatPeerRemoved(removal)", "OriginalDestroyPeer(self, index, reason)", true)
}

func TestPeerTimeoutEveryArgumentIsForwarded(t *testing.T) {
	t.Parallel()
	mustContain(t, peerTimeoutHook, source(t, peerTimeoutHook), "OriginalDestroyPeer(self, index, reason);")
}

func TestPeerTimeoutTheDestroyPeerPrologueIsCheckedBeforeAttach(t *testing.T) {
	t.Parallel()
	inOrder(t, peerTimeoutHook, source(t, peerTimeoutHook), `PrologueMatches("DestroyPeer"`, "PatchDetour(&OriginalDestroyPeer", false)
}

func TestPeerTimeoutSendIsRecordedBeforeTheOriginalRuns(t *testing.T) {
	t.Parallel()
	inOrder(t, peerTimeoutHook, source(t, peerTimeoutHook), "RecordSend(NowMicros())", "return OriginalBroadcasterSend(", false)
}

func TestPeerTimeoutTheBroadcasterSendPrologueIsCheckedBeforeAttach(t *testing.T) {
	t.Parallel()
	inOrder(t, peerTimeoutHook, source(t, peerTimeoutHook), `PrologueMatches("BroadcasterSend"`, "PatchDetour(&OriginalBroadcasterSend", false)
}

var inGameSendStateAtInfo = regexp.MustCompile(`Log\(EchoVR::LogLevel::Info,\s*"%s",\s*nevr_peer_timeout_log::FormatInGameSendState`)
var noSendWarningAtWarning = regexp.MustCompile(`Log\(EchoVR::LogLevel::Warning,\s*"%s",\s*line\.c_str\(\)\)`)

func TestPeerTimeoutEnteringInGameLogsTheSendStateAtInfo(t *testing.T) {
	t.Parallel()
	hook := source(t, peerTimeoutHook)
	mustContain(t, peerTimeoutHook, hook, "EnterInGame(NowMicros())")
	if !inGameSendStateAtInfo.MatchString(hook) {
		t.Errorf("%s does not match %s", peerTimeoutHook, inGameSendStateAtInfo)
	}
}

func TestPeerTimeoutTheNoSendWarningIsAtWarning(t *testing.T) {
	t.Parallel()
	if !noSendWarningAtWarning.MatchString(source(t, peerTimeoutHook)) {
		t.Errorf("%s does not match %s", peerTimeoutHook, noSendWarningAtWarning)
	}
}

func TestPeerTimeoutBootInstallsTheHookForTheRunRoleOutsideTheServerBranch(t *testing.T) {
	t.Parallel()
	const path = "src/runtime/lifecycle/boot.cpp"
	boot := source(t, path)
	const call = "nevr_peer_timeout_hook::Install(reinterpret_cast<std::uintptr_t>(EchoVR::g_GameBaseAddress), g_isServer);"
	if n := strings.Count(boot, call); n != 1 {
		t.Fatalf("%s calls %q %d times, want once", path, call, n)
	}
	inOrder(t, path, boot, "nevr_coop_ai_trace::Install", call, false)
}

func TestPeerTimeoutTheNetGameStateHookReportsInGame(t *testing.T) {
	t.Parallel()
	const path = "src/runtime/lifecycle/state_machine.cpp"
	mustContain(t, path, source(t, path), "nevr_peer_timeout_hook::OnNetGameState(state == EchoVR::NetGameState::InGame);")
}

func TestPeerTimeoutTheFrameTickChecksTheNoSendThreshold(t *testing.T) {
	t.Parallel()
	const path = "src/runtime/frame/tick.cpp"
	mustContain(t, path, source(t, path), "nevr_peer_timeout_hook::CheckNoSend();")
}

func TestPeerTimeoutTheSourcesAreInTheRuntimeLibrary(t *testing.T) {
	t.Parallel()
	const path = "src/runtime/CMakeLists.txt"
	cmake := source(t, path)
	mustContain(t, path, cmake, `"patch/peer_timeout_log_hook.cpp"`)
	mustContain(t, path, cmake, `"compat/peer_timeout_log.cpp"`)
}
