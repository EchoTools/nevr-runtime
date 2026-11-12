#pragma once

// The PC's StockSource (docs/adr/0047; CDN_DESIGN PC10): stock resources and scripts read from the player's own install, for the
// recipe rebuild (core/map_derive.h). Resources come from the boot package sets' manifests
// (docs/reference/package-manifest-format.md) in every folder server/installed_levels.h FindManifestDirs finds, the first that
// holds the key winning; scripts from <install>\bin\win10\scripts\<hash16>.dll.
//
// Every stock file is opened read-only with full sharing and closed after the read; nothing is held and nothing is written. A
// parsed manifest is kept while its stamp is unchanged. StampsCurrent only opens and stamps, so the boot thread may call it, and
// it shares no state with the reads.

#include <memory>
#include <string>

#include "core/map_derive.h"

namespace nevr_stock_source {

// `install_root` is the folder that holds bin\win10\echovr.exe and _data.
std::unique_ptr<nevr_maps::StockSource> MakeWinStockSource(const std::wstring& install_root);

}  // namespace nevr_stock_source
