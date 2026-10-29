// #966 slice S3 (docs/adr/0017, #1181): the Quest invite transport and the service seam it plugs into.
//
// The transport posts to the service's RPC with the player's Bearer token. These tests run it against a loopback HTTP server
// on this host. Plain http is allowed only where a test asks for it (the allow_plain_http argument, the same test-only switch
// CurlHttpClient's loopback tests use). The production client is https only and needs trust anchors: a Linux host has none,
// so the production cases below check the fail-closed answer, which is also what a device without its CA store gets.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "quest/auth/ca_bundle.h"
#include "quest/invite/invite_run.h"
#include "quest/invite/invite_transport_quest.h"
#include "quest/tests/mini_test.h"
#include "runtime/patch/invite_service.h"
#include "runtime/patch/invite_wizard.h"

namespace {

using Clock = std::chrono::steady_clock;

double SecondsSince(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }

// Collects the lines a transport or the service logs. The posting thread and the test thread both write to it. The state is
// shared with every sink, so a sink the service keeps after the test ends (its global logger) never points at a dead box.
class LogBox {
 public:
  LogBox() : state_(std::make_shared<State>()) {}
  std::function<void(nevr_invite::LogLevel, const std::string&)> Sink() {
    std::shared_ptr<State> state = state_;
    return [state](nevr_invite::LogLevel /*level*/, const std::string& line) {
      const std::lock_guard<std::mutex> lock(state->mutex);
      state->lines.push_back(line);
    };
  }
  bool Has(const std::string& part) const {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    for (const std::string& line : state_->lines) {
      if (line.find(part) != std::string::npos) return true;
    }
    return false;
  }

 private:
  struct State {
    std::mutex mutex;
    std::vector<std::string> lines;
  };
  std::shared_ptr<State> state_;
};

// The head of a recorded request (everything before the blank line) and its body (everything after it).
std::string HeadOf(const std::string& request) {
  const std::size_t end = request.find("\r\n\r\n");
  return end == std::string::npos ? request : request.substr(0, end);
}
std::string BodyOf(const std::string& request) {
  const std::size_t end = request.find("\r\n\r\n");
  return end == std::string::npos ? std::string() : request.substr(end + 4);
}

// The Content-Length of a request head, 0 when it has none. The head is ASCII; the header name is matched in lower case.
std::size_t ContentLengthOf(const std::string& head) {
  std::string lower = head;
  for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  const std::size_t at = lower.find("\r\ncontent-length:");
  if (at == std::string::npos) return 0;
  return static_cast<std::size_t>(std::strtoul(lower.c_str() + at + 17, nullptr, 10));
}

// A complete HTTP/1.1 response that closes the connection, with the body's exact length.
std::string Response(int status, const std::string& reason, const std::string& extra_headers, const std::string& body) {
  return "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n" + extra_headers +
         "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

// A loopback HTTP server for one test, on its own thread. Each accepted connection is read to the end of its request (the head
// and a Content-Length body), recorded, and answered with the next reply; the last reply repeats. A hold reply is never answered
// and ends when the server is destroyed. A request the server cannot read within five seconds is recorded as far as it got.
class Loopback {
 public:
  struct Reply {
    std::string raw;  // the whole response: status line, headers, blank line, body
    bool hold = false;
  };

  explicit Loopback(std::vector<Reply> replies) : replies_(std::move(replies)) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    socklen_t len = sizeof(addr);
    const bool ready = listen_fd_ >= 0 &&
                       ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
                       ::listen(listen_fd_, 4) == 0 &&
                       ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0;
    CHECK(ready);
    port_ = ready ? static_cast<int>(ntohs(addr.sin_port)) : 0;
    thread_ = std::thread([this] { Run(); });
  }
  ~Loopback() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    if (listen_fd_ >= 0) ::close(listen_fd_);
  }
  Loopback(const Loopback&) = delete;
  Loopback& operator=(const Loopback&) = delete;

