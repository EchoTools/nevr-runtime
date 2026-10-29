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
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "nevr/symbol_hash.h"
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
namespace gate = nevr_quest_login::attempt_gate;
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
std::mutex g_linesMutex;  // the page-enter line is written from whichever thread runs the hook
void CaptureSink(sentinel::LogLevel, const char* line) {
  std::lock_guard<std::mutex> lock(g_linesMutex);
  g_lines.emplace_back(line);
}
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
  // A nearly full reporter table (two slots left, the hook needs seventeen): the counters are refused,
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

// Failure caught (#239 review L2): a prompt published in the failure frame. The game sends its own error event
// for the failure; the Update hook then rewrote the block for the prompt and sent a second one in the same
// frame. The failure counts as the last event; the rewrite's event waits out the spacing.
void APromptPublishedInTheFailureFrameDoesNotSendASecondErrorEvent() {
  board::Withdraw();
  lp::SetClockForTest(&FakeClock);
  lp::SetQuitOnErrorForTest(&FakeQuitOnError);
  g_quitCalls = 0;
  g_now += 1000 * kMs;
  FailLocally(g_game);  // nothing on the board: the game's text stays; the game sends its own event
  Publish(Prompt("FRAME-CODE"));
  UpdateEntry()(Obj(g_game), 16);  // same instant: the block is rewritten, the event waits
  QCHECK(Line(g_game, 2) == "and enter the code FRAME-CODE");
  QCHECK(g_quitCalls == 0);
  g_now += 10 * kMs;
  UpdateEntry()(Obj(g_game), 16);  // still inside the spacing
  QCHECK(g_quitCalls == 0);
  g_now += 60 * kMs;
  UpdateEntry()(Obj(g_game), 16);  // a frame later: the screen is told once
  QCHECK(g_quitCalls == 1 && g_quitSawLine2 == "and enter the code FRAME-CODE");
  board::Withdraw();
  UpdateEntry()(Obj(g_game), 16);
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

// Failure caught (#239 review L1): a genuine error the game writes with the unhooked SetErrorMessage in the same
// frame as the page enable. The latch flag is only recomputed on the game thread, so it still said "armed"; the
// error page for that error was dropped. The enable hook now checks the live block against the latch.
void AGenuineErrorWrittenInTheSameFrameIsNotDropped() {
  PromptOnScreen("LIVE1-CODE");
  QCHECK(lp::LatchArmedForTest());
  unsigned char saved[layout::kErrorBlockBytes];
  std::memcpy(saved, BlockOf(g_game), sizeof(saved));
  const lp::Counts before = lp::CurrentCounts();
  FakeSetDelimitedErrorMessage(Obj(g_game), "Lobby is full");  // the game's writer the hooks do not see; no Update yet
  QCHECK(lp::LatchArmedForTest());                              // the flag has not caught up
  QCHECK(Enabled(ui::kErrorDisplayPage));                       // but the live block says otherwise: through
  QCHECK(Enabled(ui::kFatalErrorDisplayPage));
  QCHECK(lp::CurrentCounts().error_page_dropped == before.error_page_dropped);
  QCHECK(lp::CurrentCounts().page_passed_armed == before.page_passed_armed + 2);
  std::memcpy(BlockOf(g_game), saved, sizeof(saved));  // and with our text back in the block it is held again
  QCHECK(!Enabled(ui::kErrorDisplayPage));
  board::Withdraw();
  UpdateEntry()(Obj(g_game), 16);
  lp::ResetLatchForTest();
}

// Failure caught (#239 second review L1): the logging-in page skip and the poison were two steps. If the gate
// turned ready between the decision and the poison, the page was skipped and the attempt poisoned although the
// login could proceed: stranded on the login-failed screen, then failed by its own prerequisites. The poison is
// one compare-and-swap that fails when the gate turned ready, and then the page goes through. The flip is
// injected between the read and the swap.
void ALoggingInPageIsNotSkippedWhenTheGateTurnsReadyBeforeThePoison() {
  PromptOnScreen("CAS1-CODE");
  gate::SetReady(false);
  gate::g_beforePoisonCas = [](void) noexcept { gate::SetReady(true); };
  const lp::Counts before = lp::CurrentCounts();
  QCHECK(Enabled(ui::kLoggingInPage));  // passes through
  gate::g_beforePoisonCas = nullptr;
  QCHECK(lp::CurrentCounts().logging_in_page_dropped == before.logging_in_page_dropped);
  QCHECK(gate::LoginMayProceed());  // and the attempt is not poisoned
  // Without the flip the skip and the poison still happen together.
  gate::SetReady(false);
  QCHECK(!Enabled(ui::kLoggingInPage));
  QCHECK(!gate::LoginMayProceed() && !gate::IsReady());
  gate::SetReady(true);
  gate::ClearPoison();
  board::Withdraw();
  UpdateEntry()(Obj(g_game), 16);
  lp::ResetLatchForTest();
}

// #581: the hold bit says token auth will produce a token without the player, so the login answers wait for it.
// It is published with the ready bit in one store, a poison is never touched by it, and only an attempt that is
// not ready and not poisoned holds.
void TheHoldBitIsPublishedWithReadyAndReadsBackOnlyWhenNothingElseDecides() {
  gate::Reset();
  QCHECK(!gate::HoldsAnswers() && !gate::LoginMayProceed());
  gate::Publish(false, true);  // token auth is working on its own: wait for it
  QCHECK(gate::HoldsAnswers() && !gate::IsReady() && !gate::LoginMayProceed());
  gate::Publish(true, true);  // ready never holds, even with the hold bit set
  QCHECK(!gate::HoldsAnswers() && gate::IsReady());
  QCHECK(gate::LoginMayProceed());  // ready with the hold bit set may proceed
  gate::Publish(false, false);  // awaiting the player, or refused: deliver
  QCHECK(!gate::HoldsAnswers() && !gate::IsReady() && !gate::LoginMayProceed());

  // Publish keeps a poison, in both directions.
  gate::Reset();
  gate::Poison();
  gate::Publish(true, false);
  QCHECK(!gate::LoginMayProceed());  // the poisoned attempt still fails
  gate::Publish(false, true);
  QCHECK(!gate::HoldsAnswers());  // a poisoned attempt never holds
  gate::ClearPoison();
  QCHECK(gate::HoldsAnswers());  // the hold bit was published under the poison and is back in force
  gate::Publish(true, true);
  gate::Poison();
  QCHECK(!gate::LoginMayProceed() && !gate::HoldsAnswers());
  gate::ClearPoison();
  QCHECK(gate::LoginMayProceed());

  // PoisonIfNotReady keeps the hold bit; a ready gate is not poisoned.
  gate::Reset();
  gate::Publish(false, true);
  QCHECK(gate::PoisonIfNotReady());
  QCHECK(!gate::HoldsAnswers());  // poisoned: never holds
  gate::ClearPoison();
  QCHECK(gate::HoldsAnswers());  // the hold bit survived the poison
  gate::Publish(true, true);
  QCHECK(!gate::PoisonIfNotReady() && gate::LoginMayProceed());

  // SetReady touches only the ready bit.
  gate::Reset();
  gate::Publish(false, true);
  gate::SetReady(true);
  QCHECK(!gate::HoldsAnswers() && gate::LoginMayProceed());
  gate::SetReady(false);
  QCHECK(gate::HoldsAnswers());
  gate::Reset();
  QCHECK(!gate::HoldsAnswers() && !gate::IsReady());
  gate::SetReady(true);  // the default for the cases that follow
}

// #391: every page enable is logged with its symbol, once per symbol per second, and the game's Enter still
// runs for all of them. (The page-enter log writes only atomics and one stack-built line, so it is safe on
// the worker thread the hook can run on.)
void EveryPageEnableIsLoggedOncePerSymbolPerSecond() {
  lp::ResetLatchForTest();
  lp::ResetPageLogForTest();
  lp::SetClockForTest(&FakeClock);
  g_now = 5'000'000'000;
  g_lines.clear();
  const int originalBefore = g_enableOriginalCalls.load();
  QCHECK(Enabled(kSomeOtherPage));
  QCHECK(Enabled(kSomeOtherPage));
  QCHECK(Enabled(kSomeOtherPage));
  QCHECK(CountLines("\"event\":\"ui_page_enter\"") == 1);
  QCHECK(CountLines("\"page\":\"0x1234567890abcdef\"") == 1 && CountLines("\"enters\":1") == 1);
  QCHECK(CountLines("\"name\":\"unknown\"") == 1 && CountLines("\"latch_armed\":0") == 1);
  // A second page has its own line at once; the first logs again after a second, carrying the four enables
  // since its line (the call that triggers the line counts).
  QCHECK(Enabled(ui::kLoggingInPage));
  QCHECK(CountLines("\"event\":\"ui_page_enter\"") == 2 && CountLines("\"name\":\"logging_in_page\"") == 1);
  g_now += 999'999'999;
  QCHECK(Enabled(kSomeOtherPage));
  QCHECK(CountLines("\"event\":\"ui_page_enter\"") == 2);  // not yet
  g_now += 1;
  QCHECK(Enabled(kSomeOtherPage));
  QCHECK(CountLines("\"event\":\"ui_page_enter\"") == 3 && CountLines("\"enters\":4") == 1);
  QCHECK(g_enableOriginalCalls.load() == originalBefore + 6);  // the game's Enter ran for every one
  lp::SetClockForTest(nullptr);
}

// #391: the page ids smoke #5 run 2 logged as "unknown". Each id's name and name_basis are derived here from
// the evidence rows in tests/data/ui_page_names.tsv (one row per level-script binding that targets the id),
// and every named row's hash is checked to be CSymbol64 of its name; ui::kPageNames and the logged lines
// must match what the rows say, so editing the table without the evidence fails.
struct PageEvidence {
  std::vector<std::string> names;  // one per named binding
  bool excluded = false;           // an "unknown" row: the evidence is kept, the name is not used
};

std::map<std::uint64_t, PageEvidence> LoadPageEvidence() {
  std::map<std::uint64_t, PageEvidence> evidence;
  std::ifstream in("src/quest/tests/data/ui_page_names.tsv");
  QCHECK(in.is_open());
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> f;
    std::size_t start = 0;
    for (std::size_t tab; (tab = line.find('\t', start)) != std::string::npos; start = tab + 1) {
      f.push_back(line.substr(start, tab - start));
    }
    f.push_back(line.substr(start));
    if (f[0] == "unknown") {
      QCHECK(f.size() == 3);
      evidence[std::strtoull(f[1].c_str(), nullptr, 16)].excluded = true;
      continue;
    }
    QCHECK(f.size() == 7);
    PageEvidence& page = evidence[std::strtoull(f[0].c_str(), nullptr, 16)];
    if (f[6] != "-") {
      QCHECK(EchoVR::CSymbol64Hash(f[6].c_str()) == std::strtoull(f[5].c_str(), nullptr, 16));
      page.names.push_back(f[6]);
    }
  }
  return evidence;
}

