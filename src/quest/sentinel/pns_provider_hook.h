/* The game's platform provider is its own libpnsrad, as on PC (#1220, docs/adr/0026 section 1).
 *
 * What the game does (pinned libr15; addresses in pinned_targets.h): CR15Game::Initialize loads the platform provider with
 * CNSProvider::Create(handle, "pnsovr", json, callbacks) ("pnsdemo" under -noovr) and the RAD provider with
 * Create(handle, "pnsrad", null, callbacks). pnsovr is Meta's platform SDK adapter: its RadPluginMain initializes the SDK
 * and runs the store's entitlement check, its Update can HardExit the process. PC never loads it
 * (src/runtime/patch/pnsrad_enabler.cpp rewrites the names to pnsrad).
 *
 * Four GOT hooks on libr15's own provider slots, installed together, behind `diagnostics.pnsrad_provider` (default off;
 * the login that goes with it is #1220's second PR):
 *   Create          a `pnsovr` or `pnsdemo` request loads `pnsrad` with a null json, the call libr15 makes for the RAD
 *                   provider; every other name passes through. CModuleLoader keeps a loaded module without running its
 *                   RadPlugin* entries again, so both handles name the one libpnsrad. The platform json's `appid` is kept
 *                   first (AppId) for the login, which no longer gets it from pnsovr.
 *   Destroy         ShutdownEngine destroys the pnsdemo, pnsrad and pnsovr handles. After the rename two of them hold
 *                   libpnsrad, and a second RadPluginShutdown faults in its VoipShutdown (libpnsrad 0x20627c reads a
 *                   global the first call cleared; #1220). A handle already destroyed is only closed:
 *                   CSysModule::Unload(handle, 0), the dlclose that balances the second load's dlopen.
 *   ProviderID,     answer the OVR symbol once a rename happened (nevr_provider_identity::kOvrProviderSymbol, the answer
 *   UserProviderID  PC's pnsrad gives through its patched export); before that, or with no rename, the game's call.
 *
 * Lines (sentinel log):
 *   install  {"event":"pns_provider_install","result":"installed|refused","status":...,"slot":...}
 *   create   {"event":"pns_provider_create","asked":"pnsovr|pnsdemo|pnsrad|other","loaded":...,"result":<n>,
 *            "handle_nonzero":0|1,"appid":"kept|absent|not_read"}   one per Create
 *   destroy  {"event":"pns_provider_destroy","action":"shutdown|close_only|none","handle_nonzero":0|1}   one per Destroy
 *   answer   {"event":"pns_provider_id","slot":"ProviderID|UserProviderID","answer":"ovr|game"}   the first call of each
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "pinned_targets.h"

namespace nevr_quest::pns_provider {

using CreateThunk = sentinel::pinned::LibR15ProviderCreateThunk;
using DestroyThunk = sentinel::pinned::LibR15ProviderDestroyThunk;
using ProviderIdThunk = sentinel::pinned::LibR15ProviderIdThunk;
using UserProviderIdThunk = sentinel::pinned::LibR15UserProviderIdThunk;

// The provider a Create for `name` loads instead: "pnsrad" for "pnsovr" and "pnsdemo", nullptr (pass through) for every
// other name, a null one included.
const char* RenamedProvider(const char* name) noexcept;

// The libr15 functions the hooks call, resolved at install from the pinned image.
struct GameFunctions {
  sentinel::pinned::SysModuleUnloadFn unload = nullptr;  // CSysModule::Unload: required (the Destroy guard)
  sentinel::pinned::CJsonTStringFn tstring = nullptr;    // CJson::TString: without it the appid is not kept
};

struct Counts {
  unsigned long long creates;        // Create calls
  unsigned long long renamed;        // of them, renamed to pnsrad
  unsigned long long destroys;       // Destroy calls with a nonzero handle
  unsigned long long closed_only;    // of them, only closed
  unsigned long long provider_ids;   // ProviderID calls answered OVR
  unsigned long long user_provider_ids;  // UserProviderID calls answered OVR
};
Counts CurrentCounts() noexcept;

// The platform json's appid as the game set it, when a renamed Create kept it; nullptr otherwise.
const char* AppId() noexcept;

// Installs the four hooks into libr15 after resolving GameFunctions from the loaded, pinned libr15. True when all four
// slots hold the thunks. The order (Destroy, ProviderID, UserProviderID, Create) leaves the game's own provider in place
// when any install is refused: the rename is the last one.
bool Install() noexcept;

// The install on any targets with `functions` (the host test arms without targets). Refuses, installing nothing, when
// `functions.unload` is null.
bool InstallAt(const sentinel::GotTarget& create, const sentinel::GotTarget& destroy, const sentinel::GotTarget& providerId,
               const sentinel::GotTarget& userProviderId, const GameFunctions& functions) noexcept;

// Test support: arms the four handlers without touching a GOT slot. ResetForTest clears the counts, the destroyed
// handles, the rename and the appid.
void ArmForTest(const GameFunctions& functions) noexcept;
void ResetForTest() noexcept;

}  // namespace nevr_quest::pns_provider
