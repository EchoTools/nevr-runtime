/*
 * nevr/resource_set.h - the format of a resource set: the game resources a plugin carries inside its own file
 * (plugin API v9, register_resources; docs/reference/plugin-resources.md).
 *
 * A resource set is one block of bytes. Its records name game resources by (on-disk type, name), the keys the game's
 * resolver searches (CResourceManager::FillResourceFileRequest, echovr.exe 0x140fa1fe0). Its chunks are zstd frames,
 * each holding one or more resources back to back, the way the game's own package files hold them. The runtime
 * serves a record by pointing the game's resolver at the chunk in the plugin's file, and the game's own reader reads
 * and decompresses it. Its overrides are whole replacement bytes for resources the game already has, which a plugin
 * registers with NEVR_RegisterResourceOverride (nevr/pc/plugin_resources.h).
 *
 * Layout, little endian, offsets from the start of the set:
 *
 *   0x00  char[8]  "NEVRRES1"
 *   0x08  u32      header bytes (48)
 *   0x0c  u32      record count (at least 1)
 *   0x10  u32      chunk count
 *   0x14  u32      override count
 *   0x18  u64      offset of the record table   (record count x 0x20)
 *   0x20  u64      offset of the chunk table    (chunk count x 0x10)
 *   0x28  u64      offset of the override table (override count x 0x18)
 *
 *   record   u64 on-disk type, u64 name, u32 chunk (index in the chunk table), u32 offset in the decompressed chunk,
 *            u32 size, u32 alignment
 *   chunk    u32 reserved (0), u32 offset of the frame, u32 compressed size, u32 decompressed size
 *   override u64 on-disk type, u64 name, u32 offset of the bytes, u32 size
 *
 * The rules Parse checks are the conditions the game's reader needs (docs/engine/asset-system.md, "Reading it"):
 * every table and every frame inside the set; a frame starts with the zstd magic; a record lies inside its chunk's
 * decompressed bytes; a record of at most kBlockLimit bytes sits in a chunk of at most kBlockLimit decompressed bytes
 * (the reader's 8-slot block cache, 0x1415255a0); a larger record is alone in its chunk, at offset 0 and the chunk's
 * whole size (the reader decompresses such a chunk straight into the resource's buffer, 0x141525310); an alignment
 * is a power of two; no two records, and no two overrides, share a key, and no override shares a record's key.
 * Record order is free: the runtime sorts.
 *
 * Header-only and platform-free: the runtime (PC and Quest) and the plugin SDK read the same definition.
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace nevr_resource_set {

inline constexpr char kMagic[8] = {'N', 'E', 'V', 'R', 'R', 'E', 'S', '1'};
inline constexpr std::uint32_t kHeaderBytes = 48;
inline constexpr std::uint32_t kRecordBytes = 0x20;
inline constexpr std::uint32_t kChunkBytes = 0x10;
inline constexpr std::uint32_t kOverrideBytes = 0x18;
// The reader's block cache serves a resource of at most this many bytes from a cached decompressed chunk.
inline constexpr std::uint32_t kBlockLimit = 0x80000;
inline constexpr std::uint32_t kZstdMagic = 0xFD2FB528u;

struct Record {
    std::uint64_t type = 0;
    std::uint64_t name = 0;
    std::uint32_t chunk = 0;
    std::uint32_t offset_in_chunk = 0;
    std::uint32_t size = 0;
    std::uint32_t alignment = 0;
};

struct Chunk {
    std::uint32_t reserved = 0;
    std::uint32_t offset = 0;
    std::uint32_t compressed = 0;
    std::uint32_t decompressed = 0;
};

struct Override {
    std::uint64_t type = 0;
    std::uint64_t name = 0;
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
};

enum class Problem {
    kNone = 0,
    kTooSmall,           // fewer bytes than the header
    kMagic,              // not "NEVRRES1"
    kHeader,             // a header size other than 48
    kNoRecords,          // a record count of 0
    kTableRange,         // a table runs past the end of the set
    kChunkRange,         // a frame runs past the end of the set, or is empty
    kChunkNotZstd,       // a frame does not start with the zstd magic
    kChunkSize,          // a chunk that decompresses to 0 bytes
    kRecordChunk,        // a record names a chunk the table does not have
    kRecordRange,        // a record of 0 bytes, or one that runs past its chunk's decompressed bytes
    kRecordAlignment,    // an alignment that is 0 or not a power of two
    kRecordInLargeChunk, // a record of at most kBlockLimit bytes in a chunk larger than kBlockLimit
    kRecordNotAlone,     // a record larger than kBlockLimit that is not alone in its chunk
    kDuplicate,          // two records with one key
    kOverrideRange,      // an override of 0 bytes, or one that runs past the end of the set
    kOverrideDuplicate,  // two overrides with one key
    kOverrideIsRecord,   // an override with a record's key
};

inline const char* ProblemName(Problem problem) {
    switch (problem) {
        case Problem::kNone: return "none";
        case Problem::kTooSmall: return "too_small";
        case Problem::kMagic: return "magic";
        case Problem::kHeader: return "header";
        case Problem::kNoRecords: return "no_records";
        case Problem::kTableRange: return "table_range";
        case Problem::kChunkRange: return "chunk_range";
        case Problem::kChunkNotZstd: return "chunk_not_zstd";
        case Problem::kChunkSize: return "chunk_size";
        case Problem::kRecordChunk: return "record_chunk";
        case Problem::kRecordRange: return "record_range";
        case Problem::kRecordAlignment: return "record_alignment";
        case Problem::kRecordInLargeChunk: return "record_in_large_chunk";
        case Problem::kRecordNotAlone: return "record_not_alone";
        case Problem::kDuplicate: return "duplicate";
        case Problem::kOverrideRange: return "override_range";
        case Problem::kOverrideDuplicate: return "override_duplicate";
        case Problem::kOverrideIsRecord: return "override_is_record";
    }
    return "unknown";
}

namespace detail {

template <typename T>
inline T Load(const unsigned char* base, std::uint64_t at) {
    T v{};
    std::memcpy(&v, base + at, sizeof(v));
    return v;
}

inline bool Inside(std::uint64_t offset, std::uint64_t length, std::uint64_t total) {
    return offset <= total && length <= total - offset;
}

inline bool PowerOfTwo(std::uint32_t v) { return v != 0 && (v & (v - 1)) == 0; }

}  // namespace detail

// A parsed set: read access to the tables of bytes that passed Parse. It points into the caller's bytes.
class View {
public:
    std::uint32_t record_count() const { return records_; }
    std::uint32_t chunk_count() const { return chunks_; }
    std::uint32_t override_count() const { return overrides_; }
    std::uint64_t size() const { return size_; }
    const unsigned char* data() const { return data_; }

    Record RecordAt(std::uint32_t i) const {
        const std::uint64_t at = record_table_ + std::uint64_t{i} * kRecordBytes;
        Record r;
        r.type = detail::Load<std::uint64_t>(data_, at);
        r.name = detail::Load<std::uint64_t>(data_, at + 8);
        r.chunk = detail::Load<std::uint32_t>(data_, at + 0x10);
        r.offset_in_chunk = detail::Load<std::uint32_t>(data_, at + 0x14);
        r.size = detail::Load<std::uint32_t>(data_, at + 0x18);
        r.alignment = detail::Load<std::uint32_t>(data_, at + 0x1c);
        return r;
    }

    Chunk ChunkAt(std::uint32_t i) const {
        const std::uint64_t at = chunk_table_ + std::uint64_t{i} * kChunkBytes;
        Chunk c;
        c.reserved = detail::Load<std::uint32_t>(data_, at);
        c.offset = detail::Load<std::uint32_t>(data_, at + 4);
        c.compressed = detail::Load<std::uint32_t>(data_, at + 8);
        c.decompressed = detail::Load<std::uint32_t>(data_, at + 12);
        return c;
    }

    Override OverrideAt(std::uint32_t i) const {
        const std::uint64_t at = override_table_ + std::uint64_t{i} * kOverrideBytes;
        Override o;
        o.type = detail::Load<std::uint64_t>(data_, at);
        o.name = detail::Load<std::uint64_t>(data_, at + 8);
        o.offset = detail::Load<std::uint32_t>(data_, at + 0x10);
        o.size = detail::Load<std::uint32_t>(data_, at + 0x14);
        return o;
    }

private:
    friend struct Parser;
    const unsigned char* data_ = nullptr;
    std::uint64_t size_ = 0;
    std::uint32_t records_ = 0;
    std::uint32_t chunks_ = 0;
    std::uint32_t overrides_ = 0;
    std::uint64_t record_table_ = 0;
    std::uint64_t chunk_table_ = 0;
    std::uint64_t override_table_ = 0;
};

// What Parse found wrong, and where: the index of the record, chunk or override it names (0 for a header problem).
struct Result {
    Problem problem = Problem::kNone;
    std::uint32_t index = 0;
    bool ok() const { return problem == Problem::kNone; }
};

struct Parser {
    static Result Run(const void* bytes, std::uint64_t size, View* out) {
        *out = View{};
        const auto* data = static_cast<const unsigned char*>(bytes);
        if (data == nullptr || size < kHeaderBytes) return {Problem::kTooSmall, 0};
        if (std::memcmp(data, kMagic, sizeof(kMagic)) != 0) return {Problem::kMagic, 0};
        if (detail::Load<std::uint32_t>(data, 0x08) != kHeaderBytes) return {Problem::kHeader, 0};
        View v;
        v.data_ = data;
        v.size_ = size;
        v.records_ = detail::Load<std::uint32_t>(data, 0x0c);
        v.chunks_ = detail::Load<std::uint32_t>(data, 0x10);
        v.overrides_ = detail::Load<std::uint32_t>(data, 0x14);
        v.record_table_ = detail::Load<std::uint64_t>(data, 0x18);
        v.chunk_table_ = detail::Load<std::uint64_t>(data, 0x20);
        v.override_table_ = detail::Load<std::uint64_t>(data, 0x28);
        if (v.records_ == 0) return {Problem::kNoRecords, 0};
        if (!detail::Inside(v.record_table_, std::uint64_t{v.records_} * kRecordBytes, size) ||
            !detail::Inside(v.chunk_table_, std::uint64_t{v.chunks_} * kChunkBytes, size) ||
            !detail::Inside(v.override_table_, std::uint64_t{v.overrides_} * kOverrideBytes, size)) {
            return {Problem::kTableRange, 0};
        }
        for (std::uint32_t i = 0; i < v.chunks_; ++i) {
            const Chunk c = v.ChunkAt(i);
            if (c.compressed < 4 || !detail::Inside(c.offset, c.compressed, size)) return {Problem::kChunkRange, i};
            if (detail::Load<std::uint32_t>(data, c.offset) != kZstdMagic) return {Problem::kChunkNotZstd, i};
            if (c.decompressed == 0) return {Problem::kChunkSize, i};
        }
        std::vector<std::pair<std::uint64_t, std::uint64_t>> keys;
        keys.reserve(v.records_);
        for (std::uint32_t i = 0; i < v.records_; ++i) {
            const Record r = v.RecordAt(i);
            if (r.chunk >= v.chunks_) return {Problem::kRecordChunk, i};
            const Chunk c = v.ChunkAt(r.chunk);
            if (r.size == 0 || !detail::Inside(r.offset_in_chunk, r.size, c.decompressed)) return {Problem::kRecordRange, i};
            if (!detail::PowerOfTwo(r.alignment)) return {Problem::kRecordAlignment, i};
            if (r.size <= kBlockLimit) {
                if (c.decompressed > kBlockLimit) return {Problem::kRecordInLargeChunk, i};
            } else if (r.offset_in_chunk != 0 || r.size != c.decompressed) {
                return {Problem::kRecordNotAlone, i};
            }
            keys.emplace_back(r.type, r.name);
        }
        std::vector<std::uint32_t> order(keys.size());
        for (std::uint32_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&keys](std::uint32_t a, std::uint32_t b) { return keys[a] < keys[b]; });
        for (std::size_t i = 1; i < order.size(); ++i) {
            if (keys[order[i - 1]] == keys[order[i]]) return {Problem::kDuplicate, order[i]};
        }
        std::vector<std::pair<std::uint64_t, std::uint64_t>> overrideKeys;
        overrideKeys.reserve(v.overrides_);
        for (std::uint32_t i = 0; i < v.overrides_; ++i) {
            const Override o = v.OverrideAt(i);
            if (o.size == 0 || !detail::Inside(o.offset, o.size, size)) return {Problem::kOverrideRange, i};
            overrideKeys.emplace_back(o.type, o.name);
        }
        std::vector<std::pair<std::uint64_t, std::uint64_t>> sortedRecords(keys);
        std::sort(sortedRecords.begin(), sortedRecords.end());
        std::vector<std::uint32_t> overrideOrder(overrideKeys.size());
        for (std::uint32_t i = 0; i < overrideOrder.size(); ++i) overrideOrder[i] = i;
        std::sort(overrideOrder.begin(), overrideOrder.end(),
                  [&overrideKeys](std::uint32_t a, std::uint32_t b) { return overrideKeys[a] < overrideKeys[b]; });
        for (std::size_t i = 0; i < overrideOrder.size(); ++i) {
            const auto& key = overrideKeys[overrideOrder[i]];
            if (i > 0 && overrideKeys[overrideOrder[i - 1]] == key) return {Problem::kOverrideDuplicate, overrideOrder[i]};
            if (std::binary_search(sortedRecords.begin(), sortedRecords.end(), key)) {
                return {Problem::kOverrideIsRecord, overrideOrder[i]};
            }
        }
        *out = v;
        return {Problem::kNone, 0};
    }
};

// Parses `size` bytes at `bytes` as a resource set. On success *out reads them; on a refusal *out is empty.
inline Result Parse(const void* bytes, std::uint64_t size, View* out) { return Parser::Run(bytes, size, out); }

}  // namespace nevr_resource_set