struct DerivedName {
  std::string name = "unknown";
  std::string basis = "none";
};

std::map<std::uint64_t, DerivedName> DeriveNames(const std::map<std::uint64_t, PageEvidence>& evidence) {
  std::map<std::uint64_t, DerivedName> derived;
  std::map<std::string, int> idsPerName;
  for (const auto& [id, page] : evidence) {
    if (page.excluded || page.names.empty()) continue;
    std::map<std::string, int> counts;
    for (const std::string& n : page.names) ++counts[n];
    std::string top;
    int best = 0;
    bool tie = false;
    for (const auto& [n, c] : counts) {
      if (c > best) { top = n; best = c; tie = false; }
      else if (c == best) tie = true;
    }
    if (tie) continue;
    DerivedName d;
    d.name = top;
    d.basis = counts.size() == 1 ? (page.names.size() == 1 ? "single" : "all") : "majority";
    derived[id] = d;
    ++idsPerName[top];
  }
  for (auto& [id, d] : derived) {
    if (idsPerName[d.name] > 1) {
      char tag[8];
      std::snprintf(tag, sizeof(tag), "@%04llx", static_cast<unsigned long long>(id >> 48));
      d.name += tag;
    }
  }
  return derived;
}

void PageEnterLinesNameTheSmokePagesFromTheEvidenceRows() {
  const auto evidence = LoadPageEvidence();
  const auto derived = DeriveNames(evidence);
  // The table says what the rows say, no more and no less.
  for (const ui::PageName& known : ui::kPageNames) {
    const auto it = derived.find(known.id);
    QCHECK(it != derived.end());
    if (it != derived.end()) {
      QCHECK(it->second.name == known.name);
      QCHECK(it->second.basis == known.basis);
    }
  }
  std::size_t tableSize = 0;
  for (const ui::PageName& known : ui::kPageNames) { (void)known; ++tableSize; }
  QCHECK(tableSize == derived.size());

  // The ids smoke #5 run 2 logged (smoke5/run2-ui_page_enter.txt), checked through the log line itself.
  const std::uint64_t smoke[] = {
      0x9733f27f738d9595ULL, 0xfbc6a43d068f418dULL, 0xdadda9a8c9c49f8dULL, 0x9143e219cb923869ULL,
      0x25cbdc13a0ccdf42ULL, 0x1a92c34885065c11ULL, 0x80d0b99e73cf486aULL, 0x1f5bccac7f496eddULL,
      0xffffffffffffffffULL, 0xd436ecc9f7f9164dULL, 0x68db70ece7c24901ULL, 0xb245345073f0d3b3ULL,
      0x2fe7102931d1c4e0ULL, 0x05ce113632359ce3ULL, 0x8c94450216e31162ULL, 0xc615ef51fe8c7e5bULL,
      0x202763036b7f6f23ULL,
  };
  lp::ResetLatchForTest();
  lp::ResetPageLogForTest();
  lp::SetClockForTest(&FakeClock);
  g_now = 3'000'000'000;
  g_lines.clear();
  int named = 0;
  for (const std::uint64_t id : smoke) {
    const auto it = derived.find(id);
    const DerivedName d = it != derived.end() ? it->second : DerivedName{};
    named += it != derived.end() ? 1 : 0;
    char want[160];
    std::snprintf(want, sizeof(want), "\"page\":\"0x%016llx\",\"name\":\"%s\",\"name_basis\":\"%s\"",
                  static_cast<unsigned long long>(id), d.name.c_str(), d.basis.c_str());
    QCHECK(Enabled(id));
    QCHECK(CountLines(want) == 1);
  }
  QCHECK(named == 12);  // 12 of the 17 ids; 05ce, 68db, 9733, the boot page and the no-actor id stay unknown
  QCHECK(CountLines("\"name\":\"unknown\",\"name_basis\":\"none\"") == 5);
  lp::ResetPageLogForTest();
  lp::SetClockForTest(nullptr);
}

