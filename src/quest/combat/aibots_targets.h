/* The pinned function, symbols and code words of aibots (aibots.h). Every address is an ELF virtual address in the store libr15 (build id in
 * pinned_targets.h, `LibR15PatchedBuildId`); the disassembly's address is the ELF address plus 0x100000. src/quest/tests/got_pinned_test.cpp checks
 * that each symbol is defined at its address and every word is the file's (`just test-quest-hooks-pinned`, which needs the store APK). Nothing is
 * hooked: the module calls one function and reads the objects the words below load.
 *
 *   CR15NetAIScenarios::EnableScenario(EScenarioState) @ 0x123c2bc (388 bytes)   opens `sub sp,sp,#0x30` (the word the module checks); reads
 *       the scenarios' game at +0x558, the game's flags pointer at [+0x2da0] and takes the host path when flags & 2, or (flags & 0x44) == 0x40;
 *       the current scenario is a u64 at +0x560 (read at 0x123c30c, stored at 0x123c42c), the state a u32 at +0x568
 *   CR15NetGame::Scenarios() @ 0x128fd0c   `ldr x0,[x0,#0x2ab0]`   the CR15NetAIScenarios of the net game
 *   CR15NetAIScenarios::ConnectBot @ 0x123bf78   the connected-agent count is a u32 at scenarios+0x500 (loaded at 0x123c038, stored at 0x123c040)
 *   CR15NetGame::UpdateGameExpressionData @ 0x12809f0   `cmp w9,#9` at 0x1280a88 stores (state == 9) as the byte at x27+8, with x27 = net + 0x6340c
 *       (`mov w9,#0x340c; movk w9,#6,lsl #16; add x27,x0,x9`), and stores bytes at x27+0xf
 *
 * `matchiscombat` is the game's own field: `SR15NetGameExpressionData::Inspect<CBindingsOffsetOfInspector<bool>>` @ 0x24399f4 binds the record of the name
 * `matchiscombat` (0x31e1ad8) to the offset `w0 + 0xf` (`add w13,w0,#0xf` at 0x2439df8), and the same function binds `ingame` to +0x8 and `matchisarena` to +0xe; the
 * byte store at x27+0xf in UpdateGameExpressionData (0x1280e74) is the game type being one of several combat symbols, `echo_combat_practice_ai` among them. The net
 * game's state is the i32 at net+0 (the load before the compare).
 */
#pragma once

#include <cstdint>

#include "quest/combat/pinned_words.h"

