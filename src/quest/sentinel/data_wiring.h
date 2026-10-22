/* The first-run data step, wired into the game's start (#807, docs/adr/0011-quest-game-data-first-run.md).
 *
 * The permission-wait hook (permission_wait_hook.h) calls AfterStorage on the game's thread, after the boot request's
 * wait and before RequestPermissions returns to the game, so the data is in place before CncaGame::Initialize reads its
 * manifests. AfterStorage is told how the wait ended (permission_wait.h, StorageOutcome):
 *   - Storage not granted (a denial, a timeout, no check, no storage permission in the request): nothing is read,
 *     fetched or replaced. The message data::kNeedsStoragePermission says how to allow it, is held, a `data_step` Error
 *     line (outcome=no_storage_grant) records it, and the process ends nonzero;
 *   - Storage granted: data::RunDataStep runs over the real data roots with the pinned CurlTransport. First it puts
 *     the leftovers of an interrupted install right on both roots (RecoverReplaced, then CleanLeftovers), then:
 *   - data already there (any of the three data-root outcomes): nothing is fetched, one `data_step` line says so;
 *   - data missing or of another build: the fetch runs; on success obb_skip::ResolveRoot picks the root again so the
 *     game is served the new data;
 *   - the fetch fails: the visible message (toast_presenter.h) is shown and held ten seconds, a `data_step` Error line
 *     records it, and the process ends with a nonzero code after the `android_main_returned` line and the before-exit
 *     report (process_end::EndWithExitCode) instead of letting the game fail its own load.
 * The download ends when the game asks its activity to finish (android_app::destroyRequested, read each progress
 * callback and between the backoff's slices), and a transfer under kMinBytesPerSecond for kStallSeconds fails.
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
  data_root::ExistsFn exists = nullptr;        // null: obb_skip::DirectoryExists
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

// The step itself, for the host test. `outcome` is how the permission wait ended: without a grant the step does
// nothing but tell the player how to allow Storage, and the process ends with a nonzero code.
void AfterStorage(permission_wait::StorageOutcome outcome) noexcept;

// The download's bounds. A transfer that moves less than kMinBytesPerSecond for kStallSeconds ends as a transport
// failure (a network failure to the player), which the fetch retries from the part's size with its backoff; a
// connection that does not open ends after kConnectTimeoutSeconds.
inline constexpr long kConnectTimeoutSeconds = 15;
inline constexpr long kStallSeconds = 120;
inline constexpr long kMinBytesPerSecond = 16 * 1024;

}  // namespace nevr_quest::data_wiring
