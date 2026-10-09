#include "quest/integration/identity_source.h"

namespace nevr_quest::integration {

QuestLogin::IdentityStatus TokenIdentitySource::Fetch(QuestLogin::Identity& out) {
  using nevr::quest_auth::Readiness;
  if (!snapshot_) return QuestLogin::IdentityStatus::NotReady;
  const nevr::quest_auth::Snapshot snap = snapshot_();
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
  out.account_id = snap.discord_id;
  out.display_name = snap.username;
  out.access_token = snap.access_token;
  out.social_level = socialLevel_ ? socialLevel_() : 0;
  return QuestLogin::IdentityStatus::Ok;
}

}  // namespace nevr_quest::integration
