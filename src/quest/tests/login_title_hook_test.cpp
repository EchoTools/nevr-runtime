// Host test for the login-failed page's title hook (#1137): the real handler (src/quest/sentinel/login_title_hook.cpp)
// and prompt board (src/quest/auth/prompt_board.cpp), built without exceptions as the sentinel builds them, driven through
// the thunk's entry with a stand-in for the script library's string_t::string_t(char const*), which copies at most 63
// characters into a 64-byte buffer exactly as the disassembly 0x10df98 does (NRadString::Copy(this, text, 0x40)). No GOT slot is
// touched: got_pinned_test.cpp resolves the pinned slot in the real library, and tests/quest TestHookFramesCarryNoPersonality
// checks the installed form's frames.
//
// What the page shows is the string the original built, so every case reads the stand-in's buffer.
//
// Built and run by `just test-quest-hooks`.

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "got_hook.h"
#include "hook_log.h"
#include "hook_report.h"
#include "login_title_hook.h"
#include "quest/auth/prompt_board.h"
#include "quest/tests/test_check.h"

namespace {

namespace board = nevr::quest_auth::prompt_board;
namespace lt = nevr_quest::login_title;
using sentinel::pinned::StringTOpaque;

// The game's string_t: a 64-byte inline buffer.
struct FakeString {
  char bytes[64];
};
int g_originalCalls = 0;

void RealStringCtor(StringTOpaque* self, const char* text) {
  ++g_originalCalls;
  FakeString* const s = reinterpret_cast<FakeString*>(self);
  std::memset(s->bytes, 0, sizeof(s->bytes));
  if (text != nullptr) std::strncpy(s->bytes, text, sizeof(s->bytes) - 1);
}

lt::StringThunk::Fn Entry() { return reinterpret_cast<lt::StringThunk::Fn>(lt::StringThunk::EntryAddress()); }

// The node: builds the page's title through the slot.
std::string BuildTitle(const char* literal = "LOGIN FAILED") {
  FakeString s;
  std::memset(s.bytes, 0xAA, sizeof(s.bytes));
  Entry()(reinterpret_cast<StringTOpaque*>(&s), literal);
  return std::string(s.bytes, strnlen(s.bytes, sizeof(s.bytes)));
}

void Reset() {
  board::Withdraw();
  g_originalCalls = 0;
  lt::ResetForTest();
  *lt::StringThunk::OriginalOut() = reinterpret_cast<void*>(&RealStringCtor);
}

// What token auth publishes (prompt_presenters.cpp), written out so this test needs no JSON library.
void PublishCode() {
  const char text[] = "Sign in to play: on a phone or computer, open\necho.example/device\nand enter the code J4K9\nEach code lasts 5 minutes.";
  QCHECK(board::Publish(text, sizeof(text) - 1, board::Mode::kPrompt, "SIGN IN"));
}
void PublishSignedIn() {
  const char text[] = "Signed in to EchoVRCE.\nSelect RETRY to finish.";
  QCHECK(board::Publish(text, sizeof(text) - 1, board::Mode::kNotice, "SIGNED IN"));
}
void PublishTimedOut() {
  const char text[] = "Sign-in timed out.\nRestart the game to try again.";
  QCHECK(board::Publish(text, sizeof(text) - 1, board::Mode::kPrompt));
}

std::vector<std::string> g_lines;
void CaptureSink(sentinel::LogLevel, const char* line) { g_lines.emplace_back(line); }
int CountLines(const char* needle) {
  int n = 0;
  for (const std::string& l : g_lines) n += l.find(needle) != std::string::npos ? 1 : 0;
  return n;
}

// Without the hook armed the game's title is untouched even with a sign-in title on the board: the thunk passes through.
void AnUnarmedThunkLeavesTheGamesTitle() {
  Reset();
  PublishCode();
  QCHECK(BuildTitle() == "LOGIN FAILED");
  QCHECK(g_originalCalls == 1);
}

// The first sign-in: a code is on the board with its title, and the page is titled SIGN IN.
void TheCodeScreenIsTitledSignIn() {
  Reset();
  lt::ArmForTest();
  PublishCode();
  QCHECK(BuildTitle() == "SIGN IN");
  const lt::Counts c = lt::CurrentCounts();
  QCHECK(c.calls == 1 && c.replaced == 1 && c.kept == 0 && c.other_text == 0 && c.faults == 0);
}

// The signed-in notice carries its own title.
void TheSignedInNoticeIsTitledSignedIn() {
  Reset();
  lt::ArmForTest();
  PublishSignedIn();
  QCHECK(BuildTitle() == "SIGNED IN");
}

// A real failure keeps the game's title: nothing published (the server is unreachable before any code, a code the
// service rejected), and the timed-out text, which publishes text but no title.
void ARealFailureKeepsTheGamesTitle() {
  Reset();
  lt::ArmForTest();
  QCHECK(BuildTitle() == "LOGIN FAILED");  // nothing on the board
  PublishTimedOut();
  QCHECK(BuildTitle() == "LOGIN FAILED");  // text with no title
  board::Withdraw();
  QCHECK(BuildTitle() == "LOGIN FAILED");  // withdrawn
  const lt::Counts c = lt::CurrentCounts();
  QCHECK(c.calls == 3 && c.replaced == 0 && c.kept == 3 && c.faults == 0);
}

// A title does not outlive the publish it came with: a code (titled) replaced by the timed-out text (untitled) leaves the
// game's title, and a code published again is titled again.
void ATitleFollowsTheLatestPublish() {
  Reset();
  lt::ArmForTest();
  PublishCode();
  QCHECK(BuildTitle() == "SIGN IN");
  PublishTimedOut();
  QCHECK(BuildTitle() == "LOGIN FAILED");
  PublishCode();
  QCHECK(BuildTitle() == "SIGN IN");
}

// Only the page's literal is replaced. Any other string, the null string and an empty one go to the original untouched,
// whatever the board carries (the slot has no build ID to prove the library, so the literal is the identity check).
void OnlyTheLiteralIsReplaced() {
  Reset();
  lt::ArmForTest();
  PublishCode();
  QCHECK(BuildTitle("login failed") == "login failed");
  QCHECK(BuildTitle("LOGIN FAILED ") == "LOGIN FAILED ");
  QCHECK(BuildTitle("LOGIN FAIL") == "LOGIN FAIL");
  QCHECK(BuildTitle("") == "");
  QCHECK(BuildTitle("SIGN IN") == "SIGN IN");
  FakeString s;
  std::memset(s.bytes, 0xAA, sizeof(s.bytes));
  Entry()(reinterpret_cast<StringTOpaque*>(&s), nullptr);
  QCHECK(s.bytes[0] == '\0');
  const lt::Counts c = lt::CurrentCounts();
  QCHECK(c.calls == 6 && c.replaced == 0 && c.kept == 0 && c.other_text == 6);
}

// The original copies what it is given, so the title is gone from the stack before the page uses it: the built string
// still reads right after the buffer the hook used has been overwritten.
void TheBuiltStringDoesNotPointIntoTheHook() {
  Reset();
  lt::ArmForTest();
  PublishCode();
  FakeString s;
  Entry()(reinterpret_cast<StringTOpaque*>(&s), "LOGIN FAILED");
  char scribble[256];
  std::memset(scribble, 'x', sizeof(scribble));
  (void)scribble;
  QCHECK(std::string(s.bytes) == "SIGN IN");
}

// A title that fits the board fits the game's 64-byte string with its NUL: the longest one is built whole.
void TheLongestTitleFitsTheGamesString() {
  Reset();
  lt::ArmForTest();
  const std::string longest(board::kMaxTitleChars, 'T');
  const char text[] = "x";
  QCHECK(board::Publish(text, 1, board::Mode::kPrompt, longest.c_str()));
  QCHECK(BuildTitle() == longest);
}

// A thunk with no original is a counted fault, not a crash, and builds nothing.
void NoOriginalIsAFault() {
  Reset();
  lt::ArmForTest();
  *lt::StringThunk::OriginalOut() = nullptr;
  PublishCode();
  FakeString s;
  std::memset(s.bytes, 0xAA, sizeof(s.bytes));
  Entry()(reinterpret_cast<StringTOpaque*>(&s), "LOGIN FAILED");
  QCHECK(lt::CurrentCounts().faults == 1);
  QCHECK(g_originalCalls == 0);
}

// The install: the library is not mapped on the host, so it answers kModuleNotLoaded without a line (the caller asks again
// after each dlopen, and a line per dlopen would be a log of every library the game maps). Without registered counters it
// installs nothing and says so once.
void InstallWaitsForTheLibraryAndNeedsItsCounters() {
  Reset();
  g_lines.clear();
  const sentinel::LogSink previous = sentinel::SetLogSink(&CaptureSink);
  // Counters not registered yet: refused, loudly.
  QCHECK_STATUS(lt::Install(), sentinel::GotStatus::kNotInstalled);
  QCHECK(CountLines("\"event\":\"login_title_install\"") == 1);
  QCHECK(CountLines("\"why\":\"counters_refused\"") == 1);
  sentinel::StopReporter();
  QCHECK(lt::RegisterCounters());
  g_lines.clear();
  for (int i = 0; i < 3; ++i) QCHECK_STATUS(lt::Install(), sentinel::GotStatus::kModuleNotLoaded);
  QCHECK(g_lines.empty());
  sentinel::SetLogSink(previous);
  sentinel::StopReporter();
}

// A refused counter registration is loud, and the install then refuses (the hook never runs unreported).
void CounterRefusalIsLoudAndBlocksTheInstall() {
  Reset();
  sentinel::StopReporter();
  constexpr unsigned kFill = sentinel::kMaxReportCounters - 2;
  static std::atomic<std::uint64_t> filler[kFill];
  unsigned taken = 0;
  for (unsigned i = 0; i < kFill; ++i) taken += sentinel::RegisterReportCounter("filler", &filler[i]) ? 1 : 0;
  QCHECK(taken == kFill);
  g_lines.clear();
  const sentinel::LogSink previous = sentinel::SetLogSink(&CaptureSink);
  QCHECK(!lt::RegisterCounters());
  QCHECK(CountLines("\"event\":\"login_title_counters\"") == 1);
  QCHECK(CountLines("\"result\":\"refused\"") == 1);
  QCHECK_STATUS(lt::Install(), sentinel::GotStatus::kNotInstalled);
  sentinel::SetLogSink(previous);
  sentinel::StopReporter();
}

// The reporter holds the five counters under the names the log reads (a renamed counter is a lost one).
void TheCountersAreRegisteredUnderTheirNames() {
  Reset();
  sentinel::StopReporter();
  QCHECK(lt::RegisterCounters());
  QCHECK(lt::kCounterCount == 5);
  int extra = 0;
  static std::atomic<std::uint64_t> spare[sentinel::kMaxReportCounters];
  while (extra < static_cast<int>(sentinel::kMaxReportCounters) &&
         sentinel::RegisterReportCounter("spare", &spare[extra])) {
    ++extra;
  }
  QCHECK(extra == static_cast<int>(sentinel::kMaxReportCounters) - lt::kCounterCount);
  sentinel::StopReporter();
}

// The title read is never torn against a writer that alternates the two titles (an x86 host is TSO and shows torn reads,
// not a weakened order; the fences are in prompt_board.cpp).
void ATitleReadIsNeverTorn() {
  Reset();
  lt::ArmForTest();
  std::atomic<bool> stop{false};
  std::thread writer([&stop] {
    const char text[] = "x";
    bool flip = false;
    while (!stop.load(std::memory_order_relaxed)) {
      board::Publish(text, 1, board::Mode::kPrompt, flip ? "SIGN IN" : "SIGNED IN");
      flip = !flip;
    }
  });
  int torn = 0;
  for (int i = 0; i < 20000; ++i) {
    const std::string t = BuildTitle();
    if (t != "SIGN IN" && t != "SIGNED IN" && t != "LOGIN FAILED") ++torn;
  }
  stop.store(true);
  writer.join();
  QCHECK(torn == 0);
}

}  // namespace

int main() {
  AnUnarmedThunkLeavesTheGamesTitle();
  TheCodeScreenIsTitledSignIn();
  TheSignedInNoticeIsTitledSignedIn();
  ARealFailureKeepsTheGamesTitle();
  ATitleFollowsTheLatestPublish();
  OnlyTheLiteralIsReplaced();
  TheBuiltStringDoesNotPointIntoTheHook();
  TheLongestTitleFitsTheGamesString();
  NoOriginalIsAFault();
  InstallWaitsForTheLibraryAndNeedsItsCounters();
  CounterRefusalIsLoudAndBlocksTheInstall();
  TheCountersAreRegisteredUnderTheirNames();
  ATitleReadIsNeverTorn();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "login_title_hook_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("login_title_hook_test: all checks passed\n");
  return 0;
}
