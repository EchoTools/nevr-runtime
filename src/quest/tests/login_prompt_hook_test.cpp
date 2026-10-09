// Host test for the login-prompt hooks (#239): the real handlers (src/quest/sentinel/login_prompt_hook.cpp)
// and prompt board (src/quest/auth/prompt_board.cpp), built without exceptions as the sentinel builds
// them, driven through the thunks' entries with stand-ins for CR15NetGame::SetDelimitedErrorMessage and
// CR15NetGame::Update on a fake CR15NetGame laid out as game_layout says (pinned_targets.h). No GOT slot
// is touched: tests/quest TestHookFramesCarryNoPersonality and src/quest/tests/got_pinned_test.cpp cover
// the installed form.
//
// The concurrency check runs on an x86 host, which is TSO: it catches torn reads, not a weakened memory
// order. The arm64 order rests on the fences in prompt_board.cpp.
//
// Built and run by `just test-quest-hooks`.

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "hook_log.h"
#include "hook_report.h"
#include "login_prompt_hook.h"
#include "quest/auth/prompt_board.h"
#include "quest/login/login_attempt_gate.h"
#include "quest/game_login_failures.h"
#include "quest/tests/test_check.h"

namespace {

namespace board = nevr::quest_auth::prompt_board;
namespace lp = nevr_quest::login_prompt;
namespace layout = sentinel::pinned::game_layout;
namespace gate = QuestLogin::attempt_gate;
using sentinel::pinned::CR15NetGameOpaque;

// A CR15NetGame stand-in: large enough for the error block, state at offset 0.
struct alignas(16) FakeGame {
  unsigned char bytes[layout::kErrorBlockOffset + layout::kErrorBlockBytes + 64];
};
FakeGame g_game;
FakeGame g_other;

CR15NetGameOpaque* Obj(FakeGame& g) { return reinterpret_cast<CR15NetGameOpaque*>(&g); }
void SetState(FakeGame& g, std::int32_t s) { std::memcpy(g.bytes + layout::kStateOffset, &s, sizeof(s)); }
unsigned char* BlockOf(FakeGame& g) { return g.bytes + layout::kErrorBlockOffset; }
std::string Line(FakeGame& g, int n) {
  const char* p = reinterpret_cast<const char*>(BlockOf(g) + 1 + n * layout::kErrorLineBytes);
  return std::string(p, strnlen(p, layout::kErrorLineBytes));
}

std::string g_gameReceived;  // what the game's own function was handed (and logs)
int g_errorCalls = 0;
bool g_errorWrites = true;  // false: a game build whose block is elsewhere

// Stands in for SetDelimitedErrorMessage with a one-line message: SetErrorMessage(char const*)
// writes 0 before the lines, the message cut at 63, NUL-filled.
void FakeSetDelimitedErrorMessage(CR15NetGameOpaque* self, const char* message) {
  g_gameReceived = message;
  ++g_errorCalls;
  if (!g_errorWrites) return;
  unsigned char* block = reinterpret_cast<unsigned char*>(self) + layout::kErrorBlockOffset;
  block[0] = 0;
  std::size_t i = 0;
  for (; i + 1 < layout::kErrorLineBytes && message[i] != '\0'; ++i) block[1 + i] = static_cast<unsigned char>(message[i]);
  for (; i < layout::kErrorLineBytes; ++i) block[1 + i] = 0;
}

int g_updateCalls = 0;
void FakeUpdate(CR15NetGameOpaque*, std::uint64_t arg) {
  QCHECK(arg == 16);
  ++g_updateCalls;
}

lp::ErrorThunk::Fn ErrorEntry() { return reinterpret_cast<lp::ErrorThunk::Fn>(lp::ErrorThunk::EntryAddress()); }
lp::UpdateThunk::Fn UpdateEntry() { return reinterpret_cast<lp::UpdateThunk::Fn>(lp::UpdateThunk::EntryAddress()); }

constexpr const char* kLocal = nevr_quest::game_login_failures::kPrerequisitesMissing;
constexpr char kServerBan[] = "[XPID:OVR-ORG-1 / Discord:1]\nAccount disabled by EchoVRCE Admins";
std::string Prompt(const char* code) {
  return std::string("Sign in to play: on a phone or computer, open\necho.test/login/device\nand enter the code ") +
         code + "\nEach code lasts 5 minutes, then a new one is issued.";
}
void Publish(const std::string& text, board::Mode mode = board::Mode::kPrompt) {
  QCHECK(board::Publish(text.data(), text.size(), mode));
}

std::vector<std::string> g_lines;
void CaptureSink(sentinel::LogLevel, const char* line) { g_lines.emplace_back(line); }
int CountLines(const char* needle) {
  int n = 0;
  for (const std::string& l : g_lines) n += l.find(needle) != std::string::npos ? 1 : 0;
  return n;
}

void TheGameKnowsWhichFailuresAreItsOwn() {
  QCHECK(lp::IsLocalLoginFailure(kLocal));
  QCHECK(lp::IsLocalLoginFailure("Log in request failed: Failed to get user proof"));
  QCHECK(lp::IsLocalLoginFailure("Log in request failed: Service unavailable"));
  QCHECK(!lp::IsLocalLoginFailure(kServerBan));
  QCHECK(!lp::IsLocalLoginFailure("Log in request failed: One or more prerequisites are missing!"));
  QCHECK(!lp::IsLocalLoginFailure("Log in request failed"));
  QCHECK(!lp::IsLocalLoginFailure(""));
  QCHECK(!lp::IsLocalLoginFailure(nullptr));
}

// Puts a fresh instance into "its login just failed with a local text" with the board as it is now.
void FailLocally(FakeGame& g) {
  SetState(g, layout::kStateLoggingIn);
  ErrorEntry()(Obj(g), kLocal);
  SetState(g, layout::kStateLoginFailed);  // what LogInFailedCB's SwitchTo(-0x5e) does next
}

constexpr char kNotice[] = "Signed in to EchoVRCE.\nSelect RETRY to finish.";

void ALocalFailureWithNothingPublishedKeepsTheGameTextAndAPromptPublishedLaterReachesIt() {
  board::Withdraw();
  const lp::Counts before = lp::CurrentCounts();
  FailLocally(g_game);
  QCHECK(g_gameReceived == kLocal);
  QCHECK(Line(g_game, 0) == kLocal);
  QCHECK(BlockOf(g_game)[0] == 0);
  QCHECK(lp::CurrentCounts().kept == before.kept + 1);
  // Token auth publishes after the failure (a slow refresh, a code request that took a while).
  Publish(Prompt("LATE1-CODE"));
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(Line(g_game, 2) == "and enter the code LATE1-CODE");
  QCHECK(BlockOf(g_game)[0] == 1);
  QCHECK(lp::CurrentCounts().refreshed == before.refreshed + 1);
  board::Withdraw();  // the game's own message comes back
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(BlockOf(g_game)[0] == 0 && Line(g_game, 0) == kLocal);
  QCHECK(lp::CurrentCounts().refreshed == before.refreshed + 2);
}

void ALocalFailureWhileLoggingInShowsThePromptAndTheGameNeverSeesTheCode() {
  Publish(Prompt("CODE1-ABCD"));
  const lp::Counts before = lp::CurrentCounts();
  FailLocally(g_game);
  QCHECK(g_gameReceived == kLocal);  // the game stored and logged its own message, not the prompt
  QCHECK(BlockOf(g_game)[0] == 1);
  QCHECK(Line(g_game, 0) == "Sign in to play: on a phone or computer, open");
  QCHECK(Line(g_game, 1) == "echo.test/login/device");
  QCHECK(Line(g_game, 2) == "and enter the code CODE1-ABCD");
  QCHECK(Line(g_game, 3) == "Each code lasts 5 minutes, then a new one is issued.");
  QCHECK(lp::CurrentCounts().shown == before.shown + 1);
}

void TheScreenFollowsTheBoardWhileTheGameStaysInLoginFailed() {
  // Continues from the previous case: the prompt for CODE1 is on screen, state -94.
  g_updateCalls = 0;
  const lp::Counts before = lp::CurrentCounts();
  UpdateEntry()(Obj(g_game), 16);  // nothing changed
  QCHECK(g_updateCalls == 1);
  QCHECK(lp::CurrentCounts().refreshed == before.refreshed);
  Publish(Prompt("CODE2-WXYZ"));  // the code ran out and a new one was issued
  // Another instance, also in "login failed" and with a block identical to the followed one, is not
  // the followed instance: it is left alone (the `self == followed` guard, not the state or block
  // checks, is what stops this).
  std::memcpy(g_other.bytes, g_game.bytes, sizeof(g_other.bytes));
  UpdateEntry()(Obj(g_other), 16);
  QCHECK(Line(g_other, 2) == "and enter the code CODE1-ABCD");
  QCHECK(lp::CurrentCounts().refreshed == before.refreshed);
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(Line(g_game, 2) == "and enter the code CODE2-WXYZ");
  QCHECK(lp::CurrentCounts().refreshed == before.refreshed + 1);
  // While the other hook holds the writer flag nothing is written; the next frame writes.
  Publish(kNotice, board::Mode::kNotice);
  QCHECK(lp::HoldBlockWriterForTest());
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(Line(g_game, 2) == "and enter the code CODE2-WXYZ");
  lp::ReleaseBlockWriterForTest();
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(Line(g_game, 0) == "Signed in to EchoVRCE.");
  QCHECK(Line(g_game, 1) == "Select RETRY to finish.");
  QCHECK(Line(g_game, 2).empty() && Line(g_game, 3).empty());
  board::Withdraw();  // stopped: the game's own message comes back
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(BlockOf(g_game)[0] == 0);
  QCHECK(Line(g_game, 0) == kLocal);
  QCHECK(lp::CurrentCounts().refreshed == before.refreshed + 3);
  QCHECK(g_updateCalls == 6);  // the game's Update ran every time, for both instances
  // The game leaves "login failed": the instance is no longer followed.
  SetState(g_game, 0);
  UpdateEntry()(Obj(g_game), 16);
  Publish(Prompt("CODE3-QQQQ"));
  SetState(g_game, layout::kStateLoginFailed);
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(Line(g_game, 0) == kLocal);
  QCHECK(lp::CurrentCounts().refreshed == before.refreshed + 3);
  board::Withdraw();
}

void ABlockAnotherWriterChangedIsLeftAlone() {
  Publish(Prompt("CODE7-UUUU"));
  FailLocally(g_game);
  QCHECK(Line(g_game, 2) == "and enter the code CODE7-UUUU");
  // The game shows a different error with the same block (a lobby or game-space error).
  FakeSetDelimitedErrorMessage(Obj(g_game), "Service is unavailable");
  const lp::Counts before = lp::CurrentCounts();
  Publish(Prompt("CODE8-VVVV"));
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(Line(g_game, 0) == "Service is unavailable");  // not overwritten
  QCHECK(lp::CurrentCounts().not_ours == before.not_ours + 1);
  board::Withdraw();  // and not "restored" over the other writer's text either
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(Line(g_game, 0) == "Service is unavailable");
  QCHECK(lp::CurrentCounts().refreshed == before.refreshed);
}

// The player signed in while a login attempt was in flight and the attempt fails with the game's local
// text: the notice is what the screen shows (RETRY on it now logs in), and the game's text comes back when
// the board is withdrawn.
void ANoticePublishedBeforeAFailureIsShownForIt() {
  lp::ResetLatchForTest();
  Publish(kNotice, board::Mode::kNotice);
  const lp::Counts before = lp::CurrentCounts();
  FailLocally(g_game);
  QCHECK(g_gameReceived == kLocal);
  QCHECK(BlockOf(g_game)[0] == 1);
  QCHECK(Line(g_game, 0) == "Signed in to EchoVRCE.");
  QCHECK(Line(g_game, 1) == "Select RETRY to finish.");
  QCHECK(lp::CurrentCounts().shown == before.shown + 1);
  QCHECK(lp::CurrentCounts().kept == before.kept);
  QCHECK(lp::LatchArmedForTest());  // the notice is protected from an error page too
  board::Withdraw();
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(BlockOf(g_game)[0] == 0 && Line(g_game, 0) == kLocal);
  lp::ResetLatchForTest();
}

void ServerMessagesAndLoggedInRemovalsAreNeverReplaced() {
  Publish(Prompt("CODE4-RRRR"));
  const lp::Counts before = lp::CurrentCounts();
  // A server-sent failure (LogInFailedCB passes it through; a ban or suspension text, say).
  SetState(g_game, layout::kStateLoggingIn);
  ErrorEntry()(Obj(g_game), "Account disabled by EchoVRCE Admins");
  QCHECK(Line(g_game, 0) == "Account disabled by EchoVRCE Admins");
  // LoginRemovedCB runs only when logged in (state >= 3), e.g. reason "banned".
  SetState(g_game, 3);
  ErrorEntry()(Obj(g_game), kLocal);
  QCHECK(Line(g_game, 0) == kLocal);
  QCHECK(lp::CurrentCounts().not_local == before.not_local + 2);
  QCHECK(lp::CurrentCounts().shown == before.shown);
  board::Withdraw();
}

void ABlockThatDoesNotHoldTheGameMessageIsNotWritten() {
  Publish(Prompt("CODE5-SSSS"));
  SetState(g_game, layout::kStateLoggingIn);
  std::memset(BlockOf(g_game), 0x5a, layout::kErrorBlockBytes);
  g_errorWrites = false;  // a build whose block is not where game_layout says
  const lp::Counts before = lp::CurrentCounts();
  ErrorEntry()(Obj(g_game), kLocal);
  g_errorWrites = true;
  QCHECK(BlockOf(g_game)[0] == 0x5a && BlockOf(g_game)[1] == 0x5a);  // untouched
  QCHECK(lp::CurrentCounts().not_ours == before.not_ours + 1);
  board::Withdraw();
}

void ABusyBoardIsCountedAndThePromptFollowsOnTheNextFrame() {
  Publish(Prompt("CODE6-TTTT"));
  const lp::Counts before = lp::CurrentCounts();
  board::BeginWriteForTest();
  FailLocally(g_game);
  board::EndWriteForTest();
  QCHECK(Line(g_game, 0) == kLocal);
  QCHECK(lp::CurrentCounts().busy == before.busy + 1);
  QCHECK(lp::CurrentCounts().kept == before.kept);  // busy is not "nothing published"
  UpdateEntry()(Obj(g_game), 16);  // the board is free again: the prompt goes up
  QCHECK(Line(g_game, 2) == "and enter the code CODE6-TTTT");
  board::Withdraw();
  UpdateEntry()(Obj(g_game), 16);
}

// A notice published after a failure replaces only a prompt or a notice: on a screen that shows the game's own
// text it is not applied (a, the board was busy at the failure); one up at the failure is shown (b).
void ANoticeDoesNotReplaceTheGameTextOfAScreenThatShowedNoPrompt() {
  // (a) Busy board at the failure, a notice published: the next frame applies nothing.
  Publish(kNotice, board::Mode::kNotice);
  board::BeginWriteForTest();
  FailLocally(g_game);
  board::EndWriteForTest();
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(BlockOf(g_game)[0] == 0 && Line(g_game, 0) == kLocal);
  // (b) A notice up when the attempt fails is shown for it, and a second notice replaces the first.
  board::Withdraw();
  Publish(kNotice, board::Mode::kNotice);
  FailLocally(g_game);
  QCHECK(Line(g_game, 0) == "Signed in to EchoVRCE.");
  Publish("Signed in to EchoVRCE.\nAgain.", board::Mode::kNotice);
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(Line(g_game, 1) == "Again.");
  // A prompt goes up there too, and a notice may then replace it.
  Publish(Prompt("NOTE1-CODE"));
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(Line(g_game, 2) == "and enter the code NOTE1-CODE");
  Publish(kNotice, board::Mode::kNotice);
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(Line(g_game, 0) == "Signed in to EchoVRCE.");
  board::Withdraw();
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(Line(g_game, 0) == kLocal);
}

// A new local failure that the error hook could not take up (its writer flag was held) is the game's
// own text in a followed block: it is taken up from there, not dropped as another writer's.
void ANewLocalFailureMissedByTheErrorHookIsTakenUpByUpdate() {
  Publish(Prompt("MISS1-CODE"));
  FailLocally(g_game);  // followed, prompt on screen
  QCHECK(Line(g_game, 2) == "and enter the code MISS1-CODE");
  const lp::Counts before = lp::CurrentCounts();
  QCHECK(lp::HoldBlockWriterForTest());
  FailLocally(g_game);  // the game writes its message again; the hook cannot take it up now
  lp::ReleaseBlockWriterForTest();
  QCHECK(BlockOf(g_game)[0] == 0 && Line(g_game, 0) == kLocal);
  QCHECK(lp::CurrentCounts().busy == before.busy + 1);
  UpdateEntry()(Obj(g_game), 16);  // the board did not change; the block did
  QCHECK(BlockOf(g_game)[0] == 1);
  QCHECK(Line(g_game, 0) == "Sign in to play: on a phone or computer, open");
  QCHECK(Line(g_game, 2) == "and enter the code MISS1-CODE");
  QCHECK(lp::CurrentCounts().not_ours == before.not_ours);
  board::Withdraw();
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(Line(g_game, 0) == kLocal);
}

// A second local failure while the game is already in "login failed" (LogInFailedCB acts in any
// state >= 0) is not taken up by the error hook, which only acts while the game is logging in; Update
// finds the game's local text in the followed block and puts the prompt over it. Intended: the screen
// shows a local login failure while the player still has to sign in.
void ASecondLocalFailureWhileAlreadyInLoginFailedGetsThePromptToo() {
  Publish(Prompt("SEC1-CODE"));
  FailLocally(g_game);  // prompt shown, state -94
  const lp::Counts before = lp::CurrentCounts();
  ErrorEntry()(Obj(g_game), kLocal);  // the game is in -94, not logging in
  QCHECK(BlockOf(g_game)[0] == 0 && Line(g_game, 0) == kLocal);
  QCHECK(lp::CurrentCounts().not_local == before.not_local + 1);
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(Line(g_game, 0) == "Sign in to play: on a phone or computer, open");
  QCHECK(Line(g_game, 2) == "and enter the code SEC1-CODE");
  QCHECK(lp::CurrentCounts().not_ours == before.not_ours);
  board::Withdraw();
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(BlockOf(g_game)[0] == 0 && Line(g_game, 0) == kLocal);
}

// Not tested here: Refresh's re-check that the followed instance is still `self` after it takes the
// writer flag (login_prompt_hook.cpp, Refresh). Update only calls Refresh for the followed instance,
// and only another thread could change it between that check and the flag; both hooks run on the game
// loop's thread (docs/adr/0003), so a test would need a second thread racing the hook.

void AWithdrawnBoardKeepsNoCode() {
  Publish(Prompt("WIPE1-CODE"));
  QCHECK(board::NonZeroTextBytesForTest() == Prompt("WIPE1-CODE").size());
  Publish("short", board::Mode::kNotice);  // nothing of the longer text stays behind it
  QCHECK(board::NonZeroTextBytesForTest() == 5);
  board::Withdraw();
  QCHECK(board::NonZeroTextBytesForTest() == 0);
}

void TheBoardRefusesWhatTheGameCouldNotShow() {
  board::Withdraw();
  const std::uint64_t v = board::Version();
  const std::string tooLong(board::kCapacity + 1, 'x');
  QCHECK(!board::Publish(tooLong.data(), tooLong.size(), board::Mode::kPrompt));
  QCHECK(!board::Publish(nullptr, 3, board::Mode::kPrompt));
  QCHECK(!board::Publish("abc", 0, board::Mode::kPrompt));
  const char withNul[] = {'a', '\0', 'b'};
  QCHECK(!board::Publish(withNul, sizeof(withNul), board::Mode::kPrompt));
  QCHECK(board::Version() == v);  // a refused publish leaves the board as it was
  const std::string full(board::kCapacity, 'y');
  QCHECK(board::Publish(full.data(), full.size(), board::Mode::kNotice));
  char out[board::kCapacity + 1];
  board::Mode mode = board::Mode::kPrompt;
  std::uint64_t version = 0;
  QCHECK(board::Read(out, sizeof(out), &mode, &version) == board::ReadResult::kCopied);
  QCHECK(std::string(out) == full && mode == board::Mode::kNotice && version == board::Version());
  char small[16];
  QCHECK(board::Read(small, sizeof(small), nullptr, nullptr) == board::ReadResult::kEmpty);
  QCHECK(small[0] == '\0');
  board::Withdraw();
  QCHECK(board::Read(out, sizeof(out), nullptr, nullptr) == board::ReadResult::kEmpty);
  QCHECK(out[0] == '\0');
}

// A reader racing a writer never sees a mix of two texts: every copy it gets is one of them whole.
// Two texts of the same length that differ in every byte make a torn read visible.
void ConcurrentReadsAreNeverTorn() {
  const std::string a(board::kCapacity, 'a');
  const std::string b(board::kCapacity, 'b');
  QCHECK(board::Publish(a.data(), a.size(), board::Mode::kPrompt));
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    for (int i = 0; i < 200000 && !stop.load(); ++i) {
      const std::string& s = (i % 2 == 0) ? b : a;
      board::Publish(s.data(), s.size(), board::Mode::kPrompt);
    }
    stop.store(true);
  });
  int reads = 0, torn = 0, busy = 0;
  char out[board::kCapacity + 1];
  while (!stop.load()) {
    const board::ReadResult r = board::Read(out, sizeof(out), nullptr, nullptr);
    if (r == board::ReadResult::kBusy) {
      ++busy;
      continue;
    }
    ++reads;
    const std::string got(out);
    if (got != a && got != b) ++torn;
  }
  writer.join();
  std::printf("login_prompt_hook_test: %d whole reads, %d busy, %d torn during concurrent publishes\n", reads, busy, torn);
  QCHECK(torn == 0);
  QCHECK(reads > 0);
  board::Withdraw();
}

