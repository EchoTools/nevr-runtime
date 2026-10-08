/* GOT targets and callback types for the pinned Quest artifact (docs/adr/0003).
 *
 * Every address here is a link-time address in the pinned ELF, checked against
 * its own relocation table (`readelf -rW`) and, for the typed callbacks, against
 * the Itanium mangling of the symbol the relocation names. They apply only to
 * the build IDs below; GotHook refuses a module whose build ID differs.
 *
 * This header includes callback_thunk.h, so a translation unit that includes it must be
 * built with -fno-exceptions (see the contract there). Handlers are `noexcept`.
 *
 * Nothing in this header activates a hook. entry.cpp installs the clock_gettime
 * proof hook only; the CJson::TString thunks are declared for the config-string
 * seam and stay uninstalled until the gates in #158 pass.
 */
#pragma once

#include <time.h>

#include <cstdint>

#include "callback_thunk.h"
#include "got_hook.h"

namespace sentinel::pinned {

inline constexpr const char* kLibR15 = "libr15.so";
inline constexpr const char* kLibR15BuildId = "b243509c08ce677aeb95fa348016949b3fc45230";
inline constexpr const char* kMatchmaking = "libpnsradmatchmaking.so";
inline constexpr const char* kMatchmakingBuildId = "8c4fddc079eae65909530132a56c48da48b2708c";

inline constexpr const char* kTStringSymbol = "_ZNK10NRadEngine5CJson7TStringEPKcS2_j";
inline constexpr const char* kClockGettimeSymbol = "clock_gettime";
inline constexpr const char* kConfigRequestSendSymbol =
    "_ZN10NRadEngine18SNSConfigRequestv24SendERNS_15CTcpBroadcasterEPKcS4_";

// ---- typed callbacks --------------------------------------------------------

// libc: int clock_gettime(clockid_t, struct timespec*).
using ClockGettimeSig = int(clockid_t, struct timespec*);
struct ClockGettimeTag {};
using ClockGettimeThunk = CallbackThunk<ClockGettimeTag, ClockGettimeSig>;

// NRadEngine::CJson::TString(char const*, char const*, unsigned int) const, the
// same function in both libraries (const member: `this` in x0, key x1, fallback
// x2, flag w3; returns a borrowed char const* in x0). CJson is never
// dereferenced by a handler, so it stays an incomplete type.
struct CJsonOpaque;
using CJsonTStringSig =
    const char*(const CJsonOpaque* self, const char* key, const char* fallback, std::uint32_t flag);
struct LibR15TStringTag {};
struct MatchmakingTStringTag {};
using LibR15TStringThunk = CallbackThunk<LibR15TStringTag, CJsonTStringSig>;
using MatchmakingTStringThunk = CallbackThunk<MatchmakingTStringTag, CJsonTStringSig>;

// SNSConfigRequestv24Send(CTcpBroadcaster&, char const*, char const*): the
// mangling encodes the parameters but not the return type, and no caller has
// been decoded, so there is deliberately no thunk for it.

// ---- targets ----------------------------------------------------------------

// libr15.so imports clock_gettime from libc (JUMP_SLOT 0x36c33e8).
inline GotTarget LibR15ClockGettime() {
  return {kLibR15, kClockGettimeSymbol, RelocKind::kJumpSlot, kLibR15BuildId, 0x36c33e8ULL};
}

// libr15.so's slot for CJson::TString, defined in libr15 itself (JUMP_SLOT 0x36ebe08).
inline GotTarget LibR15TString() {
  return {kLibR15, kTStringSymbol, RelocKind::kJumpSlot, kLibR15BuildId, 0x36ebe08ULL};
}

// libpnsradmatchmaking.so's slot for the same function, defined in
// libpnsradmatchmaking itself at 0x209484 (JUMP_SLOT 0x6b4768).
inline GotTarget MatchmakingTString() {
  return {kMatchmaking, kTStringSymbol, RelocKind::kJumpSlot, kMatchmakingBuildId, 0x6b4768ULL};
}

// libr15.so's slot for SNSConfigRequestv24Send, a weak function it defines at
// 0x191625c (JUMP_SLOT 0x37015e0).
inline GotTarget LibR15ConfigRequestSend() {
  return {kLibR15, kConfigRequestSendSymbol, RelocKind::kJumpSlot, kLibR15BuildId, 0x37015e0ULL};
}

// Two GLOB_DAT slots in libr15.so (0x3715620 and 0x372b698), used as fixtures
// for the GLOB_DAT path.
inline GotTarget LibR15BindNodeGlobDat() {
  return {kLibR15,
          "_ZN10NRadEngine8BindNodeINS_8NRadGame28SR15NetConfigRequestNodeDataENS1_24CR15NetConfigRequestNodeEEExRNS_13SScriptThreadEPv",
          RelocKind::kGlobDat, kLibR15BuildId, 0x3715620ULL};
}
inline GotTarget LibR15LookupDataBindingGlobDat() {
  return {kLibR15,
          "_ZN10NRadEngine17LookupDataBindingINS_8NRadGame28SR15NetConfigRequestNodeDataEEEvNS_9CSymbol64EPKciPiPPFvPviS7_E",
          RelocKind::kGlobDat, kLibR15BuildId, 0x372b698ULL};
}

}  // namespace sentinel::pinned
