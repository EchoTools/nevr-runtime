// equiprefire (issue #2121; default ON, option Q15T_EQUIPREFIRE): the equipment change the game drops is applied again once the slot has been
// still for kSettleMs. The game's scripts drop a change event that arrives while they are still handling the previous one; the record
// already holds the new value, so the HUD shows the last choice and the game uses an earlier one. The module hooks the three equipment slot
// setters of CR15NetPlayerEquipmentCS (SetWeaponSlot, SetAbilitySlot, SetGrenadeSlot), tracks the LOCAL player's changes, and from the game
// tick calls the slot's change handler (HandleWeaponChange, HandleAbilityChange, HandleGrenadeChange) once more, never the setter.
//
// The decision logic (the debounce, the record and actor reads, the local player's slot handle) is src/runtime/patch/equipment_refire_rules.h,
// the one definition the PC build (src/runtime/patch/equipment_refire.cpp) shares. This file is the Quest glue: the six data words, the
// pins, the reads, the handler calls. Details, evidence and prediction: docs/quest/q15t/modules/equiprefire.md.
#pragma once

#include <cstddef>
#include <cstdint>

#include "import_hook.h"
#include "word_pins.h"

namespace q15t {

// ELF vaddrs of the pinned build (b243509c08ce677aeb95fa348016949b3fc45230, disassembly VA = ELF + 0x100000), slot index 0 weapon, 1 tac mod
// (ability), 2 ordnance (grenade).
struct EquipRefireAddrs {
  std::uintptr_t setter[3];       // CR15NetPlayerEquipmentCS::SetWeaponSlot / SetAbilitySlot / SetGrenadeSlot (the HOOK points)
  std::uintptr_t handler[3];      // HandleWeaponChange / HandleAbilityChange / HandleGrenadeChange (what a re-fire CALLS)
  std::uintptr_t jump_slot[3];    // the JUMP_SLOT word that holds each setter (the PLT stub's GOT slot)
  std::uintptr_t glob_dat[3];     // the GLOB_DAT word that holds each setter
  std::uintptr_t net_game;        // NetGameMT: x0 = *(ctx + 8) -> the net game
  std::uintptr_t user_id;         // CR15NetGame::UserId(game, key0, key1) -> the slot handle of the player with that actor key
};
inline constexpr EquipRefireAddrs kEquipRefirePinned = {
    {0x217ca84, 0x217cde4, 0x217cf90},
    {0x217afb4, 0x217b124, 0x217b1dc},
    {0x36f3d00, 0x36fa900, 0x36d8e10},
    {0x37233e8, 0x3727b38, 0x3711db8},
    0x12027d4,
    0x125a9b8,
};

// What the module calls, as absolute addresses: the three handlers and the two lookups. The host tests give host functions.
struct EquipRefireFns {
  std::uintptr_t handler[3];
  std::uintptr_t net_game;
  std::uintptr_t user_id;
};
// base + the pinned vaddrs.
EquipRefireFns EquipRefireFnsAt(std::uintptr_t base, const EquipRefireAddrs& addrs);

// The code words the module's reads and calls were derived from, read before any word is swapped (24): each setter's first word and the
// three reads it makes (the index table +0xc8, the records +0xf8, the slot's value field +0x18 / +0x20 / +0x28), each handler's first word and
// its +0xc read of the index table, NetGameMT whole, and the first words of UserId up to its read of the slot count. No MD5 or other
// file-hash gate.
inline constexpr WordPin kEquipRefireWordPins[] = {
    // SetWeaponSlot
    {0x217ca84, 0xd10243ff},  // sub sp,sp,#0x90
    {0x217ca9c, 0xf9406408},  // ldr x8,[x0,#0xc8]    (the index table)
    {0x217caa8, 0xf9407c1a},  // ldr x26,[x0,#0xf8]   (the records)
    {0x217cab8, 0xf8418d09},  // ldr x9,[x8,#0x18]!   (the weapon value)
    // SetAbilitySlot
    {0x217cde4, 0xd10243ff},
    {0x217cdfc, 0xf9406408},
    {0x217ce08, 0xf9407c1a},
    {0x217ce18, 0xf8420d09},  // ldr x9,[x8,#0x20]!   (the tac mod value)
    // SetGrenadeSlot
    {0x217cf90, 0xd101c3ff},  // sub sp,sp,#0x70
    {0x217cfa0, 0xf9406408},
    {0x217cfac, 0xf9407c16},  // ldr x22,[x0,#0xf8]
    {0x217cfbc, 0xf8428d09},  // ldr x9,[x8,#0x28]!   (the ordnance value)
    // the handlers: stp x23,x22,[sp,#-0x30]! and the index-table read
    {0x217afb4, 0xa9bd5bf7},
    {0x217afc0, 0xf9406408},
    {0x217b124, 0xa9bd5bf7},
    {0x217b130, 0xf9406408},
    {0x217b1dc, 0xa9bd5bf7},
    {0x217b1e8, 0xf9406408},
    // NetGameMT: mov w8,#0x8510; ldr x0,[x0,x8]; ret
    {0x12027d4, 0x5290a208},
    {0x12027d8, 0xf8686800},
    {0x12027dc, 0xd65f03c0},
    // CR15NetGame::UserId: sub sp,sp,#0x40; stp x1,x2,[sp] (the actor key); ldrh w8,[x0,#0xdc] (the slot count)
    {0x125a9b8, 0xd10103ff},
    {0x125a9c4, 0xa9000be1},
    {0x125a9c8, 0x7941b808},
};
inline constexpr std::size_t kEquipRefireWordPinCount = sizeof kEquipRefireWordPins / sizeof kEquipRefireWordPins[0];
bool EquipRefireWordPinsMatch(std::uintptr_t base);

// Gate order: `equiprefire.off` in the plugin data directory (reason "off_file", one INFO line, nothing changed) -> the code-word pins (reason
// "word_pin_mismatch", one ERROR line: pin / found / expected; nothing swapped) -> the six swaps. Each of the six data words (a JUMP_SLOT and
// a GLOB_DAT per setter) is swapped on its own, checked against base + the setter's vaddr, with its own stored original: a word that holds
// something else is refused alone (WARNING) and the others stay installed. Installs when at least one word was swapped; none: the status
// of the first refusal. Register EquipRefireTick with AddGameTick separately (plugin.cpp), only when this returns kOk.
InstallResult InstallEquipRefire(std::uintptr_t base);
// The same without the kill file and the pins, on other addresses and other functions (host tests, where the "functions" are host code).
InstallResult InstallEquipRefireAt(std::uintptr_t base, const EquipRefireAddrs& addrs, const EquipRefireFns& fns);

// A TickFn (game_tick.h), every frame on the game thread: calls the handler of each slot whose last change by the local player is older
// than kSettleMs (equipment_refire_rules.h). One bool test per frame when nothing is installed or nothing is pending.
void EquipRefireTick(std::uintptr_t game);

struct EquipRefireStats {
  unsigned tracked = 0;    // changes of the local player's record that are waiting for stillness
  unsigned unchanged = 0;  // setter calls given the value the record already held
  unsigned not_local = 0;  // changes of another player's record (never re-fired)
  unsigned refired = 0;    // handler calls made
  unsigned skipped = 0;    // due re-fires refused because the component or the record had changed under them
  bool on = false;
};
EquipRefireStats GetEquipRefireStats();

// Test seams: a millisecond clock (nullptr: CLOCK_MONOTONIC); a reset of every piece of state (the kill-file directory is
// module_switch.h's SetModuleSwitchDirForTest).
void SetEquipRefireClockForTest(std::uint64_t (*clock_ms)());
void ResetEquipRefireForTest();

}  // namespace q15t
