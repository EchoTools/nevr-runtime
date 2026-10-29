#pragma once
// #966 slice S3 (docs/adr/0017, #1181): the invite service's transport on the Quest. One POST of a JSON body to the service's RPC
// (<auth.http_uri>/v2/rpc/<rpc>?unwrap, decided by the shared nevr_invite::BuildRpcUrl) with `Authorization: Bearer <token>`,
// over the Quest client (quest/auth/curl_http.h): the Quest CA bundle, peer and host verification, no redirects, the connect and
// the whole request timed, the answer capped. Any failure is status 0 and one `[NEVR.INVITE] transport ...` line that names the
// cause and never the token, the body or the address. Called from the worker thread only; Abort() may be called from any thread.
#include <atomic>
#include <memory>
#include <string>
#include <string_view>

#include "quest/auth/curl_http.h"
#include "runtime/patch/invite_transport_curl.h"
#include "runtime/patch/invite_worker.h"

namespace nevr_quest_invite {

class QuestInviteTransport final : public nevr_invite::Transport {
 public:
  // allow_plain_http exists for the loopback tests only; production passes false (https only, and no request without trust
  // anchors). Options: the connect and total timeouts, the answer cap and the log sink.
  QuestInviteTransport(std::string base_url, nevr_invite::TransportOptions options, bool allow_plain_http = false);
  nevr_invite::HttpAnswer Post(std::string_view rpc, std::string_view body, std::string_view bearer) override;
  void Abort() override;

 private:
  void Say(nevr_invite::LogLevel level, const std::string& line) const;

  std::string base_;
  nevr_invite::TransportOptions options_;
  nevr::quest_auth::CurlHttpClient http_;
  std::atomic<bool> aborted_{false};
};

/// The transport the Quest run hands the service (invite_run.cpp). The default is the production client.
std::unique_ptr<nevr_invite::Transport> MakeQuestTransport(const std::string& base_url, const nevr_invite::TransportOptions& options,
                                                           bool allow_plain_http = false);

}  // namespace nevr_quest_invite
