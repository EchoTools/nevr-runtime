/* The pinned functions, symbol and code words of gunconnect (gunconnect.h). Every address is an ELF virtual address in the store libr15 (build id in
 * pinned_targets.h, `LibR15PatchedBuildId`); the disassembly's address is the ELF address plus 0x100000. src/quest/tests/got_pinned_test.cpp checks
 * that each symbol is defined at its address and every word is the file's (`just test-quest-hooks-pinned`, which needs the store APK). Nothing is
 * hooked: the module calls the three functions and reads the one symbol.
 *
 *   CR15NetGunCS::Connect(SR15NetUserId) @ 0x2153438 (484 bytes)   the loop over the gun CS's guns that Connect itself runs: count u16 at cs+0xf2,
 *       instances u64 at cs+0xf8 (stride 0x168, the property index u16 at inst+0xa0), properties u64 at [cs+0xb8] (stride 0xa0), the owner slot u64 at
 *       props+0x20 compared with `userId & 0xffff`, a flag u32 at props+0x9c
 *   CR15NetGame::UserActor(u16) @ 0x1251654   member stride 0x250, the 16-byte actor GUID at +0x180, returned in x0/x1
 *   CTagCS::HasTag(CncaGameMT*, const SActorGUID&, CSymbol64) @ 0x1723760   the GUID is two u64 loaded from the reference, the tag is passed by value in x2
 *   CR15ActorUtils::kActorTagCombat @ 0x37af288   a CSymbol64 (8 bytes), read
 *   CR15NetGunCS's vtable @ 0x3696040; an object's first word is the vtable + 0x10
 *   CR15NetGame::SendLobbyMemberEvent @ 0x125168c builds the same user id and bounds: member count u16 at net+0xe2, a second count u16 at net+0xdc, the slot
 *       u16 at member+0x178 masked with 0x1f, the generation u16 at net + slot*4 + 0x13a, the id `slot | gen << 16`
 *
 * [UNVERIFIED] Not read in the disassembly, kept as the reference build has them (afk_gunconnect.cpp:91-93 and the passes below it): the gun CS's
 * key 0x7a804a0a9b2f5386, net+0xde, net+0x138 (the member index of a slot), inst+0xd8 (the gun's state: 3 never processed, 1 disconnected, 0
 * connected) and cs+0x1c0 (the connect ring count). They are the conditions the reference build adds to Connect's own; Connect does not read them.
 */
#pragma once

#include <cstdint>

#include "quest/combat/pinned_words.h"

