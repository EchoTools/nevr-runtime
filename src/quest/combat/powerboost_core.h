// powerboost_core.h: the Quest-only part of powerboost.cpp (src/quest/combat/powerboost.h) that is no rule of EC's POWER BOOST: the link
// line's repeat filter. The marker decode, the cone and the link / unlink markers are runtime/patch/powerboost_rules.h, which PC's port
// calls too (#2339). Nothing here calls the game. Tested by src/quest/tests/powerboost_test.cpp.
//
// Origin (docs/audits/2026-11-05-q15t-origin-citations.md rows 255-272): the reference build's runtime/native/afkxr/afk_powerboost_core.h
// (at 1357409), the link line filter of which is this file.
#pragma once
#include <stdint.h>

namespace nevr_quest::combat::powerboost {
namespace pbcore {

// The link call can come every update of a bubble: one line per (bubble, target) change, or per kLinkRepeatMs for the
// same pair. Plain fields: the caller keeps it under its own lock (table operation only).
constexpr uint64_t kLinkRepeatMs = 1000;
struct LinkFilter {
    uint64_t key = ~0ull, atMs = 0;
    bool have = false;
    bool Show(uint32_t bubble, uint32_t target, uint64_t nowMs) {
        const uint64_t k = (static_cast<uint64_t>(bubble) << 32) | target;
        if (have && k == key && nowMs - atMs < kLinkRepeatMs) return false;
        have = true;
        key = k;
        atMs = nowMs;
        return true;
    }
};

}  // namespace pbcore
}  // namespace nevr_quest::combat::powerboost
