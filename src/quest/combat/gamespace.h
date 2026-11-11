/* The game's component systems, found in its two gamespace tables: the lookup gunconnect and weapon_arm share (and powerboost, when it moves,
 * #2187). The game thread reads CR15Game's two gamespace tables ({sym, gs*} rows of 16 bytes) and, in each gamespace of the game, its list of
 * {key, component system*} rows. Every read is a SafeRead (process_vm_readv), so a stale or half-built table is a skipped entry, never a fault.
 *
 * Offsets. The two tables are the ones combat_level_targets.h pins. A gamespace's list is at gs+0x420 with its count at gs+0x450, a row is 16 bytes, the
 * gamespace's game is at gs+0x8 (pinned: combat_level_targets.h) and a component system's gamespace is at cs+0x80; gamespace_targets.h ties the three to the game's
 * own lookup.
 *
 * Origin. The reference build's `FindCs` (kotorvr's echoquest-combat repository, commit c5a67e8, runtime/native/afkxr/afk_gunconnect.cpp:186-218),
 * the only copy in the tree.
 *
 * Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <cstdint>

namespace nevr_quest::combat::gamespace {

struct CompSys {
  std::uintptr_t gs = 0;      // the gamespace
  std::uintptr_t cs = 0;      // the component system
  int table = 0;              // 0: game+0x1f48 (gets exclusive updates), 1: game+0x1f88
  std::uint64_t gsName = 0;
};

// Every component system in both tables whose key is `key` and whose vptr word is `vptrAbs` (an absolute address: base + the ELF vaddr), and whose
// cs+0x80 is its gamespace. At most `max` entries; returns the count.
int FindComponentSystems(std::uintptr_t game, std::uint64_t key, std::uintptr_t vptrAbs, CompSys* out, int max) noexcept;

}  // namespace nevr_quest::combat::gamespace