namespace nevr_quest::combat::gunconnect::targets {

inline constexpr const char* kConnectSymbol = "_ZN10NRadEngine8NRadGame12CR15NetGunCS7ConnectENS0_13SR15NetUserIdE";
inline constexpr std::uint64_t kConnectVaddr = 0x2153438ULL;
inline constexpr const char* kHasTagSymbol = "_ZN10NRadEngine6CTagCS6HasTagEPNS_10CncaGameMTERKNS_10SActorGUIDENS_9CSymbol64E";
inline constexpr std::uint64_t kHasTagVaddr = 0x1723760ULL;
inline constexpr const char* kTagCombatSymbol = "_ZN10NRadEngine8NRadGame14CR15ActorUtils15kActorTagCombatE";
inline constexpr std::uint64_t kTagCombatVaddr = 0x37af288ULL;
inline constexpr const char* kGunCsVtableSymbol = "_ZTVN10NRadEngine8NRadGame12CR15NetGunCSE";
inline constexpr std::uint64_t kGunCsVtableVaddr = 0x3696040ULL;
inline constexpr std::uint64_t kGunCsVptrVaddr = kGunCsVtableVaddr + 0x10;  // an object's first word

inline constexpr std::uint64_t kGunCsKey = 0x7a804a0a9b2f5386ULL;  // [UNVERIFIED] the reference build's constant

// The gun CS (Connect's loop).
inline constexpr std::uintptr_t kGunCount = 0xf2, kInstances = 0xf8, kProperties = 0xb8, kInstanceStride = 0x168, kPropertyIndex = 0xa0;
inline constexpr std::uintptr_t kPropertyStride = 0xa0, kPropertyOwner = 0x20, kPropertyFlag = 0x9c;
// [UNVERIFIED] The reference build's own conditions.
inline constexpr std::uintptr_t kInstanceState = 0xd8, kConnectRing = 0x1c0;
// The local member (SendLobbyMemberEvent's id).
inline constexpr std::uintptr_t kMemberCount = 0xe2, kMemberCountCheck = 0xdc, kMemberRowSlot = 0x178, kSlotGeneration = 0x13a, kSlotStride = 4;
inline constexpr std::uint32_t kSlotMask = 0x1f;
inline constexpr std::uintptr_t kMemberStride = 0x250, kMemberGuid = 0x180;
// [UNVERIFIED] The reference build's: net+0xde (non-zero), net+0x138 (a slot's member index).
inline constexpr std::uintptr_t kNetReady = 0xde, kSlotMember = 0x138;

inline constexpr pinned::Word kWords[] = {
    {"Connect: ldrh w20,[x0,#0xf2] (guns)", 0x2153454ULL, 0x7941e414U},
    {"Connect: mov w29,#0xa0 (property stride)", 0x21534a4ULL, 0x5280141dU},
    {"Connect: mov w20,#0xa0 (property index offset)", 0x21534a8ULL, 0x52801414U},
    {"Connect: add x20,x20,#0x168 (instance stride)", 0x21534b4ULL, 0x9105a294U},
    {"Connect: ldr x9,[x19,#0xf8] (instances)", 0x21534c4ULL, 0xf9407e69U},
    {"Connect: ldr x10,[x19,#0xb8] (properties)", 0x21534c8ULL, 0xf9405e6aU},
    {"Connect: ldrh w9,[x9,x20] (property index)", 0x21534ccULL, 0x78746929U},
    {"Connect: ldr x10,[x9,#0x20] (owner)", 0x21534dcULL, 0xf940112aU},
    {"Connect: ldr w9,[x9,#0x9c] (flag)", 0x21534e8ULL, 0xb9409d29U},
    {"UserActor: mov w9,#0x250 (member stride)", 0x1251660ULL, 0x52804a09U},
    {"UserActor: ldr q0,[x8,#0x180] (actor GUID)", 0x1251668ULL, 0x3dc06100U},
    {"HasTag: sub sp,sp,#0x30", 0x1723760ULL, 0xd100c3ffU},
    {"HasTag: ldr x1,[x20] (GUID, low)", 0x1723778ULL, 0xf9400281U},
    {"HasTag: ldr x2,[x20,#8] (GUID, high)", 0x172378cULL, 0xf9400682U},
    {"SendLobbyMemberEvent: ldrh w8,[x0,#0xe2] (member count)", 0x12516a0ULL, 0x7941c408U},
    {"SendLobbyMemberEvent: ldrh w8,[x20,#0xdc]", 0x1251710ULL, 0x7941ba88U},
    {"SendLobbyMemberEvent: ldrh w8,[x8,#0x178] (slot)", 0x1251724ULL, 0x7942f108U},
    {"SendLobbyMemberEvent: and x8,x8,#0x1f", 0x1251728ULL, 0x92401108U},
    {"SendLobbyMemberEvent: add x9,x20,x8,lsl #2", 0x125172cULL, 0x8b080a89U},
    {"SendLobbyMemberEvent: ldrh w9,[x9,#0x13a] (generation)", 0x1251730ULL, 0x79427529U},
};

static_assert(pinned::LdrhOffset(kWords[0].word) == kGunCount, "the pinned word does not carry kGunCount");
static_assert(pinned::MovImmediate(kWords[1].word) == kPropertyStride, "the pinned word does not carry kPropertyStride");
static_assert(pinned::MovImmediate(kWords[2].word) == kPropertyIndex, "the pinned word does not carry kPropertyIndex");
static_assert(pinned::AddImmediate(kWords[3].word) == kInstanceStride, "the pinned word does not carry kInstanceStride");
static_assert(pinned::LdrXOffset(kWords[4].word) == kInstances, "the pinned word does not carry kInstances");
static_assert(pinned::LdrXOffset(kWords[5].word) == kProperties, "the pinned word does not carry kProperties");
static_assert(pinned::LdrXOffset(kWords[7].word) == kPropertyOwner, "the pinned word does not carry kPropertyOwner");
static_assert(pinned::LdrWOffset(kWords[8].word) == kPropertyFlag, "the pinned word does not carry kPropertyFlag");
static_assert(pinned::MovImmediate(kWords[9].word) == kMemberStride, "the pinned word does not carry kMemberStride");
static_assert(pinned::LdrQOffset(kWords[10].word) == kMemberGuid, "the pinned word does not carry kMemberGuid");
static_assert(pinned::LdrhOffset(kWords[14].word) == kMemberCount, "the pinned word does not carry kMemberCount");
static_assert(pinned::LdrhOffset(kWords[15].word) == kMemberCountCheck, "the pinned word does not carry kMemberCountCheck");
static_assert(pinned::LdrhOffset(kWords[16].word) == kMemberRowSlot, "the pinned word does not carry kMemberRowSlot");
static_assert(pinned::LdrhOffset(kWords[19].word) == kSlotGeneration, "the pinned word does not carry kSlotGeneration");

}  // namespace nevr_quest::combat::gunconnect::targets
