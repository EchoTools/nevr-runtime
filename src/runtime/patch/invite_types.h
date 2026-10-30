#pragma once
// #966 S4: the plain types of the invite service that the Quest frame path and the PC join use. No try/catch and no worker
// here, so a translation unit built with -fno-exceptions (the sentinel's hook path) can include it. The worker's header
// (invite_worker.h) includes this one.
#include <string>

#include "runtime/patch/invite_wizard.h"

namespace nevr_invite {

enum class LogLevel { Info, Warning, Error };

struct Answer {
  Op op = Op::Create;
  bool sent = false;  // false: nothing went out (no token)
  int status = 0;
  Result result = Result::Unavailable;
  bool via_status = false;  // a refusal the service did not name: the HTTP status alone decided the result
  std::string refusal_message;  // via_status only: for the scenario endpoint, never logged or shown
  std::string body;  // a success only
};

}  // namespace nevr_invite
