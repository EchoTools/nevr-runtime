/* What level the game has loaded: the shared answer the combat modules ask (#2172, docs/adr/0046), so no module reads the game's gamespace tables
 * for itself. Arena and combat run on the same APK, so `combat::Active()` does not say which one a player is in; this does.
 *
 * What it reads. CR15Game's two gamespace tables ({sym, gs*} rows of 16 bytes; counts at 0x1f78 and 0x1fb8; offsets 0x1f48/0x1f88) and, in each
 * gamespace of the game (gs+0x8 is the game), its level CSymbol64 at gs+0x58. The pinned build's offsets; the q15t plugin's `refresh_rate.cpp`
 * carried them (got_pinned_test.cpp checks them against the store libr15, and combat_level_targets.h ties each offset to its pinned word at compile time). Reads use process_vm_readv (safe_read.h): an unreadable
 * game is no level (fail closed).
 *
 * The three classes. A combat map: `mpl_combat_dyson`, `_combustion`, `_fission`, `_gauss`. The combat lobby: `mpl_lobby_b_combat` only. The other
 * lobby levels (`mpl_lobby_b`, `_b2`, `_b2_lowspec`, `_b_arena`, `_b_social`) are the lobbies arena players use and are NOT a class here: nothing
 * about them is combat's. An Echo Arena level: `mpl_arena_a`, `_a_lowspec`, `_b`, `mpl_tutorial_a`. An Echo Arena level that is loaded wins over any
 * other level still loaded during a transition. Each id is `EchoVR::CSymbol64Hash` of its name (combat_level_test checks it).
 *
 * Sub-levels are game spaces. Every level of a load set gets a game space of its own: CncaGame::LoadCombinedLevelSet (disassembly 0x18ef9d8) calls the
 * game's CreateGameSpace (vtable +0x338, CncaGame::CreateGameSpace 0x18f4a00: one 0xca0-byte CncaGameSpace per SLevelLoad) for each level not yet loaded,
 * and UpdateGameSpaceInit (0x18ecc7c) inserts each finished one into the table at +0x1f88. So a lobby's sub-level is a row these tables hold. The script
 * that loads the lobby's sub-levels (libf808c072bb49da47.so) loads the lobby level with `mpl_lobby_b_arena`, and adds `mpl_lobby_b_combat` only when the
 * headset is not a Quest: its conditions 0x4470b75a and 0xa1ebf03c read R15HEADSETTYPEEXPRESSION's `quest` output and, on a Quest, lead to the
 * LOADLEVELNODEs 0x6a659965 and 0x7c7624d (two levels: the lobby and `mpl_lobby_b_arena`), otherwise to 0x3d9a980e and 0x35a88ed2 (three: the lobby,
 * `mpl_lobby_b_arena`, `mpl_lobby_b_combat`), which is what the PC run in docs/reference/game-level.md loads. A search of the code of the game's 1,041
 * binaries for the id's `movk #0xcb99, lsl #48` finds the nodes 0x35a88ed2, 0x3d9a980e and 0xd85450a9 (reached only after 0x3d9a980e) and nothing else;
 * level data is not code and was not searched. So, as far as the code shows, an arena lobby on a Quest holds no `mpl_lobby_b_combat` game space, and
 * `lobby` adds nothing there.
 *
 * Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <cstdint>

namespace nevr_quest::combat::level {

// Every class the loaded gamespaces hold, at once, with the first level found of each (0 when none). One scan of the tables.
struct Loaded {
  bool combatMap = false;
  bool combatLobby = false;
  bool echoArena = false;
  std::uint64_t combatMapLevel = 0;
  std::uint64_t combatLobbyLevel = 0;
  std::uint64_t echoArenaLevel = 0;
  // Only with `inFlight`: the gamespaces still loading, and which of the classes above are among them (their levels are in the fields above too).
  std::uint32_t loading = 0;
  bool loadingCombatMap = false;
  bool loadingCombatLobby = false;
  std::uint64_t loadingCombatMapLevel = 0;
  std::uint64_t loadingCombatLobbyLevel = 0;
};
//
// `inFlight` also reads the gamespaces still being loaded: CncaGame's init-fiber table (rows game+0x1ed8, count +0x1f08, the fiber's gamespace at
// +0x30). A gamespace enters the two tables above only after its level's resources are built, but the level is stored in it before, so a caller that
// acts while resources are built (the table-step modules) needs this source; the same rules apply to a level found there, an Echo Arena level
// winning wherever it is found. The init-fiber table is read first and the two tables second: when a fiber finishes, the game inserts its gamespace
// into the loaded table before it removes the fiber (0x17ed480, then 0x17ed4a8 to 0x17ed4b4), so this order sees a finishing gamespace in at least one
// of them, and the other order can miss it in both. Callers that decide per frame on a level that is up (refresh_rate, pfx) leave it false and see what they always did.
Loaded Scan(std::uintptr_t game, bool inFlight = false) noexcept;

// What the loaded gamespaces decide: an Echo Arena level is kEchoArena and wins always; else a combat map (or, with `lobby`, the combat lobby)
// is kCombatOrLobby; anything else, every other lobby level included, is kNone. With `inFlight`, when any gamespace is still loading the levels
// loading decide between combat and the rest (the resources being built are theirs): a lobby arena players use that is loading while a combat map is
// still loaded is kNone, and a combat map loading while a lobby is loaded is kCombatOrLobby. A loading gamespace whose level is not stored yet (-1,
// the constructor's value) is not counted as loading. An Echo Arena level, loaded or loading, still wins.
enum class Kind { kNone, kCombatOrLobby, kEchoArena };

bool IsCombatMap(std::uint64_t level) noexcept;
bool IsCombatLobby(std::uint64_t level) noexcept;
bool IsEchoArenaLevel(std::uint64_t level) noexcept;

// What the loaded levels of `game` decide; `level` gets the deciding level's CSymbol64 (0 when none).
Kind Classify(std::uintptr_t game, bool lobby, std::uint64_t* level, bool inFlight = false) noexcept;

// Test seam: called once between the two reads of Scan(inFlight) (after the init-fiber table, before the loaded tables), so a test can make a gamespace
// finish between them. nullptr (production) calls nothing.
void SetScanSeamForTest(void (*seam)()) noexcept;

// The name of a level this file knows (a combat map, the combat lobby, an Echo Arena level, the other lobbies), or nullptr.
const char* LevelName(std::uint64_t level) noexcept;

// True when one of the game's gamespaces holds a combat map (never a lobby or arena level); `level` gets it (0 when none).
bool CombatMapLoaded(std::uintptr_t game, std::uint64_t* level) noexcept;

}  // namespace nevr_quest::combat::level
