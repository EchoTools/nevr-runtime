#include "runtime/content/config_alias_hook.h"

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <vector>

#include "core/config_alias.h"
#include "core/game_space_table.h"
#include "core/hex_bytes.h"
#include "core/logging.h"
#include "runtime/hook/addresses.h"
#include "runtime/lifecycle/config.h"
#include "runtime/lifecycle/crash_recovery.h"
#include "runtime/lifecycle/readable_memory.h"
#include "runtime/lifecycle/return_to_lobby_queue.h"

#ifndef NEVR_CONFIG_ALIAS_HOOK_TESTING
#include "nevr/pc/nevr_common.h"
#include "runtime/hook/patching.h"
#endif

namespace nevr_config_alias_hook {

namespace {

namespace addr = nevr_patch_addresses;

// A level table entry is two 8-byte words: the level symbol, then the level resource (core/game_space_table.h, which also
// says why the bound on a table's count is the table's own capacity).
constexpr std::uint64_t kEntryWords = nevr_game_space_table::kEntryWords;
constexpr std::uint64_t kEntryResourceWord = nevr_game_space_table::kResourceWord;
// splitmix64 constants (the finaliser Mix folds with) and its shifts.
constexpr std::uint64_t kMixIncrement = 0x9e3779b97f4a7c15ull;
constexpr std::uint64_t kMixMultiplierA = 0xbf58476d1ce4e5b9ull;
constexpr std::uint64_t kMixMultiplierB = 0x94d049bb133111ebull;
constexpr unsigned kMixShiftA = 30;
constexpr unsigned kMixShiftB = 27;
constexpr unsigned kMixShiftC = 31;

LookupFn g_original = nullptr;
LevelResourceFn g_enableOriginal = nullptr;
LevelResourceFn g_disableOriginal = nullptr;
RefreshFn g_refresh = nullptr;
std::atomic<std::uint64_t (*)()> g_mainLevelSource{nullptr};
std::atomic<bool> g_refreshEnabled{false};
std::atomic<bool> g_aliasMissLogged{false};
std::atomic<bool> g_levelEventFailLogged{false};

// The level detours may run off the game thread.
std::mutex g_instancesMutex;
nevr_config_alias::LevelInstances g_instances;
std::atomic<std::uint64_t> g_levelEvents{0};
std::atomic<std::size_t> g_instanceCount{0};

// Game thread only.
std::uint64_t g_lastStamp = 0;
bool g_haveStamp = false;
bool g_inReconcile = false;
bool g_reconcileFailLogged = false;
bool g_fromEventsLogged = false;
std::uint64_t g_arenaAlias = nevr_config_alias::kArenaRulesKey;
bool g_refreshDue = false;

struct LevelTable {
  const std::uint64_t* entries = nullptr;  // {level symbol, level resource} pairs
  std::uint64_t count = 0;
};

struct GameSpaces {
  bool readable = false;
  LevelTable enabled;   // game object +0x1F50
  LevelTable disabled;  // game object +0x1F90: loaded, not enabled
};

bool ReadLevelTable(const unsigned char* game, std::uintptr_t data_offset, std::uintptr_t count_offset,
                    LevelTable* out) {
  LevelTable table;
  std::memcpy(&table.entries, game + data_offset, sizeof(table.entries));
  std::memcpy(&table.count, game + count_offset, sizeof(table.count));
  std::uint64_t capacity = 0;
  std::memcpy(&capacity, game + count_offset - nevr_game_space_table::kCapacityBytesBeforeCount, sizeof(capacity));
  if (!nevr_game_space_table::Readable(table.entries, table.count, capacity)) return false;
  if (table.count == 0) {
    *out = LevelTable{};
    return true;
  }
  *out = table;
  return true;
}

const void* NetGame() { return nevr_return_to_lobby_queue::NetGameFromContext(g_pGame); }

GameSpaces ReadGameSpaces(const void* net_game) {
  GameSpaces spaces;
  if (net_game == nullptr) return spaces;
  const auto* game =
      static_cast<const unsigned char*>(nevr_return_to_lobby_queue::ReadPointer(net_game, addr::NETGAME_GAME_OFFSET));
  if (game == nullptr) return spaces;
  spaces.readable =
      ReadLevelTable(game, addr::GAME_ENABLED_SPACE_TABLE_OFFSET, addr::GAME_ENABLED_SPACE_TABLE_COUNT_OFFSET,
                     &spaces.enabled) &&
      ReadLevelTable(game, addr::GAME_SPACE_TABLE_OFFSET, addr::GAME_SPACE_TABLE_COUNT_OFFSET, &spaces.disabled);
  if (!spaces.readable) spaces = GameSpaces{};
  return spaces;
}

std::uintptr_t Session(const void* net_game) {
  if (net_game == nullptr) return 0;
  return reinterpret_cast<std::uintptr_t>(
      nevr_return_to_lobby_queue::ReadPointer(net_game, addr::NETGAME_CLIENT_LOBBY_OFFSET));
}

std::uint64_t LevelOf(const void* resource) {
  std::uint64_t level = 0;
  std::memcpy(&level, static_cast<const unsigned char*>(resource) + addr::LEVEL_RESOURCE_LEVEL_OFFSET, sizeof(level));
  return level;
}

bool ReadsEnabledAs(const void* resource, std::uint64_t level) {
  std::uint32_t flags = 0;
  std::memcpy(&flags, static_cast<const unsigned char*>(resource) + addr::LEVEL_RESOURCE_FLAGS_OFFSET, sizeof(flags));
  return LevelOf(resource) == level && (flags & addr::LEVEL_RESOURCE_DISABLED_FLAG) == 0;
}

bool Lists(const LevelTable& table, const void* resource, std::uint64_t level) {
  const auto value = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(resource));
  for (std::uint64_t i = 0; i < table.count; ++i) {
    if (table.entries[kEntryWords * i] == level && table.entries[kEntryWords * i + kEntryResourceWord] == value) return true;
  }
  return false;
}

// LevelInstances' liveness test. A resource the enabled table lists is the game's and live, so its fields can be
// read; one it no longer lists was torn down or disabled, and its memory is not touched. Only when the tables cannot
// be read at all is a resource's own memory asked, behind a readability check.
bool StillEnabled(void* ctx, const void* resource, std::uint64_t level) {
  const auto& spaces = *static_cast<const GameSpaces*>(ctx);
  if (spaces.readable) {
    if (!Lists(spaces.enabled, resource, level)) return false;
  } else if (!nevr_crash_recovery::IsReadableMemory(
                 resource, addr::LEVEL_RESOURCE_FLAGS_OFFSET + sizeof(std::uint32_t))) {
    return false;
  }
  return ReadsEnabledAs(resource, level);
}

void AppendLoaded(const LevelTable& table, std::vector<std::uint64_t>* levels) {
  for (std::uint64_t i = 0; i < table.count; ++i) {
    if (table.entries[kEntryWords * i + kEntryResourceWord] != 0) levels->push_back(table.entries[kEntryWords * i]);
  }
}

std::uint64_t Mix(std::uint64_t h, std::uint64_t value) {
  std::uint64_t z = h ^ (value + kMixIncrement);
  z = (z ^ (z >> kMixShiftA)) * kMixMultiplierA;
  z = (z ^ (z >> kMixShiftB)) * kMixMultiplierB;
  return z ^ (z >> kMixShiftC);
}

std::uint64_t MixTable(std::uint64_t h, const LevelTable& table) {
  h = Mix(h, table.count);
  for (std::uint64_t i = 0; i < kEntryWords * table.count; ++i) h = Mix(h, table.entries[i]);
  return h;
}

// Everything Reconcile's answer depends on, folded without allocating, so an unchanged state costs one table walk.
std::uint64_t StateStamp(std::uint64_t main_level, std::uintptr_t session, const GameSpaces& spaces) {
  std::uint64_t h = Mix(0, main_level);
  h = Mix(h, nevr_config_alias::Global().Generation());
  h = Mix(h, session);
  h = Mix(h, g_levelEvents.load(std::memory_order_acquire));
  h = Mix(h, spaces.readable ? 1 : 0);
  h = MixTable(h, spaces.enabled);
  return MixTable(h, spaces.disabled);
}

bool OnGameThread() {
  const unsigned long loop = GameLoopThreadId();
  return loop != 0 && loop == GetCurrentThreadId();
}

void NoteLevelEvent(void* resource, bool enabled) {
  if (resource == nullptr) return;
  try {
    std::lock_guard<std::mutex> lock(g_instancesMutex);
    if (enabled) {
      g_instances.OnEnabled(resource, LevelOf(resource));
    } else {
      g_instances.OnDisabled(resource);
    }
    g_instanceCount.store(g_instances.size(), std::memory_order_release);
    g_levelEvents.fetch_add(1, std::memory_order_acq_rel);
  } catch (const std::exception&) {
    if (!g_levelEventFailLogged.exchange(true)) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.MAPS] config_alias reason=level_event_failed (an Enable or Disable was not recorded; the game's level "
          "tables still decide while they can be read)");
    }
  }
}

