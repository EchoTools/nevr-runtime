/* The Quest's StockSource (CDN_DESIGN Rev 3 §10.4 Q2, §5.4): the player's own stock, read for PC10's recipe rebuild
 * (core/map_derive.h).
 *
 * Resources come from the `_data` tree the game itself reads (sentinel/data_root.h: the OBB skip serves that tree in place of
 * the encrypted OBB, which is never opened here): every `<root>/_data/<folder>/rad15/android/manifests` folder, sorted, and in
 * each the pinned manifest id first (data/plan.h kManifestPath), then the other 16-hex manifest names. When a bundled content
 * pack replaced a manifest, the kept `<id>.stock` is read in its place, so a recipe always rebuilds from stock records. The
 * first manifest that holds the key wins; the package file is `<folder>/rad15/android/packages/<id>_<n>`.
 *
 * Scripts come from the APK: `<native library dir>/lib<hash16>.so` when the installer extracted it, else the APK entry
 * `lib/arm64-v8a/lib<hash16>.so`.
 *
 * Every file is opened O_RDONLY and closed after the read: nothing is held and nothing is written. A parsed manifest is kept while
 * its stamp is unchanged. StampsCurrent only stats, so the boot thread may call it. Built without exceptions.
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
