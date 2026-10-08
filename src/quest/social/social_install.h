// Installs the social facade into the running game.
//
// The seam is one relocation: libr15.so calls CNSProvider::Social(unsigned long) through its own PLT,
// at one call site (CR15NetGame::Initialize), once per run. The hook calls the original, and when the
// object it returned is pnsovr's CNSOVRSocial of the pinned build, hands the game the facade instead.
// Nothing in libpnsovr is patched; the Oculus social object is simply never given to the game. Any
// other result (null, another provider, another build) passes through unchanged and one structured
// line says why.
//
// Hook frequency: Social() is called once, at startup (libr15 0x12866a4 is its only caller), so the
// handler's loader lookup is not on a per-frame path. The facade's slots are, and are measured by the
// per-slot counters in social_facade.cpp.
#pragma once

#include <cstddef>
#include <cstdint>

#include "callback_thunk.h"
#include "got_hook.h"

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
// returned, returns what the game should receive. Never throws.
void* SelectSocialObject(void* original, void* facadeObject, PnsovrLookup lookup) noexcept;

// The typed callback for the hook.
struct SocialTag {};
using SocialSig = void*(std::uint64_t handle);
using SocialThunk = sentinel::CallbackThunk<SocialTag, SocialSig>;

// Test seams. SetPnsovrLookup replaces the loader lookup the handler uses (nullptr restores the real one)
// and returns the previous one; SocialHandler is the callback InstallSocialHook arms.
PnsovrLookup SetPnsovrLookup(PnsovrLookup lookup);
SocialThunk::Handler SocialHandler();

// Builds the process-wide facade (outside any game frame), arms the callback and redirects the slot.
// `enabled` is the caller's activation decision; false touches nothing.
InstallResult InstallSocialHook(bool enabled);

}  // namespace quest_social