void RefreshCachedRules(const void* net_game) {
  if (g_refresh == nullptr || net_game == nullptr) return;
  g_refresh(const_cast<void*>(net_game));
  Log(EchoVR::LogLevel::Info,
      "[NEVR.MAPS] config_alias refresh key=mp_arena_rules alias=0x%016llx (the game re-read the rule values it caches)",
      static_cast<unsigned long long>(g_arenaAlias));
}

// The refresh runs only from the tick: it reads through the lookup, so calling it from inside one would nest the
// game's settings update in whatever game code asked for the key.
void ReconcileOnGameThread(bool from_tick) noexcept {
  if (!OnGameThread() || g_inReconcile) return;
  g_inReconcile = true;
  try {
    const auto source = g_mainLevelSource.load(std::memory_order_acquire);
    const std::uint64_t main_level = source != nullptr ? source() : 0;
    const void* net_game = NetGame();
    const std::uintptr_t session = Session(net_game);
    GameSpaces spaces = ReadGameSpaces(net_game);
    // Without the tables, a resource torn down without Disable moves no stamp, so B is revalidated every call.
    const bool from_events = !spaces.readable && g_instanceCount.load(std::memory_order_acquire) != 0;
    const std::uint64_t stamp = StateStamp(main_level, session, spaces);
    if (from_events || !g_haveStamp || stamp != g_lastStamp) {
      std::vector<std::uint64_t> loaded;
      {
        std::lock_guard<std::mutex> lock(g_instancesMutex);
        std::vector<std::uint64_t> enabled = g_instances.Revalidate(session, main_level, &StillEnabled, &spaces);
        g_instanceCount.store(g_instances.size(), std::memory_order_release);
        if (!spaces.readable) loaded = std::move(enabled);
      }
      if (spaces.readable) {
        loaded.reserve(static_cast<std::size_t>(spaces.enabled.count + spaces.disabled.count));
        AppendLoaded(spaces.enabled, &loaded);
        AppendLoaded(spaces.disabled, &loaded);
      } else if (!loaded.empty() && !g_fromEventsLogged) {
        g_fromEventsLogged = true;
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.MAPS] config_alias source=level_events (the game's level tables cannot be read; the Enable and "
            "Disable events decide which levels are loaded)");
      }
      nevr_config_alias::Reconcile(main_level, loaded);
      g_lastStamp = stamp;
      g_haveStamp = true;
      const std::uint64_t arena_alias = nevr_config_alias::Resolve(nevr_config_alias::kArenaRulesKey);
      if (arena_alias != g_arenaAlias) {
        g_arenaAlias = arena_alias;
        g_refreshDue = g_refreshEnabled.load(std::memory_order_acquire);
      }
    }
    if (from_tick && g_refreshDue) {
      g_refreshDue = false;
      RefreshCachedRules(net_game);
    }
  } catch (const std::exception&) {
    if (!g_reconcileFailLogged) {
      g_reconcileFailLogged = true;
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.MAPS] config_alias reason=reconcile_failed (the aliases in effect stay as they were; the next frame "
          "tries again)");
    }
  }
  g_inReconcile = false;
}

}  // namespace