  std::string Url() const { return "http://127.0.0.1:" + std::to_string(port_); }
  std::vector<std::string> Requests() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return requests_;
  }

 private:
  void Run() {
    while (!stop_) {
      pollfd pfd{listen_fd_, POLLIN, 0};
      if (::poll(&pfd, 1, 20) <= 0) continue;
      const int fd = ::accept(listen_fd_, nullptr, nullptr);
      if (fd < 0) continue;
      Serve(fd);
    }
  }

  void Serve(int fd) {
    timeval timeout{5, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    std::string data;
    char buf[4096];
    std::size_t needed = std::string::npos;  // head plus body, once the head has arrived
    for (;;) {
      const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
      if (n <= 0) break;
      data.append(buf, static_cast<std::size_t>(n));
      if (needed == std::string::npos) {
        const std::size_t head_end = data.find("\r\n\r\n");
        if (head_end != std::string::npos) needed = head_end + 4 + ContentLengthOf(data.substr(0, head_end));
      }
      if (needed != std::string::npos && data.size() >= needed) break;
    }
    Reply reply;
    reply.raw = Response(500, "No Reply", "", "");
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      requests_.push_back(data);
      if (!replies_.empty()) reply = replies_[std::min(requests_.size() - 1, replies_.size() - 1)];
    }
    if (reply.hold) {
      while (!stop_) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } else {
      ::send(fd, reply.raw.data(), reply.raw.size(), MSG_NOSIGNAL);
    }
    ::close(fd);
  }

  std::vector<Reply> replies_;
  int listen_fd_ = -1;
  int port_ = 0;
  std::atomic<bool> stop_{false};
  mutable std::mutex mutex_;
  std::vector<std::string> requests_;
  std::thread thread_;
};

nevr_invite::TransportOptions OptionsWithLog(LogBox& log) {
  nevr_invite::TransportOptions options;
  options.log = log.Sink();
  return options;
}

}  // namespace

// --- the transport --------------------------------------------------------------------------------

TEST(InviteQuestTransport_PostsTheRpcWithTheBearerAndTheBodyByteForByte) {
  Loopback server({{Response(200, "OK", "Content-Type: application/json\r\n", "{\"ok\":true}"), false}});
  const std::string body = "{\"code\":\"ABCDE\"}";
  nevr_quest_invite::QuestInviteTransport transport(server.Url(), {}, /*allow_plain_http=*/true);
  const nevr_invite::HttpAnswer answer = transport.Post("invite/resolve", body, "tok-123");
  CHECK_EQ(answer.status, 200);
  CHECK(answer.body == "{\"ok\":true}");
  const std::vector<std::string> requests = server.Requests();
  CHECK_EQ(requests.size(), static_cast<std::size_t>(1));
  if (requests.size() == 1) {
    CHECK(requests[0].rfind("POST /v2/rpc/invite/resolve?unwrap HTTP/1.1\r\n", 0) == 0);
    CHECK(HeadOf(requests[0]).find("Authorization: Bearer tok-123\r\n") != std::string::npos);
    CHECK(HeadOf(requests[0]).find("Content-Type: application/json\r\n") != std::string::npos);
    CHECK(BodyOf(requests[0]) == body);
  }
}

TEST(InviteQuestTransport_ARedirectIsNotFollowed) {
  Loopback server({{Response(302, "Found", "Location: http://127.0.0.1:1/elsewhere\r\n", ""), false}});
  nevr_quest_invite::QuestInviteTransport transport(server.Url(), {}, /*allow_plain_http=*/true);
  const nevr_invite::HttpAnswer answer = transport.Post("invite/resolve", "{}", "tok");
  CHECK_EQ(answer.status, 302);  // a followed redirect would fail against port 1 and answer status 0
  CHECK_EQ(server.Requests().size(), static_cast<std::size_t>(1));
}

TEST(InviteQuestTransport_ABodyOverTheCapIsNoAnswer) {
  Loopback server({{Response(200, "OK", "", std::string(64, 'x')), false}});
  LogBox log;
  nevr_invite::TransportOptions options = OptionsWithLog(log);
  options.max_body_bytes = 16;
  nevr_quest_invite::QuestInviteTransport transport(server.Url(), options, /*allow_plain_http=*/true);
  const nevr_invite::HttpAnswer answer = transport.Post("invite/resolve", "{}", "tok");
  CHECK_EQ(answer.status, 0);
  CHECK(answer.body.empty());
  CHECK(log.Has("transport failed curl_code=23"));  // CURLE_WRITE_ERROR: the write callback refused the bytes past the cap
}

