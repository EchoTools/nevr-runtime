#include "quest/invite/invite_transport_quest.h"

#include <string>
#include <utility>
#include <vector>

#include "runtime/patch/invite_transport.h"

namespace nevr_quest_invite {

namespace {

// A bearer token travels in one header line: printable ASCII, no space, no line break. Anything else would carry a second header.
bool HeaderSafe(std::string_view value) {
  for (const char c : value) {
    if (c <= ' ' || c > '~') return false;
  }
  return true;
}

// The client's own lines (the trust bundle's load) go to the invite log at the invite's levels.
nevr_invite::LogLevel InviteLevel(nevr::auth::LogLevel level) {
  switch (level) {
    case nevr::auth::LogLevel::Warning: return nevr_invite::LogLevel::Warning;
    case nevr::auth::LogLevel::Error: return nevr_invite::LogLevel::Error;
    case nevr::auth::LogLevel::Debug:
    case nevr::auth::LogLevel::Info: break;
  }
  return nevr_invite::LogLevel::Info;
}

}  // namespace

QuestInviteTransport::QuestInviteTransport(std::string base_url, nevr_invite::TransportOptions options, bool allow_plain_http)
    : base_(std::move(base_url)),
      options_(std::move(options)),
      http_(nevr::quest_auth::AndroidCaDirs(),
            [log = options_.log](nevr::auth::LogLevel level, const std::string& line) {
              if (log) log(InviteLevel(level), line);
            },
            options_.timeout_s, allow_plain_http, options_.max_body_bytes) {}

nevr_invite::HttpAnswer QuestInviteTransport::Post(std::string_view rpc, std::string_view body, std::string_view bearer) {
  nevr_invite::HttpAnswer answer;
  if (aborted_.load(std::memory_order_acquire)) return answer;
  const nevr_invite::Endpoint endpoint = nevr_invite::BuildRpcUrl(base_, rpc);
  if (endpoint.check != nevr_invite::UrlCheck::Ok) {
    Say(nevr_invite::LogLevel::Warning,
        std::string("[NEVR.INVITE] transport refused reason=") + nevr_invite::UrlCheckName(endpoint.check));
    return answer;
  }
  if (bearer.empty()) {
    Say(nevr_invite::LogLevel::Warning, "[NEVR.INVITE] transport refused reason=no_token");
    return answer;
  }
  if (!HeaderSafe(bearer)) {
    Say(nevr_invite::LogLevel::Warning, "[NEVR.INVITE] transport refused reason=bad_token");
    return answer;
  }
  const std::vector<std::string> headers{std::string("Authorization: Bearer ") + std::string(bearer)};
  const nevr::auth::HttpResponse response =
      http_.PostJsonWithHeaders(endpoint.url, std::string(body), headers, options_.connect_timeout_s, options_.timeout_s);
  if (!response.transport_ok) {
    if (response.transport_code == nevr::quest_auth::CurlHttpClient::kNoTrustAnchors) {
      Say(nevr_invite::LogLevel::Warning, "[NEVR.INVITE] transport refused reason=no_trust_anchors");
    } else if (response.transport_code == nevr::quest_auth::CurlHttpClient::kInterrupted) {
      Say(nevr_invite::LogLevel::Warning, "[NEVR.INVITE] transport failed reason=aborted");
    } else {
      Say(nevr_invite::LogLevel::Warning, "[NEVR.INVITE] transport failed curl_code=" + std::to_string(response.transport_code));
    }
    return answer;
  }
  answer.status = static_cast<int>(response.status);
  answer.body = response.body;
  return answer;
}

void QuestInviteTransport::Abort() {
  aborted_.store(true, std::memory_order_release);
  http_.Interrupt();
}

void QuestInviteTransport::Say(nevr_invite::LogLevel level, const std::string& line) const {
  if (options_.log) options_.log(level, line);
}

std::unique_ptr<nevr_invite::Transport> MakeQuestTransport(const std::string& base_url, const nevr_invite::TransportOptions& options,
                                                           bool allow_plain_http) {
  return std::make_unique<QuestInviteTransport>(base_url, options, allow_plain_http);
}

}  // namespace nevr_quest_invite
