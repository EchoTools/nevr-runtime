#include "quest/integration/identity_source.h"

namespace nevr_quest::integration {

QuestLogin::IdentityStatus TokenIdentitySource::Classify(const nevr::quest_auth::Snapshot& snap) noexcept {
  using nevr::quest_auth::Readiness;
  switch (snap.readiness) {
    case Readiness::Starting:
    case Readiness::Refreshing:
    case Readiness::AwaitingUser:
      return QuestLogin::IdentityStatus::NotReady;
    case Readiness::Expired:
    case Readiness::Failed:
    case Readiness::Stopped:
      return QuestLogin::IdentityStatus::NoToken;
    case Readiness::Ready:
      break;
  }
  if (snap.access_token.empty()) return QuestLogin::IdentityStatus::NoToken;
  if (snap.discord_id == 0) return QuestLogin::IdentityStatus::NoAccount;
  return QuestLogin::IdentityStatus::Ok;
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

QuestLogin::IdentityStatus TokenIdentitySource::Fetch(QuestLogin::Identity& out) {
  if (!snapshot_) {
    QuestLogin::attempt_gate::SetReady(false);
    return QuestLogin::IdentityStatus::NotReady;
  }
  const nevr::quest_auth::Snapshot snap = snapshot_();
  const QuestLogin::IdentityStatus status = Classify(snap);
  QuestLogin::attempt_gate::SetReady(status == QuestLogin::IdentityStatus::Ok);  // the state this login saw
  if (status != QuestLogin::IdentityStatus::Ok) return status;
  out.account_id = snap.discord_id;
  out.display_name = snap.username;
  out.access_token = snap.access_token;
  out.social_level = socialLevel_ ? socialLevel_() : 0;
  return QuestLogin::IdentityStatus::Ok;
}

void TokenIdentitySource::Observe(const nevr::quest_auth::Snapshot& snap) noexcept {
  QuestLogin::attempt_gate::SetReady(Classify(snap) == QuestLogin::IdentityStatus::Ok);
}

}  // namespace nevr_quest::integration