TEST(InviteQuestTransport_PlainHttpToARemoteHostIsRefusedBeforeAnyConnection) {
  LogBox log;
  nevr_quest_invite::QuestInviteTransport transport("http://example.com", OptionsWithLog(log));
  const nevr_invite::HttpAnswer answer = transport.Post("invite/resolve", "{}", "tok");
  CHECK_EQ(answer.status, 0);
  CHECK(log.Has("[NEVR.INVITE] transport refused reason=plain_http_to_remote"));
}

TEST(InviteQuestTransport_WithNoTrustAnchorsNothingIsSent) {
  // The host has no Android CA store, so the production client has no anchors and must fail closed.
  CHECK_EQ(nevr::quest_auth::LoadCaBundle(nevr::quest_auth::AndroidCaDirs(), nullptr).certificates,
           static_cast<std::size_t>(0));
  Loopback server({{Response(200, "OK", "", "{}"), false}});
  LogBox log;
  nevr_quest_invite::QuestInviteTransport transport(server.Url(), OptionsWithLog(log));  // production: https only
  const nevr_invite::HttpAnswer answer = transport.Post("invite/resolve", "{}", "tok");
  CHECK_EQ(answer.status, 0);
  CHECK_EQ(server.Requests().size(), static_cast<std::size_t>(0));
  CHECK(log.Has("[NEVR.INVITE] transport refused reason=no_trust_anchors"));
}

TEST(InviteQuestTransport_AnEmptyTokenSendsNothing) {
  Loopback server({{Response(200, "OK", "", "{}"), false}});
  LogBox log;
  nevr_quest_invite::QuestInviteTransport transport(server.Url(), OptionsWithLog(log), /*allow_plain_http=*/true);
  const nevr_invite::HttpAnswer answer = transport.Post("invite/resolve", "{}", "");
  CHECK_EQ(answer.status, 0);
  CHECK_EQ(server.Requests().size(), static_cast<std::size_t>(0));
  CHECK(log.Has("[NEVR.INVITE] transport refused reason=no_token"));
}

TEST(InviteQuestTransport_ATokenWithALineBreakSendsNothing) {
  // A header value with a line break would carry a second header to the service. The token is refused, not sent.
  Loopback server({{Response(200, "OK", "", "{}"), false}});
  LogBox log;
  nevr_quest_invite::QuestInviteTransport transport(server.Url(), OptionsWithLog(log), /*allow_plain_http=*/true);
  const nevr_invite::HttpAnswer answer = transport.Post("invite/resolve", "{}", "tok\r\nX-Injected: 1");
  CHECK_EQ(answer.status, 0);
  CHECK_EQ(server.Requests().size(), static_cast<std::size_t>(0));
  CHECK(log.Has("[NEVR.INVITE] transport refused reason=bad_token"));
  CHECK(!log.Has("X-Injected"));
}

TEST(InviteQuestTransport_AbortReturnsAnInFlightRequestAtOnce) {
  Loopback server({{"", true}});  // accepts, reads the request, never answers
  nevr_quest_invite::QuestInviteTransport transport(server.Url(), {}, /*allow_plain_http=*/true);
  nevr_invite::HttpAnswer answer;
  std::thread poster([&] { answer = transport.Post("invite/resolve", "{}", "tok"); });
  CHECK(mini_test::WaitUntil([&] { return !server.Requests().empty(); }, 5000));
  const Clock::time_point aborted = Clock::now();
  transport.Abort();
  poster.join();
  // The request's own timeout is ten seconds: a return inside five is the abort, not the timeout.
  CHECK(SecondsSince(aborted) < 5.0);
  CHECK_EQ(answer.status, 0);
}

