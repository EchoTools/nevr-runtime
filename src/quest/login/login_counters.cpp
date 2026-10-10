#include "quest/login/login_hook.h"
#include "quest/login/login_prerequisite_thunks.h"
#include "quest/login/login_thunk.h"
#include "quest/sentinel/hook_report.h"

namespace nevr_quest_login {

// A hook never logs on the game's call path; the reporter thread reads these (hook_report.h). Without
// them a smoke test cannot tell from the reporter lines whether CNSUser::SendLogInRequest was reached.
//
// The login-prerequisite hooks (login_prerequisites.h) register one calls counter each, 16 in all: which
// Oculus answer the game asked for and which callback ran says how far a login got. No fault counter:
// like the dlopen hook's, a thunk GotHook installed has its original published, so it cannot move, and a
// failed install is its own `quest_login_prerequisites_install` line.
bool RegisterLoginHookCounters() noexcept {
  bool ok = sentinel::RegisterReportCounter("login_calls", &LoginThunk::CallCounter());
  ok = sentinel::RegisterReportCounter("login_thunk_faults", &LoginThunk::FaultCounter(),
                                       sentinel::ReportKind::kFaults) && ok;
  ok = sentinel::RegisterReportCounter("prereq_org_callback_calls", &OrgCallbackThunk::CallCounter()) && ok;
  ok = sentinel::RegisterReportCounter("prereq_user_callback_calls", &UserCallbackThunk::CallCounter()) && ok;
  ok = sentinel::RegisterReportCounter("prereq_token_callback_calls", &TokenCallbackThunk::CallCounter()) && ok;
  ok = sentinel::RegisterReportCounter("prereq_proof_callback_calls", &ProofCallbackThunk::CallCounter()) && ok;
  ok = sentinel::RegisterReportCounter("prereq_is_error_calls", &IsErrorThunk::CallCounter()) && ok;
  ok = sentinel::RegisterReportCounter("prereq_get_string_calls", &GetStringThunk::CallCounter()) && ok;
  ok = sentinel::RegisterReportCounter("prereq_get_org_scoped_id_calls", &GetOrgScopedIdThunk::CallCounter()) && ok;
  ok = sentinel::RegisterReportCounter("prereq_org_scoped_id_get_id_calls", &OrgScopedIdGetIdThunk::CallCounter()) && ok;
  ok = sentinel::RegisterReportCounter("prereq_get_user_calls", &GetUserThunk::CallCounter()) && ok;
  ok = sentinel::RegisterReportCounter("prereq_user_get_oculus_id_calls", &UserGetOculusIdThunk::CallCounter()) && ok;
  ok = sentinel::RegisterReportCounter("prereq_get_user_proof_calls", &GetUserProofThunk::CallCounter()) && ok;
  ok = sentinel::RegisterReportCounter("prereq_user_proof_get_nonce_calls", &UserProofGetNonceThunk::CallCounter()) && ok;
  ok = sentinel::RegisterReportCounter("prereq_org_request_calls", &OrgRequestThunk::CallCounter()) && ok;
  ok = sentinel::RegisterReportCounter("prereq_user_request_calls", &UserRequestThunk::CallCounter()) && ok;
  ok = sentinel::RegisterReportCounter("prereq_token_request_calls", &TokenRequestThunk::CallCounter()) && ok;
  ok = sentinel::RegisterReportCounter("prereq_proof_request_calls", &ProofRequestThunk::CallCounter()) && ok;
  return ok;
}

}  // namespace nevr_quest_login
