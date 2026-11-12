/* The cargo store (docs/adr/0045-cdn-texture-delivery.md D3): objects appended to a few big files, an index from SHA-256 to where
 * each one lies, and the mount that turns the index and the manifest into a shadow the engine reads.
 *
 *   <dir>/store-<n>.pack   stored zstd frames back to back, at most Limits::store_file_bytes (never above 4 GiB: the engine's chunk
 *                          offset is 32 bits). One open file serves every object in it.
 *   <dir>/blobs.idx        the blob index: a header, then one fixed 52-byte record per object, appended after the object's bytes
 *                          are durable:  sha256[32] | file u32 | offset u32 | size u32 | bytes u32 | crc32 u32  (little endian,
 *                          the crc covers the 48 bytes before it)
 *   <dir>/manifest.json.zst the last manifest a fetch read, as published (one zstd frame); the mount reads it to know which (type, name)
 *                          each object is
 *
 * Hashing happens once, while an object is downloaded (cargo_run.cpp), and never again at boot: Open() reads the index and
 * measures each store file's length, nothing more. The order is the whole safety argument: an object's bytes are synced, then its
 * index record is appended and synced. A kill between the two leaves bytes no record names; the next Begin() cuts them off before it
 * appends (the store file is truncated to the end of the last object the index names in it); Open() never writes a store file. A
 * kill inside a record leaves a short or crc-bad tail of the index, and Open() cuts that off: its one write, to blobs.idx, and only
 * when it finds a tail or a wrong header. Otherwise Open() only reads. A record that names bytes past the end of its store file
 * (the file was cut short), that names a store file that cannot be opened or is numbered at or above Limits::max_store_files, or whose
 * size is 0 is dropped and counted by Open().
 *
 * Zero-byte records. The game's own manifests hold records of size 0 (docs/reference/plugin-resources.md). The engine gives them no
 * special case: FillResourceFileRequest (echovr.exe 0x140fa1fe0) copies the record's chunk, offset and size into the request like
 * any other, and the streaming reader (0x1415249b0 op 1, then 0x1415255a0 for a size of at most 0x80000) reads the chunk the record
 * names and copies `size` bytes out of it. So a zero-byte record needs a chunk that exists. The mount points every empty record
 * at the first chunk of its own shadow (offset 0, size 0) and says so in its log; with no object in the shadow there is no chunk to
 * name and the empty records are refused as `no_chunk`. That the headset's reader takes this is not established (cargo REPORT).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/cargo_env.h"
#include "core/cargo_limits.h"
#include "core/cargo_manifest.h"
#include "core/cargo_plan.h"
#include "core/resource_shadow.h"

namespace nevr_cargo {

// The index header is "NEVRCARG" | version u32 | record bytes u32 (kIndexHeaderBytes, kIndexRecordBytes: cargo_limits.h).
inline constexpr std::uint32_t kIndexVersion = 3;

std::string StoreFilePath(const std::string& dir, std::uint32_t file);
std::string IndexPath(const std::string& dir);
std::string ManifestCopyPath(const std::string& dir);
// "0x" and the value in lowercase hex without leading zeros (the form the manifest and the log use for a type or a name).
std::string FormatHex64(std::uint64_t value);
std::uint32_t Crc32(const void* data, std::size_t length) noexcept;

struct BlobLoc {
  std::uint32_t file = 0;
  std::uint32_t offset = 0;
  std::uint32_t size = 0;   // stored frame bytes
  std::uint32_t bytes = 0;  // decompressed bytes
};

struct StoreOpenInfo {
  const char* index = "absent";  // "absent", "ok", "invalid" (header wrong: the index was reset), "tail_cut" (a torn record was cut off)
  std::size_t blobs = 0;
  std::size_t files = 0;
  std::size_t dropped = 0;       // records past their store file's end, or naming a file that is not there
  std::uint64_t bytes = 0;       // stored bytes the index names
};

class Store {
 public:
  // Reads the index and measures the store files. No object is read, no hash is made. Null only when `limits` is not ok.
  static std::unique_ptr<Store> Open(Env& env, const std::string& dir, const Limits& limits, StoreOpenInfo* info);

  bool Has(const std::string& sha256) const { return blobs_.find(sha256) != blobs_.end(); }
  // Where the object lies and its two sizes (the stored frame's `size` and the decompressed `bytes`), or null. A caller that holds the
  // size the manifest states compares it with these (HoldsSized): the name is the content's hash, so a different size under one name
  // means the index or the manifest is wrong, and the object is not served.
  bool HoldsSized(const std::string& sha256, std::uint64_t size, std::uint64_t bytes) const {
    const BlobLoc* loc = Find(sha256);
    return loc != nullptr && loc->size == size && loc->bytes == bytes;
  }
  const BlobLoc* Find(const std::string& sha256) const {
    const auto it = blobs_.find(sha256);
    return it == blobs_.end() ? nullptr : &it->second;
  }
  std::size_t blob_count() const { return blobs_.size(); }
  std::size_t file_count() const { return files_.size(); }
  std::uint64_t held_bytes() const { return held_bytes_; }
  const std::string& dir() const { return dir_; }

  // One object being appended. Write streams its bytes; Commit makes it durable and names it in the index; Abort (or destruction
  // without Commit) cuts the bytes off again. Exactly one Writer is live at a time.
  class Writer {
   public:
    virtual ~Writer() = default;
    virtual bool Write(const void* data, std::size_t length) = 0;
    virtual bool Commit(const std::string& sha256, std::uint64_t bytes) = 0;
    virtual void Abort() = 0;
  };
  // `size` is the stored size the object will have. Null, with *reason set, when there is no room: "no_store_file" (the file count
  // would pass Limits::max_store_files), "too_large" (the object is larger than a store file) or "open_failed".
  std::unique_ptr<Writer> Begin(std::uint64_t size, const char** reason);

  // Reads every object back and hashes it, for a person or a test that wants to know. Never called at boot.
  // Returns the number of objects whose bytes do not hash to their name.
  std::size_t Verify(std::size_t* checked);

 private:
  struct File {
    std::uint64_t length = 0;  // bytes in the file
    std::uint64_t end = 0;     // the end of the last object the index names in it
  };
  friend class StoreWriter;
  Store(Env& env, std::string dir, const Limits& limits) : env_(env), dir_(std::move(dir)), limits_(limits) {}

  Env& env_;
  std::string dir_;
  Limits limits_;
  std::unordered_map<std::string, BlobLoc> blobs_;
  std::vector<File> files_;
  std::uint64_t held_bytes_ = 0;
  std::unique_ptr<AppendFile> index_;                  // opened at the first Commit
  std::unique_ptr<AppendFile> current_;                // the store file being appended to
  std::uint32_t current_file_ = 0;
  bool writer_live_ = false;
};

// What a mount holds: the shadow to publish and the label names the shadow's record tags index.
struct Mounted {
  std::unique_ptr<nevr_shadow::Shadow> shadow;
  std::vector<std::string> labels;  // Record::user indexes this
  std::size_t names = 0;            // resource names mounted (groups of entries)
  std::size_t records = 0;          // entries mounted (empty records included)
  std::size_t empty_records = 0;    // of them, records of 0 bytes
  std::size_t missing = 0;          // names left out because a blob is not in the store
  std::size_t refused = 0;          // names left out for another reason
  std::size_t files = 0;            // store files the shadow holds open
};

// Opens each store file the mounted objects lie in once, held for the life of the process, and builds a shadow from the manifest's
// entries of this platform whose label the policy serves and whose blob the store holds. A label's entries for one name mount
// together or not at all. A Mounted with a null shadow when nothing mounted.
Mounted Mount(Env& env, const Store& store, const Manifest& manifest, const Platform& platform, const Policy& policy);

}  // namespace nevr_cargo
