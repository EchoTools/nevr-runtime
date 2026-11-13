/* The custom-map channel's shared types (docs/adr/0047-custom-maps-channel.md): a guild's signed manifest, a map's package index,
 * and the file-system seam the channel needs beside nevr_cargo::Env. Every part of the channel compiles against this header.
 *
 * Hashes are sha256 as 64 lowercase hex; symbols (level, script hash, record key) are rad_hash values. The numbers the formats
 * define are map_format.h's.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/cargo_manifest.h"
#include "core/map_format.h"

namespace nevr_maps {

struct Pin {
  std::string sha256;
  std::uint64_t size = 0;
};

enum class Role { kSet, kScript, kRecipe };

struct Member {
  std::string path;
  Role role = Role::kSet;
  std::uint64_t size = 0;
  std::string sha256;
  std::uint64_t script_hash = 0;  // kScript only
  std::vector<Pin> pieces;        // concatenated in order they are the member
};

// Recipes (ADR 0047, the recipe format in map_format.h): a stock-derived asset ships as edits against the player's own stock
// bytes. An edit carries no stock value: that would ship stock bytes. The stock sha256 pins what it applies to.
struct FieldEdit {
  std::uint64_t at = 0;
  std::uint8_t width = 0;
  std::uint64_t to = 0;  // little-endian
};

struct DiffOp {
  bool insert = false;  // copy: a stock range; insert: a recipe-member range
  std::uint64_t offset = 0, size = 0;
};

enum class EditKind { kFields, kDiff };

struct Recipe {
  std::uint64_t type = 0, name = 0, stock_type = 0, stock_name = 0, stock_size = 0, result_size = 0;
  std::string stock_sha256, result_sha256;
  EditKind kind = EditKind::kFields;
  std::vector<FieldEdit> fields;
  std::vector<DiffOp> diff;
};

struct RekeyPin {
  std::uint64_t hash = 0;
  std::uint64_t stock_source = 0;
  std::string stock_sha256;
  std::vector<FieldEdit> edits;  // empty: the stock DLL under a new hash
  std::string result_sha256;
  std::uint64_t result_size = 0;
};

struct ConfigAlias {
  std::uint64_t key = 0;
  std::uint64_t alias = 0;
  std::string key_name, alias_name;
};

struct Package {
  std::string id, version, platform, level_name, game_build, stock_data_version, source_sha256;
  std::uint64_t level = 0, parent = 0;
  std::vector<std::string> requires_stock_manifest_sha256;
  std::vector<Member> members;
  std::vector<RekeyPin> rekeyed;
  std::vector<ConfigAlias> config_aliases;
  std::vector<Recipe> derived;
  std::vector<std::uint64_t> standins;  // informational: map-owned names the publisher stood in on Quest
};

struct PlatformEntry {
  std::string version;
  Pin index;
  std::uint64_t level = 0;
  std::string level_name, game_build;
  std::uint64_t bytes = 0;
  std::uint32_t scripts = 0;
};

struct MapEntry {
  std::string title, author;
  Pin thumbnail;
  std::map<std::string, PlatformEntry> platforms;  // keyed by kPlatformPc / kPlatformQuest
};

struct Manifest {
  std::uint64_t serial = 0;
  std::string expires;  // UTC, YYYY-MM-DDTHH:MM:SSZ
  std::vector<std::string> revoked_ids, revoked_sha256;
  std::map<std::string, MapEntry> maps;  // keyed by map id
};

// Identifies one version of one file: equal stamps mean the bytes verified at fetch are the bytes on disk (ADR 0047 decision 4).
struct FileStamp {
  std::uint64_t volume = 0, id_hi = 0, id_lo = 0, size = 0, last_write = 0;

  // Spelled out: the tree builds as C++17, which has no defaulted comparisons.
  bool operator==(const FileStamp& other) const {
    return volume == other.volume && id_hi == other.id_hi && id_lo == other.id_lo && size == other.size &&
           last_write == other.last_write;
  }
  bool operator!=(const FileStamp& other) const { return !(*this == other); }
};

// The file operations the channel needs that nevr_cargo::Env does not have: a held, share-read handle and its stamp (ADR 0047
// decision 4), and whole-directory moves. The channel reaches files through two seams: Env (src/core/cargo_env.h) for what the
// texture cargo also does, read small files, write one atomically, append, sync, query the device's facts (ADR 0045 decision 8)
// and fetch over HTTP, and MapFs for what only a map needs. A platform implements both.
class MapFs {
 public:
  virtual ~MapFs() = default;
  virtual bool StampOf(std::int64_t handle, FileStamp* out) = 0;  // through the held handle
  virtual std::string FinalPathOf(std::int64_t handle) = 0;       // UTF-8; empty on failure
  // Share-read only: while the handle is held nothing can write the file or delete it.
  virtual bool OpenHeld(const std::string& path, std::int64_t* handle, std::uint64_t* size) = 0;
  virtual bool ListDir(const std::string& dir, std::vector<std::string>* names) = 0;
  virtual bool RemoveTree(const std::string& dir) = 0;  // true when gone or never there; removes links, never follows them
  virtual bool RenameDir(const std::string& from, const std::string& to) = 0;  // refuses when `to` exists
};

// The platform names are the texture cargo's (ADR 0045, src/core/cargo_manifest.h).
inline constexpr const char* kPlatformPc = nevr_cargo::kWindowsX64.name;
inline constexpr const char* kPlatformQuest = nevr_cargo::kAndroidArm64.name;

struct StockStamp {
  std::string path;
  FileStamp stamp;
};

struct DeriveOutput {
  std::string reason;                                          // "" when every recipe rebuilt and verified
  std::string set_path, set_sha256;                            // the merged set; empty when the package has no derived[]
  std::vector<std::pair<std::uint64_t, std::string>> scripts;  // rebuilt rekeyed scripts: hash, path
  std::vector<StockStamp> stock_stamps;
};

// Rebuilds a package's derived[] and rekeyed[] from the player's own stock files. The map channel's fetch calls it; a null
// Deriver means "cannot rebuild here" (derived_unsupported).
class Deriver {
 public:
  virtual ~Deriver() = default;
  virtual DeriveOutput Derive(const Package& package, const std::string& version_dir,
                              const std::function<bool()>& keep_going) = 0;  // background thread
  virtual bool StockCurrent(const std::vector<StockStamp>& stamps) = 0;      // open, stamp, close; no read
};

}  // namespace nevr_maps
