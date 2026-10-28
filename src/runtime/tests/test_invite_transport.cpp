// #1156 (PR-D slice D1b), docs/adr/0017: where an invite request may go (patch/invite_transport.h) and the libcurl transport
// that sends it (patch/invite_transport_curl.cpp), driven against a scripted loopback HTTP server.
//
// Prediction, written before the code. A resolve through the transport to a loopback server writes this request:
//   POST /v2/rpc/invite/resolve?unwrap HTTP/1.1, Authorization: Bearer <the token>, Content-Type: application/json, and the
//   body byte for byte. The server's 404 with {"message":"invalid"} comes back as status 404 and that body. A redirect is not
//   followed (the token would go with it); a body past the cap, a refused connection and an Abort each come back as status 0.
// Falsifier: a second request after a redirect, a status other than the server's, or an Abort that waits out the timeout.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "runtime/patch/invite_transport.h"
#include "runtime/patch/invite_transport_curl.h"
#include "runtime/tests/loopback_http_server.h"

namespace {

using nevr_invite::BuildRpcUrl;
using nevr_invite::UrlCheck;

TEST(InviteUrl, TheRpcGoesToTheServicesHttpFormWithTheAnswerUnwrapped) {
  const auto e = BuildRpcUrl("https://svc.example", "invite/resolve");
  EXPECT_EQ(e.check, UrlCheck::Ok);
  EXPECT_EQ(e.url, "https://svc.example/v2/rpc/invite/resolve?unwrap");
}

TEST(InviteUrl, ATrailingSlashOnTheBaseIsDropped) {
  EXPECT_EQ(BuildRpcUrl("https://svc.example:7350//", "invite/create").url,
            "https://svc.example:7350/v2/rpc/invite/create?unwrap");
}

TEST(InviteUrl, ABaseWithAPathKeepsIt) {
  EXPECT_EQ(BuildRpcUrl("https://svc.example/api", "invite/create").url, "https://svc.example/api/v2/rpc/invite/create?unwrap");
}

TEST(InviteUrl, NothingToGoToIsRefused) {
  EXPECT_EQ(BuildRpcUrl("", "invite/create").check, UrlCheck::Empty);
  EXPECT_EQ(BuildRpcUrl("///", "invite/create").check, UrlCheck::Empty);
}

TEST(InviteUrl, OnlyHttpAndHttpsAreSchemes) {
  EXPECT_EQ(BuildRpcUrl("ftp://svc.example", "invite/create").check, UrlCheck::BadScheme);
  EXPECT_EQ(BuildRpcUrl("svc.example", "invite/create").check, UrlCheck::BadScheme);
  EXPECT_EQ(BuildRpcUrl("wss://svc.example", "invite/create").check, UrlCheck::BadScheme);
}

TEST(InviteUrl, PlainHttpIsForTheMachineItselfOnly) {
  EXPECT_EQ(BuildRpcUrl("http://127.0.0.1:7350", "invite/create").check, UrlCheck::Ok);
  EXPECT_EQ(BuildRpcUrl("http://localhost:7350", "invite/create").check, UrlCheck::Ok);
  EXPECT_EQ(BuildRpcUrl("http://[::1]:7350", "invite/create").check, UrlCheck::Ok);
  EXPECT_EQ(BuildRpcUrl("http://svc.example", "invite/create").check, UrlCheck::PlainHttpToRemote);
  EXPECT_EQ(BuildRpcUrl("http://127.0.0.1.example", "invite/create").check, UrlCheck::PlainHttpToRemote);
  EXPECT_EQ(BuildRpcUrl("http://localhost.example:7350", "invite/create").check, UrlCheck::PlainHttpToRemote);
}

TEST(InviteUrl, ABaseThatCouldRedirectTheTokenIsRefused) {
  EXPECT_EQ(BuildRpcUrl("https://user@svc.example", "invite/create").check, UrlCheck::BadBase);
  EXPECT_EQ(BuildRpcUrl("https://svc.example?x=1", "invite/create").check, UrlCheck::BadBase);
  EXPECT_EQ(BuildRpcUrl("https://svc.example#frag", "invite/create").check, UrlCheck::BadBase);
  EXPECT_EQ(BuildRpcUrl("https://svc.example back", "invite/create").check, UrlCheck::BadBase);
  EXPECT_EQ(BuildRpcUrl("https://svc.example\\evil", "invite/create").check, UrlCheck::BadBase);
  EXPECT_EQ(BuildRpcUrl("https://", "invite/create").check, UrlCheck::BadScheme);  // the slashes are trimmed first: no host
  EXPECT_EQ(BuildRpcUrl("https:///path", "invite/create").check, UrlCheck::BadBase);
}

TEST(InviteUrl, AnRpcNameIsLowerCaseWordsWithSingleSlashes) {
  for (const char* bad : {"", "/invite", "invite/", "invite//create", "Invite/create", "invite/create?x", "invite/../x",
                          "invite create", "invite/create\n"}) {
    EXPECT_EQ(BuildRpcUrl("https://svc.example", bad).check, UrlCheck::BadRpc) << bad;
  }
}

// ---- the transport against a scripted loopback server ----

using loopback::LoopbackServer;
using loopback::Script;
using loopback::Seen;

struct Logged {
  std::mutex m;
  std::vector<std::string> lines;
  nevr_invite::TransportOptions Options() {
    nevr_invite::TransportOptions options;
    options.log = [this](nevr_invite::LogLevel, const std::string& line) {
      std::lock_guard<std::mutex> lock(m);
      lines.push_back(line);
    };
    return options;
  }
  std::vector<std::string> Lines() {
    std::lock_guard<std::mutex> lock(m);
    return lines;
  }
};

}  // namespace