void CounterRefusalIsLoudAndInstallReportsBothSlots() {
  // A nearly full reporter table (two slots left, the hook needs fourteen): the counters are refused,
  // RegisterCounters says so in one line. Sized from the reporter's capacity, not a literal.
  sentinel::StopReporter();
  constexpr unsigned kFill = sentinel::kMaxReportCounters - 2;
  static std::atomic<std::uint64_t> filler[kFill];
  unsigned taken = 0;
  for (unsigned i = 0; i < kFill; ++i) taken += sentinel::RegisterReportCounter("filler", &filler[i]) ? 1 : 0;
  QCHECK(taken == kFill);
  g_lines.clear();
  QCHECK(!lp::RegisterCounters());
  QCHECK(CountLines("\"event\":\"login_prompt_counters\"") == 1);
  QCHECK(CountLines("\"result\":\"refused\"") == 1);
  sentinel::StopReporter();  // forgets the table
  g_lines.clear();
  QCHECK(lp::RegisterCounters());
  QCHECK(CountLines("login_prompt_counters") == 0);
  sentinel::StopReporter();
  // No libr15.so on the host: both installs are refused and the one line says so.
  g_lines.clear();
  QCHECK(!lp::Install());
  QCHECK(CountLines("\"event\":\"login_prompt_install\"") == 1);
  QCHECK(CountLines("\"result\":\"failed\"") == 1);
  // Without its counters the hook is not installed: one "skipped" line, no install attempt.
  g_lines.clear();
  QCHECK(!lp::InstallIfCounted(false));
  QCHECK(CountLines("\"result\":\"skipped\"") == 1);
  QCHECK(CountLines("\"why\":\"counters_refused\"") == 1);
  QCHECK(CountLines("got_hook") == 0);
  QCHECK(CountLines("\"result\":\"failed\"") == 0);
  g_lines.clear();
  QCHECK(!lp::InstallIfCounted(true));  // with them, Install runs (and is refused on the host)
  QCHECK(CountLines("\"result\":\"failed\"") == 1);
}

