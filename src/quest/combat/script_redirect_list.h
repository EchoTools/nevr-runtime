// The nine stock script libraries the combat APK carries its own build of (#1737): the 16 hex digits of each lib<hash>.so, in the
// order the packaging lists them. The combat area registers this list with the host's script redirect (combat/script_redirect.h); the
// repack stages the same nine (tools/cmd/quest-repack-libs, COMBAT_REDIRECT_HASHES); tools/tests/test_combat_script_redirect.py holds the
// two equal.
#pragma once

#include <cstdint>

namespace nevr_quest::combat::script_redirect {

inline constexpr std::uint64_t kHashes[] = {
    0x37f88adb0d8066f7ULL, 0x3c11128c3bd783ddULL, 0x4b65d1b009973281ULL, 0x6a0f689e5b91c666ULL, 0x80461e04c6d82c21ULL,
    0x812075466da9e147ULL, 0x954c89b6c61625f6ULL, 0xa5c64faa725f2334ULL, 0xac4cd8e0a786d5c5ULL,
};
inline constexpr std::uint32_t kCount = static_cast<std::uint32_t>(sizeof kHashes / sizeof kHashes[0]);

}  // namespace nevr_quest::combat::script_redirect