TEST(InviteCurl, AResolveIsAPostToTheRpcWithTheBearerAndTheBodyByteForByte) {
  LoopbackServer server({{Script::Kind::Reply, 404, R"({"message":"invalid"})", ""}});
  Logged logged;
  nevr_invite::CurlTransport transport(server.Base(), logged.Options());

  const nevr_invite::HttpAnswer answer = transport.Post("invite/resolve", R"({"code":"ABCD2345"})", "tok-secret");
  EXPECT_EQ(answer.status, 404);
  EXPECT_EQ(answer.body, R"({"message":"invalid"})");

  const std::vector<Seen> seen = server.Requests();
  ASSERT_EQ(seen.size(), 1u);
  EXPECT_EQ(seen[0].request_line, "POST /v2/rpc/invite/resolve?unwrap HTTP/1.1");
  EXPECT_EQ(seen[0].authorization, "Bearer tok-secret");
  EXPECT_EQ(seen[0].content_type, "application/json");
  EXPECT_EQ(seen[0].body, R"({"code":"ABCD2345"})");
}

TEST(InviteCurl, ASuccessBodyComesBackWhole) {
  const std::string payload = R"({"code":"KQ7M2X4P","match_id":"2f1c9b0e-6a3d-4c58-9d11-0a7e5b3c4d21"})";
  LoopbackServer server({{Script::Kind::Reply, 200, payload, ""}});
  Logged logged;
  nevr_invite::CurlTransport transport(server.Base(), logged.Options());
  const nevr_invite::HttpAnswer answer = transport.Post("invite/create", "{}", "t");
  EXPECT_EQ(answer.status, 200);
  EXPECT_EQ(answer.body, payload);
}

TEST(InviteCurl, ARedirectIsNotFollowedSoTheTokenGoesNowhereElse) {
  LoopbackServer server({{Script::Kind::Reply, 302, "", "/elsewhere"}, {Script::Kind::Reply, 200, "{}", ""}});
  Logged logged;
  nevr_invite::CurlTransport transport(server.Base(), logged.Options());
  const nevr_invite::HttpAnswer answer = transport.Post("invite/resolve", "{}", "tok-secret");
  EXPECT_EQ(answer.status, 302);
  EXPECT_EQ(server.Requests().size(), 1u);
}

TEST(InviteCurl, AnAnswerPastTheCapIsRefusedAsNoAnswer) {
  LoopbackServer server({{Script::Kind::Reply, 200, std::string(2000, 'x'), ""}});
  Logged logged;
  nevr_invite::TransportOptions options = logged.Options();
  options.max_body_bytes = 1000;
  nevr_invite::CurlTransport transport(server.Base(), options);
  const nevr_invite::HttpAnswer answer = transport.Post("invite/create", "{}", "t");
  EXPECT_EQ(answer.status, 0);
  EXPECT_TRUE(answer.body.empty());
}

