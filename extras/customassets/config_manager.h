#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>

namespace CustomAssets {

struct AssetConfig {
  std::string url;
  bool overrideServer;
  std::string etag;
};

struct HttpSettings {
  int timeoutMs;
  int maxRetries;
  std::string userAgent;
};

class ConfigManager {
 public:
  ConfigManager();
  ~ConfigManager();

  // Load configuration from file
  bool Load();

  // Get asset configuration
  bool GetAssetConfig(const std::string& assetId, AssetConfig& config) const;

  // Get HTTP settings
  const HttpSettings& GetHttpSettings() const { return httpSettings_; }

  // Check if custom assets are enabled
  bool IsEnabled() const { return enabled_; }

  // Get cache directory override (empty = use default)
  std::string GetCacheDirectory() const { return cacheDirectory_; }

  // Add server-provided asset mapping
  void AddServerAsset(const std::string& assetId, const std::string& url, const std::string& etag);

  // Get config file path
  static std::filesystem::path GetConfigPath();

 private:
  bool enabled_;
  std::string cacheDirectory_;
  std::unordered_map<std::string, AssetConfig> clientAssets_;
  std::unordered_map<std::string, AssetConfig> serverAssets_;
  HttpSettings httpSettings_;
};

}  // namespace CustomAssets
