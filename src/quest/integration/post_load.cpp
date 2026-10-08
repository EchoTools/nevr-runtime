#include "quest/integration/post_load.h"

#include <atomic>
#include <cstring>
#include <exception>
#include <mutex>

#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/outside_game_call.h"

namespace nevr_quest::integration {

namespace {

struct Slot {
  ActionFn fn = nullptr;
  bool settled = false;
  const char* lastStatus = nullptr;
  const char* name = "";
};

std::mutex g_mutex;
Slot g_login{nullptr, false, nullptr, "login"};
Slot g_matchmaking{nullptr, false, nullptr, "matchmaking"};
std::atomic<bool> g_pending{false};
std::atomic<std::uint64_t> g_calls{0};
std::atomic<std::uint64_t> g_attempts{0};

const char* Basename(const char* path) {
  if (path == nullptr) return "(null)";
  const char* slash = std::strrchr(path, '/');
  return slash != nullptr ? slash + 1 : path;
}

void RunSlot(Slot& slot, const char* module) {
  if (slot.fn == nullptr || slot.settled) return;
  g_attempts.fetch_add(1, std::memory_order_relaxed);
  const ActionResult result = slot.fn();
  if (result.settle != Settle::kRetryLater) slot.settled = true;
  // One line per distinct outcome: a retry that keeps saying "module_not_loaded" is logged once.
  if (slot.lastStatus == nullptr || std::strcmp(slot.lastStatus, result.status) != 0 || slot.settled) {
    slot.lastStatus = result.status;
    sentinel::LogFields(
        result.settle == Settle::kGiveUp ? sentinel::LogLevel::kWarn : sentinel::LogLevel::kInfo, "post_load",
        {{"action", slot.name}, {"status", result.status},
         {"settle", result.settle == Settle::kDone ? "done" : (result.settle == Settle::kRetryLater ? "retry" : "give_up")},
         {"after_module", module}});
  }
}

bool AnyUnsettled() {
  return (g_login.fn != nullptr && !g_login.settled) || (g_matchmaking.fn != nullptr && !g_matchmaking.settled);
}

}  // namespace

void SetPostLoadActions(const PostLoadActions& actions) {
  const std::lock_guard<std::mutex> lock(g_mutex);
  g_login = Slot{actions.login, false, nullptr, "login"};
  g_matchmaking = Slot{actions.matchmaking, false, nullptr, "matchmaking"};
  g_calls.store(0);
  g_attempts.store(0);
  g_pending.store(AnyUnsettled(), std::memory_order_release);
}

bool PostLoadPending() noexcept { return g_pending.load(std::memory_order_acquire); }

PostLoadStats PostLoadStatsView() noexcept {
  PostLoadStats s;
  s.calls = g_calls.load();
  s.attempts = g_attempts.load();
  const std::lock_guard<std::mutex> lock(g_mutex);
  s.loginSettled = g_login.settled ? 1 : 0;
  s.matchmakingSettled = g_matchmaking.settled ? 1 : 0;
  return s;
}

// Runs after the game's dlopen returned a handle and before the game sees it; calls no game code.
NEVR_OUTSIDE_GAME_CALL void AfterDlopen(const char* name, void* handle) noexcept {
  if (handle == nullptr) return;
  g_calls.fetch_add(1, std::memory_order_relaxed);
  if (!g_pending.load(std::memory_order_acquire)) return;
  try {
    const std::lock_guard<std::mutex> lock(g_mutex);
    const char* module = Basename(name);
    RunSlot(g_login, module);
    RunSlot(g_matchmaking, module);
    g_pending.store(AnyUnsettled(), std::memory_order_release);
  } catch (const std::exception&) {
    // std::system_error from the lock: leave the game's dlopen result alone; the next call retries.
    sentinel::LogFields(sentinel::LogLevel::kError, "post_load", {{"status", "exception"}});
  }
}

}  // namespace nevr_quest::integration
