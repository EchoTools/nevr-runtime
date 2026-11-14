/* The stock source's shared half (docs/adr/0048-custom-maps-channel.md, the recipes; core/map_derive.h StockSource): the player's own
 * stock bytes, read for the recipe rebuild. The game's package manifests (docs/reference/package-manifest-format.md) are parsed here,
 * once per file stamp, and a resource is decompressed here out of its chunk in a package file. Both platforms run this code; each
 * platform half (src/runtime/cargo/stock_source.h on the PC, src/quest/cargo/stock_source.h on the Quest) says only which manifests
 * the install has, how a stock file is opened and stamped, and where a stock script is.
 *
 * Every stock file is opened read-only and closed after the read: nothing is held and nothing is written. A manifest consulted is
 * recorded with its stamp, as is every package file read, so a key added to an earlier manifest, or a package rewritten, makes the
 * rebuild stale (StampsCurrent). StampsCurrent only opens and stamps.
 *
 * Memory (docs/adr/0045-cdn-texture-delivery.md decision 8): a manifest's file and its decompressed body, a chunk, and a whole stock
 * script are each held at once, so each is checked against the one-object share of the device's memory (the share DeriveMemoryOf
 * gives a rebuild) before it is allocated. The recipe format's own cap (map_format.h kRecipeResultMax) still applies to a chunk
 * and to a script.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/map_derive.h"
#include "core/map_types.h"

namespace nevr_maps {

// One stock file, open for reading with full sharing: a reader here never stops the game, the launcher or an update from opening,
// writing or replacing it.
class StockFile {
 public:
  virtual ~StockFile() = default;
  virtual bool Stamp(FileStamp* out) = 0;
  // Exactly `length` bytes at `offset`, or false.
  virtual bool Read(std::uint64_t offset, void* buffer, std::size_t length) = 0;
};

// A package manifest of the install, and the prefix its package files are named after: the package of chunk n's package index p is
// "<package_prefix>_<p>" (docs/reference/package-manifest-format.md, "Where it lives").
struct ManifestPlace {
  std::string manifest_path;
  std::string package_prefix;
};

class PackageStockSource : public StockSource {
 public:
  // The memory a process can still use (core/device_memory.h MemoryAvailable); a test passes its own.
  using MemoryFn = bool (*)(std::uint64_t* bytes) noexcept;
  explicit PackageStockSource(MemoryFn memory_available);

  // The first manifest of Manifests() that holds (type, name), read from its package. False when none holds it, or a file it needs
  // does not open, does not parse, or is over the memory share.
  bool ReadResource(std::uint64_t type, std::uint64_t name, std::vector<std::uint8_t>* out,
                    std::vector<StockStamp>* read_from) final;
  bool StampsCurrent(const std::vector<StockStamp>& stamps) final;

 protected:
  // The manifests to consult, in order: the first that holds a key wins.
  virtual std::vector<ManifestPlace> Manifests() = 0;
  // `path` opened for reading with full sharing, or null when it is absent or not a plain file.
  virtual std::unique_ptr<StockFile> Open(const std::string& path) = 0;
  // A whole stock file (a script image), bounded as a chunk is; recorded in `read_from`.
  bool ReadWholeFile(const std::string& path, std::vector<std::uint8_t>* out, std::vector<StockStamp>* read_from);
  // The bytes one stock read may hold at once: the one-object share of the device's memory, 0 when the device gives no figure.
  std::uint64_t ObjectBytes() const;

 private:
  struct Resource {
    std::uint64_t type = 0, name = 0;
    std::uint32_t chunk = 0, offset = 0, size = 0;
  };
  struct Chunk {
    std::uint32_t package = 0, offset = 0, compressed = 0, decompressed = 0;
  };
  struct Manifest {
    FileStamp stamp;
    bool ok = false;
    std::uint32_t packages = 0;
    std::vector<Resource> resources;  // sorted by (type, name)
    std::vector<Chunk> chunks;
  };

  const Manifest* LoadManifest(const std::string& path, std::vector<StockStamp>* read_from);
  bool ParseManifest(const std::vector<std::uint8_t>& file, std::uint64_t memory, Manifest* out);
  bool ReadFromPackage(const std::string& package_prefix, const Manifest& manifest, const Resource& resource,
                       std::vector<std::uint8_t>* out, std::vector<StockStamp>* read_from);
  std::unique_ptr<StockFile> OpenStamped(const std::string& path, FileStamp* stamp, std::vector<StockStamp>* read_from);

  MemoryFn memory_available_;
  std::mutex mutex_;                           // the parsed manifests
  std::map<std::string, Manifest> manifests_;  // by path, kept while the file's stamp is unchanged
};

}  // namespace nevr_maps
