// The combat unit's script redirect (#1737). The combat APK carries its own build of nine stock script libraries, each
// named lib<stock hash>_q15t.so beside the untouched stock lib<stock hash>.so. At init the unit asks the host to redirect
// the game's loads of those nine names (NevrQuestHostApi::register_script_redirect); the host registers all of them or
// none (src/quest/plugin/script_redirect.h). The list is the same as the packaging list in tools/quest_repack_libs.py
// (COMBAT_REDIRECT_HASHES); tools/tests/test_q15t_script_redirect.py holds the two equal.
#pragma once

#include <cstdint>

#include "nevr/plugin_interface.h"

namespace q15t {

// The stock hashes (the 16 hex digits of lib<hash>.so), in the order the packaging lists them.
inline constexpr std::uint64_t kScriptRedirectHashes[] = {
    0x37f88adb0d8066f7ULL, 0x3c11128c3bd783ddULL, 0x4b65d1b009973281ULL, 0x6a0f689e5b91c666ULL, 0x80461e04c6d82c21ULL,
    0x812075466da9e147ULL, 0x954c89b6c61625f6ULL, 0xa5c64faa725f2334ULL, 0xac4cd8e0a786d5c5ULL,
};

// Asks the host for the redirect, once, from init. Returns the host's NevrScriptRedirectResult (0 is OK), or -1 when the host
// offers no script redirect (an older host, or a context too short to carry the API). Logs the outcome either way.
int RegisterScriptRedirect(const NvrGameContext* ctx);

}  // namespace q15t
