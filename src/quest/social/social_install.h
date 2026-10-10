// Installs the social facade into the running game.
//
// The seam is one relocation: libr15.so calls CNSProvider::Social(unsigned long) through its own PLT,
// at one call site (CR15NetGame::Initialize), once per run. The hook calls the original, and when the
// object it returned is pnsovr's CNSOVRSocial of the pinned build, hands the game the facade instead.
// Nothing in libpnsovr is patched; the Oculus social object is simply never given to the game. Any
// other result (null, another provider, another build) passes through unchanged and a counter says
// why. The handler never logs (hook_log.h: logging is unsafe on a game call path); the sentinel's
// reporter thread logs the counters registered by RegisterSocialReportCounters.
//
// Hook frequency: Social() is called once, at startup (libr15 0x12866a4 is its only caller), so the
// handler's loader lookup is not on a per-frame path. The facade's slots are, and are measured by the
// per-slot counters in social_facade.cpp.
#pragma once

#include <cstddef>
#include <cstdint>

#include <atomic>

#include "callback_thunk.h"
#include "got_hook.h"
#include "quest/social/social_abi.h"

namespace quest_social {

enum class InstallStatus : std::uint8_t {
  kOk,
  kDisabled,        // the caller's switch is off: nothing was touched
  kHookFailed,      // GotHook refused (see the GotStatus); the game keeps the Oculus social object
};
const char* InstallStatusName(InstallStatus status);

struct InstallResult {
  InstallStatus status = InstallStatus::kDisabled;
  sentinel::GotStatus got = sentinel::GotStatus::kOk;  // meaningful for kHookFailed
};

// libr15.so's slot for CNSProvider::Social, pinned to the artifact's build id and link-time address.
sentinel::GotTarget LibR15Social();

// What the handler needs to know about the loaded libpnsovr. Production uses the dynamic loader; a
// test passes a fake.
struct PnsovrView {
  bool found = false;
  bool buildIdMatches = false;
  std::uintptr_t loadBias = 0;
};
using PnsovrLookup = PnsovrView (*)();
PnsovrView FindPnsovr() noexcept;  // dl_iterate_phdr + the NT_GNU_BUILD_ID note

// The decision, separated from the loader so it can be tested without a game: given what Social()
// returned, returns what the game should receive. Never throws, never logs; it counts.
void* SelectSocialObject(void* original, void* facadeObject, PnsovrLookup lookup) noexcept;

// What the handler counted. Zero is the healthy state for every counter but Selected and the thunk's
// call counter.
struct SocialCounters {
  const std::atomic<std::uint64_t>& selected;           // the game was given the facade
  const std::atomic<std::uint64_t>& nullResult;         // the provider returned no social object
  const std::atomic<std::uint64_t>& pnsovrUnavailable;  // libpnsovr not loaded, or not the pinned build
  const std::atomic<std::uint64_t>& foreignObject;      // not a CNSOVRSocial of the pinned build
};
SocialCounters Counters() noexcept;
void ResetCountersForTest() noexcept;

// The game's CJson functions (Reset, DecodeFrom, EncodeToCompact) in the loaded libr15, all nullptr unless libr15
// is loaded and its build id is the pinned one: the image base plus each export's link-time address
// (kLibR15CJson*Vaddr, which social_pinned_test checks against the library's dynamic symbol table and the first
// instruction of each). Never throws.
GameJson ResolveGameJson(sentinel::ImageLookup lookup) noexcept;

// ---- rich presence trace (#393) -----------------------------------------------------------------------------
//
// The game's SyncRichPresence asks pnsovr's CNSOVRRichPresence which destination its game type maps to and
// the destination's display name, and builds the status text under the player's name from them. libr15
// reaches that object through CNSProvider::RichPresence(unsigned long), once at startup. This hook gives
// the game the same object with its vtable replaced by a copy whose slots Destination, DestinationName and
// Set are wrappers that call the original and log what it answered (pass-through: nothing changes). The
// handler counts and never logs; the wrappers run on the game's thread and log on change.
struct PresenceTag {};
using PresenceSig = void*(std::uint64_t handle);
using PresenceThunk = sentinel::CallbackThunk<PresenceTag, PresenceSig>;
void* OnPresenceHandler(PresenceThunk::Fn original, std::uint64_t handle) noexcept;

sentinel::GotTarget LibR15RichPresence();

// What the handler decided for the object the provider returned. Never throws, never logs; it counts.
void* SelectRichPresenceObject(void* original, PnsovrLookup lookup) noexcept;

struct PresenceCounters {
  const std::atomic<std::uint64_t>& selected;     // the object's vtable was replaced by the tracing copy
  const std::atomic<std::uint64_t>& passThrough;  // null, pnsovr missing or another build, or another class
};
PresenceCounters PresenceCountersView() noexcept;
void ResetPresenceForTest() noexcept;

// Installs the hook. Called by InstallSocialHook after the social hook is in; its outcome is logged, and
// returned for a test (the social facade works without it).
sentinel::GotStatus InstallPresenceTrace();

// Test seams: whether the four wrapped slots are checked against the pinned addresses (a test's fake
// functions live elsewhere), and the game's EncodeToCompact the Set wrapper reads the document with (nullptr:
// resolved from the loaded libr15 on first use).
void SetPresenceSeamsForTest(bool slotCheck, CJsonEncodeToCompactFn encode) noexcept;

// Registers the counters with the sentinel's reporter (hook_report.h): the thunk's calls, the selected
// count, the three pass-through counters, the thunk's faults, the facade's eleven (members hidden,
// events dropped, sends failed, joins deferred, requests timed out, four callback delivery classes, JSON failures,
// frames ignored), the invite gate's two (social_invite_gate.h) and the rich presence trace's four: 23 of the
// reporter's 96.
// Call before StartReporter; returns false if any registration was refused.
bool RegisterSocialReportCounters();

// The typed callback for the hook.
struct SocialTag {};
using SocialSig = void*(std::uint64_t handle);
using SocialThunk = sentinel::CallbackThunk<SocialTag, SocialSig>;

// The hook's handler: calls the original, then SelectSocialObject on its result. Declared so the
// record (NEVR_HOOK_RECORD in social_install.cpp) and a test's own record can name it.
void* OnSocialHandler(SocialThunk::Fn original, std::uint64_t handle) noexcept;

// Test seams. SetPnsovrLookup replaces the loader lookup the handler uses (nullptr restores the real one)
// and returns the previous one; PublishFacadeObject stores the process-wide facade's object where the
// handler reads it (InstallSocialHook does this itself).
PnsovrLookup SetPnsovrLookup(PnsovrLookup lookup);
void PublishFacadeObject();

// Registers the friend-name decoder (nevr_social_names::RegisterDefaultDecoder, the zstd profile reader), builds the
// process-wide facade (outside any game frame), arms the callback and redirects the slot. Once that has worked it also
// installs the party-invite gate override (social_invite_gate.h), whose outcome is logged, not returned.
// `enabled` is the caller's activation decision; false touches nothing.
InstallResult InstallSocialHook(bool enabled);

}  // namespace quest_social
