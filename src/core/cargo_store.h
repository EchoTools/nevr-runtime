/* The cargo cache (docs/adr/0045-cdn-texture-delivery.md D3): blobs named by their SHA-256, an index naming the ones to serve, and
 * the mount that turns the index into a shadow manifest.
 *
 *   <dir>/<sha256>.blob   one stored zstd frame (a stock-format chunk)
 *   <dir>/index.json      {"version":2,"entries":[{"type","name","sha256","label","format","bytes","align"}]}
 *
 * Order is the whole safety argument: a blob is renamed into place only after its size and SHA-256 matched, and the index is written
 * last, whole, by temp and rename, naming only blobs that are in place. A kill between the two leaves the old index, which names
 * only blobs that are still there. A flipped byte in a blob is caught at mount, which hashes each blob again through the handle it
 * keeps open for the life of the process.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/cargo_env.h"
#include "core/cargo_plan.h"
#include "core/resource_shadow.h"

namespace nevr_cargo {

inline constexpr const char* kIndexFile = "index.json";
inline constexpr int kIndexVersion = 2;
inline constexpr std::size_t kMaxIndexBytes = std::size_t{1} << 20;

std::string BlobPath(const std::string& dir, const std::string& sha256);
// "0x" and the value in lowercase hex without leading zeros (the form the manifest and the log use for a type or a name).
std::string FormatHex64(std::uint64_t value);
std::string IndexPath(const std::string& dir);

struct IndexEntry {
  std::uint64_t type = 0;
  std::uint64_t name = 0;
  std::string sha256;
  std::string label;
  std::string format;  // "zstd-frame"
  std::uint64_t bytes = 0;
  std::uint32_t align = 0;
  bool operator==(const IndexEntry& o) const {
    return type == o.type && name == o.name && sha256 == o.sha256 && label == o.label && format == o.format && bytes == o.bytes &&
           align == o.align;
  }
};

struct Index {
  std::vector<IndexEntry> entries;  // sorted by (label, type, name) by Normalize
  bool operator==(const Index& o) const { return entries == o.entries; }
};

void Normalize(Index* index);

enum class IndexStatus { kOk, kAbsent, kUnreadable, kInvalid };
const char* IndexStatusName(IndexStatus status) noexcept;

IndexStatus ReadIndex(Env& env, const std::string& dir, Index* out);
bool WriteIndex(Env& env, const std::string& dir, const Index& index);

// Reads a blob whole through `env` and returns its SHA-256, or an empty string when it cannot be read. For the "already held" check.
std::string HashFile(Env& env, const std::string& path);

// What a mount holds: the shadow to publish and the label names the shadow's record tags index.
struct Mounted {
  std::unique_ptr<nevr_shadow::Shadow> shadow;
  std::vector<std::string> labels;  // Record::user indexes this
  std::size_t names = 0;            // resource names mounted (groups of entries)
  std::size_t records = 0;          // entries mounted
  std::size_t refused = 0;          // names left out because an entry did not open or did not hash to its name
};

// Opens every blob of every entry whose label the policy serves, holds each open, hashes it again through the held handle, and
// builds a shadow from the entries that verified. A label's entries for one name mount together or not at all. Returns a Mounted
// with a null shadow when nothing mounted.
Mounted Mount(Env& env, const std::string& dir, const Index& index, const Policy& policy);

}  // namespace nevr_cargo
