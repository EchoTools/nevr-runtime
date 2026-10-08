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
  explicit TokenIdentitySource(SnapshotFn snapshot) : snapshot_(std::move(snapshot)) {}

  QuestLogin::IdentityStatus Fetch(QuestLogin::Identity& out) override;

 private:
  SnapshotFn snapshot_;
};

}  // namespace nevr_quest::integration
