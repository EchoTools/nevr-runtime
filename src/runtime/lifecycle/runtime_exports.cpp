#include "runtime/lifecycle/runtime_exports.h"

#include "core/logging.h"
#include "extension/module_interface.h"
#include "runtime/hook/patching.h"
#include "runtime/patch/plugin_assets.h"

#include <cstdint>
#include <exception>

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
void Threw(const char* entry, const std::exception& error) noexcept {
  Log(EchoVR::LogLevel::Warning, "[NEVR.BOOT] host export entry=%s threw: %s", entry, error.what());
}

void ScheduleReturnToLobby() {
  try {
    NEVR_ScheduleReturnToLobby();
  } catch (const std::exception& error) {
    Threw("schedule_return_to_lobby", error);
  }
}

void GetUPnPConfig(void* out) {
  try {
    NEVR_GetUPnPConfig(static_cast<NevrUPnPConfig*>(out));
  } catch (const std::exception& error) {
    Threw("get_upnp_config", error);
  }
}

void RegisterResourceOverride(std::uint64_t type_hash, std::uint64_t name_hash, const void* data,
                              std::uint64_t size, const char* label) {
  try {
    NEVR_RegisterResourceOverride(type_hash, name_hash, data, size, label);
  } catch (const std::exception& error) {
    Threw("register_resource_override", error);
  }
}

void ResetResourceOverrides() {
  try {
    NEVR_ResetResourceOverrides();
  } catch (const std::exception& error) {
    Threw("reset_resource_overrides", error);
  }
}

void DeregisterResourceOverrides(const void* data_start, const void* data_end) {
  try {
    NEVR_DeregisterResourceOverrides(data_start, data_end);
  } catch (const std::exception& error) {
    Threw("deregister_resource_overrides", error);
  }
}

int RegisterAsset(std::uint64_t type_hash, std::uint64_t name_hash, const void* data, std::uint64_t size,
                  const char* label) {
  try {
    return NEVR_RegisterAsset(type_hash, name_hash, data, size, label);
  } catch (const std::exception& error) {
    Threw("register_asset", error);
    return kAssetUnavailable;
  }
}

void DeregisterAssets(const void* data_start, const void* data_end) {
  try {
    NEVR_DeregisterAssets(data_start, data_end);
  } catch (const std::exception& error) {
    Threw("deregister_assets", error);
  }
}

const char* TokenAuthGetToken() {
  try {
    return TokenAuth_GetToken();
  } catch (const std::exception& error) {
    Threw("token_auth_get_token", error);
    return "";
  }
}

std::uint64_t TokenAuthGetDiscordId() {
  try {
    return TokenAuth_GetDiscordId();
  } catch (const std::exception& error) {
    Threw("token_auth_get_discord_id", error);
    return 0;
  }
}

const char* TokenAuthGetUsername() {
  try {
    return TokenAuth_GetUsername();
  } catch (const std::exception& error) {
    Threw("token_auth_get_username", error);
    return "";
  }
}

std::uint32_t PlatformCompatApiVersion() {
  try {
    return platform_compat_ApiVersion();
  } catch (const std::exception& error) {
    Threw("platform_compat_api_version", error);
    return 0;
  }
}

int PlatformCompatInit(const void* ctx) {
  try {
    return platform_compat_Init(static_cast<const NvrModuleContext*>(ctx));
  } catch (const std::exception& error) {
    Threw("platform_compat_init", error);
    return kInitFailed;
  }
}

void PlatformCompatShutdown() {
  try {
    platform_compat_Shutdown();
  } catch (const std::exception& error) {
    Threw("platform_compat_shutdown", error);
  }
}

std::uint32_t TokenAuthApiVersion() {
  try {
    return token_auth_ApiVersion();
  } catch (const std::exception& error) {
    Threw("token_auth_api_version", error);
    return 0;
  }
}

int TokenAuthInit(const void* ctx) {
  try {
    return token_auth_Init(static_cast<const NvrModuleContext*>(ctx));
  } catch (const std::exception& error) {
    Threw("token_auth_init", error);
    return kInitFailed;
  }
}

void TokenAuthShutdown() {
  try {
    token_auth_Shutdown();
  } catch (const std::exception& error) {
    Threw("token_auth_shutdown", error);
  }
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
