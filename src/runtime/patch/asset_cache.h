#pragma once

// #703: a content-addressed cache of asset bytes under %LOCALAPPDATA%\nevr\echovr\cache\.
//
// Layout (tools/asset_cache.py writes the same layout):
//   <sha256 lowercase hex>.blob   the bytes; the name IS the SHA-256 of the contents
//   index.json                    {"version":1,"overrides":[{"type":"0x..","name":"0x..","sha256":"..","label":".."}]}
//                                 which blob stands in for which (type, name) resource
//   <sha256>.<pid>.<tick>.tmp     a write in progress, never read
//
// Rules, each pinned by test_asset_cache.cpp:
//   * A blob is trusted only after it is read and hashed again: Load() compares the SHA-256 of the bytes it read
//     with the key, every time, not only when the file was downloaded.
//   * Bytes are written to a temp file in the same directory, flushed, then renamed into place, so a kill
//     mid-write leaves a .tmp that nothing reads, never a short blob under a trusted name.
//   * Size cap: Evict() removes blobs that no index entry names, least recently used first (a Load hit or a
//     Store refreshes a blob's last-write time), until the blobs fit the cap. A blob the index names is in use
//     and is never evicted; when only those remain over the cap, the report says so. Temp files older than a
//     day are removed.
//
// Pure of the game and of the logger: results are returned, the caller logs them. Paths are UTF-16 end to end.

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

// std::min/std::max in the .cpp: the core PCH pulls windows.h WITHOUT NOMINMAX, so this unit skips the PCH.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace nevr_asset_cache {

/// Blob bytes a cache keeps in total before Evict() starts removing unreferenced ones.
inline constexpr std::uint64_t kDefaultCapBytes = 256ull * 1024 * 1024;
/// A single blob larger than this is refused on read and on store: a texture override is a few MiB.
inline constexpr std::uint64_t kMaxBlobBytes = 64ull * 1024 * 1024;
/// A .tmp older than this is a leftover of a killed write and is removed by Evict().
inline constexpr std::uint64_t kStaleTempSeconds = 24ull * 60 * 60;

inline constexpr wchar_t kIndexName[] = L"index.json";

/// %LOCALAPPDATA%\nevr\echovr\cache; empty when LOCALAPPDATA is unset.
std::wstring DefaultDir();

/// Lowercase hex SHA-256 of the bytes; empty when the platform hash fails.
std::string Sha256Hex(const void* data, std::size_t len);

/// True for exactly 64 lowercase hex characters: the only spelling a key may have (no path separators).
bool IsSha256Hex(const std::string& s);

std::wstring BlobPath(const std::wstring& dir, const std::string& sha256);

enum class StoreStatus { Stored, AlreadyPresent, Failed };
struct StoreResult {
  StoreStatus status = StoreStatus::Failed;
  std::string sha256;
  DWORD error = 0;
  const char* failedStep = "";  ///< which step failed, for the log
};

/// Writes the bytes under their own SHA-256: temp file, flush, rename. An existing blob that re-hashes to its key
/// is left alone (AlreadyPresent); a corrupt one is replaced.
StoreResult Store(const std::wstring& dir, const void* data, std::size_t len);

enum class LoadStatus { Hit, Missing, HashMismatch, TooLarge, ReadFailed, BadKey };
struct LoadResult {
  LoadStatus status = LoadStatus::BadKey;
  std::string expectedSha256;
  std::string actualSha256;  ///< set on Hit and HashMismatch
  std::vector<std::uint8_t> bytes;  ///< only on Hit
  DWORD error = 0;
};

/// Reads the blob for the key and hashes it again. Hit only when the hash equals the key.
LoadResult Load(const std::wstring& dir, const std::string& sha256);

struct IndexEntry {
  std::uint64_t typeHash = 0;  ///< 0 matches any type, as in resource_override
  std::uint64_t nameHash = 0;
  std::string sha256;
  std::string label;
};

struct IndexResult {
  bool present = false;  ///< index.json existed
  bool ok = true;        ///< false: it existed and could not be read or parsed (error says why)
  std::string error;
  std::vector<IndexEntry> entries;
  std::vector<std::string> skipped;  ///< one reason per entry that was malformed and ignored
};

IndexResult ReadIndex(const std::wstring& dir);

/// Adds or replaces the entry for (type, name) in index.json, written temp-and-rename. False with `error` set on failure.
bool SetIndexEntry(const std::wstring& dir, const IndexEntry& entry, std::string* error);

struct EvictReport {
  std::uint64_t blobBytesBefore = 0;
  std::uint64_t blobBytesAfter = 0;
  std::uint32_t blobsEvicted = 0;
  std::uint32_t staleTempsRemoved = 0;
  bool overCapAllReferenced = false;  ///< still over the cap, every remaining blob named by the index
};

/// Test support: every write (a blob, the index) stops with ERROR_DISK_FULL once it has written this many bytes
/// ("write temp" step) and, like a kill, removes nothing it wrote. The default, UINT64_MAX, never stops.
void SetWriteFailureAfterBytesForTest(std::uint64_t bytes);

/// Applies the size cap. `referenced` is the set of keys the index names.
EvictReport Evict(const std::wstring& dir, std::uint64_t capBytes, const std::set<std::string>& referenced);

}  // namespace nevr_asset_cache
