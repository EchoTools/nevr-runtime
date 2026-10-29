// Host test for "the game's platform provider is its own libpnsrad" (#1220): the real handlers
// (src/quest/sentinel/pns_provider_hook.cpp), built without exceptions as the sentinel builds them, driven through the
// thunks' entries with stand-ins for libr15's CNSProvider::Create, Destroy, ProviderID and UserProviderID and for
// CSysModule::Unload and CJson::TString. No GOT slot is touched: got_pinned_test.cpp resolves the pinned slots, their
// stubs and their callers in the real library.
//
// Built and run by `just test-quest-hooks`.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "pns_provider_hook.h"
#include "quest/tests/test_check.h"
#include "runtime/patch/provider_identity.h"

namespace {

namespace pns = nevr_quest::pns_provider;

constexpr unsigned long kPnsradHandle = 0x7000;
constexpr unsigned long kOtherHandle = 0x9000;
constexpr std::uint64_t kRadSymbol = 0xc8e8d0b1a882e3eeULL;  // what libpnsrad's own export answers

// What the stand-ins saw.
std::vector<std::string> g_createdNames;
std::vector<const void*> g_createdJson;
std::vector<unsigned long> g_shutDown;           // Destroy reached the game's function
std::vector<unsigned long> g_closed;             // CSysModule::Unload(handle, 0)
std::vector<unsigned int> g_closeHow;
int g_providerIdCalls = 0;
int g_userProviderIdCalls = 0;
int g_tstringCalls = 0;
const char* g_appIdText = "1369078409873402";

// CModuleLoader keeps a loaded module, so every load of libpnsrad hands back the same handle.
int StandInCreate(unsigned long* handle, const char* name, const void* json, const void* /*callbacks*/) {
  g_createdNames.emplace_back(name != nullptr ? name : "(null)");
  g_createdJson.push_back(json);
  *handle = (name != nullptr && std::strcmp(name, "pnsrad") == 0) ? kPnsradHandle : kOtherHandle;
  return 0;
}
void StandInDestroy(unsigned long handle) { g_shutDown.push_back(handle); }
std::uint64_t StandInProviderId(unsigned long /*handle*/) {
  ++g_providerIdCalls;
  return kRadSymbol;
}
std::uint64_t StandInUserProviderId(unsigned long /*handle*/) {
  ++g_userProviderIdCalls;
  return kRadSymbol;
}
void StandInUnload(unsigned long handle, unsigned int how) {
  g_closed.push_back(handle);
  g_closeHow.push_back(how);
}
const char* StandInTString(const void* /*json*/, const char* path, const char* fallback, unsigned int /*flags*/) {
  ++g_tstringCalls;
  return std::strcmp(path, "appid") == 0 && g_appIdText != nullptr ? g_appIdText : fallback;
}

pns::CreateThunk::Fn Create() { return reinterpret_cast<pns::CreateThunk::Fn>(pns::CreateThunk::EntryAddress()); }
pns::DestroyThunk::Fn Destroy() { return reinterpret_cast<pns::DestroyThunk::Fn>(pns::DestroyThunk::EntryAddress()); }
pns::ProviderIdThunk::Fn ProviderId() {
  return reinterpret_cast<pns::ProviderIdThunk::Fn>(pns::ProviderIdThunk::EntryAddress());
}
pns::UserProviderIdThunk::Fn UserProviderId() {
  return reinterpret_cast<pns::UserProviderIdThunk::Fn>(pns::UserProviderIdThunk::EntryAddress());
}

const int kJson = 0;  // the platform json: only its address matters to the stand-ins
const void* const kPlatformJson = &kJson;

void Reset() {
  g_createdNames.clear();
  g_createdJson.clear();
  g_shutDown.clear();
  g_closed.clear();
  g_closeHow.clear();
  g_providerIdCalls = 0;
  g_userProviderIdCalls = 0;
  g_tstringCalls = 0;
  g_appIdText = "1369078409873402";
  pns::ResetForTest();
  pns::CreateThunk::Reset();
  pns::DestroyThunk::Reset();
  pns::ProviderIdThunk::Reset();
  pns::UserProviderIdThunk::Reset();
  *pns::CreateThunk::OriginalOut() = reinterpret_cast<void*>(&StandInCreate);
  *pns::DestroyThunk::OriginalOut() = reinterpret_cast<void*>(&StandInDestroy);
  *pns::ProviderIdThunk::OriginalOut() = reinterpret_cast<void*>(&StandInProviderId);
  *pns::UserProviderIdThunk::OriginalOut() = reinterpret_cast<void*>(&StandInUserProviderId);
}

pns::GameFunctions Functions() {
  pns::GameFunctions functions;
  functions.unload = &StandInUnload;
  functions.tstring = &StandInTString;
  return functions;
}

// The two Creates of CR15Game::Initialize, in its order: the platform provider, then pnsrad.
void InitializeTheGame(const char* platform) {
  unsigned long platformHandle = 0;
  unsigned long radHandle = 0;
  QCHECK(Create()(&platformHandle, platform, kPlatformJson, nullptr) == 0);
  QCHECK(Create()(&radHandle, "pnsrad", nullptr, nullptr) == 0);
}

void TheNameRule() {
  QCHECK(std::strcmp(pns::RenamedProvider("pnsovr"), "pnsrad") == 0);
  QCHECK(std::strcmp(pns::RenamedProvider("pnsdemo"), "pnsrad") == 0);
  QCHECK(pns::RenamedProvider("pnsrad") == nullptr);
  QCHECK(pns::RenamedProvider("pnsovr2") == nullptr);
  QCHECK(pns::RenamedProvider("PNSOVR") == nullptr);
  QCHECK(pns::RenamedProvider("") == nullptr);
  QCHECK(pns::RenamedProvider(nullptr) == nullptr);
}

// Not armed, every entry is the game's call: pnsovr is loaded with its json.
void WithoutTheHooksTheGameLoadsPnsovr() {
  Reset();
  InitializeTheGame("pnsovr");
  QCHECK(g_createdNames.size() == 2);
  QCHECK(g_createdNames[0] == "pnsovr");
  QCHECK(g_createdJson[0] == kPlatformJson);
  QCHECK(ProviderId()(kOtherHandle) == kRadSymbol);
  QCHECK(pns::CurrentCounts().creates == 0);
}

// The platform Create loads pnsrad with a null json, after keeping the json's appid; pnsrad's own Create is untouched.
void PnsovrBecomesPnsrad() {
  Reset();
  pns::ArmForTest(Functions());
  InitializeTheGame("pnsovr");
  QCHECK(g_createdNames.size() == 2);
  QCHECK(g_createdNames[0] == "pnsrad");
  QCHECK(g_createdJson[0] == nullptr);
  QCHECK(g_createdNames[1] == "pnsrad");
  QCHECK(g_createdJson[1] == nullptr);
  QCHECK(pns::AppId() != nullptr && std::strcmp(pns::AppId(), "1369078409873402") == 0);
  QCHECK(g_tstringCalls == 1);
  const pns::Counts counts = pns::CurrentCounts();
  QCHECK(counts.creates == 2);
  QCHECK(counts.renamed == 1);
}

void PnsdemoBecomesPnsradToo() {
  Reset();
  pns::ArmForTest(Functions());
  InitializeTheGame("pnsdemo");
  QCHECK(g_createdNames[0] == "pnsrad");
  QCHECK(g_createdJson[0] == nullptr);
  QCHECK(pns::CurrentCounts().renamed == 1);
}

// Any other name, and its json, reach the game as they are.
void OtherNamesPassThrough() {
  Reset();
  pns::ArmForTest(Functions());
  unsigned long handle = 0;
  QCHECK(Create()(&handle, "pnsfoo", kPlatformJson, nullptr) == 0);
  QCHECK(g_createdNames.size() == 1 && g_createdNames[0] == "pnsfoo");
  QCHECK(g_createdJson[0] == kPlatformJson);
  QCHECK(pns::CurrentCounts().renamed == 0);
  QCHECK(pns::AppId() == nullptr);
  QCHECK(g_tstringCalls == 0);
}

// No TString, or a json with no appid: the rename still happens, and no appid is kept.
void TheRenameDoesNotNeedTheAppId() {
  Reset();
  pns::GameFunctions noTString = Functions();
  noTString.tstring = nullptr;
  pns::ArmForTest(noTString);
  InitializeTheGame("pnsovr");
  QCHECK(g_createdNames[0] == "pnsrad");
  QCHECK(pns::AppId() == nullptr);
  QCHECK(pns::CurrentCounts().renamed == 1);

  Reset();
  g_appIdText = nullptr;
  pns::ArmForTest(Functions());
  InitializeTheGame("pnsovr");
  QCHECK(g_createdNames[0] == "pnsrad");
  QCHECK(g_tstringCalls == 1);
  QCHECK(pns::AppId() == nullptr);
}

// ProviderID and UserProviderID are the game's until a rename happened, then OVR, the answer PC's pnsrad gives.
void TheProviderIsOvrOnceRenamed() {
  Reset();
  pns::ArmForTest(Functions());
  QCHECK(ProviderId()(kPnsradHandle) == kRadSymbol);
  QCHECK(UserProviderId()(kPnsradHandle) == kRadSymbol);
  QCHECK(g_providerIdCalls == 1 && g_userProviderIdCalls == 1);
  InitializeTheGame("pnsovr");
  QCHECK(ProviderId()(kPnsradHandle) == nevr_provider_identity::kOvrProviderSymbol);
  QCHECK(UserProviderId()(kPnsradHandle) == nevr_provider_identity::kOvrProviderSymbol);
  QCHECK(g_providerIdCalls == 1 && g_userProviderIdCalls == 1);
  const pns::Counts counts = pns::CurrentCounts();
  QCHECK(counts.provider_ids == 1);
  QCHECK(counts.user_provider_ids == 1);
}

// ShutdownEngine destroys the pnsdemo (0 here), pnsrad and pnsovr handles. After the rename the last two are one
// libpnsrad: its RadPluginShutdown runs once, and the second handle is only closed.
void TeardownShutsLibpnsradDownOnce() {
  Reset();
  pns::ArmForTest(Functions());
  InitializeTheGame("pnsovr");
  Destroy()(0);
  Destroy()(kPnsradHandle);
  Destroy()(kPnsradHandle);
  QCHECK(g_shutDown.size() == 2);  // the 0 handle reaches the game's own `cbz`, and pnsrad once
  QCHECK(g_shutDown[0] == 0);
  QCHECK(g_shutDown[1] == kPnsradHandle);
  QCHECK(g_closed.size() == 1);
  QCHECK(g_closed[0] == kPnsradHandle && g_closeHow[0] == 0);
  const pns::Counts counts = pns::CurrentCounts();
  QCHECK(counts.destroys == 2);
  QCHECK(counts.closed_only == 1);
}

// Two different modules are each shut down: the guard keys on the handle, not on the order.
void DifferentHandlesAreEachShutDown() {
  Reset();
  pns::ArmForTest(Functions());
  Destroy()(kOtherHandle);
  Destroy()(kPnsradHandle);
  QCHECK(g_shutDown.size() == 2);
  QCHECK(g_closed.empty());
}

// A module loaded again after its shutdown is live: its next Destroy shuts it down again.
void ACreateAfterTheShutdownMakesTheHandleLiveAgain() {
  Reset();
  pns::ArmForTest(Functions());
  Destroy()(kPnsradHandle);
  unsigned long handle = 0;
  QCHECK(Create()(&handle, "pnsrad", nullptr, nullptr) == 0);
  Destroy()(kPnsradHandle);
  QCHECK(g_shutDown.size() == 2);
  QCHECK(g_closed.empty());
}

// Without CSysModule::Unload there is no guard: the install refuses and arms nothing, so the game's calls stay.
void NoUnloadNoInstall() {
  Reset();
  pns::GameFunctions noUnload = Functions();
  noUnload.unload = nullptr;
  const sentinel::GotTarget none{"libnone.so", "none", sentinel::RelocKind::kJumpSlot, nullptr, std::nullopt};
  QCHECK(!pns::InstallAt(none, none, none, none, noUnload));
  InitializeTheGame("pnsovr");
  QCHECK(g_createdNames[0] == "pnsovr");
  QCHECK(pns::CurrentCounts().creates == 0);
}

}  // namespace

int main() {
  TheNameRule();
  WithoutTheHooksTheGameLoadsPnsovr();
  PnsovrBecomesPnsrad();
  PnsdemoBecomesPnsradToo();
  OtherNamesPassThrough();
  TheRenameDoesNotNeedTheAppId();
  TheProviderIsOvrOnceRenamed();
  TeardownShutsLibpnsradDownOnce();
  DifferentHandlesAreEachShutDown();
  ACreateAfterTheShutdownMakesTheHandleLiveAgain();
  NoUnloadNoInstall();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "pns_provider_hook_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("pns_provider_hook_test: all checks pass\n");
  return 0;
}
