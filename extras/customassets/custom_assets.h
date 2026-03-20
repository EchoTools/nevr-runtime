#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace CustomAssets {

// Forward declarations
class ConfigManager;
class CacheManager;
class HttpClient;
class AssetInterceptor;
class AssetConverter;

// Initialize the custom assets system
bool Initialize();

// Shutdown and cleanup
void Shutdown();

// Check if an asset ID is a custom asset (starts with "custom_")
bool IsCustomAssetId(uint64_t symbol_hash);

// Get the string ID from a symbol hash
std::string GetAssetStringId(uint64_t symbol_hash);

// Register a custom asset (computes and stores hash)
void RegisterCustomAsset(const std::string& asset_id, const std::string& url);

// Get the URL for a custom asset
std::string GetAssetUrl(const std::string& asset_id);

// Global instances (managed internally)
extern std::unique_ptr<ConfigManager> g_configManager;
extern std::unique_ptr<CacheManager> g_cacheManager;
extern std::unique_ptr<HttpClient> g_httpClient;
extern std::unique_ptr<AssetInterceptor> g_assetInterceptor;
extern std::unique_ptr<AssetConverter> g_assetConverter;

}  // namespace CustomAssets