TEST(InviteQuestTransport_AfterAbortNothingIsSent) {
  Loopback server({{Response(200, "OK", "", "{}"), false}});
  nevr_quest_invite::QuestInviteTransport transport(server.Url(), {}, /*allow_plain_http=*/true);
  transport.Abort();
  const nevr_invite::HttpAnswer answer = transport.Post("invite/resolve", "{}", "tok");
  CHECK_EQ(answer.status, 0);
  CHECK_EQ(server.Requests().size(), static_cast<std::size_t>(0));
}

TEST(InviteQuestTransport_ATimeoutIsNoAnswer) {
  Loopback server({{"", true}});
  LogBox log;
  nevr_invite::TransportOptions options = OptionsWithLog(log);
  options.timeout_s = 1;
  nevr_quest_invite::QuestInviteTransport transport(server.Url(), options, /*allow_plain_http=*/true);
  const Clock::time_point start = Clock::now();
  const nevr_invite::HttpAnswer answer = transport.Post("invite/resolve", "{}", "tok");
  const double elapsed = SecondsSince(start);
  CHECK_EQ(answer.status, 0);
  CHECK(elapsed > 0.5 && elapsed < 6.0);  // the one-second timeout, not the library default
  CHECK(log.Has("transport failed curl_code=28"));  // CURLE_OPERATION_TIMEDOUT
}

TEST(InviteQuestTransport_TheProductionFactoryRefusesPlainHttpToLoopback) {
  // MakeQuestTransport's default is the production client: https only. Plain http to this machine is not sent.
  Loopback server({{Response(200, "OK", "", "{}"), false}});
  LogBox log;
  std::unique_ptr<nevr_invite::Transport> transport = nevr_quest_invite::MakeQuestTransport(server.Url(), OptionsWithLog(log));
  CHECK(transport != nullptr);
  if (transport) {
    const nevr_invite::HttpAnswer answer = transport->Post("invite/resolve", "{}", "tok");
    CHECK_EQ(answer.status, 0);
  }
  CHECK_EQ(server.Requests().size(), static_cast<std::size_t>(0));
}

// --- the service seam -----------------------------------------------------------------------------

namespace {

nevr_invite_service::Settings QuestSettings(const std::string& http_uri, std::function<std::string()> token, LogBox& log) {
  nevr_invite_service::Settings settings;
  settings.invite_codes = nevr_invite::Switch::On;
  settings.http_uri = http_uri;
  settings.token = std::move(token);
  settings.log = log.Sink();
  settings.salt = "invite-quest-test";
  settings.transport_factory = [](const std::string& base, const nevr_invite::TransportOptions& options) {
    return nevr_quest_invite::MakeQuestTransport(base, options, /*allow_plain_http=*/true);
  };
  return settings;
}

nevr_invite::Plan ResolvePlan() {
  nevr_invite::Plan plan;
  plan.op = nevr_invite::Op::Resolve;
  plan.request = nevr_invite::Request{"invite/resolve", "{\"code\":\"ABCDE\"}"};
  plan.code = "ABCDE";
  plan.code_len = 5;
  return plan;
}

}  // namespace

TEST(InviteQuestService_AResolveReachesTheServiceWithTheBearerAndLogsNeitherTheTokenNorTheCode) {
  Loopback server({{Response(200, "OK", "Content-Type: application/json\r\n", "{\"lobby\":\"x\"}"), false}});
  LogBox log;
  nevr_invite_service::Start(QuestSettings(server.Url(), [] { return std::string("Sentinel-Token-Q7x"); }, log));
  CHECK(nevr_invite_service::Submit(ResolvePlan()) == nevr_invite::Submit::Queued);
  const std::vector<nevr_invite::Answer> answers = nevr_invite_service::WaitForAnswers(std::chrono::seconds(5));
  CHECK_EQ(answers.size(), static_cast<std::size_t>(1));
  if (answers.size() == 1) {
    CHECK(answers[0].sent);
    CHECK_EQ(answers[0].status, 200);
    CHECK(answers[0].result == nevr_invite::Result::Ok);
  }
  nevr_invite_service::Shutdown();
  const std::vector<std::string> requests = server.Requests();
  CHECK_EQ(requests.size(), static_cast<std::size_t>(1));
  if (requests.size() == 1) CHECK(HeadOf(requests[0]).find("Authorization: Bearer Sentinel-Token-Q7x\r\n") != std::string::npos);
  CHECK(log.Has("[NEVR.INVITE] service on"));
  CHECK(!log.Has("Sentinel-Token-Q7x"));
  CHECK(!log.Has("ABCDE"));
}

