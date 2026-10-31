/* The first-run data step, wired into the game's start (#807, #758, docs/adr/0013-quest-game-data-from-the-launcher.md).
 *
 * The permission-wait hook (permission_wait_hook.h) calls AfterStorage on the game's thread, once, before the game's
 * manifest read: from the CheckPermission hook when the microphone permission is already granted (no request follows),
 * otherwise after the boot request's wait, before RequestPermissions returns. AfterStorage is told what the storage
 * permissions are (permission_wait.h, StorageOutcome).
 *
 * The one root that counts is the root the engine reads this launch. With an OBB file the OBB skip ran before this
 * (CSysFile::Init mounts in android_main, ahead of the game's Initialize) and the game recorded the root it served
 * (obb_skip::CurrentRoot); without one CSysFile::Init returned at "OBB not found" and the skip never ran. Then
 * CSysInfo::GetDataRootDir decides: our hook on it answers the root ResolveRoot chose (obb_skip::DataRootRedirect, #866), the
 * media root unless the legacy root alone holds the game's data.
 * `data_step_start` logs the root and where it came from (`engine_root_source`: obb_skip, data_root_hook or
 * engine_default). So:
 *   - Storage not granted and the engine reads the legacy root (shared storage): nothing is looked at, fetched or
 *     written; the player is shown data::kNeedsStoragePermission, it is held ten seconds (a release build holds
 *     nothing: Options::hold_ms 0), and the process ends nonzero
 *     (`data_step outcome=no_storage_grant`). The media root needs no permission and is used as it is, whether the engine
 *     reads it through a skipped mount or through the data-root hook.
 *   - data::RunDataStep looks at that root only (StepConfig::engine_root), after (development builds only) putting the
 *     leftovers of an interrupted install right (RecoverReplaced, CleanLeftovers):
 *   - data already there: nothing is fetched and one `data_check action=use` line says so;
 *   - data missing or of another build, in a development build (Options::allow_fetch): the fetch runs into that root;
 *   - data missing or of another build, in a release build: nothing is fetched or written; the player is shown
 *     data::kGameDataMissing (install with Echo Launcher), it is not held (Options::hold_ms 0), a `data_step outcome=missing`
 *     Error line records it, and the process ends nonzero (reason `game_data_missing`);
 *   - the fetch fails: the visible message (toast_presenter.h) is shown and held ten seconds, a `data_step` Error line
 *     records it, and the process ends with a nonzero code after the `android_main_returned` line and the before-exit
 *     report (process_end::EndWithExitCode) instead of letting the game fail its own load.
 * There is no cancel. The step blocks android_main's thread, the only thread that reads the glue's destroy command
 * (android_app::destroyRequested is written by android_main's own looper poll), so nothing can end the download from
 * inside. A player who quits ends the process by the system; the marked `_data.zip.part` stays, and the next launch
 * resumes from its size. A transfer under kMinBytesPerSecond for kStallSeconds fails as a network failure.
 * The android_main thread blocks for the whole step. Whether Android tolerates that for the length of a download is
 * ADR 0011 measurement 2; `data_probe` below is how the headset run measures it.
 *
 * After the data step (`kUsed` or `kFetched`, whichever the build) the bundled content pack runs (ADR 0029, data/pack.h) on the
 * same thread, in the root the data step used: an APK without `assets/nevr-pack/pack.json` logs one `pack_check outcome=none`;
 * one with it overlays the pack's files over the stock data when the stock manifest is one the pack names (`pack_check`,
 * `pack_place`, `pack_step`, and `data_version` for what is in place). A failed placement shows the floor message, holds it
 * and ends the process nonzero with reason `pack_failed`.
 *
 * Events (sentinel log, no URL): `data_step_install`; `data_check` (carries `data_version` and `manifest_sha256`: the data found), `data_fetch`,
 * `data_extract`, `data_swap`, `data_version` (the data a fetch placed) (the step's own events, fields as the step reports them); `data_step` (outcome, action, root, status, detail);
 * `data_present` (result token of the toast, looper_prepared; "requested" means Toast.show() returned, not that it
 * drew); `data_probe`.
 *
 * Debug probe (ADR 0011 measurements 1 and 2, ADR 0013 #829): a file `nevr-data-probe` in the media data root whose first
 * line (probe_command.h) is `toast <text>` (show the text as the floor does and log how far it got), `ui-toast <text>` (the
 * same text through a Toast whose handler runs on the main looper, toast::ShowOnMainLooper), `ui-post <text>` (the same text
 * queued to the UI thread's native looper, toast::ShowOnUiThread; the Toast's own result is a later `ui_post` line), `dialog <text>` (a dialog; no
 * presenter exists, so the line says `no_presenter`) or `block <seconds>` (hold the android_main thread, at most 900 s). The
 * `ui-toast`, `ui-post` and `dialog` kinds log one `data_probe` line with the presenter's result, the thread id, whether it was the
 * process's main thread and whether a looper was prepared. It is read and renamed to `nevr-data-probe.done` the first time AfterStorage runs with Storage
 * granted, so a probe acts for one launch. It is for the maintainers' headset runs and exists only in development
 * builds (Options::probe_enabled, set from nevr_build::kIsDev); no config key and no shipped file creates it.
 *
 * Built with -fno-exceptions, like every translation unit the hook path reaches.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "permission_wait_hook.h"
#include "quest/data/transport.h"
#include "quest/sentinel/data_root.h"

namespace nevr_quest::data_wiring {

// Shows `text` on the headset; the result token as toast_presenter.h defines it. `app` is the game's android_app.
using ShowFn = const char* (*)(void* app, const char* text, bool* looperPrepared) noexcept;
// Ends the process with `exitCode` (nonzero) after the before-exit report; `reason` names why. Never returns in production.
using EnderFn = void (*)(int exitCode, const char* reason) noexcept;
using SleepFn = void (*)(unsigned ms) noexcept;
using AppFn = void* (*)() noexcept;

// What the step touches. Production leaves everything but `show` at its default (null / empty).
struct Options {
  ShowFn show = nullptr;                       // required for a visible message; null: the message is only logged
  ShowFn show_ui = nullptr;                    // probe kind `ui-toast`: a Toast on the main looper; null: logged as no_presenter
  ShowFn show_post = nullptr;                  // probe kind `ui-post`: a call queued to the UI thread's looper; null: no_presenter
  ShowFn show_dialog = nullptr;                // probe kind `dialog`; null: logged as no_presenter
  AppFn app = nullptr;                         // the android_app; null: process_end::CurrentApp
  data_root::ExistsFn exists = nullptr;        // null: obb_skip::PathExists (directories and non-empty files)
  data::Transport* transport = nullptr;        // null: a CurlTransport with the pinned roots (data_host_roots.h)
  EnderFn ender = nullptr;                     // null: process_end::EndWithExitCode
  SleepFn sleep = nullptr;                     // null: nanosleep
  std::string probe_path;                      // empty: <media root>/nevr-data-probe
  std::string fetch_root_override;             // empty: where the data step decides (test seam)
  unsigned hold_ms = 5000;                     // the message is held twice this long (ten seconds), shown again between
                                               // 0: not held and not shown again (release builds, entry.cpp)
  bool probe_enabled = false;                  // read the measurement probe file (dev builds only; entry.cpp sets it)
  bool allow_fetch = false;                    // download missing data (dev builds only; entry.cpp sets it); false: only look
  std::vector<std::string> recovery_roots;     // empty: both data roots (test seam)
  // The bundled content pack (docs/adr/0029-quest-bundled-content-pack.md). `apk_path` empty: the APK the sentinel was
  // mapped from (dladdr). The rest are test seams: free bytes of the root (null: statvfs) and a probe the pack step calls
  // after a temporary is written and after a rename, whose false stops the step as a kill would.
  std::string apk_path;
  std::function<std::uint64_t(const std::string& root)> pack_available_bytes;
  std::function<bool(const char* point, const std::string& path)> pack_probe;
  // Test seams for the pinned zip: when `zip_sha256` is not empty the fetch uses these instead of the pins in
  // data/plan.h, with two attempts and no real backoff.
  std::string zip_sha256;
  std::uint64_t zip_bytes = 0;
  std::uint64_t extracted_bytes = 0;
};

// Sets the after-storage step on the permission-wait hook. One `data_step_install` line. True always: the step
// is armed whether or not the permission-wait hook's own install succeeded (a game that never calls
// RequestPermissions never reaches it).
bool Install(const Options& options) noexcept;

// The step itself, for the host test. `outcome` is how the permission wait ended: without a grant the legacy root is
// skipped (see above).
void AfterStorage(permission_wait::StorageOutcome outcome) noexcept;

// Whether the engine reads shared storage this launch: true when its root is the legacy (shared storage) root. The
// permission-wait hook asks for the storage permissions only then (permission_wait_hook.h, StorageNeededFn).
bool StorageNeeded() noexcept;

// The download's bounds. A transfer that moves less than kMinBytesPerSecond for kStallSeconds ends as a transport
// failure (a network failure to the player), which the fetch retries from the part's size with its backoff; a
// connection that does not open ends after kConnectTimeoutSeconds.
inline constexpr long kConnectTimeoutSeconds = 15;
inline constexpr long kStallSeconds = 120;
inline constexpr long kMinBytesPerSecond = 16 * 1024;

}  // namespace nevr_quest::data_wiring