namespace nevr_quest::combat::aibots::targets {

inline constexpr const char* kEnableSymbol = "_ZN10NRadEngine8NRadGame18CR15NetAIScenarios14EnableScenarioENS1_14EScenarioStateE";
inline constexpr std::uint64_t kEnableVaddr = 0x123c2bcULL;
inline constexpr std::uint32_t kEnableFirstWord = 0xd100c3ffU;  // sub sp, sp, #0x30
inline constexpr const char* kScenariosSymbol = "_ZN10NRadEngine8NRadGame11CR15NetGame9ScenariosEv";
inline constexpr std::uint64_t kScenariosVaddr = 0x128fd0cULL;
inline constexpr const char* kConnectBotSymbol = "_ZN10NRadEngine8NRadGame18CR15NetAIScenarios10ConnectBotENS0_13SR15NetUserIdE";
inline constexpr std::uint64_t kConnectBotVaddr = 0x123bf78ULL;
inline constexpr const char* kUpdateExpressionSymbol = "_ZN10NRadEngine8NRadGame11CR15NetGame24UpdateGameExpressionDataEv";
inline constexpr std::uint64_t kUpdateExpressionVaddr = 0x12809f0ULL;

inline constexpr std::uintptr_t kNetScenarios = 0x2ab0, kNetMatchIsCombat = 0x6341b, kExpressionBase = 0x6340c, kExpressionCombatByte = 0xf;
inline constexpr std::uintptr_t kNetFlagsPtr = 0x2da0, kScenBotCount = 0x500, kScenCurrent = 0x560, kScenGame = 0x558;
inline constexpr int kInGame = 9;
inline constexpr long long kMaxAgents = 16;  // 0x500 / 0x50: ConnectBot does not bound-check

inline constexpr pinned::Word kWords[] = {
    {"EnableScenario: sub sp,sp,#0x30", 0x123c2bcULL, 0xd100c3ffU},
    {"EnableScenario: ldr x20,[x0,#0x558]", 0x123c2c8ULL, 0xf942ac14U},
    {"EnableScenario: ldr x8,[x20,#0x2da0] (flags)", 0x123c2d4ULL, 0xf956d288U},
    {"EnableScenario: tbnz w8,#1 (host flag)", 0x123c2dcULL, 0x37080188U},
    {"EnableScenario: mov w9,#0x44", 0x123c2e0ULL, 0x52800889U},
    {"EnableScenario: cmp x8,#0x40", 0x123c2e8ULL, 0xf101011fU},
    {"EnableScenario: ldr x8,[x19,#0x560] (current)", 0x123c30cULL, 0xf942b268U},
    {"Scenarios: ldr x0,[x0,#0x2ab0]", 0x128fd0cULL, 0xf9555800U},
    {"ConnectBot: ldr w8,[x19,#0x500] (agents)", 0x123c038ULL, 0xb9450268U},
    {"ConnectBot: str w9,[x19,#0x500]", 0x123c040ULL, 0xb9050269U},
    {"UpdateGameExpressionData: mov w9,#0x340c", 0x1280a10ULL, 0x52868189U},
    {"UpdateGameExpressionData: movk w9,#6,lsl #16", 0x1280a14ULL, 0x72a000c9U},
    {"UpdateGameExpressionData: cmp w9,#9 (in game)", 0x1280a88ULL, 0x7100253fU},
    {"UpdateGameExpressionData: strb w11,[x27,#8] (state == 9)", 0x1280a98ULL, 0x3900236bU},
    {"UpdateGameExpressionData: strb w8,[x27,#0xf]", 0x1280e74ULL, 0x39003f68U},
    {"Inspect<bool>: add x13,x13,#0xadc (the record of matchiscombat)", 0x2439ddcULL, 0x912b71adU},
    {"Inspect<bool>: add w13,w0,#0xf (matchiscombat's field offset)", 0x2439df8ULL, 0x11003c0dU},
};
inline constexpr pinned::Pins kPins = {kWords, sizeof(kWords) / sizeof(kWords[0]), nullptr, 0};

static_assert(pinned::LdrXOffset(kWords[1].word) == kScenGame, "the pinned word does not carry kScenGame");
static_assert(pinned::LdrXOffset(kWords[2].word) == kNetFlagsPtr, "the pinned word does not carry kNetFlagsPtr");
static_assert(pinned::LdrXOffset(kWords[6].word) == kScenCurrent, "the pinned word does not carry kScenCurrent");
static_assert(pinned::LdrXOffset(kWords[7].word) == kNetScenarios, "the pinned word does not carry kNetScenarios");
static_assert(pinned::LdrWOffset(kWords[8].word) == kScenBotCount, "the pinned word does not carry kScenBotCount");
static_assert(pinned::LdrWOffset(kWords[9].word) == kScenBotCount, "the pinned store does not carry kScenBotCount");
static_assert(pinned::MovImmediate(kWords[10].word) + (pinned::MovImmediate(kWords[11].word) << 16) == kExpressionBase,
              "the pinned words do not build kExpressionBase");
static_assert(pinned::AddImmediate(kWords[12].word) == kInGame, "the pinned compare is not with kInGame");
static_assert(pinned::LdrbOffset(kWords[14].word) == kExpressionCombatByte, "the pinned store does not carry kExpressionCombatByte");
static_assert(pinned::AddImmediate(kWords[16].word) == kExpressionCombatByte, "the pinned word does not carry the matchiscombat field offset");
static_assert(kExpressionBase + kExpressionCombatByte == kNetMatchIsCombat, "kNetMatchIsCombat is not the expression base plus the byte");

}  // namespace nevr_quest::combat::aibots::targets
