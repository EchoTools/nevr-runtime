/* The PC's nevr_maps::MapFs (docs/adr/0048-custom-maps-channel.md, "Consequences": the channel's file seams; posix_map_fs.h is the
 * Quest's half). Win32 files, read through the same handles the PC's nevr_cdn::WindowsEnv opens, so the map channel's core runs on
 * the PC as it runs on the Quest.
 *
 * A held file is opened read-only with FILE_SHARE_READ (WindowsEnv::OpenBlob), so while it is open nothing can write it, delete it or
 * rename another file over it: the file the engine reads is the file that was hashed.
 *
 * StampOf: `volume` and `id_*` come from FILE_ID_INFO where the file system has it (NTFS, ReFS), else from the 32-bit serial and
 * 64-bit index; `last_write` is a FILETIME. FinalPathOf: normalized, with a drive letter and without the \\?\ prefix. RemoveTree is
 * true when the folder is gone (or was never there); it never follows a link inside the tree, it removes the link. RenameDir
 * refuses when `to` exists. Paths are absolute, UTF-8.
 */
#pragma once

#include <cstdint>
#include <memory>

#include "core/map_types.h"

namespace nevr_maps {

std::unique_ptr<MapFs> MakeWinMapFs();

}  // namespace nevr_maps
