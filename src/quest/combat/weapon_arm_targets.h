/* The pinned functions, symbols and vtable of weapon_arm (weapon_arm.h). Every address is an ELF virtual address in the store libr15 (build id in
 * pinned_targets.h, `LibR15PatchedBuildId`); the disassembly's address is the ELF address plus 0x100000. src/quest/tests/got_pinned_test.cpp checks
 * that each symbol is defined at its address (`just test-quest-hooks-pinned`, which needs the store APK). Nothing is hooked: the module calls nine
 * functions of the game. The first four of the nine are shared (netgame_targets.h holds CR15Game::NetGameMT).
 *
 * The functor layout the deferred route writes (CDeferredMethodQueue::SFunctor2<void, CR15NetPlayerEquipmentCS, SCompactPoolHandle, unsigned long>,
 * the station's hand swap): {cs @0, &SetWeaponArm @8, adjust 0 @0x10, handle @0x18, argument @0x20}, queued on the gamespace's queue at gs+0x170.
 * [UNVERIFIED] The layout, gs+0x368 (the gamespace's direct flag), gs+0x170, the equipment CS's slot at cs+0x130 and generation table at cs+0xc8, and
 * the equipment key 0xca14940c84b03f6e are the reference build's (afk_gunconnect.cpp:496-520, T-26); the store file's code that uses them was not
 * traced for this move.
 */
#pragma once

#include <cstdint>

#include "quest/combat/pinned_words.h"

namespace nevr_quest::combat::weapon_arm::targets {

using Pin = pinned::Symbol;

inline constexpr Pin kClientProfile = {"_ZNK10NRadEngine8NRadGame11CR15NetGame13ClientProfileEv", 0x1254698ULL};
inline constexpr Pin kTString = {"_ZNK10NRadEngine5CJson7TStringEPKcS2_j", 0xfa2e7cULL};
inline constexpr Pin kJsonInt = {"_ZNK10NRadEngine5CJson3IntEPKcxj", 0xfa40ecULL};
inline constexpr Pin kIsValid = {"_ZNK10NRadEngine8NRadGame24CR15NetPlayerEquipmentCS16IsComponentValidENS_18SCompactPoolHandleE", 0x2238c04ULL};
inline constexpr Pin kGetArm = {"_ZNK10NRadEngine8NRadGame24CR15NetPlayerEquipmentCS12GetWeaponArmENS_18SCompactPoolHandleE", 0x217d100ULL};
inline constexpr Pin kSetArm = {"_ZN10NRadEngine8NRadGame24CR15NetPlayerEquipmentCS12SetWeaponArmENS_18SCompactPoolHandleEy", 0x217cc30ULL};
inline constexpr Pin kInvoke = {
    "_ZN10NRadEngine20CDeferredMethodQueue9SFunctor2IvNS_8NRadGame24CR15NetPlayerEquipmentCSENS_18SCompactPoolHandleEyE12InvokeMethodERS0_Pc",
    0x24acf18ULL};
inline constexpr Pin kQueue = {
    "_ZN10NRadEngine20CDeferredMethodQueue12QueueFunctorINS0_9SFunctor2IvNS_8NRadGame24CR15NetPlayerEquipmentCSENS_18SCompactPoolHandleEyEEEEPT_PFvRS0_PcE",
    0x24acce4ULL};

inline constexpr const char* kEquipCsVtableSymbol = "_ZTVN10NRadEngine8NRadGame24CR15NetPlayerEquipmentCSE";
inline constexpr std::uint64_t kEquipCsVtableVaddr = 0x36981f0ULL;
inline constexpr std::uint64_t kEquipCsVptrVaddr = kEquipCsVtableVaddr + 0x10;  // an object's first word

inline constexpr std::uint64_t kEquipCsKey = 0xca14940c84b03f6eULL;  // [UNVERIFIED] R14NetPlayerEquipment (the CS's DefaultName)

inline constexpr std::uintptr_t kCsSlot = 0x130, kCsGenerations = 0xc8, kCsGamespace = 0x80;  // [UNVERIFIED]
inline constexpr std::uintptr_t kGamespaceDirect = 0x368, kGamespaceQueue = 0x170;           // [UNVERIFIED]

}  // namespace nevr_quest::combat::weapon_arm::targets
