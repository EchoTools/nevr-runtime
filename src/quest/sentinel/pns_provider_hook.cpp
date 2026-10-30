#include "pns_provider_hook.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "hook_install.h"
#include "hook_log.h"
#include "runtime/patch/provider_identity.h"

namespace nevr_quest::pns_provider {

namespace {

namespace pinned = sentinel::pinned;

constexpr const char* kPnsrad = "pnsrad";

sentinel::GotHook g_createHook;
sentinel::GotHook g_destroyHook;
sentinel::GotHook g_providerIdHook;
sentinel::GotHook g_userProviderIdHook;

std::atomic<sentinel::pinned::SysModuleUnloadFn> g_unload{nullptr};
std::atomic<sentinel::pinned::CJsonTStringFn> g_tstring{nullptr};

std::atomic<unsigned long long> g_creates{0};
std::atomic<unsigned long long> g_renamed{0};
std::atomic<unsigned long long> g_destroys{0};
std::atomic<unsigned long long> g_closedOnly{0};
std::atomic<unsigned long long> g_providerIds{0};
std::atomic<unsigned long long> g_userProviderIds{0};

// Handles ShutdownEngine has already destroyed. It destroys three; a Create that succeeds again takes its handle back
// out. Fixed and lock-free: the hooks run on the game thread, but nothing here may allocate or block.
constexpr std::size_t kDestroyedSlots = 8;
std::atomic<unsigned long> g_destroyed[kDestroyedSlots] = {};

// The platform json's appid, kept by a renamed Create (the login sends it, as pnsovr did). Published by g_appIdKept.
constexpr std::size_t kAppIdBytes = 32;
char g_appId[kAppIdBytes] = {};
std::atomic<bool> g_appIdKept{false};

bool WasDestroyed(unsigned long handle) noexcept {
  for (const auto& slot : g_destroyed) {
    if (slot.load(std::memory_order_acquire) == handle) return true;
  }
  return false;
}

// False when every slot is taken: the handle is then destroyed as the game asks, as without the hook.
bool NoteDestroyed(unsigned long handle) noexcept {
  for (auto& slot : g_destroyed) {
    unsigned long empty = 0;
    if (slot.compare_exchange_strong(empty, handle, std::memory_order_acq_rel)) return true;
  }
  return false;
}

void ForgetDestroyed(unsigned long handle) noexcept {
  for (auto& slot : g_destroyed) {
    unsigned long seen = handle;
    slot.compare_exchange_strong(seen, 0, std::memory_order_acq_rel);
  }
}

const char* NameToken(const char* name) noexcept {
  if (name == nullptr) return "null";
  if (std::strcmp(name, "pnsovr") == 0) return "pnsovr";
  if (std::strcmp(name, "pnsdemo") == 0) return "pnsdemo";
  if (std::strcmp(name, kPnsrad) == 0) return kPnsrad;
  return "other";
}

// Copies the json's appid before the json is dropped. "kept", "absent" (no appid string), or "not_read" (no CJson::TString
// or no json).
const char* KeepAppId(const void* json) noexcept {
  const pinned::CJsonTStringFn tstring = g_tstring.load(std::memory_order_acquire);
  if (tstring == nullptr || json == nullptr) return "not_read";
  const char* const text = tstring(json, "appid", "", 0);
  if (text == nullptr || text[0] == '\0') return "absent";
  std::size_t length = 0;
  while (length + 1 < kAppIdBytes && text[length] != '\0') ++length;
  std::memcpy(g_appId, text, length);
  g_appId[length] = '\0';
  g_appIdKept.store(true, std::memory_order_release);
  return "kept";
}

// CNSProvider::Create. A platform provider request loads libpnsrad instead, with the null json libr15 gives the RAD
// provider; every other name is the game's call.
int CreateHandler(CreateThunk::Fn original, unsigned long* handle, const char* name, const void* json,
                  const void* callbacks) noexcept {
  g_creates.fetch_add(1, std::memory_order_acq_rel);
  const char* const asked = NameToken(name);
  const char* const renamed = RenamedProvider(name);
  const char* appid = "not_read";
  int result = 0;
  if (renamed != nullptr) {
    appid = KeepAppId(json);
    result = original(handle, renamed, nullptr, callbacks);
  } else {
    result = original(handle, name, json, callbacks);
  }
  const unsigned long loaded = handle != nullptr ? *handle : 0;
  if (renamed != nullptr && result == 0 && loaded != 0) g_renamed.fetch_add(1, std::memory_order_acq_rel);
  if (result == 0 && loaded != 0) ForgetDestroyed(loaded);
  sentinel::LogFields(result == 0 ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn, "pns_provider_create",
                      {{"asked", asked},
                       {"loaded", renamed != nullptr ? renamed : asked},
                       {"result", static_cast<long long>(result)},
                       {"handle_nonzero", loaded != 0},
                       {"appid", appid}});
  return result;
}

// CNSProvider::Destroy. A handle already destroyed in this process is only closed: its module's RadPluginShutdown has run.
void DestroyHandler(DestroyThunk::Fn original, unsigned long handle) noexcept {
  if (handle == 0) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "pns_provider_destroy", {{"action", "none"}, {"handle_nonzero", false}});
    original(handle);
    return;
  }
  g_destroys.fetch_add(1, std::memory_order_acq_rel);
  const pinned::SysModuleUnloadFn unload = g_unload.load(std::memory_order_acquire);
  if (unload != nullptr && WasDestroyed(handle)) {
    g_closedOnly.fetch_add(1, std::memory_order_acq_rel);
    sentinel::LogFields(sentinel::LogLevel::kInfo, "pns_provider_destroy",
                        {{"action", "close_only"}, {"handle_nonzero", true}});
    unload(handle, 0);
    return;
  }
  const bool noted = NoteDestroyed(handle);
  sentinel::LogFields(noted ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn, "pns_provider_destroy",
                      {{"action", "shutdown"}, {"handle_nonzero", true}, {"tracked", noted}});
  original(handle);
}

