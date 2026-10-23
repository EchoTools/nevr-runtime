/* The first-run data step, wired into the game's start (#807, docs/adr/0011-quest-game-data-first-run.md).
 *
 * The permission-wait hook (permission_wait_hook.h) calls AfterStorage on the game's thread, after the boot request's
 * wait and before RequestPermissions returns to the game, so the data is in place before CncaGame::Initialize reads its
 * manifests. AfterStorage is told how the wait ended (permission_wait.h, StorageOutcome):
 *   - data::RunDataStep runs over the data roots with the pinned CurlTransport. The media root
 *     (Android/media/<package>, the app's own directory) needs no storage permission; the legacy root does. With the
 *     grant both roots are used. Without it (a denial, a timeout, no check, no storage permission in the request) the
 *     legacy root is neither recovered nor read, the decision is made on the media root alone and one
 *     `data_legacy_skipped` line says so; a failure to write the media root then ends with the message
 *     data::kNeedsStoragePermission (how to allow Storage), `data_step outcome=no_storage_grant`, and a nonzero exit.
 *     First the step puts the leftovers of an interrupted install right on the roots it may use (RecoverReplaced, then
 *     CleanLeftovers), then:
 *   - data already there (any of the three data-root outcomes): nothing is fetched, one `data_step` line says so,
 *     and a whole `_data.zip.part` of ours left beside it is removed;
 *   - data missing or of another build: the fetch runs; on success obb_skip::ResolveRoot picks the root again so the
 *     game is served the new data;
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
 * Events (sentinel log, no URL): `data_step_install`; `data_check`, `data_fetch`, `data_extract`, `data_swap` (the
 * step's own events, fields as the step reports them); `data_step` (outcome, action, root, status, detail);
 * `data_present` (result token of the toast, looper_prepared; "requested" means Toast.show() returned, not that it
 * drew); `data_probe`.
 *
 * Debug probe (ADR 0011 measurements 1 and 2): a file `nevr-data-probe` in the media data root whose first line is
 * `toast <text>` (show the text as the floor does and log how far it got) or `block <seconds>` (hold the android_main
 * thread, at most 900 s). It is read and renamed to `nevr-data-probe.done` the first time AfterStorage runs with Storage
 * granted, so a probe acts for one launch. It is for the maintainers' headset runs and exists only in development
 * builds (Options::probe_enabled, set from nevr_build::kIsDev); no config key and no shipped file creates it.
 *
 * Built with -fno-exceptions, like every translation unit the hook path reaches.
 */
#pragma once

#include <cstdint>
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
  AppFn app = nullptr;                         // the android_app; null: process_end::CurrentApp
  data_root::ExistsFn exists = nullptr;        // null: obb_skip::PathExists (directories and non-empty files)
  data::Transport* transport = nullptr;        // null: a CurlTransport with the pinned roots (data_host_roots.h)
  EnderFn ender = nullptr;                     // null: process_end::EndWithExitCode
  SleepFn sleep = nullptr;                     // null: nanosleep
  std::string probe_path;                      // empty: <media root>/nevr-data-probe
  std::string fetch_root_override;             // empty: where the data step decides (test seam)
  unsigned hold_ms = 5000;                     // the message is held twice this long (ten seconds), shown again between
  bool probe_enabled = false;                  // read the measurement probe file (dev builds only; entry.cpp sets it)
  std::vector<std::string> recovery_roots;     // empty: both data roots (test seam)
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

// The download's bounds. A transfer that moves less than kMinBytesPerSecond for kStallSeconds ends as a transport
// failure (a network failure to the player), which the fetch retries from the part's size with its backoff; a
// connection that does not open ends after kConnectTimeoutSeconds.
inline constexpr long kConnectTimeoutSeconds = 15;
inline constexpr long kStallSeconds = 120;
inline constexpr long kMinBytesPerSecond = 16 * 1024;

}  // namespace nevr_quest::data_wiring
