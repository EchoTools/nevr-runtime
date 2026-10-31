#include "runtime/lifecycle/arena_round_time.h"

#include <atomic>
#include <mutex>
#include <utility>

namespace nevr_arena_round_time {

namespace {

std::mutex g_sessionGameTypeMutex;
std::optional<std::string> g_sessionGameType;
std::atomic<std::uint32_t> g_readsWithoutType{0};

}  // namespace

std::optional<std::string> SetSessionGameType(std::string_view gameType) {
  std::lock_guard<std::mutex> lock(g_sessionGameTypeMutex);
  std::optional<std::string> previous = std::move(g_sessionGameType);
  g_sessionGameType = std::string(gameType);
  return previous;
}

std::optional<std::string> SessionGameType() {
  std::lock_guard<std::mutex> lock(g_sessionGameTypeMutex);
  return g_sessionGameType;
}

void NoteReadWithoutType() { g_readsWithoutType.fetch_add(1, std::memory_order_relaxed); }

std::uint32_t TakeReadsWithoutType() { return g_readsWithoutType.exchange(0, std::memory_order_relaxed); }

RoundTimeDecision DecideRoundTimeDefault(const std::optional<std::string>& sessionGameType) {
  if (!sessionGameType) return {false, "no_session_type"};
  if (*sessionGameType != kPublicArenaGameType) return {false, "non_arena"};
  return {true, "applied"};
}

#ifdef NEVR_ARENA_ROUND_TIME_TESTING
void ResetForTest() {
  std::lock_guard<std::mutex> lock(g_sessionGameTypeMutex);
  g_sessionGameType.reset();
  g_readsWithoutType.store(0, std::memory_order_relaxed);
}
#endif

}  // namespace nevr_arena_round_time
