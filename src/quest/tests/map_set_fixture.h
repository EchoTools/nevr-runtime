// A synthetic NEVRRES1 map set for the Quest host tests (core/tests/map_derive_test.cpp builds the same format): the builder, and the
// valid set of one Android level root with its archive and one record. Shared by the Quest map host tests (map_sets_test, map_mount_test, map_service_test,
// map_service_start_test).
#pragma once

#include <fcntl.h>
#include <unistd.h>
#include <zstd.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "core/resource_set_registry.h"
#include "quest/data/sha256.h"
#include "quest/sentinel/map_sets.h"

namespace map_set_fixture {

namespace rs = nevr_resource_sets;

inline void PutLe(std::string* out, std::size_t at, unsigned width, std::uint64_t value) {
  constexpr unsigned kBitsPerByte = 8;
  for (unsigned i = 0; i < width; ++i) (*out)[at + i] = static_cast<char>(value >> (kBitsPerByte * i));
}

// The NEVRRES1 layout this builder writes (core/tests/map_derive_test.cpp BuildSet builds the same): the file header, then the record
// table (one entry per record), then the chunk table (one entry per record), then the zstd frames.
constexpr std::size_t kMagicBytes = 8;
constexpr std::size_t kHeaderBytes = 48;
constexpr std::size_t kRecordEntryBytes = 0x20, kChunkEntryBytes = 0x10;
constexpr std::size_t kHeaderSizeAt = 8, kHeaderRecordCountAt = 0x0c, kHeaderChunkCountAt = 0x10, kHeaderRecordTableAt = 0x18,
                      kHeaderChunkTableAt = 0x20, kHeaderFileSizeAt = 0x28;
constexpr std::size_t kRecordTypeAt = 0, kRecordNameAt = 8, kRecordFirstChunkAt = 0x10, kRecordSizeAt = 0x18, kRecordChunkBytesAt = 0x1c;
constexpr std::size_t kChunkOffsetAt = 4, kChunkSizeAt = 8, kChunkRawSizeAt = 12;
constexpr unsigned kWidth4 = 4, kWidth8 = 8;
constexpr std::uint64_t kRecordChunkBytes = 16;  // the bytes of chunk table a record's chunk list names
constexpr int kZstdLevel = 1;

// A NEVRRES1 set, each record in its own zstd chunk; records sorted by key.
inline std::string BuildSet(const std::vector<std::pair<std::pair<std::uint64_t, std::uint64_t>, std::string>>& records) {
  std::vector<std::string> frames;
  for (const auto& r : records) {
    std::string frame(ZSTD_compressBound(r.second.size()), '\0');
    frame.resize(ZSTD_compress(&frame[0], frame.size(), r.second.data(), r.second.size(), kZstdLevel));
    frames.push_back(frame);
  }
  const std::size_t n = records.size();
  std::string set(kHeaderBytes + n * kRecordEntryBytes + n * kChunkEntryBytes, '\0');
  std::memcpy(&set[0], "NEVRRES1", kMagicBytes);
  PutLe(&set, kHeaderSizeAt, kWidth4, kHeaderBytes);
  PutLe(&set, kHeaderRecordCountAt, kWidth4, n);
  PutLe(&set, kHeaderChunkCountAt, kWidth4, n);
  PutLe(&set, kHeaderRecordTableAt, kWidth8, kHeaderBytes);
  PutLe(&set, kHeaderChunkTableAt, kWidth8, kHeaderBytes + n * kRecordEntryBytes);
  PutLe(&set, kHeaderFileSizeAt, kWidth8, set.size());
  std::uint64_t at = set.size();
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t r = kHeaderBytes + i * kRecordEntryBytes, c = kHeaderBytes + n * kRecordEntryBytes + i * kChunkEntryBytes;
    PutLe(&set, r + kRecordTypeAt, kWidth8, records[i].first.first);
    PutLe(&set, r + kRecordNameAt, kWidth8, records[i].first.second);
    PutLe(&set, r + kRecordFirstChunkAt, kWidth4, i);
    PutLe(&set, r + kRecordSizeAt, kWidth4, records[i].second.size());
    PutLe(&set, r + kRecordChunkBytesAt, kWidth4, kRecordChunkBytes);
    PutLe(&set, c + kChunkOffsetAt, kWidth4, at);
    PutLe(&set, c + kChunkSizeAt, kWidth4, frames[i].size());
    PutLe(&set, c + kChunkRawSizeAt, kWidth4, records[i].second.size());
    at += frames[i].size();
  }
  for (const std::string& f : frames) set += f;
  return set;
}

