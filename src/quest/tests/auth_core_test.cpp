// Host + Android test for the platform-neutral auth core and the Quest session.
// No network, no real clock, no Windows: a fake HTTP server and a fake clock drive
// token model, refresh, the device-code loop and the Session worker.
// Built by `just test-quest-shared` (host g++) and by src/quest/CMakeLists.txt (NDK).

#include "core/auth_refresh.h"
#include "core/auth_token_model.h"
#include "core/device_auth_flow.h"
#include "quest/auth/ca_bundle.h"
#include "quest/auth/file_store.h"
#include "quest/auth/session.h"
#include "quest/tests/mini_test.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using namespace nevr::auth;
using namespace nevr::quest_auth;
using std::chrono::seconds;

using namespace mini_test;

// ---------------------------------------------------------------- fixtures
std::string B64Url(const std::string& in) {
  static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  size_t i = 0;
  while (i + 2 < in.size() + 2 && i < in.size()) {
    const uint32_t b0 = static_cast<unsigned char>(in[i]);
    const uint32_t b1 = i + 1 < in.size() ? static_cast<unsigned char>(in[i + 1]) : 0;
    const uint32_t b2 = i + 2 < in.size() ? static_cast<unsigned char>(in[i + 2]) : 0;
    const uint32_t v = (b0 << 16) | (b1 << 8) | b2;
    out.push_back(t[(v >> 18) & 63]);
    out.push_back(t[(v >> 12) & 63]);
    if (i + 1 < in.size()) out.push_back(t[(v >> 6) & 63]);
    if (i + 2 < in.size()) out.push_back(t[v & 63]);
    i += 3;
  }
  return out;
}

// A JWT-shaped string (not signed, not secret) with an exp and a discord id.
std::string MakeJwt(uint64_t exp, const std::string& discord_id = "424242") {
  nlohmann::json claims;
  claims["exp"] = exp;
  claims["vrs"] = {{"did", discord_id}};
  return B64Url("{\"alg\":\"none\"}") + "." + B64Url(claims.dump()) + ".sig";
}

constexpr uint64_t kT0 = 1'800'000'000;  // fake wall clock start

class FakeClock : public InterruptibleClock {
 public:
  uint64_t UnixNow() override {
    std::lock_guard<std::mutex> l(m_);
    return kT0 + elapsed_;
  }
  std::chrono::steady_clock::time_point SteadyNow() override {
    std::lock_guard<std::mutex> l(m_);
    return std::chrono::steady_clock::time_point{} + seconds(1000 + elapsed_);
  }
  // Each sleep needs a permit from the test, then advances time by its duration.
  bool SleepFor(std::chrono::steady_clock::duration d) override {
    std::unique_lock<std::mutex> l(m_);
    cv_.wait(l, [this] { return permits_ > 0 || interrupted_; });
    if (interrupted_) return true;
    --permits_;
    elapsed_ += static_cast<uint64_t>(std::chrono::duration_cast<seconds>(d).count());
    ++sleeps_;
    return false;
  }
  void Interrupt() override {
    {
      std::lock_guard<std::mutex> l(m_);
      interrupted_ = true;
    }
    cv_.notify_all();
  }
  void Allow(int n) {
    {
      std::lock_guard<std::mutex> l(m_);
      permits_ += n;
    }
    cv_.notify_all();
  }
  void Advance(uint64_t s) {
    std::lock_guard<std::mutex> l(m_);
    elapsed_ += s;
  }
  int Sleeps() {
    std::lock_guard<std::mutex> l(m_);
    return sleeps_;
  }

 private:
  std::mutex m_;
  std::condition_variable cv_;
  uint64_t elapsed_ = 0;
  int permits_ = 0;
  int sleeps_ = 0;
  bool interrupted_ = false;
};

struct Call {
  std::string endpoint;
  std::string url;
  std::string body;
};

class FakeHttp : public HttpClient {
 public:
  std::function<HttpResponse(const std::string& endpoint, const std::string& body)> handler;
  std::atomic<int> interrupts{0};
  void Interrupt() override { ++interrupts; }
  HttpResponse PostJson(const std::string& url, const std::string& body) override {
    const size_t a = url.find("/device/auth/");
    const size_t b = url.find('?', a);
    const std::string endpoint = a == std::string::npos ? "?" : url.substr(a + 13, b - (a + 13));
    {
      std::lock_guard<std::mutex> l(m_);
      calls_.push_back({endpoint, url, body});
    }
    return handler(endpoint, body);
  }
  std::vector<Call> Calls() {
    std::lock_guard<std::mutex> l(m_);
    return calls_;
  }
  int Count(const std::string& endpoint) {
    int n = 0;
    for (const Call& c : Calls()) n += c.endpoint == endpoint ? 1 : 0;
    return n;
  }

 private:
  std::mutex m_;
  std::vector<Call> calls_;
};

HttpResponse Ok(const nlohmann::json& j) {
  HttpResponse r;
  r.transport_ok = true;
  r.status = 200;
  r.body = j.dump();
  return r;
}
HttpResponse Status(long status) {
  HttpResponse r;
  r.transport_ok = true;
  r.status = status;
  r.body = "oops";
  return r;
}
HttpResponse NoTransport(int code = 28) {
  HttpResponse r;
  r.transport_ok = false;
  r.transport_code = code;
  return r;
}

struct LogCapture {
  std::mutex m;
  std::vector<std::string> lines;
  LogSink Sink() {
    return [this](LogLevel, const std::string& s) {
      std::lock_guard<std::mutex> l(m);
      lines.push_back(s);
    };
  }
  std::string All() {
    std::lock_guard<std::mutex> l(m);
    std::string out;
    for (const std::string& s : lines) out += s + "\n";
    return out;
  }
};

class FakeStore : public CredentialStore {
 public:
  CachedAuthToken initial;
  std::vector<CachedAuthToken> saved;
  bool fail_save = false;
  CachedAuthToken Load(uint64_t) override { return initial; }
  bool Save(const CachedAuthToken& a) override {
    std::lock_guard<std::mutex> l(m_);
    if (fail_save) return false;
    saved.push_back(a);
    return true;
  }
  size_t SaveCount() {
    std::lock_guard<std::mutex> l(m_);
    return saved.size();
  }
  CachedAuthToken Last() {
    std::lock_guard<std::mutex> l(m_);
    return saved.back();
  }

