/* A "clear all new unlocks" made while a profile update is unacknowledged is sent after the acknowledgement (#2410).
 *
 * What the game does (store libr15; addresses are the ELF vaddr, pinned_targets.h; the disassembly lists each 0x100000 higher).
 * CR15NetGame::RemoveAllNewlyUnlockedItems 0x1297114 zeroes the count of new unlocks (net game + 0x51c50), clears the client profile's "newunlocks"
 * key and sets bit 0 of the user's flag word (user = *(net game + 0x28d0), flag word user + 0x9c; CNSUser::SetDirty). SaveClientProfileChanges
 * then sends the pending changes, unless an update was sent and is not yet acknowledged: the buffer at user + 0x28 is not empty, so it merges
 * the changes into that buffer, sends nothing and clears the bit; the acknowledgement (ProfileUpdateSuccessCB) resets the buffer and sets the bit again
 * only when the pending changes at user + 0x18 are not empty. A clear leaves none there, so a clear made in that window is merged, discarded and
 * never sent (measured on PC: scenario clear_while_update_in_flight; the same code is in libr15.so 0x19331f8 and 0x1934104).
 *
 * README rules this module does not follow, and why. Rule 9 (a hook on a slot arena's scripts also call tests the level scope and does nothing outside combat): the
 * fix must act in every level, arena included, because it repairs the profile; the slot is called once per click (a menu action, never per frame), the game's
 * function runs first with the same arguments, and the reads that follow are bounded to the one clear. Rule 6 (the pinned slots in a `<module>_targets.h` with a
 * pinned test of its own): the pins are in sentinel/pinned_targets.h and checked by got_pinned_test.cpp, as the equipment re-fire's are.
 *
 * Origin (README rule 5). This repository's own fix for #2410, found by measurement (the PC scenarios clear_while_update_in_flight and
 * clear_after_ack); it ports no reference-build source and no other project's code.
 *
 * What this does. The shared rules (runtime/patch/new_unlocks_clear_rules.h, the one definition the PC build compiles too) decide: the clear
 * runs at once (the list empties on screen), and when it ran while the game's own CJson::Empty says the buffer at user + 0x28 is not empty, the
 * frame work asks that test once per frame and, once the acknowledgement has reset the buffer, sets the dirty bit, so the game sends the clear itself,
 * as it does when nothing is in flight. A held clear is dropped when an add came after it (the list is not empty again) and when the user is
 * gone; each ends in one sentinel line. This file is only the Quest side: the reads, the one call into the game, the lock, the log lines and the
 * hook (QuestEnv-style glue in new_unlocks_clear.cpp). SaveClientProfileChanges and the acknowledgement are NOT hooked: libr15.so, libpnsovr.so and
 * libpnsrad.so each define their own copies of them (and of CJson::Empty) and call them through their own PLT slots, the copy that runs on a headset
 * is not known from the files, and the user object's fields are the same in every copy, so the poll sets the dirty bit, a field they all read.
 *
 * Which words. The clear's address is held by two data words (pinned_targets.h, kNewUnlocksClear): the JUMP_SLOT a direct call goes through and the
 * GLOB_DAT word script nodes load. Both are hooked, each with its own thunk and its own stored original.
 *
 * The game thread. The frame work is registered with the frame dispatcher (invite/frame_dispatch.h), which runs on the game thread once per
 * CR15NetGame::Update from the hook the sentinel already holds on that function; no hook is added for it. A lock guards the hold because the
 * clear can be reached from a script node on another path.
 *
 * Install. The build ID, then the code words in kNewUnlocksClearCode, then the two slots, each checked against base + the clear's address (a word that
 * holds anything else is refused on its own and logged by GotHook with its chain). A pin that differs refuses the module and changes nothing.
 *
 * Which APKs (docs/adr/0046 rule 5). This ships to every APK, the release APK and the variants alike, and is not gated by combat::Active()
 * (combat_active.h): it installs on every launch (entry.cpp, InstallPermissionWaitHook).
 *
 * Frequency (ADR 0038). The clear runs once per click. The frame work costs one lock and one pointer test per frame while nothing is held. While a clear is
 * held it compares the frame hook's net game and the net game's user pointer with the held ones, reads the count and calls the game's CJson::Empty once, with
 * no mapping scan (the pages were checked once, when the hold began): measured on the host, 10 ns per frame with nothing held and a few tens of ns
 * held (the first version scanned /proc/self/maps twice per frame, 28 us). A hold ends at the acknowledgement, when an add follows, at session end (the
 * frame hook's net game is no longer the one the clear was made on), at logout (the net game's user pointer changed), or when the next clear replaces it.
 * The cost was not measured on a headset.
 *
 * Owner (ADR 0039). This file owns sending a clear made while an update is unacknowledged. The argument and the effect of the game's clear are not changed.
 *
 * Lines (sentinel log):
 *   install  {"event":"combat_new_unlocks_clear_install","result":"installed|partial|refused","jump_slot":<GotStatus>,"glob_dat":<GotStatus>,
 *             "consequence":".."} or, when nothing was tried, {"event":..,"result":"refused","why":"module_not_loaded|no_build_id|
 *             build_id_mismatch|pin_mismatch|frame_work_full","pin":"<name>" (pin_mismatch only),"consequence":".."}
 *   held / rearmed / superseded / dropped
 *            {"event":"combat_new_unlocks_clear_<held|rearmed|superseded|dropped>","msg":"<the PC [NEVR.UNLOCKS] line's words>","user_id":<id>,
 *             "count":<how many new unlocks the clear removed>}  level info; a held clear ends in one of the other three.
 *
 * Not verified: any run on a headset; which copy of SaveClientProfileChanges a headset runs; that user_id (the social layer's id of the local
 * account) is set by the time of a clear (0 when it is not).
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::combat::new_unlocks_clear {

inline constexpr std::size_t kWordCount = 2;  // JUMP_SLOT, GLOB_DAT

// Installs the module over the live libr15 (entry.cpp). True when both words went in. A refusal is logged and changes nothing.
bool Install() noexcept;

// The same over `lookup` (a host test's image). Registers the frame work once per process.
bool InstallAt(sentinel::ImageLookup lookup) noexcept;

// The name of an install result: "installed" when every word went in, "partial" when some did, "refused" when none did.
const char* InstallResultName(std::size_t installed) noexcept;

// One frame's work: re-arms a held clear whose update was acknowledged. What the registered frame work calls; public for the host test.
void PollNow() noexcept;

// The game function the module calls, published at install: CJson::Empty. A test supplies its own (base + the pinned address in production).
struct GameFunctions {
  std::uintptr_t jsonEmpty = 0;
};
GameFunctions PublishedGameFunctions() noexcept;
void SetGameFunctionsForTest(const GameFunctions* functions) noexcept;  // null: back to base + the pinned address
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::new_unlocks_clear
