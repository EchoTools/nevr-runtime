// The production IdentitySource for the Quest login rewrite: it answers from the token-auth
// session (docs/adr/0003, "Integration"). The rewrite asks once per login attempt, from the game's
// login path; this class only reads a snapshot, so it never blocks and never touches the network.
#pragma once

#include <functional>

#include "quest/auth/session.h"
#include "quest/login/login_attempt_gate.h"
#include "quest/login/login_rewrite.h"
#include "runtime/compat/session_router.h"

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
  // The readiness it publishes is process-wide (the attempt gate), so constructing a source starts it not ready
  // and not poisoned: the process has one.
  explicit TokenIdentitySource(SnapshotFn snapshot, std::function<int()> socialLevel = nullptr)
      : snapshot_(std::move(snapshot)), socialLevel_(std::move(socialLevel)) {
    QuestLogin::attempt_gate::Reset();
  }

  QuestLogin::IdentityStatus Fetch(QuestLogin::Identity& out) override;

  // The router's login gate for one token-auth state: Ready when Fetch would answer Ok, Awaiting while a
  // token is still to come (starting, refreshing, waiting for the player, expired and being replaced, a
  // failure the session retries every recovery period, a token that ran out and is being refreshed): the
  // login connection is held through all of these. Refused only when no token will come: a final failure
  // (Snapshot::will_retry false), a stopped session, a token without an account. Allocation-free; the answer is exactly Classify's, so the gate and the login rewrite cannot
  // disagree.
  static SessionRouter::LoginGate GateFor(const nevr::quest_auth::Snapshot& snap) noexcept;

  // The #240 login prerequisites ask this from the Oculus message pump before they stand in for an Oculus
  // answer (login_rewrite.h). It is one lock-free atomic load of the process-wide attempt gate
  // (login_attempt_gate.h) that Observe and Fetch keep, which the sign-in prompt's page-enable hook reads
  // too: no snapshot, no allocation, no lock. True only after the last observed state was one in which
  // Fetch returns Ok (token auth Ready with an access token and a NEVR account id) and the current login
  // attempt has not been poisoned by a skipped logging-in page.
  bool Ready() const noexcept override { return QuestLogin::attempt_gate::LoginMayProceed(); }

  // Records a token-auth state change: sets the Ready flag to (Classify(snap) == Ok), so it is cleared in
  // every other state (starting, refreshing, awaiting the player, expired, failed, stopped, no token, no
  // account). Allocation-free; called from whichever thread observes the state (the integration's
  // token-auth poll thread) and by Fetch with the snapshot it read.
  void Observe(const nevr::quest_auth::Snapshot& snap) noexcept;

 private:
  // The answer for one snapshot, shared by Fetch and Observe so Fetch and Ready cannot disagree.
  static QuestLogin::IdentityStatus Classify(const nevr::quest_auth::Snapshot& snap) noexcept;

  SnapshotFn snapshot_;
  std::function<int()> socialLevel_;
};

}  // namespace nevr_quest::integration
