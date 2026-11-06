/* Combat music: one instance at a time (#2184, docs/adr/0046; moved from the q15t plugin's `music`).
 *
 * What the game does. Two Wwise music trees of the combat bank start with a script Play (capture maps `2f22c384`, payload maps `b1b06cfb`); the only
 * Stop of either is posted from the script's return-to-lobby listener. A way out of a match that does not raise that event leaves the old instance
 * playing, and the next match's Play starts a second one on top of it (a tester heard the music twice, 2026-10-28).
 *
 * What this does. A thunk on libr15's JUMP_SLOT for CWwiseEngine::PostSoundEvent (music_targets.h). For one of the two music Play events, while a
 * combat map is loaded, it first posts BOTH music Stop events on the same game object (and on the previous Play's object when that differs) through
 * the game's own function, then lets the Play through unchanged. With nothing playing a Stop is a no-op, so a normal lobby, match, lobby round is
 * unchanged. A tick callback (combat_tick.h) posts the two Stops once when no combat map is loaded any more after a Play. No code word is written.
 *
 * Which APKs (ADR 0046 rule 5). The q15t and combat variants only: with `combat::Active()` false the step never runs.
 *
 * Switch. `combat.music` in config.yaml, true unless the file says false (kotorvr's shipped build has it on: seat-brief UPSTREAM-DEFAULTS row `music`, `afk_music.cpp:49`;
 * the q15t plugin's `Q15T_MUSIC` was on). No headset session of ours has yet shown it acting (the q15t sessions reach no combat map), so its headset row requires
 * `combat_music_first` and `combat_music_play` (docs/quest/combat/music.md). The plugin's kill file `music.off` is no longer read: one found in the plugin's data
 * directory is a Warning (`combat_music_legacy_switch`).
 *
 * Arena (same APK). Arena has its own music (`mp_arena_music`), which must stay as stock. PostSoundEvent is the game's one post for every sound
 * event of every level, so the handler first tests the event against the 24 ids of the combat music bank (a binary search of a constant table); any other
 * event, which is every sound arena plays, goes straight to the game's function. The two Play ids start a combat tree only: the handler asks the level
 * (combat_level.h Classify, a few process_vm_readv reads) once per Play, never per post, and acts only when the loaded levels hold a combat map and no
 * Echo Arena level. The ask reads the init-fiber table too (`inFlight`), so a Play posted while its combat map is still building, in a map-to-map change where the
 * previous map is still loaded, posts the Stops (upstream posts them on every Play). A Play with no combat map loaded is counted and passed through. The arena
 * path is proved by count: music_test `ArenaPaysNothingByCount` asserts zero `SafeReadCalls()` and zero `level::ScanCalls()` for every sound outside the bank.
 *
 * Frequency (ADR 0038). PostSoundEvent runs for every sound the game posts, on the audio-posting threads, many times per frame in a busy scene (not measured on
 * a headset). The added work on a post that is not in the bank is the thunk's counter and loads plus 5 compares; a bank post (24 ids, 10-20 a match) adds
 * one atomic add; a Play adds the level scan and up to 4 Stop posts. The tick callback runs once per rendered frame: one relaxed load when no Play
 * has happened, and, from a Play until the music is stopped at the end of the match, one clock read and, twice a second, the level scan.
 *
 * Owner (ADR 0039). This file owns stopping the combat music trees before a new Play and when the combat map unloads. The arguments and the result of
 * PostSoundEvent are not changed.
 *
 * Origin. kotorvr's echoquest-combat music module: runtime/native/afkxr/afk_music.cpp (commit c5a67e8, the whole file: the bank at :51-60, the wrapper
 * at :83-125, the unload tick at :128-190). Its combat-map scope is ours: upstream acts on every Play.
 *
 * Lines (sentinel log):
 *   install  {"event":"combat_music_install","state":"on|off|refused","reason":"default|off_config|<refusal>","switch":"combat.music","scope":"combat_map",
 *             "status":"<GotStatus>","msg":".."}   Info on and off; Error when refused (module_not_loaded, no_build_id, build_id_mismatch, pin_mismatch, tick_table_full, hook)
 *   first    {"event":"combat_music_first","kind":"play|stop|state","music_event":"0x%08x","object":<n>,"in_combat_map":<0|1>,"msg":".."}  Info, once, the first bank post
 *   play     {"event":"combat_music_play","music_event":"0x%08x","object":<n>,"prev_object":<n>,"stops_posted":<n>,"in_combat_map":<0|1>,"n":<n>,"msg":".."}
 *            Info, a Play, the first kMaxPlayLines of the process
 *   unload   {"event":"combat_music_unload","object":<n>,"stops_posted":<n>,"bank_posts":<n>,"plays":<n>,"plays_out_of_scope":<n>,"msg":".."}  Info, the combat map is gone
 *   legacy   {"event":"combat_music_legacy_switch","file":"music.off","replaced_by":"combat.music: false","msg":".."}  Warning
 *
 * Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/combat/combat_install.h"
#include "quest/combat/music_targets.h"
#include "quest/sentinel/got_hook.h"

namespace nevr_quest::combat::music {

enum class Kind : std::uint8_t { kNone, kPlay, kStop, kState };

// The combat music bank: 2 Play, 2 Stop, 20 SetState events (kotorvr afk_music.cpp:54-58).
inline constexpr std::uint32_t kPlay[2] = {0x2f22c384, 0xb1b06cfb};
inline constexpr std::uint32_t kStop[2] = {0xde586538, 0x2acba759};
inline constexpr std::uint32_t kState[20] = {0x045d3c23, 0x215e97af, 0x4a61827d, 0x6a599cee, 0x7a9959a5, 0x98a1b868, 0xa3117938,
                                             0xaee8f745, 0xd3282d2b, 0xd9a48db0, 0xd9a48db1, 0xd9a48db3, 0xdb7c7cf9, 0xe34a79b4,
                                             0xe34a79b5, 0xe34a79b6, 0xe44bd2d0, 0xf0a25298, 0xf7898c0b, 0xffae8834};
inline constexpr std::size_t kMaxPlayLines = 32;
inline constexpr std::size_t kTickPeriodMs = 500;

// What a music-bank event is; kNone for every other event (every sound arena plays).
Kind Classify(std::uint32_t event) noexcept;

// The module's normal step: installs the thunk on the loaded libr15 when `configOn`, registers the tick callback, writes the install line.
ModuleResult Install(bool configOn) noexcept;
// The install on the libr15 the lookup finds (the host test supplies an image built in memory).
ModuleResult InstallAt(bool configOn, sentinel::ImageLookup lookup, const targets::Pins& pins) noexcept;

// The tick callback (combat_tick.h `TickFn`), registered by the install; exposed for the host test.
void Tick(std::uintptr_t game) noexcept;

struct Stats {
  std::uint64_t bankPosts;        // music-bank posts seen
  std::uint64_t plays;            // Play posts seen
  std::uint64_t playsActed;       // of those, with a combat map loaded: the Stops were posted first
  std::uint64_t playsOutOfScope;  // of those, with no combat map (or an Echo Arena level): passed through untouched
  std::uint64_t stopsPosted;      // extra Stop posts, by the handler and by the tick
  std::uint64_t tickStops;        // unloads the tick stopped the music for
};
Stats CurrentStats() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

// Replaces the millisecond clock the tick's schedule reads (nullptr: CLOCK_MONOTONIC).
void SetClockForTest(std::int64_t (*clockMs)() noexcept) noexcept;
// The directory the legacy kill file is looked for in (nullptr restores production).
void SetLegacyDirForTest(const char* dir) noexcept;
// Forgets the hook, the thunk, the state, the counters and the log budget.
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::music