TEST(InviteQuestService_ATokenWithALineBreakIsRefusedThroughTheSeam) {
  // The libcurl transport would send this token; the Quest transport refuses it. The refusal line is the proof the seam ran.
  Loopback server({{Response(200, "OK", "", "{}"), false}});
  LogBox log;
  nevr_invite_service::Start(QuestSettings(server.Url(), [] { return std::string("tok\r\nX-Injected: 1"); }, log));
  CHECK(nevr_invite_service::Submit(ResolvePlan()) == nevr_invite::Submit::Queued);
  const std::vector<nevr_invite::Answer> answers = nevr_invite_service::WaitForAnswers(std::chrono::seconds(5));
  CHECK_EQ(answers.size(), static_cast<std::size_t>(1));
  nevr_invite_service::Shutdown();
  CHECK(log.Has("[NEVR.INVITE] transport refused reason=bad_token"));
  CHECK_EQ(server.Requests().size(), static_cast<std::size_t>(0));
}

TEST(InviteQuestService_AFactoryThatReturnsNothingLeavesTheServiceOff) {
  LogBox log;
  nevr_invite_service::Settings settings = QuestSettings("http://127.0.0.1:1", [] { return std::string("tok"); }, log);
  settings.transport_factory = [](const std::string&, const nevr_invite::TransportOptions&) {
    return std::unique_ptr<nevr_invite::Transport>();
  };
  nevr_invite_service::Start(settings);
  CHECK(log.Has("[NEVR.INVITE] feature off reason=transport_unavailable"));
  CHECK(nevr_invite_service::Submit(ResolvePlan()) == nevr_invite::Submit::NotConfigured);
  nevr_invite_service::Shutdown();
}

TEST(InviteQuestService_ShutdownDoesNotWaitOutARequestInFlight) {
  // Shutdown aborts the transport before it joins the worker, so a request the worker is waiting on returns at once.
  Loopback server({{"", true}});
  LogBox log;
  nevr_invite_service::Start(QuestSettings(server.Url(), [] { return std::string("Sentinel-Token-Q7x"); }, log));
  CHECK(nevr_invite_service::Submit(ResolvePlan()) == nevr_invite::Submit::Queued);
  CHECK(mini_test::WaitUntil([&] { return !server.Requests().empty(); }, 5000));
  const Clock::time_point started = Clock::now();
  nevr_invite_service::Shutdown();
  CHECK(SecondsSince(started) < 5.0);  // the transport's own total timeout is ten seconds
}

// --- the run: the feature gate and the start and stop the sentinel calls ---------------------------

TEST(InviteQuestRun_TheStubGateIsOffAndSendsNothing) {
  // S1 replaces this with the network.invite_codes feature; this check then fails on purpose and is updated with it.
  CHECK(nevr_quest_invite::InviteCodesSwitch() == nevr_invite::Switch::Off);
  Loopback server({{Response(200, "OK", "", "{}"), false}});
  LogBox log;
  nevr_quest_invite::RunInputs inputs;
  inputs.http_uri = server.Url();
  inputs.token = [] { return std::string("tok"); };
  inputs.log = log.Sink();
  nevr_quest_invite::Start(inputs);
  CHECK(log.Has("[NEVR.INVITE] feature off reason=not_configured"));
  CHECK(nevr_invite_service::Submit(ResolvePlan()) == nevr_invite::Submit::NotConfigured);
  CHECK(log.Has("[NEVR.INVITE] request not sent reason=not_configured"));
  nevr_quest_invite::Shutdown();
  CHECK_EQ(server.Requests().size(), static_cast<std::size_t>(0));
}

int main(int argc, char** argv) { return mini_test::RunAll(argc, argv); }
