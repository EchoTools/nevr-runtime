// The patched libr15's identity (ADR 0021 section 7): the canonical serialisation of the table and the build ID
// derived from it. The patcher writes the ID into the file's GNU build-ID note; the sentinel recomputes the same
// value from its compiled-in table and compares, so no generated constant can drift from the patcher's.
//
//   id = first 20 bytes of SHA-256( "NEVR-LIBR15-PATCH-1" || stock build-ID bytes || CanonicalTable(...) )
//
// Pure functions of their arguments (no clock, no environment, no file).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "quest/patch/libr15_patch_table.h"

namespace nevr_quest::patch {

inline constexpr const char* kBuildIdDomain = "NEVR-LIBR15-PATCH-1";
inline constexpr std::size_t kBuildIdBytes = 20;
using BuildIdBytes = std::array<std::uint8_t, kBuildIdBytes>;

/// 40 lowercase hex characters to 20 bytes. False for any other string.
bool ParseBuildIdHex(const std::string& hex, BuildIdBytes* out);
std::string BuildIdHex(const BuildIdBytes& id);

/// The serialisation hashed into the ID: one text line per fact, in a fixed order, every number as 0x-hex.
///   layout|bias=<kDisasmBias>|slot_area=<slotArea>|slot_bytes=<kSlotBytes>
///   row|<id>|<kind>|slot=<n>|<disasmVa>:<w0>,<w1>,<w2>|...              (one per public row, table order)
/// and, only when there are extra rows, the text line `extra|<count>` followed by one fixed binary record per row,
/// sorted by disasmVa (ties by id), so the file's key order, spacing and row order do not matter (ADR section 11):
///   u8 id length | id bytes | u64le disasmVa | u8 word count | per word: u16le offset, 4 new bytes, 4 stock bytes
/// A row's handler and game-function names are not part of it: they change nothing in the file.
std::string CanonicalTable(const Row* rows, std::size_t rowCount, const std::vector<WordPatchRow>& extra,
                           std::uint64_t slotArea);

/// The derived ID for a stock build ID and a serialisation.
BuildIdBytes DeriveBuildId(const BuildIdBytes& stockBuildId, const std::string& canonicalTable);

/// The patched ID of the compiled-in public table (kRows, kLibR15StockBuildId, kSlotAreaVaddr) plus `extra`
/// (ADR section 11), as 40 hex characters. Empty extra: the public release's ID.
std::string PatchedBuildId(const std::vector<WordPatchRow>& extra = {});

}  // namespace nevr_quest::patch
