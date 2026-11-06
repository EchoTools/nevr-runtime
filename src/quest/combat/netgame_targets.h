/* The CR15Game -> CR15NetGame step that gunconnect, aibots and weapon_arm share (docs/adr/0046), pinned to the store libr15 (build id in
 * pinned_targets.h, `LibR15PatchedBuildId`; the disassembly's address is the ELF address plus 0x100000).
 *
 *   CR15Game::NetGameMT() const @ 0x12027d4   `mov w8,#0x8510; ldr x0,[x0,x8]`   game + 0x8510 is the CR15NetGame
 *   CR15NetGame::UserActor(u16) @ 0x1251654   `ldr x0,[x0,#8]`                   net + 8 is the game (the back pointer aibots checks)
 *
 * got_pinned_test.cpp checks that both symbols are defined at these addresses and every word below is the file's (`just test-quest-hooks-pinned`).
 */
#pragma once

#include <cstdint>

#include "quest/combat/pinned_words.h"

namespace nevr_quest::combat::netgame {

inline constexpr const char* kNetGameMTSymbol = "_ZNK10NRadEngine8NRadGame8CR15Game9NetGameMTEv";
inline constexpr std::uint64_t kNetGameMTVaddr = 0x12027d4ULL;
inline constexpr pinned::Symbol kNetGameMTPin = {kNetGameMTSymbol, kNetGameMTVaddr};
inline constexpr const char* kUserActorSymbol = "_ZNK10NRadEngine8NRadGame11CR15NetGame9UserActorEt";
inline constexpr std::uint64_t kUserActorVaddr = 0x1251654ULL;

inline constexpr std::uintptr_t kNetGameOffset = 0x8510;  // CR15Game -> CR15NetGame*
inline constexpr std::uintptr_t kNetGameGame = 0x8;       // CR15NetGame -> CR15Game*

inline constexpr pinned::Word kWords[] = {
    {"NetGameMT: mov w8,#0x8510", 0x12027d4ULL, 0x5290a208U},
    {"NetGameMT: ldr x0,[x0,x8]", 0x12027d8ULL, 0xf8686800U},
    {"UserActor: ldr x0,[x0,#8] (the game)", 0x1251674ULL, 0xf9400400U},
};

static_assert(pinned::MovImmediate(kWords[0].word) == kNetGameOffset, "the pinned word does not carry kNetGameOffset");
static_assert(pinned::LdrXOffset(kWords[2].word) == kNetGameGame, "the pinned word does not carry kNetGameGame");

}  // namespace nevr_quest::combat::netgame
