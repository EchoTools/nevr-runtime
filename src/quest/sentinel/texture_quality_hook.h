/* The texture-quality setting (#943, #1012): the eye-buffer size libr15 creates its swap chain with.
 *
 * What the game does (pinned libr15; pinned_targets.h has the addresses): CVR::RefreshRTSize asks the headset runtime
 * for the suggested eye buffer size, vrapi_GetSystemPropertyInt id 5 (width) and id 6 (height), multiplies each by
 * CustomViewportScale (1.0) and stores the pair; CVR::Resize creates the swap chain from it and logs
 * "Creating OVR swap chain with size %i x %i (2 slices, %u sample[s])". RefreshRTSize also runs every frame, from
 * CEngine::CheckForResize, so the pair the per-frame users read follows what this hook answers.
 *
 * Two GOT hooks, one per route, and the route the live libr15 needs is decided by its bytes (refresh_eye_size.h):
 *
 *   1. The vrapi_GetSystemPropertyInt slot (JUMP_SLOT 0x36fa728), installed at every level. The handler calls the original
 *      with exactly the arguments it was given, and returns:
 *        - for id 5 and id 6, when the applied level is not `stock`, that level's width or height for the headset the runtime's
 *          own answer names (texture_quality.h HeadsetForStockSize, SizeFor);
 *        - for every other id, and for 5 and 6 at `stock`, exactly what the original returned.
 *      This is the one place the eye-size decision is made, on both inputs.
 *   2. CVR::RefreshRTSize's slot (JUMP_SLOT 0x36dbd10), installed only when the live body is the reference build's (kReference:
 *      its two vrapi calls go to a cave that never reaches the runtime). The handler reconstructs the stock body: it asks the
 *      runtime for ids 5 and 6 through the vrapi PLT stub, so route 1 answers them, and stores the pair as the game does. It never
 *      calls the original on the normal path (the original would run the cave). On the stock body no hook is installed and nothing
 *      here runs; on an unrecognised body (or a route the live bytes refuse) only the route is refused, and the vrapi hook still installs.
 *
 * The applied level. It is the level the swap chain and the render targets are built for. It starts as the configuration's
 * graphics.texture_quality (quest_config.h; `stock` when absent or wrong, texture_quality.h kDefaultLevel, kFallbackLevel),
 * latched by the first Install in the process (a second Install with another level is refused), and is replaced, once, by the
 * level the player saved in the game's settings when a start resolver is set (SetStartResolver; the first eye-size query asks
 * it, because that query comes before the game delivers its settings). On the reference input, with no saved level and a
 * `stock` configuration, the start is his rule (texture_menu_hook.cpp ResolveReference). From then on it changes only through
 * CommitApplied, which the live apply (texture_menu_hook.h) calls on the render thread right before it asks the engine to rebuild
 * everything sized from the eye size. A request alone (RequestLevel: the menu, a development property) changes no answer.
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
 * refusal (the status rides in `status`) or a second level. The `route` field says what the RefreshRTSize route did: installed,
 * refused (an unrecognised RefreshRTSize body, or pins or a slot the route cannot take: its own error line, texture_quality_route,
 * names the address and the bytes, and the vrapi hook still installs), not_needed (stock) or not_attempted; `skipped` is the counters refused. Counters (hook_report.h), registered by
 * RegisterCounters(); the first two are values, not counts, and carry the runtime's own answers (what the game would have got at
 * `stock`), which is the headset's native size:
 *   gfx_native_eye_width, gfx_native_eye_height   the runtime's last answer to id 5 and id 6
 *   gfx_eye_size_served                           answers replaced by the applied level's size
 *   gfx_thunk_faults                              the vrapi thunk had no original
 *
 * The hardware dump (sysprop_int_share.h) shares nothing with this hook: the vrapi slot is owned here (a successful install marks
 * it owned), so the dump installs its own observer and is told each query's native answer by this handler.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include "pinned_targets.h"
#include "refresh_eye_size.h"
#include "sysprop_int_share.h"
#include "texture_quality.h"

namespace nevr_quest::texture_quality {

using SystemPropertyIntThunk = sentinel::pinned::LibR15GetSystemPropertyIntThunk;
using RefreshThunk = sentinel::pinned::LibR15RefreshRTSizeThunk;

inline constexpr int kCounterCount = 4;

// "No level": the applied or requested level before anything set it.
inline constexpr int kLevelUnset = -1;

// Registers the counters. Call before sentinel::StartReporter. Returns false, having logged one line
// {"event":"texture_quality_counters","result":"refused",...}, when any was refused.
bool RegisterCounters() noexcept;

// Latches `level` as the applied level, arms the handler and installs the hooks in libr15.so. `fromConfig` says whether the
// file gave the level (source "config") or the default did (source "default"). One line {"event":"texture_quality",...} says
// what happened. True when the hook is in place; false when GotHook refused a slot, the RefreshRTSize body is not recognised or
// another level is already latched (the answers are then the runtime's, or the first level's).
bool Install(Level level, bool fromConfig) noexcept;

// Install() when `countersRegistered`; otherwise logs the skip and installs nothing.
bool InstallIfCounted(Level level, bool fromConfig, bool countersRegistered) noexcept;

// Test support: Install() with the GOT step replaced by `install`, which returns the status GotHook would, so the
// success path (the latch, the sizes, the log line) runs on a host that has no libr15.
using InstallFn = sentinel::GotStatus (*)() noexcept;
bool InstallVia(Level level, bool fromConfig, InstallFn install) noexcept;

// The GOT steps of the RefreshRTSize route and the vrapi slot (test support: a host test replaces each with a stand-in).
// `refreshRollback` takes the RefreshRTSize slot back when the vrapi step refuses after the route was installed.
struct InstallSteps {
  sentinel::GotStatus (*refresh)() noexcept;
  sentinel::GotStatus (*refreshRollback)() noexcept;
  sentinel::GotStatus (*vrapi)() noexcept;
};
// Installs over the library image at `base` the way RealInstall does, with `steps` in place of the GOT calls: classifies the
// RefreshRTSize body, installs the route on the reference body only, then the vrapi slot. An unrecognised body or a refused route
// refuses the route alone (its own error line) and the vrapi slot still installs. Returns the vrapi step's status.
sentinel::GotStatus InstallLibraryVia(unsigned char* base, const InstallSteps& steps) noexcept;

// Test support: latch and arm without touching a GOT slot. Returns what the latch did (false: another level was
// already latched and stays). ResetForTest forgets the latch, the request, the resolver, the sizes, the counters, the route and
// the test game, and unmarks the slot.
bool ArmForTest(Level level) noexcept;
void ResetForTest() noexcept;

// The applied level, `stock` when none.
Level LatchedLevel() noexcept;
bool IsLatched() noexcept;

// ---- the RefreshRTSize route (#1737 C4) ---------------------------------------------------------------------------------

// What the live libr15's RefreshRTSize body is, as Install classified it (kUnknown before any install).
EyeInput InputKind() noexcept;

// The game's pieces the reference route calls and writes, as pointers: the pair it stores, the stored override it reads first, the
// ovrJava object, CustomViewportScale and the vrapi PLT stub (the property call). Production fills them from the live library's
// base after the prologues are proved; a test supplies stand-ins through SetGameForTest.
using ViewportScaleFn = EyeVector (*)(unsigned long long index) noexcept;
using PropertyFn = int (*)(const sentinel::pinned::OvrJavaOpaque* java, int property) noexcept;
struct EyeGame {
  std::uint64_t* storedPair;
  const std::uint64_t* overridePair;
  const sentinel::pinned::OvrJavaOpaque* java;
  ViewportScaleFn scale;
  PropertyFn property;
};
void SetGameForTest(const EyeGame& game) noexcept;
void SetInputForTest(EyeInput input) noexcept;
// Test support: arms the RefreshRTSize thunk without touching its GOT slot, so HookedRefresh() answers.
void ArmRefreshForTest() noexcept;

// The entry the live apply calls in place of base+0x1a709a0 when the route is in place; nullptr when the route is not installed or
// armed (the caller then calls the game's own function).
sentinel::pinned::RefreshRTSizeFn HookedRefresh() noexcept;

// Where the last RefreshRTSize call got its size: the width and height it stored (or the override's, for `game_override`), and
// what decided them. Written by the route's handler and read by the poller.
enum class DecisionSource : int { kNone = 0, kLevel = 1, kNative = 2, kGameOverride = 3 };
struct Decision {
  int w;
  int h;
  DecisionSource source;
};
Decision LastDecision() noexcept;
const char* DecisionSourceName(DecisionSource source) noexcept;

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