std::atomic<bool> g_providerIdLogged{false};
std::atomic<bool> g_userProviderIdLogged{false};

std::uint64_t Answer(std::uint64_t (*original)(unsigned long), unsigned long handle, const char* slot,
                     std::atomic<bool>& logged, std::atomic<unsigned long long>& answered) noexcept {
  const bool ovr = g_renamed.load(std::memory_order_acquire) != 0;
  if (!logged.exchange(true, std::memory_order_acq_rel)) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "pns_provider_id", {{"slot", slot}, {"answer", ovr ? "ovr" : "game"}});
  }
  if (!ovr) return original(handle);
  answered.fetch_add(1, std::memory_order_acq_rel);
  return nevr_provider_identity::kOvrProviderSymbol;
}

std::uint64_t ProviderIdHandler(ProviderIdThunk::Fn original, unsigned long handle) noexcept {
  return Answer(original, handle, "ProviderID", g_providerIdLogged, g_providerIds);
}

std::uint64_t UserProviderIdHandler(UserProviderIdThunk::Fn original, unsigned long handle) noexcept {
  return Answer(original, handle, "UserProviderID", g_userProviderIdLogged, g_userProviderIds);
}

NEVR_HOOK_RECORD(kCreateHook, CreateThunk, &CreateHandler);
NEVR_HOOK_RECORD(kDestroyHook, DestroyThunk, &DestroyHandler);
NEVR_HOOK_RECORD(kProviderIdHook, ProviderIdThunk, &ProviderIdHandler);
NEVR_HOOK_RECORD(kUserProviderIdHook, UserProviderIdThunk, &UserProviderIdHandler);

void Arm(const GameFunctions& functions) noexcept {
  g_unload.store(functions.unload, std::memory_order_release);
  g_tstring.store(functions.tstring, std::memory_order_release);
  DestroyThunk::Arm(kDestroyHook);
  ProviderIdThunk::Arm(kProviderIdHook);
  UserProviderIdThunk::Arm(kUserProviderIdHook);
  CreateThunk::Arm(kCreateHook);
}

template <typename Thunk>
bool InstallOne(sentinel::GotHook& hook, const sentinel::GotTarget& target, const char* slot) noexcept {
  const sentinel::GotStatus status = sentinel::InstallThunk<Thunk>(hook, target);
  const bool ok = status == sentinel::GotStatus::kOk;
  sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "pns_provider_install",
                      {{"result", ok ? "installed" : "refused"}, {"status", sentinel::GotStatusName(status)}, {"slot", slot}});
  return ok;
}

