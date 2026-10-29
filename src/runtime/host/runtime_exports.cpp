// The sixteen exports of BugSplat64.dll that nevr-runtime.dll owns (docs/adr/0012-static-install-root.md).
//
// Plugins, the launcher and the tests resolve these by name from BugSplat64.dll (GetProcAddress on the host
// module), and tools/verify_pe_contract.py pins the host's export table. The code behind them is in the runtime,
// so each export here is a typed thunk, not a PE forwarder: a forwarder to nevr-runtime.dll would make the
// loader map the runtime when a caller resolved the name, outside the rendezvous, before the runtime was
// verified and initialized. A thunk calls the runtime only through the table the runtime published
// (core/runtime_abi.h, NevrRuntimeApi::exports) after its initialize() succeeded; before that, and when the table
// or the entry is absent, it returns the failure value written beside it.
//
// No thunk throws, allocates or takes a lock: it reads one atomic pointer and calls through it. The runtime's
// entries catch their own exceptions (runtime/lifecycle/runtime_exports.cpp).

#include "core/runtime_abi.h"
#include "runtime/host/host_entry.h"

namespace {

const NevrRuntimeExports* Table() noexcept {
  return nevr_runtime_abi::RuntimeExports(nevr_host::Rendezvous().RuntimeApi());
}

// The string a name-returning thunk gives before the runtime is up: valid, empty, never freed.
const char kEmptyString[] = "";

}  // namespace

extern "C" {

// --- NEVR_* (failure value: nothing happens) ---

void NEVR_ScheduleReturnToLobby() {
  const NevrRuntimeExports* table = Table();
  if (table != nullptr && table->schedule_return_to_lobby != nullptr) table->schedule_return_to_lobby();
}

// `out` is left untouched before the runtime is up: the caller's struct is not the host's to size.
void NEVR_GetUPnPConfig(void* out) {
  const NevrRuntimeExports* table = Table();
  if (table != nullptr && table->get_upnp_config != nullptr) table->get_upnp_config(out);
}

void NEVR_RegisterResourceOverride(std::uint64_t type_hash, std::uint64_t name_hash, const void* data,
                                   std::uint64_t size, const char* label) {
  const NevrRuntimeExports* table = Table();
  if (table != nullptr && table->register_resource_override != nullptr) {
    table->register_resource_override(type_hash, name_hash, data, size, label);
  }
}

void NEVR_ResetResourceOverrides() {
  const NevrRuntimeExports* table = Table();
  if (table != nullptr && table->reset_resource_overrides != nullptr) table->reset_resource_overrides();
}

void NEVR_DeregisterResourceOverrides(const void* data_start, const void* data_end) {
  const NevrRuntimeExports* table = Table();
  if (table != nullptr && table->deregister_resource_overrides != nullptr) {
    table->deregister_resource_overrides(data_start, data_end);
  }
}

// Failure value -1: the plugins' NEVR_ASSET_UNAVAILABLE (sdk/include/nevr/pc/plugin_assets.h), the value a
// plugin already gets from a host with no asset exports, and no NevrAssetStatus.
int NEVR_RegisterAsset(std::uint64_t type_hash, std::uint64_t name_hash, const void* data, std::uint64_t size,
                       const char* label) {
  const NevrRuntimeExports* table = Table();
  if (table == nullptr || table->register_asset == nullptr) return -1;
  return table->register_asset(type_hash, name_hash, data, size, label);
}

void NEVR_DeregisterAssets(const void* data_start, const void* data_end) {
  const NevrRuntimeExports* table = Table();
  if (table != nullptr && table->deregister_assets != nullptr) table->deregister_assets(data_start, data_end);
}

// --- TokenAuth_* (failure value: no token, no id, no name) ---

const char* TokenAuth_GetToken() {
  const NevrRuntimeExports* table = Table();
  if (table == nullptr || table->token_auth_get_token == nullptr) return kEmptyString;
  return table->token_auth_get_token();
}

std::uint64_t TokenAuth_GetDiscordId() {
  const NevrRuntimeExports* table = Table();
  if (table == nullptr || table->token_auth_get_discord_id == nullptr) return 0;
  return table->token_auth_get_discord_id();
}

const char* TokenAuth_GetUsername() {
  const NevrRuntimeExports* table = Table();
  if (table == nullptr || table->token_auth_get_username == nullptr) return kEmptyString;
  return table->token_auth_get_username();
}

// --- The static modules (failure values: API version 0, which no loader accepts; init -1, a failure) ---

std::uint32_t platform_compat_ApiVersion() {
  const NevrRuntimeExports* table = Table();
  if (table == nullptr || table->platform_compat_api_version == nullptr) return 0;
  return table->platform_compat_api_version();
}

int platform_compat_Init(const void* ctx) {
  const NevrRuntimeExports* table = Table();
  if (table == nullptr || table->platform_compat_init == nullptr) return -1;
  return table->platform_compat_init(ctx);
}

void platform_compat_Shutdown() {
  const NevrRuntimeExports* table = Table();
  if (table != nullptr && table->platform_compat_shutdown != nullptr) table->platform_compat_shutdown();
}

std::uint32_t token_auth_ApiVersion() {
  const NevrRuntimeExports* table = Table();
  if (table == nullptr || table->token_auth_api_version == nullptr) return 0;
  return table->token_auth_api_version();
}

int token_auth_Init(const void* ctx) {
  const NevrRuntimeExports* table = Table();
  if (table == nullptr || table->token_auth_init == nullptr) return -1;
  return table->token_auth_init(ctx);
}

void token_auth_Shutdown() {
  const NevrRuntimeExports* table = Table();
  if (table != nullptr && table->token_auth_shutdown != nullptr) table->token_auth_shutdown();
}

}  // extern "C"
