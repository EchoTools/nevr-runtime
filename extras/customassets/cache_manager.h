#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace CustomAssets {

struct CacheMetadata {
  std::string assetId;
  std::string url;
  std::string etag;
  std::string lastModified;
  std::string cachedAt;
  size_t sizeBytes;
  std::string sha256Hash;
};

struct CachedAsset {
  std::string assetId;
  std::vector<uint8_t> data;
  CacheMetadata metadata;
  bool valid;
};

class CacheManager {
 public:
  CacheManager();
  ~CacheManager();

  // Initialize with cache directory (empty = use default)
  bool Initialize(const std::string& cacheDir = "");

  // Check if asset is cached
  bool HasCached(const std::string& assetId);

  // Load cached asset
  CachedAsset LoadCached(const std::string& assetId);

  // Save asset to cache
  bool SaveCached(const std::string& assetId, const std::vector<uint8_t>& data, const CacheMetadata& metadata);

  // Check if cached asset needs refresh
  bool NeedsRefresh(const std::string& assetId);

  // Clear all cached assets
  void Clear();

  // Get cache directory path
  std::filesystem::path GetCacheDir() const { return cacheDir_; }

  // Get default cache directory
  static std::filesystem::path GetDefaultCacheDir();

 private:
  std::filesystem::path cacheDir_;
  std::filesystem::path assetsDir_;
  std::filesystem::path convertedDir_;

  std::filesystem::path GetAssetPath(const std::string& assetId);
  std::filesystem::path GetMetaPath(const std::string& assetId);
  std::filesystem::path GetConvertedPath(const std::string& assetId);

  bool LoadMetadata(const std::string& assetId, CacheMetadata& metadata);
  bool SaveMetadata(const std::string& assetId, const CacheMetadata& metadata);
};

}  // namespace CustomAssets
