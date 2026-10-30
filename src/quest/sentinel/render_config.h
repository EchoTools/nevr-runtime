/* The render-configuration log line (#1139, docs/adr/0022-render-config-log.md): one `render_config` line at the first
 * rendered frame and another on every change, so a report of stutter or low frame rate says what the game was rendering.
 *
 * What it reads, and nothing else (no code page is written, ADR 0021):
 *   - five GOT hooks on libr15's VrApi imports, each a pass-through that calls the original with the game's arguments, stores
 *     what the game asked for and what the runtime answered in atomics, and returns the original's result:
 *       vrapi_SetDisplayRefreshRate  the refresh rate the game requests, and the runtime's answer
 *       vrapi_SetClockLevels         the CPU and GPU levels, and the answer
 *       vrapi_SetPropertyInt         foveation level (property 0xf) and dynamic foveation (property 0x1e)
 *       vrapi_GetSystemPropertyFloat the refresh rate the runtime reports (property 4)
 *       vrapi_SubmitFrame2           one call per rendered frame: the frame count
 *   - three game globals in libr15, read by the poller thread after the build id is proved and the address is shown to lie
 *     inside a PT_LOAD segment of the image: the eye-buffer size pair CVR::RefreshRTSize stores, the RT-size override pair
 *     CVR::SetRTSizeOverride stores, and CGRenderConfig's MSAA mode;
 *   - the texture-quality hook's native answers (texture_quality_hook.h) when it replaces the eye size;
 *   - the Android system properties ro.product.model and ro.build.display.id.
 *
 * Nothing in a hook logs, allocates or throws (callback_thunk.h). A poller thread (`nevr-renderconfig`) wakes once a second,
 * builds a Snapshot and logs. It is created once and ends with the process; the sentinel is never unloaded.
 *
 * The line (sentinel log, `event":"render_config"`): platform, reason (`first_frame` | `change`), changed (only on a change:
 * the field names that differ from the previous line, comma separated), device_model, os_build, refresh_requested_hz,
 * refresh_request_result, refresh_reported_hz, eye_w, eye_h, eye_source (`runtime_suggested` | `game_rt_size_override` |
 * `nevr_texture_quality`), texture_quality, texture_quality_source (`config` | `default`), native_eye_w, native_eye_h (only when
 * a texture-quality level replaces the size), foveation_level, foveation_dynamic, cpu_level, gpu_level, clock_result,
 * msaa_mode, spacewarp (`unknown`: ADR 0022 "Not available yet"), thunk_faults (only when non-zero) and frames. A field the
 * game has not set, or the sentinel could not read, is left out of the line; it is never written as a made-up value.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "got_hook.h"
#include "texture_quality.h"

namespace nevr_quest::render_config {

using Value = std::int64_t;
// A field nobody has set (or that could not be read). Never a legitimate value of any field.
inline constexpr Value kUnset = INT64_MIN;

// The eye-size sources (Snapshot::eyeSource).
inline constexpr Value kEyeRuntimeSuggested = 0;
inline constexpr Value kEyeGameOverride = 1;
inline constexpr Value kEyeNevrTextureQuality = 2;

// Everything the line reports that can change while the game runs. Refresh rates are in millihertz.
struct Snapshot {
  Value refreshRequestedMilli = kUnset;
  Value refreshRequestResult = kUnset;
  Value refreshReportedMilli = kUnset;
  Value cpuLevel = kUnset;
  Value gpuLevel = kUnset;
  Value clockResult = kUnset;
  Value foveationLevel = kUnset;
  Value foveationDynamic = kUnset;
  Value eyeW = kUnset;
  Value eyeH = kUnset;
  Value overrideW = kUnset;
  Value overrideH = kUnset;
  Value eyeSource = kUnset;
  Value nativeEyeW = kUnset;
  Value nativeEyeH = kUnset;
  Value msaaMode = kUnset;
  Value thunkFaults = 0;
  Value frames = 0;  // not compared: it grows every frame
};

// The names of the fields of `after` that differ from `before`, comma separated, into `out` (always NUL terminated).
// Returns how many differ. `frames` and the override pair (inputs of eye_source) are not fields of the line.
std::size_t ChangedFields(const Snapshot& before, const Snapshot& after, char* out, std::size_t capacity) noexcept;

// Refresh rate in tenths of a hertz as text ("72.0"); "" when unset. `out` holds at least 16 bytes.
void FormatHertz(Value milli, char* out, std::size_t capacity) noexcept;

// The libr15 globals the poller reads. `false`: libr15 could not be proved (not loaded, other build id) or an address is
// outside the image; the fields stay unset.
struct Globals {
  Value eyeW = kUnset;
  Value eyeH = kUnset;
  Value overrideW = kUnset;
  Value overrideH = kUnset;
  Value msaaMode = kUnset;
};

struct Device {
  char model[96] = {};
  char osBuild[96] = {};
};

// Where the poller gets the data that is not in an atomic. Production reads libr15 and the system properties; a test
// supplies its own.
struct Source {
  bool (*readGlobals)(Globals* out) noexcept;
  void (*readDevice)(Device* out) noexcept;
};
const Source& ProductionSource() noexcept;

// Reads the globals from `image` with the same checks the production source makes (build id, address inside a PT_LOAD, size
// in range). Exposed so a test can feed an image it built in memory.
bool ReadGlobalsFrom(const sentinel::ElfImage& image, const char* expectedBuildId, Globals* out) noexcept;

// One poll: builds a Snapshot from the atomics and `source`, and logs the line when it is the first with a frame submitted
// or the snapshot differs from the last one logged. Returns true when it logged. Called by the poller thread, and by tests.
bool PollOnce(const Source& source) noexcept;

// Installs the five hooks and starts the poller. `textureLevel` and `fromConfig` are the texture-quality choice the
// constructor sequence resolved (the line says what set the size). Returns true when all five slots hold their thunks. One
// line {"event":"render_config_install",...} says what happened, whatever the outcome.
bool Install(texture_quality::Level textureLevel, bool fromConfig) noexcept;

// Called on the game's render thread at the start of every vrapi_SubmitFrame2, before the call into VrApi (null: none). The
// frozen-load guard (hang_watchdog.h) takes its frame heartbeat from here, so the frame slot has one hook. The observer must
// be cheap (it runs once per frame), must not block and must not call into the game.
using FrameObserver = void (*)() noexcept;
void SetFrameObserver(FrameObserver observer) noexcept;

// Test support. `InstallVia` is Install() with the GOT step replaced by `install` (the status GotHook would return for
// the slot number 0..4 in the order refresh-rate, clock-levels, set-property-int, system-property-float, submit-frame). The
// poller starts, with `pollerSource` and a period of `periodMs`, only when `pollerSource` is given.
using InstallFn = sentinel::GotStatus (*)(int slot) noexcept;
bool InstallVia(texture_quality::Level textureLevel, bool fromConfig, InstallFn install,
                const Source* pollerSource = nullptr, unsigned periodMs = 1000) noexcept;
// Arms the five thunks without touching a slot.
void ArmForTest() noexcept;
// Forgets everything: the atomics, the last logged snapshot, the cached device, the slot marks. Stops the poller.
void ResetForTest() noexcept;
// Starts the poller with `source` and a poll period in milliseconds; StopPollerForTest joins it.
bool StartPollerForTest(const Source& source, unsigned periodMs) noexcept;
void StopPollerForTest() noexcept;

}  // namespace nevr_quest::render_config
