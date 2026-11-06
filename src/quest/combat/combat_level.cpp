#include "quest/combat/combat_level.h"

#include "quest/combat/combat_level_targets.h"
#include "quest/combat/safe_read.h"

namespace nevr_quest::combat::level {

namespace {

constexpr std::uint64_t kCombatLevels[4] = {0x43e2da7914642604ULL, 0x42670f2bed45703cULL, 0xdf5ca7b7dfa383d4ULL, 0x43e2da7a0c623a19ULL};
constexpr std::uint64_t kCombatLobby = 0xcb9977f7fc2b4526ULL;  // mpl_lobby_b_combat
constexpr std::uint64_t kEchoArenaLevels[4] = {0x576ed3f8428ebc4bULL, 0x7691d78d414c5701ULL, 0x576ed3f8428ebc48ULL, 0x3f20b99c97955721ULL};
namespace pin = targets;
constexpr std::uintptr_t kTables[2][2] = {{pin::kTable0Rows, pin::kTable0Count}, {pin::kTable1Rows, pin::kTable1Count}};
constexpr std::uint64_t kMaxGamespaces = 64;
constexpr std::uintptr_t kRowGamespace = 8;
constexpr std::uintptr_t kGamespaceOwner = pin::kGamespaceOwnerOffset;
constexpr std::uintptr_t kGamespaceLevel = pin::kGamespaceLevelOffset;

template <std::size_t N>
bool In(std::uint64_t level, const std::uint64_t (&set)[N]) noexcept {
  for (const std::uint64_t v : set) {
    if (level == v) return true;
  }
  return false;
}

}  // namespace

#if defined(NEVR_COMBAT_READ_COUNTERS)
namespace {
std::atomic<std::uint64_t> g_scanCalls{0};
}
#endif

bool IsCombatMap(std::uint64_t level) noexcept { return In(level, kCombatLevels); }
bool IsCombatLobby(std::uint64_t level) noexcept { return level == kCombatLobby; }
bool IsEchoArenaLevel(std::uint64_t level) noexcept { return In(level, kEchoArenaLevels); }

#if defined(NEVR_COMBAT_READ_COUNTERS)
std::uint64_t ScanCallsForTest() noexcept { return g_scanCalls.load(std::memory_order_relaxed); }
void ResetScanCallsForTest() noexcept { g_scanCalls.store(0, std::memory_order_relaxed); }
#endif

Loaded Scan(std::uintptr_t game) noexcept {
#if defined(NEVR_COMBAT_READ_COUNTERS)
  g_scanCalls.fetch_add(1, std::memory_order_relaxed);
#endif
  Loaded out;
  for (const auto& table : kTables) {
    bool ok = true;
    const auto rows = SafeRead<std::uintptr_t>(game + table[0], &ok);
    const auto count = SafeRead<std::uint64_t>(game + table[1], &ok);
    if (!ok || rows == 0 || count > kMaxGamespaces) continue;
    for (std::uint64_t i = 0; i < count; ++i) {
      bool rowOk = true;
      const auto gamespace = SafeRead<std::uintptr_t>(rows + i * 16 + kRowGamespace, &rowOk);
      if (!rowOk || gamespace == 0) continue;
      if (SafeRead<std::uintptr_t>(gamespace + kGamespaceOwner, &rowOk) != game || !rowOk) continue;
      const auto current = SafeRead<std::uint64_t>(gamespace + kGamespaceLevel, &rowOk);
      if (!rowOk) continue;
      if (IsEchoArenaLevel(current) && !out.echoArena) {
        out.echoArena = true;
        out.echoArenaLevel = current;
      } else if (IsCombatMap(current) && !out.combatMap) {
        out.combatMap = true;
        out.combatMapLevel = current;
      } else if (IsCombatLobby(current) && !out.combatLobby) {
        out.combatLobby = true;
        out.combatLobbyLevel = current;
      }
    }
  }
  return out;
}

Kind Classify(std::uintptr_t game, bool lobby, std::uint64_t* level) noexcept {
  const Loaded loaded = Scan(game);
  if (loaded.echoArena) {  // an Echo Arena level wins, whatever else is loaded and whether or not the lobby rule is on
    *level = loaded.echoArenaLevel;
    return Kind::kEchoArena;
  }
  if (loaded.combatMap) {
    *level = loaded.combatMapLevel;
    return Kind::kCombatOrLobby;
  }
  if (lobby && loaded.combatLobby) {
    *level = loaded.combatLobbyLevel;
    return Kind::kCombatOrLobby;
  }
  *level = 0;
  return Kind::kNone;
}

bool CombatMapLoaded(std::uintptr_t game, std::uint64_t* level) noexcept {
  const Loaded loaded = Scan(game);
  *level = loaded.combatMap ? loaded.combatMapLevel : 0;
  return loaded.combatMap;
}

}  // namespace nevr_quest::combat::level
