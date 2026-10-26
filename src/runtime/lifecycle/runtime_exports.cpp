#include "runtime/lifecycle/runtime_exports.h"

#include "core/logging.h"
#include "extension/module_interface.h"
#include "runtime/hook/patching.h"
#include "runtime/lifecycle/export_guard.h"
#include "runtime/patch/plugin_assets.h"

#include <cstdint>

// The functions behind the host's exports, defined where they always were (the modules, initialize.cpp,
// resource_override.cpp, plugin_assets.cpp). Declared here as the host's table needs them.
extern "C" {
void NEVR_ScheduleReturnToLobby();
void NEVR_GetUPnPConfig(NevrUPnPConfig* out);
void NEVR_RegisterResourceOverride(uint64_t type_hash, uint64_t name_hash, const void* data, uint64_t size,
                                   const char* label);
void NEVR_ResetResourceOverrides();
void NEVR_DeregisterResourceOverrides(const void* data_start, const void* data_end);
}

// The module entry points are declared NEVR_MODULE_API (dllimport without NEVR_MODULE_EXPORTS) in the module
// interface; the static modules are linked into this DLL, so declare them plainly, as boot.cpp does.
extern "C" {
int platform_compat_Init(const NvrModuleContext* ctx);
uint32_t platform_compat_ApiVersion(void);
void platform_compat_Shutdown(void);
int token_auth_Init(const NvrModuleContext* ctx);
uint32_t token_auth_ApiVersion(void);
void token_auth_Shutdown(void);
const char* TokenAuth_GetToken(void);
uint64_t TokenAuth_GetDiscordId(void);
const char* TokenAuth_GetUsername(void);
}

namespace nevr_runtime_exports {
namespace {

// Log what threw, once per call, at Warning: the caller gets the failure value and the log says why.
void Threw(const char* entry, const char* what) noexcept {
  Log(EchoVR::LogLevel::Warning, "[NEVR.BOOT] host export entry=%s threw: %s", entry,
      what != nullptr ? what : "(not a std::exception)");
}

void ScheduleReturnToLobby() {
  GuardedVoid("schedule_return_to_lobby", &Threw, [] { NEVR_ScheduleReturnToLobby(); });
}

void GetUPnPConfig(void* out) {
  GuardedVoid("get_upnp_config", &Threw, [out] { NEVR_GetUPnPConfig(static_cast<NevrUPnPConfig*>(out)); });
}

void RegisterResourceOverride(std::uint64_t type_hash, std::uint64_t name_hash, const void* data,
                              std::uint64_t size, const char* label) {
  GuardedVoid("register_resource_override", &Threw,
              [=] { NEVR_RegisterResourceOverride(type_hash, name_hash, data, size, label); });
}

void ResetResourceOverrides() {
  GuardedVoid("reset_resource_overrides", &Threw, [] { NEVR_ResetResourceOverrides(); });
}

void DeregisterResourceOverrides(const void* data_start, const void* data_end) {
  GuardedVoid("deregister_resource_overrides", &Threw,
              [=] { NEVR_DeregisterResourceOverrides(data_start, data_end); });
}

int RegisterAsset(std::uint64_t type_hash, std::uint64_t name_hash, const void* data, std::uint64_t size,
                  const char* label) {
  return Guarded("register_asset", &Threw, kAssetUnavailable,
                 [=] { return NEVR_RegisterAsset(type_hash, name_hash, data, size, label); });
}

void DeregisterAssets(const void* data_start, const void* data_end) {
  GuardedVoid("deregister_assets", &Threw, [=] { NEVR_DeregisterAssets(data_start, data_end); });
}

const char* TokenAuthGetToken() {
  return Guarded("token_auth_get_token", &Threw, "", [] { return TokenAuth_GetToken(); });
}

std::uint64_t TokenAuthGetDiscordId() {
  return Guarded("token_auth_get_discord_id", &Threw, std::uint64_t{0}, [] { return TokenAuth_GetDiscordId(); });
}

const char* TokenAuthGetUsername() {
  return Guarded("token_auth_get_username", &Threw, "", [] { return TokenAuth_GetUsername(); });
}

std::uint32_t PlatformCompatApiVersion() {
  return Guarded("platform_compat_api_version", &Threw, std::uint32_t{0}, [] { return platform_compat_ApiVersion(); });
}

int PlatformCompatInit(const void* ctx) {
  return Guarded("platform_compat_init", &Threw, kInitFailed,
                 [ctx] { return platform_compat_Init(static_cast<const NvrModuleContext*>(ctx)); });
}

void PlatformCompatShutdown() {
  GuardedVoid("platform_compat_shutdown", &Threw, [] { platform_compat_Shutdown(); });
}

std::uint32_t TokenAuthApiVersion() {
  return Guarded("token_auth_api_version", &Threw, std::uint32_t{0}, [] { return token_auth_ApiVersion(); });
}

int TokenAuthInit(const void* ctx) {
  return Guarded("token_auth_init", &Threw, kInitFailed,
                 [ctx] { return token_auth_Init(static_cast<const NvrModuleContext*>(ctx)); });
}

void TokenAuthShutdown() {
  GuardedVoid("token_auth_shutdown", &Threw, [] { token_auth_Shutdown(); });
}

const NevrRuntimeExports g_table = [] {
  NevrRuntimeExports table{};
  table.schedule_return_to_lobby = &ScheduleReturnToLobby;
  table.get_upnp_config = &GetUPnPConfig;
  table.register_resource_override = &RegisterResourceOverride;
  table.reset_resource_overrides = &ResetResourceOverrides;
  table.deregister_resource_overrides = &DeregisterResourceOverrides;
  table.register_asset = &RegisterAsset;
  table.deregister_assets = &DeregisterAssets;
  table.token_auth_get_token = &TokenAuthGetToken;
  table.token_auth_get_discord_id = &TokenAuthGetDiscordId;
  table.token_auth_get_username = &TokenAuthGetUsername;
  table.platform_compat_api_version = &PlatformCompatApiVersion;
  table.platform_compat_init = &PlatformCompatInit;
  table.platform_compat_shutdown = &PlatformCompatShutdown;
  table.token_auth_api_version = &TokenAuthApiVersion;
  table.token_auth_init = &TokenAuthInit;
  table.token_auth_shutdown = &TokenAuthShutdown;
  return table;
}();

}  // namespace

const NevrRuntimeExports& Table() { return g_table; }

}  // namespace nevr_runtime_exports
