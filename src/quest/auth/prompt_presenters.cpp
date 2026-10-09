#include "quest/auth/prompt_presenters.h"

#include "core/device_auth_flow.h"
#include "quest/auth/prompt_board.h"

#include <exception>
#include <utility>

namespace nevr::quest_auth {

namespace {
using nevr::auth::LogLevel;

void Emit(const nevr::auth::LogSink& log, LogLevel level, const std::string& message) {
  if (log) log(level, message);
}

// "https://echovrce.com/login/device" -> "echovrce.com/login/device": what a player types.
std::string DisplayUrl(const std::string& url) {
  for (const char* scheme : {"https://", "http://"}) {
    const std::string s(scheme);
    if (url.compare(0, s.size(), s) == 0) return url.substr(s.size());
  }
  return url;
}

constexpr char kLine1[] = "Sign in to play: on a phone or computer, open";
constexpr char kLine3Prefix[] = "and enter the code ";
constexpr char kLine4[] = "Each code lasts 5 minutes, then a new one is issued.";
static_assert(sizeof(kLine1) - 1 <= prompt_board::kMaxLineChars, "line 1 fits the game's line");
static_assert(sizeof(kLine4) - 1 <= prompt_board::kMaxLineChars, "line 4 fits the game's line");
static_assert(sizeof(kSignedInText) - 1 <= prompt_board::kCapacity, "the signed-in notice fits the board");
static_assert(sizeof(kTimedOutText) - 1 <= prompt_board::kCapacity, "the timed-out text fits the board");

const char* ModeName(prompt_board::Mode mode) {
  return mode == prompt_board::Mode::kNotice ? "notice" : "prompt";
}
}  // namespace

std::string LoginPromptLogLine(const char* mechanism, const char* result, const nlohmann::json& fields) {
  nlohmann::json line = {{"event", "login_prompt"}, {"mechanism", mechanism}, {"result", result}};
  if (fields.is_object()) {
    for (auto it = fields.begin(); it != fields.end(); ++it) line[it.key()] = it.value();
  }
  return line.dump();
}

bool FormatGamePromptText(const LoginPrompt& prompt, std::string& out, std::string& why) {
  out.clear();
  const std::string url = DisplayUrl(prompt.url);
  if (prompt.code.empty()) {
    why = "code_missing";
    return false;
  }
  if (url.find('\n') != std::string::npos || prompt.code.find('\n') != std::string::npos) {
    why = "newline_in_prompt";
    return false;
  }
  if (url.empty() || url.size() > prompt_board::kMaxLineChars) {
    why = "url_too_long";
    return false;
  }
  std::string line3 = kLine3Prefix + prompt.code;
  if (line3.size() > prompt_board::kMaxLineChars) {
    nevr::auth::WipeSecret(line3);
    why = "code_too_long";
    return false;
  }
  out = std::string(kLine1) + "\n" + url + "\n" + line3 + "\n" + kLine4;
  nevr::auth::WipeSecret(line3);
  why.clear();
  return true;
}

GameTextPresenter::GameTextPresenter(nevr::auth::LogSink log) : log_(std::move(log)) {}

bool GameTextPresenter::Publish(const std::string& text, prompt_board::Mode mode, const char* what) {
  if (!prompt_board::Publish(text.data(), text.size(), mode)) {
    prompt_board::Withdraw();  // an older code must not stay on screen
    Emit(log_, LogLevel::Error,
         LoginPromptLogLine("game_error_text", "refused",
                            {{"why", "board_refused"}, {"what", what}, {"chars", text.size()}}));
    return false;
  }
  Emit(log_, LogLevel::Info,
       LoginPromptLogLine("game_error_text", "published",
                          {{"what", what}, {"mode", ModeName(mode)}, {"chars", text.size()},
                           {"version", prompt_board::Version()}}));
  return true;
}

intptr_t GameTextPresenter::Present(const LoginPrompt& prompt) {
  std::string text;
  std::string why;
  if (!FormatGamePromptText(prompt, text, why)) {
    prompt_board::Withdraw();  // an older code must not stay on screen
    Emit(log_, LogLevel::Error, LoginPromptLogLine("game_error_text", "refused", {{"what", "code"}, {"why", why}}));
    return 0;
  }
  const bool published = Publish(text, prompt_board::Mode::kPrompt, "code");
  nevr::auth::WipeSecret(text);  // the board holds the only copy this presenter needs
  return published ? nevr::auth::kBrowserOpenAcceptedAbove + 1 : 0;
}

void GameTextPresenter::Clear() {
  if (prompt_board::Withdraw()) Emit(log_, LogLevel::Info, LoginPromptLogLine("game_error_text", "withdrawn"));
}

void GameTextPresenter::Conclude(LoginOutcome outcome) {
  if (outcome == LoginOutcome::SignedIn) {
    // Only a screen that shows a prompt is changed: a later login failure shows the game's own text.
    Publish(kSignedInText, prompt_board::Mode::kNotice, "signed_in");
  } else {
    // Shown at every later login failure: no code is coming until the game restarts.
    Publish(kTimedOutText, prompt_board::Mode::kPrompt, "timed_out");
  }
}

FanOutPresenter::FanOutPresenter(std::vector<Mechanism> presenters, nevr::auth::LogSink log)
    : presenters_(std::move(presenters)), log_(std::move(log)) {}

intptr_t FanOutPresenter::Present(const LoginPrompt& prompt) {
  bool delivered = false;
  for (const Mechanism& m : presenters_) {
    if (m.presenter == nullptr) continue;
    // One mechanism failing must not keep the others from the player.
    try {
      if (m.presenter->Present(prompt) > nevr::auth::kBrowserOpenAcceptedAbove) delivered = true;
    } catch (const std::exception& e) {
      Emit(log_, LogLevel::Error, LoginPromptLogLine(m.name, "failed", {{"why", e.what()}}));
    }
  }
  if (!delivered) {
    // Neither the file nor the game's screen can show this code: say so once, without the code.
    Emit(log_, LogLevel::Error,
         LoginPromptLogLine("all", "not_shown",
                            {{"url", prompt.url},
                             {"why", "no mechanism could show the code (see the lines above)"},
                             {"next", "a new code is requested after the recovery period; restarting the game also "
                                      "asks again"}}));
  }
  return delivered ? nevr::auth::kBrowserOpenAcceptedAbove + 1 : 0;
}

void FanOutPresenter::Clear() {
  for (const Mechanism& m : presenters_) {
    if (m.presenter == nullptr) continue;
    try {
      m.presenter->Clear();
    } catch (const std::exception& e) {
      Emit(log_, LogLevel::Error, LoginPromptLogLine(m.name, "clear_failed", {{"why", e.what()}}));
    }
  }
}

void FanOutPresenter::Conclude(LoginOutcome outcome) {
  for (const Mechanism& m : presenters_) {
    if (m.presenter == nullptr) continue;
    try {
      m.presenter->Conclude(outcome);
    } catch (const std::exception& e) {
      Emit(log_, LogLevel::Error, LoginPromptLogLine(m.name, "conclude_failed", {{"why", e.what()}}));
    }
  }
}

}  // namespace nevr::quest_auth