// The volume: one hot page enabled 60 times a second for ten minutes, and a handful of others, stay at
// one line per symbol per second at most, and the latch state is carried.
void PageEnterLogVolumeIsBounded() {
  lp::ResetLatchForTest();
  lp::ResetPageLogForTest();
  lp::SetClockForTest(&FakeClock);
  g_now = 1'000'000'000;
  g_lines.clear();
  for (int second = 0; second < 600; ++second) {
    for (int frame = 0; frame < 60; ++frame) {
      QCHECK(Enabled(kSomeOtherPage));
      g_now += 16'666'666;
    }
  }
  const int hot = CountLines("\"event\":\"ui_page_enter\"");
  QCHECK(hot >= 580 && hot <= 601);  // one a second for the 10 minutes, not 36000
  g_lines.clear();
  QCHECK(Enabled(ui::kErrorDisplayPage) && Enabled(ui::kLoggingInPage));
  QCHECK(CountLines("\"event\":\"ui_page_enter\"") == 2);
  lp::SetClockForTest(nullptr);
}

// Every one of more pages than the table holds still reaches the game, and the table says once that it is full.
void APageTableThatIsFullStillPassesEveryEnable() {
  lp::ResetLatchForTest();
  lp::ResetPageLogForTest();
  lp::SetClockForTest(&FakeClock);
  g_now = 9'000'000'000;
  g_lines.clear();
  for (std::uint64_t i = 0; i < 300; ++i) QCHECK(Enabled(0x7000000000000000ULL + i * 0x10001ULL));
  QCHECK(CountLines("\"event\":\"ui_page_enter\"") == 128);
  QCHECK(CountLines("\"event\":\"ui_page_enter_table_full\"") == 1);
  lp::ResetPageLogForTest();
  lp::SetClockForTest(nullptr);
}

// #672: the device code was accepted, the screen shows the signed-in notice ("Select RETRY to finish") and the
// login may proceed. RETRY runs the game's BeginLogIn; the hook runs it once for the notice so the player does not
// have to. Never before the gate is ready (the logging-in page would be held back), never for the prompt, never
// when the game left "login failed", and once per notice.
int g_beginCalls = 0;
CR15NetGameOpaque* g_beginSelf = nullptr;
std::int32_t g_beginSawState = 0;
void FakeBeginLogIn(CR15NetGameOpaque* self) noexcept {
  ++g_beginCalls;
  g_beginSelf = self;
  std::memcpy(&g_beginSawState, reinterpret_cast<unsigned char*>(self) + layout::kStateOffset, sizeof(g_beginSawState));
  SetState(g_game, layout::kStateLoggingIn);  // what the game's BeginLogIn does
}

bool HoldBlockWriterForTestOrFail() { return lp::HoldBlockWriterForTest(); }

void TheSignedInNoticeStartsTheLoginOnce() {
  lp::ResetLatchForTest();
  gate::SetReady(true);
  lp::SetBeginLogInForTest(&FakeBeginLogIn);
  g_beginCalls = 0;
  board::Withdraw();
  Publish(Prompt("RST1-CODE"));
  FailLocally(g_game);
  const lp::Counts before = lp::CurrentCounts();
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(g_beginCalls == 0);  // the prompt is on screen: the account does not exist yet

  Publish(kNotice, board::Mode::kNotice);  // the code was accepted
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(Line(g_game, 0) == "Signed in to EchoVRCE.");
  QCHECK(g_beginCalls == 1 && g_beginSelf == Obj(g_game));
  QCHECK(g_beginSawState == layout::kStateLoginFailed);  // called while the game sits in "login failed"
  QCHECK(lp::CurrentCounts().login_restarted == before.login_restarted + 1);
  for (int i = 0; i < 3; ++i) UpdateEntry()(Obj(g_game), 16);
  QCHECK(g_beginCalls == 1);  // the game is now logging in: nothing more

  // The same notice on a game that stays in "login failed" (the login failed again, locally): once per notice.
  FailLocally(g_game);
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(g_beginCalls == 1);
  Publish(kNotice, board::Mode::kNotice);  // a new publish is a new version
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(g_beginCalls == 2);
  board::Withdraw();
  UpdateEntry()(Obj(g_game), 16);
  lp::SetBeginLogInForTest(nullptr);
  lp::ResetLatchForTest();
}

void TheLoginIsNotStartedBeforeItMayProceedOrWhenTheGameMovedOn() {
  lp::ResetLatchForTest();
  lp::SetBeginLogInForTest(&FakeBeginLogIn);
  g_beginCalls = 0;
  // Not ready: the notice is not even on screen yet, and BeginLogIn would only strand the player.
  gate::SetReady(false);
  board::Withdraw();
  Publish(Prompt("RST2-CODE"));
  FailLocally(g_game);
  Publish(kNotice, board::Mode::kNotice);
  for (int i = 0; i < 3; ++i) UpdateEntry()(Obj(g_game), 16);
  QCHECK(g_beginCalls == 0);
  gate::SetReady(true);
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(g_beginCalls == 1);

  // The game left "login failed" on its own (the player selected RETRY first): no second login.
  lp::SetBeginLogInForTest(&FakeBeginLogIn);
  g_beginCalls = 0;
  board::Withdraw();
  Publish(Prompt("RST3-CODE"));
  FailLocally(g_game);
  Publish(kNotice, board::Mode::kNotice);
  SetState(g_game, layout::kStateLoggingIn);  // RETRY pressed in this very frame
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(g_beginCalls == 0);

  // The notice was applied by the error hook (the board held it when the attempt failed), and the login may not
  // proceed any more by the next frame: nothing is started.
  lp::SetBeginLogInForTest(&FakeBeginLogIn);
  g_beginCalls = 0;
  board::Withdraw();
  Publish(kNotice, board::Mode::kNotice);
  FailLocally(g_game);
  QCHECK(Line(g_game, 0) == "Signed in to EchoVRCE.");
  gate::SetReady(false);
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(g_beginCalls == 0);
  gate::SetReady(true);
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(g_beginCalls == 1);

  // The game left "login failed" while the other hook held the block writer (so the followed instance was not
  // dropped this frame): no second login.
  lp::SetBeginLogInForTest(&FakeBeginLogIn);
  g_beginCalls = 0;
  board::Withdraw();
  Publish(kNotice, board::Mode::kNotice);
  FailLocally(g_game);
  QCHECK(g_beginCalls == 0);
  SetState(g_game, layout::kStateLoggingIn);
  QCHECK(HoldBlockWriterForTestOrFail());
  UpdateEntry()(Obj(g_game), 16);
  lp::ReleaseBlockWriterForTest();
  QCHECK(g_beginCalls == 0);
  board::Withdraw();
  UpdateEntry()(Obj(g_game), 16);
  lp::SetBeginLogInForTest(nullptr);
  lp::ResetLatchForTest();
}

int g_retryCalls = 0;
void FakeRetry(CR15NetGameOpaque*) noexcept {
  ++g_retryCalls;
}

// With the RETRY select available the restart sends it alone (#672): the page script that follows "logged in"
// runs only for the select, and its BeginLogin node starts the login. BeginLogIn is the fallback: called once if
// the game is still in "login failed" two seconds after the select.
void TheRestartSendsTheRetrySelectAloneAndFallsBackToBeginLogIn() {
  lp::ResetLatchForTest();
  gate::SetReady(true);
  lp::SetClockForTest(&FakeClock);
  lp::SetBeginLogInForTest(&FakeBeginLogIn);
  lp::SetRetryEventForTest(&FakeRetry);
  g_beginCalls = 0;
  g_retryCalls = 0;
  g_now = 1'000'000'000;
  board::Withdraw();
  Publish(Prompt("RST5-CODE"));
  FailLocally(g_game);
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(g_retryCalls == 0 && g_beginCalls == 0);  // the prompt is on screen: nothing
  Publish(kNotice, board::Mode::kNotice);
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(g_retryCalls == 1 && g_beginCalls == 0);  // the select alone
  g_now += 1'900'000'000;
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(g_retryCalls == 1 && g_beginCalls == 0);  // inside the window
  g_now += 200'000'000;
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(g_retryCalls == 1 && g_beginCalls == 1);  // the select did nothing: the direct call, once
  for (int i = 0; i < 3; ++i) {
    g_now += 5'000'000'000;
    UpdateEntry()(Obj(g_game), 16);
  }
  QCHECK(g_retryCalls == 1 && g_beginCalls == 1);
  board::Withdraw();
  UpdateEntry()(Obj(g_game), 16);
  lp::SetRetryEventForTest(nullptr);
  lp::SetBeginLogInForTest(nullptr);
  lp::SetClockForTest(nullptr);
  lp::ResetLatchForTest();
}

