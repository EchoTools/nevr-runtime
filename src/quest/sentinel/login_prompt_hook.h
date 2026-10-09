/* The in-headset sign-in prompt (issue #239): the game's own login-error screen.
 *
 * What the game does (pinned libr15; pinned_targets.h has the addresses): a failed login reaches
 * CR15NetGame::LogInFailedCB, which hands the message to SetDelimitedErrorMessage (the error block
 * in game_layout), switches to "login failed" (-94) and queues QuitOnError, which has three paths:
 * with CR15Game+0x1f08 set it only marks itself pending (bit 0x200000 of the flags at
 * CR15NetGame+0x2da0) and returns; otherwise it ends multiplayer and then sends the game space a
 * component event when CR15Game+0x7af0 is set, or sets the flag CR15Game::UpdateGame also sets when
 * CVR::ShouldQuit() is true (a quit request). Which one runs on a headset is not measured. The UI
 * script reads the
 * block through CR15NetErrorMessageExpression. A new login is started by the UI script
 * (CR15NetBeginLoginNode calls BeginLogIn); "logged out" (0) is entered only by
 * CR15NetGame::LogOut, called only from ~CR15NetGame, called only from CR15Game::ShutdownEngine.
 * The prompt therefore has to be useful also if the game quits after the failure: the player
 * restarts it and sees a new code (or logs in with the cached sign-in).
 *
 * Two GOT hooks in libr15.so, both lock-free. Both run on the game loop's thread: CncaGame::RunLoop
 * calls CR15Game::Update, which updates the login providers and the broadcaster (where login
 * failures arrive) and then CR15NetGame::Update (through CncaGame::Update and CR15Game::UpdateGame);
 * the writer flag below does not depend on that.
 *   - SetDelimitedErrorMessage: the game stores and logs its own message first (no code ever
 *     passes through the game's logging). If the game was logging in (state 2), the message is one
 *     of the game's own local login-failure texts (quest/game_login_failures.h; never a
 *     server-sent message, so a ban or suspension text always shows) and the block holds that
 *     message, the instance is followed from then on and the game's block is saved; if the prompt
 *     board holds a prompt (Mode::kPrompt), it is written over the block now.
 *   - CR15NetGame::Update (once per game update): while the followed instance is still in "login
 *     failed" and the board or the block has changed since the block was written (a prompt
 *     published after the failure, a new code, a notice in place of a prompt, the timed-out text,
 *     or withdrawn), the block is rewritten, or the game's saved block restored. A notice never
 *     replaces the game's own text. It checks every frame that the block still holds what this
 *     hook last left there: the game has other writers of the block (lobby, lobby-status and
 *     game-space errors, for example), and a block one of them changed is left alone for good,
 *     unless it holds one of the local login-failure texts again (a new failure the error hook
 *     could not take up), which becomes the game's text to follow. Whether the UI re-reads the
 *     block while the error screen is up is not known from the binary; the next login failure
 *     writes the current text either way.
 *
 * It never logs on the game's call path. Its eight counters (hook_report.h), registered by
 * RegisterCounters():
 *   login_prompt_text_shown         a login failure's message was replaced by the prompt
 *   login_prompt_text_refreshed     the followed block was rewritten or restored after a change
 *   login_prompt_text_kept          a local login failure kept the game's text for now: nothing,
 *                                   or only a notice, was published (the instance is followed)
 *   login_prompt_text_not_local     not a local login failure while logging in (a server message,
 *                                   a profile or removal error): the game's text, untouched
 *   login_prompt_board_busy         the board, or the other hook, was busy at the failure (the
 *                                   instance is followed when the board was the busy one)
 *   login_prompt_block_not_ours     the block did not hold the game's message at the failure, or
 *                                   another writer changed it later: nothing written, not followed
 *   login_prompt_error_thunk_faults / login_prompt_update_thunk_faults  a thunk had no original
 *
 * Integration surface. The sentinel's own entry.cpp installs the hooks, but nothing in it creates
 * QuestTokenAuth, so there the board stays empty and the game's text always passes through. A
 * startup sequence that links token auth into the same shared object and creates it
 * (QuestTokenAuth::Create, whose presenters publish to the board) calls, in this order:
 *   1. RegisterCounters()  before sentinel::StartReporter. Returns false, after one JSON line, when
 *                          the reporter refused any of the eight.
 *   2. InstallIfCounted(<result of 1>)  after QuestTokenAuth::Create; nothing else needs
 *                          attaching. Skips, with one JSON line, when the counters were refused;
 *                          otherwise Install(). Both slots are in
 *                          libr15.so, which is relocated before the sentinel's constructor runs,
 *                          so it does not wait for the dlopen hook and does not depend on the login
 *                          hook. Logs one JSON line with both install results.
 * The prompt board is a process global (src/quest/auth/prompt_board.h): token auth and these hooks
 * share it only when they are linked into the same shared object.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstdint>

#include "pinned_targets.h"

namespace nevr_quest::login_prompt {

using ErrorThunk = sentinel::pinned::LibR15SetDelimitedErrorMessageThunk;
using UpdateThunk = sentinel::pinned::LibR15NetGameUpdateThunk;

inline constexpr int kCounterCount = 8;

// Registers the counters. Call before sentinel::StartReporter. Returns false, having logged one
// line {"event":"login_prompt_counters","result":"refused",...}, when any was refused.
bool RegisterCounters() noexcept;

// Arms both handlers and installs both thunks in libr15.so. A refused install is logged by GotHook
// and leaves that game call intact; one line {"event":"login_prompt_install",...} says what was
// installed. Returns true only when both slots hold their thunks.
bool Install() noexcept;

// Install() when `countersRegistered`; otherwise logs {"event":"login_prompt_install","result":
// "skipped","why":"counters_refused"} and installs nothing. Returns whether both slots hold thunks.
bool InstallIfCounted(bool countersRegistered) noexcept;

// Arms both handlers without touching a GOT slot: for the host test, which publishes originals
// through the thunks' OriginalOut() and calls their EntryFn() directly.
void ArmForTest() noexcept;

// Test support: takes / gives back the hooks' single-writer flag, as the other hook would hold it.
// Returns false when it was already taken. Nothing in production calls these.
bool HoldBlockWriterForTest() noexcept;
void ReleaseBlockWriterForTest() noexcept;

// The counters' current values, in the order of the comment above.
struct Counts {
  std::uint64_t shown, refreshed, kept, not_local, busy, not_ours;
};
Counts CurrentCounts() noexcept;

// Whether `message` is exactly one of quest/game_login_failures.h's texts.
bool IsLocalLoginFailure(const char* message) noexcept;

}  // namespace nevr_quest::login_prompt
