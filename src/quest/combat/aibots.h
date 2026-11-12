/* Practice AI bots (#2185, docs/adr/0046): shipped EC never starts an AI scenario, so the Practice AI bots stand still.
 * CR15NetAIScenarios::Update does nothing until EnableScenario creates one; its only caller is a script node no Quest script contains. This calls
 * EnableScenario(scenarios, state) the way that node does, from the game tick:
 *   CR15Game+0x8510 = CR15NetGame; net state (+0) == 9 "in game"; matchiscombat (net+0x6341b) set;
 *   scenarios = net+0x2ab0 (CR15NetGame::Scenarios()); connected bot agents = u32 scenarios+0x500.
 * When the agent count is non-zero and has not changed for 3 s, EnableScenario(scenarios, scenario) once (default RandomAttack = 1); again when the
 * count changes later; re-armed when the net game leaves "in game". Bots exist only in Practice AI, so "bot count non-zero in a combat match" is the
 * offline-practice condition: arena, lobby and online matches have no agents and nothing happens.
 *
 * Which APKs (ADR 0046 rule 5). The combat variants only; with `combat::Active()` false the install step never runs.
 *
 * Switches. `combat.aibots` in config.yaml, on unless the file says false (the reference build's aibots is on in its shipped build, afk_aibots.cpp:19,145;
 * the combat plugin imported in #1591 had it on). No headset log shows the module calling EnableScenario yet (the headset runs that loaded the plugin's copy hold its start
 * line and one `idle` line, never an enable), so its headset row requires `combat_aibots_enable`. `combat.aibots_scenario` is the scenario it enables: a number 0-6 or one of patrol, randomattack (the default), weaponattack,
 * spike, aithrowtest, aionevone, aibdebugarena; `off` is the module off. It is read once, at install. A word that is none of these keeps the default
 * with a Warning (`combat_aibots_scenario`) that does not name the word. In a development build the plugin's request file `<data dir>/aibots.req`
 * (`scenario=<0-6|name|off> delay=<seconds>`) is also polled at 1 Hz and overrides both; in a release build it is not read, and one found is a
 * Warning (`combat_aibots_legacy_switch`) naming the key.
 *
 * Arena (same APK). The game's combat flag is 0 in an arena match, so the look ends after its four reads whatever the length of the match (no scenarios read, no idle
 * line, no read of the level); and the call itself is refused while the combat level answer does not rule out an Echo Arena level (combat_scope.h `SeenEchoArena`:
 * its last look saw one, or the record is not fresh: it fails closed; the module's tick runs in the same loop right after the scope row's refresh, so its
 * record is at most 250 ms old, and a stopped tick runs neither), whatever the game's flags say, with one Info line per
 * match (`combat_aibots_scoped_out`, `reason` echo_arena or no_fresh_look). The module is one of the scope row's readers and refuses its install with
 * `scope_not_live` when the answer cannot change (a scenario that is off in a release build is the module off and wins). The combat-map flag the plugin logged
 * beside the call (`combat_map`) is the combat-map bit of the same look record (fresh: a stale record refuses the call), not a copy of its level list.
 *
 * Frequency (ADR 0038). With the switch off nothing is registered: no frame runs any of this. With it on, the tick callback runs once per rendered
 * frame and does one clock read and one compare; the look runs at 4 Hz and is four process_vm_readv reads (the net game, its back pointer and
 * state, the matchiscombat byte) in an arena match, where it stops there; with bots in a combat match it is six. The module reads no level itself: at the
 * moment of a call it reads the combat level answer (a sequence-count read and a clock read). Not measured on a headset; aibots_test prints the host cost of a look.
 *
 * Origin. The reference build's aibots in kotorvr's echoquest-combat repository (commit c5a67e8): the header runtime/native/afkxr/afk_aibots.cpp:1-27,
 * the constants :52-62, the request protocol (ParseScenario :158, ReadRequest :167) and afk_aibots_tick :306-346. The combat plugin imported in #1591
 * reconstructed it (`aibots.cpp`); this file is that reconstruction. Not ported: the watcher thread and its vptr scan for the CR15Game (the game
 * tick passes it), the debug dump and aibots.done.
 *
 * Lines (sentinel log):
 *   install   {"event":"combat_aibots_install","result":"installed|off|refused","reason":"..","scenario":<-1..6>,"delay_ms":<n>,"poll":<0|1>,"msg":".."}
 *   scenario  {"event":"combat_aibots_scenario","reason":"unknown_word","msg":".."}   Warning
 *   req       {"event":"combat_aibots_req","scenario":<n>,"delay_ms":<n>}   a development build read the request file
 *   bots      {"event":"combat_aibots_bots","scenario":<n>,"bots":<n>,"msg":".."}   the agent count changed; waiting for it to hold
 *   agents    {"event":"combat_aibots_agents","bots":<n>,"msg":".."}   Warning: near the table size
 *   enable    {"event":"combat_aibots_enable","scenario_name":"..","scenario":<n>,"bots":<n>,"case":"first call|bot count changed|replacing one already set",
 *              "scenario_object":"set|not_set","host":"yes|no|unknown","combat_map":<0|1>,"delay_ms":<n>}
 *   scoped    {"event":"combat_aibots_scoped_out","bots":<n>,"reason":"echo_arena|no_fresh_look","msg":".."}   an Echo Arena level is not ruled out; EnableScenario was not called
 *   rearmed   {"event":"combat_aibots_rearmed","msg":".."}   left the match
 *   idle      {"event":"combat_aibots_idle","matchiscombat":<n>,"scenarios_object":"yes|no","scenario":<n>,"bots":<n>,"read_failed":<0|1>}   10 s in a combat match (the game's combat flag set), no call; never in an arena match
 *   legacy    {"event":"combat_aibots_legacy_switch","file":"aibots.req","replaced_by":"combat.aibots_scenario","msg":".."}   Warning
 *
 * Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <cstdint>

#include "quest/combat/combat_install.h"
#include "quest/sentinel/got_hook.h"

namespace nevr_quest::combat::aibots {

struct Fns {
  void (*enable)(void* scenarios, int state);  // CR15NetAIScenarios::EnableScenario(EScenarioState)
  int named;                                   // 1 when confirmed by its exported name (log only)
};

using SymbolLookup = void* (*)(const char* symbol);

// -1 off, 0-6 a scenario, -2 not a scenario word.
int ParseScenario(const char* text) noexcept;

// EnableScenario on a libr15 loaded at `base`: a name that resolves elsewhere, or a first word that is not `sub sp, sp, #0x30`, is a refusal (false,
// `why` set).
bool Resolve(std::uintptr_t base, SymbolLookup lookup, Fns* out, const char** why) noexcept;

// What the module is asked: the switch, the scenario text from `combat.aibots_scenario` (null: the default), and whether this is a development build
// (the request file is polled).
struct Switches {
  bool on = false;
  const char* scenario = nullptr;
  bool devBuild = false;
  bool scenarioChecked = false;  // InstallAt has already warned about an unknown scenario word (InstallResolved does not warn twice)
};

// The module's normal step. Off: one Info line, nothing registered. On: finds the loaded libr15, checks its build id and the pinned words, resolves
// EnableScenario and registers the tick callback.
ModuleResult Install(const Switches& switches) noexcept;
ModuleResult InstallAt(sentinel::ImageLookup lookup, SymbolLookup symbols, const Switches& switches) noexcept;
// The part after the resolution (a test calls it with a fake EnableScenario).
ModuleResult InstallResolved(std::uintptr_t base, const Fns& fns, const Switches& switches) noexcept;

// The tick callback (combat_tick.h): looks at the game 4 Hz.
void Tick(std::uintptr_t game) noexcept;

// 0 not installed, 1 running, 2 off or refused.
int Phase() noexcept;

struct Stats {
  std::uint32_t looks;      // looks that ran (past the clock)
  std::uint32_t enables;    // EnableScenario calls made
  std::uint32_t scopedOut;  // calls refused because an Echo Arena level was loaded
};
Stats CurrentStats() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

void SetClockForTest(double (*clock)()) noexcept;
void SetLegacyDirForTest(const char* dir) noexcept;
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::aibots
