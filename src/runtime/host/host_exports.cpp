#include "runtime/host/host_exports.h"

#include "extension/module_interface.h"  // NvrModuleContext (module entry-point signature)

#include <cstdint>
#include <cstring>

namespace NevrHost {
namespace {

HMODULE g_runtime_module = nullptr;

}  // namespace

void PublishRuntimeModule(HMODULE module) { g_runtime_module = module; }

FARPROC ResolveRuntimeExport(const char* name) {
  if (g_runtime_module == nullptr) return nullptr;
  return GetProcAddress(g_runtime_module, name);
}

}  // namespace NevrHost

// Mirror of struct NevRUPnPConfig in src/runtime/hook/patching.h:57. The host
// keeps only the layout here (to zero an absent runtime's result) rather than
// pull patching.h's heavier hook/logging includes. Keep this layout in sync
// with patching.h:57.
struct NevRUPnPConfig {
  BOOL   enabled;
  UINT16 port;
  CHAR   internalIp[46];
  CHAR   externalIp[46];
};

// --- NEVR cross-DLL exports (in exports.def; runtime-owned) -------------------
// Null-safe: each resolves the runtime's implementation and no-ops when nevr.dll
// is absent or the symbol is missing, so a degraded run never dereferences a null
// function pointer.

extern "C" void NEVR_ScheduleReturnToLobby() {
  const auto fn = reinterpret_cast<void (*)()>(NevrHost::ResolveRuntimeExport("NEVR_ScheduleReturnToLobby"));
  if (fn != nullptr) fn();
}

extern "C" void NEVR_GetUPnPConfig(NevRUPnPConfig* out) {
  if (out == nullptr) return;
  const auto fn = reinterpret_cast<void (*)(NevRUPnPConfig*)>(
      NevrHost::ResolveRuntimeExport("NEVR_GetUPnPConfig"));
  if (fn != nullptr) {
    fn(out);
  } else {
    std::memset(out, 0, sizeof(*out));  // degraded run: deterministic zeroed config
  }
}

extern "C" void NEVR_RegisterResourceOverride(std::uint64_t type_hash, std::uint64_t name_hash,
                                              const void* data, std::uint64_t size,
                                              const char* label) {
  const auto fn = reinterpret_cast<void (*)(std::uint64_t, std::uint64_t, const void*,
                                            std::uint64_t, const char*)>(
      NevrHost::ResolveRuntimeExport("NEVR_RegisterResourceOverride"));
  if (fn != nullptr) fn(type_hash, name_hash, data, size, label);
}

extern "C" void NEVR_ResetResourceOverrides() {
  const auto fn = reinterpret_cast<void (*)()>(NevrHost::ResolveRuntimeExport("NEVR_ResetResourceOverrides"));
  if (fn != nullptr) fn();
}

extern "C" void NEVR_DeregisterResourceOverrides(const void* data_start, const void* data_end) {
  const auto fn = reinterpret_cast<void (*)(const void*, const void*)>(
      NevrHost::ResolveRuntimeExport("NEVR_DeregisterResourceOverrides"));
  if (fn != nullptr) fn(data_start, data_end);
}

// --- Runtime module entry points (dllexport; runtime-owned) ------------------
// These names are exported by the host because the game-facing PE surface pins
// all 26 symbols on BugSplat64.dll; the implementations live in nevr.dll.

extern "C" __declspec(dllexport) std::uint32_t platform_compat_ApiVersion() {
  const auto fn = reinterpret_cast<std::uint32_t (*)()>(
      NevrHost::ResolveRuntimeExport("platform_compat_ApiVersion"));
  return fn != nullptr ? fn() : 0;
}

extern "C" __declspec(dllexport) int platform_compat_Init(const NvrModuleContext* ctx) {
  const auto fn = reinterpret_cast<int (*)(const NvrModuleContext*)>(
      NevrHost::ResolveRuntimeExport("platform_compat_Init"));
  return fn != nullptr ? fn(ctx) : 1;
}

extern "C" __declspec(dllexport) void platform_compat_Shutdown() {
  const auto fn = reinterpret_cast<void (*)()>(NevrHost::ResolveRuntimeExport("platform_compat_Shutdown"));
  if (fn != nullptr) fn();
}

extern "C" __declspec(dllexport) std::uint32_t token_auth_ApiVersion() {
  const auto fn = reinterpret_cast<std::uint32_t (*)()>(
      NevrHost::ResolveRuntimeExport("token_auth_ApiVersion"));
  return fn != nullptr ? fn() : 0;
}

extern "C" __declspec(dllexport) int token_auth_Init(const NvrModuleContext* ctx) {
  const auto fn = reinterpret_cast<int (*)(const NvrModuleContext*)>(
      NevrHost::ResolveRuntimeExport("token_auth_Init"));
  return fn != nullptr ? fn(ctx) : 1;
}

extern "C" __declspec(dllexport) void token_auth_Shutdown() {
  const auto fn = reinterpret_cast<void (*)()>(NevrHost::ResolveRuntimeExport("token_auth_Shutdown"));
  if (fn != nullptr) fn();
}

extern "C" __declspec(dllexport) const char* TokenAuth_GetToken() {
  const auto fn = reinterpret_cast<const char* (*)()>(NevrHost::ResolveRuntimeExport("TokenAuth_GetToken"));
  return fn != nullptr ? fn() : nullptr;
}

extern "C" __declspec(dllexport) std::uint64_t TokenAuth_GetDiscordId() {
  const auto fn = reinterpret_cast<std::uint64_t (*)()>(
      NevrHost::ResolveRuntimeExport("TokenAuth_GetDiscordId"));
  return fn != nullptr ? fn() : 0;
}

extern "C" __declspec(dllexport) const char* TokenAuth_GetUsername() {
  const auto fn = reinterpret_cast<const char* (*)()>(NevrHost::ResolveRuntimeExport("TokenAuth_GetUsername"));
  return fn != nullptr ? fn() : nullptr;
}
