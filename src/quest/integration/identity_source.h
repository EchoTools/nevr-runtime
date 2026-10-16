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

// Maps the session's state onto the rewrite's fail-close answers, and onto whether the login answers wait
// (HoldsFor; the (ready, hold) pair is published to the attempt gate):
//   state                                     answer      hold
//   Ready with a token and an account id      Ok          no
//   Starting / Refreshing                     NotReady    yes (a token is coming without the player)
//   AwaitingUser                              NotReady    no  (the player is awaited; the game's local login
//                                                              failure is where the sign-in code is shown)
//   Ready without a token (it ran out)        NoToken     yes (a refresh is under way)
//   Ready without an account id               NoAccount   no
//   Expired                                   NoToken     yes (being replaced)
//   Failed, will_retry                        NoToken     yes
//   Failed, final / Stopped                   NoToken     no
// The account id is the NEVR account id the token carries (`discord_id` in the session snapshot, the
// id the PCVR bridge sends as its account); the display name is the session's user name.
class TokenIdentitySource final : public nevr_quest_login::IdentitySource {
 public:
  using SnapshotFn = std::function<nevr::quest_auth::Snapshot()>;
  // `socialLevel` is asked on every Fetch and answers the level the login declares ("nevr_social"):
  // nevr_social_party::kSocialLevel only when the social facade is installed, else 0.
  // The readiness it publishes is process-wide (the attempt gate), so constructing a source starts it not ready
  // and not poisoned, and holding: the process has one, and a default Snapshot is Starting.
  explicit TokenIdentitySource(SnapshotFn snapshot, std::function<int()> socialLevel = nullptr)
      : snapshot_(std::move(snapshot)), socialLevel_(std::move(socialLevel)) {
    nevr_quest_login::attempt_gate::Reset();
    Observe(nevr::quest_auth::Snapshot{});
  }

  nevr_quest_login::IdentityStatus Fetch(nevr_quest_login::Identity& out) override;

  // The router's login gate for one token-auth state: Ready when Fetch would answer Ok, Awaiting while a
  // token is still to come (starting, refreshing, waiting for the player, expired and being replaced, a
  // failure the session retries every recovery period, a token that ran out and is being refreshed): the
  // login connection is held through all of these. Refused only when no token will come: a final failure
  // (Snapshot::will_retry false), a stopped session, a token without an account. Allocation-free; the answer is exactly Classify's, so the gate and the login rewrite cannot
  // disagree.
  static nevr_session_router::LoginGate GateFor(const nevr::quest_auth::Snapshot& snap) noexcept;

  // The #240 login prerequisites ask this from the Oculus message pump before they stand in for an Oculus
  // answer (login_rewrite.h). It is one lock-free atomic load of the process-wide attempt gate
  // (login_attempt_gate.h) that Observe and Fetch keep, which the sign-in prompt's page-enable hook reads
  // too: no snapshot, no allocation, no lock. True only after the last observed state was one in which
  // Fetch returns Ok (token auth Ready with an access token and a NEVR account id) and the current login
  // attempt has not been poisoned by a skipped logging-in page.
  bool Ready() const noexcept override { return nevr_quest_login::attempt_gate::LoginMayProceed(); }

  // The #581 hold, one lock-free atomic load of the same attempt gate. The login prerequisites ask it on every
  // pump of the game's message queue: while it is true the four local login answers wait for token auth; while it
  // is false they are delivered, and an identity that is not ready then makes the game fail the login on its own
  // path, which is the screen the sign-in code is written onto. True only while the gate is not ready, not
  // poisoned, and the last observed state was one for which HoldsFor is true.
  bool HoldsAnswers() const noexcept override { return nevr_quest_login::attempt_gate::HoldsAnswers(); }

  // Whether the login answers wait for token auth in this state: a token is coming without the player. The
  // router's gate is Awaiting (GateFor) and the session is not waiting for the player: starting, refreshing,
  // expired, a failure the session retries, a ready snapshot whose token ran out. False when the player is
  // awaited, when the identity is ready, and in every state where no token will come. Allocation-free.
  static bool HoldsFor(const nevr::quest_auth::Snapshot& snap) noexcept;

  // Logs one Info record (event "quest_login_answers": mode "held" or "answered", status the readiness name the
  // caller passes) each time `hold` (HoldsFor of the polled snapshot) differs from what `*last` holds, and on the
  // first call (`*last` starts at -1). Carries no token, nonce, user name or id. Called from the token-auth poll
  // thread, which keeps `*last`.
  static void ReportHold(bool hold, const char* status, int* last);

  // Records a token-auth state change: publishes (Classify(snap) == Ok, HoldsFor(snap)) to the attempt gate in
  // one store, so ready is cleared in every other state (starting, refreshing, awaiting the player, expired,
  // failed, stopped, no token, no account). Allocation-free; called from whichever thread observes the state (the
  // integration's token-auth poll thread) and by Fetch with the snapshot it read.
  void Observe(const nevr::quest_auth::Snapshot& snap) noexcept;

 private:
  // The answer for one snapshot, shared by Fetch and Observe so Fetch and Ready cannot disagree.
  static nevr_quest_login::IdentityStatus Classify(const nevr::quest_auth::Snapshot& snap) noexcept;

  SnapshotFn snapshot_;
  std::function<int()> socialLevel_;
};

}  // namespace nevr_quest::integration
