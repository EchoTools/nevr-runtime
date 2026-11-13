#include "core/map_set_tables.h"

#include <algorithm>
#include <cstring>

#include "core/cargo_validate.h"

namespace nevr_maps {
namespace {

namespace set = nevr_resource_set;

// The header's fields, at the offsets sdk/include/nevr/resource_set.h gives.
constexpr std::uint64_t kHeaderSizeAt = 0x08;
constexpr std::uint64_t kRecordCountAt = 0x0c;
constexpr std::uint64_t kChunkCountAt = 0x10;
constexpr std::uint64_t kOverrideCountAt = 0x14;
constexpr std::uint64_t kRecordTableAt = 0x18;
constexpr std::uint64_t kChunkTableAt = 0x20;
constexpr std::uint64_t kOverrideTableAt = 0x28;
// A record's and a chunk's fields.
constexpr std::uint64_t kRecordNameAt = 0x08;
constexpr std::uint64_t kRecordChunkAt = 0x10;
constexpr std::uint64_t kRecordOffsetAt = 0x14;
constexpr std::uint64_t kRecordSizeAt = 0x18;
constexpr std::uint64_t kRecordAlignmentAt = 0x1c;
constexpr std::uint64_t kChunkOffsetAt = 0x04;
constexpr std::uint64_t kChunkCompressedAt = 0x08;
constexpr std::uint64_t kChunkDecompressedAt = 0x0c;
constexpr unsigned kBitsPerByte = 8;

SetProblem Problem(const char* reason, std::uint32_t index = 0) {
  SetProblem p;
  p.reason = reason;
  p.index = index;
  return p;
}

SetProblem Problem(set::Problem problem, std::uint32_t index = 0) { return Problem(set::ProblemName(problem), index); }

std::uint64_t LoadLe(const unsigned char* p, std::size_t width) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < width; ++i) value |= std::uint64_t{p[i]} << (kBitsPerByte * i);
  return value;
}

void StoreLe(std::uint8_t* p, std::size_t width, std::uint64_t value) {
  for (std::size_t i = 0; i < width; ++i) p[i] = static_cast<std::uint8_t>(value >> (kBitsPerByte * i));
}

std::uint32_t LoadU32(const unsigned char* p) { return static_cast<std::uint32_t>(LoadLe(p, sizeof(std::uint32_t))); }
std::uint64_t LoadU64(const unsigned char* p) { return LoadLe(p, sizeof(std::uint64_t)); }

bool ReadExact(nevr_cargo::Env& env, std::int64_t handle, std::uint64_t offset, unsigned char* out, std::size_t length) {
  while (length > 0) {
    std::size_t got = 0;
    if (!env.ReadBlob(handle, offset, out, length, &got) || got == 0 || got > length) return false;
    out += got;
    offset += got;
    length -= got;
  }
  return true;
}

}  // namespace

std::uint64_t SetTableMemoryBytes(std::uint64_t records, std::uint64_t chunks) noexcept {
  return records * (set::kRecordBytes + sizeof(set::Record)) + chunks * (set::kChunkBytes + sizeof(set::Chunk));
}

SetProblem ReadSetTables(nevr_cargo::Env& env, std::int64_t handle, std::uint64_t size, std::uint64_t table_bytes,
                         SetTables* out) {
  *out = SetTables();
  unsigned char head[set::kHeaderBytes];
  if (size < set::kHeaderBytes) return Problem(set::Problem::kTooSmall);
  if (!ReadExact(env, handle, 0, head, sizeof(head))) return Problem("read");
  if (std::memcmp(head, set::kMagic, sizeof(set::kMagic)) != 0) return Problem(set::Problem::kMagic);
  if (LoadU32(head + kHeaderSizeAt) != set::kHeaderBytes) return Problem(set::Problem::kHeader);
  const std::uint64_t records = LoadU32(head + kRecordCountAt), chunks = LoadU32(head + kChunkCountAt);
  const std::uint64_t overrides = LoadU32(head + kOverrideCountAt);
  const std::uint64_t record_table = LoadU64(head + kRecordTableAt), chunk_table = LoadU64(head + kChunkTableAt);
  const std::uint64_t override_table = LoadU64(head + kOverrideTableAt);
  if (records == 0) return Problem(set::Problem::kNoRecords);
  if (!set::detail::Inside(record_table, records * set::kRecordBytes, size) ||
      !set::detail::Inside(chunk_table, chunks * set::kChunkBytes, size) ||
      !set::detail::Inside(override_table, overrides * set::kOverrideBytes, size)) {
    return Problem(set::Problem::kTableRange);
  }
  // Both tables and their parsed form are held at once: the device's share bounds them before anything is allocated.
  if (SetTableMemoryBytes(records, chunks) > table_bytes) return Problem("no_memory");

  SetTables tables;
  tables.override_count = static_cast<std::uint32_t>(overrides);
  std::vector<unsigned char> bytes(static_cast<std::size_t>(chunks * set::kChunkBytes));
  if (!bytes.empty() && !ReadExact(env, handle, chunk_table, bytes.data(), bytes.size())) return Problem("read");
  tables.chunks.resize(static_cast<std::size_t>(chunks));
  for (std::size_t i = 0; i < tables.chunks.size(); ++i) {
    const unsigned char* at = bytes.data() + i * set::kChunkBytes;
    set::Chunk& c = tables.chunks[i];
    c.reserved = LoadU32(at);
    c.offset = LoadU32(at + kChunkOffsetAt);
    c.compressed = LoadU32(at + kChunkCompressedAt);
    c.decompressed = LoadU32(at + kChunkDecompressedAt);
    const auto index = static_cast<std::uint32_t>(i);
    if (c.reserved != 0) return Problem(set::Problem::kChunkReserved, index);
    if (c.compressed == 0 || !set::detail::Inside(c.offset, c.compressed, size)) return Problem(set::Problem::kChunkRange, index);
  }
  bytes.assign(static_cast<std::size_t>(records * set::kRecordBytes), 0);
  if (!ReadExact(env, handle, record_table, bytes.data(), bytes.size())) return Problem("read");
  tables.records.resize(static_cast<std::size_t>(records));
  for (std::size_t i = 0; i < tables.records.size(); ++i) {
    const unsigned char* at = bytes.data() + i * set::kRecordBytes;
    set::Record& r = tables.records[i];
    r.type = LoadU64(at);
    r.name = LoadU64(at + kRecordNameAt);
    r.chunk = LoadU32(at + kRecordChunkAt);
    r.offset_in_chunk = LoadU32(at + kRecordOffsetAt);
    r.size = LoadU32(at + kRecordSizeAt);
    r.alignment = LoadU32(at + kRecordAlignmentAt);
    if (r.chunk >= chunks) return Problem(set::Problem::kRecordChunk, static_cast<std::uint32_t>(i));
  }
  *out = std::move(tables);
  return SetProblem();
}

