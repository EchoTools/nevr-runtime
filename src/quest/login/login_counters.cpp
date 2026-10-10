#include "quest/login/login_hook.h"
#include "quest/login/login_thunk.h"
#include "quest/sentinel/hook_report.h"

namespace QuestLogin {

// A hook never logs on the game's call path; the reporter thread reads these (hook_report.h). Without
// them a smoke test cannot tell from the reporter lines whether CNSUser::SendLogInRequest was reached.
bool RegisterLoginHookCounters() noexcept {
  bool ok = sentinel::RegisterReportCounter("login_calls", &LoginThunk::CallCounter());
  ok = sentinel::RegisterReportCounter("login_thunk_faults", &LoginThunk::FaultCounter(),
                                       sentinel::ReportKind::kFaults) && ok;
  return ok;
}

}  // namespace QuestLogin
