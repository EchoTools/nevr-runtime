#include "asset_interceptor.h"

#include <vector>

#include "asset_converter.h"
#include "cache_manager.h"
#include "config_manager.h"
#include "custom_assets.h"
#include "hooking.h"
#include "http_client.h"
#include "logging.h"
#include "symbols.h"

namespace CustomAssets {

// Global pointer to the interceptor instance
static AssetInterceptor* g_interceptorInstance = nullptr;

// Hook function that intercepts Loadout_ResolveDataFromId
void* Hook_Loadout_ResolveDataFromId(void* context, uint64_t cosmetic_id) {
  if (!g_interceptorInstance) {
    // This shouldn't happen, but safety first
    return nullptr;
  }

  // Check if this is a custom asset
  if (g_interceptorInstance->IsCustomAsset(cosmetic_id)) {
    std::string assetId = g_interceptorInstance->GetAssetId(cosmetic_id);

    Log(EchoVR::LogLevel::Info, "[CustomAssets] Intercepted custom asset: %s (0x%llx)", assetId.c_str(), cosmetic_id);

    // Try to load custom asset
    // 1. Check if we have config for this asset
    AssetConfig config;
    if (!g_configManager->GetAssetConfig(assetId, config)) {
      Log(EchoVR::LogLevel::Warning, "[CustomAssets] No URL configured for custom asset: %s", assetId.c_str());
      // No config - call original function to get default/fallback
      return g_interceptorInstance->GetOriginalFunction()(context, cosmetic_id);
    }

    // 2. Check cache
    if (g_cacheManager->HasCached(assetId) && !g_cacheManager->NeedsRefresh(assetId)) {
      Log(EchoVR::LogLevel::Info, "[CustomAssets] Loading from cache: %s", assetId.c_str());

      auto cached = g_cacheManager->LoadCached(assetId);
      if (cached.valid) {
        // Parse the cached JSON
        std::string jsonStr(cached.data.begin(), cached.data.end());
        AssetBundle bundle = g_assetConverter->ParseAssetBundle(jsonStr);

        if (bundle.valid) {
          // Convert to game format
          void* gameAsset = g_assetConverter->ConvertToGameFormat(bundle);
          if (gameAsset) {
            return gameAsset;
          }
        }
      }
    }

    // 3. Download from URL
    Log(EchoVR::LogLevel::Info, "[CustomAssets] Downloading asset from: %s", config.url.c_str());

    // Prepare cache info for conditional request
    CacheInfo cacheInfo;
    if (g_cacheManager->HasCached(assetId)) {
      auto cached = g_cacheManager->LoadCached(assetId);
      if (cached.valid) {
        cacheInfo.etag = cached.metadata.etag;
        cacheInfo.lastModified = cached.metadata.lastModified;
        cacheInfo.cachedBody = std::string(cached.data.begin(), cached.data.end());
      }
    }

    HttpResponse response = g_httpClient->Get(config.url, g_cacheManager->HasCached(assetId) ? &cacheInfo : nullptr);

    if (!response.success) {
      Log(EchoVR::LogLevel::Error, "[CustomAssets] Failed to download asset %s: %s", assetId.c_str(),
          response.errorMessage.c_str());
      // Fallback to original function
      return g_interceptorInstance->GetOriginalFunction()(context, cosmetic_id);
    }

    // 4. Parse and convert
    AssetBundle bundle = g_assetConverter->ParseAssetBundle(response.body);
    if (!bundle.valid) {
      Log(EchoVR::LogLevel::Error, "[CustomAssets] Invalid asset bundle: %s", assetId.c_str());
      return g_interceptorInstance->GetOriginalFunction()(context, cosmetic_id);
    }

    // 5. Save to cache
    CacheMetadata metadata;
    metadata.assetId = assetId;
    metadata.url = config.url;
    metadata.etag = response.etag;
    metadata.lastModified = response.lastModified;
    metadata.sizeBytes = response.body.size();
    // TODO: Compute SHA256 hash

    std::vector<uint8_t> bodyData(response.body.begin(), response.body.end());
    g_cacheManager->SaveCached(assetId, bodyData, metadata);

    // 6. Convert to game format
    void* gameAsset = g_assetConverter->ConvertToGameFormat(bundle);
    if (gameAsset) {
      return gameAsset;
    }

    // Conversion failed, fallback to original
    Log(EchoVR::LogLevel::Warning, "[CustomAssets] Conversion failed for %s, using fallback", assetId.c_str());
  }

  // Not a custom asset or failed to load - call original function
  return g_interceptorInstance->GetOriginalFunction()(context, cosmetic_id);
}

AssetInterceptor::AssetInterceptor() : originalFunction_(nullptr), initialized_(false) {}

AssetInterceptor::~AssetInterceptor() { Shutdown(); }

bool AssetInterceptor::Initialize() {
  if (initialized_) {
    return true;
  }

  Log(EchoVR::LogLevel::Info, "[CustomAssets] Initializing asset interceptor");

  // Set the global instance pointer
  g_interceptorInstance = this;

  // Get the address of Loadout_ResolveDataFromId
  // Address: 0x1404f37a0 (from documentation)
  originalFunction_ = reinterpret_cast<Loadout_ResolveDataFromId_t>(0x1404f37a0);

  // Install the hook
  if (!Hooking::Attach(&(PVOID&)originalFunction_, (PVOID)Hook_Loadout_ResolveDataFromId)) {
    Log(EchoVR::LogLevel::Error, "[CustomAssets] Failed to install hook for Loadout_ResolveDataFromId");
    return false;
  }

  Log(EchoVR::LogLevel::Info, "[CustomAssets] Successfully hooked Loadout_ResolveDataFromId at 0x1404f37a0");

  initialized_ = true;
  return true;
}

void AssetInterceptor::Shutdown() {
  if (!initialized_) {
    return;
  }

  // Unhook (MinHook disables all hooks)
  Hooking::Detach(&(PVOID&)originalFunction_, (PVOID)Hook_Loadout_ResolveDataFromId);

  g_interceptorInstance = nullptr;
  initialized_ = false;
}

void AssetInterceptor::RegisterAssetHash(uint64_t hash, const std::string& assetId) {
  customAssetHashes_[hash] = assetId;
  Log(EchoVR::LogLevel::Info, "[CustomAssets] Registered custom asset: %s -> 0x%llx", assetId.c_str(), hash);
}

bool AssetInterceptor::IsCustomAsset(uint64_t hash) {
  return customAssetHashes_.find(hash) != customAssetHashes_.end();
}

std::string AssetInterceptor::GetAssetId(uint64_t hash) {
  auto it = customAssetHashes_.find(hash);
  if (it != customAssetHashes_.end()) {
    return it->second;
  }
  return "";
}

}  // namespace CustomAssets