 private:
  std::mutex m_;
};

class FakePresenter : public LinkPresenter {
 public:
  std::atomic<int> presented{0};
  std::atomic<int> cleared{0};
  std::atomic<bool> deliver{true};
  std::string last_url;
  intptr_t Present(const std::string& url) override {
    last_url = url;
    ++presented;
    return deliver ? kBrowserOpenAcceptedAbove + 1 : 0;
  }
  void Clear() override { ++cleared; }
};

nlohmann::json RefreshOkBody(uint64_t exp, const std::string& refresh = "rt-new") {
  return {{"access_token", MakeJwt(exp)}, {"refresh_token", refresh}, {"refresh_token_expires_in", 2592000}};
}

CachedAuthToken CachedWithRefresh(const std::string& rt = "rt-old", uint64_t exp = kT0 + 100000) {
  CachedAuthToken a;
  a.refresh_token = rt;
  a.refresh_token_expiry = exp;
  a.user_id = "u1";
  a.username = "name1";
  return a;
}

// ---------------------------------------------------------------- token model
TEST(credentials_roundtrip_never_persists_the_access_token) {
  CachedAuthToken a = CachedWithRefresh();
  a.token = "access-token-should-not-be-written";
  a.token_expiry = kT0 + 10;
  const std::string text = SerializeCredentialsJson(a);
  CHECK(text.find("access-token-should-not-be-written") == std::string::npos);
  const CachedAuthToken b = ParseCredentialsJson(text, kT0);
  CHECK_EQ(b.refresh_token, std::string("rt-old"));
  CHECK_EQ(b.refresh_token_expiry, a.refresh_token_expiry);
  CHECK_EQ(b.user_id, std::string("u1"));
  CHECK_EQ(b.username, std::string("name1"));
  CHECK(b.token.empty());
}

TEST(credentials_parse_clamps_a_legacy_access_token_and_survives_garbage) {
  const std::string legacy = R"({"token":"t","token_expiry":9999999999,"refresh_token":"r"})";
  CHECK_EQ(ParseCredentialsJson(legacy, kT0).token_expiry, kT0 + kMaxDiskAccessTokenLifetimeSec);
  CHECK(ParseCredentialsJson("not json", kT0).refresh_token.empty());
  CHECK(ParseCredentialsJson("[1,2]", kT0).refresh_token.empty());
  CHECK(ParseCredentialsJson("", kT0).token.empty());
}

TEST(expiry_authority_order_is_jwt_then_expires_in_then_fallback) {
  CHECK_EQ(ResolveAccessTokenExpirySec(kT0, MakeJwt(kT0 + 3600), 10), kT0 + 3600);
  CHECK_EQ(ResolveAccessTokenExpirySec(kT0, "opaque", 7200), kT0 + 7200);
  CHECK_EQ(ResolveAccessTokenExpirySec(kT0, "opaque", std::nullopt), kT0 + kFallbackAccessTokenLifetimeSec);
  CHECK_EQ(ResolveRefreshTokenExpirySec(kT0, 99), kT0 + 99);
  CHECK_EQ(ResolveRefreshTokenExpirySec(kT0, std::nullopt), kT0 + kFallbackRefreshTokenLifetimeSec);
}

TEST(validity_uses_the_injected_clock_with_a_60s_margin) {
  CachedAuthToken a;
  a.token = "t";
  a.token_expiry = kT0 + 61;
  CHECK(a.HasValidToken(kT0));
  a.token_expiry = kT0 + 60;
  CHECK(!a.HasValidToken(kT0));
}

// ---------------------------------------------------------------- refresh
TEST(refresh_url_and_body_match_the_server_contract) {
  CHECK_EQ(BuildDeviceAuthUrl("https://h", "k", "refresh"),
           std::string("https://h/v2/rpc/device/auth/refresh?http_key=k&unwrap"));
  const nlohmann::json body = nlohmann::json::parse(BuildRefreshBody("rt"));
  CHECK_EQ(body.value("refresh_token", ""), std::string("rt"));
  CHECK_EQ(body.value("token", ""), std::string("rt"));
}

TEST(refresh_success_takes_expiry_from_the_jwt_and_rotates_the_refresh_token) {
  FakeHttp http;
  http.handler = [](const std::string&, const std::string&) { return Ok(RefreshOkBody(kT0 + 3600)); };
  CachedAuthToken a = CachedWithRefresh();
  LogCapture log;
  CHECK(RefreshAccessToken(a, "https://h", "k", http, kT0, log.Sink()) == RefreshOutcome::Refreshed);
  CHECK_EQ(a.token_expiry, kT0 + 3600);
  CHECK_EQ(a.refresh_token, std::string("rt-new"));
  CHECK_EQ(a.refresh_token_expiry, kT0 + 2592000);
  CHECK_EQ(a.GetDiscordId(), uint64_t(424242));
  CHECK_EQ(http.Calls()[0].endpoint, std::string("refresh"));
}

TEST(refresh_accepts_the_deprecated_token_field_and_keeps_the_old_refresh_token_when_none_is_sent) {
  FakeHttp http;
  http.handler = [](const std::string&, const std::string&) {
    return Ok({{"token", "legacy-access"}, {"expires_in", 600}});
  };
  CachedAuthToken a = CachedWithRefresh();
  CHECK(RefreshAccessToken(a, "https://h", "k", http, kT0, nullptr) == RefreshOutcome::Refreshed);
  CHECK_EQ(a.token, std::string("legacy-access"));
  CHECK_EQ(a.token_expiry, kT0 + 600);
  CHECK_EQ(a.refresh_token, std::string("rt-old"));
  CHECK_EQ(a.refresh_token_expiry, uint64_t(kT0 + 100000));
}

