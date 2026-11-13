/* The map channel's cache index, <MapsCacheDir>/index.json (docs/adr/0048-custom-maps-channel.md decisions 3 and 4): which version
 * of each map the cache holds, the stamp of every file the mount opens, the serial and version floors per channel, guild and key,
 * and the lobby groups whose maps it keeps, the latest first. It is written last, whole and atomically, so it is the commit point of a fetch: a version directory it does not name is
 * garbage. Its schema, file names and the version directory's name length are map_format.h's.
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
#include "core/map_format.h"
#include "core/map_manifest.h"
#include "core/map_types.h"

namespace nevr_maps {

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
  std::vector<StockStamp> stock_stamps;  // the stock files the rebuild read
  bool operator==(const CachedMap& o) const;
};

// A lobby group whose maps the cache keeps (ADR 0048: a client and a server both take their group from the lobby session). The
// id is opaque (IsPathSafeGroupId). `maps` are the "<id>/<platform>" keys its last verified manifest listed for this platform, and
// `bytes` what keeping the group costs the disk: its stored manifest and the bytes those maps list.
struct CachedGroup {
  std::string id;
  std::uint64_t bytes = 0;
  std::vector<std::string> maps;
  bool operator==(const CachedGroup& o) const { return id == o.id && bytes == o.bytes && maps == o.maps; }
};

struct CacheIndex {
  FloorBook floors;
  std::map<std::string, CachedMap> maps;  // keyed by "<id>/<platform>"
  std::vector<CachedGroup> groups;        // the groups fetched, the latest first
};

std::string MapKey(const std::string& id, const std::string& platform);
// The version directory's name: the first kVersionDirChars hex digits of the package index sha256.
std::string VersionDirName(const std::string& index_sha256);

std::string CacheIndexToJson(const CacheIndex& index);
// False for anything that is not a well-formed nevr-maps-index v1; `out` is then left empty.
bool CacheIndexFromJson(std::string_view text, CacheIndex* out);

enum class CacheIndexStatus { kOk, kAbsent, kTooLarge, kInvalid, kNoMemoryFact };
const char* CacheIndexStatusName(CacheIndexStatus status) noexcept;
// The index is read whole, so it may be at most MapDocumentBytes of the device's facts (Env::QueryDevice on `dir`; ADR 0045
// decision 8): a device that gives no memory fact reads none (kNoMemoryFact), and a larger file is kTooLarge.
// kInvalid keeps the bytes beside the index as index.json.bad, for the log. Anything but kOk leaves `out` empty.
CacheIndexStatus ReadCacheIndex(nevr_cargo::Env& env, const std::string& dir, CacheIndex* out);
bool WriteCacheIndex(nevr_cargo::Env& env, const std::string& dir, const CacheIndex& index);

}  // namespace nevr_maps
