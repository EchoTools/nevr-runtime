#include "quest/integration/identity_source.h"

#include "quest/sentinel/hook_log.h"

namespace nevr_quest::integration {

nevr_quest_login::IdentityStatus TokenIdentitySource::Classify(const nevr::quest_auth::Snapshot& snap) noexcept {
  using nevr::quest_auth::Readiness;
  switch (snap.readiness) {
    case Readiness::Starting:
    case Readiness::Refreshing:
    case Readiness::AwaitingUser:
      return nevr_quest_login::IdentityStatus::NotReady;
    case Readiness::Expired:
    case Readiness::Failed:
    case Readiness::Stopped:
      return nevr_quest_login::IdentityStatus::NoToken;
    case Readiness::Ready:
      break;
  }
  if (snap.access_token.empty()) return nevr_quest_login::IdentityStatus::NoToken;
  if (snap.discord_id == 0) return nevr_quest_login::IdentityStatus::NoAccount;
  return nevr_quest_login::IdentityStatus::Ok;
}

nevr_session_router::LoginGate TokenIdentitySource::GateFor(const nevr::quest_auth::Snapshot& snap) noexcept {
  using nevr::quest_auth::Readiness;
  using nevr_session_router::LoginGate;
  switch (snap.readiness) {
    case Readiness::Starting:
    case Readiness::Refreshing:
    case Readiness::AwaitingUser:
    case Readiness::Expired:  // the background refresh or re-login is replacing the token
      return LoginGate::Awaiting;
    case Readiness::Failed:  // a failure the session retries every recovery period is not the end
      return snap.will_retry ? LoginGate::Awaiting : LoginGate::Refused;
    case Readiness::Stopped:
      return LoginGate::Refused;
    case Readiness::Ready:
      break;
  }
  if (snap.access_token.empty()) return LoginGate::Awaiting;  // the token ran out; a refresh is under way
  if (snap.discord_id == 0) return LoginGate::Refused;        // a token without an account will not get one
  return LoginGate::Ready;
}

bool TokenIdentitySource::HoldsFor(const nevr::quest_auth::Snapshot& snap) noexcept {
  return GateFor(snap) == nevr_session_router::LoginGate::Awaiting &&
         snap.readiness != nevr::quest_auth::Readiness::AwaitingUser;
}

void TokenIdentitySource::ReportHold(bool hold, const char* status, int* last) {
  const int now = hold ? 1 : 0;
  if (now == *last) return;
  *last = now;
  sentinel::LogFields(sentinel::LogLevel::kInfo, "quest_login_answers",
                      {{"mode", hold ? "held" : "answered"}, {"status", status}});
}

nevr_quest_login::IdentityStatus TokenIdentitySource::Fetch(nevr_quest_login::Identity& out) {
  if (Terminal()) {
    nevr_quest_login::attempt_gate::Publish(false, false);
    return nevr_quest_login::IdentityStatus::NoToken;
  }
  if (!snapshot_) {
    nevr_quest_login::attempt_gate::Publish(false, false);
    return nevr_quest_login::IdentityStatus::NotReady;
  }
  const nevr::quest_auth::Snapshot snap = snapshot_();
  const nevr_quest_login::IdentityStatus status = Classify(snap);
  nevr_quest_login::attempt_gate::Publish(status == nevr_quest_login::IdentityStatus::Ok, HoldsFor(snap));  // the state this login saw
  if (status != nevr_quest_login::IdentityStatus::Ok) return status;
  out.account_id = snap.discord_id;
  out.display_name = snap.username;
  out.access_token = snap.access_token;
  out.social_level = socialLevel_ ? socialLevel_() : 0;
  return nevr_quest_login::IdentityStatus::Ok;
}

void TokenIdentitySource::Observe(const nevr::quest_auth::Snapshot& snap) noexcept {
  if ((snap.readiness == nevr::quest_auth::Readiness::Failed && !snap.will_retry) ||
      snap.readiness == nevr::quest_auth::Readiness::Stopped) {
    terminal_.store(true, std::memory_order_release);
  }
  nevr_quest_login::attempt_gate::Publish(!Terminal() && Classify(snap) == nevr_quest_login::IdentityStatus::Ok,
                                          !Terminal() && HoldsFor(snap));
}

}  // namespace nevr_quest::integration
