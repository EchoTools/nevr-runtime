#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace CustomAssets {

enum class ResourceType { Texture, Mesh, Material, Unknown };

enum class TextureSlot { Diffuse, Normal, Specular, Emissive, Unknown };

struct TextureResource {
  ResourceType type = ResourceType::Texture;
  TextureSlot slot = TextureSlot::Diffuse;
  std::string format;  // "png", "jpeg"
  std::vector<uint8_t> data;
  std::string url;
  int width;
  int height;
};

struct MeshResource {
  ResourceType type = ResourceType::Mesh;
  std::string format;  // "obj", "gltf"
  std::vector<uint8_t> data;
  std::string url;
};

struct AssetBundle {
  std::string version;
  std::string assetId;
  std::string assetType;  // "decal", "chassis", "emote", etc.
  std::string name;
  std::string author;
  std::string description;
  std::vector<TextureResource> textures;
  std::vector<MeshResource> meshes;
  std::string etag;
  int maxAge;
  bool valid;
};

class AssetConverter {
 public:
  AssetConverter();
  ~AssetConverter();

  // Parse JSON asset bundle
  AssetBundle ParseAssetBundle(const std::string& json);

  // Convert asset bundle to game format
  // Returns pointer to game's internal asset structure
  // For now, this is a placeholder - we'll need to reverse engineer the format
  void* ConvertToGameFormat(const AssetBundle& bundle);

  // Type-specific converters (stubs for now)
  void* ConvertTexture(const TextureResource& resource);
  void* ConvertMesh(const MeshResource& resource);

 private:
  // Helper: Decode base64 data
  std::vector<uint8_t> DecodeBase64(const std::string& encoded);

  // Helper: Load PNG/JPEG into raw pixel data
  std::vector<uint8_t> LoadImageData(const std::vector<uint8_t>& imageData, int& width, int& height, int& channels);
};

}  // namespace CustomAssets
