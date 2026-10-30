#pragma once
// What the player reads while device sign-in is pending on Windows (#397): the page, the code and what to
// expect, and the line that replaces it when the wait ends. Pure text, so a test can pin every sentence;
// the window that shows it lives with the token-auth module.

#include "core/device_auth_flow.h"

#include <string>

namespace nevr::auth {

struct SignInDialogContent {
  std::string title;
  std::string instruction;  // what to do, or what happened
  std::string code;         // the device code, large; empty once the wait has ended
  bool closes_by_itself = false;  // true: shown only for a moment (signed in); false: stays for the player
};

// "https://echovrce.com/device" -> "echovrce.com/device": what a player types.
inline std::string SignInDisplayUrl(const std::string& url) {
  for (const char* scheme : {"https://", "http://"}) {
    const std::string s(scheme);
    if (url.compare(0, s.size(), s) == 0) return url.substr(s.size());
  }
  return url;
}

// The dialog while the code is waiting for the player.
inline SignInDialogContent SignInWaitingContent(const std::string& login_url, const std::string& code) {
  SignInDialogContent c;
  c.title = "Echo VR - sign in";
  c.instruction = "Sign in to play: on a phone or computer, open\n" + SignInDisplayUrl(login_url) +
                  "\nand enter the code below.\nYour browser was also opened to that page. "
                  "The code lasts 5 minutes.";
  c.code = code;
  return c;
}

// The dialog once the wait has ended, said in the player's terms.
inline SignInDialogContent SignInEndedContent(FlowEnd end) {
  SignInDialogContent c;
  c.title = "Echo VR - sign in";
  switch (end) {
    case FlowEnd::Verified:
      c.instruction = "Signed in. Starting Echo VR.";
      c.closes_by_itself = true;
      break;
    case FlowEnd::CodeExpired:
      c.instruction = "The sign-in code expired before sign-in finished.\nRestart Echo VR to get a new code.";
      break;
    case FlowEnd::TimedOut:
      c.instruction = "Sign-in timed out. Restart Echo VR to get a new code.";
      break;
    case FlowEnd::PollErrors:
      c.instruction = "Echo VR could not reach the sign-in service.\nCheck your connection and restart Echo VR.";
      break;
    case FlowEnd::NoCode:
      c.instruction = "Echo VR could not get a sign-in code.\nCheck your connection and restart Echo VR.";
      break;
    case FlowEnd::BrowserFailed:
      c.instruction = "Sign-in was stopped because the browser could not be opened.\nRestart Echo VR to try again.";
      break;
    case FlowEnd::Cancelled:
      c.instruction = "Sign-in was cancelled because Echo VR is closing.";
      c.closes_by_itself = true;
      break;
  }
  return c;
}

// How long the window stays after the wait ended. A success or a cancel is read for a moment and closes itself; a
// failure stays until the player closes it, because a failure is what the player has to act on (#1446). 0: stays.
inline constexpr unsigned kSignInSuccessLingerMs = 1500;
inline unsigned SignInEndedLingerMs(const SignInDialogContent& content) {
  return content.closes_by_itself ? kSignInSuccessLingerMs : 0;
}

}  // namespace nevr::auth
