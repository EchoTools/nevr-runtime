#pragma once
// How a Quest player is told where to sign in (issue #239). QuestTokenAuth hands every prompt to
// a FanOutPresenter that tries each mechanism independently, so no single one has to work:
//
//   file             device_login.txt under the external files dir (FileLinkPresenter,
//                    file_store.h).
//   game_error_text  the game's login-error screen: the text is published on the prompt board
//                    (prompt_board.h); the sentinel's hooks (src/quest/sentinel/login_prompt_hook.h)
//                    write it over the game's message when a local login failure happens while the
//                    game is logging in, and keep that screen current while the game stays in
//                    "login failed" (a new code, then "signed in" or "timed out").
//
// Every mechanism logs one JSON line per attempt: {"event":"login_prompt","mechanism":...,
// "result":...,"why":...}. The device code is never in those lines.

#include "quest/auth/prompt_board.h"
#include "quest/auth/session.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace nevr::quest_auth {

// What the game's login-error screen says once the player has signed in (refreshes a screen that
// shows a prompt; Mode::kNotice), and once the bound on unanswered codes is reached (Mode::kPrompt):
// kTimedOutText when a code was shown, kNoCodeShownText when none could be. The new sign-in is
// stored in the credential cache and served by QuestTokenAuth::Token(); nothing hands it to a login
// the game is already running, and the game may quit after a failed login (QuitOnError), so the
// player is told to restart.
inline constexpr char kSignedInText[] = "Signed in to EchoVRCE.\nRestart the game to finish.";
inline constexpr char kTimedOutText[] = "Sign-in timed out.\nRestart the game to try again.";
inline constexpr char kNoCodeShownText[] = "No sign-in code could be shown.\nRestart the game to try again.";

// The four lines the game's login-error text shows, '\n'-separated, each at most
// prompt_board::kMaxLineChars characters (the game truncates longer lines). The URL is shown
// without its scheme. Returns false and sets `why` ("url_too_long", "code_too_long",
// "code_missing", "newline_in_prompt") when the prompt cannot be shown whole.
bool FormatGamePromptText(const LoginPrompt& prompt, std::string& out, std::string& why);

// Publishes the prompt on the prompt board while the player is asked to sign in; on the outcome it
// publishes the signed-in notice or the timed-out text, and Clear withdraws whatever is there.
// Present returns accepted only when the text was published; whether the game then shows it
// depends on the game reaching its login-error screen, which the sentinel's hook counters record
// (login_prompt_text_shown, login_prompt_text_refreshed).
class GameTextPresenter : public LinkPresenter {
 public:
  explicit GameTextPresenter(nevr::auth::LogSink log);
  intptr_t Present(const LoginPrompt& prompt) override;
  void Clear() override;
  void Conclude(LoginOutcome outcome) override;

 private:
  bool Publish(const std::string& text, prompt_board::Mode mode, const char* what);
  nevr::auth::LogSink log_;
};

// Hands every prompt to each presenter in order. Present returns accepted when at least one of
// them accepted it; Clear clears all of them. A presenter that throws is logged under its
// mechanism name and the others still run. Holds the presenters by pointer: they must outlive it.
class FanOutPresenter : public LinkPresenter {
 public:
  struct Mechanism {
    const char* name;  // a literal: "file", "game_error_text"
    LinkPresenter* presenter;
  };
  FanOutPresenter(std::vector<Mechanism> presenters, nevr::auth::LogSink log);
  intptr_t Present(const LoginPrompt& prompt) override;
  void Clear() override;
  void Conclude(LoginOutcome outcome) override;

 private:
  std::vector<Mechanism> presenters_;
  nevr::auth::LogSink log_;
};

// One JSON line for a login-prompt attempt: {"event":"login_prompt","mechanism":...,"result":...}
// plus `fields` (an object), built with nlohmann::json.
std::string LoginPromptLogLine(const char* mechanism, const char* result,
                               const nlohmann::json& fields = nlohmann::json::object());

}  // namespace nevr::quest_auth
