/* The map channel's cache index, <MapsCacheDir>/index.json (docs/adr/0047; CDN_DESIGN §4.5): which version of each map
 * the cache holds, the stamp of every file the boot mount opens, and the serial and version floors. It is written last,
 * whole and atomically, so it is the commit point of a fetch: a version directory it does not name is garbage.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/cargo_env.h"
#include "core/map_manifest.h"
#include "core/map_types.h"

namespace nevr_maps {

inline constexpr const char* kCacheIndexFile = "index.json";
inline constexpr const char* kStoredManifestFile = "manifest.signed";
inline constexpr std::size_t kMaxCacheIndexBytes = std::size_t{8} << 20;

enum class FileRole { kSet, kScript, kMergedSet, kDerivedScript };

// One file the boot mount opens, relative to the version directory with '/' separators.
struct CachedFile {
  std::string path;
  FileRole role = FileRole::kSet;
  std::uint64_t script_hash = 0;  // kScript and kDerivedScript
  std::uint64_t size = 0;
  std::string sha256;
  FileStamp stamp;
  bool frames_checked = false;  // kSet and kMergedSet: every frame header was walked after the sha256 matched
  bool operator==(const CachedFile& o) const {
    return path == o.path && role == o.role && script_hash == o.script_hash && size == o.size && sha256 == o.sha256 &&
           stamp == o.stamp && frames_checked == o.frames_checked;
  }
};

struct CachedMap {
  std::string id, platform, version, index_sha256, level_name, game_build;
  std::uint64_t index_size = 0, level = 0;
  std::string dir;  // relative to the cache directory: "<id>/<platform>/<index sha12>"
  std::uint64_t pinned_serial = 0;
  bool stock_manifest_ok = false;
  std::vector<CachedFile> files;
  std::vector<StockStamp> stock_stamps;  // the stock files PC10's rebuild read
  bool operator==(const CachedMap& o) const;
};

struct CacheIndex {
  FloorBook floors;
  std::map<std::string, CachedMap> maps;  // keyed by "<id>/<platform>"
};

std::string MapKey(const std::string& id, const std::string& platform);
// The version directory's name: the first 12 hex digits of the package index sha256.
std::string VersionDirName(const std::string& index_sha256);

std::string CacheIndexToJson(const CacheIndex& index);
// False for anything that is not a well-formed nevr-maps-index v1; `out` is then left empty.
bool CacheIndexFromJson(std::string_view text, CacheIndex* out);

enum class CacheIndexStatus { kOk, kAbsent, kTooLarge, kInvalid };
const char* CacheIndexStatusName(CacheIndexStatus status) noexcept;
// kInvalid keeps the bytes beside the index as index.json.bad, for the log. Anything but kOk leaves `out` empty.
CacheIndexStatus ReadCacheIndex(nevr_cargo::Env& env, const std::string& dir, CacheIndex* out);
bool WriteCacheIndex(nevr_cargo::Env& env, const std::string& dir, const CacheIndex& index);

}  // namespace nevr_maps
