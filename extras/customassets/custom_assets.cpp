#include "custom_assets.h"

#include "asset_converter.h"
#include "asset_interceptor.h"
#include "cache_manager.h"
#include "config_manager.h"
#include "hooking.h"
#include "http_client.h"
#include "logging.h"
#include "symbols.h"

namespace CustomAssets {

// Global instances
std::unique_ptr<ConfigManager> g_configManager;
std::unique_ptr<CacheManager> g_cacheManager;
std::unique_ptr<HttpClient> g_httpClient;
std::unique_ptr<AssetInterceptor> g_assetInterceptor;
std::unique_ptr<AssetConverter> g_assetConverter;

// Custom asset hash registry
std::unordered_map<uint64_t, std::string> g_customAssetRegistry;

// Simple hash function (matching EchoVR's symbol hash - FNV-1a 64-bit)
uint64_t ComputeSymbolHash(const char* str) {
  uint64_t hash = 0xcbf29ce484222325ULL;  // FNV offset basis
  while (*str) {
    hash ^= static_cast<uint64_t>(*str++);
    hash *= 0x100000001b3ULL;  // FNV prime
  }
  return hash;
}

bool Initialize() {
  Log(EchoVR::LogLevel::Info, "[CustomAssets] ============================================");
  Log(EchoVR::LogLevel::Info, "[CustomAssets] Initializing Custom Assets DLL v1.0");
  Log(EchoVR::LogLevel::Info, "[CustomAssets] ============================================");

  // Initialize hooking library
  if (!Hooking::Initialize()) {
    Log(EchoVR::LogLevel::Error, "[CustomAssets] Failed to initialize hooking library");
    return false;
  }

  // Create managers
  g_configManager = std::make_unique<ConfigManager>();
  g_cacheManager = std::make_unique<CacheManager>();
  g_httpClient = std::make_unique<HttpClient>();
  g_assetInterceptor = std::make_unique<AssetInterceptor>();
  g_assetConverter = std::make_unique<AssetConverter>();

  // Load configuration
  if (!g_configManager->Load()) {
    Log(EchoVR::LogLevel::Warning, "[CustomAssets] Failed to load config, continuing with defaults");
  }

  // Check if enabled
  if (!g_configManager->IsEnabled()) {
    Log(EchoVR::LogLevel::Info, "[CustomAssets] Custom assets disabled in config");
    return true;  // Not an error, just disabled
  }

  // Initialize cache manager
  std::string cacheDir = g_configManager->GetCacheDirectory();
  if (!g_cacheManager->Initialize(cacheDir)) {
    Log(EchoVR::LogLevel::Error, "[CustomAssets] Failed to initialize cache manager");
    return false;
  }

  // Initialize HTTP client
  auto& httpSettings = g_configManager->GetHttpSettings();
  if (!g_httpClient->Initialize(httpSettings.userAgent, httpSettings.timeoutMs, httpSettings.maxRetries)) {
    Log(EchoVR::LogLevel::Error, "[CustomAssets] Failed to initialize HTTP client");
    return false;
  }

  // Initialize asset interceptor
  if (!g_assetInterceptor->Initialize()) {
    Log(EchoVR::LogLevel::Error, "[CustomAssets] Failed to initialize asset interceptor");
    return false;
  }

  Log(EchoVR::LogLevel::Info, "[CustomAssets] Initialization complete!");
  return true;
}

void Shutdown() {
  Log(EchoVR::LogLevel::Info, "[CustomAssets] Shutting down");

  if (g_assetInterceptor) {
    g_assetInterceptor->Shutdown();
  }

  if (g_httpClient) {
    g_httpClient->Shutdown();
  }

  // Shutdown hooking library
  Hooking::Shutdown();

  // Clear managers
  g_assetInterceptor.reset();
  g_assetConverter.reset();
  g_httpClient.reset();
  g_cacheManager.reset();
  g_configManager.reset();

  Log(EchoVR::LogLevel::Info, "[CustomAssets] Shutdown complete");
}

bool IsCustomAssetId(uint64_t symbol_hash) {
  if (!g_assetInterceptor) {
    return false;
  }
  return g_assetInterceptor->IsCustomAsset(symbol_hash);
}

std::string GetAssetStringId(uint64_t symbol_hash) {
  if (!g_assetInterceptor) {
    return "";
  }
  return g_assetInterceptor->GetAssetId(symbol_hash);
}

void RegisterCustomAsset(const std::string& asset_id, const std::string& url) {
  if (!g_assetInterceptor || !g_configManager) {
    return;
  }

  // Compute hash for the asset ID
  uint64_t hash = ComputeSymbolHash(asset_id.c_str());

  // Register with interceptor
  g_assetInterceptor->RegisterAssetHash(hash, asset_id);

  // Add to config as server asset
  g_configManager->AddServerAsset(asset_id, url, "");

  Log(EchoVR::LogLevel::Info, "[CustomAssets] Registered custom asset: %s (0x%llx) -> %s", asset_id.c_str(), hash,
      url.c_str());
}

std::string GetAssetUrl(const std::string& asset_id) {
  if (!g_configManager) {
    return "";
  }

  AssetConfig config;
  if (g_configManager->GetAssetConfig(asset_id, config)) {
    return config.url;
  }

  return "";
}

}  // namespace CustomAssets
