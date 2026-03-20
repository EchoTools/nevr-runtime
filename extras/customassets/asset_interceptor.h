#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace CustomAssets {

// Function prototype for Loadout_ResolveDataFromId
typedef void* (*Loadout_ResolveDataFromId_t)(void* context, uint64_t cosmetic_id);

class AssetInterceptor {
 public:
  AssetInterceptor();
  ~AssetInterceptor();

  // Initialize and install hooks
  bool Initialize();

  // Shutdown and remove hooks
  void Shutdown();

  // Register a custom asset hash
  void RegisterAssetHash(uint64_t hash, const std::string& assetId);

  // Check if a hash is for a custom asset
  bool IsCustomAsset(uint64_t hash);

  // Get asset ID from hash
  std::string GetAssetId(uint64_t hash);

  // Get the original function pointer (for calling original)
  Loadout_ResolveDataFromId_t GetOriginalFunction() const { return originalFunction_; }

 private:
  Loadout_ResolveDataFromId_t originalFunction_;
  std::unordered_map<uint64_t, std::string> customAssetHashes_;
  bool initialized_;

  // The hook function (defined in .cpp)
  friend void* Hook_Loadout_ResolveDataFromId(void* context, uint64_t cosmetic_id);
};

}  // namespace CustomAssets
