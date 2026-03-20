#include "config_manager.h"

#include <shlobj.h>

#include <fstream>

#include "logging.h"
#include "third_party/json.hpp"

using json = nlohmann::json;

namespace CustomAssets {

ConfigManager::ConfigManager() : enabled_(true), httpSettings_{30000, 3, "EchoVR-CustomAssets/1.0"} {}

ConfigManager::~ConfigManager() {}

std::filesystem::path ConfigManager::GetConfigPath() {
  // Get %APPDATA% on Windows
  wchar_t* appDataPath = nullptr;
  if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appDataPath) == S_OK) {
    std::filesystem::path configPath = appDataPath;
    CoTaskMemFree(appDataPath);

    configPath /= "EchoVR";
    configPath /= "CustomAssets";
    configPath /= "config.json";

    return configPath;
  }

  // Fallback to relative path
  return "./CustomAssets/config.json";
}

bool ConfigManager::Load() {
  auto configPath = GetConfigPath();

  Log(EchoVR::LogLevel::Info, "[CustomAssets] Loading config from: %s", configPath.string().c_str());

  if (!std::filesystem::exists(configPath)) {
    Log(EchoVR::LogLevel::Warning, "[CustomAssets] Config file not found, using defaults");
    return true;  // Not an error, just use defaults
  }

  try {
    std::ifstream file(configPath);
    if (!file.is_open()) {
      Log(EchoVR::LogLevel::Error, "[CustomAssets] Failed to open config file");
      return false;
    }

    json config = json::parse(file);

    // Load enabled flag
    if (config.contains("enabled")) {
      enabled_ = config["enabled"].get<bool>();
    }

    // Load cache directory override
    if (config.contains("cache_directory") && !config["cache_directory"].is_null()) {
      cacheDirectory_ = config["cache_directory"].get<std::string>();
    }

    // Load HTTP settings
    if (config.contains("http_settings")) {
      auto& http = config["http_settings"];
      if (http.contains("timeout_ms")) {
        httpSettings_.timeoutMs = http["timeout_ms"].get<int>();
      }
      if (http.contains("max_retries")) {
        httpSettings_.maxRetries = http["max_retries"].get<int>();
      }
      if (http.contains("user_agent")) {
        httpSettings_.userAgent = http["user_agent"].get<std::string>();
      }
    }

    // Load client assets
    if (config.contains("assets")) {
      for (auto& [assetId, assetConfig] : config["assets"].items()) {
        AssetConfig cfg;
        cfg.url = assetConfig["url"].get<std::string>();
        cfg.overrideServer = assetConfig.value("override_server", false);
        cfg.etag = assetConfig.value("etag", "");

        clientAssets_[assetId] = cfg;

        Log(EchoVR::LogLevel::Info, "[CustomAssets] Loaded asset: %s -> %s", assetId.c_str(), cfg.url.c_str());
      }
    }

    Log(EchoVR::LogLevel::Info, "[CustomAssets] Config loaded successfully, %zu assets", clientAssets_.size());
    return true;

  } catch (const std::exception& e) {
    Log(EchoVR::LogLevel::Error, "[CustomAssets] Failed to parse config: %s", e.what());
    return false;
  }
}

bool ConfigManager::GetAssetConfig(const std::string& assetId, AssetConfig& config) const {
  // Priority 1: Client config with override_server
  auto clientIt = clientAssets_.find(assetId);
  if (clientIt != clientAssets_.end() && clientIt->second.overrideServer) {
    config = clientIt->second;
    return true;
  }

  // Priority 2: Server-provided config
  auto serverIt = serverAssets_.find(assetId);
  if (serverIt != serverAssets_.end()) {
    config = serverIt->second;
    return true;
  }

  // Priority 3: Client config without override
  if (clientIt != clientAssets_.end()) {
    config = clientIt->second;
    return true;
  }

  return false;
}

void ConfigManager::AddServerAsset(const std::string& assetId, const std::string& url, const std::string& etag) {
  AssetConfig config;
  config.url = url;
  config.overrideServer = false;
  config.etag = etag;

  serverAssets_[assetId] = config;

  Log(EchoVR::LogLevel::Info, "[CustomAssets] Added server asset: %s -> %s", assetId.c_str(), url.c_str());
}

}  // namespace CustomAssets