// The select starting the login (the game leaves "login failed") ends the wait: no direct call afterwards.
void ASelectThatStartsTheLoginIsNotFollowedByBeginLogIn() {
  lp::ResetLatchForTest();
  gate::SetReady(true);
  lp::SetClockForTest(&FakeClock);
  lp::SetBeginLogInForTest(&FakeBeginLogIn);
  lp::SetRetryEventForTest(&FakeRetry);
  g_beginCalls = 0;
  g_retryCalls = 0;
  g_now = 1'000'000'000;
  board::Withdraw();
  Publish(Prompt("RST6-CODE"));
  FailLocally(g_game);
  Publish(kNotice, board::Mode::kNotice);
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(g_retryCalls == 1);
  SetState(g_game, layout::kStateLoggingIn);  // what the script's BeginLogin node does
  g_now += 10'000'000'000;
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(g_beginCalls == 0);
  board::Withdraw();
  UpdateEntry()(Obj(g_game), 16);
  lp::SetRetryEventForTest(nullptr);
  lp::SetBeginLogInForTest(nullptr);
  lp::SetClockForTest(nullptr);
  lp::ResetLatchForTest();
}

void AnUnresolvedBeginLogInIsCountedAndNotCalled() {
  lp::ResetLatchForTest();
  gate::SetReady(true);
  lp::SetBeginLogInForTest(nullptr);
  board::Withdraw();
  Publish(Prompt("RST4-CODE"));
  FailLocally(g_game);
  Publish(kNotice, board::Mode::kNotice);
  const lp::Counts before = lp::CurrentCounts();
  UpdateEntry()(Obj(g_game), 16);
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(lp::CurrentCounts().login_restart_unavailable == before.login_restart_unavailable + 1);  // once per notice
  QCHECK(lp::CurrentCounts().login_restarted == before.login_restarted);
  QCHECK(Line(g_game, 1) == "Select RETRY to finish.");  // the screen still tells the player what to do
  board::Withdraw();
  UpdateEntry()(Obj(g_game), 16);
  lp::ResetLatchForTest();
}

// ---- the RETRY select, the fallback decision and the NetGame state (#1391) -------------------------------------
// Each line is read back from the captured log, so a line that is not written, or written with another outcome,
// fails here. The select is the real SendRetrySelect (UseRetrySelectForTest), sent through a fake.

int g_sendCalls = 0;
void* g_sendSpace = nullptr;
std::uint64_t g_sendEventArg = 0;
std::uint64_t g_sendKeyArg = 0;
std::uint64_t g_sendActorArg = 0;
std::uint32_t g_sendFlagArg = 0;
void FakeSend(void* space, std::uint64_t event, std::uint64_t actor, std::uint64_t key, std::uint32_t flag) {
  ++g_sendCalls;
  g_sendSpace = space;
  g_sendEventArg = event;
  g_sendActorArg = actor;
  g_sendKeyArg = key;
  g_sendFlagArg = flag;
}

// The object CR15NetGame+0x8 points to, with the game space at +0x7af0 (SendRetrySelect reads both).
struct alignas(16) FakeGameObject {
  unsigned char bytes[layout::kGameSpaceOffset + 16];
};
FakeGameObject g_gameObject;
int g_spaceMarker = 0;  // stands for the game space: only its address is handed on

void LinkGameObject(FakeGame& g, const void* object) { std::memcpy(g.bytes + layout::kGameOffset, &object, sizeof(object)); }
void SetSpaceOf(FakeGameObject& o, const void* space) {
  std::memcpy(o.bytes + layout::kGameSpaceOffset, &space, sizeof(space));
}

// The last captured line that holds `needle`, or "" when none does.
std::string LastLine(const char* needle) {
  for (auto it = g_lines.rbegin(); it != g_lines.rend(); ++it) {
    if (it->find(needle) != std::string::npos) return *it;
  }
  return std::string();
}
bool Holds(const std::string& line, const char* text) { return line.find(text) != std::string::npos; }

// A new attempt: the notice goes on the board, the attempt fails with the game's local text, and the Update that
// restarts the login runs.
void RestartFromNotice() {
  board::Withdraw();
  Publish(kNotice, board::Mode::kNotice);
  FailLocally(g_game);
  UpdateEntry()(Obj(g_game), 16);
}

constexpr const char* kSelectLine = "\"event\":\"login_prompt_select\"";
constexpr const char* kFallbackLine = "\"event\":\"login_prompt_fallback\"";
constexpr const char* kStateLine = "\"event\":\"login_prompt_netgame_state\"";

// The select that is sent says what it sends: the outcome, the event (evt_btn_clicked), the key (btn_exit's branch,
// not RETRY's), the actor, and the flag. The counter counts the send.
void TheRetrySelectLogsWhatItSends() {
  lp::ResetLatchForTest();
  gate::SetReady(true);
  lp::SetClockForTest(&FakeClock);
  lp::SetBeginLogInForTest(&FakeBeginLogIn);
  lp::SetSendEventForTest(&FakeSend);
  lp::UseRetrySelectForTest();
  g_beginCalls = 0;
  g_sendCalls = 0;
  g_now = 1'000'000'000;
  LinkGameObject(g_game, &g_gameObject);
  SetSpaceOf(g_gameObject, &g_spaceMarker);
  const lp::Counts before = lp::CurrentCounts();
  const int linesBefore = CountLines(kSelectLine);
  RestartFromNotice();
  QCHECK(g_sendCalls == 1 && g_sendSpace == &g_spaceMarker);
  QCHECK(g_sendEventArg == sentinel::pinned::kRetrySelectEvent);
  QCHECK(g_sendKeyArg == sentinel::pinned::kLoginPageExitKey && g_sendActorArg == sentinel::pinned::kAllActors);
  QCHECK(g_sendFlagArg == 1);
  QCHECK(CountLines(kSelectLine) == linesBefore + 1);
  const std::string line = LastLine(kSelectLine);
  QCHECK(Holds(line, "\"outcome\":\"sent\""));
  QCHECK(Holds(line, "\"trigger\":\"after_sign_in\""));
  QCHECK(Holds(line, "\"send_event\":\"0x8f0094c7bb02681c\""));
  QCHECK(Holds(line, "\"send_event_name\":\"evt_btn_clicked\""));
  QCHECK(Holds(line, "\"key\":\"0x34f99a282ce78d72\"") && Holds(line, "\"key_name\":\"btn_exit\""));
  QCHECK(Holds(line, "\"actor\":\"0x0000ffffffffffff\"") && Holds(line, "\"actor_name\":\"all_actors\""));
  QCHECK(lp::CurrentCounts().select_sent == before.select_sent + 1);

  LinkGameObject(g_game, nullptr);
  lp::SetSendEventForTest(nullptr);
  lp::SetRetryEventForTest(nullptr);
  lp::SetBeginLogInForTest(nullptr);
  lp::SetClockForTest(nullptr);
  board::Withdraw();
  UpdateEntry()(Obj(g_game), 16);
  lp::ResetLatchForTest();
}

