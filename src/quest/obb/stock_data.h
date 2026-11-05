// The stock-data step (docs/adr/0042): the store OBB in, `<cache root>/<obb sha256>/_data/...` out.
//
// 1. read the pins; hash the OBB; refuse an OBB the pins do not name (before any decryption);
// 2. a cache directory for that digest that holds the pinned tree is a hit: nothing else runs;
// 3. otherwise read the footer, derive the key, read the FAT16 image through the decrypting source and write the tree
//    into `<digest>.partial`, hashing each file as it is written;
// 4. check the written tree against the pins; only a tree that passes is renamed to `<digest>`.
#pragma once

#include <string>
#include <vector>

#include "quest/obb/build_log.h"
#include "quest/obb/obb_key.h"
#include "quest/obb/status.h"
#include "quest/obb/stock_pins.h"

namespace nevr_quest::obb {

struct StockDataOptions {
  std::string obb_path;
  std::string pins_path;
  std::string cache_root;
  std::string key_string = kGameObbKeyString;
  // Hash every cached file again on a hit, instead of trusting the sizes and the digests the cache's manifest recorded.
  bool rehash_cache = false;
};

struct StockDataResult {
  std::string obb_sha256;
  std::string directory;  // <cache root>/<obb sha256>
  bool cache_hit = false;
  std::vector<MeasuredFile> files;
};

constexpr char kStockDataManifestName[] = "stock-data.json";

Status BuildStockData(const StockDataOptions& options, BuildLog& log, StockDataResult* result);

// The SHA-256 and size of a whole file.
Status HashFile(const std::string& path, std::string* sha256, std::uint64_t* size);

}  // namespace nevr_quest::obb