// ---- the error event (D2) -------------------------------------------------------------------------

int g_quitCalls = 0;
CR15NetGameOpaque* g_quitSelf = nullptr;
std::string g_quitSawLine2;  // what the block held when the event was sent
void FakeQuitOnError(CR15NetGameOpaque* self) noexcept {
  ++g_quitCalls;
  g_quitSelf = self;
  g_quitSawLine2 = Line(g_game, 2);
}
std::int64_t g_now = 1'000'000'000;
std::int64_t FakeClock() noexcept { return g_now; }
constexpr std::int64_t kMs = 1'000'000;

// Failure caught (#239 smoke): a code published after the failure was written into the block, but the
// status script copies the block to the screen only on the game's error event, so the player still saw the
// old text. A rewrite in "login failed" is followed by one event, after the new text is in the block, once
// per change, and never twice within a frame.
void ARewriteInLoginFailedSendsOneErrorEvent() {
  board::Withdraw();
  lp::SetClockForTest(&FakeClock);
  lp::SetQuitOnErrorForTest(&FakeQuitOnError);
  g_quitCalls = 0;
  Publish(Prompt("EVT1-CODE"));
  FailLocally(g_game);  // the game sends its own error event for this failure: the hook sends none
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(g_quitCalls == 0);
  const lp::Counts before = lp::CurrentCounts();

  g_now += 100 * kMs;
  Publish(Prompt("EVT2-CODE"));  // the code ran out: a new one
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(g_quitCalls == 1 && g_quitSelf == Obj(g_game));
  QCHECK(g_quitSawLine2 == "and enter the code EVT2-CODE");  // the text was already there
  QCHECK(lp::CurrentCounts().resent == before.resent + 1);
  for (int i = 0; i < 3; ++i) UpdateEntry()(Obj(g_game), 16);  // the rest of the iteration, same instant
  QCHECK(g_quitCalls == 1);

  // A change 10 ms later waits for the spacing, then goes out once.
  g_now += 10 * kMs;
  Publish(Prompt("EVT3-CODE"));
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(g_quitCalls == 1);
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(g_quitCalls == 1);  // still inside the spacing
  g_now += 60 * kMs;
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(g_quitCalls == 2 && g_quitSawLine2 == "and enter the code EVT3-CODE");
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(g_quitCalls == 2);
  g_now += 200 * kMs;  // time passes with no change: at most one event per change
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(g_quitCalls == 2);
  QCHECK(lp::CurrentCounts().resent == before.resent + 2);

  // The notice that replaces the prompt after a sign-in is a change too; withdrawing restores the game's
  // text and is another.
  g_now += 100 * kMs;
  Publish(kNotice, board::Mode::kNotice);
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(g_quitCalls == 3);
  g_now += 100 * kMs;
  board::Withdraw();
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(g_quitCalls == 4 && Line(g_game, 0) == kLocal);
  lp::SetQuitOnErrorForTest(nullptr);
}