TEST(a_failed_refresh_leaves_the_token_record_untouched) {
  struct Case {
    const char* name;
    HttpResponse response;
    RefreshOutcome expect;
  };
  HttpResponse notObject = Ok(nlohmann::json::array({1, 2}));
  HttpResponse garbage;
  garbage.transport_ok = true;
  garbage.status = 200;
  garbage.body = "<html>";
  const std::vector<Case> cases = {
      {"transport", NoTransport(), RefreshOutcome::TransportFailed},
      {"http500", Status(500), RefreshOutcome::Rejected},
      {"http401", Status(401), RefreshOutcome::Denied},
      {"garbage", garbage, RefreshOutcome::Malformed},
      {"array", notObject, RefreshOutcome::Malformed},
      {"no_token", Ok({{"refresh_token", "x"}}), RefreshOutcome::NoAccessToken},
  };
  for (const Case& c : cases) {
    FakeHttp http;
    const HttpResponse response = c.response;
    http.handler = [response](const std::string&, const std::string&) { return response; };
    CachedAuthToken a = CachedWithRefresh();
    a.token = "prev-access";
    a.token_expiry = kT0 + 5;
    const CachedAuthToken before = a;
    LogCapture log;
    const RefreshOutcome got = RefreshAccessToken(a, "https://h", "k", http, kT0, log.Sink());
    if (got != c.expect) std::fprintf(stderr, "  case %s: wrong outcome %s\n", c.name, RefreshOutcomeName(got));
    CHECK(got == c.expect);
    CHECK_EQ(a.token, before.token);
    CHECK_EQ(a.token_expiry, before.token_expiry);
    CHECK_EQ(a.refresh_token, before.refresh_token);
    CHECK_EQ(a.refresh_token_expiry, before.refresh_token_expiry);
    CHECK(!log.All().empty());  // every failure path logs
    CHECK(log.All().find("rt-old") == std::string::npos);
    CHECK(log.All().find("http_key") == std::string::npos);
  }
}

TEST(no_refresh_token_means_no_request) {
  FakeHttp http;
  http.handler = [](const std::string&, const std::string&) { return Ok({}); };
  CachedAuthToken a;
  CHECK(RefreshAccessToken(a, "https://h", "k", http, kT0, nullptr) == RefreshOutcome::NoRefreshToken);
  CHECK_EQ(http.Calls().size(), size_t(0));
}

TEST(refresh_is_due_inside_the_lead_window_and_not_before) {
  CHECK(!AccessTokenNeedsRefresh(kT0 + kRefreshLeadSec + 1, kT0));
  CHECK(AccessTokenNeedsRefresh(kT0 + kRefreshLeadSec, kT0));
  CHECK(AccessTokenNeedsRefresh(kT0 - 1, kT0));
  CHECK(AccessTokenNeedsRefresh(0, kT0));
}

// ---------------------------------------------------------------- device flow
struct FlowRig {
  uint64_t now_s = 0;  // fake steady clock, seconds
  std::vector<TokenAuth::DevicePollResponse> polls;
  size_t poll_index = 0;
  int sleeps = 0;
  bool cancel = false;
  intptr_t browser = 33;
  int ui = 1;
  std::string code = "SECRETCODE123";
  std::string opened_url;
  LogCapture log;
  DeviceFlowOps Ops() {
    DeviceFlowOps o;
    o.now = [this] { return std::chrono::steady_clock::time_point{} + seconds(now_s); };
    o.request_device_code = [this] { return code; };
    o.open_browser = [this](const std::string& u) {
      opened_url = u;
      return browser;
    };
    o.show_open_failure = [this](const std::string&, const std::string&, intptr_t) { return ui; };
    o.poll = [this](const std::string&) {
      TokenAuth::DevicePollResponse r = poll_index < polls.size() ? polls[poll_index] : TokenAuth::DevicePollResponse{};
      if (poll_index < polls.size()) ++poll_index;
      else r.status = TokenAuth::DevicePollStatus::Pending;
      return r;
    };
    o.sleep = [this](std::chrono::steady_clock::duration d) {
      now_s += static_cast<uint64_t>(std::chrono::duration_cast<seconds>(d).count());
      ++sleeps;
    };
    o.cancelled = [this] { return cancel; };
    o.log = log.Sink();
    return o;
  }
};

TokenAuth::DevicePollResponse Poll(TokenAuth::DevicePollStatus s) {
  TokenAuth::DevicePollResponse r;
  r.status = s;
  if (s == TokenAuth::DevicePollStatus::Verified) {
    r.access_token = "access";
    r.refresh_token = "refresh";
  }
  return r;
}

TEST(flow_returns_the_verified_response_after_pending_polls_and_masks_the_code) {
  FlowRig rig;
  rig.polls = {Poll(TokenAuth::DevicePollStatus::Pending), Poll(TokenAuth::DevicePollStatus::Pending),
               Poll(TokenAuth::DevicePollStatus::Verified)};
  const DeviceFlowResult r = RunDeviceCodeFlow(rig.Ops(), "https://x/login");
  CHECK(r.verified);
  CHECK_EQ(r.response.refresh_token, std::string("refresh"));
  CHECK_EQ(rig.sleeps, 3);
  CHECK_EQ(rig.opened_url, std::string("https://x/login?code=SECRETCODE123"));
  CHECK(rig.log.All().find("SECRETCODE123") == std::string::npos);
}

TEST(flow_gives_up_at_the_five_minute_deadline) {
  FlowRig rig;  // never verifies
  const DeviceFlowResult r = RunDeviceCodeFlow(rig.Ops(), "https://x/login");
  CHECK(!r.verified);
  CHECK_EQ(rig.now_s, uint64_t(300));
  CHECK(rig.log.All().find("timed out after 5 minutes") != std::string::npos);
}

TEST(flow_stops_on_server_expiry_error_cancel_and_undeliverable_link) {
  {
    FlowRig rig;
    rig.polls = {Poll(TokenAuth::DevicePollStatus::Expired)};
    CHECK(!RunDeviceCodeFlow(rig.Ops(), "u").verified);
    CHECK(rig.log.All().find("Device code expired") != std::string::npos);
  }
  {
    FlowRig rig;
    rig.polls = {Poll(TokenAuth::DevicePollStatus::Error)};
    CHECK(!RunDeviceCodeFlow(rig.Ops(), "u").verified);
    CHECK(rig.log.All().find("polling aborted") != std::string::npos);
  }
  {
    FlowRig rig;
    rig.cancel = true;
    CHECK(!RunDeviceCodeFlow(rig.Ops(), "u").verified);
    CHECK(rig.log.All().find("cancelled") != std::string::npos);
  }
  {
    FlowRig rig;
    rig.browser = 0;
    rig.ui = 0;
    CHECK(!RunDeviceCodeFlow(rig.Ops(), "u").verified);
    CHECK_EQ(rig.sleeps, 0);
    CHECK(rig.log.All().find("browser could not be opened") != std::string::npos);
  }
  {
    FlowRig rig;
    rig.code.clear();
    CHECK(!RunDeviceCodeFlow(rig.Ops(), "u").verified);
    CHECK(rig.log.All().find("device code request failed") != std::string::npos);
  }
}

