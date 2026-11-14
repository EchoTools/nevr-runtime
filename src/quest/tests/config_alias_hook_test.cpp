// Host test for the Quest ConfigAlias (src/quest/sentinel/config_alias_hook.h, docs/adr/0048 decision 5): the loaded levels
// read from a fake game object (both game-space tables, an unreadable table, the cap), and the lookup with the alias
// applied over the real core table. Built by the hooks group the way the sentinel builds the lookup (no exceptions) and
// its levels file (with them). The game's lookup is a stub; nothing is hooked.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/config_alias.h"
#include "quest/sentinel/config_alias_hook.h"

namespace hook = nevr_quest::config_alias_hook;

namespace {

int g_failures = 0;
#define CA_CHECK(cond)                                                     \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

// The game object's loaded and enabled game-space tables (CncaGame::EnableGameSpace reads them at these offsets, ReVault
// libr15 0x18DC124 less the file's 0x100000; see kEnabledTableOffset in config_alias_levels.cpp).
constexpr std::size_t kEnabledTable = 0x1F48, kEnabledCount = 0x1F78;
constexpr std::size_t kLoadedTable = 0x1F88, kLoadedCount = 0x1FB8;
constexpr std::size_t kGameBytes = 0x2000;

// rad_hash("m1_arena_rules"), the record a map's set carries for mp_arena_rules.
constexpr std::uint64_t kAlias = 0x6216cd74aba74633ull;
constexpr std::uint64_t kMain = 0x1000, kSub = 0x1001, kStock = 0x9000;
constexpr std::uint64_t kResourceBase = 0x7700;  // a non-null level resource per level in the fake tables

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

  // Nothing loaded: both tables are empty.
  std::uint64_t levels[2 * 1024];
  WriteTable(&game, &enabled, {}, kEnabledTable, kEnabledCount);
  WriteTable(&game, &loaded, {}, kLoadedTable, kLoadedCount);
  CA_CHECK(hook::ReadLoadedLevels(game.data(), levels, 2 * 1024) == 0);

  // One level in each table: the enabled one is listed first, then the loaded one.
  WriteTable(&game, &enabled, {kSub}, kEnabledTable, kEnabledCount);
  WriteTable(&game, &loaded, {kMain}, kLoadedTable, kLoadedCount);
  CA_CHECK(hook::ReadLoadedLevels(game.data(), levels, 2 * 1024) == 2);
  CA_CHECK(levels[0] == kSub);
  CA_CHECK(levels[1] == kMain);

  // A count with a null data pointer is a table that cannot be read: -1, and no level is listed.
  Put<const std::uint64_t*>(&game, kLoadedTable, nullptr);
  Put<std::uint64_t>(&game, kLoadedCount, 1);
  CA_CHECK(hook::ReadLoadedLevels(game.data(), levels, 2 * 1024) == -1);

  // A count over the table bound (1024) is not a table this code reads: -1.
  Put<std::uint64_t>(&game, kLoadedCount, 1025);
  CA_CHECK(hook::ReadLoadedLevels(game.data(), levels, 2 * 1024) == -1);
}

void TestLookup() {
  g_stockRecord = 0;
  g_aliasRecord = 0;
  ResetAlias();
  // No alias staged: the lookup is the game's own answer.
  CA_CHECK(hook::RunLookupForTest(&GameLookup, nevr_config_alias::kArenaRulesKey) == &g_stockRecord);

  // Staged and armed, with the sublevel loaded: the published winner answers with the alias's record.
  CA_CHECK(nevr_config_alias::Stage("owner-q", "m1", kMain, {{nevr_config_alias::kArenaRulesKey, kAlias}}) == 1u);
  CA_CHECK(nevr_config_alias::Arm({{"owner-q", {kMain, kSub}}}, &HoldsAll, nullptr) == 1u);
  nevr_config_alias::Reconcile(kStock, {kSub});
  CA_CHECK(hook::RunLookupForTest(&GameLookup, nevr_config_alias::kArenaRulesKey) == &g_aliasRecord);
  CA_CHECK(nevr_config_alias::LiveMapId(nevr_config_alias::kArenaRulesKey) == "m1");

  // The sublevel unloads: the stock record again.
  nevr_config_alias::Reconcile(kStock, {});
  CA_CHECK(hook::RunLookupForTest(&GameLookup, nevr_config_alias::kArenaRulesKey) == &g_stockRecord);
  ResetAlias();
}

}  // namespace

int main() {
  TestLoadedLevels();
  TestLookup();
  if (g_failures != 0) {
    std::fprintf(stderr, "config_alias_hook_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("config_alias_hook_test: OK\n");
  return 0;
}