// An event is due only while the followed instance is in "login failed": if the game left that state
// before the spacing passed, nothing is sent, and a later failure does not inherit the old request.
void NoErrorEventAfterTheGameLeftLoginFailed() {
  lp::SetQuitOnErrorForTest(&FakeQuitOnError);
  g_quitCalls = 0;
  board::Withdraw();
  Publish(Prompt("EVT4-CODE"));
  FailLocally(g_game);
  g_now += 100 * kMs;
  Publish(Prompt("EVT5-CODE"));
  UpdateEntry()(Obj(g_game), 16);  // a change: the event goes out
  QCHECK(g_quitCalls == 1);
  g_now += 1 * kMs;
  Publish(Prompt("EVT6-CODE"));
  UpdateEntry()(Obj(g_game), 16);  // another change 1 ms later: pending, inside the spacing
  QCHECK(g_quitCalls == 1);
  SetState(g_game, 3);  // the game leaves "login failed" before the spacing passed
  g_now += 100 * kMs;
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(g_quitCalls == 1);
  SetState(g_game, layout::kStateLoginFailed);  // back in it (a later failure): the old request is gone
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(g_quitCalls == 1);
  board::Withdraw();
  lp::SetQuitOnErrorForTest(nullptr);
}

// Failure caught: a build where QuitOnError could not be proven crashing the game, or the miss going
// unseen. Nothing is called, the block is still rewritten, and the miss is counted.
void AnUnresolvedErrorEventIsCountedAndNotCalled() {
  lp::SetQuitOnErrorForTest(nullptr);
  board::Withdraw();
  Publish(Prompt("EVT6-CODE"));
  FailLocally(g_game);
  const lp::Counts before = lp::CurrentCounts();
  g_now += 100 * kMs;
  Publish(Prompt("EVT7-CODE"));
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(Line(g_game, 2) == "and enter the code EVT7-CODE");
  QCHECK(lp::CurrentCounts().resend_unavailable == before.resend_unavailable + 1);
  QCHECK(lp::CurrentCounts().resent == before.resent);
  board::Withdraw();
  UpdateEntry()(Obj(g_game), 16);
}

