/* The in-headset sign-in prompt (issue #239): the game's own login-error screen.
 *
 * What the game does (pinned libr15; pinned_targets.h has the addresses): a failed login reaches
 * CR15NetGame::LogInFailedCB, which hands the message to SetDelimitedErrorMessage (the error block
 * in game_layout), switches to "login failed" (-94) and queues QuitOnError, whose component event
 * takes the UI to its error screen; the UI script reads the block through
 * CR15NetErrorMessageExpression. A new login is started by the UI script (CR15NetBeginLoginNode
 * calls BeginLogIn), not by CR15NetGame itself; "logged out" (0) is entered only by
 * CR15NetGame::LogOut, whose one call is in ~CR15NetGame.
 *
 * Two GOT hooks in libr15.so, both on the thread the game runs them on, both lock-free:
 *   - SetDelimitedErrorMessage: the game stores and logs its own message first (no code ever
 *     passes through the game's logging). Then, if the game was logging in (state 2), the message
 *     is one of the game's own local login-failure texts (never a server-sent message, so a ban or
 *     suspension text always shows), and the prompt board holds a prompt (Mode::kPrompt), the
 *     prompt is written over the error block after checking the block holds the game's message,
 *     and the game's block is saved.
 *   - CR15NetGame::Update (once per game update): while that same object is still in "login failed"
 *     and the board has changed since it was written (a new code, the signed-in notice, the
 *     timed-out text, or withdrawn), the block is rewritten, or the game's saved block restored.
 *     Whether the UI re-reads the block while the error screen is up is not known from the binary;
 *     the next login failure writes the current text either way.
 *
 * It never logs on the game's call path. Its eight counters (hook_report.h), registered by
 * RegisterCounters():
 *   login_prompt_text_shown         a login failure's message was replaced by the prompt
 *   login_prompt_text_refreshed     a prompt on screen was rewritten or restored after a change
 *   login_prompt_text_kept          a local login failure kept the game's text: nothing, or only a
 *                                   notice, was published
 *   login_prompt_text_not_local     not a local login failure while logging in (a server message,
 *                                   a profile or removal error): the game's text, untouched
 *   login_prompt_board_busy         the board stayed busy through every read attempt
 *   login_prompt_layout_mismatch    the error block did not hold the game's message: nothing written
 *   login_prompt_error_thunk_faults / login_prompt_update_thunk_faults  a thunk had no original
 *
 * Integration surface (the startup sequence owns the order):
 *   1. RegisterCounters()  before sentinel::StartReporter. Returns false, after one JSON line, when
 *                          the reporter refused any of the eight; then do not Install.
 *   2. Install()           after token auth is created (QuestTokenAuth publishes to the prompt
 *                          board itself; nothing else needs attaching). Both slots are in
 *                          libr15.so, which is relocated before the sentinel's constructor runs,
 *                          so it does not wait for the dlopen hook and does not depend on the login
 *                          hook. Logs one JSON line with both install results.
 * The prompt board is a process global (src/quest/auth/prompt_board.h); token auth and these hooks
 * must link into the same shared object, as they do in libovrplatformloader.so.
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

// Arms both handlers without touching a GOT slot: for the host test, which publishes originals
// through the thunks' OriginalOut() and calls their EntryFn() directly.
void ArmForTest() noexcept;

// The counters' current values, in the order of the comment above.
struct Counts {
  std::uint64_t shown, refreshed, kept, not_local, busy, layout_mismatch;
};
Counts CurrentCounts() noexcept;

// Whether `message` is one of the game's own local login-failure texts (libpnsovr / libr15
// "Log in request failed: ..." literals, passed only to CNSUser::LogInFailed).
bool IsLocalLoginFailure(const char* message) noexcept;

}  // namespace nevr_quest::login_prompt
