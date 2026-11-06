#include "quest/combat/combat_level.h"

#include <atomic>

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
constexpr std::uint64_t kLevelNotStored = ~0ULL;  // CncaGameSpace's constructor leaves the level -1 until LoadLevel stores it (0x17e54ec)
constexpr std::uintptr_t kRowGamespace = 8;
constexpr std::uintptr_t kGamespaceOwner = pin::kGamespaceOwnerOffset;
constexpr std::uintptr_t kGamespaceLevel = pin::kGamespaceLevelOffset;

struct NamedLevel {
  std::uint64_t id;
  const char* name;
};
constexpr NamedLevel kNames[] = {
    {0x43e2da7914642604ULL, "mpl_combat_dyson"}, {0x42670f2bed45703cULL, "mpl_combat_combustion"}, {0xdf5ca7b7dfa383d4ULL, "mpl_combat_fission"},
    {0x43e2da7a0c623a19ULL, "mpl_combat_gauss"}, {0xcb9977f7fc2b4526ULL, "mpl_lobby_b_combat"},      {0x576ed3f8428ebc4bULL, "mpl_arena_a"},
    {0x7691d78d414c5701ULL, "mpl_arena_a_lowspec"}, {0x576ed3f8428ebc48ULL, "mpl_arena_b"},         {0x3f20b99c97955721ULL, "mpl_tutorial_a"},
    {0x5763ceff4e96bc48ULL, "mpl_lobby_b"},      {0xd09afd15b1c75c04ULL, "mpl_lobby_b2"},           {0x05361c73bb73db19ULL, "mpl_lobby_b2_lowspec"},
    {0x6daa00a6d33d44b7ULL, "mpl_lobby_b_arena"}, {0xcb9967f7f220453eULL, "mpl_lobby_b_social"},
};

template <std::size_t N>
bool In(std::uint64_t level, const std::uint64_t (&set)[N]) noexcept {
  for (const std::uint64_t v : set) {
    if (level == v) return true;
  }
  return false;
}

}  // namespace

bool IsCombatMap(std::uint64_t level) noexcept { return In(level, kCombatLevels); }
bool IsCombatLobby(std::uint64_t level) noexcept { return level == kCombatLobby; }
bool IsEchoArenaLevel(std::uint64_t level) noexcept { return In(level, kEchoArenaLevels); }

const char* LevelName(std::uint64_t level) noexcept {
  for (const NamedLevel& named : kNames) {
    if (named.id == level) return named.name;
  }
  return nullptr;
}

namespace {

std::atomic<void (*)()> g_seam{nullptr};
std::atomic<std::uint32_t> g_scans{0};

void Between() noexcept {
  if (const auto seam = g_seam.load(std::memory_order_acquire)) seam();
}

void Note(Loaded& out, std::uint64_t current) noexcept {
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

// The gamespaces still being loaded: the init fibers' gamespaces, whose level is stored before their resources are built.
void ScanInFlight(Loaded& out, std::uintptr_t game) noexcept {
  bool ok = true;
  const auto rows = SafeRead<std::uintptr_t>(game + pin::kFiberRows, &ok);
  const auto count = SafeRead<std::uint64_t>(game + pin::kFiberCount, &ok);
  if (!ok || rows == 0 || count > kMaxGamespaces) return;
  for (std::uint64_t i = 0; i < count; ++i) {
    bool rowOk = true;
    const auto fiber = SafeRead<std::uintptr_t>(rows + i * sizeof(std::uintptr_t), &rowOk);
    if (!rowOk || fiber == 0) continue;
    const auto gamespace = SafeRead<std::uintptr_t>(fiber + pin::kFiberGamespace, &rowOk);
    if (!rowOk || gamespace == 0) continue;  // the fiber table is this game's own, so there is no owner to check
    const auto current = SafeRead<std::uint64_t>(gamespace + kGamespaceLevel, &rowOk);
    if (!rowOk) continue;
    if (current == kLevelNotStored) continue;  // just started: the gamespace's level is still the constructor's -1, so it is loading nothing yet
    ++out.loading;
    if (IsCombatMap(current) && !out.loadingCombatMap) {
      out.loadingCombatMap = true;
      out.loadingCombatMapLevel = current;
    } else if (IsCombatLobby(current) && !out.loadingCombatLobby) {
      out.loadingCombatLobby = true;
      out.loadingCombatLobbyLevel = current;
    }
    Note(out, current);
  }
}

void ScanLoaded(Loaded& out, std::uintptr_t game) noexcept {
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
      Note(out, current);
    }
  }
}

}  // namespace

void SetScanSeamForTest(void (*seam)()) noexcept { g_seam.store(seam, std::memory_order_release); }

std::uint32_t ScanCalls() noexcept { return g_scans.load(std::memory_order_relaxed); }

Loaded Scan(std::uintptr_t game, bool inFlight) noexcept {
  g_scans.fetch_add(1, std::memory_order_relaxed);
  Loaded out;
  if (inFlight) ScanInFlight(out, game);
  Between();
  ScanLoaded(out, game);
  return out;
}

Kind Classify(std::uintptr_t game, bool lobby, std::uint64_t* level, bool inFlight) noexcept {
  const Loaded loaded = Scan(game, inFlight);
  if (loaded.echoArena) {  // an Echo Arena level wins, whatever else is loaded and whether or not the lobby rule is on
    *level = loaded.echoArenaLevel;
    return Kind::kEchoArena;
  }
  if (inFlight && loaded.loading > 0) {  // what is loading decides: the resources being built are its own
    if (loaded.loadingCombatMap) {
      *level = loaded.loadingCombatMapLevel;
      return Kind::kCombatOrLobby;
    }
    if (lobby && loaded.loadingCombatLobby) {
      *level = loaded.loadingCombatLobbyLevel;
      return Kind::kCombatOrLobby;
    }
    *level = 0;
    return Kind::kNone;
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