// The event is sent for the followed instance only.
void NoErrorEventForAnInstanceThatIsNotFollowed() {
  lp::SetQuitOnErrorForTest(&FakeQuitOnError);
  g_quitCalls = 0;
  board::Withdraw();
  Publish(Prompt("EVT8-CODE"));
  FailLocally(g_game);
  std::memcpy(g_other.bytes, g_game.bytes, sizeof(g_other.bytes));
  g_now += 100 * kMs;
  Publish(Prompt("EVT9-CODE"));
  UpdateEntry()(Obj(g_other), 16);
  QCHECK(g_quitCalls == 0);
  board::Withdraw();
  UpdateEntry()(Obj(g_game), 16);
  g_quitCalls = 0;
  lp::SetQuitOnErrorForTest(nullptr);
}

// ---- the prompt latch and the page-enable hook (D5, D6') ------------------------------------------

namespace ui = sentinel::pinned::ui_layout;

struct alignas(16) FakePageData {
  unsigned char bytes[0x80];
};
std::atomic<int> g_enableOriginalCalls{0};
void FakeEnableOriginal(void*, const void*) { g_enableOriginalCalls.fetch_add(1); }
lp::EnablePageThunk::Fn EnableEntry() { return reinterpret_cast<lp::EnablePageThunk::Fn>(lp::EnablePageThunk::EntryAddress()); }

