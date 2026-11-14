/* The Quest's stock source (docs/adr/0048-custom-maps-channel.md, the recipes; core/stock_source.h is the shared half): the player's
 * own stock, read for the recipe rebuild.
 *
 * Resources come from the `_data` tree the game itself reads (sentinel/data_root.h: the OBB skip serves that tree in place of the
 * encrypted OBB, which is never opened here): every `<root>/_data/<folder>/rad15/android/manifests` folder, sorted, and in each the
 * pinned manifest id first (data/plan.h kManifestPath), then the other 16-hex manifest names. When a bundled content pack replaced
 * a manifest, the kept `<id>.stock` is read in its place (docs/adr/0029-quest-bundled-content-pack.md), so a recipe always rebuilds
 * from stock records. The first manifest that holds the key wins; the package file is `<folder>/rad15/android/packages/<id>_<n>`.
 *
 * Scripts come from the APK: `<native library dir>/lib<hash16>.so` when the installer extracted it, else the APK entry
 * `lib/arm64-v8a/lib<hash16>.so`. This is the Quest's own part: a script is a library inside the APK there, a DLL in the install on
 * the PC.
 *
 * Every file is opened O_RDONLY and closed after the read, and its stamp is the Quest MapFs's (quest/cargo/posix_map_fs.h): nothing
 * is held and nothing is written. Built without exceptions.
 */
#pragma once

#include <memory>
#include <string>

#include "core/map_derive.h"

namespace nevr_quest::maps {

// The data root the game reads (obb_skip::CurrentRoot in production); asked at each read, so a root resolved after the source was
// made is still found. Null or empty: no stock resource is found.
using DataRootFn = const char* (*)() noexcept;

// `native_lib_dir` is the directory the sentinel was loaded from; `apk_path` is base.apk (empty: none).
std::unique_ptr<nevr_maps::StockSource> MakePosixStockSource(DataRootFn data_root, std::string native_lib_dir,
                                                             std::string apk_path);

}  // namespace nevr_quest::maps
