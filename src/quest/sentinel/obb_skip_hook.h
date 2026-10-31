/* Skip the OBB mount (issue #319: "Echo VR is not responding" at every Quest launch).
 *
 * What the game does (pinned libr15; pinned_targets.h has the addresses and the call contract):
 * CSysFile::Init, running on the android_main thread before it services the looper, calls
 * AStorageManager_mountObb for the encrypted OBB and polls for the callback for about 30 s
 * (20 s of 100 ms polls, then 51 retries 200 ms apart). Horizon OS refuses keyed OBB mounts and never calls
 * the callback, so every launch waits the whole time and the system shows the not-responding dialog. After
 * the wait the game uses CSysInfo::GetDataRootDir's fallback, /storage/emulated/0/readyatdawn, which is
 * where its assets are read from. The skip serves the first root that holds the game's `_data` directory
 * (data_root.h: the installer app's Android/media directory, then that fallback), chosen once by ResolveRoot.
 *
 * Three GOT hooks in libr15.so, all lock-free, all on the caller's thread (android_main):
 *   - AStorageManager_mountObb: when the game passes a callback, report AOBB_STATE_MOUNTED to it at once
 *     (cb(filename, 1, data), synchronously, the original is not called). The game's own callback then
 *     asks for the mounted path, records it and sets its completion flag, exactly as for a real mount.
 *   - AStorageManager_getMountedObbPath: after that skip, return the root ResolveRoot chose (the game's
 *     fallback root when neither candidate holds `_data`), so the game records the root it would have used
 *     after the timeout.
 *   - CSysInfo::GetDataRootDir (#866): with no OBB file CSysFile::Init returns at "OBB not found", before any mount, so
 *     the two hooks above never run and the engine's own question is the only one. This hook answers it with the root
 *     ResolveRoot chose: the media root, unless the legacy root alone holds the game's data (the same root a skipped mount
 *     serves). Once ResolveRoot has run, the file's own body is not consulted; before it, the original answers.
 *     By code reading (a scan of the pinned libr15, ADR 0013 Premise C; not observed on a headset), the function has two
 *     callers, both through this slot, and both only read the returned pointer.
 * The path hook is installed first and the mount hook only when it took: a mount skipped without the path
 * would record an empty path and the game would retry for the same ~30 s. The data-root hook is independent of both.
 *
 * A call without a callback goes to the original. A path request before any skip goes to the original.
 *
 * It never logs on the game's call path. Counters (hook_report.h), registered by RegisterCounters():
 *   obb_mount_skipped              the mount request was answered with "mounted"
 *   obb_path_served                the fallback data root was returned for a path request
 *   obb_mount_thunk_faults / obb_path_thunk_faults   a thunk had no original
 *   data_root_served               the engine's data-root question was answered with the root ResolveRoot chose
 *   data_root_thunk_faults         the data-root thunk had no original
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include "data_root.h"
#include "pinned_targets.h"

namespace nevr_quest::obb_skip {

using MountThunk = sentinel::pinned::LibR15MountObbThunk;
using PathThunk = sentinel::pinned::LibR15GetMountedObbPathThunk;
using DataRootThunk = sentinel::pinned::LibR15GetDataRootDirThunk;

inline constexpr int kCounterCount = 6;

// Registers the counters. Call before sentinel::StartReporter. Returns false, having logged one line
// {"event":"obb_skip_counters","result":"refused",...}, when any was refused.
bool RegisterCounters() noexcept;

// Arms the handlers and installs the thunks in libr15.so (path, mount, then the data root). One line
// {"event":"obb_skip_install",...} says what was installed; when the data-root slot took, one line
// {"event":"data_root_original",...} says which body the file's own GetDataRootDir has. True only when all three slots hold
// their thunks.
bool Install() noexcept;

// Install() with its outside world given: the filesystem ResolveRoot reads and the three slot installers. Install() is
// `InstallWith` over the real filesystem and the real GOT hooks; the host test drives it with fakes, because no libr15
// slot exists on the host (tools/tests/test_quest_data_root_wiring.py pins that Install() is exactly that call).
struct InstallEnv {
  data_root::ExistsFn dirExists;
  data_root::ExistsFn fileExists;
  sentinel::GotStatus (*installPath)() noexcept;
  sentinel::GotStatus (*installMount)() noexcept;
  sentinel::GotStatus (*installDataRoot)() noexcept;
};
bool InstallWith(const InstallEnv& env) noexcept;

// Install() when `countersRegistered`; otherwise logs the skip and installs nothing.
bool InstallIfCounted(bool countersRegistered) noexcept;

// Chooses the root the skipped mount serves (data_root.h) and logs one line {"event":"data_root",...} saying which won,
// or that neither holds the game's `_data`. `dirExists` answers for directories (the roots and `_data`), `fileExists` for
// the manifest file below `_data` (quest/data/plan.h, kManifestPath), which is what the data step takes as "this version of
// the data is complete". The one-argument form uses `exists` for both. Install() calls it with the real filesystem; call it
// before the game's first mount request. Until it runs, the game's own fallback root is served.
//
// The root the ENGINE is told to read with no OBB file (DataRootRedirect) is the root a skipped mount serves: the media
// root, unless the legacy root alone holds the game's data (#932, #965). A `_data` an older installer left behind, partial
// or of another build, does not take the engine away from complete data in the legacy root.
void ResolveRoot(data_root::ExistsFn exists) noexcept;
void ResolveRoot(data_root::ExistsFn dirExists, data_root::ExistsFn fileExists) noexcept;

// The real filesystem for ResolveRoot: true when `path` is an existing directory.
bool DirectoryExists(const char* path) noexcept;

// The real filesystem for the data step: true for an existing directory or a non-empty regular file. The step asks
// about the data root and `_data` (directories) and about the manifest under it (a regular file, 828213 bytes in the
// pinned zip), so DirectoryExists would call present data missing.
bool PathExists(const char* path) noexcept;

// The root the hook answers the engine's data-root question with: the root ResolveRoot chose, non-null whenever the data-root
// hook is installed (the slot took) and ResolveRoot has run; nullptr otherwise. The first-run data step treats it as the
// engine's root when no mount was skipped (data_wiring.cpp, #866).
const char* DataRootRedirect() noexcept;

// Test support: arms the handlers without touching a GOT slot, marks the data-root hook installed, and clears the
// "skipped" state.
void ArmForTest() noexcept;
void ResetForTest() noexcept;

// The root ResolveRoot chose last (a string literal, never freed): what the game's data-root question is answered with.
const char* CurrentRoot() noexcept;

struct Counts {
  unsigned long long mountSkipped, pathServed, dataRootServed;
};
Counts CurrentCounts() noexcept;

}  // namespace nevr_quest::obb_skip
