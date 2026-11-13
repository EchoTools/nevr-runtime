/* The limits of one cargo run, computed from the machine and never written as constants (docs/adr/0045-cdn-texture-delivery.md).
 *
 * Two kinds of number are named in this header, and only the first is a limit:
 *   - format facts: what the engine's own manifest layout can express. A chunk's file offset and sizes are 32-bit words
 *     (resource_shadow.h), so a store file is at most 4 GiB and an object's sizes fit 32 bits; an alignment is a power of two of at
 *     most 4096 (docs/reference/plugin-resources.md, the game's own manifests use 4, 16, 2048 and 4096). They are properties of the
 *     engine, the same on every device, and they are the only constants here.
 *   - shares: what fraction of a resource the cargo code is allowed to take. A share is a policy ratio, not a size: the size comes
 *     out of the device's number (memory, free space, descriptors) and moves with it.
 *
 * A fact the device did not give makes the limits that need it "not ok", with the name of the fact: the run then does nothing and says
 * why, so a bad or missing value never turns into a guess.
 */
#pragma once

#include <cstdint>

#include "core/cargo_env.h"
#include "core/cargo_format.h"

namespace nevr_cargo {

// Format facts.
inline constexpr std::uint64_t kFormatMaxStoreBytes = 0xFFFFFFFFull;   // a chunk's file offset is a u32 (resource_shadow.h Chunk)
inline constexpr std::uint64_t kFormatMaxObjectBytes = 0xFFFFFFFFull;  // record size and chunk sizes are u32 words
inline constexpr std::uint32_t kFormatMaxAlignment = 4096;
// The blob index's layout is cargo_format.h's: a header, then one fixed record per stored object.
constexpr std::uint64_t kIndexBytesFor(std::uint64_t objects) noexcept { return kIndexHeaderBytes + objects * kIndexRecordBytes; }

// Shares. Each is "1 part in N" of the named device number.
inline constexpr std::uint64_t kMemoryShareForManifest = 8;  // reading a manifest (its frame, its text and its parsed form) takes at most 1/N of free memory ...
inline constexpr std::uint64_t kManifestBufferCopies = 2;   // reading a manifest holds its frame and its decoded text, one copy each ...
inline constexpr std::uint64_t kJsonExpansion = 6;           // ... and the parsed form is this many times the text: measured 3.85 on the 11.5 MB all-assets
                                                              // manifest (nlohmann::ordered_json, mallinfo2, cargo_limits_test measures it again)
inline constexpr std::uint64_t kMemoryShareForEntries = 4;    // the run's tables (entries, records, chunks) take at most 1/N of free memory
inline constexpr std::uint64_t kMemoryShareForObject = 2;     // one object, decompressed by the engine, takes at most 1/N of free memory
inline constexpr std::uint64_t kStorageShareKept = 8;         // 1/N of the free space is left free
inline constexpr std::uint64_t kDescriptorShareForStore = 2;  // 1/N of the free descriptors may hold store files

struct Limits {
  bool ok = false;
  const char* reason = "";  // when !ok: the device fact that is missing or leaves no room ("no_memory_fact", "no_free_space", ...)

  std::uint64_t manifest_text_bytes = 0;  // the most manifest text a client reads
  std::uint64_t max_entries = 0;          // the most entries a manifest may list
  std::uint64_t max_object_bytes = 0;     // the most decompressed bytes one object may state
  std::uint64_t store_file_bytes = 0;     // the most bytes one store file holds (never above the format's 4 GiB)
  std::uint64_t max_store_files = 0;      // the most store files held open (and so the most store files there are)
  std::uint64_t storage_budget = 0;       // the most bytes a plan may add to the store
  std::uint64_t index_bytes = 0;          // the most bytes of blob index a client reads
};

// `entry_bytes` is what one manifest entry costs the run in memory (ManifestEntryMemoryBytes()); the caller states it so this header
// need not know the manifest.
Limits ComputeLimits(const DeviceFacts& facts, std::uint64_t entry_bytes);

}  // namespace nevr_cargo
