// Host test for the Quest ConfigAlias (src/quest/sentinel/config_alias_hook.h, docs/adr/0048 decision 5): the loaded levels
// read from a fake game object (both game-space tables, an unreadable table, the cap), and the lookup with the alias
// applied over the real core table. Built by the hooks group the way the sentinel builds the lookup (no exceptions) and
// its levels file (with them). The game's lookup is a stub; nothing is hooked.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/config_alias.h"
#include "quest/sentinel/config_alias_hook.h"
#include "quest/sentinel/game_space_layout.h"
#include "test_check.h"

namespace hook = nevr_quest::config_alias_hook;
namespace layout = sentinel::pinned::game_layout;

namespace {

// The game object's game-space tables, from the shared layout (game_space_layout.h).
constexpr std::size_t kGameBytes = 0x2000;  // large enough for the highest table count offset in the layout

// The table bound the reader holds to (config_alias_levels.cpp kMaxTableCount), and the cap of two tables read.
constexpr std::size_t kTableBound = 1024;
constexpr std::size_t kLevelTables = 2;
constexpr std::size_t kLevelCap = kLevelTables * kTableBound;
constexpr std::size_t kOneLevel = 1;
constexpr std::size_t kOverBound = kTableBound + kOneLevel;

// rad_hash("m1_arena_rules"), the record a map's set carries for mp_arena_rules.
constexpr std::uint64_t kAlias = 0x6216cd74aba74633ull;
constexpr std::uint64_t kMain = 0x1000, kSub = 0x1001, kStock = 0x9000;
constexpr std::uint64_t kResourceBase = 0x7700;  // a non-null level resource per level in the fake tables
constexpr std::size_t kNoLevels = 0;

int g_stockRecord = 0, g_aliasRecord = 0;

void* GameLookup(std::uint64_t key) {
  if (key == nevr_config_alias::kArenaRulesKey) return &g_stockRecord;
  if (key == kAlias) return &g_aliasRecord;
  return nullptr;
}

bool HoldsAll(void*, const std::string&, std::uint64_t) { return true; }

template <typename T>
void Put(std::vector<unsigned char>* bytes, std::size_t offset, T value) {
  std::memcpy(bytes->data() + offset, &value, sizeof(value));
}

// Writes `levels` as {level symbol, resource} entries into the table at `table`, with its count at `count`. The entries
// live in `storage`, which the caller keeps alive for the read.
void WriteTable(std::vector<unsigned char>* game, std::vector<std::uint64_t>* storage, const std::vector<std::uint64_t>& levels,
                std::size_t table, std::size_t count) {
  storage->clear();
  for (const std::uint64_t level : levels) {
    storage->push_back(level);
    storage->push_back(kResourceBase + level);
  }
  Put<const std::uint64_t*>(game, table, storage->empty() ? nullptr : storage->data());
  Put<std::uint64_t>(game, count, static_cast<std::uint64_t>(levels.size()));
}

void ResetAlias() {
  nevr_config_alias::Unstage("owner-q");
  nevr_config_alias::Reconcile(0, {});
}

void TestLoadedLevels() {
  std::vector<unsigned char> game(kGameBytes, 0);
  std::vector<std::uint64_t> enabled, loaded;
  std::uint64_t levels[kLevelCap];
  const int cap = static_cast<int>(kLevelCap);
  const std::size_t enabledTable = layout::kEnabledSpaceTableOffset, enabledCount = layout::kEnabledSpaceCountOffset;
  const std::size_t loadedTable = layout::kLoadedSpaceTableOffset, loadedCount = layout::kLoadedSpaceCountOffset;

  // Nothing loaded: both tables are empty.
  WriteTable(&game, &enabled, {}, enabledTable, enabledCount);
  WriteTable(&game, &loaded, {}, loadedTable, loadedCount);
  QCHECK(hook::ReadLoadedLevels(game.data(), levels, cap) == static_cast<int>(kNoLevels));

  // One level in each table: the enabled one is listed first, then the loaded one.
  WriteTable(&game, &enabled, {kSub}, enabledTable, enabledCount);
  WriteTable(&game, &loaded, {kMain}, loadedTable, loadedCount);
  QCHECK(hook::ReadLoadedLevels(game.data(), levels, cap) == static_cast<int>(kLevelTables));
  QCHECK(levels[0] == kSub);
  QCHECK(levels[1] == kMain);

  // A count with a null data pointer is a table that cannot be read: -1.
  Put<const std::uint64_t*>(&game, loadedTable, nullptr);
  Put<std::uint64_t>(&game, loadedCount, kOneLevel);
  QCHECK(hook::ReadLoadedLevels(game.data(), levels, cap) == -static_cast<int>(kOneLevel));

  // A count over the table bound is not a table this code reads: -1.
  Put<std::uint64_t>(&game, loadedCount, kOverBound);
  QCHECK(hook::ReadLoadedLevels(game.data(), levels, cap) == -static_cast<int>(kOneLevel));
}

void TestLookup() {
  g_stockRecord = 0;
  g_aliasRecord = 0;
  ResetAlias();
  // No alias staged: the lookup is the game's own answer.
  QCHECK(hook::RunLookupForTest(&GameLookup, nevr_config_alias::kArenaRulesKey) == &g_stockRecord);

  // Staged and armed, with the sublevel loaded: the published winner answers with the alias's record.
  QCHECK(nevr_config_alias::Stage("owner-q", "m1", kMain, {{nevr_config_alias::kArenaRulesKey, kAlias}}) == kOneLevel);
  QCHECK(nevr_config_alias::Arm({{"owner-q", {kMain, kSub}}}, &HoldsAll, nullptr) == kOneLevel);
  nevr_config_alias::Reconcile(kStock, {kSub});
  QCHECK(hook::RunLookupForTest(&GameLookup, nevr_config_alias::kArenaRulesKey) == &g_aliasRecord);
  QCHECK(nevr_config_alias::LiveMapId(nevr_config_alias::kArenaRulesKey) == "m1");

  // The sublevel unloads: the stock record again.
  nevr_config_alias::Reconcile(kStock, {});
  QCHECK(hook::RunLookupForTest(&GameLookup, nevr_config_alias::kArenaRulesKey) == &g_stockRecord);
  ResetAlias();
}

}  // namespace

int main() {
  TestLoadedLevels();
  TestLookup();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "config_alias_hook_test: %d failure(s)\n", quest_test::Failures());
    return 1;
  }
  std::printf("config_alias_hook_test: OK\n");
  return 0;
}
