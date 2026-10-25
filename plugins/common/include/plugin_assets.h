/*
 * plugin_assets.h — Plugin-side wrappers for the asset exports (API v7): NEVR_RegisterAsset and
 * NEVR_DeregisterAssets. See docs/reference/plugin-assets.md for the contract.
 *
 * Lazily resolves the exports on first call from the runtime module: BugSplat64.dll (its deployed name; the
 * runtime masquerades as the game's crash reporter) or dbgcore.dll (the older deployed name resource_registry.h
 * still uses). Returns NEVR_ASSET_UNAVAILABLE / no-ops when no loaded module exports them (a pre-v7 host, or a
 * test harness outside the game process).
 */
#pragma once

#include <windows.h>
#include <cstdint>
#include <mutex>

#include "extension/plugin_interface.h"

namespace nevr {

// Returned by RegisterAsset when the host has no asset exports. Not one of the host's NevrAssetStatus values.
constexpr int NEVR_ASSET_UNAVAILABLE = -1;

namespace detail {

inline std::once_flag g_assetOnce;
inline NevrRegisterAsset_fn g_fnRegisterAsset = nullptr;
inline NevrDeregisterAssets_fn g_fnDeregisterAssets = nullptr;

inline void ResolveAssetExports() {
    static const char* const kHostModules[] = {"BugSplat64.dll", "dbgcore.dll"};
    for (const char* module : kHostModules) {
        HMODULE h = GetModuleHandleA(module);
        if (!h) continue;
        auto registerFn = reinterpret_cast<NevrRegisterAsset_fn>(GetProcAddress(h, "NEVR_RegisterAsset"));
        auto deregisterFn =
            reinterpret_cast<NevrDeregisterAssets_fn>(GetProcAddress(h, "NEVR_DeregisterAssets"));
        if (registerFn && deregisterFn) {
            g_fnRegisterAsset = registerFn;
            g_fnDeregisterAssets = deregisterFn;
            return;
        }
    }
}

}  // namespace detail

// Register an asset carried in this plugin's image. Returns an NevrAssetStatus, or NEVR_ASSET_UNAVAILABLE.
inline int RegisterAsset(uint64_t type_hash, uint64_t name_hash, const void* data, uint64_t size,
                         const char* label) {
    std::call_once(detail::g_assetOnce, detail::ResolveAssetExports);
    if (!detail::g_fnRegisterAsset) return NEVR_ASSET_UNAVAILABLE;
    return detail::g_fnRegisterAsset(type_hash, name_hash, data, size, label);
}

// Deregister every asset whose data lies in [start, end). Call from NvrPluginShutdown with the plugin image.
inline void DeregisterAssets(const void* start, const void* end) {
    std::call_once(detail::g_assetOnce, detail::ResolveAssetExports);
    if (detail::g_fnDeregisterAssets) detail::g_fnDeregisterAssets(start, end);
}

}  // namespace nevr
