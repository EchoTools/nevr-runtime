#include "cache_manager.h"

#include <shlobj.h>

#include <chrono>
#include <fstream>

#include "logging.h"
#include "third_party/json.hpp"

using json = nlohmann::json;

namespace CustomAssets {

CacheManager::CacheManager() {}

CacheManager::~CacheManager() {}

std::filesystem::path CacheManager::GetDefaultCacheDir() {
  // Get %LOCALAPPDATA% on Windows
  wchar_t* localAppDataPath = nullptr;
  if (SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppDataPath) == S_OK) {
    std::filesystem::path cachePath = localAppDataPath;
    CoTaskMemFree(localAppDataPath);

    cachePath /= "EchoVR";
    cachePath /= "CustomAssets";
    cachePath /= "cache";

    return cachePath;
  }

  // Fallback to relative path
  return "./CustomAssets/cache";
}

bool CacheManager::Initialize(const std::string& cacheDir) {
  if (cacheDir.empty()) {
    cacheDir_ = GetDefaultCacheDir();
  } else {
    cacheDir_ = cacheDir;
  }

  assetsDir_ = cacheDir_ / "assets";
  convertedDir_ = cacheDir_ / "converted";

  // Create directories if they don't exist
  try {
    std::filesystem::create_directories(assetsDir_);
    std::filesystem::create_directories(convertedDir_);

    Log(EchoVR::LogLevel::Info, "[CustomAssets] Cache initialized: %s", cacheDir_.string().c_str());
    return true;
  } catch (const std::exception& e) {
    Log(EchoVR::LogLevel::Error, "[CustomAssets] Failed to create cache directories: %s", e.what());
    return false;
  }
}

std::filesystem::path CacheManager::GetAssetPath(const std::string& assetId) {
  return assetsDir_ / (assetId + ".json");
}

std::filesystem::path CacheManager::GetMetaPath(const std::string& assetId) { return assetsDir_ / (assetId + ".meta"); }

std::filesystem::path CacheManager::GetConvertedPath(const std::string& assetId) {
  return convertedDir_ / (assetId + ".bin");
}

bool CacheManager::HasCached(const std::string& assetId) { return std::filesystem::exists(GetAssetPath(assetId)); }

CachedAsset CacheManager::LoadCached(const std::string& assetId) {
  CachedAsset cached;
  cached.assetId = assetId;
  cached.valid = false;

  auto assetPath = GetAssetPath(assetId);
  if (!std::filesystem::exists(assetPath)) {
    return cached;
  }

  try {
    // Load asset data
    std::ifstream file(assetPath, std::ios::binary);
    if (!file.is_open()) {
      return cached;
    }

    cached.data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    file.close();

    // Load metadata
    if (LoadMetadata(assetId, cached.metadata)) {
      cached.valid = true;
    }

    return cached;
  } catch (const std::exception& e) {
    Log(EchoVR::LogLevel::Error, "[CustomAssets] Failed to load cached asset %s: %s", assetId.c_str(), e.what());
    return cached;
  }
}

bool CacheManager::SaveCached(const std::string& assetId, const std::vector<uint8_t>& data,
                              const CacheMetadata& metadata) {
  try {
    // Save asset data
    auto assetPath = GetAssetPath(assetId);
    std::ofstream file(assetPath, std::ios::binary);
    if (!file.is_open()) {
      return false;
    }

    file.write(reinterpret_cast<const char*>(data.data()), data.size());
    file.close();

    // Save metadata
    return SaveMetadata(assetId, metadata);

  } catch (const std::exception& e) {
    Log(EchoVR::LogLevel::Error, "[CustomAssets] Failed to save cached asset %s: %s", assetId.c_str(), e.what());
    return false;
  }
}

bool CacheManager::LoadMetadata(const std::string& assetId, CacheMetadata& metadata) {
  auto metaPath = GetMetaPath(assetId);
  if (!std::filesystem::exists(metaPath)) {
    return false;
  }

  try {
    std::ifstream file(metaPath);
    if (!file.is_open()) {
      return false;
    }

    json meta = json::parse(file);

    metadata.assetId = meta.value("asset_id", "");
    metadata.url = meta.value("url", "");
    metadata.etag = meta.value("etag", "");
    metadata.lastModified = meta.value("last_modified", "");
    metadata.cachedAt = meta.value("cached_at", "");
    metadata.sizeBytes = meta.value("size_bytes", 0);
    metadata.sha256Hash = meta.value("hash_sha256", "");

    return true;
  } catch (const std::exception& e) {
    Log(EchoVR::LogLevel::Error, "[CustomAssets] Failed to load metadata for %s: %s", assetId.c_str(), e.what());
    return false;
  }
}

bool CacheManager::SaveMetadata(const std::string& assetId, const CacheMetadata& metadata) {
  auto metaPath = GetMetaPath(assetId);

  try {
    json meta;
    meta["asset_id"] = metadata.assetId;
    meta["url"] = metadata.url;
    meta["etag"] = metadata.etag;
    meta["last_modified"] = metadata.lastModified;
    meta["cached_at"] = metadata.cachedAt;
    meta["size_bytes"] = metadata.sizeBytes;
    meta["hash_sha256"] = metadata.sha256Hash;

    std::ofstream file(metaPath);
    if (!file.is_open()) {
      return false;
    }

    file << meta.dump(2);  // Pretty print with 2-space indent
    file.close();

    return true;
  } catch (const std::exception& e) {
    Log(EchoVR::LogLevel::Error, "[CustomAssets] Failed to save metadata for %s: %s", assetId.c_str(), e.what());
    return false;
  }
}

bool CacheManager::NeedsRefresh(const std::string& assetId) {
  // For now, always return false (cache never expires)
  // TODO: Implement proper cache expiration based on max-age
  return false;
}

void CacheManager::Clear() {
  try {
    if (std::filesystem::exists(assetsDir_)) {
      std::filesystem::remove_all(assetsDir_);
      std::filesystem::create_directories(assetsDir_);
    }
    if (std::filesystem::exists(convertedDir_)) {
      std::filesystem::remove_all(convertedDir_);
      std::filesystem::create_directories(convertedDir_);
    }

    Log(EchoVR::LogLevel::Info, "[CustomAssets] Cache cleared");
  } catch (const std::exception& e) {
    Log(EchoVR::LogLevel::Error, "[CustomAssets] Failed to clear cache: %s", e.what());
  }
}

}  // namespace CustomAssets
