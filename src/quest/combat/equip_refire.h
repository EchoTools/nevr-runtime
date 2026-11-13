/* An equipment change the game's scripts dropped is applied again once the slot has been still (#2121).
 *
 * What the game does (store libr15; addresses are the ELF vaddr, pinned_targets.h; the disassembly lists each 0x100000 higher).
 * CR15NetPlayerEquipmentCS keeps one 0x58-byte record per player (index table at component+0xc8, record array at +0xf8, link table at
 * +0x100, context at +0x80). Three setters take (component, handle, value) and write one slot of the record only when the value differs:
 * SetWeaponSlot 0x217ca84 (record+0x18), SetAbilitySlot 0x217cde4 (+0x20), SetGrenadeSlot 0x217cf90 (+0x28). When the value
 * changed, a setter then sends the slot's equipment-changed event to the scripts that arm the slot: SetWeaponSlot and SetAbilitySlot
 * do the handler's work inline (the send is `bl 0x104a800`, the stub of CncaGameSpace::SendComponentEventGlobal, unconditional), and
 * SetGrenadeSlot calls HandleGrenadeChange. The slot's change handler is HandleWeaponChange 0x217afb4, HandleAbilityChange 0x217b124 or
 * HandleGrenadeChange 0x217b1dc: void(component, handle), it reads the value from the record and sends the event.
 * The record's +0x10 and +0x14 words gate neither the send nor the handler: in the handlers and in the setters a zero +0x10 only skips
 * `bl 0xff81d0` (RegisterExclusiveUpdate), and in the setters +0x10 and +0x14 both zero skips the tail that broadcasts the update
 * (Broadcast<SR15NetPlayerEquipmentUpdate>, disassembly 0x2404244). So a handler called on its own sends the event whatever they hold.
 * The scripts drop an event that arrives while they are still handling the previous one (#2121: the ability core's change handler, "the
 * earlier tac mod is armed"); the record already holds the new value, so the HUD shows the last choice and the game uses an earlier one.
 * A tester reported the same for weapons and ordnance. The only thing lost is the event, so sending it once more when nothing is in
 * flight restores the state the record already has.
 *
 * Which words. A setter's address is held by two kinds of data word (pinned_targets.h, kEquipmentSetters): the JUMP_SLOT a direct call
 * goes through and the GLOB_DAT word a queued call's functor is built from (R15NetPlayerEquipmentSetAbilitySlotNode and
 * CR15NetPlayerSetEquipmentNode::Enter load it). `readelf -rW` on the store file names the setters in exactly these six relocations, and
 * all six are hooked, so a change reaches the wrapper whichever way the game made it. The hook points are the setters; the handlers
 * are what a re-fire calls, never hooked, so a re-fire cannot start another debounce.
 *
 * What this does. Each wrapper reads the record's value BEFORE the setter runs, calls the setter with the same three arguments, and
 * hands the call to the shared rule (runtime/patch/equipment_refire_rules.h, the one definition the PC build compiles too): was the
 * value changed, is the record the local player's (the net game's slot 0: NetGameMT 0x12027d4 and CR15NetGame::UserId 0x125a9b8, the
 * game's own lookups, called through their pinned addresses), and, for the local player's change, when it happened. After the slot has
 * been still for kSettleMs (2500 ms, longer than the ~2 s chain a change starts) the frame work calls the slot's handler once, on the
 * game thread, unless the component, the record or the choice changed under it (StaleReason). Another player's record, an unchanged
 * value and an unreadable record are never re-fired. This file is only the Quest side of that rule: the reads, the two lookups, the
 * handler calls, the clock, the lock and the log lines (QuestEnv in equip_refire.cpp).
 *
 * The game thread. The frame work is registered with the frame dispatcher (invite/frame_dispatch.h), which runs on the game thread once
 * per CR15NetGame::Update, from the hook the sentinel already holds on that function; no hook is added for it.
 *
 * Install. The build ID, then the code words in kEquipmentRefireCode (the reads the wrappers repeat and the two lookups' bodies), then
 * the six slots, each checked against base + the setter's address (a word that holds anything else, such as a word another runtime
 * already replaced, is refused on its own and logged by GotHook with its chain); a slot that is refused leaves the game's call through
 * it as it is. A pin that differs refuses the module and changes nothing.
 *
 * Which APKs (docs/adr/0047). It installs only where combat::Active() is true (combat_active.h): the constructor sequence's own step
 * `install_equip_refire` (integration/ctor_sequence.h), right after the name tags. An arena or plain release skips it
 * (`combat_inactive`) and keeps the game's own setters.
 *
 * Frequency (ADR 0038). The setters run once per loadout event (a station button press, a bot cycling its gun, the game restoring a
 * loadout), reached from script nodes and the deferred queue, never per frame (the callers were read in ReVault; calls per frame were not
 * measured). A wrapper's cost is at most 21 reads (6 for an unchanged value), each a scan of /proc/self/maps (mapping_perms.h), and a lock per
 * call; the per-frame cost is the frame work: one lock, one clock read and an empty-table test, once per CR15NetGame::Update. The
 * cost of a read was not measured on a headset.
 *
 * Owner (ADR 0039). This file owns re-sending the slot's change event after the local player's last change of a slot. The arguments and
 * the result of the setters are not changed.
 *
 * Lines (sentinel log):
 *   install  {"event":"combat_equip_refire_install","result":"installed|partial|refused","weapon_jump":<GotStatus>,"weapon_glob":..,
 *             "tac_mod_jump":..,"tac_mod_glob":..,"ordnance_jump":..,"ordnance_glob":..,"settle_ms":2500,"consequence":".."}
 *            or, when nothing was tried, {"event":"combat_equip_refire_install","result":"refused","why":"module_not_loaded|no_build_id|
 *            build_id_mismatch|pin_mismatch|frame_work_full","pin":"<name>" (pin_mismatch only),"consequence":".."}
 *   change   {"event":"combat_equip_refire_change","msg":"equipment change","slot":"weapon|tac_mod|ordnance",
 *             "word":"jump_slot|glob_dat","verdict":"tracked|unchanged|not_local|unreadable","handle":"0x%08x",
 *             "value":"0x%016llx","read":"yes|no","changed":"yes|no","local":"yes|no"}
 *            level debug, one per setter call, the first kChangeLines of the process; then one combat_equip_refire_change_cap
 *            warning says the rest are counted, not written.
 *   refired  {"event":"combat_equip_refire_refired","msg":"equipment re-fired (..)","slot":..,"value":..,"handle":..,"changes":<n>,
 *             "quiet_ms":<n>}  after the handler returned
 *   skipped  {"event":"combat_equip_refire_skipped","msg":"equipment re-fire skipped (..)","slot":..,"value":..,"handle":..,
 *             "reason":"component_unreadable|component_replaced|record_gone|record_changed_elsewhere|not_local"}
 *   The change, refired and skipped lines carry the PC `[NEVR.EQUIP]` line's field names (the change line adds `word`). The change line
 *   is written after the game's setter returned and the other two from the frame work, so no logging frame is live while the game's
 *   setter or a change handler runs (callback_thunk.h rule 1).
 *
 * Checked against the library: `readelf -rW` on the libr15 of a release APK (build ID fe49d0fe5a0555e3d718bc4271d8602dde81a30e) lists the
 * setters in exactly the six relocations of pinned_targets.h, and the sixteen code pins equal the words at their addresses in that file.
 * got_pinned_test.cpp makes both checks on the store file (it needs the store APK).
 *
 * The reference runtime's libvrapi.so. Its strings name no setter of this module, and none of the six words' or three setters' addresses
 * occurs in it as an 8-byte or 4-byte constant, a movz/movk pair or an add-by-12-bit pair (a static scan of the release APK's file; the
 * file's other modules do hook libr15 words, ADR 0034, so this is a scan, not a disassembly). A word it had replaced before this install
 * is refused on its own (kOriginalMismatch, logged by GotHook with the chain). Its log strings show a check of the stock value before it
 * writes a word ("GOT slot ... doesn't hold it"), which an installed thunk fails, so a word it replaced later would be refused by it.
 *
 * Not verified: any run on a headset; kSettleMs on Quest (the first log's quiet_ms is the measurement); that slot 0 is the local player
 * on Quest (the rule's assumption, from CR15NetGame::UserId and, on PC, SetTier; it fails closed); the cost of a read on the device.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::combat::equip_refire {

inline constexpr std::size_t kSlotCount = 3;                 // weapon, tac mod, ordnance
inline constexpr std::size_t kWordsPerSlot = 2;              // JUMP_SLOT, GLOB_DAT
inline constexpr std::size_t kWordCount = kSlotCount * kWordsPerSlot;
inline constexpr std::size_t kChangeLines = 256;             // per-call debug records written per process, then counted only

// "installed" when all six words took their thunk, "partial" when some did, "refused" when none did.
const char* InstallResultName(std::size_t installed) noexcept;

// Installs on the libr15 the lookup finds. True when all six words took their thunk. Logs one combat_equip_refire_install line.
bool InstallAt(sentinel::ImageLookup lookup) noexcept;
// InstallAt on the loaded libr15.
bool Install() noexcept;

// The frame work: fires what is due (the rule's FireDue). The dispatcher calls it on the game thread; a test calls it directly.
void FireDueNow() noexcept;

struct Counts {
  std::uint64_t tracked;    // changes of the local player's record that wait for stillness
  std::uint64_t unchanged;  // setter calls given the value the record already held
  std::uint64_t notLocal;   // changes of another player's record
  std::uint64_t unreadable; // calls whose record could not be read
  std::uint64_t refired;    // handler calls made
  std::uint64_t skipped;    // due re-fires refused because something changed under them
};
Counts CurrentCounts() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

// Replaces the millisecond clock (nullptr: CLOCK_MONOTONIC).
void SetClockForTest(std::uint64_t (*clock)() noexcept) noexcept;
// The five game functions the module calls, as addresses: the three change handlers (weapon, tac mod, ordnance), NetGameMT and
// CR15NetGame::UserId. Production calls base + the pinned vaddrs (pinned_targets.h); a host test, whose image holds the pinned
// words at those vaddrs and cannot hold callable host code there, supplies its own after the pin proof.
struct GameFunctions {
  std::uintptr_t handler[kSlotCount];
  std::uintptr_t netGame;
  std::uintptr_t userId;
};
// nullptr restores base + the pinned vaddrs. Takes effect at the next InstallAt.
void SetGameFunctionsForTest(const GameFunctions* functions) noexcept;
// The addresses the last InstallAt published (all zero before one). A test with no stand-ins reads the production ones here.
GameFunctions PublishedGameFunctions() noexcept;
// Forgets the six hooks, clears the thunks, the tracker, the counters and the log budget, and forgets that the frame work was
// registered (reset the dispatcher with invite::frame::ResetForTest first).
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::equip_refire