// The two functions from the loaded libr15, only when its build ID is the pinned one. `*why` names a failure.
GameFunctions ResolvePinned(const char** why) noexcept {
  GameFunctions functions;
  sentinel::ElfImage image;
  if (!sentinel::FindLoadedImage(pinned::kLibR15, &image)) {
    *why = "module_not_loaded";
    return functions;
  }
  char id[64] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id))) {
    *why = "no_build_id";
    return functions;
  }
  if (std::strcmp(id, pinned::LibR15PatchedBuildId()) != 0) {
    *why = "build_id_mismatch";
    return functions;
  }
  functions.unload = reinterpret_cast<pinned::SysModuleUnloadFn>(image.base + pinned::kSysModuleUnloadVaddr);
  functions.tstring = reinterpret_cast<pinned::CJsonTStringFn>(image.base + pinned::kCJsonTStringVaddr);
  *why = "pinned";
  return functions;
}

}  // namespace

const char* RenamedProvider(const char* name) noexcept {
  if (name == nullptr) return nullptr;
  if (std::strcmp(name, "pnsovr") == 0 || std::strcmp(name, "pnsdemo") == 0) return kPnsrad;
  return nullptr;
}

Counts CurrentCounts() noexcept {
  return {g_creates.load(std::memory_order_acquire),     g_renamed.load(std::memory_order_acquire),
          g_destroys.load(std::memory_order_acquire),    g_closedOnly.load(std::memory_order_acquire),
          g_providerIds.load(std::memory_order_acquire), g_userProviderIds.load(std::memory_order_acquire)};
}

const char* AppId() noexcept { return g_appIdKept.load(std::memory_order_acquire) ? g_appId : nullptr; }

bool InstallAt(const sentinel::GotTarget& create, const sentinel::GotTarget& destroy, const sentinel::GotTarget& providerId,
               const sentinel::GotTarget& userProviderId, const GameFunctions& functions) noexcept {
  if (functions.unload == nullptr) {
    sentinel::LogFields(sentinel::LogLevel::kError, "pns_provider_install",
                        {{"result", "refused"}, {"status", "no_unload"}, {"slot", "none"}});
    return false;
  }
  Arm(functions);
  // The rename goes last: until it is in, the game loads its own provider and the other three hooks change nothing.
  return InstallOne<DestroyThunk>(g_destroyHook, destroy, pinned::kProviderDestroySymbol) &&
         InstallOne<ProviderIdThunk>(g_providerIdHook, providerId, pinned::kProviderIdSymbol) &&
         InstallOne<UserProviderIdThunk>(g_userProviderIdHook, userProviderId, pinned::kUserProviderIdSymbol) &&
         InstallOne<CreateThunk>(g_createHook, create, pinned::kProviderCreateSymbol);
}

bool Install() noexcept {
  const char* why = "unknown";
  const GameFunctions functions = ResolvePinned(&why);
  if (functions.unload == nullptr) {
    sentinel::LogFields(sentinel::LogLevel::kError, "pns_provider_install",
                        {{"result", "refused"}, {"status", why}, {"slot", "none"}});
    return false;
  }
  return InstallAt(pinned::LibR15ProviderCreate(), pinned::LibR15ProviderDestroy(), pinned::LibR15ProviderId(),
                   pinned::LibR15UserProviderId(), functions);
}

void ArmForTest(const GameFunctions& functions) noexcept { Arm(functions); }

void ResetForTest() noexcept {
  g_creates.store(0, std::memory_order_relaxed);
  g_renamed.store(0, std::memory_order_relaxed);
  g_destroys.store(0, std::memory_order_relaxed);
  g_closedOnly.store(0, std::memory_order_relaxed);
  g_providerIds.store(0, std::memory_order_relaxed);
  g_userProviderIds.store(0, std::memory_order_relaxed);
  for (auto& slot : g_destroyed) slot.store(0, std::memory_order_relaxed);
  g_appIdKept.store(false, std::memory_order_relaxed);
  g_appId[0] = '\0';
  g_providerIdLogged.store(false, std::memory_order_relaxed);
  g_userProviderIdLogged.store(false, std::memory_order_relaxed);
  g_unload.store(nullptr, std::memory_order_relaxed);
  g_tstring.store(nullptr, std::memory_order_relaxed);
}

}  // namespace nevr_quest::pns_provider
