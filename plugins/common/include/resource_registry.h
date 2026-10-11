/*
 * resource_registry.h — Plugin-side wrappers for the runtime's resource
 * override exports (NEVR_RegisterResourceOverride et al).
 *
 * Lazily resolves function pointers from the module that exports them on
 * first call. Returns false / no-op if no loaded module exports them (safe
 * for test harnesses running outside the game process).
 *
 * Each plugin DLL gets its own resolved state — separate inline variables
 * per DLL, which is correct since each DLL independently loads.
 */
#pragma once

#include <windows.h>
#include <cstdarg>
#include <cstdint>
#include <mutex>

#include "plugin_logger.h"

namespace nevr {
namespace detail {

using RegisterResourceOverrideFn = void (*)(uint64_t, uint64_t, const void*, uint64_t, const char*);
using DeregisterResourceOverridesFn = void (*)(const void*, const void*);
using ResetResourceOverridesFn = void (*)();

inline std::once_flag g_resRegOnce;
inline RegisterResourceOverrideFn g_fnRegister = nullptr;
inline DeregisterResourceOverridesFn g_fnDeregister = nullptr;
inline ResetResourceOverridesFn g_fnReset = nullptr;

// The modules that may export the override functions, in the order they are tried.
inline constexpr const char* kResourceHostModules[] = {"dbgcore.dll"};

struct ResourceExports {
    RegisterResourceOverrideFn registerFn = nullptr;
    DeregisterResourceOverridesFn deregisterFn = nullptr;
    ResetResourceOverridesFn resetFn = nullptr;
};

// Resolves the three exports from the first listed module that is loaded and exports all of them.
// `moduleLookup(name)` returns the module handle (or null); `procLookup(module, name)` the export.
template <typename ModuleLookup, typename ProcLookup>
inline ResourceExports ResolveResourceExportsFrom(ModuleLookup moduleLookup, ProcLookup procLookup) {
    for (const char* moduleName : kResourceHostModules) {
        HMODULE h = moduleLookup(moduleName);
        if (!h) continue;
        ResourceExports found;
        found.registerFn = reinterpret_cast<RegisterResourceOverrideFn>(procLookup(h, "NEVR_RegisterResourceOverride"));
        found.deregisterFn =
            reinterpret_cast<DeregisterResourceOverridesFn>(procLookup(h, "NEVR_DeregisterResourceOverrides"));
        found.resetFn = reinterpret_cast<ResetResourceOverridesFn>(procLookup(h, "NEVR_ResetResourceOverrides"));
        if (found.registerFn && found.deregisterFn && found.resetFn) return found;
    }
    return ResourceExports{};
}

inline void LogResourceExportsMissing(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ::nevr::WritePluginLog("[NEVR.PLUGIN]", "WARNING", fmt, args);
    va_end(args);
}

inline void ResolveResourceExports() {
    const ResourceExports found = ResolveResourceExportsFrom(
        [](const char* name) { return GetModuleHandleA(name); },
        [](HMODULE module, const char* name) { return reinterpret_cast<void*>(GetProcAddress(module, name)); });
    g_fnRegister = found.registerFn;
    g_fnDeregister = found.deregisterFn;
    g_fnReset = found.resetFn;
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
