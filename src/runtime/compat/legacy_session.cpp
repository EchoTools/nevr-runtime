#include "runtime/compat/legacy_session.h"

#include <utility>

namespace nevr_legacy_session {

namespace {

namespace codec = nevr_evr_codec;
namespace legacy = nevr_legacy_codec;

// Appends the messages of one translation to the right side of `step`, and counts them.
void Route(const legacy::Translation& t, const std::string& original, bool fromGameClient, Stats& stats, Step& step) {
  std::string& sameSide = fromGameClient ? step.toGameService : step.toGameClient;
  switch (t.outcome) {
    case legacy::Outcome::Translated:
      ++stats.translated;
      break;
    case legacy::Outcome::Passthrough:
      ++stats.passed;
      sameSide.append(original);
      break;
    case legacy::Outcome::Dropped:
      ++stats.dropped;
      break;
    case legacy::Outcome::Local:
      ++stats.answeredLocally;
      break;
    case legacy::Outcome::Unsupported:
      ++stats.unsupported;
      break;
    case legacy::Outcome::Malformed:
      ++stats.malformed;
      break;
  }
  for (const legacy::OutMessage& m : t.toGameService)
    step.toGameService.append(codec::BuildMessage(m.symbol, m.payload));
  for (const legacy::OutMessage& m : t.toGameClient) step.toGameClient.append(codec::BuildMessage(m.symbol, m.payload));
}

}  // namespace

LegacySession::LegacySession(Config config) : config_(std::move(config)) {}

legacy::Context LegacySession::MakeContext() const {
  legacy::Context ctx;
  ctx.loginSession = loginSession_;
  ctx.self = self_;
  ctx.currentUsesQuestFlags = config_.currentUsesQuestFlags;
  ctx.loginProfileJson = config_.loginProfileJson;
  ctx.profileRequestJson = config_.profileRequestJson;
  return ctx;
}

void LegacySession::ForgetLogin() {
  loginSession_ = {};
  self_ = {};
  loggedIn_ = false;
  ownProfileReplies_.clear();
  otherProfileReplies_.clear();
}

Step LegacySession::FromGameClient(const std::string& frame) {
  Step step;
  std::size_t offset = 0;
  for (;;) {
    codec::Message message;
    const codec::ReadStatus status = codec::ReadMessage(frame, offset, &message);
    if (status == codec::ReadStatus::End) break;
    if (status != codec::ReadStatus::Ok) {
      ++stats_.malformed;
      break;
    }
    const std::string payload(reinterpret_cast<const char*>(message.payload), static_cast<std::size_t>(message.length));
    const std::string original = codec::BuildMessage(message.symbol, payload);
    offset += codec::kHeaderSize + static_cast<std::size_t>(message.length);

    if (message.symbol == legacy::kSNSLoginRequest) ForgetLogin();  // a new login: nothing outstanding survives

    const legacy::Context ctx = MakeContext();
    const legacy::Translation t = legacy::LegacyToCurrent(message.symbol, payload, ctx, config_.tables);
    Route(t, original, true, stats_, step);

    // A profile request now in flight owes its answer a legacy reply type.
    if (t.outcome == legacy::Outcome::Translated && t.toGameService.size() == 1) {
      const uint64_t sent = t.toGameService[0].symbol;
      if (sent == codec::kSymLoggedInUserProfileRequest) {
        ownProfileReplies_.push_back(Reply::RefreshProfileResult);
      } else if (sent == codec::kSymOtherUserProfileRequest) {
        const bool refresh = message.symbol == legacy::kSNSRefreshProfile;
        otherProfileReplies_.push_back(refresh ? Reply::RefreshProfileResult : Reply::ProfileResponse);
      }
    }
  }
  return step;
}

Step LegacySession::FromGameService(const std::string& frame) {
  Step step;
  std::size_t offset = 0;
  for (;;) {
    codec::Message message;
    const codec::ReadStatus status = codec::ReadMessage(frame, offset, &message);
    if (status == codec::ReadStatus::End) break;
    if (status != codec::ReadStatus::Ok) {
      ++stats_.malformed;
      break;
    }
    const std::string payload(reinterpret_cast<const char*>(message.payload), static_cast<std::size_t>(message.length));
    const std::string original = codec::BuildMessage(message.symbol, payload);
    offset += codec::kHeaderSize + static_cast<std::size_t>(message.length);

    if (message.symbol == codec::kSymLoginSuccess) {
      // Layout: session id (16), then the user id (16). The game client always receives it unchanged.
      step.toGameClient.append(original);
      ++stats_.passed;
      if (payload.size() < 32) continue;
      legacy::Guid session{};
      for (std::size_t i = 0; i < session.size(); ++i) session[i] = static_cast<uint8_t>(payload[i]);
      if (loggedIn_ && session == loginSession_) continue;  // the same login answered again: nothing new to ask

      ForgetLogin();
      loginSession_ = session;
      const uint8_t* bytes = reinterpret_cast<const uint8_t*>(payload.data());
      self_.platformCode = codec::ReadLE64(bytes + 16);
      self_.accountId = codec::ReadLE64(bytes + 24);
      loggedIn_ = true;
      // The legacy game client never asks for its own profile, so ask on its behalf, once per login.
      const legacy::OutMessage ask = legacy::BuildLoggedInUserProfileRequest(MakeContext());
      step.toGameService.append(codec::BuildMessage(ask.symbol, ask.payload));
      ownProfileReplies_.push_back(Reply::LoginProfileResult);
      ++stats_.loginsInjected;
      continue;
    }

    if (message.symbol == codec::kSymLoggedInUserProfileSuccess ||
        message.symbol == codec::kSymLoggedInUserProfileFailure) {
      if (ownProfileReplies_.empty()) {
        ++stats_.profileAnswersUnmatched;
        continue;
      }
      const Reply reply = ownProfileReplies_.front();
      ownProfileReplies_.pop_front();
      if (message.symbol == codec::kSymLoggedInUserProfileFailure) {
        ++stats_.profileFailuresDropped;
        continue;
      }
      Route(legacy::CurrentToLegacy(message.symbol, payload, MakeContext(), config_.tables, reply), original, false,
            stats_, step);
      continue;
    }

    if (message.symbol == codec::kSymOtherUserProfileSuccess || message.symbol == codec::kSymOtherUserProfileFailure) {
      if (otherProfileReplies_.empty()) {
        ++stats_.profileAnswersUnmatched;
        continue;
      }
      const Reply reply = otherProfileReplies_.front();
      otherProfileReplies_.pop_front();
      if (message.symbol == codec::kSymOtherUserProfileFailure) {
        ++stats_.profileFailuresDropped;
        continue;
      }
      Route(legacy::CurrentToLegacy(message.symbol, payload, MakeContext(), config_.tables, reply), original, false,
            stats_, step);
      continue;
    }

    if (message.symbol == codec::kSymLoginFailure) {
      Route(legacy::CurrentToLegacy(message.symbol, payload, MakeContext(), config_.tables), original, false, stats_,
            step);
      ForgetLogin();
      continue;
    }

    Route(legacy::CurrentToLegacy(message.symbol, payload, MakeContext(), config_.tables), original, false, stats_,
          step);
  }
  return step;
}

}  // namespace nevr_legacy_session
