/* The PC's nevr_cargo::Env and nevr_maps::MapFs (docs/adr/0048-custom-maps-channel.md, "Consequences": the channel's two file
 * seams; docs/adr/0045-cdn-texture-delivery.md decision 8: the device's facts): Win32 files, libcurl for the network, the portable
 * SHA-256 (quest/data/sha256.h, the one the Quest's PosixEnv uses) and the runtime log for events, so the platform-free cargo and
 * map-channel code runs on the PC as it runs on the Quest.
 *
 * A blob or held file is opened read-only with FILE_SHARE_READ and its handle is the HANDLE: while it is open nothing can write it,
 * delete it or rename another file over it, so the file the engine reads is the file that was hashed.
 *
 * QueryDevice: free space is GetDiskFreeSpaceExW's bytes free to this caller on the volume of the directory; memory is
 * nevr_device::MemoryAvailable (GlobalMemoryStatusEx's available physical memory on Windows). A Win32 process has no soft limit on
 * open files; what bounds the handles it may hold is the kernel's per-process handle limit, kProcessHandleLimit below, and the
 * handles it holds now are GetProcessHandleCount's. Those two stand for fd_soft_limit and fds_open, so the descriptor shares of
 * cargo_limits.h and map_run.h are shares of the handles this process may still open. Wine answers GetProcessHandleCount with 0
 * whatever is open (measured on Wine 11.21); the 0 is taken as given, because a game's few thousand handles are under 0.1% of the
 * limit, so the share it moves is that small. A fact a call does not give is left absent, and the limits that need it stop with
 * its name (cargo_limits.h).
 *
 * Every call blocks (disk or network); callers run on a worker thread, never the game's thread.
 */
#pragma once

#include <cstdint>
#include <memory>

#include "core/cargo_env.h"
#include "core/map_types.h"

namespace nevr_cargo {

// Get's transport bounds: a connect that takes longer, or a transfer that moves under the floor for kLowSpeedSeconds, ends with
// CURLE_OPERATION_TIMEDOUT. A thread inside Get therefore leaves it within about these bounds even when the server stalls, which a
// caller that stops a fetch relies on. No whole-transfer timeout: one piece may be a large frame on a slow link.
inline constexpr long kConnectTimeoutSeconds = 10;
inline constexpr long kLowSpeedBytesPerSecond = 1024;
inline constexpr long kLowSpeedSeconds = 15;
// The redirects Get follows; every hop after the first must be https.
inline constexpr long kMaxRedirects = 5;

// The most kernel handles one Windows process may hold: 2^24 (Microsoft Learn, "Kernel Objects": "the limit on kernel handles is
// 2^24 per process"). A Win32 property, the same on every machine; QueryDevice reports it as fd_soft_limit.
inline constexpr std::uint64_t kProcessHandleLimit = std::uint64_t{1} << 24;

// `event_tag` starts every event line, "[NEVR.MAPS]" for the map channel. Get streams the body to `on_body` as it arrives, with the
// status it came with (200, and 206 for a range answer), and ends the transfer when `on_body` returns false.
std::unique_ptr<Env> MakeWinEnv(const char* event_tag);

}  // namespace nevr_cargo

namespace nevr_maps {

// StampOf: `volume` and `id_*` come from FILE_ID_INFO where the file system has it (NTFS, ReFS), else from the 32-bit serial and
// 64-bit index; `last_write` is a FILETIME. FinalPathOf: normalized, with a drive letter and without the \\?\ prefix. RemoveTree is
// true when the folder is gone (or was never there); it never follows a link inside the tree, it removes the link. RenameDir
// refuses when `to` exists.
std::unique_ptr<MapFs> MakeWinMapFs();

}  // namespace nevr_maps
