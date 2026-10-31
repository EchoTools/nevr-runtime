// Practice AI bots (q15t the source build's aibots): shipped EC never starts an AI scenario, so the Practice AI bots stand
// still. CR15NetAIScenarios::Update does nothing until EnableScenario creates one; its only caller is a script node no
// Quest script contains. This calls EnableScenario(scenarios, state) the way that node does, from the game tick:
//   CR15Game+0x8510 = CR15NetGame; net state (+0) == 9 "in game"; matchiscombat (net+0x6341b) set;
//   scenarios = net+0x2ab0 (CR15NetGame::Scenarios()); connected bot agents = u32 scenarios+0x500.
// When the agent count is non-zero and has not changed for `delay` (default 3 s), EnableScenario(scenarios, scenario)
// once (default RandomAttack = 1); again when the count changes later; re-armed when the net game leaves "in game".
// Bots exist only in Practice AI, so "bot count non-zero in a combat match" is the offline-practice condition: arena,
// lobby and online matches have no agents and nothing happens. Default ON (the source build's default).
// Optional request file <data dir>/aibots.req: `scenario=<0-6|patrol|randomattack|weaponattack|spike|aithrowtest|aionevone|
// aibdebugarena|off> delay=<seconds>`, re-read when its mtime changes (1 Hz). Not ported: debug=1 and aibots.done.
// Game-library calls only (EnableScenario at its fixed address, cross-checked against its exported name and its first word).
#pragma once

#include <cstdint>

namespace q15t {

struct AiBotsFns {
  void (*enable)(void* scenarios, int state);  // CR15NetAIScenarios::EnableScenario(EScenarioState)
  int named;                                   // 1 when confirmed by its exported name (log only)
};

// `lookup`: dlsym on the loaded game library (nullptr: fixed address only). A name that resolves elsewhere, or a first
// word that is not `sub sp, sp, #0x30` (0xd100c3ff; disassembly 0x133c2bc: ffc300d1), is a refusal (false, `why` set).
bool ResolveAiBotsFns(std::uintptr_t base, void* (*lookup)(const char* symbol), AiBotsFns* out, const char** why);

// An all-null `fns` means "resolve from the loaded libr15 on the first tick".
void InitAiBots(std::uintptr_t base, const AiBotsFns& fns);

// A TickFn (game_tick.h). Looks at the game 4 Hz.
void AiBotsTick(std::uintptr_t game);

// 0 not started, 1 running, 2 off
int AiBotsPhase();

void SetAiBotsClockForTest(double (*clock)());
void SetAiBotsDirForTest(const char* dir);
void ResetAiBotsForTest();

}  // namespace q15t
