#include "runtime/lifecycle/arena_round_time.h"

#include <mutex>

namespace nevr_arena_round_time {

namespace {

std::mutex g_sessionGameTypeMutex;
std::optional<std::string> g_sessionGameType;

}  // namespace

void SetSessionGameType(std::string_view gameType) {
  std::lock_guard<std::mutex> lock(g_sessionGameTypeMutex);
  g_sessionGameType = std::string(gameType);
}

std::optional<std::string> SessionGameType() {
  std::lock_guard<std::mutex> lock(g_sessionGameTypeMutex);
  return g_sessionGameType;
}

RoundTimeDecision DecideRoundTimeDefault(const std::optional<std::string>& sessionGameType) {
  if (!sessionGameType) return {false, "no_session_type"};
  if (*sessionGameType != kPublicArenaGameType) return {false, "non_arena"};
  return {true, "applied"};
}

}  // namespace nevr_arena_round_time
