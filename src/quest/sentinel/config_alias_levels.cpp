// ConfigAlias on Quest, the half that allocates and catches (config_alias_hook.h). Built with exceptions. AliasFor and
// ReconcileWork are NEVR_OUTSIDE_GAME_CALL: the lookup handler calls AliasFor before it calls the game's lookup, the frame
// dispatcher runs ReconcileWork between game calls, neither calls game code, and nothing leaves them by exception.
#include <atomic>
#include <cstdint>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <utility>
#include <vector>

#include "core/config_alias.h"
#include "hook_log.h"
#include "game_space_layout.h"
#include "outside_game_call.h"
#include "quest/invite/frame_dispatch.h"
#include "quest/sentinel/config_alias_hook.h"

namespace nevr_quest::config_alias_hook {

namespace {

// The game object's game-space tables: the offsets are in sentinel::pinned::game_layout (pinned_targets.h). Entries are
// {CSymbol64 level, CncaGameSpace* space}, 16 bytes.
constexpr std::size_t kEnabledTableOffset = sentinel::pinned::game_layout::kEnabledSpaceTableOffset;
constexpr std::size_t kEnabledCountOffset = sentinel::pinned::game_layout::kEnabledSpaceCountOffset;
constexpr std::size_t kLoadedTableOffset = sentinel::pinned::game_layout::kLoadedSpaceTableOffset;
constexpr std::size_t kLoadedCountOffset = sentinel::pinned::game_layout::kLoadedSpaceCountOffset;
constexpr std::size_t kEntryBytes = 16;
constexpr std::uint64_t kMaxTableCount = 1024;  // the bound the PC hook uses: more reads as a table that is not one
constexpr int kMaxLevels = 2 * static_cast<int>(kMaxTableCount);

std::atomic<std::uint64_t (*)() noexcept> g_mainLevelSource{nullptr};
std::atomic<bool> g_failureLogged{false};
std::atomic<bool> g_unreadableLogged{false};
std::uint64_t g_lastStamp = 0;  // tick thread only
bool g_haveStamp = false;

template <typename T>
T ReadAt(const void* base, std::size_t offset) noexcept {
  T value{};
  std::memcpy(&value, static_cast<const unsigned char*>(base) + offset, sizeof value);
  return value;
}

const void* GameObject() noexcept {
  const void* netGame = invite::frame::NetGame();
  return netGame == nullptr ? nullptr : ReadAt<const void*>(netGame, sentinel::pinned::game_layout::kGameOffset);
}

// FNV-1a: the offset basis and the prime the stamp folds with.
constexpr std::uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ull;
constexpr std::uint64_t kFnvPrime = 0x100000001b3ull;

std::uint64_t Fold(std::uint64_t stamp, std::uint64_t value) noexcept { return (stamp ^ value) * kFnvPrime; }

void LogOnce(std::atomic<bool>& flag, const char* reason, const char* consequence) noexcept {
  if (!flag.exchange(true)) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "config_alias", {{"reason", reason}, {"consequence", consequence}});
  }
}

void Reconcile() noexcept {
  try {
    const auto source = g_mainLevelSource.load(std::memory_order_acquire);
    const std::uint64_t mainLevel = source != nullptr ? source() : 0;
    static std::uint64_t levels[kMaxLevels];  // tick thread only
    const void* game = GameObject();
    int count = game != nullptr ? ReadLoadedLevels(game, levels, kMaxLevels) : 0;
    if (count < 0) {
      LogOnce(g_unreadableLogged, "tables_unreadable", "the loaded levels read as none; only the main level decides");
      count = 0;
    }
    std::uint64_t stamp = Fold(Fold(kFnvOffsetBasis, mainLevel), nevr_config_alias::Global().Generation());
    for (int i = 0; i < count; ++i) stamp = Fold(stamp, levels[i]);
    if (g_haveStamp && stamp == g_lastStamp) return;
    nevr_config_alias::Reconcile(mainLevel, std::vector<std::uint64_t>(levels, levels + count));
    g_lastStamp = stamp;
    g_haveStamp = true;
  } catch (const std::exception&) {
    LogOnce(g_failureLogged, "reconcile_failed", "the previous aliases stay until a later reconcile succeeds");
  }
}

}  // namespace

void SetMainLevelSource(std::uint64_t (*source)() noexcept) noexcept {
  g_mainLevelSource.store(source, std::memory_order_release);
}

int ReadLoadedLevels(const void* game, std::uint64_t* out, int cap) noexcept {
  int written = 0;
  for (const auto& table : {std::pair<std::size_t, std::size_t>{kEnabledTableOffset, kEnabledCountOffset},
                            std::pair<std::size_t, std::size_t>{kLoadedTableOffset, kLoadedCountOffset}}) {
    const auto* entries = ReadAt<const unsigned char*>(game, table.first);
    const auto count = ReadAt<std::uint64_t>(game, table.second);
    if (count == 0) continue;
    if (entries == nullptr || count > kMaxTableCount) return -1;
    for (std::uint64_t i = 0; i < count && written < cap; ++i) {
      const auto level = ReadAt<std::uint64_t>(entries, i * kEntryBytes);
      if (level != 0 && ReadAt<const void*>(entries, i * kEntryBytes + 8) != nullptr) out[written++] = level;
    }
  }
  return written;
}

void ReconcileIfFrameMoved() noexcept {
  if (invite::frame::OnTickThread()) Reconcile();
}

NEVR_OUTSIDE_GAME_CALL std::uint64_t AliasFor(std::uint64_t key) noexcept {
  if (!nevr_config_alias::KeyAllowed(key)) return key;
  ReconcileIfFrameMoved();
  return nevr_config_alias::Resolve(key);
}

NEVR_OUTSIDE_GAME_CALL bool ReconcileWork(void*) noexcept {
  Reconcile();
  return true;
}

}  // namespace nevr_quest::config_alias_hook

namespace nevr_config_alias {

void PlatformWarn(const char* line) noexcept {
  sentinel::LogFields(sentinel::LogLevel::kWarn, "config_alias", {{"line", line}});
}

}  // namespace nevr_config_alias
