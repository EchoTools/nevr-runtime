// Host test for the login-prompt hook (#239): the real handler (src/quest/sentinel/login_prompt_hook.cpp)
// and prompt board (src/quest/auth/prompt_board.cpp), built without exceptions as the sentinel builds
// them, driven through the thunk's entry with a stand-in for CR15NetGame::SetDelimitedErrorMessage.
// No GOT slot is touched: tests/quest TestHookFramesCarryNoPersonality and
// src/quest/tests/got_pinned_test.cpp cover the installed form.
//
// Built and run by `just test-quest-hooks`.

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "login_prompt_hook.h"
#include "quest/auth/prompt_board.h"
#include "quest/tests/test_check.h"

namespace {

namespace board = nevr::quest_auth::prompt_board;
namespace lp = nevr_quest::login_prompt;
using sentinel::pinned::CR15NetGameOpaque;

int g_game = 0;
std::string g_received;  // what the "game" stored
int g_calls = 0;

// Stands in for the game's SetDelimitedErrorMessage: records the message it was handed.
void FakeSetDelimitedErrorMessage(CR15NetGameOpaque* self, const char* message) {
  QCHECK(self == reinterpret_cast<CR15NetGameOpaque*>(&g_game));
  g_received = message != nullptr ? message : "(null)";
  ++g_calls;
}

lp::Thunk::Fn Entry() { return reinterpret_cast<lp::Thunk::Fn>(lp::Thunk::EntryAddress()); }
CR15NetGameOpaque* Game() { return reinterpret_cast<CR15NetGameOpaque*>(&g_game); }

constexpr char kGameMessage[] = "Log in request failed: One or more prerequisites are missing";
constexpr char kPrompt[] =
    "Sign in to play: on a phone or computer, open\n"
    "echovrce.com/login/device\n"
    "and enter the code ABCDEFGHI\n"
    "A new code appears here if this one expires.";

void GameMessagePassesWhenNothingIsPublished() {
  board::Withdraw();
  g_calls = 0;
  const std::uint64_t passed = lp::PassedCount();
  Entry()(Game(), kGameMessage);
  QCHECK(g_calls == 1);
  QCHECK(g_received == kGameMessage);
  QCHECK(lp::PassedCount() == passed + 1);
}

void PublishedPromptReplacesTheGameMessage() {
  QCHECK(board::Publish(kPrompt, std::strlen(kPrompt)));
  g_calls = 0;
  const std::uint64_t shown = lp::ShownCount();
  Entry()(Game(), kGameMessage);
  QCHECK(g_calls == 1);
  QCHECK(g_received == kPrompt);
  QCHECK(lp::ShownCount() == shown + 1);
  // Withdrawn: the game's own message is back.
  QCHECK(board::Withdraw());
  Entry()(Game(), kGameMessage);
  QCHECK(g_received == kGameMessage);
  QCHECK(!board::Withdraw());  // nothing left to withdraw
}

void ANewPromptReplacesTheOldOne() {
  QCHECK(board::Publish(kPrompt, std::strlen(kPrompt)));
  const std::string next = std::string(kPrompt).replace(std::string(kPrompt).find("ABCDEFGHI"), 9, "ZYXWVUTSR");
  QCHECK(board::Publish(next.data(), next.size()));
  Entry()(Game(), kGameMessage);
  QCHECK(g_received == next);
  board::Withdraw();
}

void TheBoardRefusesWhatTheGameCouldNotShow() {
  board::Withdraw();
  const std::uint64_t v = board::Version();
  const std::string tooLong(board::kCapacity + 1, 'x');
  QCHECK(!board::Publish(tooLong.data(), tooLong.size()));
  QCHECK(!board::Publish(nullptr, 3));
  QCHECK(!board::Publish("abc", 0));
  const char withNul[] = {'a', '\0', 'b'};
  QCHECK(!board::Publish(withNul, sizeof(withNul)));
  QCHECK(board::Version() == v);  // a refused publish leaves the board as it was
  const std::string full(board::kCapacity, 'y');
  QCHECK(board::Publish(full.data(), full.size()));
  char out[board::kCapacity + 1];
  QCHECK(board::Copy(out, sizeof(out)));
  QCHECK(std::string(out) == full);
  char small[16];
  QCHECK(!board::Copy(small, sizeof(small)));  // a buffer that cannot hold the prompt gets none of it
  QCHECK(small[0] == '\0');
  board::Withdraw();
  QCHECK(!board::Copy(out, sizeof(out)));
  QCHECK(out[0] == '\0');
}

// A reader racing a writer never sees a mix of two prompts: every copy it gets is one of them
// whole. Two prompts of the same length that differ in every byte make a torn read visible.
void ConcurrentReadsAreNeverTorn() {
  const std::string a(board::kCapacity, 'a');
  const std::string b(board::kCapacity, 'b');
  QCHECK(board::Publish(a.data(), a.size()));
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    for (int i = 0; i < 200000 && !stop.load(); ++i) {
      const std::string& s = (i % 2 == 0) ? b : a;
      board::Publish(s.data(), s.size());
    }
    stop.store(true);
  });
  int reads = 0;
  int torn = 0;
  char out[board::kCapacity + 1];
  while (!stop.load()) {
    if (!board::Copy(out, sizeof(out))) continue;  // gave up on a busy buffer: allowed
    ++reads;
    const std::string got(out);
    if (got != a && got != b) ++torn;
  }
  writer.join();
  std::printf("login_prompt_hook_test: %d whole reads during concurrent publishes, %d torn\n", reads, torn);
  QCHECK(torn == 0);
  QCHECK(reads > 0);
  board::Withdraw();
}

}  // namespace

int main() {
  lp::Thunk::Reset();
  *lp::Thunk::OriginalOut() = reinterpret_cast<void*>(&FakeSetDelimitedErrorMessage);
  lp::ArmForTest();

  GameMessagePassesWhenNothingIsPublished();
  PublishedPromptReplacesTheGameMessage();
  ANewPromptReplacesTheOldOne();
  TheBoardRefusesWhatTheGameCouldNotShow();
  ConcurrentReadsAreNeverTorn();
  QCHECK(lp::Thunk::Faults() == 0);

  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "login_prompt_hook_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("login_prompt_hook_test: the prompt replaces the game's login error text only while published\n");
  return 0;
}
