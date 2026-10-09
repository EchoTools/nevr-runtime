// The production IdentitySource for the Quest login rewrite: it answers from the token-auth
// session (docs/adr/0003, "Integration"). The rewrite asks once per login attempt, from the game's
// login path; this class only reads a snapshot, so it never blocks and never touches the network.
#pragma once

#include <functional>

#include "quest/auth/session.h"
#include "quest/login/login_rewrite.h"

namespace nevr_quest::integration {

// Maps the session's state onto the rewrite's fail-close answers:
//   Ready with a token and an account id      Ok
//   Starting / Refreshing / AwaitingUser      NotReady (the Oculus login goes out unchanged; the next
//                                             login asks again)
//   Ready without a token                     NoToken
//   Ready without an account id               NoAccount
//   Expired / Failed / Stopped                NoToken
// The account id is the NEVR account id the token carries (`discord_id` in the session snapshot, the
// id the PCVR bridge sends as its account); the display name is the session's user name.
class TokenIdentitySource final : public QuestLogin::IdentitySource {
 public:
  using SnapshotFn = std::function<nevr::quest_auth::Snapshot()>;
  // `socialLevel` is asked on every Fetch and answers the level the login declares ("nevr_social"):
  // SocialParty::kSocialLevel only when the social facade is installed, else 0.
  explicit TokenIdentitySource(SnapshotFn snapshot, std::function<int()> socialLevel = nullptr)
      : snapshot_(std::move(snapshot)), socialLevel_(std::move(socialLevel)) {}

  QuestLogin::IdentityStatus Fetch(QuestLogin::Identity& out) override;

  // True exactly when Fetch would return Ok: token auth is Ready with an access token and a NEVR account id.
  // The #240 login prerequisites ask it from the Oculus message pump before they stand in for an Oculus
  // answer (login_rewrite.h); false in every other state, when there is no snapshot function, and when
  // taking the snapshot throws (contained here, so nothing unwinds into the game's frames). Cost: one
  // snapshot, the same read Fetch does; no network, no wait on the game.
  bool Ready() const noexcept override;

 private:
  // The answer for one snapshot, shared by Fetch and Ready so the two cannot disagree.
  static QuestLogin::IdentityStatus Classify(const nevr::quest_auth::Snapshot& snap);

  SnapshotFn snapshot_;
  std::function<int()> socialLevel_;
};

}  // namespace nevr_quest::integration