// A select that is not sent says why, is logged with that outcome, and is not counted. Each early return of
// SendRetrySelect has its own outcome.
void TheRetrySelectSaysWhyItWasNotSent() {
  lp::ResetLatchForTest();
  gate::SetReady(true);
  lp::SetClockForTest(&FakeClock);
  lp::SetBeginLogInForTest(&FakeBeginLogIn);
  lp::UseRetrySelectForTest();
  g_beginCalls = 0;
  g_sendCalls = 0;
  g_now = 1'000'000'000;
  const lp::Counts before = lp::CurrentCounts();
  const int linesBefore = CountLines(kSelectLine);

  lp::SetSendEventForTest(nullptr);  // the send function is not resolved
  LinkGameObject(g_game, &g_gameObject);
  SetSpaceOf(g_gameObject, &g_spaceMarker);
  RestartFromNotice();
  QCHECK(Holds(LastLine(kSelectLine), "\"outcome\":\"send_unresolved\""));

  lp::SetSendEventForTest(&FakeSend);
  LinkGameObject(g_game, nullptr);  // the game object is missing
  RestartFromNotice();
  QCHECK(Holds(LastLine(kSelectLine), "\"outcome\":\"game_null\""));

  LinkGameObject(g_game, &g_gameObject);
  SetSpaceOf(g_gameObject, nullptr);  // the game space is missing
  RestartFromNotice();
  QCHECK(Holds(LastLine(kSelectLine), "\"outcome\":\"space_null\""));

  QCHECK(CountLines(kSelectLine) == linesBefore + 3);  // one line per restart, none of them a send
  QCHECK(g_sendCalls == 0);
  QCHECK(lp::CurrentCounts().select_sent == before.select_sent);

  LinkGameObject(g_game, nullptr);
  lp::SetSendEventForTest(nullptr);
  lp::SetRetryEventForTest(nullptr);
  lp::SetBeginLogInForTest(nullptr);
  lp::SetClockForTest(nullptr);
  board::Withdraw();
  UpdateEntry()(Obj(g_game), 16);
  lp::ResetLatchForTest();
}

// The fallback line is written when the direct BeginLogIn call is made: its reason, the time since the select in
// nanoseconds, the NetGame state it saw, and the outcome of the select.
void TheFallbackLogsItsDecisionTimeAndState() {
  lp::ResetLatchForTest();
  gate::SetReady(true);
  lp::SetClockForTest(&FakeClock);
  lp::SetBeginLogInForTest(&FakeBeginLogIn);
  lp::SetRetryEventForTest(&FakeRetry);
  g_beginCalls = 0;
  g_retryCalls = 0;
  g_now = 1'000'000'000;
  board::Withdraw();
  Publish(Prompt("FBK1-CODE"));
  FailLocally(g_game);
  UpdateEntry()(Obj(g_game), 16);
  const int fallbacksBefore = CountLines(kFallbackLine);
  Publish(kNotice, board::Mode::kNotice);
  UpdateEntry()(Obj(g_game), 16);  // the select, at g_now
  QCHECK(g_retryCalls == 1 && g_beginCalls == 0);
  g_now += 1'900'000'000;
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(CountLines(kFallbackLine) == fallbacksBefore);  // inside the window: no decision yet
  g_now += 200'000'000;
  UpdateEntry()(Obj(g_game), 16);  // 2.1 s after the select: the direct call
  QCHECK(g_beginCalls == 1);
  QCHECK(CountLines(kFallbackLine) == fallbacksBefore + 1);
  const std::string line = LastLine(kFallbackLine);
  QCHECK(Holds(line, "\"reason\":\"select_no_effect\""));
  QCHECK(Holds(line, "\"elapsed_ns\":2100000000"));
  QCHECK(Holds(line, "\"state\":-94,\"state_name\":\"login_failed\""));
  QCHECK(Holds(line, "\"select_outcome\":\"none\""));  // FakeRetry is not SendRetrySelect: no outcome was set

  board::Withdraw();
  UpdateEntry()(Obj(g_game), 16);
  lp::SetRetryEventForTest(nullptr);
  lp::SetBeginLogInForTest(nullptr);
  lp::SetClockForTest(nullptr);
  lp::ResetLatchForTest();
}

// Without a select the fallback is the direct call at once, with reason select_unavailable and elapsed_ns 0. When
// the select starts the login (the game leaves "login failed") no fallback line is written.
void TheFallbackSaysWhyItCallsAndIsSilentWhenTheSelectStartsTheLogin() {
  lp::ResetLatchForTest();
  gate::SetReady(true);
  lp::SetClockForTest(&FakeClock);
  lp::SetBeginLogInForTest(&FakeBeginLogIn);
  lp::SetRetryEventForTest(nullptr);
  g_beginCalls = 0;
  g_now = 1'000'000'000;
  const int fallbacksBefore = CountLines(kFallbackLine);
  RestartFromNotice();
  QCHECK(g_beginCalls == 1);
  QCHECK(CountLines(kFallbackLine) == fallbacksBefore + 1);
  const std::string line = LastLine(kFallbackLine);
  QCHECK(Holds(line, "\"reason\":\"select_unavailable\""));
  QCHECK(Holds(line, "\"elapsed_ns\":0"));
  QCHECK(Holds(line, "\"state\":-94,\"state_name\":\"login_failed\""));

  lp::SetRetryEventForTest(&FakeRetry);
  g_beginCalls = 0;
  g_retryCalls = 0;
  RestartFromNotice();  // the select is sent, and the game starts the login on it
  QCHECK(g_retryCalls == 1);
  SetState(g_game, layout::kStateLoggingIn);
  g_now += 10'000'000'000;
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(g_beginCalls == 0);
  QCHECK(CountLines(kFallbackLine) == fallbacksBefore + 1);  // unchanged: the select started the login

  board::Withdraw();
  UpdateEntry()(Obj(g_game), 16);
  lp::SetRetryEventForTest(nullptr);
  lp::SetBeginLogInForTest(nullptr);
  lp::SetClockForTest(nullptr);
  lp::ResetLatchForTest();
}

// The NetGame state is logged once per change, from the Update that sees it, with the state as a number and a name.
// The first state this hook sees is the baseline and is not a change.
void TheNetGameStateIsLoggedOncePerChange() {
  constexpr std::int32_t kLoggedIn = 3;
  lp::ResetLatchForTest();
  gate::SetReady(true);
  board::Withdraw();
  SetState(g_game, layout::kStateLoginFailed);
  UpdateEntry()(Obj(g_game), 16);  // the baseline, whatever the state was before
  const int before = CountLines(kStateLine);
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(CountLines(kStateLine) == before);  // the same state again: nothing

  SetState(g_game, layout::kStateLoggingIn);
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(CountLines(kStateLine) == before + 1);
  QCHECK(Holds(LastLine(kStateLine), "\"from\":-94,\"from_name\":\"login_failed\",\"to\":2,\"to_name\":\"logging_in\""));
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(CountLines(kStateLine) == before + 1);  // still logging in

  SetState(g_game, kLoggedIn);
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(CountLines(kStateLine) == before + 2);
  QCHECK(Holds(LastLine(kStateLine), "\"from\":2,\"from_name\":\"logging_in\",\"to\":3,\"to_name\":\"logged_in\""));

  SetState(g_game, layout::kStateLoadingGlobal);
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(CountLines(kStateLine) == before + 3);
  QCHECK(Holds(LastLine(kStateLine), "\"from\":3,\"from_name\":\"logged_in\",\"to\":4,\"to_name\":\"loading_global\""));

  SetState(g_game, 0);  // logged out
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(CountLines(kStateLine) == before + 4);
  QCHECK(Holds(LastLine(kStateLine), "\"from\":4,\"from_name\":\"loading_global\",\"to\":0,\"to_name\":\"logged_out\""));

  SetState(g_game, 7);  // a state this file does not name
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(CountLines(kStateLine) == before + 5);
  QCHECK(Holds(LastLine(kStateLine), "\"from\":0,\"from_name\":\"logged_out\",\"to\":7,\"to_name\":\"unnamed\""));
  lp::ResetLatchForTest();  // "loading global" ends the latch for good; the next case starts clean
}