// A level's archive: no dependencies, then one record `type`/`name` (the archive format of ValidSet's record list).
inline std::string BuildArchive(std::uint64_t type, std::uint64_t name) {
  constexpr std::size_t kCountAt = 4, kTypeAt = 8, kNameAt = 16, kArchiveBytes = 24;
  std::string archive(kArchiveBytes, '\0');
  PutLe(&archive, 0, kWidth4, 0);  // no dependencies
  PutLe(&archive, kCountAt, kWidth4, 1);
  PutLe(&archive, kTypeAt, kWidth8, type);
  PutLe(&archive, kNameAt, kWidth8, name);
  return archive;
}

constexpr std::size_t kPartBytes = 300, kRootBytes = 64;  // the sizes of a content record and of a level root in the valid set

constexpr std::uint64_t kLevel = 0x7691d78d414c5701ULL;  // mpl_arena_a_lowspec
constexpr std::uint64_t kComponent = 0x1111222233334444ULL, kPart = 0x55;

// The valid-set fixture (synthetic): an Android level root, its archive listing one record, and that record.
inline std::string ValidSet(std::uint64_t level = kLevel, std::uint64_t component = kComponent) {
  const std::string archive = BuildArchive(component, kPart);
  return BuildSet({{{rs::kAndroidTypes.archive, level}, archive},
                   {{component, kPart}, std::string(kPartBytes, 'c')},
                   {{rs::kAndroidTypes.level_root, level}, std::string(kRootBytes, 'r')}});
}

// A set whose only content record is a script (the Android script type, named kPart): what a map with a route-B script carries.
inline std::string SetWithScript() { return ValidSet(kLevel, rs::kAndroidTypes.script); }

// Registers `set` for `owner` the way the Quest's mounter does for a fetched map: written to a file under NEVR_TEST_TMP (named by `name`,
// the process and the owner), opened read-only, and handed to the registry with the digest the fetch recorded (no re-hash). True when the
// registry took it; on a refusal the descriptor stays open, as with the mounter's caller.
inline std::string SetFilePath(const char* name) {
  const char* dir = std::getenv("NEVR_TEST_TMP");
  return std::string(dir != nullptr ? dir : "/tmp") + "/map_set_" + std::to_string(::getpid()) + "_" + name;
}

inline bool RegisterSetFile(const std::string& owner, const char* name, const std::string& set) {
  const std::string path = SetFilePath(name);
  constexpr mode_t kFileMode = 0600;
  const int writer = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, kFileMode);
  if (writer < 0 || ::write(writer, set.data(), set.size()) != static_cast<ssize_t>(set.size())) {
    if (writer >= 0) ::close(writer);
    ::unlink(path.c_str());
    return false;
  }
  ::close(writer);
  const int fd = ::open(path.c_str(), O_RDONLY);
  ::unlink(path.c_str());  // the descriptor keeps the file; nothing is left in the test directory
  rs::MapFileRequest request;
  request.owner = owner;
  request.label = owner + "@1";
  request.sha256 = nevr_quest::data::Sha256Hex(set.data(), set.size());
  request.rehash = false;
  return nevr_quest::map_sets::RegisterFileResourceSet(request, fd).ok();
}

}  // namespace map_set_fixture
