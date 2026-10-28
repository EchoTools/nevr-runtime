#include "login_title_hook.h"

#include <atomic>
#include <cstring>

#include "hook_install.h"
#include "hook_log.h"
#include "hook_report.h"
#include "quest/auth/prompt_board.h"

namespace nevr_quest::login_title {

namespace {

namespace board = nevr::quest_auth::prompt_board;

sentinel::GotHook g_hook;

std::atomic<std::uint64_t> g_replaced{0};
std::atomic<std::uint64_t> g_kept{0};
std::atomic<std::uint64_t> g_otherText{0};
std::atomic<bool> g_countersRegistered{false};

// string_t::string_t(char const*) in the login-failed page's script library. The title is copied into the game's own
// 64-byte buffer by the original, so the stack buffer below is gone before anything could keep it.
void StringHandler(StringThunk::Fn original, sentinel::pinned::StringTOpaque* self, const char* text) noexcept {
  if (text == nullptr || std::strcmp(text, kGameTitle) != 0) {
    g_otherText.fetch_add(1, std::memory_order_relaxed);
    original(self, text);
    return;
  }
  char title[board::kMaxTitleChars + 1];
  if (!board::ReadTitle(title, sizeof(title))) {
    g_kept.fetch_add(1, std::memory_order_relaxed);
    original(self, text);
    return;
  }
  g_replaced.fetch_add(1, std::memory_order_relaxed);
  original(self, title);
}

NEVR_HOOK_RECORD(kStringHook, StringThunk, &StringHandler);

}  // namespace

bool RegisterCounters() noexcept {
  struct Entry {
    const char* name;
    const std::atomic<std::uint64_t>* value;
    sentinel::ReportKind kind;
  };
  const Entry entries[kCounterCount] = {
      {"login_title_string_calls", &StringThunk::CallCounter(), sentinel::ReportKind::kCalls},
      {"login_title_replaced", &g_replaced, sentinel::ReportKind::kCalls},
      {"login_title_kept", &g_kept, sentinel::ReportKind::kCalls},
      {"login_title_other_text", &g_otherText, sentinel::ReportKind::kCalls},
      {"login_title_thunk_faults", &StringThunk::FaultCounter(), sentinel::ReportKind::kFaults},
  };
  int registered = 0;
  for (const Entry& e : entries) registered += sentinel::RegisterReportCounter(e.name, e.value, e.kind) ? 1 : 0;
  if (registered != kCounterCount) {
    sentinel::LogFields(sentinel::LogLevel::kError, "login_title_counters",
                        {{"result", "refused"}, {"registered", registered}, {"wanted", kCounterCount}});
    return false;
  }
  g_countersRegistered.store(true, std::memory_order_release);
  return true;
}

sentinel::GotStatus Install() noexcept {
  if (g_hook.installed()) return sentinel::GotStatus::kAlreadyInstalled;
  if (!g_countersRegistered.load(std::memory_order_acquire)) {
    sentinel::LogFields(sentinel::LogLevel::kError, "login_title_install",
                        {{"result", "skipped"}, {"why", "counters_refused"}});
    return sentinel::GotStatus::kNotInstalled;
  }
  // Asked before the backend, which logs a line for every refusal: the caller tries again after each dlopen until the
  // game maps the library, and that is not a result.
  const sentinel::GotTarget target = sentinel::pinned::LoginScriptStringCtor();
  sentinel::ElfImage image;
  if (!sentinel::FindLoadedImage(target.module, &image)) return sentinel::GotStatus::kModuleNotLoaded;
  StringThunk::Arm(kStringHook);
  const sentinel::GotStatus status = sentinel::InstallThunk<StringThunk>(g_hook, target);
  if (status != sentinel::GotStatus::kOk) StringThunk::Disarm();
  sentinel::LogFields(status == sentinel::GotStatus::kOk ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError,
                      "login_title_install",
                      {{"result", status == sentinel::GotStatus::kOk ? "installed" : "failed"},
                       {"status", sentinel::GotStatusName(status)}});
  return status;
}

void ArmForTest() noexcept { StringThunk::Arm(kStringHook); }

void ResetForTest() noexcept {
  StringThunk::Reset();
  g_replaced.store(0, std::memory_order_relaxed);
  g_kept.store(0, std::memory_order_relaxed);
  g_otherText.store(0, std::memory_order_relaxed);
  g_countersRegistered.store(false, std::memory_order_release);
}

Counts CurrentCounts() noexcept {
  return {StringThunk::Calls(), g_replaced.load(std::memory_order_relaxed), g_kept.load(std::memory_order_relaxed),
          g_otherText.load(std::memory_order_relaxed), StringThunk::Faults()};
}

}  // namespace nevr_quest::login_title
