/* The Texture Resolution control in the game's settings menu, and the level changing while the game runs (#1012, ADR 0015).
 *
 * The design of the control and of the live change is marshmallow-mia's (EchoTools/echoApkPatch, graphics v9 and v10); only facts
 * were taken from it. Every function below was read in the disassembly (the Quest libr15) and is hooked through a GOT slot libr15 calls
 * it by: no code byte is written (docs/adr/0021), none of the patch repository's caves or addresses is used.
 *
 * Five GOT hooks (pinned_targets.h has the slots, addresses and the evidence for each):
 *
 *   CJson::DecodeFrom(CFileSp const&, uint)    the game reads json/r14/config/gamesettings_config.json and
 *                                              .../uisettings/menu_settings.json through it. The handler arms the calling thread
 *                                              for those two paths and disarms it when the call returns.
 *   CJson::DecodeFrom(char const*, ull)        the parse of the text that call has read and stripped of comments. On an armed
 *                                              thread the text is rewritten (texture_menu.h) and the game parses that. Every other
 *                                              call, on any thread, is passed through untouched.
 *   ValidateGfxSettings(SR15GfxSettings&)      clamps `graphics|msaa` to 0..2. The handler restores the value the settings held,
 *                                              clamped to the menu's 0..5, so six levels can be saved and read back.
 *   ApplyRuntimeGfxSettings(settings, ...)     the game delivers its settings here at boot and on every menu change. The handler
 *                                              reads `graphics|msaa` (SR15GfxSettings+0x1c) and asks for that level.
 *   CEngine::CheckForResize()                  once per rendered frame, on the render thread. The handler runs the original and
 *                                              then, when a level other than the applied one was asked for, the engine's own
 *                                              rebuild: CVR::RefreshRTSize (the eye-size route answers it with the new level: the
 *                                              reference input's RefreshRTSize hook when installed, else the game's own function)
 *                                              and CEngine::Resize, which recreates the swap chain, the render targets and the pass
 *                                              framebuffers.
 *
 * The two JSON hooks and the rest are installed together or not at all: the control is offered only when the three hooks that make
 * it work are in place.
 *
 * What a player meets, from one setting. The settings menu has a GRAPHICS page with one control, "Texture Resolution", of six
 * entries (Very Low, Low, Medium, High, Very High, Ultra), default Low (the headset's stock size, or the level the configuration
 * names). A change applies at the next rendered frame. The choice is remembered in a marker file of our own,
 * nevr_texture_quality.json beside the game's settings (texture_menu.h), written by the poller after the player changes the control
 * and read back at the next launch, before the swap chain is made (ResolveStart reads it, because the game delivers its settings
 * after that). The game's own saved slot (`msaa` in settings_mp_v2_quest.json) is not read: a settings file that never held the
 * setting reads 0, which is Very Low, and cannot say whether the player chose. A player who never chose starts at the default: the
 * configuration's graphics.texture_quality, Low for `stock`; on the reference libr15 with a `stock` configuration it is the reference
 * build's own start rule (its settings file's `msaa` index, default High), queued once as the marker. The game's first delivery of its
 * settings is changed to the marker's level, or to that default when there is no marker, so the menu shows it. A marker always wins
 * over the configuration.
 *
 * What the apply refuses, and why. It waits for the engine's ready flag and for the VR layer to be initialised and not
 * display-lost (CheckForResize itself and ApplyRuntimeGfxSettings test the same). It refuses for good when the engine's resize
 * slot is not CEngine::Resize, or when RefreshRTSize did not ask the runtime (the stored size override, texture_quality_hook.h).
 * A rebuild that fails is undone: the previous level is put back, the engine rebuilt at it, and the failed level is not asked
 * for again. Two levels that give the same eye size (`stock` and Low on one headset) are committed without a rebuild.
 *
 * Nothing on the game's call path logs. A poller thread (`nevr-texture`) wakes twice a second, polls the development property
 * `debug.nevr.texture_level` (a level word or a menu index 0..5, set from adb), and writes one line for the start level, one for
 * each request and one for each change of the applied level; counters (hook_report.h) carry the rest.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include "got_hook.h"
#include "texture_quality.h"
#include "texture_quality_hook.h"

namespace nevr_quest::texture_menu {

inline constexpr int kCounterCount = 13;

// The development property a level can be set through while the game runs (a level word, or a menu index 0..5).
inline constexpr const char* kLevelProperty = "debug.nevr.texture_level";

// Registers the counters. Call before sentinel::StartReporter, with texture_quality::RegisterCounters.
bool RegisterCounters() noexcept;

// Proves libr15 (build ID, then the first words of every function the apply calls), installs the five hooks, sets the start
// resolver and starts the poller. `level` is the configuration's level and `fromConfig` says whether the file gave it. One
// line {"event":"texture_menu_install",...} says what happened. True when all five hold their thunks; false (the control is not
// offered, the level is fixed at launch) otherwise.
bool Install(texture_quality::Level level, bool fromConfig) noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

// The game functions the apply calls. Production resolves them from libr15; a test supplies stand-ins.
struct Game {
  sentinel::pinned::RefreshRTSizeFn refreshRtSize = nullptr;
  sentinel::pinned::VrFlagFn vrInitialized = nullptr;
  sentinel::pinned::VrFlagFn vrGraphicsInitialized = nullptr;
  sentinel::pinned::VrFlagFn vrDisplayLost = nullptr;
  sentinel::pinned::LocalAppDataDirectoryFn localAppDataDirectory = nullptr;
  sentinel::pinned::EngineResizeFn engineResize = nullptr;  // the function the engine's resize slot must hold
};

// Where the development property is read: copies the value into `out` (at least 92 bytes) and returns its length, or 0.
using PropertyReader = int (*)(const char* name, char* out) noexcept;

// Arms the handlers without touching a GOT slot: the game functions, the configuration's level, the property reader, and the
// start resolver. ResetForTest forgets all of it (not the thunks' own state: reset those through their Reset()).
void ArmForTest(const Game& game, texture_quality::Level configLevel, bool fromConfig, PropertyReader property) noexcept;
void ResetForTest() noexcept;

// One poller pass, as the thread runs it: the property, then the lines. Returns the number of lines written.
int PollOnceForTest() noexcept;

struct Totals {
  unsigned long long deliveries;       // ApplyRuntimeGfxSettings calls seen
  unsigned long long jsonRewrites;     // documents rewritten
  unsigned long long applies;          // rebuilds that succeeded
  unsigned long long equalCommits;     // level changes committed without a rebuild (same eye size)
  unsigned long long refused;          // applies refused for good
  unsigned long long failed;           // rebuilds the engine reported as failed
  unsigned long long configOverrides;  // first deliveries changed to the default level (the player had not chosen)
  unsigned long long markerWrites;     // the player's choices written to the marker file
  unsigned long long markerFailures;   // the player's choices that could not be written
  bool disabled;                       // the live apply is off for this process
  const char* disabledWhy;
};
Totals CurrentTotals() noexcept;

}  // namespace nevr_quest::texture_menu
