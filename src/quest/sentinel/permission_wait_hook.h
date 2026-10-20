/* The game waits for the storage permission it just asked for (#640).
 *
 * On the first launch of a fresh install the game asks for RECORD_AUDIO, READ_EXTERNAL_STORAGE and
 * WRITE_EXTERNAL_STORAGE and does not wait for the answer: CR15Game::Initialize calls CSysInfo::RequestPermissions
 * (a fire and forget JNI requestPermissions on the NativeActivity) and goes straight on into CncaGame::Initialize,
 * whose package-manifest read needs the storage permissions. The read fails 260 ms later, while the grant dialog has
 * not started yet; the game reports an initialisation error and finishes its own activity (pinned_targets.h,
 * LibR15RequestPermissions, has the call chain and the addresses).
 *
 * One GOT hook on libr15's RequestPermissions slot. The handler calls the original; on the FIRST call only, which is
 * the boot request (the later callers are script nodes that must not block), it then waits, on the same thread, for
 * the storage permissions in the request to be granted:
 *   - it checks each requested name that ends in "_EXTERNAL_STORAGE" with the game's own CSysInfo::CheckPermission
 *     (libr15 base + kCheckPermissionVaddr, used only after the module's build ID and the function's first four
 *     instructions match the pinned build);
 *   - while one is denied it sleeps kPollMs and checks again, for at most kTimeoutMs; on timeout it returns and the
 *     game goes on exactly as it does without the hook.
 * The wait does not pump the looper. Whether that matters to the grant dialog is what a headset run shows
 * (docs/adr/0009-quest-permission-wait.md has the prediction).
 *
 * Lines (sentinel log, no counters): {"event":"permission_wait_install",...} at install and
 * {"event":"permission_wait","state":"already_granted"|"waiting"|"granted"|"timeout"|"check_unavailable"|
 * "no_storage_permission",...}. They are written after the original returned, never inside the game's call.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include "pinned_targets.h"

namespace nevr_quest::permission_wait {

using RequestThunk = sentinel::pinned::LibR15RequestPermissionsThunk;
// The game's CheckPermission: true when `permission` is granted.
using CheckFn = bool (*)(const char* permission) noexcept;

inline constexpr unsigned kPollMs = 500;
inline constexpr unsigned kTimeoutMs = 30000;

// Arms the handler and installs the thunk in libr15's RequestPermissions slot. One line
// {"event":"permission_wait_install",...} says what was installed and whether CheckPermission was found. True when the
// slot holds the thunk (a CheckPermission that was not found is logged and makes the wait a no-op, not a failure).
bool Install() noexcept;

// The install on any target, with `check` as the permission check (null: resolve the pinned libr15's, as Install does
// on its own slot). The host test installs it in a fixture library.
bool InstallAt(const sentinel::GotTarget& target, CheckFn check) noexcept;

// Test support: arms the handler without touching a GOT slot, with `check`, a poll of `pollMs` and a timeout of
// `timeoutMs`. ResetForTest clears the one-shot and the counts.
void ArmForTest(CheckFn check, unsigned pollMs, unsigned timeoutMs) noexcept;
void ResetForTest() noexcept;

struct Counts {
  unsigned long long requests, waits, granted, timeouts;
};
Counts CurrentCounts() noexcept;

}  // namespace nevr_quest::permission_wait
