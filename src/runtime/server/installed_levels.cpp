#include "runtime/server/installed_levels.h"

#include <zstd.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <system_error>
#include <utility>

#include "abi/symbol_hash.h"

namespace nevr_game_server {
namespace {

// docs/reference/package-manifest-format.md: a 24-byte container, then one zstd frame.
constexpr std::size_t kContainerBytes = 24;
constexpr uint32_t kContainerVersion = 0x10;
// The body's fixed fields and the resource array (0x20 bytes per record, count at 0x38, array at 0xc0).
constexpr std::size_t kCountOffset = 0x38;
constexpr std::size_t kResourcesOffset = 0xc0;
constexpr std::size_t kResourceBytes = 0x20;
// A real manifest is 5 MB decompressed and 2 MB on disk; the caps only stop a corrupt length from allocating
// or reading without bound.
constexpr uint64_t kMaxDecompressedBytes = 256ULL * 1024 * 1024;
constexpr uintmax_t kMaxManifestFileBytes = 64ULL * 1024 * 1024;

// The engine's own layout under the install root (asset-system.md, "Where assets live").
constexpr const char* kDataDirName = "_data";
constexpr const char* kProjectDirName = "rad15";
constexpr const char* kPlatformDirName = "win10";
constexpr const char* kManifestsDirName = "manifests";

uint64_t ReadU64(const std::vector<uint8_t>& bytes, std::size_t at) {
  uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) value |= static_cast<uint64_t>(bytes[at + i]) << (8 * i);
  return value;
}

uint32_t ReadU32(const std::vector<uint8_t>& bytes, std::size_t at) {
  uint32_t value = 0;
  for (std::size_t i = 0; i < 4; ++i) value |= static_cast<uint32_t>(bytes[at + i]) << (8 * i);
  return value;
}

std::string SetFileName(uint64_t setId) {
  std::ostringstream hex;
  hex << std::hex << std::setw(16) << std::setfill('0') << setId;
  return hex.str();
}

bool IsRegionChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

// nakama: regionPattern ^[-A-Za-z0-9_]+$ and guildPattern ^([0-9]+|any)$ (server/session_ws.go).
bool IsServerDbItem(std::string_view item, ListItemKind kind) {
  if (kind == ListItemKind::kRegion) {
    if (item.empty()) return false;
    return std::all_of(item.begin(), item.end(), IsRegionChar);
  }
  if (item == "any") return true;
  if (item.empty()) return false;
  return std::all_of(item.begin(), item.end(), [](char c) { return c >= '0' && c <= '9'; });
}

}  // namespace

std::vector<uint64_t> BootPackageSets() {
  return {EchoVR::CSymbol64Hash("r14netclient"), EchoVR::CSymbol64Hash("r14netclient_en_US")};
}

std::filesystem::path InstallRootFromExe(const std::filesystem::path& exePath) {
  // The exe's directory is bin/win10; the project root is the parent of bin.
  return exePath.parent_path().parent_path().parent_path();
}

std::vector<std::filesystem::path> FindManifestDirs(const std::filesystem::path& exePath) {
  std::vector<std::filesystem::path> dirs;
  std::error_code ec;
  const std::filesystem::path dataDir = InstallRootFromExe(exePath) / kDataDirName;
  std::filesystem::directory_iterator it(dataDir, ec);
  if (ec) return dirs;
  for (const std::filesystem::directory_entry& entry : it) {
    std::error_code entryEc;
    if (!entry.is_directory(entryEc) || entryEc) continue;
    const std::filesystem::path candidate =
        entry.path() / kProjectDirName / kPlatformDirName / kManifestsDirName;
    if (std::filesystem::is_directory(candidate, entryEc) && !entryEc) dirs.push_back(candidate);
  }
  std::sort(dirs.begin(), dirs.end());
  return dirs;
}

bool ParseLevelRoots(const std::vector<uint8_t>& file, std::vector<uint64_t>* levels, std::string* error) {
  levels->clear();
  if (file.size() < kContainerBytes || std::memcmp(file.data(), "ZSTD", 4) != 0) {
    *error = "not a ZSTD manifest";
    return false;
  }
  if (ReadU32(file, 4) != kContainerVersion) {
    *error = "unsupported container version";
    return false;
  }
  const uint64_t decompressedBytes = ReadU64(file, 8);
  const uint64_t compressedBytes = ReadU64(file, 16);
  if (compressedBytes > file.size() - kContainerBytes) {
    *error = "compressed length runs past the end of the file";
    return false;
  }
  if (decompressedBytes < kResourcesOffset || decompressedBytes > kMaxDecompressedBytes) {
    *error = "decompressed length out of range";
    return false;
  }
  // Cross-check the frame's own declared size before anything is allocated. An unknown size (a frame
  // written without one) is accepted and bounded by the cap above; a size that disagrees is refused.
  const unsigned long long frameSize =
      ZSTD_getFrameContentSize(file.data() + kContainerBytes, static_cast<std::size_t>(compressedBytes));
  if (frameSize == ZSTD_CONTENTSIZE_ERROR) {
    *error = "zstd frame header is not valid";
    return false;
  }
  if (frameSize != ZSTD_CONTENTSIZE_UNKNOWN && frameSize != decompressedBytes) {
    *error = "frame declares a different size than the container";
    return false;
  }

  std::vector<uint8_t> body(static_cast<std::size_t>(decompressedBytes));
  const std::size_t written = ZSTD_decompress(body.data(), body.size(), file.data() + kContainerBytes,
                                              static_cast<std::size_t>(compressedBytes));
  if (ZSTD_isError(written) != 0 || written != body.size()) {
    *error = "zstd frame does not decode to the declared length";
    return false;
  }

  const uint64_t resourceCount = ReadU64(body, kCountOffset);
  const uint64_t room = (body.size() - kResourcesOffset) / kResourceBytes;
  if (resourceCount > room) {
    *error = "resource count runs past the decompressed body";
    return false;
  }
  for (uint64_t i = 0; i < resourceCount; ++i) {
    const std::size_t record = kResourcesOffset + static_cast<std::size_t>(i) * kResourceBytes;
    if (ReadU64(body, record) == kLevelRootType) {
      levels->push_back(ReadU64(body, record + 8));
    }
  }
  return true;
}