// ---------------------------------------------------------------- session
SessionConfig TestConfig() {
  SessionConfig c;
  c.base_url = "https://nakama.test";
  c.http_key = "key";
  c.login_url = "https://login.test/device";
  return c;
}

void DeviceHandler(FakeHttp& http, int pending_polls, uint64_t access_exp) {
  auto polls = std::make_shared<std::atomic<int>>(0);
  http.handler = [=](const std::string& endpoint, const std::string&) -> HttpResponse {
    if (endpoint == "request") return Ok({{"code", "DEVCODE"}});
    if (endpoint == "poll") {
      if (polls->fetch_add(1) < pending_polls) return Ok({{"status", "pending"}});
      return Ok({{"status", "verified"}, {"access_token", MakeJwt(access_exp)}, {"refresh_token", "rt-dev"},
                 {"refresh_token_expires_in", 2592000}, {"user_id", "u9"}, {"username", "nine"}});
    }
    return Status(404);
  };
}

TEST(session_device_login_succeeds_publishes_the_token_and_persists_only_the_refresh_token) {
  FakeClock clock;
  FakeHttp http;
  FakeStore store;
  FakePresenter presenter;
  LogCapture log;
  DeviceHandler(http, 2, kT0 + 3600);
  Session s(TestConfig(), http, clock, store, presenter, log.Sink());
  s.Start();
  clock.Allow(3);  // three 3-second poll waits
  CHECK(WaitUntil([&] { return s.Get().readiness == Readiness::Ready; }));
  CHECK_EQ(s.Token(), MakeJwt(kT0 + 3600));
  CHECK_EQ(s.Get().discord_id, uint64_t(424242));
  CHECK_EQ(s.Get().username, std::string("nine"));
  CHECK_EQ(store.SaveCount(), size_t(1));
  CHECK_EQ(store.Last().refresh_token, std::string("rt-dev"));
  CHECK(SerializeCredentialsJson(store.Last()).find("eyJ") == std::string::npos);  // no JWT on disk
  CHECK_EQ(presenter.presented.load(), 1);
  CHECK_EQ(presenter.last_url, std::string("https://login.test/device?code=DEVCODE"));
  CHECK(presenter.cleared.load() >= 1);
  CHECK_EQ(http.Count("poll"), 3);
  const std::string all = log.All();
  CHECK(all.find("DEVCODE") == std::string::npos);
  CHECK(all.find("rt-dev") == std::string::npos);
  CHECK(all.find("eyJ") == std::string::npos);
  CHECK(all.find("http_key") == std::string::npos);
  s.Stop();
}

TEST(session_start_returns_while_the_network_call_is_still_blocked) {
  FakeClock clock;
  FakeHttp http;
  FakeStore store;
  FakePresenter presenter;
  std::mutex m;
  std::condition_variable cv;
  bool entered = false, release = false;
  http.handler = [&](const std::string&, const std::string&) -> HttpResponse {
    std::unique_lock<std::mutex> l(m);
    entered = true;
    cv.notify_all();
    cv.wait(l, [&] { return release; });
    return NoTransport();
  };
  Session s(TestConfig(), http, clock, store, presenter, nullptr);
  std::atomic<bool> start_returned{false};
  std::thread caller([&] {
    s.Start();
    start_returned = true;
  });
  {
    std::unique_lock<std::mutex> l(m);
    CHECK(cv.wait_for(l, std::chrono::seconds(5), [&] { return entered; }));
  }
  CHECK(WaitUntil([&] { return start_returned.load(); }));  // returned although HTTP is still blocked
  CHECK(s.Get().readiness != Readiness::Ready);
  {
    std::lock_guard<std::mutex> l(m);
    release = true;
  }
  cv.notify_all();
  caller.join();
  s.Stop();
}

TEST(session_cached_refresh_token_logs_in_without_the_device_flow) {
  FakeClock clock;
  FakeHttp http;
  FakeStore store;
  store.initial = CachedWithRefresh("rt-old", kT0 + 100000);
  FakePresenter presenter;
  http.handler = [](const std::string& endpoint, const std::string&) {
    return endpoint == "refresh" ? Ok(RefreshOkBody(kT0 + 3600, "rt-rotated")) : Status(404);
  };
  Session s(TestConfig(), http, clock, store, presenter, nullptr);
  s.Start();
  CHECK(WaitUntil([&] { return s.Get().readiness == Readiness::Ready; }));
  CHECK_EQ(http.Count("request"), 0);
  CHECK_EQ(presenter.presented.load(), 0);
  CHECK_EQ(store.SaveCount(), size_t(1));
  CHECK_EQ(store.Last().refresh_token, std::string("rt-rotated"));
  s.Stop();
}

TEST(session_a_flaky_first_refresh_is_retried_before_the_cache_is_given_up_on) {
  FakeClock clock;
  FakeHttp http;
  FakeStore store;
  store.initial = CachedWithRefresh();
  FakePresenter presenter;
  auto n = std::make_shared<std::atomic<int>>(0);
  http.handler = [n](const std::string& endpoint, const std::string&) {
    if (endpoint != "refresh") return Status(404);
    return n->fetch_add(1) == 0 ? NoTransport() : Ok(RefreshOkBody(kT0 + 3600));
  };
  Session s(TestConfig(), http, clock, store, presenter, nullptr);
  s.Start();
  clock.Allow(1);  // the pause between attempt 1 and 2
  CHECK(WaitUntil([&] { return s.Get().readiness == Readiness::Ready; }));
  CHECK_EQ(http.Count("refresh"), 2);
  CHECK_EQ(http.Count("request"), 0);
  s.Stop();
}

TEST(session_refresh_failure_keeps_the_cache_and_reports_failed_when_login_also_fails) {
  FakeClock clock;
  FakeHttp http;
  FakeStore store;
  store.initial = CachedWithRefresh();
  FakePresenter presenter;
  LogCapture log;
  http.handler = [](const std::string&, const std::string&) { return NoTransport(); };
  Session s(TestConfig(), http, clock, store, presenter, log.Sink());
  s.Start();
  clock.Allow(2);  // two pauses between the three refresh attempts
  CHECK(WaitUntil([&] { return s.Get().readiness == Readiness::Failed; }));
  CHECK_EQ(http.Count("refresh"), 3);
  CHECK_EQ(store.SaveCount(), size_t(0));  // nothing written: the cached login survives
  CHECK(s.Token().empty());
  CHECK(log.All().find("cache file kept") != std::string::npos);
  CHECK(log.All().find("Authentication failed") != std::string::npos);
  s.Stop();
}