// The key and the event are the names the log gives them: CSymbol64 of "btn_exit" and of "evt_btn_clicked".
void TheSelectsKeyAndEventAreTheirSymbolNames() {
  QCHECK(EchoVR::CSymbol64Hash("btn_exit") == sentinel::pinned::kLoginPageExitKey);
  QCHECK(EchoVR::CSymbol64Hash("evt_btn_clicked") == sentinel::pinned::kRetrySelectEvent);
  QCHECK(sentinel::pinned::kLoginPageExitKey == 0x34f99a282ce78d72ULL);  // the rename keeps the value
}

// A select for an unset game object says so: the outcome is self_null, nothing is sent and the counter stays put.
void TheSelectNamesAnUnsetSelfAndSendsNothing() {
  lp::SetSendEventForTest(&FakeSend);
  g_sendCalls = 0;
  const lp::Counts before = lp::CurrentCounts();
  const int linesBefore = CountLines(kSelectLine);
  lp::SendRetrySelectForTest(nullptr);
  QCHECK(CountLines(kSelectLine) == linesBefore + 1);
  QCHECK(Holds(LastLine(kSelectLine), "\"outcome\":\"self_null\""));
  QCHECK(g_sendCalls == 0);
  QCHECK(lp::CurrentCounts().select_sent == before.select_sent);
  lp::SetSendEventForTest(nullptr);
}

// The fallback line names the select's outcome. A notice that gets no select names "none": the outcome is reset for
// each notice, so it is never the outcome of the notice before.
void TheFallbackNamesTheSelectsOutcomeAndTheNextNoticeNamesNone() {
  lp::ResetLatchForTest();
  gate::SetReady(true);
  lp::SetClockForTest(&FakeClock);
  lp::SetBeginLogInForTest(&FakeBeginLogIn);
  lp::SetSendEventForTest(&FakeSend);
  lp::UseRetrySelectForTest();
  g_beginCalls = 0;
  g_sendCalls = 0;
  g_now = 1'000'000'000;
  LinkGameObject(g_game, &g_gameObject);
  SetSpaceOf(g_gameObject, &g_spaceMarker);
  const int fallbacksBefore = CountLines(kFallbackLine);
  RestartFromNotice();  // the select is sent
  QCHECK(g_sendCalls == 1);
  g_now += 2'100'000'000;
  UpdateEntry()(Obj(g_game), 16);  // 2.1 s after the select: the direct call, which names the select
  QCHECK(g_beginCalls == 1);
  QCHECK(CountLines(kFallbackLine) == fallbacksBefore + 1);
  QCHECK(Holds(LastLine(kFallbackLine), "\"reason\":\"select_no_effect\""));
  QCHECK(Holds(LastLine(kFallbackLine), "\"select_outcome\":\"sent\""));

  lp::SetRetryEventForTest(nullptr);  // the next notice gets no select
  g_beginCalls = 0;
  RestartFromNotice();
  QCHECK(CountLines(kFallbackLine) == fallbacksBefore + 2);
  QCHECK(Holds(LastLine(kFallbackLine), "\"reason\":\"select_unavailable\""));
  QCHECK(Holds(LastLine(kFallbackLine), "\"select_outcome\":\"none\""));

  LinkGameObject(g_game, nullptr);
  lp::SetSendEventForTest(nullptr);
  lp::SetRetryEventForTest(nullptr);
  lp::SetBeginLogInForTest(nullptr);
  lp::SetClockForTest(nullptr);
  board::Withdraw();
  UpdateEntry()(Obj(g_game), 16);
  lp::ResetLatchForTest();
}

// The first state the hook sees for an instance is its baseline, whatever state it last saw on another instance:
// a change is logged only between two states of the same instance.
void TheFirstStateOfAnInstanceIsABaseline() {
  static FakeGame fresh;  // an object the hook has never seen
  lp::ResetLatchForTest();
  gate::SetReady(true);
  board::Withdraw();
  SetState(g_game, 3);  // the hook last saw g_game in "logged in"
  UpdateEntry()(Obj(g_game), 16);
  const int before = CountLines(kStateLine);
  SetState(fresh, layout::kStateLoginFailed);
  UpdateEntry()(Obj(fresh), 16);  // its baseline: not logged, although the state seen last was 3
  QCHECK(CountLines(kStateLine) == before);
  SetState(fresh, layout::kStateLoggingIn);
  UpdateEntry()(Obj(fresh), 16);
  QCHECK(CountLines(kStateLine) == before + 1);
  QCHECK(Holds(LastLine(kStateLine), "\"from\":-94,\"from_name\":\"login_failed\",\"to\":2,\"to_name\":\"logging_in\""));
  board::Withdraw();
  UpdateEntry()(Obj(g_game), 16);
  lp::ResetLatchForTest();
}

// The reporter's table holds the hook's counters: after RegisterCounters() it has room for exactly the rest of its
// capacity, so a counter that was not registered shows as one slot too many.
void TheCountersAreAllInTheReporterTable() {
  sentinel::StopReporter();  // forgets the table
  QCHECK(lp::RegisterCounters());
  constexpr unsigned kFill = sentinel::kMaxReportCounters;
  static std::atomic<std::uint64_t> filler[kFill];
  unsigned room = 0;
  for (unsigned i = 0; i < kFill; ++i) room += sentinel::RegisterReportCounter("filler", &filler[i]) ? 1 : 0;
  QCHECK(room == sentinel::kMaxReportCounters - static_cast<unsigned>(lp::kCounterCount));
  sentinel::StopReporter();
}

// ---- the select after the fallback's login (#1391) ---------------------------------------------------------------
// A select that does nothing is followed by the fallback's BeginLogIn: the game logs in but the page stays on LOGGING
// IN. When the login the fallback started goes 2 (logging in) -> 3 (logged in) on the same NetGame instance, the same
// select is sent once more, with trigger after_fallback_login. The select, the 2 -> 3 change and the instance are the
// only conditions; no timer.

// Common setup: the select is the real one (sent through the fake send), BeginLogIn is the fake, the game object is linked.
void SetUpFallbackCase() {
  g_lines.clear();  // each case counts only its own lines
  lp::ResetLatchForTest();
  gate::SetReady(true);
  lp::SetClockForTest(&FakeClock);
  lp::SetBeginLogInForTest(&FakeBeginLogIn);
  lp::SetSendEventForTest(&FakeSend);
  lp::UseRetrySelectForTest();
  g_beginCalls = 0;
  g_sendCalls = 0;
  g_now = 1'000'000'000;
  board::Withdraw();
  LinkGameObject(g_game, &g_gameObject);
  SetSpaceOf(g_gameObject, &g_spaceMarker);
}

void TearDownFallbackCase() {
  LinkGameObject(g_game, nullptr);
  lp::SetSendEventForTest(nullptr);
  lp::SetRetryEventForTest(nullptr);
  lp::SetBeginLogInForTest(nullptr);
  lp::SetClockForTest(nullptr);
  board::Withdraw();
  UpdateEntry()(Obj(g_game), 16);
  lp::ResetLatchForTest();
}

// The notice's select is sent; 2.1 s later the game is still in "login failed", so the fallback runs BeginLogIn.
void NoticeWhoseSelectDidNothingFallsBack() {
  RestartFromNotice();  // the select (trigger after_sign_in) is sent
  g_now += 2'100'000'000;
  UpdateEntry()(Obj(g_game), 16);  // the fallback: BeginLogIn, which sets "logging in" in the fake
}

