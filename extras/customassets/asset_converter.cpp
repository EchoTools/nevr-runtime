#include "asset_converter.h"

#include "logging.h"

#define STB_IMAGE_IMPLEMENTATION
#include "third_party/base64.h"
#include "third_party/json.hpp"
#include "third_party/stb_image.h"

using json = nlohmann::json;

namespace CustomAssets {

AssetConverter::AssetConverter() {}

AssetConverter::~AssetConverter() {}

AssetBundle AssetConverter::ParseAssetBundle(const std::string& jsonStr) {
  AssetBundle bundle;
  bundle.valid = false;

  try {
    json j = json::parse(jsonStr);

    bundle.version = j.value("version", "1.0");
    bundle.assetId = j.value("asset_id", "");
    bundle.assetType = j.value("asset_type", "");

    // Parse metadata
    if (j.contains("metadata")) {
      auto& meta = j["metadata"];
      bundle.name = meta.value("name", "");
      bundle.author = meta.value("author", "");
      bundle.description = meta.value("description", "");
    }

    // Parse resources
    if (j.contains("resources")) {
      for (auto& resource : j["resources"]) {
        std::string type = resource.value("type", "");

        if (type == "texture") {
          TextureResource tex;
          tex.format = resource.value("format", "png");
          tex.width = resource.value("width", 0);
          tex.height = resource.value("height", 0);
          tex.url = resource.value("url", "");

          // Parse slot
          std::string slotStr = resource.value("slot", "diffuse");
          if (slotStr == "diffuse")
            tex.slot = TextureSlot::Diffuse;
          else if (slotStr == "normal")
            tex.slot = TextureSlot::Normal;
          else if (slotStr == "specular")
            tex.slot = TextureSlot::Specular;
          else if (slotStr == "emissive")
            tex.slot = TextureSlot::Emissive;
          else
            tex.slot = TextureSlot::Unknown;

          // Decode base64 data if present
          if (resource.contains("data") && !resource["data"].is_null()) {
            std::string base64Data = resource["data"].get<std::string>();
            tex.data = DecodeBase64(base64Data);
          }

          bundle.textures.push_back(tex);

        } else if (type == "mesh") {
          MeshResource mesh;
          mesh.format = resource.value("format", "obj");
          mesh.url = resource.value("url", "");

          // Decode base64 data if present
          if (resource.contains("data") && !resource["data"].is_null()) {
            std::string base64Data = resource["data"].get<std::string>();
            mesh.data = DecodeBase64(base64Data);
          }

          bundle.meshes.push_back(mesh);
        }
      }
    }

    // Parse cache control
    if (j.contains("cache_control")) {
      auto& cache = j["cache_control"];
      bundle.etag = cache.value("etag", "");
      bundle.maxAge = cache.value("max_age", 86400);
    }

    bundle.valid = true;

    Log(EchoVR::LogLevel::Info, "[CustomAssets] Parsed asset bundle: %s (%zu textures, %zu meshes)",
        bundle.assetId.c_str(), bundle.textures.size(), bundle.meshes.size());

  } catch (const std::exception& e) {
    Log(EchoVR::LogLevel::Error, "[CustomAssets] Failed to parse asset bundle: %s", e.what());
  }

  return bundle;
}

void* AssetConverter::ConvertToGameFormat(const AssetBundle& bundle) {
  // TODO: Reverse engineer the game's internal asset format
  // For now, return nullptr (will fallback to default asset)

  Log(EchoVR::LogLevel::Warning, "[CustomAssets] Asset conversion not yet implemented for: %s", bundle.assetId.c_str());

  return nullptr;
}

void* AssetConverter::ConvertTexture(const TextureResource& resource) {
  // TODO: Convert texture to game format
  return nullptr;
}

void* AssetConverter::ConvertMesh(const MeshResource& resource) {
  // TODO: Convert mesh to game format
  return nullptr;
}

std::vector<uint8_t> AssetConverter::DecodeBase64(const std::string& encoded) { return base64::Decode(encoded); }

std::vector<uint8_t> AssetConverter::LoadImageData(const std::vector<uint8_t>& imageData, int& width, int& height,
                                                   int& channels) {
  int w, h, c;
  unsigned char* pixels = stbi_load_from_memory(imageData.data(), static_cast<int>(imageData.size()), &w, &h, &c, 0);

  if (!pixels) {
    Log(EchoVR::LogLevel::Error, "[CustomAssets] Failed to decode image: %s", stbi_failure_reason());
    return {};
  }

  width = w;
  height = h;
  channels = c;

  size_t dataSize = w * h * c;
  std::vector<uint8_t> result(pixels, pixels + dataSize);

  stbi_image_free(pixels);

  return result;
}

}  // namespace CustomAssets