void SetMainLevelSource(std::uint64_t (*source)()) { g_mainLevelSource.store(source, std::memory_order_release); }

void SetCachedRulesRefresh(bool enabled) { g_refreshEnabled.store(enabled, std::memory_order_release); }

void ReconcileIfFrameMoved() noexcept { ReconcileOnGameThread(true); }

void* LookupDetour(std::uint64_t key) {
  if (nevr_config_alias::KeyAllowed(key)) {
    ReconcileOnGameThread(false);
    // The record this answer is read from, for the arena override's check: the aliased record while the alias answers, else none.
    nevr_config_alias::SetLiveRecord(nullptr);
    const std::uint64_t alias = nevr_config_alias::Resolve(key);
    if (alias != key) {
      if (void* record = g_original(alias)) {
        nevr_config_alias::SetLiveRecord(record);
        return record;
      }
      if (!g_aliasMissLogged.exchange(true)) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.MAPS] config_alias reason=alias_not_found key=0x%016llx alias=0x%016llx (the game holds no record "
            "under the alias, so the stock one is read)",
            static_cast<unsigned long long>(key), static_cast<unsigned long long>(alias));
      }
    }
  }
  return g_original(key);
}

std::int64_t EnableDetour(void* level_resource) {
  const std::int64_t result = g_enableOriginal(level_resource);
  // The callers test the low 32 bits (EnableGameSpace 0x1404EF1EA, the unload 0x140515E40 for Disable).
  if (static_cast<std::uint32_t>(result) == 0) NoteLevelEvent(level_resource, true);
  return result;
}

