/* The game's storage permission request, made once and awaited only when the engine reads shared storage (#640, #947, #1737).
 *
 * On the first launch of a fresh install the game asks for RECORD_AUDIO, READ_EXTERNAL_STORAGE and
 * WRITE_EXTERNAL_STORAGE and does not wait for the answer: CR15Game::Initialize calls CSysInfo::RequestPermissions
 * (a fire and forget JNI requestPermissions on the NativeActivity) and goes straight on into CncaGame::Initialize,
 * whose package-manifest read needs the storage permissions. The read fails 260 ms later, while the grant dialog has
 * not started yet; the game reports an initialisation error and finishes its own activity (pinned_targets.h,
 * LibR15RequestPermissions, has the call chain and the addresses).
 *
 * One GOT hook on libr15's RequestPermissions slot. The handler decides what to ask and makes the request, then on the
 * FIRST call only (the boot request; later callers are script nodes that must not block) it awaits the storage grant
 * when the storage permissions were asked and the request reached Java:
 *   - it checks each requested name that ends in "_EXTERNAL_STORAGE" with the game's own CSysInfo::CheckPermission
 *     (libr15 base + kCheckPermissionVaddr, used only after the module's build ID and the function's first four
 *     instructions match the pinned build);
 *   - while one is denied it sleeps kPollMs and checks again, for at most kTimeoutMs; on timeout it returns and the
 *     game goes on.
 * The wait does not pump the looper. Whether that matters to the grant dialog is what a headset run shows
 * (docs/adr/0009-quest-permission-wait.md has the prediction).
 *
 * Whether the storage permissions are asked at all is the engine's choice of root (StorageNeededFn; production:
 * data_wiring::StorageNeeded, true when the engine reads the legacy shared-storage root):
 *   - storage needed: every name is asked, and the boot request waits for the grant (the behavior above);
 *   - storage not needed (the engine reads the app-owned media root, which needs no permission): the storage names
 *     are dropped from the request, the microphone (RECORD_AUDIO) is still asked when it is named, and nothing waits.
 *     When no name is left the request is not made at all. This is the behavior of the reference build's byte patch
 *     (which turns RequestPermissions into `ret`), except that it keeps the microphone prompt.
 *
 * The request is not always made. CR15Game::Initialize calls RequestPermissions only when its RECORD_AUDIO check
 * (CSysInfo::CheckPermission, the first call in the process) answers "denied"; a headset that already holds the
 * microphone permission (any that has run the game before) goes straight into CncaGame::Initialize and the manifest
 * read, with no request and no wait (headset run 2026-10-22, #758). So a second GOT hook sits on the CheckPermission
 * slot: on the FIRST call, when it is the microphone check and the game's answer is "granted", the request will not
 * come, and the after-storage step runs there instead, on the same thread, before CR15Game::Initialize goes on. Its
 * outcome is kAlreadyGranted when both storage permissions are granted and kDeniedNotRequested when one is not (nothing
 * will ask for it; the step must not write). A denied microphone leaves the step to the request path. The step runs at
 * most once across both paths.
 *
 * The request itself (#947). The game's RequestPermissions passes Activity.requestPermissions no request code: its JNI
 * call carries the array and nothing in the int's register (permission_request.h has the instructions), so the code is
 * whatever that register holds, and a negative one aborts the process. The handler does not call it: it makes the
 * same request through the requester (SetRequester; production: permission_request::Request on the game's activity)
 * with kRequestCode, and calls the game's function only when the requester could not make the call (the result says
 * why). The game's function is then asked only when it is not the reference build's `ret` (OriginalKind): on the
 * reference build nothing is asked and nothing is waited on. The original's kind is read once at install from its
 * first instruction (ClassifyOriginalWord).
 *
 * Lines (sentinel log, no counters):
 *   {"event":"permission_wait_original",...} at install: kind calls_java or returns_at_once, or unknown with a WARN;
 *   {"event":"permission_request",...} for the first kMaxLoggedRequests: result, path ("ours", "game", "game_noop",
 *     "none"), permissions (the game's count), asked, storage_needed, and request_code when ours;
 *   {"event":"permission_wait",...} with the states of the wait ("waiting", "granted", "timeout", ...) and, for the
 *     boot request when no wait applies, state "not_requested" with reason storage_not_needed, nothing_to_ask or
 *     original_no_op.
 * They are written after the original returned, never inside the game's call.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include "pinned_targets.h"

namespace nevr_quest::permission_wait {

using RequestThunk = sentinel::pinned::LibR15RequestPermissionsThunk;
using CheckThunk = sentinel::pinned::LibR15CheckPermissionThunk;
// The game's CheckPermission: true when `permission` is granted.
using CheckFn = bool (*)(const char* permission) noexcept;

inline constexpr unsigned kPollMs = 500;
inline constexpr unsigned kTimeoutMs = 30000;
// The request code of every request the handler makes. Activity.requestPermissions refuses a negative one; the game
// never reads it back.
inline constexpr int kRequestCode = 1;
inline constexpr unsigned kMaxLoggedRequests = 8;

// Makes the request: "requested" when it was made, else a token saying why not (permission_request::Request's).
using RequestFn = const char* (*)(const char* const* names, std::uint64_t count, int requestCode) noexcept;
// Sets the requester. Null (the default): every request goes to the game's own function. Set it before the game runs.
void SetRequester(RequestFn fn) noexcept;

// How the boot request's wait ended. Only kGranted and kAlreadyGranted mean the app may touch shared storage.
enum class StorageOutcome {
  kGranted,             // denied at first, granted while waiting
  kAlreadyGranted,      // every storage permission in the request was granted already
  kTimeout,             // still denied after kTimeoutMs (a denial or a dialog nobody answered)
  kCheckUnavailable,    // CheckPermission was not resolved: nothing can be said about the grant
  kNoStoragePermission, // the boot request named no storage permission
  kDeniedNotRequested   // the microphone was already granted, so the game asked for nothing, and storage is denied
};
const char* StorageOutcomeName(StorageOutcome outcome) noexcept;
inline bool StorageGranted(StorageOutcome outcome) noexcept {
  return outcome == StorageOutcome::kGranted || outcome == StorageOutcome::kAlreadyGranted;
}

// What runs on the game's thread right after the boot request's wait, still before RequestPermissions returns to the
// game: the first-run data step (data_wiring.h), told how the wait ended. It runs on the first call only, the boot
// request, and is reached through this pointer so the handler holds no frame of its own across it. Null: none. Set it
// before the game runs.
using AfterStorageFn = void (*)(StorageOutcome outcome) noexcept;
void SetAfterStorage(AfterStorageFn fn) noexcept;

// Whether the engine reads shared storage, so the storage permissions are needed. Null (default): needed.
using StorageNeededFn = bool (*)() noexcept;
void SetStorageNeeded(StorageNeededFn fn) noexcept;

// What libr15's own RequestPermissions does: stock calls Java, the reference build's patched copy returns at once.
enum class OriginalKind { kUnknown, kCallsJava, kReturnsAtOnce };
const char* OriginalKindName(OriginalKind kind) noexcept;  // "unknown" | "calls_java" | "returns_at_once"
// The kind of a function whose first instruction word is `firstWord`: the pinned stock word, the pinned return word, else unknown.
OriginalKind ClassifyOriginalWord(std::uint32_t firstWord) noexcept;
// Sets the kind the handler acts on. Install sets it from the live function; tests set it.
void SetOriginalKind(OriginalKind kind) noexcept;
// The kind the handler acts on now (what install read from the module, or what a test set).
OriginalKind CurrentOriginalKind() noexcept;

// Arms the handlers and installs the thunks in libr15's RequestPermissions and CheckPermission slots. One line
// {"event":"permission_wait_install",...} says what was installed and whether CheckPermission was found, and one line
// {"event":"permission_wait_check_install",...} says whether the CheckPermission slot holds its thunk. True when both
// slots hold their thunks (a CheckPermission that was not found is logged and makes the wait a no-op, not a failure).
bool Install() noexcept;

// The install on any target, with `check` as the permission check (null: resolve the pinned libr15's, as Install does
// on its own slot). The host test installs it in a fixture library. `checkTarget` is the CheckPermission slot.
bool InstallAt(const sentinel::GotTarget& target, const sentinel::GotTarget& checkTarget, CheckFn check) noexcept;

// The same install with the module lookup given: the live module is read through `lookup` (the install-time kind, the
// build ID, the CheckPermission prologue) and the slots are found through it. A test passes a lookup that returns an
// image it built in memory, to pin the classification without libr15.
bool InstallAtWith(sentinel::ImageLookup lookup, const sentinel::GotTarget& target, const sentinel::GotTarget& checkTarget,
                   CheckFn check) noexcept;

// Test support: arms the handler without touching a GOT slot, with `check`, a poll of `pollMs` and a timeout of
// `timeoutMs`. ResetForTest clears the one-shot, the counts and the predicate and kind set above.
void ArmForTest(CheckFn check, unsigned pollMs, unsigned timeoutMs) noexcept;
void ResetForTest() noexcept;

struct Counts {
  unsigned long long requests, waits, granted, timeouts, game_requests, no_request;  // game_requests: made by the game's function; no_request: nothing to ask
};
Counts CurrentCounts() noexcept;

}  // namespace nevr_quest::permission_wait
