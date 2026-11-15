#include "runtime/lifecycle/arena_round_time.h"

#include <cstdint>
#include <mutex>
#include <utility>

namespace nevr_arena_round_time {

namespace {

std::mutex g_sessionGameTypeMutex;
std::optional<std::string> g_sessionGameType;
std::uint64_t g_sessionGeneration = 0;   // #2242: counts the stores, so a watcher sees each new session

}  // namespace

std::optional<std::string> SetSessionGameType(std::string_view gameType) {
  std::lock_guard<std::mutex> lock(g_sessionGameTypeMutex);
  std::optional<std::string> previous = std::move(g_sessionGameType);
  g_sessionGameType = std::string(gameType);
  ++g_sessionGeneration;
  return previous;
}

std::optional<std::string> SessionGameType() {
  std::lock_guard<std::mutex> lock(g_sessionGameTypeMutex);
  return g_sessionGameType;
}

std::uint64_t SessionGeneration() {
  std::lock_guard<std::mutex> lock(g_sessionGameTypeMutex);
  return g_sessionGeneration;
}

#ifdef NEVR_ARENA_ROUND_TIME_TESTING
void ResetForTest() {
  std::lock_guard<std::mutex> lock(g_sessionGameTypeMutex);
  g_sessionGameType.reset();
  g_sessionGeneration = 0;
}
#endif

}  // namespace nevr_arena_round_time
