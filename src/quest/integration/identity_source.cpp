#include "quest/integration/identity_source.h"

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

nevr_quest_login::IdentityStatus TokenIdentitySource::Fetch(nevr_quest_login::Identity& out) {
  if (!snapshot_) {
    nevr_quest_login::attempt_gate::SetReady(false);
    return nevr_quest_login::IdentityStatus::NotReady;
  }
  const nevr::quest_auth::Snapshot snap = snapshot_();
  const nevr_quest_login::IdentityStatus status = Classify(snap);
  nevr_quest_login::attempt_gate::SetReady(status == nevr_quest_login::IdentityStatus::Ok);  // the state this login saw
  if (status != nevr_quest_login::IdentityStatus::Ok) return status;
  out.account_id = snap.discord_id;
  out.display_name = snap.username;
  out.access_token = snap.access_token;
  out.social_level = socialLevel_ ? socialLevel_() : 0;
  return nevr_quest_login::IdentityStatus::Ok;
}

void TokenIdentitySource::Observe(const nevr::quest_auth::Snapshot& snap) noexcept {
  nevr_quest_login::attempt_gate::SetReady(Classify(snap) == nevr_quest_login::IdentityStatus::Ok);
}

}  // namespace nevr_quest::integration
