#pragma once

// Test support (#2161): writes resource sets (nevr/resource_set.h) and archives in the layouts the runtime reads. The
// chunks are zstd frames of raw blocks, written byte by byte from the zstd format (RFC 8878: a frame header with the
// single-segment flag and a 4-byte content size, then raw blocks of at most 128 KiB), so a set's bytes depend on
// nothing but its contents and every zstd decoder reads them.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "nevr/resource_set.h"

namespace nevr_test_sets {

inline void PutU32(std::vector<unsigned char>* out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) out->push_back(static_cast<unsigned char>(v >> (8 * i)));
}
inline void PutU64(std::vector<unsigned char>* out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) out->push_back(static_cast<unsigned char>(v >> (8 * i)));
}
inline void SetU32(std::vector<unsigned char>* out, std::size_t at, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) (*out)[at + i] = static_cast<unsigned char>(v >> (8 * i));
}
inline void SetU64(std::vector<unsigned char>* out, std::size_t at, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) (*out)[at + i] = static_cast<unsigned char>(v >> (8 * i));
}

// One zstd frame holding `plain` in raw blocks.
inline std::vector<unsigned char> RawZstdFrame(const std::vector<unsigned char>& plain) {
  std::vector<unsigned char> frame = {0x28, 0xB5, 0x2F, 0xFD, 0xA0};  // magic; FCS 4 bytes, single segment
  PutU32(&frame, static_cast<std::uint32_t>(plain.size()));
  const std::size_t kBlock = 128 * 1024;
  std::size_t at = 0;
  do {
    const std::size_t n = std::min(kBlock, plain.size() - at);
    const bool last = at + n == plain.size();
    const std::uint32_t header = (static_cast<std::uint32_t>(n) << 3) | (last ? 1u : 0u);  // raw block (type 0)
    frame.push_back(static_cast<unsigned char>(header));
    frame.push_back(static_cast<unsigned char>(header >> 8));
    frame.push_back(static_cast<unsigned char>(header >> 16));
    frame.insert(frame.end(), plain.begin() + static_cast<std::ptrdiff_t>(at),
                 plain.begin() + static_cast<std::ptrdiff_t>(at + n));
    at += n;
  } while (at < plain.size());
  return frame;
}

// An archive body (docs/engine/asset-system.md, "Archives"): dependencies, entries, an empty type table, a closing 0.
inline std::vector<unsigned char> Archive(const std::vector<std::uint64_t>& dependencies,
                                          const std::vector<std::pair<std::uint64_t, std::uint64_t>>& entries) {
  std::vector<unsigned char> out;
  PutU32(&out, static_cast<std::uint32_t>(dependencies.size()));
  for (std::uint64_t d : dependencies) PutU64(&out, d);
  PutU32(&out, static_cast<std::uint32_t>(entries.size()));
  for (const auto& e : entries) {
    PutU64(&out, e.first);
    PutU64(&out, e.second);
  }
  PutU32(&out, 0);
  PutU32(&out, 0);
  return out;
}

class SetBuilder {
 public:
  std::uint32_t AddChunk() {
    chunks_.emplace_back();
    return static_cast<std::uint32_t>(chunks_.size() - 1);
  }
  // Appends `bytes` to chunk `chunk`, padded to `alignment`.
  void AddRecord(std::uint64_t type, std::uint64_t name, const std::vector<unsigned char>& bytes, std::uint32_t chunk,
                 std::uint32_t alignment = 16) {
    std::vector<unsigned char>& plain = chunks_[chunk];
    while (plain.size() % alignment != 0) plain.push_back(0);
    nevr_resource_set::Record r;
    r.type = type;
    r.name = name;
    r.chunk = chunk;
    r.offset_in_chunk = static_cast<std::uint32_t>(plain.size());
    r.size = static_cast<std::uint32_t>(bytes.size());
    r.alignment = alignment;
    plain.insert(plain.end(), bytes.begin(), bytes.end());
    records_.push_back(r);
  }
  void AddOverride(std::uint64_t type, std::uint64_t name, const std::vector<unsigned char>& bytes) {
    overrides_.push_back({type, name, bytes});
  }

