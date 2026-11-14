/* The PC's stock source (docs/adr/0048-custom-maps-channel.md, the recipes; core/stock_source.h is the shared half): the player's
 * own install, read for the recipe rebuild. Resources come from the boot package sets' manifests (server/installed_levels.h
 * BootPackageSets) in every <install>\_data\<version>\rad15\win10\manifests folder (FindManifestDirs), the first that holds the key
 * winning; scripts from <install>\bin\win10\scripts\<hash16>.dll. Nothing outside the install is read.
 *
 * A stock file is opened read-only with full sharing and closed after the read, and its stamp is the PC MapFs's
 * (runtime/cargo/win_env.h): nothing is held and nothing is written.
 */
#pragma once

#include <memory>
#include <string>

#include "core/map_derive.h"

namespace nevr_maps {

// `install_root` is the folder that holds bin\win10\echovr.exe and _data.
std::unique_ptr<StockSource> MakeWinStockSource(const std::wstring& install_root);

}  // namespace nevr_maps
