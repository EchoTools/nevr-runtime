/* The one reader of a map's resource-set file on disk (docs/adr/0047-custom-maps-channel.md, the set layout of
 * sdk/include/nevr/resource_set.h). The fetch reads a set's tables to check every frame header after the set's pieces matched
 * their pins, and the recipe rebuild reads them to merge its records into the set. A set can be far larger than memory, so the
 * file is read through an open blob (nevr_cargo::Env::ReadBlob): the header, the record and chunk tables, and the first bytes of
 * each frame, never a frame's body.
 *
 * Each frame header is judged by the cargo validator (core/cargo_validate.h, CheckFrame): the bytes start a zstd frame whose
 * header states the chunk's decompressed size. The registry's own parse runs again at the mount; this check is what the cache
 * records as `frames_checked`.
 *
 * Memory (docs/adr/0045-cdn-texture-delivery.md decision 8): the tables are held whole, so the caller states the most bytes they
 * may take (`table_bytes`, a share of the device's memory) and a set whose tables need more is refused as `no_memory`.
 *
 * Reasons: the set format's own problem names (nevr_resource_set::ProblemName: too_small, magic, header, no_records,
 * table_range, chunk_reserved, chunk_range, record_chunk), no_memory, open and read (the file could not be opened or read), and
 * from the frame check the cargo validator's rule name (frame_magic, content_size_missing, content_size_mismatch), each with the
 * chunk it names.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/cargo_env.h"
#include "nevr/resource_set.h"

namespace nevr_maps {

struct SetTables {
  std::uint32_t override_count = 0;
  std::vector<nevr_resource_set::Record> records;
  std::vector<nevr_resource_set::Chunk> chunks;
};

struct SetProblem {
  std::string reason;  // "" when the set reads
  std::uint32_t index = 0;  // the record or chunk the reason names
};

// The bytes the tables of a set with these counts take in memory.
std::uint64_t SetTableMemoryBytes(std::uint64_t records, std::uint64_t chunks) noexcept;

// Reads the header and the record and chunk tables of the set in the open blob `handle` of `size` bytes. Each chunk lies inside
// the file and its reserved word is 0; each record names a chunk of the table. Frames are not read.
SetProblem ReadSetTables(nevr_cargo::Env& env, std::int64_t handle, std::uint64_t size, std::uint64_t table_bytes,
                         SetTables* out);

// Reads the first bytes of every chunk's frame and holds them to the chunk's sizes with nevr_cargo::CheckFrame.
SetProblem CheckSetFrames(nevr_cargo::Env& env, std::int64_t handle, const SetTables& tables);

// The header and the two tables of a set with these records and chunks and no overrides, as the bytes that start its file: the
// record table follows the header, the chunk table follows it, and the frames start where the chunk table ends.
std::vector<std::uint8_t> SetHeaderAndTables(const std::vector<nevr_resource_set::Record>& records,
                                             const std::vector<nevr_resource_set::Chunk>& chunks);

// Both, over the file at `path`: the open, the tables and every frame header. The blob is closed again.
SetProblem CheckSetFile(nevr_cargo::Env& env, const std::string& path, std::uint64_t table_bytes);

}  // namespace nevr_maps
