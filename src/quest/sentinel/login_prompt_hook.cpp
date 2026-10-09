#include "login_prompt_hook.h"

#include <atomic>
#include <cstdint>

#include "hook_install.h"
#include "hook_report.h"
#include "quest/auth/prompt_board.h"

namespace nevr_quest::login_prompt {

namespace {

namespace board = nevr::quest_auth::prompt_board;
using sentinel::pinned::CR15NetGameOpaque;

sentinel::GotHook g_hook;
std::atomic<std::uint64_t> g_shown{0};
std::atomic<std::uint64_t> g_passed{0};

// Runs on the game thread inside a login error callback. A stack copy of the board (no lock,
// no allocation, nothing with a destructor), then the original with either the prompt or the
// game's own message. SetDelimitedErrorMessage copies what it keeps, so the stack buffer only
// has to live for the call.
void HookedSetDelimitedErrorMessage(Thunk::Fn original, CR15NetGameOpaque* self, const char* message) noexcept {
  char prompt[board::kCapacity + 1];
  if (board::Copy(prompt, sizeof(prompt))) {
    g_shown.fetch_add(1, std::memory_order_relaxed);
    original(self, prompt);
    return;
  }
  g_passed.fetch_add(1, std::memory_order_relaxed);
  original(self, message);
}

NEVR_HOOK_RECORD(kLoginPromptHook, Thunk, &HookedSetDelimitedErrorMessage);

}  // namespace

bool RegisterCounters() noexcept {
  bool ok = sentinel::RegisterReportCounter("login_prompt_text_shown", &g_shown);
  ok = sentinel::RegisterReportCounter("login_prompt_text_passed", &g_passed) && ok;
  ok = sentinel::RegisterReportCounter("login_prompt_thunk_faults", &Thunk::FaultCounter(),
                                       sentinel::ReportKind::kFaults) &&
       ok;
  return ok;
}

bool Install() noexcept {
  Thunk::Arm(kLoginPromptHook);
  return sentinel::InstallThunk<Thunk>(g_hook, sentinel::pinned::LibR15SetDelimitedErrorMessage()) ==
         sentinel::GotStatus::kOk;
}

void ArmForTest() noexcept { Thunk::Arm(kLoginPromptHook); }

std::uint64_t ShownCount() noexcept { return g_shown.load(std::memory_order_relaxed); }
std::uint64_t PassedCount() noexcept { return g_passed.load(std::memory_order_relaxed); }

}  // namespace nevr_quest::login_prompt
