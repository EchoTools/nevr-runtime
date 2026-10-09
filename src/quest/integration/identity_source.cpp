#include "quest/integration/identity_source.h"

#include <exception>

namespace nevr_quest::integration {

QuestLogin::IdentityStatus TokenIdentitySource::Classify(const nevr::quest_auth::Snapshot& snap) {
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

QuestLogin::IdentityStatus TokenIdentitySource::Fetch(QuestLogin::Identity& out) {
  if (!snapshot_) return QuestLogin::IdentityStatus::NotReady;
  const nevr::quest_auth::Snapshot snap = snapshot_();
  const QuestLogin::IdentityStatus status = Classify(snap);
  if (status != QuestLogin::IdentityStatus::Ok) return status;
  out.account_id = snap.discord_id;
  out.display_name = snap.username;
  out.access_token = snap.access_token;
  out.social_level = socialLevel_ ? socialLevel_() : 0;
  return QuestLogin::IdentityStatus::Ok;
}

bool TokenIdentitySource::Ready() const noexcept {
  if (!snapshot_) return false;
  try {
    return Classify(snapshot_()) == QuestLogin::IdentityStatus::Ok;
  } catch (const std::exception&) {
    return false;  // no snapshot: not ready, and nothing leaves this frame
  }
}

}  // namespace nevr_quest::integration