std::int64_t DisableDetour(void* level_resource) {
  const std::int64_t result = g_disableOriginal(level_resource);
  if (static_cast<std::uint32_t>(result) == 0) NoteLevelEvent(level_resource, false);
  return result;
}

#ifdef NEVR_CONFIG_ALIAS_HOOK_TESTING
void SetOriginalForTesting(LookupFn original) { g_original = original; }

void SetLevelResourceOriginalsForTesting(LevelResourceFn enable, LevelResourceFn disable) {
  g_enableOriginal = enable;
  g_disableOriginal = disable;
}

void SetRefreshForTesting(RefreshFn refresh) { g_refresh = refresh; }

void ResetForTesting() {
  g_mainLevelSource.store(nullptr);
  g_refreshEnabled.store(false);
  g_aliasMissLogged.store(false);
  g_levelEventFailLogged.store(false);
  {
    std::lock_guard<std::mutex> lock(g_instancesMutex);
    g_instances = nevr_config_alias::LevelInstances{};
  }
  g_levelEvents.store(0);
  g_instanceCount.store(0);
  g_lastStamp = 0;
  g_haveStamp = false;
  g_inReconcile = false;
  g_reconcileFailLogged = false;
  g_fromEventsLogged = false;
  g_arenaAlias = nevr_config_alias::kArenaRulesKey;
  g_refreshDue = false;
}
#endif

}  // namespace nevr_config_alias_hook

#ifndef NEVR_CONFIG_ALIAS_HOOK_TESTING
namespace nevr_config_alias {

namespace {

namespace addr = nevr_patch_addresses;

// The VAs are the image base plus the RVA in addresses.h, the one place each is written. The prologues are the first
// bytes ReVault reads at each address in echovr.exe (sha256 b6d08277..., project echovr); a mismatch refuses the hook.
// MinHook relocates whole instructions over its 5-byte jump: here the RIP-relative cmp, and plain pushes/stores in
// the others.
constexpr std::uint64_t kLookupVa = nevr::ECHOVR_DEFAULT_IMAGE_BASE + addr::CONFIG_BALANCE_LOOKUP;
// cmp dword [rip+0x13c88f9], 0; jne +0x51; mov rax, [rip+0x13c88e8]
constexpr std::uint8_t kLookupPrologue[] = {0x83, 0x3d, 0xf9, 0x88, 0x3c, 0x01, 0x00, 0x75,
                                            0x51, 0x48, 0x8b, 0x05, 0xe8, 0x88, 0x3c, 0x01};

constexpr std::uint64_t kEnableVa = nevr::ECHOVR_DEFAULT_IMAGE_BASE + addr::CGAMELEVELRESOURCE_ENABLE;
// push rbp; push rbx; push rsi; push rdi; push r12; push r13; push r14; push r15; lea rbp, [rsp-0x528]
constexpr std::uint8_t kEnablePrologue[] = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41,
                                            0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xac};

constexpr std::uint64_t kDisableVa = nevr::ECHOVR_DEFAULT_IMAGE_BASE + addr::CGAMELEVELRESOURCE_DISABLE;
// mov [rsp+0x18], rbx; push rbp; push rsi; push rdi; push r12; push r13; push r14; push r15
constexpr std::uint8_t kDisablePrologue[] = {0x48, 0x89, 0x5c, 0x24, 0x18, 0x55, 0x56, 0x57,
                                             0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};

constexpr std::uint64_t kRefreshVa = nevr::ECHOVR_DEFAULT_IMAGE_BASE + addr::NETGAME_BALANCE_SETTINGS_UPDATE;
// mov [rsp+0x10], rbx; mov [rsp+0x20], rsi; push rbp; push rdi; push r14; lea rbp, ...
constexpr std::uint8_t kRefreshPrologue[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74,
                                             0x24, 0x20, 0x55, 0x57, 0x41, 0x56, 0x48, 0x8d};

// The game function at `va` when its first bytes are `prologue`; null, logged with `consequence`, otherwise.
void* PinnedTarget(std::uint64_t va, const std::uint8_t* prologue, std::size_t size, const char* what,
                   const char* consequence) {
  void* target = nevr::ResolveVA_Checked(reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress), va);
  if (target != nullptr && nevr::ValidatePrologue(target, prologue, size)) return target;
  const std::string actual = target != nullptr && nevr_crash_recovery::IsReadableMemory(target, size)
                                 ? nevr_hex::Bytes(static_cast<const std::uint8_t*>(target), size)
                                 : std::string("unreadable");
  Log(EchoVR::LogLevel::Warning,
      "[NEVR.MAPS] config_alias %s not pinned va=0x%llx expected=%s actual=%s (the game build is not the one this "
      "hook was written against, so %s)",
      what, static_cast<unsigned long long>(va), nevr_hex::Bytes(prologue, size).c_str(), actual.c_str(), consequence);
  return nullptr;
}

