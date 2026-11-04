// quest_obb_extract: the store OBB in, the pinned stock-data tree out (docs/adr/0042-quest-store-variant-built-from-store-apk-and-obb.md).
//   quest_obb_extract --obb <file> --pins <file> --cache-root <dir> [--rehash]
// Writes <cache root>/<obb sha256>/ and one JSON line per step on standard output. Exit 0: the tree is there and matches the pins
// (extracted now, or a cache hit). Exit 1: refused or failed, the last line names the code. Exit 2: the command line is wrong.

#include <cstdio>
#include <cstring>
#include <string>

#include "quest/obb/build_log.h"
#include "quest/obb/stock_data.h"

namespace {
constexpr char kUsage[] = "usage: quest_obb_extract --obb <store OBB> --pins <pin file> --cache-root <directory> [--rehash]";
}

int main(int argc, char** argv) {
  using namespace nevr_quest::obb;
  BuildLog log = BuildLog::Stdout();
  StockDataOptions options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&](std::string* into) {
      if (i + 1 >= argc) return false;
      *into = argv[++i];
      return true;
    };
    bool ok = true;
    if (arg == "--obb") {
      ok = value(&options.obb_path);
    } else if (arg == "--pins") {
      ok = value(&options.pins_path);
    } else if (arg == "--cache-root") {
      ok = value(&options.cache_root);
    } else if (arg == "--rehash") {
      options.rehash_cache = true;
    } else if (arg == "--help") {
      std::printf("%s\n", kUsage);
      return 0;
    } else {
      ok = false;
    }
    if (!ok) {
      log.Error("refused", {{"code", "usage"}, {"detail", "unknown or incomplete argument '" + arg + "'; " + kUsage}});
      return 2;
    }
  }
  if (options.obb_path.empty() || options.pins_path.empty() || options.cache_root.empty()) {
    log.Error("refused", {{"code", "usage"}, {"detail", std::string("--obb, --pins and --cache-root are all required; ") + kUsage}});
    return 2;
  }
  StockDataResult result;
  return BuildStockData(options, log, &result).ok ? 0 : 1;
}
