/* Skip the OBB mount (issue #319: "Echo VR is not responding" at every Quest launch).
 *
 * What the game does (pinned libr15; pinned_targets.h has the addresses and the call contract):
 * CSysFile::Init, running on the android_main thread before it services the looper, calls
 * AStorageManager_mountObb for the encrypted OBB and polls for the callback for about 30 s
 * (20 s of 100 ms polls, then 51 retries 200 ms apart). Horizon OS refuses keyed OBB mounts and never calls
 * the callback, so every launch waits the whole time and the system shows the not-responding dialog. After
 * the wait the game uses CSysInfo::GetDataRootDir's fallback, /storage/emulated/0/readyatdawn, which is
 * where its assets are read from.
 *
 * Two GOT hooks in libr15.so, both lock-free, both on the caller's thread (android_main):
 *   - AStorageManager_mountObb: when the game passes a callback, report AOBB_STATE_MOUNTED to it at once
 *     (cb(filename, 1, data), synchronously, the original is not called). The game's own callback then
 *     asks for the mounted path, records it and sets its completion flag, exactly as for a real mount.
 *   - AStorageManager_getMountedObbPath: after that skip, return the fallback data root, so the game
 *     records the root it would have used after the timeout.
 * The path hook is installed first and the mount hook only when it took: a mount skipped without the path
 * would record an empty path and the game would retry for the same ~30 s.
 *
 * A call without a callback goes to the original. A path request before any skip goes to the original.
 *
 * It never logs on the game's call path. Counters (hook_report.h), registered by RegisterCounters():
 *   obb_mount_skipped              the mount request was answered with "mounted"
 *   obb_path_served                the fallback data root was returned for a path request
 *   obb_mount_thunk_faults / obb_path_thunk_faults   a thunk had no original
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include "pinned_targets.h"

namespace nevr_quest::obb_skip {

using MountThunk = sentinel::pinned::LibR15MountObbThunk;
using PathThunk = sentinel::pinned::LibR15GetMountedObbPathThunk;

inline constexpr int kCounterCount = 4;

// Registers the counters. Call before sentinel::StartReporter. Returns false, having logged one line
// {"event":"obb_skip_counters","result":"refused",...}, when any was refused.
bool RegisterCounters() noexcept;

// Arms both handlers and installs the thunks in libr15.so (path first). One line
// {"event":"obb_skip_install",...} says what was installed. True only when both slots hold their thunks.
bool Install() noexcept;

// Install() when `countersRegistered`; otherwise logs the skip and installs nothing.
bool InstallIfCounted(bool countersRegistered) noexcept;

// Test support: arms both handlers without touching a GOT slot, and clears the "skipped" state.
void ArmForTest() noexcept;
void ResetForTest() noexcept;

struct Counts {
  unsigned long long mountSkipped, pathServed;
};
Counts CurrentCounts() noexcept;

}  // namespace nevr_quest::obb_skip
