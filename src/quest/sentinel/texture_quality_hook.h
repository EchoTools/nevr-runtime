/* The texture-quality setting (#943): the eye-buffer size libr15 creates its swap chain with.
 *
 * What the game does (pinned libr15; pinned_targets.h has the addresses): CVR::RefreshRTSize asks the headset runtime
 * for the suggested eye buffer size, vrapi_GetSystemPropertyInt id 5 (width) and id 6 (height), multiplies each by
 * CustomViewportScale (1.0) and stores the pair; CVR::Resize creates the swap chain from it, once per process, and logs
 * "Creating OVR swap chain with size %i x %i (2 slices, %u sample[s])".
 *
 * One GOT hook, on libr15's vrapi_GetSystemPropertyInt slot (JUMP_SLOT 0x36fa728). The handler calls the original with
 * exactly the arguments it was given, and returns:
 *   - for id 5 and id 6, when a level other than `stock` is latched, that level's width or height (texture_quality.h);
 *   - for every other id, and for 5 and 6 with no level latched, exactly what the original returned.
 * The level is the config's graphics.texture_quality (quest_config.h), `stock` when it is absent or wrong (texture_quality.h
 * kDefaultLevel, kFallbackLevel). It is latched by the first
 * Install in the process and never changes: the swap chain is created once and its size has to stay what it was, so a
 * second Install with another level is refused and the first level keeps answering. A change of the setting takes
 * effect at the next launch. `stock` installs nothing.
 *
 * One path the hook does not cover: CVR::RefreshRTSize skips the query when the stored size override (ELF 0x37a33e8, set
 * only by CVR::SetRTSizeOverride, whose one caller found is the desktop 1920x1080 case in ConfigureEngine) is non-zero,
 * so no level applies there. A Quest run does not set it (static reading, not observed on a headset).
 *
 * It never logs on the game's call path. Install logs one line, whatever the level and the outcome:
 *   {"level":"info","event":"texture_quality","result":"installed|not_installed|refused|skipped",
 *    "quality":"high","source":"config|default","w":3072,"h":3380,...}
 * (`level` is the log severity; the setting is `quality`.) `w` and `h` are the sizes this hook answers with, 0 when it
 * answers with none. `not_installed` is `stock` (no hook, the runtime's own size); `refused` is a GotHook refusal (the status rides in
 * `status`) or a second level; `skipped` is the counters refused. Counters (hook_report.h), registered by
 * RegisterCounters(); the first two are values, not counts, and carry the runtime's own answers (what the game would
 * have got with `stock`), which is the headset's native size:
 *   gfx_native_eye_width, gfx_native_eye_height   the runtime's last answer to id 5 and id 6
 *   gfx_eye_size_served                           answers replaced by the latched level's size
 *   gfx_thunk_faults                              the thunk had no original
 *
 * The slot is shared with the hardware dump (sysprop_int_share.h): a successful install marks the slot owned and the
 * handler tells the dump's observer each query's native answer.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include "pinned_targets.h"
#include "sysprop_int_share.h"
#include "texture_quality.h"

namespace nevr_quest::texture_quality {

using SystemPropertyIntThunk = sentinel::pinned::LibR15GetSystemPropertyIntThunk;

inline constexpr int kCounterCount = 4;

// Registers the counters. Call before sentinel::StartReporter. Returns false, having logged one line
// {"event":"texture_quality_counters","result":"refused",...}, when any was refused.
bool RegisterCounters() noexcept;

// Latches `level`, arms the handler and installs the thunk in libr15.so. `fromConfig` says whether the file gave the
// level (source "config") or the default did (source "default"). `stock` installs nothing and returns true. One line
// {"event":"texture_quality",...} says what happened. True when the hook is in place or `stock` asked for none; false
// when GotHook refused the slot or another level is already latched (the answers are then the runtime's, or the first
// level's).
bool Install(Level level, bool fromConfig) noexcept;

// Install() when `countersRegistered` (or the level is `stock`, which needs none); otherwise logs the skip and installs
// nothing.
bool InstallIfCounted(Level level, bool fromConfig, bool countersRegistered) noexcept;

// Test support: Install() with the GOT step replaced by `install`, which returns the status GotHook would, so the
// success path (the latch, the sizes, the slot mark, the log line) runs on a host that has no libr15.
using InstallFn = sentinel::GotStatus (*)() noexcept;
bool InstallVia(Level level, bool fromConfig, InstallFn install) noexcept;

// Test support: latch and arm without touching a GOT slot. Returns what the latch did (false: another level was
// already latched and stays). ResetForTest forgets the latch, the sizes and the counters, and unmarks the slot.
bool ArmForTest(Level level) noexcept;
void ResetForTest() noexcept;

// The latched level, `stock` when none.
Level LatchedLevel() noexcept;
bool IsLatched() noexcept;

struct Counts {
  unsigned long long nativeWidth, nativeHeight, served;
};
Counts CurrentCounts() noexcept;

}  // namespace nevr_quest::texture_quality