// Fallback, then the login is observed going 2 -> 3: the same select goes out once more, with the fallback's trigger.
void TheFallbackLoginReachingLoggedInSendsTheSelectOnceWithItsTrigger() {
  SetUpFallbackCase();
  NoticeWhoseSelectDidNothingFallsBack();
  QCHECK(g_beginCalls == 1);
  QCHECK(g_sendCalls == 1);  // the post-sign-in select only
  UpdateEntry()(Obj(g_game), 16);  // observes "logging in"
  QCHECK(g_sendCalls == 1);
  SetState(g_game, 3);  // logged in
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(g_sendCalls == 2);
  QCHECK(g_sendSpace == &g_spaceMarker);
  QCHECK(g_sendEventArg == sentinel::pinned::kRetrySelectEvent);
  QCHECK(g_sendKeyArg == sentinel::pinned::kLoginPageExitKey && g_sendActorArg == sentinel::pinned::kAllActors);
  QCHECK(g_sendFlagArg == 1);
  QCHECK(Holds(LastLine(kSelectLine), "\"trigger\":\"after_fallback_login\""));
  QCHECK(Holds(LastLine(kSelectLine), "\"outcome\":\"sent\""));
  QCHECK(CountLines("\"trigger\":\"after_fallback_login\"") == 1);
  QCHECK(CountLines("\"trigger\":\"after_sign_in\"") == 1);
  // A second 2 -> 3 of the same attempt sends nothing.
  SetState(g_game, 2);
  UpdateEntry()(Obj(g_game), 16);
  SetState(g_game, 3);
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(g_sendCalls == 2);
  QCHECK(CountLines("\"trigger\":\"after_fallback_login\"") == 1);
  TearDownFallbackCase();
}

// The select starts the login (the page script's BeginLogin node moves the game to "logging in"): no fallback, so the
// later 2 -> 3 sends nothing more.
void ASelectThatStartsTheLoginSendsNothingMoreWhenItReachesLoggedIn() {
  SetUpFallbackCase();
  const int fallbacks = CountLines(kFallbackLine);
  RestartFromNotice();  // the select, sent
  QCHECK(g_sendCalls == 1);
  SetState(g_game, 2);  // what the script's BeginLogin node does on the select
  UpdateEntry()(Obj(g_game), 16);
  SetState(g_game, 3);
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(g_sendCalls == 1);
  QCHECK(g_beginCalls == 0);
  QCHECK(CountLines(kFallbackLine) == fallbacks);
  QCHECK(CountLines("\"trigger\":\"after_fallback_login\"") == 0);
  TearDownFallbackCase();
}

// The fallback's login goes back to "login failed" before it logs in: that attempt is over, and a later 2 -> 3 is not
// the fallback's, so nothing is sent for it.
void AFallbackLoginThatGoesBackToLoginFailedSendsNothing() {
  SetUpFallbackCase();
  NoticeWhoseSelectDidNothingFallsBack();
  UpdateEntry()(Obj(g_game), 16);  // observes "logging in"
  SetState(g_game, layout::kStateLoginFailed);
  UpdateEntry()(Obj(g_game), 16);
  SetState(g_game, 2);  // a later login, not the fallback's
  UpdateEntry()(Obj(g_game), 16);
  SetState(g_game, 3);
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(g_sendCalls == 1);  // the post-sign-in select only
  QCHECK(CountLines("\"trigger\":\"after_fallback_login\"") == 0);
  TearDownFallbackCase();
}

// The fallback's login is not seen in "logging in" before it reaches "logged in": the change is -94 -> 3, not 2 -> 3,
// so nothing is sent.
void TheFallbackLoginMustBeSeenLoggingInBeforeItSendsForLoggedIn() {
  SetUpFallbackCase();
  NoticeWhoseSelectDidNothingFallsBack();  // BeginLogIn ran, but no Update has seen "logging in" yet
  SetState(g_game, 3);
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(g_sendCalls == 1);
  QCHECK(CountLines("\"trigger\":\"after_fallback_login\"") == 0);
  TearDownFallbackCase();
}

// Another NetGame instance going 2 -> 3 while the fallback is armed for g_game: not the instance the fallback started.
void AnotherNetGameReachingLoggedInSendsNothing() {
  SetUpFallbackCase();
  NoticeWhoseSelectDidNothingFallsBack();
  UpdateEntry()(Obj(g_game), 16);  // observes "logging in" on the fallback's instance
  SetState(g_other, 2);
  UpdateEntry()(Obj(g_other), 16);  // g_other's baseline
  SetState(g_other, 3);
  UpdateEntry()(Obj(g_other), 16);  // 2 -> 3 on another instance
  QCHECK(g_sendCalls == 1);
  QCHECK(CountLines("\"trigger\":\"after_fallback_login\"") == 0);
  TearDownFallbackCase();
}

// A fallback whose BeginLogIn is not resolved starts no login, so it arms nothing: a later 2 -> 3 is not its.
void AnUnresolvedBeginLogInArmsNothing() {
  SetUpFallbackCase();
  lp::SetBeginLogInForTest(nullptr);
  const lp::Counts before = lp::CurrentCounts();
  NoticeWhoseSelectDidNothingFallsBack();  // the select, then the fallback, which is counted as unavailable
  QCHECK(lp::CurrentCounts().login_restart_unavailable == before.login_restart_unavailable + 1);
  QCHECK(g_beginCalls == 0);
  SetState(g_game, 2);
  UpdateEntry()(Obj(g_game), 16);
  SetState(g_game, 3);
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(g_sendCalls == 1);  // the post-sign-in select only
  QCHECK(CountLines("\"trigger\":\"after_fallback_login\"") == 0);
  TearDownFallbackCase();
}

// The select cannot be sent (no RETRY seam), so the fallback starts the login at once; the login's 2 -> 3 then sends the
// select once, with the fallback's trigger, as after a select that had no effect.
void TheUnavailableSelectsFallbackAlsoSendsOnceAfterItsLogin() {
  SetUpFallbackCase();
  lp::SetRetryEventForTest(nullptr);
  RestartFromNotice();  // no select: the fallback runs now
  QCHECK(g_beginCalls == 1);
  QCHECK(g_sendCalls == 0);
  UpdateEntry()(Obj(g_game), 16);  // observes "logging in"
  QCHECK(g_sendCalls == 0);
  SetState(g_game, 3);
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(g_sendCalls == 1);
  QCHECK(Holds(LastLine(kSelectLine), "\"trigger\":\"after_fallback_login\""));
  QCHECK(CountLines("\"trigger\":\"after_fallback_login\"") == 1);
  TearDownFallbackCase();
}

// The fallback's BeginLogIn did not take the game out of "login failed": the first Update after it is not "logging in", so
// the attempt is over, and a later "logging in" -> "logged in" is not the fallback's.
void AFallbackThatDidNotStartTheLoginSendsNothingLater() {
  SetUpFallbackCase();
  NoticeWhoseSelectDidNothingFallsBack();       // BeginLogIn ran, and the fake moved the game to "logging in"
  SetState(g_game, layout::kStateLoginFailed);  // the test puts the game back: the login did not start
  UpdateEntry()(Obj(g_game), 16);
  SetState(g_game, 2);
  UpdateEntry()(Obj(g_game), 16);
  SetState(g_game, 3);
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(g_sendCalls == 1);  // the post-sign-in select only
  QCHECK(CountLines("\"trigger\":\"after_fallback_login\"") == 0);
  TearDownFallbackCase();
}