// Calls the hooked Enter for a record naming `actor`; true when the game's own Enter ran.
bool Enabled(std::uint64_t actor, std::ptrdiff_t nodeOffset = static_cast<std::ptrdiff_t>(ui::kEnablePageNodeOffset)) {
  FakePageData data;
  std::memset(data.bytes, 0, sizeof(data.bytes));
  std::memcpy(data.bytes + ui::kEnablePageActorIdOffset, &actor, sizeof(actor));
  const int before = g_enableOriginalCalls.load();
  EnableEntry()(data.bytes + nodeOffset, data.bytes);
  return g_enableOriginalCalls.load() == before + 1;
}

constexpr std::uint64_t kSomeOtherPage = 0x1234567890abcdefULL;

// Starts a case with the sign-in prompt on screen after a local failure, latch armed.
void PromptOnScreen(const char* code) {
  lp::ResetLatchForTest();
  board::Withdraw();
  Publish(Prompt(code));
  FailLocally(g_game);
}

// Failure caught (#239 D5): after RETRY a parked UI script replaced the screen that shows the code with the
// error page; the player never saw the code. While the sentinel's text is in the block the error pages are
// skipped; the logging-in page is skipped only while the login may not proceed (the attempt gate); anything
// else goes through.
void WhileThePromptIsOnScreenTheErrorPagesAreNotEnabled() {
  PromptOnScreen("LATCH1-CODE");
  QCHECK(lp::LatchArmedForTest());
  const lp::Counts before = lp::CurrentCounts();
  QCHECK(!Enabled(ui::kErrorDisplayPage));
  QCHECK(!Enabled(ui::kFatalErrorDisplayPage));
  QCHECK(lp::CurrentCounts().error_page_dropped == before.error_page_dropped + 2);
  // The logging-in page: through while the login may proceed, skipped (and the attempt poisoned) while it
  // may not, and through again the instant it may.
  gate::SetReady(true);
  QCHECK(Enabled(ui::kLoggingInPage));
  QCHECK(lp::CurrentCounts().page_passed_armed == before.page_passed_armed + 1);
  QCHECK(gate::LoginMayProceed());
  gate::SetReady(false);
  QCHECK(!Enabled(ui::kLoggingInPage));
  QCHECK(lp::CurrentCounts().logging_in_page_dropped == before.logging_in_page_dropped + 1);
  gate::SetReady(true);
  QCHECK(Enabled(ui::kLoggingInPage));  // the skip stopped with the readiness
  gate::SetReady(true);
  // Any other page is the game's business.
  QCHECK(Enabled(kSomeOtherPage));
  // A record that is not the measured shape (node != data + 0x20) goes through, counted.
  const std::uint64_t passed = lp::CurrentCounts().page_passed_armed;
  QCHECK(Enabled(ui::kErrorDisplayPage, 0x28));
  QCHECK(lp::CurrentCounts().page_passed_armed == passed + 1);
  // A null record goes through.
  const int calls = g_enableOriginalCalls.load();
  EnableEntry()(nullptr, nullptr);
  QCHECK(g_enableOriginalCalls.load() == calls + 1);
  board::Withdraw();
  UpdateEntry()(Obj(g_game), 16);
  lp::ResetLatchForTest();
}

