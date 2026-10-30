/* The texture-quality setting (#943, #1012): the eye-buffer size libr15 creates its swap chain with.
 *
 * What the game does (pinned libr15; pinned_targets.h has the addresses): CVR::RefreshRTSize asks the headset runtime
 * for the suggested eye buffer size, vrapi_GetSystemPropertyInt id 5 (width) and id 6 (height), multiplies each by
 * CustomViewportScale (1.0) and stores the pair; CVR::Resize creates the swap chain from it and logs
 * "Creating OVR swap chain with size %i x %i (2 slices, %u sample[s])". RefreshRTSize also runs every frame, from
 * CEngine::CheckForResize, so the pair the per-frame users read follows what this hook answers.
 *
 * One GOT hook, on libr15's vrapi_GetSystemPropertyInt slot (JUMP_SLOT 0x36fa728), installed at every level. The handler calls
 * the original with exactly the arguments it was given, and returns:
 *   - for id 5 and id 6, when the applied level is not `stock`, that level's width or height for the headset the runtime's own
 *     answer names (texture_quality.h HeadsetForStockSize, SizeFor);
 *   - for every other id, and for 5 and 6 at `stock`, exactly what the original returned.
 *
 * The applied level. It is the level the swap chain and the render targets are built for. It starts as the configuration's
 * graphics.texture_quality (quest_config.h; `stock` when absent or wrong, texture_quality.h kDefaultLevel, kFallbackLevel),
 * latched by the first Install in the process (a second Install with another level is refused), and is replaced, once, by the
 * level the player saved in the game's settings when a start resolver is set (SetStartResolver; the first eye-size query asks
 * it, because that query comes before the game delivers its settings). From then on it changes only through CommitApplied,
 * which the live apply (texture_menu_hook.h) calls on the render thread right before it asks the engine to rebuild everything
 * sized from the eye size. A request alone (RequestLevel: the menu, a development property) changes no answer.
 *
 * One path the hook does not cover: CVR::RefreshRTSize skips the query when the stored size override (ELF 0x37a33e8, set
 * only by CVR::SetRTSizeOverride) is non-zero, so no level applies there. The live apply detects it: SizeQueries() does not
 * move across a RefreshRTSize call.
 *
 * It never logs on the game's call path. Install logs one line, whatever the level and the outcome:
 *   {"level":"info","event":"texture_quality","result":"installed|refused|skipped",
 *    "quality":"high","source":"config|default","w":0,"h":0,"sizes":"per_headset",...}
 * (`level` is the log severity; the setting is `quality`.) `w` and `h` are 0 on an installed hook: the size depends on
 * the headset, which the first query names, so the swap-chain line is the record of the size served. `refused` is a GotHook
 * refusal (the status rides in `status`) or a second level; `skipped` is the counters refused. Counters (hook_report.h),
 * registered by RegisterCounters(); the first two are values, not counts, and carry the runtime's own answers (what the game
 * would have got at `stock`), which is the headset's native size:
 *   gfx_native_eye_width, gfx_native_eye_height   the runtime's last answer to id 5 and id 6
 *   gfx_eye_size_served                           answers replaced by the applied level's size
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

// "No level": the applied or requested level before anything set it.
inline constexpr int kLevelUnset = -1;

// Registers the counters. Call before sentinel::StartReporter. Returns false, having logged one line
// {"event":"texture_quality_counters","result":"refused",...}, when any was refused.
bool RegisterCounters() noexcept;

// Latches `level` as the applied level, arms the handler and installs the thunk in libr15.so. `fromConfig` says whether the
// file gave the level (source "config") or the default did (source "default"). One line {"event":"texture_quality",...} says
// what happened. True when the hook is in place; false when GotHook refused the slot or another level is already latched
// (the answers are then the runtime's, or the first level's).
bool Install(Level level, bool fromConfig) noexcept;

// Install() when `countersRegistered`; otherwise logs the skip and installs nothing.
bool InstallIfCounted(Level level, bool fromConfig, bool countersRegistered) noexcept;

// Test support: Install() with the GOT step replaced by `install`, which returns the status GotHook would, so the
// success path (the latch, the sizes, the slot mark, the log line) runs on a host that has no libr15.
using InstallFn = sentinel::GotStatus (*)() noexcept;
bool InstallVia(Level level, bool fromConfig, InstallFn install) noexcept;

// Test support: latch and arm without touching a GOT slot. Returns what the latch did (false: another level was
// already latched and stays). ResetForTest forgets the latch, the request, the resolver, the sizes and the counters, and unmarks the slot.
bool ArmForTest(Level level) noexcept;
void ResetForTest() noexcept;

// The applied level, `stock` when none.
Level LatchedLevel() noexcept;
bool IsLatched() noexcept;

// ---- the level while the game runs (#1012) -----------------------------------------------------------------------------

// What the first eye-size query asks, once: the Level (as an int) the player saved in the game's settings, or kLevelUnset when
// there is none. The handler replaces the configuration's level with it (the player's choice outlives the configuration's
// default). It runs inside the game's call: no logging, no exceptions, no allocation it cannot do without. A resolver set after
// the first query is not asked.
using StartResolver = int (*)() noexcept;
void SetStartResolver(StartResolver resolver) noexcept;

// Where a request came from, for the log line the non-hook thread writes.
enum class RequestSource : int { kMenu = 0, kProperty = 1 };

// Asks for a level. The request is the level the game should be rebuilt at; nothing happens until the live apply commits it.
void RequestLevel(Level level, RequestSource source) noexcept;
// kLevelUnset until something asked, else the Level.
int RequestedLevelValue() noexcept;
RequestSource LastRequestSource() noexcept;

// Makes `level` the applied level: the next answers to ids 5 and 6 are its sizes. Only the live apply calls this, on the render
// thread, right before it rebuilds.
void CommitApplied(Level level) noexcept;

// The number of eye-size queries (ids 5 and 6) the handler has seen; the live apply compares it across a RefreshRTSize call.
unsigned long long SizeQueries() noexcept;

struct Counts {
  unsigned long long nativeWidth, nativeHeight, served;
};
Counts CurrentCounts() noexcept;

}  // namespace nevr_quest::texture_quality