// A second fallback (a new notice) is seen "logging in" on its own: the first attempt's "logging in" does not count for it.
void ASecondFallbackMustBeSeenLoggingInOnItsOwn() {
  SetUpFallbackCase();
  NoticeWhoseSelectDidNothingFallsBack();       // the first fallback
  UpdateEntry()(Obj(g_game), 16);               // seen "logging in"
  SetState(g_game, layout::kStateLoginFailed);  // the first attempt ends
  UpdateEntry()(Obj(g_game), 16);
  RestartFromNotice();  // a new notice: its select
  g_now += 2'100'000'000;
  UpdateEntry()(Obj(g_game), 16);  // the second fallback: BeginLogIn, which the fake answers with "logging in"
  QCHECK(g_beginCalls == 2);
  SetState(g_game, 3);  // the second attempt's first observation is "logged in"
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(g_sendCalls == 2);  // the two notices' selects only
  QCHECK(CountLines("\"trigger\":\"after_fallback_login\"") == 0);
  TearDownFallbackCase();
}

// The test seam forgets an armed fallback: a case that starts after one does not inherit its login, so the next "logged in"
// of that instance sends nothing.
void SettingBeginLogInForgetsAnArmedFallback() {
  SetUpFallbackCase();
  NoticeWhoseSelectDidNothingFallsBack();  // the fallback runs, and is armed for g_game
  lp::SetBeginLogInForTest(&FakeBeginLogIn);  // the next case begins: the armed fallback is forgotten
  UpdateEntry()(Obj(g_game), 16);  // observes "logging in"
  SetState(g_game, 3);
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(g_sendCalls == 1);  // the post-sign-in select only
  QCHECK(CountLines("\"trigger\":\"after_fallback_login\"") == 0);
  TearDownFallbackCase();
}

// #1391: the counters are written once at process exit, every one of them. The reporter writes a counter only when it
// changes, so a short run would otherwise end without its final values. The expected values are read before the flush;
// nothing in the flush changes them.
void TheExitFlushWritesEachCounterOnce() {
  const lp::Counts c = lp::CurrentCounts();
  struct Expect {
    const char* name;
    std::uint64_t value;
  };
  const Expect want[] = {
      {"login_prompt_text_shown", c.shown},
      {"login_prompt_text_refreshed", c.refreshed},
      {"login_prompt_text_kept", c.kept},
      {"login_prompt_text_not_local", c.not_local},
      {"login_prompt_board_busy", c.busy},
      {"login_prompt_block_not_ours", c.not_ours},
      {"login_prompt_error_thunk_faults", static_cast<std::uint64_t>(lp::ErrorThunk::Faults())},
      {"login_prompt_update_thunk_faults", static_cast<std::uint64_t>(lp::UpdateThunk::Faults())},
      {"login_prompt_enable_thunk_faults", static_cast<std::uint64_t>(lp::EnablePageThunk::Faults())},
      {"login_prompt_error_page_dropped", c.error_page_dropped},
      {"login_prompt_logging_in_page_dropped", c.logging_in_page_dropped},
      {"login_prompt_page_passed_armed", c.page_passed_armed},
      {"login_prompt_error_resent", c.resent},
      {"login_prompt_error_resend_unavailable", c.resend_unavailable},
      {"login_prompt_login_restarted", c.login_restarted},
      {"login_prompt_login_restart_unavailable", c.login_restart_unavailable},
      {"login_prompt_select_sent", c.select_sent},
  };
  static_assert(sizeof(want) / sizeof(want[0]) == static_cast<std::size_t>(lp::kCounterCount),
                "one expectation per counter");
  g_lines.clear();
  lp::FlushCountersAtExit();
  for (const Expect& e : want) {
    char needle[192];
    std::snprintf(needle, sizeof(needle), "\"counter\":\"%s\",\"value\":%llu,\"why\":\"exit\"", e.name,
                  static_cast<unsigned long long>(e.value));
    QCHECK(CountLines(needle) == 1);
  }
  QCHECK(CountLines("\"why\":\"exit\"") == lp::kCounterCount);
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
  APromptPublishedInTheFailureFrameDoesNotSendASecondErrorEvent();
  NoErrorEventAfterTheGameLeftLoginFailed();
  AnUnresolvedErrorEventIsCountedAndNotCalled();
  NoErrorEventForAnInstanceThatIsNotFollowed();
  WhileThePromptIsOnScreenTheErrorPagesAreNotEnabled();
  TheLatchOutlivesLoginFailedAndDiesAtLoadingGlobal();
  AGenuineErrorTextPassesThrough();
  AGenuineErrorWrittenInTheSameFrameIsNotDropped();
  WithoutAPromptOnScreenNothingIsHeldBack();
  ANoticeWaitsForTheLoginToBeAbleToProceed();
  TheSignedInNoticeStartsTheLoginOnce();
  TheLoginIsNotStartedBeforeItMayProceedOrWhenTheGameMovedOn();
  AnUnresolvedBeginLogInIsCountedAndNotCalled();
  TheRestartSendsTheRetrySelectAloneAndFallsBackToBeginLogIn();
  ASelectThatStartsTheLoginIsNotFollowedByBeginLogIn();
  TheRetrySelectLogsWhatItSends();
  TheRetrySelectSaysWhyItWasNotSent();
  TheFallbackLogsItsDecisionTimeAndState();
  TheFallbackSaysWhyItCallsAndIsSilentWhenTheSelectStartsTheLogin();
  TheNetGameStateIsLoggedOncePerChange();
  TheSelectsKeyAndEventAreTheirSymbolNames();
  TheSelectNamesAnUnsetSelfAndSendsNothing();
  TheFallbackNamesTheSelectsOutcomeAndTheNextNoticeNamesNone();
  TheFallbackLoginReachingLoggedInSendsTheSelectOnceWithItsTrigger();
  ASelectThatStartsTheLoginSendsNothingMoreWhenItReachesLoggedIn();
  AFallbackLoginThatGoesBackToLoginFailedSendsNothing();
  TheFallbackLoginMustBeSeenLoggingInBeforeItSendsForLoggedIn();
  AnotherNetGameReachingLoggedInSendsNothing();
  AnUnresolvedBeginLogInArmsNothing();
  TheUnavailableSelectsFallbackAlsoSendsOnceAfterItsLogin();
  AFallbackThatDidNotStartTheLoginSendsNothingLater();
  ASecondFallbackMustBeSeenLoggingInOnItsOwn();
  SettingBeginLogInForgetsAnArmedFallback();
  TheFirstStateOfAnInstanceIsABaseline();
  TheCountersAreAllInTheReporterTable();
  ASkippedLoggingInPagePoisonsTheAttemptUntilItEnds();
  ALoggingInPageIsNotSkippedWhenTheGateTurnsReadyBeforeThePoison();
  TheHoldBitIsPublishedWithReadyAndReadsBackOnlyWhenNothingElseDecides();
  EnablesOnAnotherThreadDuringRewritesAreAllSkipped();
  EveryPageEnableIsLoggedOncePerSymbolPerSecond();
  PageEnterLinesNameTheSmokePagesFromTheEvidenceRows();
  PageEnterLogVolumeIsBounded();
  APageTableThatIsFullStillPassesEveryEnable();
  AWithdrawnBoardKeepsNoCode();
  TheBoardRefusesWhatTheGameCouldNotShow();
  ConcurrentReadsAreNeverTorn();
  TheExitFlushWritesEachCounterOnce();
  QCHECK(lp::ErrorThunk::Faults() == 0 && lp::UpdateThunk::Faults() == 0 && lp::EnablePageThunk::Faults() == 0);
  CounterRefusalIsLoudAndInstallReportsBothSlots();

  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "login_prompt_hook_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("login_prompt_hook_test: the prompt replaces only the game's own login-failure text and follows the board\n");
  return 0;
}
