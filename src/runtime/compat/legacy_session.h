#pragma once
// The per-login state a legacy game client's translation needs, on top of the message-by-message rules in
// legacy_codec.h: it turns the frames the game client sends into frames for the game service, and the frames
// the game service sends into frames for the game client (nevr-runtime #417).
//
// Platform neutral like evr_codec.h and session_router.h: no Windows or socket headers, no threads, no logging
// backend, no game state. The wiring (a ws_bridge or a session_router seam) hands it whole WebSocket frames and
// sends whatever comes back.
//
// What it remembers that a single message cannot say:
//   * the login session and own user id, learned from the game service's SNSLogInSuccess;
//   * that a legacy game client never asks for its own profile, so the first SNSLogInSuccess of a session is
//     followed by exactly one injected SNSLoggedInUserProfileRequest;
//   * which legacy reply each current profile answer becomes: the answers to one kind of request arrive in the
//     order the requests were made, so each kind keeps a FIFO of the replies it owes (UNVERIFIED against a
//     legacy game client: no capture here has two requests in flight);
//
// Not covered: where the frames come from. The game's own SNSLoginRequest is translated in place, as the Quest
// wiring does, so nothing here builds a login.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

#include "runtime/compat/legacy_codec.h"

namespace nevr_legacy_session {

struct Config {
  nevr_legacy_codec::BuildTables tables;
  // The bridge's own login profile JSON; replaces the JSON of the game's SNSLoginRequest when non-empty.
  std::string loginProfileJson;
  // The request JSON carried by the current-family profile requests.
  std::string profileRequestJson = "{}";
  // The game service writes SessionSuccess encoder flags in the Quest layout.
  bool currentUsesQuestFlags = false;
};

// Frames to send. Either may be empty; when both are empty nothing is sent.
struct Step {
  std::string toGameService;
  std::string toGameClient;
};

struct Stats {
  uint64_t translated = 0;               // messages rewritten
  uint64_t passed = 0;                   // messages forwarded unchanged
  uint64_t dropped = 0;                  // consumed on purpose (telemetry, match results)
  uint64_t answeredLocally = 0;          // replies the bridge made itself
  uint64_t unsupported = 0;              // not a row; not forwarded
  uint64_t malformed = 0;                // a row whose payload did not parse; not forwarded
  uint64_t profileAnswersUnmatched = 0;  // a profile answer with no outstanding request to attribute it to
  uint64_t profileFailuresDropped = 0;   // a profile failure: the legacy form is not established
  uint64_t loginsInjected = 0;           // SNSLoggedInUserProfileRequest sent on the game client's behalf
};

class LegacySession {
 public:
  explicit LegacySession(Config config);

  // One WebSocket binary frame from the legacy game client. May hold several messages; their order is kept.
  Step FromGameClient(const std::string& frame);
  // One WebSocket binary frame from the game service.
  Step FromGameService(const std::string& frame);

  const Stats& GetStats() const { return stats_; }

 private:
  using Reply = nevr_legacy_codec::ProfileReply;

  void ForgetLogin();
  nevr_legacy_codec::Context MakeContext() const;

  Config config_;
  nevr_legacy_codec::Guid loginSession_{};
  nevr_evr_codec::UserId self_;
  bool loggedIn_ = false;                  // a SNSLogInSuccess has been seen for the current login
  std::deque<Reply> ownProfileReplies_;    // owed for SNSLoggedInUserProfileRequest, oldest first
  std::deque<Reply> otherProfileReplies_;  // owed for SNSOtherUserProfileRequest, oldest first
  Stats stats_;
};

}  // namespace nevr_legacy_session