// Failure caught: tying the latch to the followed instance, which is dropped when the game leaves "login
// failed" -- before the parked script wakes on RETRY. The latch lives through "logging in" and "logged in"
// and dies for good at "loading global", after which an error page is a real error.
void TheLatchOutlivesLoginFailedAndDiesAtLoadingGlobal() {
  PromptOnScreen("LATCH2-CODE");
  SetState(g_game, 2);  // RETRY: "logging in"
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(lp::LatchArmedForTest());
  QCHECK(!Enabled(ui::kErrorDisplayPage));
  SetState(g_game, 3);  // logged in
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(lp::LatchArmedForTest() && !Enabled(ui::kFatalErrorDisplayPage));
  SetState(g_game, layout::kStateLoadingGlobal);
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(!lp::LatchArmedForTest());
  QCHECK(Enabled(ui::kErrorDisplayPage));
  QCHECK(Enabled(ui::kFatalErrorDisplayPage));
  // Dead for good: a later prompt on the same instance does not arm it again.
  Publish(Prompt("LATCH3-CODE"));
  FailLocally(g_game);
  QCHECK(!lp::LatchArmedForTest());
  QCHECK(Enabled(ui::kErrorDisplayPage));
  board::Withdraw();
  UpdateEntry()(Obj(g_game), 16);
  lp::ResetLatchForTest();
}

// Failure caught: holding back a genuine error. Text the game writes into the block is not ours: the error
// text hook sees the write at once; a writer that bypasses it is seen at the next Update.
void AGenuineErrorTextPassesThrough() {
  PromptOnScreen("LATCH4-CODE");
  QCHECK(lp::LatchArmedForTest());
  SetState(g_game, layout::kStateLoggingIn);
  ErrorEntry()(Obj(g_game), "Account disabled by EchoVRCE Admins");  // a server message, not replaced
  QCHECK(Line(g_game, 0) == "Account disabled by EchoVRCE Admins");
  QCHECK(!lp::LatchArmedForTest());
  QCHECK(Enabled(ui::kErrorDisplayPage));
  QCHECK(Enabled(ui::kFatalErrorDisplayPage));
  board::Withdraw();

  PromptOnScreen("LATCH5-CODE");
  QCHECK(lp::LatchArmedForTest());
  FakeSetDelimitedErrorMessage(Obj(g_game), "Service is unavailable");  // another writer, not through the hook
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(!lp::LatchArmedForTest());
  QCHECK(Enabled(ui::kErrorDisplayPage));
  board::Withdraw();
  UpdateEntry()(Obj(g_game), 16);
  lp::ResetLatchForTest();
}

// No prompt was ever shown, or it was withdrawn and the game's own text came back: nothing is held.
void WithoutAPromptOnScreenNothingIsHeldBack() {
  lp::ResetLatchForTest();
  board::Withdraw();
  const lp::Counts before = lp::CurrentCounts();
  QCHECK(Enabled(ui::kErrorDisplayPage) && Enabled(ui::kFatalErrorDisplayPage) && Enabled(ui::kLoggingInPage));
  gate::SetReady(false);
  QCHECK(Enabled(ui::kLoggingInPage));  // nothing on screen to protect: not skipped even though not ready
  gate::SetReady(true);
  QCHECK(lp::CurrentCounts().error_page_dropped == before.error_page_dropped);
  QCHECK(lp::CurrentCounts().logging_in_page_dropped == before.logging_in_page_dropped);
  QCHECK(lp::CurrentCounts().page_passed_armed == before.page_passed_armed);
  PromptOnScreen("LATCH6-CODE");
  QCHECK(lp::LatchArmedForTest());
  board::Withdraw();  // the game's own message comes back
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(Line(g_game, 0) == kLocal);
  QCHECK(!lp::LatchArmedForTest());
  QCHECK(Enabled(ui::kErrorDisplayPage));
  lp::ResetLatchForTest();
}

// The page-enable hook may run on a worker thread while Update rewrites the block on the game thread: it
// reads only atomics, and the latch never opens while the block holds our text, so every enable of the error
// page is skipped. Built without ThreadSanitizer here; the invariant is exact counts and no game call.
void EnablesOnAnotherThreadDuringRewritesAreAllSkipped() {
  PromptOnScreen("RACE0-CODE");
  QCHECK(lp::LatchArmedForTest());
  const lp::Counts before = lp::CurrentCounts();
  g_enableOriginalCalls = 0;
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> calls{0};
  std::thread worker([&] {
    FakePageData data;
    std::memset(data.bytes, 0, sizeof(data.bytes));
    const std::uint64_t actor = ui::kErrorDisplayPage;
    std::memcpy(data.bytes + ui::kEnablePageActorIdOffset, &actor, sizeof(actor));
    while (!stop.load()) {
      EnableEntry()(data.bytes + ui::kEnablePageNodeOffset, data.bytes);
      calls.fetch_add(1);
    }
  });
  for (int i = 0; i < 3000; ++i) {
    char code[32];
    std::snprintf(code, sizeof(code), "RACE%04d-CODE", i);
    Publish(Prompt(code));
    UpdateEntry()(Obj(g_game), 16);
    QCHECK(lp::LatchArmedForTest());
  }
  stop = true;
  worker.join();
  QCHECK(calls.load() > 0);
  QCHECK(g_enableOriginalCalls.load() == 0);
  QCHECK(lp::CurrentCounts().error_page_dropped == before.error_page_dropped + calls.load());
  board::Withdraw();
  UpdateEntry()(Obj(g_game), 16);
  lp::ResetLatchForTest();
}

// ---- the attempt gate (review H2) --------------------------------------------------------------------

