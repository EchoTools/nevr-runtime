/* The first-run data step, wired into the game's start (#807, docs/adr/0011-quest-game-data-first-run.md).
 *
 * The permission-wait hook (permission_wait_hook.h) calls AfterStorage on the game's thread, after the boot request's
 * wait and before RequestPermissions returns to the game, so the data is in place before CncaGame::Initialize reads its
 * manifests. AfterStorage runs data::RunDataStep over the real data roots with the pinned CurlTransport:
 *   - data already there (any of the three data-root outcomes): nothing is fetched, one `data_step` line says so;
 *   - data missing or of another build: the fetch runs; on success obb_skip::ResolveRoot picks the root again so the
 *     game is served the new data;
 *   - the fetch fails: the visible message (toast_presenter.h) is shown and held about twelve seconds, a `data_step`
 *     Error line records it, and the process ends cleanly (process_end::EndProcess, exit 0) instead of letting the
 *     game fail its own load.
 * The android_main thread blocks for the whole step. Whether Android tolerates that for the length of a download is
 * ADR 0011 measurement 2; `data_probe` below is how the headset run measures it.
 *
 * Events (sentinel log, no URL): `data_step_install`; `data_check`, `data_fetch`, `data_extract`, `data_swap` (the
 * step's own events, fields as the step reports them); `data_step` (outcome, action, root, status, detail);
 * `data_present` (result token of the toast, looper_prepared); `data_probe`.
 *
 * Debug probe (ADR 0011 measurements 1 and 2): a file `nevr-data-probe` in the media data root whose first line is
 * `toast <text>` (show the text as the floor does and log how far it got) or `block <seconds>` (hold the android_main
 * thread, at most 900 s). It is read and renamed to `nevr-data-probe.done` the first time AfterStorage runs, so a probe
 * acts for one launch. It is for the maintainers' headset runs; no config key and no shipped file creates it.
 *
 * Built with -fno-exceptions, like every translation unit the hook path reaches.
 */
#pragma once

#include <cstdint>
#include <string>

#include "quest/data/transport.h"
#include "quest/sentinel/data_root.h"

namespace nevr_quest::data_wiring {

// Shows `text` on the headset; the result token as toast_presenter.h defines it. `app` is the game's android_app.
using ShowFn = const char* (*)(void* app, const char* text, bool* looperPrepared) noexcept;
using EnderFn = void (*)() noexcept;
using SleepFn = void (*)(unsigned ms) noexcept;
using AppFn = void* (*)() noexcept;

// What the step touches. Production leaves everything but `show` at its default (null / empty).
struct Options {
  ShowFn show = nullptr;                       // required for a visible message; null: the message is only logged
  AppFn app = nullptr;                         // the android_app; null: process_end::CurrentApp
  data_root::ExistsFn exists = nullptr;        // null: obb_skip::DirectoryExists
  data::Transport* transport = nullptr;        // null: a CurlTransport with the pinned roots (data_host_roots.h)
  EnderFn ender = nullptr;                     // null: process_end::EndProcess
  SleepFn sleep = nullptr;                     // null: nanosleep
  std::string probe_path;                      // empty: <media root>/nevr-data-probe
  std::string fetch_root_override;             // empty: where the data step decides (test seam)
  unsigned hold_ms = 4000;                     // between the message and its two repeats
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

// The step itself, for the host test.
void AfterStorage() noexcept;

}  // namespace nevr_quest::data_wiring
