/* Bullet impact sounds (#2183, docs/adr/0046; docs/quest/combat/impactsnd.md): the Quest logs "Cannot find default impact sound for bullet type
 * 0x<gun>" on every bullet hit and plays no impact sound. The engine's impact table (CR15NetBulletCS::kSoundSettings, rows {u64 key = sym(subkey) ^
 * sym(gun), u32 id}) is built wrong on the Quest in two ways, and this module repairs both with data writes to the table only:
 *   (a) the loader builds only 5 of the 15 rows (its row scratch overlaps its path string). The loader is rebuilt in C++ and appends every row; it
 *       runs only with the Shutdown handler in and every callee stub pinned (docs/quest/combat/impactsnd.md, "The rebuilt loader");
 *   (b) every row's id is the stock 1 instead of the Wwise event id (FNV-1 32 of the lowercase event name, because the game's own
 *       CalculateSymbolValue returns 1 for every name on the Quest). The rows are rewritten in place: by the rebuilt loader as it appends, by a pass
 *       straight after a stock load, and by a poll on the game tick for every sorted or unsorted table instance of the process.
 * The four hit-code words and D (the "default" symbol word the hit code XORs into the gun symbol) are checked, never changed. No code page is written.
 *
 * Owner (ADR 0039). This file owns CR15NetBulletCS::LoadSoundSettings' JUMP_SLOT and CR15NetBulletCS::Shutdown's vtable word as the first handler on
 * the latter; the decals module (decals.h) joins the Shutdown word's chain above this one. The q15t plugin no longer installs this module, so
 * nothing else installs on either word; its build option and its data file (none) are gone.
 *
 * Arena (same APK). The impact table and these two game functions exist in an arena match too, and the sounds they feed play there, so the module
 * acts only where a combat level is loaded (combat_level.h; `Classify` with the combat lobby on: an Echo Arena level that is loaded wins, and the
 * lobbies arena players use are no class):
 *   - LoadSoundSettings: the handler asks the loaded levels once per load (a level change), through the CR15Game the combat tick supplies. A combat
 *     level: the rebuilt loader (or the stock loader and the after-load pass). Anything else, an unreadable game or no game seen yet included: the
 *     game's own loader runs, nothing is read or written, and the table keeps the game's own rows and ids (`scope` in the load line says which).
 *   - Shutdown: takes the table lock around the game's function in every level. The lock guards the row writes, which only a combat level starts;
 *     in an arena match the handler is a lock, a per-thread depth word and the call (once per level unload).
 *   - the tick: one atomic load of the game, one of the scope flag, and a return, per frame, until a table that a combat level loaded is live. When the
 *     load handler could not be hooked (`combat_impactsnd_hook` Error) nothing marks a load, so the tick asks the levels itself once a second.
 * A table is "ours" from the load that decided combat until the game frees it (the global reads 0 after Shutdown).
 *
 * Which APKs (ADR 0046 rule 5). The q15t and combat variants only (the normal step of combat_install.h).
 *
 * Frequency (ADR 0038). LoadSoundSettings: once per game-space load (CR15NetBulletCS::Initialize). Shutdown: once per bullet system teardown. The
 * tick callback: once per rendered frame; a poll (four a second) only while the scope flag is set. Arena cost, per call and per frame, is in the
 * module doc ("Arena cost") with the measured host numbers.
 *
 * Origin. The reference build's `impactsnd` (the source build's impactsnd v1..v6; rows R12..R16, docs/audits/2026-11-05-q15t-origin-citations.md lists
 * its file and lines), reconstructed in the q15t plugin and moved here; R17 (the Shutdown lock) is ours. It is the reference build's logic, so the move
 * carries kotorvr's credit.
 *
 * Lines (sentinel log; every record's fields are typed, a value is never a free-text sentence):
 *   install   {"event":"combat_impactsnd_install","result":"installed|refused|off","status":"<token>","shutdown":"<GotStatus>","load":"<GotStatus>",
 *              "row_writes":"enabled|disabled","rebuild":"reconstructed|stock","msg":".."}  Info when installed, Error when refused
 *   hook      {"event":"combat_impactsnd_hook","hook":"shutdown|load","status":"<GotStatus>"}    Info when ok, Error otherwise
 *   refused   {"event":"combat_impactsnd_refused","reason":"..","vaddr":"0x..","read":"0x..","pinned":"0x..","written":0}   Error
 *   default   {"event":"combat_impactsnd_default_symbol","vaddr":"0x..","value":"0x..","matches":0|1}
 *   scope     {"event":"combat_impactsnd_scope","where":"load","scope":"combat|other_level|no_game","loader":"rebuilt|stock|game"}   per load (capped)
 *   load      {"event":"combat_impactsnd_load","table":"0x..","rows":<n>,"flags":"..","word_patch":"in_effect|absent|unexpected|no_verdict"}
 *   rebuilt   {"event":"combat_impactsnd_rebuilt","table":"0x..","rows":<n>,"json_result":<n>}
 *   ids       {"event":"combat_impactsnd_ids","when":"..","table":"0x..","rows":<n>,"known":<n>,"rewrote":<n>,"right":<n>,"left":<n>,"found_ids":".."}
 *   table     {"event":"combat_impactsnd_table","map":"set|null","rows":<n>,"after_s":"..","default_symbol":"0x..","word_patch":".."}
 *   notes     combat_impactsnd_no_table, _row_pass_skipped, _unsorted_wait, _unsorted_foreign_keys, _shutdown_returned, _thread, _log_cap
 * and the manager's own `got_hook` line for each slot.
 *
 * Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/combat/combat_install.h"
#include "quest/sentinel/got_hook.h"

namespace nevr_quest::combat::impactsnd {

struct Row {
  std::uint64_t key;  // CSymbol64(subkey) ^ CSymbol64(gun)
  std::uint32_t id;   // FNV-1 32 of the lowercase Wwise event name
};

// The 15 known rows (5 guns x {player, barrier, default}).
const Row* Rows(std::size_t* count) noexcept;

// The module's normal step: `on` is `combat.impactsnd`, `write` is `combat.impactsnd_write` (false: the module reads and logs, and writes nothing to
// the table). Installs the two handlers on the loaded libr15 and registers the tick.
ModuleResult Install(bool on, bool write) noexcept;
// The same on the libr15 the lookup finds (the host test supplies an image built in memory).
ModuleResult InstallAt(bool on, bool write, sentinel::ImageLookup lookup) noexcept;

// The game-thread tick callback (combat_tick.h `TickFn`): records the CR15Game for the load handler's scope question, and polls the table four times
// a second while a table a combat level loaded is live.
void Tick(std::uintptr_t game) noexcept;

struct Stats {
  bool on = false;                 // the install checks passed
  bool writersOk = false;          // the Shutdown handler is in: row writes are enabled (else the module reads and logs only)
  bool scoped = false;             // a table that a combat level loaded is live
  std::uint32_t polls = 0;         // tick polls that ran (not the frames that returned early)
  std::uint32_t loads = 0;         // LoadSoundSettings returns seen by the handler (any level)
  std::uint32_t scopedLoads = 0;   // of those, loads in a combat level
  std::uint32_t passes = 0;        // row passes that had something to write
  std::uint32_t rowsWritten = 0;   // ids rewritten (read back right)
  std::uint32_t rowsLeft = 0;      // known rows that needed a write and did not get one (read only, or a lost race)
  std::uint32_t skipped = 0;       // row passes that wrote nothing because the table lock was busy or the table had changed
  std::uint32_t shutdowns = 0;     // CR15NetBulletCS::Shutdown calls seen by the handler
};
Stats GetStats() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

// A clock (seconds); a hook called at stage 1 (between a table read and the re-read before the rows are written), stage 2 (after a row store, before
// its read-back, table lock held) and stage 4 (in a row pass, after the table was validated and before the lock is taken); a reset of every piece of
// state, the hooks and the thunks'.
void SetClockForTest(double (*clock)()) noexcept;
// The CR15Game the load handler asks the levels of, as the tick would hand it (0: none seen); and whether a table a combat level loaded is live, for
// the tests of the tick's watch, which start from "a combat level loaded a table".
void SetGameForTest(std::uintptr_t game) noexcept;
void SetScopedForTest(bool scoped) noexcept;
void SetHookForTest(void (*hook)(int stage)) noexcept;
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::impactsnd