  // The set's bytes. Offsets of the tables for tests that corrupt them: RecordAt(i), ChunkAt(i), OverrideAt(i).
  std::vector<unsigned char> Build() {
    std::vector<unsigned char> out(nevr_resource_set::kHeaderBytes, 0);
    std::memcpy(out.data(), nevr_resource_set::kMagic, sizeof(nevr_resource_set::kMagic));
    SetU32(&out, 0x08, nevr_resource_set::kHeaderBytes);
    SetU32(&out, 0x0c, static_cast<std::uint32_t>(records_.size()));
    SetU32(&out, 0x10, static_cast<std::uint32_t>(chunks_.size()));
    SetU32(&out, 0x14, static_cast<std::uint32_t>(overrides_.size()));
    recordTable_ = out.size();
    SetU64(&out, 0x18, recordTable_);
    out.resize(out.size() + records_.size() * nevr_resource_set::kRecordBytes);
    chunkTable_ = out.size();
    SetU64(&out, 0x20, chunkTable_);
    out.resize(out.size() + chunks_.size() * nevr_resource_set::kChunkBytes);
    overrideTable_ = out.size();
    SetU64(&out, 0x28, overrideTable_);
    out.resize(out.size() + overrides_.size() * nevr_resource_set::kOverrideBytes);
    for (std::size_t i = 0; i < chunks_.size(); ++i) {
      const std::vector<unsigned char> frame = RawZstdFrame(chunks_[i]);
      const std::size_t at = ChunkAt(i);
      SetU32(&out, at + 4, static_cast<std::uint32_t>(out.size()));
      SetU32(&out, at + 8, static_cast<std::uint32_t>(frame.size()));
      SetU32(&out, at + 12, static_cast<std::uint32_t>(chunks_[i].size()));
      out.insert(out.end(), frame.begin(), frame.end());
    }
    for (std::size_t i = 0; i < records_.size(); ++i) {
      const std::size_t at = RecordAt(i);
      SetU64(&out, at, records_[i].type);
      SetU64(&out, at + 8, records_[i].name);
      SetU32(&out, at + 0x10, records_[i].chunk);
      SetU32(&out, at + 0x14, records_[i].offset_in_chunk);
      SetU32(&out, at + 0x18, records_[i].size);
      SetU32(&out, at + 0x1c, records_[i].alignment);
    }
    for (std::size_t i = 0; i < overrides_.size(); ++i) {
      const std::size_t at = OverrideAt(i);
      SetU64(&out, at, overrides_[i].type);
      SetU64(&out, at + 8, overrides_[i].name);
      SetU32(&out, at + 0x10, static_cast<std::uint32_t>(out.size()));
      SetU32(&out, at + 0x14, static_cast<std::uint32_t>(overrides_[i].bytes.size()));
      out.insert(out.end(), overrides_[i].bytes.begin(), overrides_[i].bytes.end());
    }
    return out;
  }

  std::size_t RecordAt(std::size_t i) const { return recordTable_ + i * nevr_resource_set::kRecordBytes; }
  std::size_t ChunkAt(std::size_t i) const { return chunkTable_ + i * nevr_resource_set::kChunkBytes; }
  std::size_t OverrideAt(std::size_t i) const { return overrideTable_ + i * nevr_resource_set::kOverrideBytes; }
  const std::vector<unsigned char>& Plain(std::uint32_t chunk) const { return chunks_[chunk]; }

 private:
  struct PendingOverride {
    std::uint64_t type;
    std::uint64_t name;
    std::vector<unsigned char> bytes;
  };
  std::vector<std::vector<unsigned char>> chunks_;
  std::vector<nevr_resource_set::Record> records_;
  std::vector<PendingOverride> overrides_;
  std::size_t recordTable_ = 0;
  std::size_t chunkTable_ = 0;
  std::size_t overrideTable_ = 0;
};

inline std::vector<unsigned char> Bytes(std::size_t n, unsigned char fill) { return std::vector<unsigned char>(n, fill); }

}  // namespace nevr_test_sets