TEST(InviteCurl, AnAnswerExactlyTheCapIsKept) {
  LoopbackServer server({{Script::Kind::Reply, 200, std::string(1000, 'x'), ""}});
  Logged logged;
  nevr_invite::TransportOptions options = logged.Options();
  options.max_body_bytes = 1000;
  nevr_invite::CurlTransport transport(server.Base(), options);
  const nevr_invite::HttpAnswer answer = transport.Post("invite/create", "{}", "t");
  EXPECT_EQ(answer.status, 200);
  EXPECT_EQ(answer.body.size(), 1000u);
}

TEST(InviteCurl, ARefusedConnectionIsNoAnswerAndTheLineSaysWhy) {
  // Bind and close a port so nothing listens on it.
  std::string base;
  {
    LoopbackServer gone({});
    base = gone.Base();
  }
  Logged logged;
  nevr_invite::CurlTransport transport(base, logged.Options());
  const nevr_invite::HttpAnswer answer = transport.Post("invite/resolve", "{}", "tok-secret");
  EXPECT_EQ(answer.status, 0);
  const std::vector<std::string> lines = logged.Lines();
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].find("[NEVR.INVITE] transport failed curl_code="), 0u);
  EXPECT_EQ(lines[0].find("tok-secret"), std::string::npos);
}

TEST(InviteCurl, AnAddressTheTokenMayNotGoToIsRefusedBeforeAnyConnection) {
  Logged logged;
  nevr_invite::CurlTransport transport("http://svc.example", logged.Options());
  const nevr_invite::HttpAnswer answer = transport.Post("invite/resolve", "{}", "tok-secret");
  EXPECT_EQ(answer.status, 0);
  const std::vector<std::string> lines = logged.Lines();
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0], "[NEVR.INVITE] transport refused reason=plain_http_to_remote");
}

TEST(InviteCurl, ASlowServerIsGivenUpOnAtTheTimeout) {
  LoopbackServer server({{Script::Kind::Hold, 0, "", ""}});
  Logged logged;
  nevr_invite::TransportOptions options = logged.Options();
  options.timeout_s = 1;
  nevr_invite::CurlTransport transport(server.Base(), options);
  const auto start = std::chrono::steady_clock::now();
  const nevr_invite::HttpAnswer answer = transport.Post("invite/resolve", "{}", "t");
  const auto took = std::chrono::steady_clock::now() - start;
  EXPECT_EQ(answer.status, 0);
  EXPECT_LT(took, std::chrono::seconds(8));
  ASSERT_EQ(logged.Lines().size(), 1u);
  EXPECT_NE(logged.Lines()[0].find("curl_code=28"), std::string::npos);  // CURLE_OPERATION_TIMEDOUT
}

TEST(InviteCurl, AbortReturnsARequestInFlightAtOnce) {
  LoopbackServer server({{Script::Kind::Hold, 0, "", ""}});
  Logged logged;
  nevr_invite::TransportOptions options = logged.Options();
  options.timeout_s = 60;
  nevr_invite::CurlTransport transport(server.Base(), options);

  nevr_invite::HttpAnswer answer;
  std::atomic<bool> done{false};
  std::thread caller([&] {
    answer = transport.Post("invite/resolve", "{}", "t");
    done = true;
  });
  ASSERT_TRUE(server.WaitForRequests(1));  // the request is in the server's hands and unanswered
  const auto start = std::chrono::steady_clock::now();
  transport.Abort();
  caller.join();
  EXPECT_TRUE(done.load());
  EXPECT_EQ(answer.status, 0);
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(8));  // not the 60 s timeout
}

TEST(InviteCurl, AfterAbortNothingIsSent) {
  LoopbackServer server({{Script::Kind::Reply, 200, "{}", ""}});
  Logged logged;
  nevr_invite::CurlTransport transport(server.Base(), logged.Options());
  transport.Abort();
  const nevr_invite::HttpAnswer answer = transport.Post("invite/resolve", "{}", "t");
  EXPECT_EQ(answer.status, 0);
  EXPECT_TRUE(server.Requests().empty());
}
