/* NEVR_OUTSIDE_GAME_CALL: the one explicit exemption from the hook-frame rule.
 *
 * The rule (callback_thunk.h, rule 1): a frame that is live while game code runs under a hook
 * must be personality-free ("zR"). The build-time frame sensor (tests/quest
 * TestHookFramesCarryNoPersonality) starts from every hook record's entry and handler, follows
 * direct bl/b edges, and fails on any reachable function whose CIE names a personality.
 *
 * Some code a handler calls DIRECTLY is not live across a game call: it runs entirely before or
 * after the call into the game, it calls no game code, and it lets no exception out. Such a
 * function may allocate, throw and catch. Mark its DEFINITION with this macro:
 *
 *   NEVR_OUTSIDE_GAME_CALL void AfterDlopen(const char* name, void* handle) noexcept { ... }
 *
 * The macro places the function in the output section `nevr_outside_game_call`. The sensor reads
 * that section from the built library, does not enter a function inside it and does not follow its
 * callees, and lists every such function it reached so a reviewer sees the full set. This is the
 * only exemption: there is no name list in the sensor.
 *
 * What the annotation asserts, which the sensor cannot check, so a reviewer must:
 *   1. the function (and everything it calls) never calls into game code, directly or through a
 *      function pointer, a virtual call or a callback it registers for the game to run;
 *   2. no exception leaves it (it is `noexcept` or catches every exception it can raise, by named
 *      type);
 *   3. it is not on the stack across any call into game code: its caller reaches it before or after
 *      the game call, never from inside a callback the game runs.
 * A function that is live across a game call and carries a personality is a violation however it
 * is marked: do not annotate it, restructure it (split the work, as login_rewrite.cpp does).
 *
 * The section name is a C identifier on purpose, so the linker defines __start_/__stop_ symbols
 * for it; nothing uses them at run time.
 */
#pragma once

#define NEVR_OUTSIDE_GAME_CALL __attribute__((noinline, section("nevr_outside_game_call")))
