/* The frozen-load guard (#1737, G6A-045): when the game stops drawing while it is in VR mode, the process ends and the player
 * lands in Home, instead of looking at a frozen view until they force-stop the app.
 *
 * What it measures. The game submits one frame per rendered frame through vrapi_SubmitFrame2 (one call site,
 * NRadEngine::DoVRSubmission 0x1a70954 in the pinned libr15). The render-configuration hook already owns that slot
 * (render_config_hook.cpp); this module takes its heartbeat from that hook's frame observer (render_config.h,
 * SetFrameObserver), so the slot keeps one hook. It also hooks the two VR-mode calls, vrapi_EnterVrMode and
 * vrapi_LeaveVrMode (both from CVR::UpdateTracking, once per resume and pause; LeaveVrMode also from
 * CVR::ShutdownGraphics), because a paused app submits no frames and must never be taken for a hung one.
 *
 * The rule. Armed by the first frame after each successful EnterVrMode; disarmed by LeaveVrMode. A watcher thread looks once
 * a second: armed, in VR mode, and no frame for `seconds` (default 30) -> one Warning line
 *   {"event":"hangwd_exit","reason":"no_frame_submitted_in_vr","gap_ms":..,"limit_s":..,"frames":..,"armed_ms":..}
 * then process_end::EndNow("hang_watchdog"): its process_ending line, the before-exit flush, _exit(0). The next launch is a
 * cold process. The 30 s bound is the reference build's: its measured hangs ran 445 to 638 s, its clean loading gaps at most
 * 12 s (a slow load that submits no frame for 30 s in VR mode is ended too; that is the trade the bound makes).
 *
 * Switches, in the q15t data directory (/sdcard/Android/data/<package>/files/q15t, module_switch.h's directory), read once at
 * install: `hangwd.off` turns the guard off; `hangwd.sec` holds another bound in seconds, 10 to 900 (anything else is
 * ignored and the line says so). On by default.
 *
 * Install (from the render-configuration step, sentinel/entry.cpp, after that step's own hooks): the two VR-mode hooks and
 * the watcher; the step then sets BeatNow as the frame observer. Both hooks are required: without them the guard cannot
 * tell a pause from a hang, so it installs neither the observer nor the watcher and says so. One line
 *   {"event":"q15t_hangwd_install","result":"on|off|refused","reason":..,"limit_s":..,"limit_source":"default|hangwd.sec|
 *    hangwd.sec_ignored","enter_status":..,"leave_status":..,"frame_source":"render_config_submit_frame"}
 * and one {"event":"hangwd_vr","state":"entered|left","frames":..} line per VR-mode change (a pause or a resume).
 *
 * Cost per frame (the observer): one acquire load, one clock_gettime(CLOCK_MONOTONIC) (vDSO), two relaxed stores and one
 * fetch_add. Estimated, not measured.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "got_hook.h"
#include "pinned_targets.h"

namespace nevr_quest::hang_watchdog {

using EnterThunk = sentinel::pinned::LibR15EnterVrModeThunk;
using LeaveThunk = sentinel::pinned::LibR15LeaveVrModeThunk;

inline constexpr unsigned kDefaultSeconds = 30;
inline constexpr unsigned kMinSeconds = 10;
inline constexpr unsigned kMaxSeconds = 900;
inline constexpr unsigned kPollMs = 1000;
inline constexpr const char* kOffSwitch = "hangwd.off";
inline constexpr const char* kSecondsFile = "hangwd.sec";

// ---- the state (render thread writes, watcher reads) -----------------------------------------------------------------------

// The frame observer: one frame submitted at `nowNs` (CLOCK_MONOTONIC). The first frame after EnterVr arms the guard.
void Beat(std::int64_t nowNs) noexcept;
// Beat(now): what render_config's SubmitFrame2 handler calls.
void BeatNow() noexcept;
// A successful vrapi_EnterVrMode: in VR mode, armed again at the next frame. LeaveVr: out of VR mode, disarmed.
void EnterVr() noexcept;
void LeaveVr() noexcept;

struct Verdict {
  bool expired;                 // armed, in VR mode, and no frame for the bound
  long long gapMs;              // since the last frame (0 when not armed)
  unsigned long long frames;    // frames since install
  long long armedMs;            // from the arming frame to the last frame
};
// The watcher's test at `nowNs` against a bound of `seconds`. Pure apart from reading the state.
Verdict Check(std::int64_t nowNs, unsigned seconds) noexcept;

// ---- the switches ----------------------------------------------------------------------------------------------------------

struct Settings {
  bool off;
  unsigned seconds;
  const char* source;  // "default", "hangwd.sec" or "hangwd.sec_ignored"
};
// Reads `hangwd.off` and `hangwd.sec` in `dir` (null: the q15t data directory; none to name: the defaults).
Settings ReadSettings(const char* dir) noexcept;
// Parses the text of `hangwd.sec`: a decimal integer in [kMinSeconds, kMaxSeconds], surrounding whitespace allowed.
bool ParseSeconds(const char* text, unsigned* out) noexcept;

// ---- install ---------------------------------------------------------------------------------------------------------------

// Ends the process (production: process_end::EndNow). Tests inject their own.
using EndFn = void (*)(const char* reason) noexcept;
// The GOT step for slot 0 (EnterVrMode) or 1 (LeaveVrMode); tests replace it.
using InstallFn = sentinel::GotStatus (*)(int slot) noexcept;

// The production install: the q15t data directory, the pinned slots, process_end::EndNow, a watcher thread. True when on;
// the caller then sets BeatNow as render_config's frame observer. `frameSourceLive`: render_config's SubmitFrame2 hook is
// installed (false: nothing is hooked and the line says refused, frame_source_not_installed).
bool Install(bool frameSourceLive) noexcept;
// The same with every seam named. `startWatcher` false: no thread (a test calls PollOnce).
bool InstallVia(const char* dir, InstallFn install, EndFn end, bool startWatcher, bool frameSourceLive) noexcept;

// One look of the watcher at `nowNs`: on expiry the hangwd_exit line and `end`. True when it ended.
bool PollOnce(std::int64_t nowNs) noexcept;

// Test support: arms the two thunks without touching a slot; ResetForTest forgets the state and the settings.
void ArmForTest() noexcept;
void ResetForTest() noexcept;

}  // namespace nevr_quest::hang_watchdog
