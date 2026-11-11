/* The pinned slot, function and normalise words the bullet-velocity guard relies on (the guard skips a FireBullet whose |v| is below 1e-6 or whose
 * components are not finite; the Quest side is tracked in #1737).
 *
 * Every address is an ELF virtual address in the store libr15 (the store file's build ID is `kLibR15StockBuildId` in pinned_targets.h); the disassembly's address is
 * the ELF address plus 0x100000. src/quest/tests/bullet_velocity_guard_pinned_test.cpp checks each word against the real file
 * (`just test-quest-hooks-pinned`, which needs the store APK).
 *
 *   CR15NetBulletCS::FireBullet  JUMP_SLOT 0x36f3cf8 -> 0x210c9c4 (one slot; its pin is dronemuzzle_targets.h's, not repinned here)
 *
 * FireBullet normalises its 3-vector argument with no test: `fmov s3, #1.0` at 0x210ccac, `fsqrt s4, s4` at 0x210ccc0 (|v|) and
 * `fdiv s3, s3, s4` at 0x210ccc4 (1.0 / |v|), with no compare between them. A zero vector therefore divides by zero.
 *
 * Upstream: `kotorvr/echoquest-combat` at `c5a67e8`, runtime/native/afkxr/afk_fx.cpp (the fireguard flag, around lines 214 and 5148-5264).
 *
 * PC (echovr.exe): echovr.exe CR15NetBulletCS::FireBullet is VA 0x140ce2020 and its normalise divides by |v| with no guard (SQRTSS at
 * 0x140ce2442, DIVSS at 0x140ce244e).
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/combat/dronemuzzle_targets.h"
#include "quest/combat/pinned_words.h"

namespace nevr_quest::combat::bullet_velocity_guard::targets {

// FireBullet's slot and function are dronemuzzle's pins; this module names them, it does not repin them.
inline constexpr const char* kFireSymbol = dronemuzzle::kFireSymbol;

// The normalise words the guard relies on: the numerator, the magnitude and the divide.
inline constexpr pinned::Word kWords[] = {
    {"FireBullet normalise: fmov s3, #1.0 (the numerator)", 0x210ccac, 0x1e2e1003},
    {"FireBullet normalise: fsqrt s4, s4 (|v|)", 0x210ccc0, 0x1e21c084},
    {"FireBullet normalise: fdiv s3, s3, s4 (1.0 / |v|, no zero or finite test)", 0x210ccc4, 0x1e241863},
};
inline constexpr std::size_t kWordCount = sizeof(kWords) / sizeof(kWords[0]);
inline constexpr pinned::Pins kWordPins = {kWords, kWordCount, nullptr, 0};

}  // namespace nevr_quest::combat::bullet_velocity_guard::targets