InstalledLevels CollectInstalledLevels(const std::filesystem::path& manifestDir, const std::vector<uint64_t>& setIds) {
  InstalledLevels result;
  for (const uint64_t setId : setIds) {
    const std::filesystem::path file = manifestDir / SetFileName(setId);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec) || ec) {
      result.problems.push_back(SetFileName(setId) + ": not found");
      continue;
    }
    const uintmax_t size = std::filesystem::file_size(file, ec);
    if (ec) {
      result.problems.push_back(SetFileName(setId) + ": size unreadable");
      continue;
    }
    if (size > kMaxManifestFileBytes) {
      result.problems.push_back(SetFileName(setId) + ": larger than the manifest size cap");
      continue;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
      result.problems.push_back(SetFileName(setId) + ": not readable");
      continue;
    }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<uint64_t> levels;
    std::string error;
    if (!ParseLevelRoots(bytes, &levels, &error)) {
      result.problems.push_back(SetFileName(setId) + ": " + error);
      continue;
    }
    ++result.manifestsRead;
    result.levels.insert(result.levels.end(), levels.begin(), levels.end());
  }
  std::sort(result.levels.begin(), result.levels.end());
  result.levels.erase(std::unique(result.levels.begin(), result.levels.end()), result.levels.end());
  return result;
}

InstalledLevels CollectInstalledLevelsFromDirs(const std::vector<std::filesystem::path>& manifestDirs,
                                               const std::vector<uint64_t>& setIds) {
  InstalledLevels merged;
  for (const std::filesystem::path& dir : manifestDirs) {
    InstalledLevels one = CollectInstalledLevels(dir, setIds);
    merged.levels.insert(merged.levels.end(), one.levels.begin(), one.levels.end());
    merged.manifestsRead += one.manifestsRead;
    for (const std::string& problem : one.problems) merged.problems.push_back(dir.string() + " " + problem);
  }
  std::sort(merged.levels.begin(), merged.levels.end());
  merged.levels.erase(std::unique(merged.levels.begin(), merged.levels.end()), merged.levels.end());
  return merged;
}

void AddRegisteredLevels(const std::vector<std::pair<uint64_t, uint64_t>>& registered, InstalledLevels* levels) {
  for (const auto& [type, name] : registered) {
    if (type != kLevelRootType) continue;
    levels->levels.push_back(name);
    ++levels->pluginLevels;
  }
  std::sort(levels->levels.begin(), levels->levels.end());
  levels->levels.erase(std::unique(levels->levels.begin(), levels->levels.end()), levels->levels.end());
}

std::vector<std::string> ParseServerDbList(std::string_view csv, ListItemKind kind, std::size_t* rejected) {
  std::vector<std::string> items;
  *rejected = 0;
  std::size_t start = 0;
  while (start <= csv.size()) {
    const std::size_t comma = csv.find(',', start);
    const std::size_t end = comma == std::string_view::npos ? csv.size() : comma;
    if (end > start) {
      std::string item(csv.substr(start, std::min(end - start, kServerDbListMaxItemBytes)));
      if (IsServerDbItem(item, kind)) {
        items.push_back(std::move(item));
      } else {
        ++*rejected;
      }
    }
    if (comma == std::string_view::npos) break;
    start = comma + 1;
  }
  std::sort(items.begin(), items.end());
  return items;
}

std::vector<std::string> SplitCsvList(std::string_view csv) {
  std::vector<std::string> items;
  std::size_t start = 0;
  while (start <= csv.size()) {
    const std::size_t comma = csv.find(',', start);
    const std::size_t end = comma == std::string_view::npos ? csv.size() : comma;
    if (end > start) items.emplace_back(csv.substr(start, end - start));
    if (comma == std::string_view::npos) break;
    start = comma + 1;
  }
  return items;
}

}  // namespace nevr_game_server
