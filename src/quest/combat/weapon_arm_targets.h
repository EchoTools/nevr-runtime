/* The pinned functions, symbols and vtable of weapon_arm (weapon_arm.h). Every address is an ELF virtual address in the store libr15 (build id in
 * pinned_targets.h, `LibR15PatchedBuildId`); the disassembly's address is the ELF address plus 0x100000. src/quest/tests/got_pinned_test.cpp checks
 * that each symbol is defined at its address (`just test-quest-hooks-pinned`, which needs the store APK). Nothing is hooked: the module calls nine
 * functions of the game. The first four of the nine are shared (netgame_targets.h holds CR15Game::NetGameMT).
 *
 * The functor layout the deferred route writes (CDeferredMethodQueue::SFunctor2<void, CR15NetPlayerEquipmentCS, SCompactPoolHandle, unsigned long>,
 * the station's hand swap): {cs @0, &SetWeaponArm @8, adjust 0 @0x10, handle @0x18, argument @0x20}, queued on the gamespace's queue at gs+0x170.
 * All of it is the game's own: the route is the game's SetWeaponArm script node (inside LookupDataBinding<SComponentMethodScriptNode3<...>> at 0x23df0f4 to
 * 0x23df164: the gamespace at cs+0x80, the direct flag at gs+0x368, the queue at gs+0x170, then the stores at +0, +8, +0x18 and +0x20 of the slot QueueFunctor
 * returns), the slot at cs+0x130 is what SavePlayerEquipment @ 0x217aedc reads (`ldrsh w8,[x0,#0x130]`), the generation table at cs+0xc8 is what IsComponentValid
 * @ 0x2238c04 reads, and the key is the record of the name `R14NetPlayerEquipment` that CR15NetPlayerEquipmentCS::DefaultName @ 0x2239834 returns (the 8 bytes at
 * 0x2bb0318). The words below pin each. QueueFunctor never returns null: it traps when the queue is full (0x24acd4c), so the module's null check is dead code.
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

inline constexpr std::uint64_t kEquipCsKey = 0xca14940c84b03f6eULL;  // the record of R14NetPlayerEquipment (the CS's DefaultName)

inline constexpr std::uintptr_t kCsSlot = 0x130, kCsGenerations = 0xc8, kCsGamespace = 0x80;
inline constexpr std::uintptr_t kGamespaceDirect = 0x368, kGamespaceQueue = 0x170;

inline constexpr pinned::Quad kQuads[] = {
    {"R14NetPlayerEquipment's record (the equipment system's key)", 0x2bb0318ULL, kEquipCsKey},
};
inline constexpr pinned::Word kWords[] = {
    {"CR15NetPlayerEquipmentCS::DefaultName: add x8,x8,#0x31e (the record's address)", 0x2239838ULL, 0x910c7908U},
    {"SetWeaponArm node: ldr x8,[x21,#0x80] (the gamespace)", 0x23df0f4ULL, 0xf94042a8U},
    {"SetWeaponArm node: ldr w9,[x8,#0x368] (the direct flag)", 0x23df100ULL, 0xb9436909U},
    {"SetWeaponArm node: add x0,x8,#0x170 (the queue)", 0x23df140ULL, 0x9105c100U},
    {"SetWeaponArm node: str w22,[x0,#0x18] (the handle)", 0x23df158ULL, 0xb9001816U},
    {"SetWeaponArm node: str x20,[x0,#0x20] (the argument)", 0x23df15cULL, 0xf9001014U},
    {"SetWeaponArm node: stur q0,[x0,#8] (the method and its adjustment)", 0x23df164ULL, 0x3c808000U},
    {"SavePlayerEquipment: ldrsh w8,[x0,#0x130] (the local slot)", 0x217aee8ULL, 0x79c26008U},
    {"IsComponentValid: ldr x8,[x0,#0xc8] (the generation table)", 0x2238c10ULL, 0xf9406408U},
    {"IsComponentValid: ldrh w9,[x9,#2] (the generation)", 0x2238c18ULL, 0x79400529U},
};
static_assert(pinned::LdrXOffset(kWords[1].word) == kCsGamespace, "the pinned word does not carry the gamespace link");
static_assert(pinned::LdrWOffset(kWords[2].word) == kGamespaceDirect, "the pinned word does not carry kGamespaceDirect");
static_assert(pinned::AddImmediate(kWords[3].word) == kGamespaceQueue, "the pinned word does not carry kGamespaceQueue");
static_assert(pinned::LdrWOffset(kWords[4].word) == 0x18 && pinned::LdrXOffset(kWords[5].word) == 0x20 && pinned::SturOffset(kWords[6].word) == 0x8,
              "the pinned stores are not the functor layout the module writes");
static_assert(pinned::LdrhOffset(kWords[7].word) == kCsSlot, "the pinned word does not carry kCsSlot");
static_assert(pinned::LdrXOffset(kWords[8].word) == kCsGenerations, "the pinned word does not carry kCsGenerations");
inline constexpr pinned::Pins kPins = {kWords, sizeof(kWords) / sizeof(kWords[0]), kQuads, sizeof(kQuads) / sizeof(kQuads[0])};

}  // namespace nevr_quest::combat::weapon_arm::targets
