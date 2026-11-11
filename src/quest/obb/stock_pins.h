// The pins of the stock game data (tools/quest_stock_data.pin): the store OBB the build accepts and the exact tree it must
// hold. The extraction is refused when the OBB is another file, or the tree has a file the pins do not list, lacks one they
// list, or holds one whose size or SHA-256 differs.
//
// The file is text, one record per line, `#` starts a comment:
//   obb_sha256 <64 hex>
//   obb_size <bytes>
//   file <64 hex> <bytes> <path relative to the tree, '/'-separated>
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "quest/obb/status.h"

namespace nevr_quest::obb {

struct PinnedFile {
  std::string path;
  std::string sha256;
  std::uint64_t size = 0;
};

struct StockPins {
  std::string obb_sha256;
  std::uint64_t obb_size = 0;
  std::vector<PinnedFile> files;
};

struct MeasuredFile {
  std::string path;
  std::string sha256;
  std::uint64_t size = 0;
};

// Codes: pin_syntax (the line number is in the detail), pin_missing_obb, pin_no_files, pin_duplicate_file, pin_unsafe_path.
Status ParseStockPins(std::string_view text, StockPins* out);
// Codes: pin_obb_mismatch.
Status CheckObbAgainstPins(const StockPins& pins, const std::string& obb_sha256, std::uint64_t obb_size);
// Codes: pin_unlisted_file, pin_missing_file, pin_size_mismatch, pin_digest_mismatch. The detail names the path, the pinned
// value and the measured one.
Status CheckTreeAgainstPins(const StockPins& pins, const std::vector<MeasuredFile>& measured);

}  // namespace nevr_quest::obb
