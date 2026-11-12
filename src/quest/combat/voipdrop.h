/* A remote voice packet is dropped when the receiver's voice table needs a page and the page cannot be allocated (#2178).
 *
 * What the game does. CHashTable<uint,ull>::AllocPage asks the table's allocator for one page (table+0x64 bytes) and never checks the answer; a NULL page
 * is then written from its end backwards (SIGSEGV in CR15NetVoipReceiverCS::ReceiveVoip -> FindOrCreate -> AllocPage, tester dump c63511b8). The table
 * frees every page each frame (UpdateDefragment), so every packet after that needs a fresh page: a heap with no free block that size crashes on the next
 * voice packet (voipdrop_targets.h has the code).
 *
 * What this does. The handler reads the table's free head ([cs+0x1b8], int32). When it is -1 the next packet needs a page: it asks the same allocator the
 * same way (CMemoryContext::SetCurrent(CMemBlock::Allocator(cs+0x180)), vtable +0x10 with [cs+0x1cc] bytes), gives the block straight back (vtable +0x38) and
 * restores the memory context; a failed request drops the packet (the game's function is not called). Every other packet, and every doubt (self null, the page size or the vtable unreadable or missing an entry), goes to the game's function with the same arguments. The probe is not a reservation:
 * it narrows the crash to a race, it does not close it.
 *
 * Arena (same APK). It acts in EVERY level, on purpose: it is a guard against a crash in voice code that arena and combat share, and a level answer (combat_scope.h) would leave
 * the crash in place (and could lag a level change); it asks nothing of it and is not a reader of the scope row. It changes what the game does only where the game would otherwise fault: that packet is dropped. Every
 * other packet reaches the stock function with the same arguments, so an arena match with free memory sees no change. Cost in arena: per voice packet received,
 * one plain 4-byte load of the free head (about 3 ns added on the host; the game dereferences the receiver on every path that reaches the table but can return before that (0x22c3260), so on that path
 * this is a load the stock function would not make; `self` being a valid receiver is inferred, not shown, see voipdrop.cpp) and two relaxed atomic increments in the common case (plus the thunk's own counter); the allocator probe (a never-faulting read of the page size,
 * three game calls and the restore) only when the head is -1. When the head stays -1 (the live speakers fill every page exactly) each packet pays one allocate and one
 * free on the allocator: `probed` against `packets` in `combat_voipdrop_stats` shows whether that is a steady state. The host cost of the common path is in docs/quest/combat/voipdrop.md; not measured on a
 * headset, and the packet rate is not known: `combat_voipdrop_stats` counts it.
 *
 * Which APKs (docs/adr/0046 rule 5). It runs where combat::Active() is true: the combat variants. Its switch is `combat.voipdrop` in config.yaml
 * (default true). The plugin's `voipdrop.off` file is no longer read; one found in the data directory (legacy_switch.h) is a Warning naming the key.
 *
 * Install. The patched build ID, the code words (voipdrop_targets.h), the four engine exports (dlsym on the loaded libr15), then the JUMP_SLOT, which must hold
 * ReceiveVoip itself. A refused install leaves the game's calls as they are. It registers in combat::InstallNormal()'s table at the position the plugin
 * installed it (after fxtrail, before custctx).
 *
 * Frequency (ADR 0038). ReceiveVoip runs once per remote voice packet; the rate per frame is not known and is what `combat_voipdrop_stats` reports.
 *
 * Owner (ADR 0039). This file owns whether a voice packet whose table page cannot be allocated reaches ReceiveVoip.
 *
 * Origin. Written here (imported in #1591) on the reference build's `afk_voipdrop.cpp` (Ducky's echoquest-combat;
 * docs/audits/2026-11-05-q15t-origin-citations.md rows 133-136 and 279-280: afk_voipdrop.cpp:1,42-43,51,55-64,82-94,96-112,114-130). The plugin's
 * probe-with-doubt rule and word pins are additions to the source's.
 *
 * Lines (sentinel log):
 *   install  {"event":"combat_voipdrop_install","result":"installed|refused|off","reason":"ok|off_config|<token>","hook":<GotStatus>,"consequence":".."}
 *            Info for installed and off, Error for refused (pinned_word_mismatch adds "pin","found","expected"; reasons module_not_loaded, no_build_id,
 *            build_id_mismatch, pinned_word_mismatch, export_missing, slot_refused)
 *   dropped  {"event":"combat_voipdrop_dropped","n":<count>,"page_bytes":<n>,"consequence":".."}  Info, the first kDropLines drops
 *   total    {"event":"combat_voipdrop_dropped_total","n":<count>,"consequence":".."}  Info, every kTotalEvery-th drop after them, at most kTotalLines
 *   cap      {"event":"combat_voipdrop_log_cap","lines":"dropped|dropped_total","cap":16|8,"msg":".."}  Warning, once per kind, written by the first line over its cap
 *            (line_budget.h); the counts go on in the stats line
 *   stats    {"event":"combat_voipdrop_stats","packets":<n>,"probed":<n>,"dropped":<n>,"final":0|1,"consequence":".."}  Debug every kStatsEvery packets,
 *            Info once at the end of the process when the guard is installed (packets = every packet that reached the handler, probed = those that
 *            needed a page, dropped = those dropped)
 *   legacy   {"event":"combat_voipdrop_legacy_switch","key":"combat.voipdrop","file":"voipdrop.off","consequence":".."}  Warning
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/combat/combat_install.h"
#include "quest/combat/voipdrop_targets.h"
#include "quest/sentinel/got_hook.h"

namespace nevr_quest::combat::voipdrop {

inline constexpr unsigned kDropLines = 16;      // `dropped` lines written per process
inline constexpr unsigned kTotalEvery = 50;     // a running total at every this-many-th drop after them
inline constexpr unsigned kTotalLines = 16;     // at most this many totals
inline constexpr unsigned kStatsEvery = 16384;  // a Debug stats line every this many packets (a power of two)

// The two words the install pins: the slot and the function it must hold. The host test supplies a stand-in's address.
struct Pins {
  std::uint64_t slot, fn;
};
inline constexpr Pins kPins = {kReceiveVoipSlotVaddr, kReceiveVoipVaddr};

// The four engine functions the probe calls (libr15 exports).
struct Fns {
  void* (*blockAllocator)(const void* block);  // CMemBlock::Allocator() const
  void* (*currentPtr)();                       // CMemoryContext::CurrentPtr()
  void (*setCurrent)(void* allocator);         // CMemoryContext::SetCurrent(CAllocator*)
  void* (*current)();                          // CMemoryContext::Current()
};

// The four exports by name through `find(context, symbol)` (null when it has none): the production lookup is dlsym, the host test's a table.
using FindSymbolFn = void* (*)(void* context, const char* symbol);
Fns ResolveFnsWith(void* context, FindSymbolFn find) noexcept;

// dlsym on the loaded game library (RTLD_NOLOAD). Any member null when its export is missing (or the library is not loaded).
Fns ResolveFns() noexcept;

// Installs on the libr15 `lookup` finds. `on` is combat.voipdrop; Logs one combat_voipdrop_install line. A module that is off by its switch answers `off` (status "off_config").
ModuleResult InstallAt(sentinel::ImageLookup lookup, const Pins& pins, const Fns& fns, bool on) noexcept;

// The table's row: InstallAt on the loaded libr15 with the production pins, the dlsym'd exports and the plugin's data directory.
ModuleResult Install(bool on) noexcept;

struct Stats {
  std::uint64_t packets;  // every packet that reached the handler
  std::uint64_t probed;   // those whose table needed a page
  std::uint64_t dropped;  // those dropped
};
Stats CurrentStats() noexcept;

// The table's end-of-process step: writes the final combat_voipdrop_stats line when the guard is installed, nothing otherwise.
void NoteEnd() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

// How the install finds libr15 (production: the loaded image). nullptr restores production.
void SetImageLookupForTest(sentinel::ImageLookup lookup) noexcept;
// Forgets the hook, clears the thunk, the counters and the exports.
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::voipdrop