TEST(session_token_stops_being_served_at_expiry_and_a_background_refresh_replaces_it) {
  FakeClock clock;
  FakeHttp http;
  FakeStore store;
  FakePresenter presenter;
  auto refreshes = std::make_shared<std::atomic<int>>(0);
  http.handler = [refreshes](const std::string& endpoint, const std::string&) -> HttpResponse {
    if (endpoint == "request") return Ok({{"code", "C"}});
    if (endpoint == "poll")
      return Ok({{"status", "verified"}, {"access_token", MakeJwt(kT0 + 400)}, {"refresh_token", "rt1"},
                 {"refresh_token_expires_in", 2592000}});
    if (endpoint == "refresh") {
      ++*refreshes;
      return Ok(RefreshOkBody(kT0 + 4000, "rt2"));
    }
    return Status(404);
  };
  SessionConfig cfg = TestConfig();
  Session s(cfg, http, clock, store, presenter, nullptr);
  s.Start();
  clock.Allow(1);  // one 3 s poll wait
  CHECK(WaitUntil([&] { return s.Get().readiness == Readiness::Ready; }));
  CHECK_EQ(s.Token(), MakeJwt(kT0 + 400));

  // Token (exp = T0+400) is 397 s from expiry at T0+3: outside the 300 s lead, so one
  // background period (60 s) does not refresh.
  clock.Allow(1);
  CHECK(WaitUntil([&] { return clock.Sleeps() >= 2; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK_EQ(refreshes->load(), 0);

  // After enough periods the token is inside the lead window and gets refreshed.
  clock.Allow(2);
  CHECK(WaitUntil([&] { return refreshes->load() == 1; }));
  CHECK(WaitUntil([&] { return store.SaveCount() == 2; }));
  CHECK_EQ(store.Last().refresh_token, std::string("rt2"));
  CHECK_EQ(s.Token(), MakeJwt(kT0 + 4000));
  s.Stop();
  CHECK(s.Token().empty());  // stopped: nothing served
}

TEST(session_serves_nothing_once_the_wall_clock_passes_the_expiry) {
  FakeClock clock;
  FakeHttp http;
  FakeStore store;
  FakePresenter presenter;
  DeviceHandler(http, 0, kT0 + 400);
  Session s(TestConfig(), http, clock, store, presenter, nullptr);
  s.Start();
  clock.Allow(1);
  CHECK(WaitUntil([&] { return s.Get().readiness == Readiness::Ready; }));
  CHECK(!s.Token().empty());
  clock.Advance(500);
  CHECK(s.Token().empty());
  s.Stop();
}

TEST(session_background_refresh_failure_keeps_the_token_in_memory_and_the_cache_on_disk) {
  FakeClock clock;
  FakeHttp http;
  FakeStore store;
  FakePresenter presenter;
  LogCapture log;
  http.handler = [](const std::string& endpoint, const std::string&) -> HttpResponse {
    if (endpoint == "request") return Ok({{"code", "C"}});
    if (endpoint == "poll")
      return Ok({{"status", "verified"}, {"access_token", MakeJwt(kT0 + 320)}, {"refresh_token", "rt1"},
                 {"refresh_token_expires_in", 2592000}});
    return Status(503);  // refresh endpoint down
  };
  Session s(TestConfig(), http, clock, store, presenter, log.Sink());
  s.Start();
  clock.Allow(1);
  CHECK(WaitUntil([&] { return s.Get().readiness == Readiness::Ready; }));
  const size_t savesAfterLogin = store.SaveCount();
  clock.Allow(1);  // token is inside the lead window: refresh is attempted and fails
  CHECK(WaitUntil([&] { return http.Count("refresh") >= 1; }));
  CHECK(WaitUntil([&] { return log.All().find("refresh failed (1 consecutive)") != std::string::npos; }));
  CHECK_EQ(store.SaveCount(), savesAfterLogin);
  CHECK_EQ(s.Token(), MakeJwt(kT0 + 320));
  s.Stop();
}

TEST(session_stop_interrupts_a_login_that_is_waiting_for_the_player) {
  FakeClock clock;
  FakeHttp http;
  FakeStore store;
  FakePresenter presenter;
  DeviceHandler(http, 1000000, kT0 + 3600);  // never verifies
  Session s(TestConfig(), http, clock, store, presenter, nullptr);
  s.Start();
  CHECK(WaitUntil([&] { return presenter.presented.load() == 1; }));
  s.Stop();  // must return: the sleep is interrupted
  CHECK(s.Get().readiness == Readiness::Stopped);
  CHECK_EQ(store.SaveCount(), size_t(0));
}

TEST(session_undeliverable_login_link_ends_the_login_without_waiting_out_the_code) {
  FakeClock clock;
  FakeHttp http;
  FakeStore store;
  FakePresenter presenter;
  presenter.deliver = false;
  DeviceHandler(http, 0, kT0 + 3600);
  Session s(TestConfig(), http, clock, store, presenter, nullptr);
  s.Start();
  CHECK(WaitUntil([&] { return s.Get().readiness == Readiness::Failed; }));
  CHECK_EQ(http.Count("poll"), 0);
  s.Stop();
}

// ---------------------------------------------------------------- file adapters
std::string TempDir() {
  // Under the build tree (gitignored), never /tmp.
  const std::string base = "build/quest-shared-host/scratch";
  std::error_code ec;
  std::filesystem::create_directories(base, ec);
  std::string tmpl = base + "/auth-XXXXXX";
  const char* d = ::mkdtemp(&tmpl[0]);
  return d == nullptr ? std::string() : std::string(d);
}

TEST(file_store_round_trips_with_mode_0600_and_creates_the_directory) {
  const std::string dir = TempDir();
  CHECK(!dir.empty());
  const std::string path = JoinPath(dir, "a/b/.credentials.json");
  FileCredentialStore store(path, nullptr);
  CHECK(store.Save(CachedWithRefresh("rt-file")));
  struct stat st {};
  CHECK_EQ(::stat(path.c_str(), &st), 0);
  CHECK_EQ(static_cast<int>(st.st_mode & 0777), 0600);
  const CachedAuthToken back = store.Load(kT0);
  CHECK_EQ(back.refresh_token, std::string("rt-file"));
  CHECK_EQ(back.username, std::string("name1"));
  CHECK(!std::filesystem::exists(path + ".tmp"));
  std::filesystem::remove_all(dir);
}

TEST(file_store_failed_write_leaves_the_previous_file_whole) {
  const std::string dir = TempDir();
  const std::string path = JoinPath(dir, ".credentials.json");
  LogCapture log;
  FileCredentialStore store(path, log.Sink());
  CHECK(store.Save(CachedWithRefresh("rt-keep")));
  // A directory squatting on the temp name makes open() fail.
  std::filesystem::create_directory(path + ".tmp");
  CHECK(!store.Save(CachedWithRefresh("rt-lost")));
  CHECK_EQ(store.Load(kT0).refresh_token, std::string("rt-keep"));
  CHECK(log.All().find("previous file unchanged") != std::string::npos);
  CHECK(log.All().find("rt-lost") == std::string::npos);
  std::filesystem::remove_all(dir);
}

TEST(file_store_missing_and_corrupt_files_read_as_empty) {
  const std::string dir = TempDir();
  const std::string path = JoinPath(dir, ".credentials.json");
  FileCredentialStore store(path, nullptr);
  CHECK(store.Load(kT0).refresh_token.empty());
  { std::ofstream(path) << "{{{"; }
  CHECK(store.Load(kT0).refresh_token.empty());
  CHECK(!store.Save(CachedAuthToken{}));  // no refresh token: nothing to persist
  std::filesystem::remove_all(dir);
}

TEST(link_presenter_writes_the_url_privately_and_clears_it) {
  const std::string dir = TempDir();
  const std::string path = JoinPath(dir, "device_login.txt");
  LogCapture log;
  FileLinkPresenter p(path, log.Sink());
  CHECK(p.Present("https://login.test/device?code=ABC") > kBrowserOpenAcceptedAbove);
  struct stat st {};
  CHECK_EQ(::stat(path.c_str(), &st), 0);
  CHECK_EQ(static_cast<int>(st.st_mode & 0777), 0600);
  std::ifstream in(path);
  std::string line;
  std::getline(in, line);
  CHECK_EQ(line, std::string("https://login.test/device?code=ABC"));
  CHECK(log.All().find("ABC") == std::string::npos);  // path in the log, never the code
  p.Clear();
  CHECK(!std::filesystem::exists(path));
  p.Clear();  // idempotent
  std::filesystem::remove_all(dir);
}

TEST(link_presenter_reports_failure_when_it_cannot_write) {
  FileLinkPresenter p("/proc/nevr-no-such-dir/device_login.txt", nullptr);
  CHECK_EQ(p.Present("u"), intptr_t(0));
}


// ---------------------------------------------------------------- refresh classification
TEST(refresh_denied_means_the_server_refuses_the_token_other_statuses_are_retryable) {
  for (const long status : {400L, 401L, 403L}) {
    CachedAuthToken a = CachedWithRefresh();
    HttpResponse r = Status(status);
    CHECK(ApplyRefreshResponse(a, r, kT0, nullptr) == RefreshOutcome::Denied);
  }
  for (const long status : {429L, 500L, 502L, 503L}) {
    CachedAuthToken a = CachedWithRefresh();
    HttpResponse r = Status(status);
    CHECK(ApplyRefreshResponse(a, r, kT0, nullptr) == RefreshOutcome::Rejected);
  }
}

TEST(refresh_log_wording_tells_bad_json_from_json_of_the_wrong_shape) {
  const auto outcome_log = [](const std::string& body) {
    CachedAuthToken a = CachedWithRefresh();
    HttpResponse r;
    r.transport_ok = true;
    r.status = 200;
    r.body = body;
    LogCapture log;
    CHECK(ApplyRefreshResponse(a, r, kT0, log.Sink()) == RefreshOutcome::Malformed);
    return log.All();
  };
  CHECK(outcome_log("<html>").find("was not valid JSON") != std::string::npos);
  CHECK(outcome_log("[]").find("unexpected shape") != std::string::npos);
  CHECK(outcome_log("{\"access_token\":1}").find("unexpected shape") != std::string::npos);
  CHECK(outcome_log("[]").find("not valid JSON") == std::string::npos);
}

// ---------------------------------------------------------------- CA bundle
std::string ScratchDir(const std::string& name) {
  const std::string dir = "build/quest-shared-host/scratch/" + name;
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir;
}
void WriteFile(const std::string& path, const std::string& data) {
  std::ofstream(path, std::ios::binary) << data;
}
const char kFakePem[] = "-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\nCertificate:\n  text dump\n";

TEST(der_is_wrapped_as_a_pem_certificate_block) {
  CHECK_EQ(DerToPem(std::string("\x30\x03\x02\x01\x05", 5)),
           std::string("-----BEGIN CERTIFICATE-----\nMAMCAQU=\n-----END CERTIFICATE-----\n"));
}

TEST(ca_bundle_takes_pem_and_der_and_skips_everything_else) {
  const std::string dir = ScratchDir("ca-mixed");
  WriteFile(dir + "/aaaa1111.0", kFakePem);
  WriteFile(dir + "/bbbb2222.0", std::string("\x30\x03\x02\x01\x05", 5));
  WriteFile(dir + "/readme.txt", "not a certificate");
  WriteFile(dir + "/empty.0", "");
  LogCapture log;
  const CaBundle b = LoadCaBundle({dir}, log.Sink());
  CHECK_EQ(b.certificates, size_t(2));
  CHECK(b.pem.find("AAAA") != std::string::npos);
  CHECK(b.pem.find("MAMCAQU=") != std::string::npos);
  CHECK(b.pem.find("not a certificate") == std::string::npos);
  CHECK(log.All().find("certificates=2 skipped_files=2") != std::string::npos);
}

TEST(ca_bundle_prefers_the_first_directory_that_has_certificates_and_fails_loudly_with_none) {
  const std::string apex = ScratchDir("ca-apex");
  const std::string sys = ScratchDir("ca-system");
  WriteFile(sys + "/a.0", kFakePem);
  WriteFile(sys + "/b.0", kFakePem);
  {
    LogCapture log;
    CHECK_EQ(LoadCaBundle({apex, sys}, log.Sink()).certificates, size_t(2));  // apex empty: fall through
    CHECK(log.All().find("held no certificates") != std::string::npos);
  }
  WriteFile(apex + "/c.0", kFakePem);
  CHECK_EQ(LoadCaBundle({apex, sys}, nullptr).certificates, size_t(1));  // apex wins, not merged
  {
    LogCapture log;
    CHECK_EQ(LoadCaBundle({"build/quest-shared-host/scratch/nope"}, log.Sink()).certificates, size_t(0));
    CHECK(log.All().find("fail closed") != std::string::npos);
  }
}

// ---------------------------------------------------------------- credential store location and hygiene
TEST(the_private_directory_comes_from_the_process_package_name_or_not_at_all) {
  const std::string cmd("com.readyatdawn.r15\0\0", 21);
  CHECK_EQ(AppInternalFilesDirFromCmdline(cmd), std::string("/data/data/com.readyatdawn.r15/files"));
  CHECK_EQ(AppInternalFilesDirFromCmdline("com.readyatdawn.r15"), std::string("/data/data/com.readyatdawn.r15/files"));
  for (const char* bad : {"", "nodot", "com.x:service", "../x.y", "a..b", ".a.b", "a.b.", "com.x y", "/system/bin/app_process"}) {
    CHECK_EQ(AppInternalFilesDirFromCmdline(bad), std::string());
  }
}

TEST(a_store_without_a_private_directory_neither_loads_nor_saves_and_says_so) {
  LogCapture log;
  FileCredentialStore store("", log.Sink());
  CHECK(store.Load(kT0).refresh_token.empty());
  CHECK(!store.Save(CachedWithRefresh("rt-nowhere")));
  CHECK(log.All().find("no app-internal directory") != std::string::npos);
  CHECK(log.All().find("rt-nowhere") == std::string::npos);
}

TEST(the_store_never_follows_a_symlink_on_load_or_save) {
  const std::string dir = ScratchDir("store-symlink");
  const std::string path = dir + "/.credentials.json";
  const std::string real = dir + "/real.json";
  WriteFile(real, SerializeCredentialsJson(CachedWithRefresh("rt-behind-link")));
  CHECK(::symlink("real.json", path.c_str()) == 0);
  LogCapture log;
  FileCredentialStore store(path, log.Sink());
  CHECK(store.Load(kT0).refresh_token.empty());  // O_NOFOLLOW: the link is refused
  CHECK(log.All().find("unreadable") != std::string::npos);

  // A symlink squatting on the temp name must not redirect the write.
  const std::string victim = dir + "/victim.txt";
  WriteFile(victim, "untouched");
  std::filesystem::remove(path);
  CHECK(::symlink("victim.txt", (path + ".tmp").c_str()) == 0);
  CHECK(store.Save(CachedWithRefresh("rt-saved")));
  std::ifstream in(victim);
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK_EQ(text, std::string("untouched"));
  CHECK_EQ(store.Load(kT0).refresh_token, std::string("rt-saved"));
}

TEST(a_stale_temp_file_from_a_crashed_run_does_not_block_a_save) {
  const std::string dir = ScratchDir("store-stale");
  const std::string path = dir + "/.credentials.json";
  WriteFile(path + ".tmp", "half written garbage");
  FileCredentialStore store(path, nullptr);
  CHECK(store.Save(CachedWithRefresh("rt-fresh")));
  CHECK_EQ(store.Load(kT0).refresh_token, std::string("rt-fresh"));
  CHECK(!std::filesystem::exists(path + ".tmp"));
}

// ---------------------------------------------------------------- session: poll failures
TEST(session_one_failed_poll_request_does_not_end_the_login) {
  FakeClock clock;
  FakeHttp http;
  FakeStore store;
  FakePresenter presenter;
  auto polls = std::make_shared<std::atomic<int>>(0);
  http.handler = [polls](const std::string& endpoint, const std::string&) -> HttpResponse {
    if (endpoint == "request") return Ok({{"code", "C"}});
    if (polls->fetch_add(1) == 0) return NoTransport();
    return Ok({{"status", "verified"}, {"access_token", MakeJwt(kT0 + 3600)}, {"refresh_token", "rt"}});
  };
  Session s(TestConfig(), http, clock, store, presenter, nullptr);
  s.Start();
  clock.Allow(2);
  CHECK(WaitUntil([&] { return s.Get().readiness == Readiness::Ready; }));
  CHECK_EQ(http.Count("poll"), 2);
  s.Stop();
}

TEST(session_a_permanent_poll_error_gives_up_after_the_failure_limit_and_logs_the_status) {
  FakeClock clock;
  FakeHttp http;
  FakeStore store;
  FakePresenter presenter;
  LogCapture log;
  http.handler = [](const std::string& endpoint, const std::string&) -> HttpResponse {
    return endpoint == "request" ? Ok({{"code", "C"}}) : Status(400);
  };
  Session s(TestConfig(), http, clock, store, presenter, log.Sink());
  s.Start();
  clock.Allow(100);
  CHECK(WaitUntil([&] { return s.Get().readiness == Readiness::Failed; }));
  CHECK_EQ(http.Count("poll"), 5);  // the default limit, not the five-minute deadline (100 polls)
  CHECK(log.All().find("5/5 consecutive") != std::string::npos);
  CHECK(log.All().find("http_status=400") != std::string::npos);
  CHECK_EQ(store.SaveCount(), size_t(0));
  s.Stop();
}

// ---------------------------------------------------------------- session: credentials that die
TEST(session_an_expired_refresh_token_publishes_expired_and_starts_a_new_device_login) {
  FakeClock clock;
  FakeHttp http;
  FakeStore store;
  FakePresenter presenter;
  LogCapture log;
  auto logins = std::make_shared<std::atomic<int>>(0);
  http.handler = [logins](const std::string& endpoint, const std::string&) -> HttpResponse {
    if (endpoint == "request") return Ok({{"code", "C"}});
    if (endpoint == "poll") {
      const int n = ++*logins;
      return Ok({{"status", "verified"},
                 {"access_token", MakeJwt(n == 1 ? kT0 + 400 : kT0 + 9000)},
                 {"refresh_token", n == 1 ? "rt-a" : "rt-b"},
                 {"refresh_token_expires_in", n == 1 ? 2000 : 2592000}});
    }
    return Status(500);
  };
  Session s(TestConfig(), http, clock, store, presenter, log.Sink());
  s.Start();
  clock.Allow(1);
  CHECK(WaitUntil([&] { return s.Get().readiness == Readiness::Ready; }));
  clock.Advance(2500);  // access token and refresh token are both dead now
  clock.Allow(2);       // one background period, then the poll wait of the second login
  CHECK(WaitUntil([&] { return presenter.presented.load() == 2; }));
  CHECK(WaitUntil([&] { return s.Token() == MakeJwt(kT0 + 9000); }));
  CHECK_EQ(http.Count("refresh"), 0);  // an expired refresh token is not sent
  CHECK(log.All().find("auth state ready -> expired") != std::string::npos);
  CHECK(log.All().find("the refresh token has expired") != std::string::npos);
  CHECK_EQ(store.Last().refresh_token, std::string("rt-b"));
  s.Stop();
}

TEST(session_a_refresh_the_server_denies_starts_a_new_device_login_instead_of_retrying_forever) {
  FakeClock clock;
  FakeHttp http;
  FakeStore store;
  FakePresenter presenter;
  LogCapture log;
  auto logins = std::make_shared<std::atomic<int>>(0);
  http.handler = [logins](const std::string& endpoint, const std::string&) -> HttpResponse {
    if (endpoint == "request") return Ok({{"code", "C"}});
    if (endpoint == "poll") {
      const int n = ++*logins;
      return Ok({{"status", "verified"},
                 {"access_token", MakeJwt(n == 1 ? kT0 + 320 : kT0 + 9000)},
                 {"refresh_token", n == 1 ? "rt-a" : "rt-b"},
                 {"refresh_token_expires_in", 2592000}});
    }
    return Status(401);
  };
  Session s(TestConfig(), http, clock, store, presenter, log.Sink());
  s.Start();
  clock.Allow(1);
  CHECK(WaitUntil([&] { return s.Get().readiness == Readiness::Ready; }));
  clock.Allow(2);  // background period (token is inside the lead window), then the second poll wait
  CHECK(WaitUntil([&] { return s.Token() == MakeJwt(kT0 + 9000); }));
  CHECK_EQ(http.Count("refresh"), 1);
  CHECK_EQ(presenter.presented.load(), 2);
  CHECK(log.All().find("the server refuses the refresh token") != std::string::npos);
  s.Stop();
}

TEST(session_publishes_expired_while_refresh_keeps_failing_and_serves_no_token) {
  FakeClock clock;
  FakeHttp http;
  FakeStore store;
  FakePresenter presenter;
  http.handler = [](const std::string& endpoint, const std::string&) -> HttpResponse {
    if (endpoint == "request") return Ok({{"code", "C"}});
    if (endpoint == "poll")
      return Ok({{"status", "verified"}, {"access_token", MakeJwt(kT0 + 320)}, {"refresh_token", "rt1"},
                 {"refresh_token_expires_in", 2592000}});
    return Status(503);
  };
  Session s(TestConfig(), http, clock, store, presenter, nullptr);
  s.Start();
  clock.Allow(1);
  CHECK(WaitUntil([&] { return s.Get().readiness == Readiness::Ready; }));
  clock.Advance(400);
  clock.Allow(1);
  CHECK(WaitUntil([&] { return s.Get().readiness == Readiness::Expired; }));
  CHECK(s.Token().empty());
  CHECK_EQ(store.SaveCount(), size_t(1));  // the cache from the login, untouched by the failures
  s.Stop();
}

// ---------------------------------------------------------------- session lifetime
TEST(session_concurrent_stops_both_return_and_leave_it_stopped) {
  FakeClock clock;
  FakeHttp http;
  FakeStore store;
  FakePresenter presenter;
  DeviceHandler(http, 1000000, kT0 + 3600);
  Session s(TestConfig(), http, clock, store, presenter, nullptr);
  s.Start();
  CHECK(WaitUntil([&] { return presenter.presented.load() == 1; }));
  std::thread a([&] { s.Stop(); });
  std::thread b([&] { s.Stop(); });
  a.join();
  b.join();
  CHECK(s.Get().readiness == Readiness::Stopped);
  CHECK(http.interrupts.load() >= 1);
}

TEST(session_log_sink_may_call_back_into_the_session) {
  FakeClock clock;
  FakeHttp http;
  FakeStore store;
  FakePresenter presenter;
  DeviceHandler(http, 0, kT0 + 3600);
  Session* self = nullptr;
  std::atomic<int> sink_calls{0};
  const LogSink sink = [&](LogLevel, const std::string&) {
    if (self != nullptr) (void)self->Get();  // would deadlock if a lock were held across the sink
    ++sink_calls;
  };
  Session s(TestConfig(), http, clock, store, presenter, sink);
  self = &s;
  s.Start();
  clock.Allow(1);
  CHECK(WaitUntil([&] { return s.Get().readiness == Readiness::Ready; }));
  CHECK(sink_calls.load() > 0);
  s.Stop();
}

TEST(session_stop_does_not_wait_for_a_blocked_request) {
  FakeClock clock;
  FakeHttp http;
  FakeStore store;
  FakePresenter presenter;
  std::mutex m;
  std::condition_variable cv;
  bool entered = false;
  http.handler = [&](const std::string&, const std::string&) -> HttpResponse {
    std::unique_lock<std::mutex> l(m);
    entered = true;
    cv.notify_all();
    cv.wait(l, [&] { return http.interrupts.load() > 0; });  // a real client returns when interrupted
    return NoTransport(-2);
  };
  Session s(TestConfig(), http, clock, store, presenter, nullptr);
  s.Start();
  {
    std::unique_lock<std::mutex> l(m);
    CHECK(cv.wait_for(l, std::chrono::seconds(5), [&] { return entered; }));
  }
  std::thread stopper([&] { s.Stop(); });
  // Interrupt() is called without the handler's mutex; wake the handler's wait.
  CHECK(WaitUntil([&] { return http.interrupts.load() > 0; }));
  { std::lock_guard<std::mutex> l(m); }  // order the notify after the handler is waiting or has seen the flag
  cv.notify_all();
  stopper.join();
  CHECK(s.Get().readiness == Readiness::Stopped);
}

}  // namespace

int main(int argc, char** argv) { return mini_test::RunAll(argc, argv); }
