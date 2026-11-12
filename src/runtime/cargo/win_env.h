/* The PC's nevr_cargo::Env and nevr_maps::MapFs (docs/adr/0047): Win32 files, libcurl for the network, BCrypt SHA-256, and the
 * runtime log for events, so the platform-neutral cargo and map-channel code runs on the PC as the cargo code runs on the Quest.
 *
 * A blob or held file is opened read-only with FILE_SHARE_READ and its handle is the HANDLE: while it is open nothing can write it,
 * delete it or rename another file over it, so the file the engine reads is the file that was hashed.
 *
 * Every call blocks (disk or network); callers run on a worker thread, never the game thread.
 */
#pragma once

#include <memory>

#include "core/cargo_env.h"
#include "core/map_types.h"

namespace nevr_cargo {

// Get's transport bounds (CDN_DESIGN PC1, Rev 2): a connect that takes longer, or a transfer that moves under the floor for
// kLowSpeedSeconds, ends with CURLE_OPERATION_TIMEDOUT. A thread inside Get therefore leaves it within about these bounds
// even when the server stalls, which the maps channel's StopFetch relies on.
inline constexpr long kConnectTimeoutSeconds = 10;
inline constexpr long kLowSpeedBytesPerSecond = 1024;
inline constexpr long kLowSpeedSeconds = 15;

// `event_prefix` starts every event line, "[NEVR.MAPS]" for the map channel. Get streams the body to `on_body` as it arrives and
// ends the transfer when `on_body` returns false; the caller's byte cap is enforced there.
std::unique_ptr<Env> MakeWinEnv(const char* event_prefix);

}  // namespace nevr_cargo

namespace nevr_maps {

// StampOf: `volume` and `id_*` come from FILE_ID_INFO where the file system has it (NTFS, ReFS), else from the 32-bit serial and
// 64-bit index; `last_write` is a FILETIME. FinalPathOf: normalized, with a drive letter and without the \\?\ prefix.
// RemoveTree is true when the folder is gone (or was never there); it never follows a link inside the tree, it removes the link.
// RenameDir refuses when `to` exists.
std::unique_ptr<MapFs> MakeWinMapFs();

}  // namespace nevr_maps
