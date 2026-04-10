/*
 * resource_registry.h — Plugin-side wrappers for gamepatches resource
 * override exports (NEVR_RegisterResourceOverride et al).
 *
 * Lazily resolves function pointers from dbgcore.dll (the deployed name
 * of gamepatches.dll) on first call. Returns false / no-op if dbgcore.dll
 * is not loaded (safe for test harnesses running outside the game process).
 *
 * Each plugin DLL gets its own resolved state — separate inline variables
 * per DLL, which is correct since each DLL independently loads.
 */
#pragma once

#include <windows.h>
#include <cstdint>
#include <mutex>

namespace nevr {
namespace detail {

inline std::once_flag g_resRegOnce;
inline void(*g_fnRegister)(uint64_t, uint64_t, const void*, uint64_t, const char*) = nullptr;
inline void(*g_fnDeregister)(const void*, const void*) = nullptr;
inline void(*g_fnReset)() = nullptr;

inline void ResolveResourceExports() {
    HMODULE h = GetModuleHandleA("dbgcore.dll");
    if (!h) return;
    g_fnRegister = reinterpret_cast<decltype(g_fnRegister)>(
        GetProcAddress(h, "NEVR_RegisterResourceOverride"));
    g_fnDeregister = reinterpret_cast<decltype(g_fnDeregister)>(
        GetProcAddress(h, "NEVR_DeregisterResourceOverrides"));
    g_fnReset = reinterpret_cast<decltype(g_fnReset)>(
        GetProcAddress(h, "NEVR_ResetResourceOverrides"));
}

} // namespace detail

inline bool RegisterResourceOverride(uint64_t type_hash, uint64_t name_hash,
                                     const void* data, uint64_t size,
                                     const char* label) {
    std::call_once(detail::g_resRegOnce, detail::ResolveResourceExports);
    if (!detail::g_fnRegister) return false;
    detail::g_fnRegister(type_hash, name_hash, data, size, label);
    return true;
}

inline void DeregisterResourceOverrides(const void* start, const void* end) {
    std::call_once(detail::g_resRegOnce, detail::ResolveResourceExports);
    if (detail::g_fnDeregister) detail::g_fnDeregister(start, end);
}

inline void ResetResourceOverrides() {
    std::call_once(detail::g_resRegOnce, detail::ResolveResourceExports);
    if (detail::g_fnReset) detail::g_fnReset();
}

} // namespace nevr