// Failure caught (#239 review H2): "Select RETRY" shown before the login may proceed. The notice is published
// at verification; the readiness the page-enable hook and the login prerequisites share turns ready on the
// token-auth poll, up to 2 s later, and a RETRY in between got its logging-in page skipped while the login
// went on to succeed. The notice is not applied until the gate is ready, on a screen showing the prompt and on
// one showing the game's own text, and then it is, on the next frame.
void ANoticeWaitsForTheLoginToBeAbleToProceed() {
  lp::ResetLatchForTest();
  gate::SetReady(false);
  // (a) The prompt is on screen when the player signs in.
  board::Withdraw();
  Publish(Prompt("GATE1-CODE"));
  FailLocally(g_game);
  Publish(kNotice, board::Mode::kNotice);
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(Line(g_game, 2) == "and enter the code GATE1-CODE");  // still the prompt: RETRY would be held back
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(Line(g_game, 2) == "and enter the code GATE1-CODE");
  gate::SetReady(true);
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(Line(g_game, 0) == "Signed in to EchoVRCE." && Line(g_game, 1) == "Select RETRY to finish.");
  board::Withdraw();
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(Line(g_game, 0) == kLocal);
  // (b) The attempt in flight fails with the notice already on the board but the login not yet able to proceed.
  lp::ResetLatchForTest();
  gate::SetReady(false);
  Publish(kNotice, board::Mode::kNotice);
  FailLocally(g_game);
  QCHECK(Line(g_game, 0) == kLocal);  // the game's text, not the notice
  QCHECK(!lp::LatchArmedForTest());
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(Line(g_game, 0) == kLocal);
  gate::SetReady(true);
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(Line(g_game, 0) == "Signed in to EchoVRCE." && Line(g_game, 1) == "Select RETRY to finish.");
  QCHECK(lp::LatchArmedForTest());
  board::Withdraw();
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(Line(g_game, 0) == kLocal);
  lp::ResetLatchForTest();
}

// Failure caught (#239 review H2b): an attempt whose logging-in page was skipped must fail its prerequisites,
// even if the readiness flips before they run (they run a moment after the page enable): one shared word, and
// the poison ends with the attempt, when the game leaves "logging in".
void ASkippedLoggingInPagePoisonsTheAttemptUntilItEnds() {
  PromptOnScreen("GATE2-CODE");
  gate::SetReady(false);
  SetState(g_game, layout::kStateLoggingIn);  // RETRY
  QCHECK(!Enabled(ui::kLoggingInPage));       // skipped: poisoned
  gate::SetReady(true);                       // the poll catches up before the prerequisites run
  QCHECK(!gate::LoginMayProceed());           // the attempt still fails them
  QCHECK(Enabled(ui::kLoggingInPage));        // the skip itself stopped with the readiness, at once
  UpdateEntry()(Obj(g_game), 16);             // still logging in: the poison stays
  QCHECK(!gate::LoginMayProceed());
  SetState(g_game, layout::kStateLoginFailed);  // the attempt failed (prerequisites)
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(gate::LoginMayProceed());              // the next RETRY starts clean
  // A RETRY with the gate ready is not poisoned and its page is enabled.
  SetState(g_game, layout::kStateLoggingIn);
  QCHECK(Enabled(ui::kLoggingInPage));
  QCHECK(gate::LoginMayProceed());
  board::Withdraw();
  SetState(g_game, layout::kStateLoginFailed);
  UpdateEntry()(Obj(g_game), 16);
  lp::ResetLatchForTest();
}

}  // namespace

int main() {
  sentinel::SetLogSink(&CaptureSink);
  lp::ErrorThunk::Reset();
  lp::UpdateThunk::Reset();
  lp::EnablePageThunk::Reset();
  *lp::ErrorThunk::OriginalOut() = reinterpret_cast<void*>(&FakeSetDelimitedErrorMessage);
  *lp::UpdateThunk::OriginalOut() = reinterpret_cast<void*>(&FakeUpdate);
  *lp::EnablePageThunk::OriginalOut() = reinterpret_cast<void*>(&FakeEnableOriginal);
  lp::ArmForTest();
  gate::SetReady(true);  // the default for the cases below; the ones that need another state set it

  TheGameKnowsWhichFailuresAreItsOwn();
  ALocalFailureWithNothingPublishedKeepsTheGameTextAndAPromptPublishedLaterReachesIt();
  ALocalFailureWhileLoggingInShowsThePromptAndTheGameNeverSeesTheCode();
  TheScreenFollowsTheBoardWhileTheGameStaysInLoginFailed();
  ABlockAnotherWriterChangedIsLeftAlone();
  ANoticePublishedBeforeAFailureIsShownForIt();
  ServerMessagesAndLoggedInRemovalsAreNeverReplaced();
  ABlockThatDoesNotHoldTheGameMessageIsNotWritten();
  ABusyBoardIsCountedAndThePromptFollowsOnTheNextFrame();
  ANoticeDoesNotReplaceTheGameTextOfAScreenThatShowedNoPrompt();
  ANewLocalFailureMissedByTheErrorHookIsTakenUpByUpdate();
  ASecondLocalFailureWhileAlreadyInLoginFailedGetsThePromptToo();
  ARewriteInLoginFailedSendsOneErrorEvent();
  NoErrorEventAfterTheGameLeftLoginFailed();
  AnUnresolvedErrorEventIsCountedAndNotCalled();
  NoErrorEventForAnInstanceThatIsNotFollowed();
  WhileThePromptIsOnScreenTheErrorPagesAreNotEnabled();
  TheLatchOutlivesLoginFailedAndDiesAtLoadingGlobal();
  AGenuineErrorTextPassesThrough();
  WithoutAPromptOnScreenNothingIsHeldBack();
  ANoticeWaitsForTheLoginToBeAbleToProceed();
  ASkippedLoggingInPagePoisonsTheAttemptUntilItEnds();
  EnablesOnAnotherThreadDuringRewritesAreAllSkipped();
  AWithdrawnBoardKeepsNoCode();
  TheBoardRefusesWhatTheGameCouldNotShow();
  ConcurrentReadsAreNeverTorn();
  QCHECK(lp::ErrorThunk::Faults() == 0 && lp::UpdateThunk::Faults() == 0 && lp::EnablePageThunk::Faults() == 0);
  CounterRefusalIsLoudAndInstallReportsBothSlots();

  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "login_prompt_hook_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("login_prompt_hook_test: the prompt replaces only the game's own login-failure text and follows the board\n");
  return 0;
}
