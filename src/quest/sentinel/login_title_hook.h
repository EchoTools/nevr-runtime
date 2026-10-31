/* The title of the login-failed page while the player is asked to sign in (issue #1137).
 *
 * What the game does: the login-failed page is the one the sign-in code is shown on (login_prompt_hook.h), and the page
 * is titled LOGIN FAILED whatever the reason. The title is not part of the error block our prompt writes. It is a literal
 * in the page's script library (lib7f2363b8976491aa.so): one script node, execute_node_cfa11df1 (disassembly 0x10d0e8), builds
 * `string_t("LOGIN FAILED")` and passes it to EXTERNALFUNCTIONNODE::setparam as the page's title parameter. On a Quest that
 * is not signed in yet, this reads as "the game is broken" on the first screen a new player sees.
 *
 * The seam. `string_t::string_t(char const*)` is defined in that library (disassembly 0x10df98: NRadString::Copy(this, text,
 * 0x40), so it copies at most 63 characters into the string_t's own 64-byte buffer and keeps no pointer) and is reached
 * through the library's own JUMP_SLOT (pinned_targets.h, LoginScriptStringCtor). The node is the slot's only caller
 * (callers of 0x10df98 and 0x104a90 in the disassembly: one edge), so the thunk runs once each time that page's title is built, and
 * never for any other string in the game. The alternative seam, the title parameter's `setparam` call, goes through a
 * GLOB_DAT function-pointer variable (0x12ea0) that every script node in the library calls through, and it hands over a
 * string_t whose layout we would have to write; here the argument is a `char const*` and the game builds the string.
 *
 * One GOT hook, lock-free, on the thread that runs the script node. When the text is exactly "LOGIN FAILED" and the prompt
 * board carries a title (prompt_board.h; token auth publishes "SIGN IN" with the code and "SIGNED IN" with the signed-in
 * notice, prompt_presenters.h), the original constructor is handed the board's title in place of the literal. Every other
 * case calls the original with the game's own text: a string that is not the literal (the slot is pinned by module, symbol
 * and address, with no build ID to prove the library, so the exact literal is the identity check), and the literal with no
 * title on the board (nothing published, a timed-out or no-code text, a real failure the prompt does not cover). It never
 * logs on the game's call path. Its counters (hook_report.h), registered by RegisterCounters():
 *   login_title_string_calls      the slot was entered (the thunk's own count: one per title the page builds)
 *   login_title_replaced          the literal was replaced by the board's title
 *   login_title_kept              the literal was passed on because the board carried no title
 *   login_title_other_text        the string was not the literal
 *   login_title_thunk_faults      the thunk had no original
 *
 * Installed after the library loads (integration/post_load.h, the `loginTitle` action): the sentinel's constructor runs
 * before the game maps its script libraries.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstdint>

#include "pinned_targets.h"

namespace nevr_quest::login_title {

using StringThunk = sentinel::pinned::LoginScriptStringThunk;

// The literal the game titles the page with. The hook replaces exactly this string.
inline constexpr char kGameTitle[] = "LOGIN FAILED";

inline constexpr int kCounterCount = 5;

// Registers the counters. Call before sentinel::StartReporter. Returns false, having logged one line
// {"event":"login_title_counters","result":"refused",...}, when any was refused. Install() refuses until this succeeded.
bool RegisterCounters() noexcept;

// Arms the handler and installs the thunk in the script library's slot. Returns kModuleNotLoaded, without a log line,
// while the library is not mapped (the caller tries again after the next dlopen). Otherwise one line
// {"event":"login_title_install",...} says what happened. Returns kNotInstalled, after one line, when the counters were
// refused.
sentinel::GotStatus Install() noexcept;

// Arms the handler without touching a GOT slot: for the host test, which publishes the original through the thunk's
// OriginalOut() and calls its EntryFn() directly.
void ArmForTest() noexcept;

// Test support: clears the thunk and the counters, and the counters-registered flag.
void ResetForTest() noexcept;

struct Counts {
  std::uint64_t calls, replaced, kept, other_text, faults;
};
Counts CurrentCounts() noexcept;

}  // namespace nevr_quest::login_title
