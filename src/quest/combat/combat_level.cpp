#include "quest/combat/combat_level.h"

#include "quest/combat/safe_read.h"

namespace nevr_quest::combat::level {

namespace {

constexpr std::uint64_t kCombatLevels[4] = {0x43e2da7914642604ULL, 0x42670f2bed45703cULL, 0xdf5ca7b7dfa383d4ULL, 0x43e2da7a0c623a19ULL};
constexpr std::uint64_t kLobbyLevels[6] = {0x5763ceff4e96bc48ULL, 0xd09afd15b1c75c04ULL, 0x05361c73bb73db19ULL,
                                           0xcb9977f7fc2b4526ULL, 0x6daa00a6d33d44b7ULL, 0xcb9967f7f220453eULL};
constexpr std::uint64_t kEchoArenaLevels[4] = {0x576ed3f8428ebc4bULL, 0x7691d78d414c5701ULL, 0x576ed3f8428ebc48ULL, 0x3f20b99c97955721ULL};
constexpr std::uintptr_t kTables[2][2] = {{0x1f48, 0x1f78}, {0x1f88, 0x1fb8}};
constexpr std::uint64_t kMaxGamespaces = 64;
constexpr std::uintptr_t kRowGamespace = 8;
constexpr std::uintptr_t kGamespaceOwner = 0x8;
constexpr std::uintptr_t kGamespaceLevel = 0x58;

template <std::size_t N>
bool In(std::uint64_t level, const std::uint64_t (&set)[N]) noexcept {
  for (const std::uint64_t v : set) {
    if (level == v) return true;
  }
  return false;
}

}  // namespace

bool IsCombatMap(std::uint64_t level) noexcept { return In(level, kCombatLevels); }
bool IsLobbyLevel(std::uint64_t level) noexcept { return In(level, kLobbyLevels); }
bool IsEchoArenaLevel(std::uint64_t level) noexcept { return In(level, kEchoArenaLevels); }

Kind Classify(std::uintptr_t game, bool lobby, std::uint64_t* level) noexcept {
  std::uint64_t hit = 0;
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
      if (lobby && IsEchoArenaLevel(current)) {  // an Echo Arena level wins
        *level = current;
        return Kind::kEchoArena;
      }
      if (hit == 0 && (IsCombatMap(current) || (lobby && IsLobbyLevel(current)))) hit = current;
    }
  }
  *level = hit;
  return hit != 0 ? Kind::kCombatOrLobby : Kind::kNone;
}

bool CombatMapLoaded(std::uintptr_t game, std::uint64_t* level) noexcept {
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
      if (rowOk && IsCombatMap(current)) {
        *level = current;
        return true;
      }
    }
  }
  *level = 0;
  return false;
}

}  // namespace nevr_quest::combat::level
