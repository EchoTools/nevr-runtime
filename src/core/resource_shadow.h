/* The shadow manifest: a read-only package the game's own resolver can be pointed at (docs/adr/0045-cdn-texture-delivery.md).
 *
 * The game fills every read request in one function, CResourceManager::FillResourceFileRequest (echovr.exe 0x140fa1fe0,
 * libr15.so 0x25439f0). It reads a 16-byte manager whose +8 points at a manifest, binary-searches the manifest's records for
 * (on-disk type, name) with signed 64-bit compares, and fills the request from the record, the chunk it names and the package
 * file handle the chunk names. A Shadow is such a manager and manifest, built in the engine's own layout from records the
 * runtime holds, so a hook can call the game's function with the shadow's manager and the engine fills the request itself and
 * its own reader reads our file.
 *
 * The layout (docs/engine/asset-system.md "Manifest"; docs/reference/package-manifest-format.md; pinned_targets.h
 * resource_manifest_layout for the Quest side):
 *   manager   +0x08 pointer to the manifest
 *   manifest  +0x00 pointer to the records, sorted as signed (type, name)
 *             +0x30 u64 record count
 *             +0x80 pointer to the chunk table, moved down by chunk_base * 0x10 (see below)
 *             +0x128 pointer to the package handle table, one 8-byte handle per package
 *   record    0x20 bytes: u64 type, u64 name, u32 chunk, u32 offset in the decompressed chunk, u32 size, u32 alignment
 *   chunk     0x10 bytes: u32 package, u32 file offset, u32 compressed size, u32 decompressed size
 *
 * Chunk numbering. A record's chunk word is chunk_base + its index in Input::chunks, and the +0x80 pointer is the table's
 * address minus chunk_base * 0x10 (as an integer, wrapping), so the game's `table + chunk * 0x10` lands on the right entry. The
 * engine's block cache is keyed by the chunk index alone (echovr.exe 0x1415255a0: the cached block's slot holds the chunk index),
 * so a shadow chunk index must never equal a stock one (stock has 10,304 chunks) and never equal another shadow's, whether that is
 * a second kind of shadow (cargo and plugin packages) or the same kind built again (a cargo republish): a cached block of one would
 * be served as the other. So the indices of every shadow in the process come from ONE counter that never gives a value out twice:
 * Build takes n fresh indices, above every index given before, and fails (taking none) when fewer than n are left below
 * kChunkIndexLimit. A base above the stock count, with (base + chunks) * 0x10 below 2^31, lands on the same entry whether the game
 * does the multiply in 32 or 64 bits, signed or unsigned.
 *
 * This is the one builder both the CDN textures (cargo) and the plugin levels use. Nothing here allocates after Build, takes a
 * lock or writes: a Shadow is immutable, and a reader needs no synchronisation once it holds the pointer. It is never freed by
 * the code that publishes it (a request being filled from it must stay valid).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace nevr_shadow {

static_assert(sizeof(void*) == 8, "the shadow's pointers are the game's 64-bit ones");

inline constexpr std::size_t kManagerBytes = 16;
inline constexpr std::size_t kManagerManifest = 0x8;
inline constexpr std::size_t kManifestBytes = 0x168;
inline constexpr std::size_t kManifestRecords = 0x0;
inline constexpr std::size_t kManifestCount = 0x30;
inline constexpr std::size_t kManifestChunks = 0x80;
inline constexpr std::size_t kManifestHandles = 0x128;
inline constexpr std::size_t kRecordBytes = 0x20;
inline constexpr std::size_t kChunkBytes = 0x10;

// The stock manifest holds 10,304 chunks; no shadow chunk index may be below this (kFirstChunkIndex is far above).
inline constexpr std::uint32_t kStockChunkCount = 10304;
// The first index the process-wide counter gives out (above stock; the cargo design's Q20), and the limit it stays under:
// (index) * 0x10 stays below 2^31 for every index below kChunkIndexLimit, so the game's index multiply is the same in any width.
inline constexpr std::uint32_t kFirstChunkIndex = 0x01000000;
inline constexpr std::uint64_t kChunkIndexLimit = (std::uint64_t{1} << 31) / kChunkBytes;  // 0x08000000

// One resource, as the caller states it. `chunk` indexes Input::chunks (0-based); Build adds the base.
struct Record {
  std::uint64_t type = 0;
  std::uint64_t name = 0;
  std::uint32_t chunk = 0;
  std::uint32_t offset_in_chunk = 0;
  std::uint32_t size = 0;
  std::uint32_t alignment = 0;
  std::uint32_t user = 0;  // the caller's tag (the cargo label's index); never written into the engine's layout
};

// One stored block of a package file. `package` indexes Input::handles.
struct Chunk {
  std::uint32_t package = 0;
  std::uint32_t file_offset = 0;
  std::uint32_t compressed = 0;
  std::uint32_t decompressed = 0;
};

struct Input {
  std::vector<Record> records;  // any order; Build sorts them as signed (type, name)
  std::vector<Chunk> chunks;
  std::vector<std::int64_t> handles;  // one open file handle per package, in the engine's handle type (a long on Android)
};

enum class Status {
  kOk = 0,
  kEmpty,         // no records
  kIndexSpace,    // fewer indices are left below kChunkIndexLimit than the input has chunks (none are taken)
  kChunkIndex,    // a record names a chunk that is not in Input::chunks
  kPackageIndex,  // a chunk names a package that has no handle
  kDuplicate,     // two records with one (type, name)
};

const char* StatusName(Status status) noexcept;

// What the engine reads for one key: the record, its chunk and the package handle.
struct Found {
  std::uint32_t chunk = 0;  // the record's chunk word (base included)
  std::uint32_t offset_in_chunk = 0;
  std::uint32_t size = 0;
  std::uint32_t alignment = 0;
  std::uint32_t package = 0;
  std::uint32_t file_offset = 0;
  std::uint32_t compressed = 0;
  std::uint32_t decompressed = 0;
  std::int64_t handle = 0;
};

// The engine's own reading of a manager in the layout above: manager + 8, the record search (signed type, then signed name, an
// unsigned count), then the chunk by `table + chunk * 0x10` and the handle by package. True on a hit. Works on any manager in
// this layout, a Shadow's or a test's; it is the reference the tests hold the builder to.
bool ReadLikeTheEngine(const void* manager, std::uint64_t type, std::uint64_t name, Found* out) noexcept;

class Shadow {
 public:
  // Builds a shadow, or returns the first thing wrong with the input and leaves *out empty. A shadow takes its chunk indices from
  // the process-wide counter (see the header); a Build that fails takes none.
  static Status Build(const Input& input, std::unique_ptr<Shadow>* out);

  // The next index the counter gives out. Test support: ResetChunkCounterForTest sets it (kFirstChunkIndex by default).
  static std::uint32_t NextChunkIndex() noexcept;
  static void ResetChunkCounterForTest(std::uint32_t next = kFirstChunkIndex) noexcept;

  // The 16-byte manager to pass to the game's function in place of the live one.
  const void* manager() const noexcept { return manager_; }

  // The engine's reading of this shadow for (type, name); also gives the caller's tag of the record.
  bool Find(std::uint64_t type, std::uint64_t name, Found* out, std::uint32_t* user = nullptr) const noexcept;

  std::size_t record_count() const noexcept { return records_.size() / kRecordBytes; }
  // The first of this shadow's chunk indices; it owns [chunk_base(), chunk_base() + chunk_count()).
  std::uint32_t chunk_base() const noexcept { return chunk_base_; }
  std::size_t chunk_count() const noexcept { return chunks_.size() / kChunkBytes; }

  // Test support: the manifest bytes (kManifestBytes) as the engine would read them.
  const unsigned char* manifest_for_test() const noexcept { return manifest_.data(); }

 private:
  Shadow() = default;
  Shadow(const Shadow&) = delete;
  Shadow& operator=(const Shadow&) = delete;

  std::uint32_t chunk_base_ = 0;
  // The storage the manifest points into. Vectors never move after Build.
  std::vector<unsigned char> records_;
  std::vector<unsigned char> chunks_;
  std::vector<std::int64_t> handles_;
  std::vector<std::uint32_t> users_;  // by sorted record index
  std::vector<unsigned char> manifest_;
  alignas(8) unsigned char manager_[kManagerBytes] = {};
};

}  // namespace nevr_shadow