// Source B. Either detour missing leaves a set that only grows or only shrinks, so both go in or neither stays.
bool InstallLevelEvents() {
  using nevr_config_alias_hook::g_disableOriginal;
  using nevr_config_alias_hook::g_enableOriginal;
  const char* consequence = "the game's level tables alone say which levels are loaded";
  void* enable = PinnedTarget(kEnableVa, kEnablePrologue, sizeof(kEnablePrologue), "level enable", consequence);
  void* disable = PinnedTarget(kDisableVa, kDisablePrologue, sizeof(kDisablePrologue), "level disable", consequence);
  if (enable == nullptr || disable == nullptr) return false;
  g_enableOriginal = reinterpret_cast<nevr_config_alias_hook::LevelResourceFn>(enable);
  if (!PatchDetour(&g_enableOriginal, reinterpret_cast<PVOID>(&nevr_config_alias_hook::EnableDetour),
                   "level resource enable (map config alias)")) {
    return false;
  }
  g_disableOriginal = reinterpret_cast<nevr_config_alias_hook::LevelResourceFn>(disable);
  if (!PatchDetour(&g_disableOriginal, reinterpret_cast<PVOID>(&nevr_config_alias_hook::DisableDetour),
                   "level resource disable (map config alias)")) {
    const MH_STATUS retired = PatchRetire(enable);
    if (retired != MH_OK && retired != MH_ERROR_NOT_CREATED) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.MAPS] config_alias level enable hook could not be removed status=%d (it records enables only; each "
          "reconcile still drops what the game's level tables do not list)",
          static_cast<int>(retired));
    }
    return false;
  }
  return true;
}

}  // namespace

bool InstallIfStaged() {
  static bool installed = false;
  if (installed) return true;
  if (!Global().AnyStaged()) return false;
  void* lookup = PinnedTarget(kLookupVa, kLookupPrologue, sizeof(kLookupPrologue), "hook",
                              "maps play on the stock arena rules");
  if (lookup == nullptr) return false;
  nevr_config_alias_hook::g_original = reinterpret_cast<nevr_config_alias_hook::LookupFn>(lookup);
  if (!PatchDetour(&nevr_config_alias_hook::g_original, reinterpret_cast<PVOID>(&nevr_config_alias_hook::LookupDetour),
                   "balance config lookup (map config alias)")) {
    return false;  // PatchDetour logged the cause at Warning (N126)
  }
  installed = true;
  const bool level_events = InstallLevelEvents();
  nevr_config_alias_hook::g_refresh = reinterpret_cast<nevr_config_alias_hook::RefreshFn>(
      PinnedTarget(kRefreshVa, kRefreshPrologue, sizeof(kRefreshPrologue), "rules refresh",
                   "the rule values the game caches are not refreshed when the winner changes"));
  Log(EchoVR::LogLevel::Info,
      "[NEVR.MAPS] config_alias hook installed va=0x%llx source=level_tables%s refresh=%s (a map's aliases apply while "
      "any of its levels is loaded)",
      static_cast<unsigned long long>(kLookupVa), level_events ? "+level_events" : "",
      nevr_config_alias_hook::g_refresh != nullptr ? "available" : "unavailable");
  return true;
}

}  // namespace nevr_config_alias
#endif