SetProblem CheckSetFrames(nevr_cargo::Env& env, std::int64_t handle, const SetTables& tables) {
  for (std::size_t i = 0; i < tables.chunks.size(); ++i) {
    const set::Chunk& c = tables.chunks[i];
    unsigned char head[nevr_cargo::kMaxFrameHeaderBytes];
    const std::size_t length = static_cast<std::size_t>(std::min<std::uint64_t>(sizeof(head), c.compressed));
    if (!ReadExact(env, handle, c.offset, head, length)) return Problem("read", static_cast<std::uint32_t>(i));
    const nevr_cargo::Rule rule = nevr_cargo::CheckFrame(head, length, c.compressed, c.decompressed);
    if (rule != nevr_cargo::Rule::kNone) {
      SetProblem p;
      p.reason = nevr_cargo::RuleName(rule);
      p.index = static_cast<std::uint32_t>(i);
      return p;
    }
  }
  return SetProblem();
}

std::vector<std::uint8_t> SetHeaderAndTables(const std::vector<set::Record>& records, const std::vector<set::Chunk>& chunks) {
  const std::uint64_t record_table = set::kHeaderBytes;
  const std::uint64_t chunk_table = record_table + records.size() * set::kRecordBytes;
  const std::uint64_t tables_end = chunk_table + chunks.size() * set::kChunkBytes;
  std::vector<std::uint8_t> out(static_cast<std::size_t>(tables_end), 0);
  std::memcpy(out.data(), set::kMagic, sizeof(set::kMagic));
  StoreLe(out.data() + kHeaderSizeAt, sizeof(std::uint32_t), set::kHeaderBytes);
  StoreLe(out.data() + kRecordCountAt, sizeof(std::uint32_t), records.size());
  StoreLe(out.data() + kChunkCountAt, sizeof(std::uint32_t), chunks.size());
  StoreLe(out.data() + kRecordTableAt, sizeof(std::uint64_t), record_table);
  StoreLe(out.data() + kChunkTableAt, sizeof(std::uint64_t), chunk_table);
  StoreLe(out.data() + kOverrideTableAt, sizeof(std::uint64_t), tables_end);  // no overrides: an empty table at the end
  for (std::size_t i = 0; i < records.size(); ++i) {
    std::uint8_t* p = out.data() + record_table + i * set::kRecordBytes;
    StoreLe(p, sizeof(std::uint64_t), records[i].type);
    StoreLe(p + kRecordNameAt, sizeof(std::uint64_t), records[i].name);
    StoreLe(p + kRecordChunkAt, sizeof(std::uint32_t), records[i].chunk);
    StoreLe(p + kRecordOffsetAt, sizeof(std::uint32_t), records[i].offset_in_chunk);
    StoreLe(p + kRecordSizeAt, sizeof(std::uint32_t), records[i].size);
    StoreLe(p + kRecordAlignmentAt, sizeof(std::uint32_t), records[i].alignment);
  }
  for (std::size_t i = 0; i < chunks.size(); ++i) {
    std::uint8_t* p = out.data() + chunk_table + i * set::kChunkBytes;
    StoreLe(p, sizeof(std::uint32_t), chunks[i].reserved);
    StoreLe(p + kChunkOffsetAt, sizeof(std::uint32_t), chunks[i].offset);
    StoreLe(p + kChunkCompressedAt, sizeof(std::uint32_t), chunks[i].compressed);
    StoreLe(p + kChunkDecompressedAt, sizeof(std::uint32_t), chunks[i].decompressed);
  }
  return out;
}

SetProblem CheckSetFile(nevr_cargo::Env& env, const std::string& path, std::uint64_t table_bytes) {
  std::int64_t handle = 0;
  std::uint64_t size = 0;
  if (!env.OpenBlob(path, &handle, &size)) return Problem("open");
  SetTables tables;
  SetProblem problem = ReadSetTables(env, handle, size, table_bytes, &tables);
  if (problem.reason.empty()) problem = CheckSetFrames(env, handle, tables);
  env.CloseBlob(handle);
  return problem;
}

}  // namespace nevr_maps
