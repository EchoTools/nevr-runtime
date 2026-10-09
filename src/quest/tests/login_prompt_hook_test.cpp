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
#include "quest/tests/test_check.h"

namespace {

namespace board = nevr::quest_auth::prompt_board;
namespace lp = nevr_quest::login_prompt;
namespace layout = sentinel::pinned::game_layout;
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

constexpr char kLocal[] = "Log in request failed: One or more prerequisites are missing";
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

void ALocalFailureWithNothingPublishedKeepsTheGameText() {
  board::Withdraw();
  SetState(g_game, layout::kStateLoggingIn);
  const lp::Counts before = lp::CurrentCounts();
  ErrorEntry()(Obj(g_game), kLocal);
  QCHECK(g_gameReceived == kLocal);
  QCHECK(Line(g_game, 0) == kLocal);
  QCHECK(BlockOf(g_game)[0] == 0);
  QCHECK(lp::CurrentCounts().kept == before.kept + 1);
}

void ALocalFailureWhileLoggingInShowsThePromptAndTheGameNeverSeesTheCode() {
  Publish(Prompt("CODE1-ABCD"));
  SetState(g_game, layout::kStateLoggingIn);
  const lp::Counts before = lp::CurrentCounts();
  ErrorEntry()(Obj(g_game), kLocal);
  QCHECK(g_gameReceived == kLocal);  // the game stored and logged its own message, not the prompt
  QCHECK(BlockOf(g_game)[0] == 1);
  QCHECK(Line(g_game, 0) == "Sign in to play: on a phone or computer, open");
  QCHECK(Line(g_game, 1) == "echo.test/login/device");
  QCHECK(Line(g_game, 2) == "and enter the code CODE1-ABCD");
  QCHECK(Line(g_game, 3) == "Each code lasts 5 minutes, then a new one is issued.");
  QCHECK(lp::CurrentCounts().shown == before.shown + 1);
}

void TheScreenFollowsTheBoardWhileTheGameStaysInLoginFailed() {
  // Continues from the previous case: the prompt for CODE1 is on screen.
  SetState(g_game, layout::kStateLoginFailed);
  g_updateCalls = 0;
  const lp::Counts before = lp::CurrentCounts();
  UpdateEntry()(Obj(g_game), 16);  // nothing changed
  QCHECK(g_updateCalls == 1);
  QCHECK(lp::CurrentCounts().refreshed == before.refreshed);
  Publish(Prompt("CODE2-WXYZ"));  // the code ran out and a new one was issued
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(Line(g_game, 2) == "and enter the code CODE2-WXYZ");
  QCHECK(lp::CurrentCounts().refreshed == before.refreshed + 1);
  UpdateEntry()(Obj(g_other), 16);  // another object is left alone
  QCHECK(lp::CurrentCounts().refreshed == before.refreshed + 1);
  Publish("Signed in to EchoVRCE.\nThe game's next login attempt uses this sign-in.", board::Mode::kNotice);
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(Line(g_game, 0) == "Signed in to EchoVRCE.");
  QCHECK(Line(g_game, 1) == "The game's next login attempt uses this sign-in.");
  QCHECK(Line(g_game, 2).empty() && Line(g_game, 3).empty());
  board::Withdraw();  // stopped: the game's own message comes back
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(BlockOf(g_game)[0] == 0);
  QCHECK(Line(g_game, 0) == kLocal);
  QCHECK(lp::CurrentCounts().refreshed == before.refreshed + 3);
  QCHECK(g_updateCalls == 5);  // the game's Update ran every time
  // The game leaves "login failed": the object is no longer followed.
  SetState(g_game, 0);
  UpdateEntry()(Obj(g_game), 16);
  Publish(Prompt("CODE3-QQQQ"));
  SetState(g_game, layout::kStateLoginFailed);
  UpdateEntry()(Obj(g_game), 16);
  QCHECK(Line(g_game, 0) == kLocal);
  QCHECK(lp::CurrentCounts().refreshed == before.refreshed + 3);
  board::Withdraw();
}

void ANoticeIsNotShownForANewFailure() {
  Publish("Signed in to EchoVRCE.\nThe game's next login attempt uses this sign-in.", board::Mode::kNotice);
  SetState(g_game, layout::kStateLoggingIn);
  const lp::Counts before = lp::CurrentCounts();
  ErrorEntry()(Obj(g_game), kLocal);
  QCHECK(Line(g_game, 0) == kLocal);
  QCHECK(lp::CurrentCounts().kept == before.kept + 1);
  board::Withdraw();
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
  QCHECK(lp::CurrentCounts().layout_mismatch == before.layout_mismatch + 1);
  board::Withdraw();
}

void ABusyBoardIsCountedAndTheGameTextKept() {
  Publish(Prompt("CODE6-TTTT"));
  SetState(g_game, layout::kStateLoggingIn);
  const lp::Counts before = lp::CurrentCounts();
  board::BeginWriteForTest();
  ErrorEntry()(Obj(g_game), kLocal);
  board::EndWriteForTest();
  QCHECK(Line(g_game, 0) == kLocal);
  QCHECK(lp::CurrentCounts().busy == before.busy + 1);
  QCHECK(lp::CurrentCounts().kept == before.kept);  // busy is not "nothing published"
  board::Withdraw();
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
  // A nearly full reporter table (two slots left, the hook needs eight): the counters are refused,
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
}

}  // namespace

int main() {
  sentinel::SetLogSink(&CaptureSink);
  lp::ErrorThunk::Reset();
  lp::UpdateThunk::Reset();
  *lp::ErrorThunk::OriginalOut() = reinterpret_cast<void*>(&FakeSetDelimitedErrorMessage);
  *lp::UpdateThunk::OriginalOut() = reinterpret_cast<void*>(&FakeUpdate);
  lp::ArmForTest();

  TheGameKnowsWhichFailuresAreItsOwn();
  ALocalFailureWithNothingPublishedKeepsTheGameText();
  ALocalFailureWhileLoggingInShowsThePromptAndTheGameNeverSeesTheCode();
  TheScreenFollowsTheBoardWhileTheGameStaysInLoginFailed();
  ANoticeIsNotShownForANewFailure();
  ServerMessagesAndLoggedInRemovalsAreNeverReplaced();
  ABlockThatDoesNotHoldTheGameMessageIsNotWritten();
  ABusyBoardIsCountedAndTheGameTextKept();
  TheBoardRefusesWhatTheGameCouldNotShow();
  ConcurrentReadsAreNeverTorn();
  QCHECK(lp::ErrorThunk::Faults() == 0 && lp::UpdateThunk::Faults() == 0);
  CounterRefusalIsLoudAndInstallReportsBothSlots();

  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "login_prompt_hook_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("login_prompt_hook_test: the prompt replaces only the game's own login-failure text and follows the board\n");
  return 0;
}
