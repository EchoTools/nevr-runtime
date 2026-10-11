/*
 * plugin_assets.h — Plugin-side wrappers for the asset exports (API v6): NEVR_RegisterAsset and
 * NEVR_DeregisterAssets. See docs/reference/plugin-assets.md for the contract.
 *
 * Lazily resolves the exports from dbgcore.dll (the deployed name of the runtime module) on first call, like
 * resource_registry.h. Returns NEVR_ASSET_UNAVAILABLE / no-ops when the host does not export them (a pre-v6
 * host, or a test harness outside the game process).
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
    HMODULE h = GetModuleHandleA("dbgcore.dll");
    if (!h) return;
    g_fnRegisterAsset =
        reinterpret_cast<NevrRegisterAsset_fn>(GetProcAddress(h, "NEVR_RegisterAsset"));
    g_fnDeregisterAssets =
        reinterpret_cast<NevrDeregisterAssets_fn>(GetProcAddress(h, "NEVR_DeregisterAssets"));
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
