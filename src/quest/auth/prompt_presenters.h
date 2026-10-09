#pragma once
// How a Quest player is told where to sign in (issue #239). QuestTokenAuth hands every prompt to
// a FanOutPresenter that tries each mechanism independently, so no single one has to work:
//
//   file             device_login.txt under the external files dir (FileLinkPresenter,
//                    file_store.h), plus the logcat line when that file cannot be written.
//   game_error_text  the text the game shows when its login fails: the prompt is published on the
//                    prompt board (prompt_board.h) and the sentinel's hook on
//                    CR15NetGame::SetDelimitedErrorMessage puts it in place of the game's own
//                    message while the board holds it (src/quest/sentinel/login_prompt_hook.h).
//
// Every mechanism logs one JSON line per attempt: {"event":"login_prompt","mechanism":...,
// "result":...,"why":...}. The device code is never in those lines.

#include "quest/auth/session.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace nevr::quest_auth {

// The four lines the game's login-error text shows, '\n'-separated, each at most
// prompt_board::kMaxLineChars characters (the game truncates longer lines). The URL is shown
// without its scheme. Returns false and sets `why` ("url_too_long", "code_too_long",
// "code_missing", "newline_in_prompt") when the prompt cannot be shown whole.
bool FormatGamePromptText(const LoginPrompt& prompt, std::string& out, std::string& why);

// Publishes the prompt on the prompt board while the player is asked to sign in and withdraws it
// when the login ends. Present returns accepted only when the text was published; whether the game
// then shows it depends on the game reaching its login-error screen, which the sentinel's hook
// counters record (login_prompt_text_shown).
class GameTextPresenter : public LinkPresenter {
 public:
  explicit GameTextPresenter(nevr::auth::LogSink log);
  intptr_t Present(const LoginPrompt& prompt) override;
  void Clear() override;

 private:
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

 private:
  std::vector<Mechanism> presenters_;
  nevr::auth::LogSink log_;
};

// One JSON line for a login-prompt attempt: {"event":"login_prompt","mechanism":...,"result":...}
// plus `fields` (an object), built with nlohmann::json.
std::string LoginPromptLogLine(const char* mechanism, const char* result,
                               const nlohmann::json& fields = nlohmann::json::object());

}  // namespace nevr::quest_auth
