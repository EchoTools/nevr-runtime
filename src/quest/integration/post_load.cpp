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

// The slots and their lock live in one function-local object that is never destroyed (a namespace-scope
// std::mutex registers an atexit destructor, which tools/check_quest_static_init.sh rejects).
struct State {
  std::mutex mutex;
  Slot login{nullptr, false, nullptr, "login"};
  Slot matchmaking{nullptr, false, nullptr, "matchmaking"};
};
State& S() {
  static State* const state = new State();
  return *state;
}
std::atomic<bool> g_pending{false};
std::atomic<std::uint64_t> g_calls{0};
std::atomic<std::uint64_t> g_attempts{0};
std::atomic<bool> g_sawPnsovr{false};
std::atomic<bool> g_sawMatchmaking{false};
std::atomic<std::uint64_t> g_matchmakingImages{0};
std::atomic<void*> g_lastMatchmakingHandle{nullptr};

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
  return (S().login.fn != nullptr && !S().login.settled) || (S().matchmaking.fn != nullptr && !S().matchmaking.settled);
}

}  // namespace

void SetPostLoadActions(const PostLoadActions& actions) {
  const std::lock_guard<std::mutex> lock(S().mutex);
  S().login = Slot{actions.login, false, nullptr, "login"};
  S().matchmaking = Slot{actions.matchmaking, false, nullptr, "matchmaking"};
  g_calls.store(0);
  g_sawPnsovr.store(false);
  g_sawMatchmaking.store(false);
  g_matchmakingImages.store(0);
  g_lastMatchmakingHandle.store(nullptr);
  g_attempts.store(0);
  g_pending.store(AnyUnsettled(), std::memory_order_release);
}

bool PostLoadPending() noexcept { return g_pending.load(std::memory_order_acquire); }

std::uint64_t MatchmakingImages() noexcept { return g_matchmakingImages.load(std::memory_order_acquire); }

PostLoadStats PostLoadStatsView() noexcept {
  PostLoadStats s;
  s.calls = g_calls.load();
  s.attempts = g_attempts.load();
  const std::lock_guard<std::mutex> lock(S().mutex);
  s.loginSettled = S().login.settled ? 1 : 0;
  s.matchmakingSettled = S().matchmaking.settled ? 1 : 0;
  return s;
}

// Runs after the game's dlopen returned a handle and before the game sees it; calls no game code.
NEVR_OUTSIDE_GAME_CALL void AfterDlopen(const char* name, void* handle) noexcept {
  if (handle == nullptr) return;
  g_calls.fetch_add(1, std::memory_order_relaxed);
  // Stage lines (stage_log.h): the game mapped the libraries the later installs need. Once each.
  if (name != nullptr) {
    if (std::strstr(name, "pnsradmatchmaking") != nullptr) {
      // A handle that differs from the last one is another mapping of the library (the game closed it and
      // opened it again): the redirect installed on the first one does not cover it.
      if (g_lastMatchmakingHandle.exchange(handle) != handle) {
        g_matchmakingImages.fetch_add(1, std::memory_order_release);
      }
      if (!g_sawMatchmaking.exchange(true)) {
        sentinel::LogFields(sentinel::LogLevel::kInfo, "libpnsradmatchmaking_loaded", {{"status", "ok"}});
      }
    } else if (std::strstr(name, "pnsovr") != nullptr) {
      if (!g_sawPnsovr.exchange(true)) {
        sentinel::LogFields(sentinel::LogLevel::kInfo, "libpnsovr_loaded", {{"status", "ok"}});
      }
    }
  }
  if (!g_pending.load(std::memory_order_acquire)) return;
  try {
    const std::lock_guard<std::mutex> lock(S().mutex);
    const char* module = Basename(name);
    RunSlot(S().login, module);
    RunSlot(S().matchmaking, module);
    g_pending.store(AnyUnsettled(), std::memory_order_release);
  } catch (const std::exception&) {
    // std::system_error from the lock: leave the game's dlopen result alone; the next call retries.
    sentinel::LogFields(sentinel::LogLevel::kError, "post_load", {{"status", "exception"}});
  }
}

}  // namespace nevr_quest::integration
