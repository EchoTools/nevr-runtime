// chassisfork (issue #1737 row G6A-027; on by default, kill file chassisfork.off): in a combat match (and in the lobby) a player's
// chassis body loads as its fork copy from the content pack, which carries the PC's outline creation mode, so the threat scanner's
// outline draws on it. Everywhere else the stock body loads.
//
// CModelSwapperCS::BeginModelSwap(swapper, handle, target) starts swapping a player's model to `target`. Its one PLT caller is
// CR15NetPlayerModelSwapperCS::BeginModelSwap, once per swap (a player joining, a loadout change), never per frame. A wrapper on its
// JUMP_SLOT (a data word) replaces a stock chassis id with its fork id only when all hold:
//   - the target is one of the 32 Quest-only combat chassis bodies (or, unless chassisfork_soc.off, one of their 32 social models);
//   - the scene is a combat scene, published by a game-thread tick (below); the lobby counts unless chassisfork_lobby.off;
//   - the fork's scene, mesh list and archive are all keys of the resource manager's package table (the content pack is installed).
// Anything else passes the stock id. The fork id of a stock id S is CSymbol64("<S as 16 lowercase hex digits>_cbt") for a combat body
// and "..._soc" for a social model: the content pack's naming rule, so the table here holds only the game's stock ids.
//
// The scene tick reads CR15Game's two gamespace tables every 0.5 s, and at once when a table word changes. Scene = combat only when a
// combat map (or war_room) is loaded and every other loaded level is a combat sublevel or a neutral parent (global_mp, menus); any other
// level makes it 0. Off to combat needs two equal looks in a row; combat to off is at once. Details: docs/quest/q15t/modules/chassisfork.md.
#pragma once

#include <cstddef>
#include <cstdint>

#include "import_hook.h"
#include "word_pins.h"

namespace q15t {

struct ChassisForkPins {
  std::uintptr_t swap_slot;  // CModelSwapperCS::BeginModelSwap JUMP_SLOT (ELF vaddr): swapped
  std::uintptr_t swap_fn;    // CModelSwapperCS::BeginModelSwap
  std::uintptr_t res_mgr;    // the global holding the CResourceManager*
};
inline constexpr ChassisForkPins kChassisForkPinned = {0x3700470, 0x1634f7c, 0x376ed60};

inline constexpr WordPin kChassisForkWords[] = {
    {0x1634f7c, 0xd10183ff}, {0x1634f80, 0xa9016bfb}, {0x1634f84, 0xa90263f9},  // BeginModelSwap prologue
    {0x217dd50, 0x97b74f70},                                                    // the net swapper's bl to its PLT stub
    {0x17fabd4, 0x9000fba8}, {0x17fabd8, 0xf946b100},                           // a reader of the resource manager global
    {0x2544250, 0xf9400408}, {0x2544254, 0xf940390b}, {0x254425c, 0xf9402108},  // LookupResourcePath: db +8, count +0x70, table +0x40
    {0x2544268, 0x5280050a}, {0x2544290, 0xf86d690d}, {0x25442a0, 0xf94005ad},  // row 0x28: type at +0, id at +8 (signed compare)
};

// The package table LookupResourcePath searches: [[mgr] + 8] = db; db+0x40 rows of 0x28 bytes sorted by (type, id) as signed
// 64-bit values; db+0x70 the row count.
inline constexpr std::uintptr_t kResDb = 0x8, kPkgRows = 0x40, kPkgCount = 0x70, kPkgRow = 0x28;
inline constexpr std::uint64_t kPkgMaxRows = 1u << 22;
inline constexpr std::uint64_t kTypeScene = 0x3ef2335265daea50ull, kTypeMeshList = 0x616671cd1c9b4046ull,
                               kTypeArchive = 0x004c5273be3eab58ull;

// CR15Game's gamespace tables: {table, count} at these offsets; entry i: gamespace at table + 16 i + 8; a gamespace belongs to the
// game when [gs + 8] == game; its level at gs + 0x58 (the same tables gamespace.cpp walks).
inline constexpr std::uintptr_t kGameTables[2][2] = {{0x1f48, 0x1f78}, {0x1f88, 0x1fb8}};
inline constexpr std::uintptr_t kGsGame = 0x8, kGsLevel = 0x58;
inline constexpr int kMaxLevels = 64;

// The fork naming rule, at compile time.
std::uint64_t ForkIdCombat(std::uint64_t stock);
std::uint64_t ForkIdSocial(std::uint64_t stock);

// Level kinds and the scene rule (pure; the tick feeds it).
enum class LevelKind { kOther, kCombat, kNeutral, kLobby };
LevelKind ClassifyLevel(std::uint64_t level, bool lobby_counts);
// true = combat scene: no read failed, at least one combat (or lobby) level, and no level of another kind.
bool SceneIsCombat(const std::uint64_t* levels, int n, bool read_failed, bool lobby_counts);

// The fork for `target` when it is a listed stock id, else 0.
std::uint64_t ForkFor(std::uint64_t target, bool social);

// Binary search of the package table with SafeRead; false on any unreadable word.
bool PackageHas(std::uintptr_t res_mgr_global, std::uint64_t type, std::uint64_t id);

InstallResult InstallChassisFork(std::uintptr_t base);
InstallResult InstallChassisForkAt(std::uintptr_t base, const ChassisForkPins& pins, const WordPin* words, std::size_t n_words);

// The scene tick (game thread, once per frame; a few word reads unless the tables moved or 0.5 s passed). Register with AddGameTick.
void ChassisForkTick(std::uintptr_t game);

struct ChassisForkCounters {
  std::uint64_t rewritten;  // swaps given the fork
  std::uint64_t passed;     // listed swaps outside a combat scene: stock
  std::uint64_t closed;     // listed swaps in a combat scene whose fork is not in the package table: stock
  std::uint64_t scene_changes;
};
ChassisForkCounters ChassisForkGetCounters();
bool ChassisForkSceneIsCombat();

// Test seams.
void ResetChassisForkForTest();
void SetChassisForkClockForTest(double (*now)());
std::uintptr_t ChassisForkWrapperForTest();

}  // namespace q15t
