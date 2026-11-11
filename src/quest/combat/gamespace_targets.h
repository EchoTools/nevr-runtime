/* The offsets gamespace.cpp reads, tied to the store libr15 (build id in pinned_targets.h) by the game's own code that uses them: the component-system lookup
 * `FindRequiredComponentSystemMT<CR15NetGunCS>` @ 0x2106738 reads a system's gamespace at cs+0x80 (0x2106748 `ldr x23,[x0,#0x80]`), then the gamespace's count at
 * gs+0x450 (0x2106758) and its list at gs+0x420 (0x2106798) and binary-searches 16-byte `{key, cs*}` rows. The game keeps the list sorted (a re-sort flag at gs+0x458
 * bit 0); the module scans it linearly from the game thread, so the order does not matter. The words below are checked by got_pinned_test.cpp against the file and
 * by each module's install (a different libr15 is a refusal).
 */
#pragma once

#include <cstdint>

#include "quest/combat/pinned_words.h"

namespace nevr_quest::combat::gamespace::targets {

inline constexpr std::uintptr_t kSystems = 0x420, kSystemCount = 0x450, kSystemGamespace = 0x80;

inline constexpr pinned::Word kWords[] = {
    {"FindRequiredComponentSystemMT: ldr x23,[x0,#0x80] (a system's gamespace)", 0x2106748ULL, 0xf9404017U},
    {"FindRequiredComponentSystemMT: ldr x1,[x23,#0x450] (the count)", 0x2106758ULL, 0xf9422ae1U},
    {"FindRequiredComponentSystemMT: ldr x8,[x23,#0x420] (the list)", 0x2106798ULL, 0xf94212e8U},
};

inline constexpr pinned::Pins kPins = {kWords, sizeof(kWords) / sizeof(kWords[0]), nullptr, 0};

static_assert(pinned::LdrXOffset(kWords[0].word) == kSystemGamespace, "the pinned word does not carry kSystemGamespace");
static_assert(pinned::LdrXOffset(kWords[1].word) == kSystemCount, "the pinned word does not carry kSystemCount");
static_assert(pinned::LdrXOffset(kWords[2].word) == kSystems, "the pinned word does not carry kSystems");

}  // namespace nevr_quest::combat::gamespace::targets
