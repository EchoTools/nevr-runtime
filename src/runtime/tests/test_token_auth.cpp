#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "core/auth_token.h"
#include "core/auth_refresh.h"
#include "core/bounded_retry.h"
#include "auth_snapshot.h"
#include "device_poll_response.h"
#include "extension/module_interface.h"
#include "off_thread_wait.h"
#include "token_auth.h"

extern "C" int token_auth_Init(const NvrModuleContext* ctx);
extern "C" void token_auth_Shutdown(void);
extern "C" uint32_t token_auth_ApiVersion(void);

namespace {

constexpr char kJwtHeader[] = "eyJhbGciOiJub25lIn0";
constexpr char kJwtSignature[] = "signature";

std::string MakeJwt(const std::string& payload) {
  return std::string(kJwtHeader) + "." + payload + "." + kJwtSignature;
}

NvrModuleContext MakeModuleContext(uint32_t flags) {
  NvrModuleContext context{};
  context.flags = flags;
  return context;
}

class ExecutableCredentialCacheFixture {
 public:
  explicit ExecutableCredentialCacheFixture(const nlohmann::json& credentials)
      : mutex_(CreateMutexA(nullptr, FALSE, "Local\\nevr-runtime-test-token-auth-cache")) {
    if (mutex_ == nullptr) {
      throw std::runtime_error("CreateMutexA failed for credential cache fixture");
    }
    const DWORD wait = WaitForSingleObject(mutex_, INFINITE);
    if (wait != WAIT_OBJECT_0) {
      CloseHandle(mutex_);
      mutex_ = nullptr;
      throw std::runtime_error("credential cache fixture mutex was not acquired");
    }
    mutex_acquired_ = true;

    cache_directory_ = GetExeDirectory() + "_local";
    cache_path_ = cache_directory_ + "/.credentials.json";
    const DWORD attributes = GetFileAttributesA(cache_directory_.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      if (!CreateDirectoryA(cache_directory_.c_str(), nullptr)) {
        ReleaseMutex(mutex_);
        CloseHandle(mutex_);
        mutex_ = nullptr;
        mutex_acquired_ = false;
        throw std::runtime_error("CreateDirectoryA failed for credential cache fixture");
      }
      created_directory_ = true;
    } else if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
      ReleaseMutex(mutex_);
      CloseHandle(mutex_);
      mutex_ = nullptr;
      mutex_acquired_ = false;
      throw std::runtime_error("credential cache fixture path is not a directory");
    }

    std::ifstream existing(cache_path_, std::ios::binary);
    if (existing.is_open()) {
      original_contents_.assign(std::istreambuf_iterator<char>(existing),
                                std::istreambuf_iterator<char>());
      had_original_file_ = true;
    }

    std::ofstream fixture(cache_path_, std::ios::binary | std::ios::trunc);
    if (!fixture.is_open()) {
      Restore();
      throw std::runtime_error("failed to write credential cache fixture");
    }
    fixture << credentials.dump();
    fixture.close();
  }

  ExecutableCredentialCacheFixture(const ExecutableCredentialCacheFixture&) = delete;
  ExecutableCredentialCacheFixture& operator=(const ExecutableCredentialCacheFixture&) = delete;

  ~ExecutableCredentialCacheFixture() { Restore(); }

 private:
  void Restore() noexcept {
    if (mutex_ == nullptr) {
      return;
    }
    if (had_original_file_) {
      std::ofstream original(cache_path_, std::ios::binary | std::ios::trunc);
      if (original.is_open()) {
        original.write(original_contents_.data(), static_cast<std::streamsize>(original_contents_.size()));
      }
    } else {
      DeleteFileA(cache_path_.c_str());
    }
    if (created_directory_) {
      RemoveDirectoryA(cache_directory_.c_str());
    }
    if (mutex_acquired_) {
      ReleaseMutex(mutex_);
    }
    CloseHandle(mutex_);
    mutex_ = nullptr;
    mutex_acquired_ = false;
  }

  HANDLE mutex_ = nullptr;
  bool mutex_acquired_ = false;
  bool created_directory_ = false;
  bool had_original_file_ = false;
  std::string cache_directory_;
  std::string cache_path_;
  std::string original_contents_;
};

}  // namespace

TEST(CachedAuthTokenClaims, ValidJwtDecodesTopLevelClaims) {
  CachedAuthToken token;
  token.token = MakeJwt("eyJkaWQiOiIxMjMiLCJleHAiOjQxMDI0NDQ4MDB9");

  const nlohmann::json claims = token.DecodeClaims();
  EXPECT_EQ(claims.at("did"), "123");
  EXPECT_EQ(token.GetDiscordId(), 123U);
  EXPECT_EQ(token.GetJwtExpiry(), 4102444800ULL);
}

TEST(CachedAuthTokenClaims, VarsDiscordIdTakesPrecedence) {
  CachedAuthToken token;
  token.token = MakeJwt("eyJ2cnMiOnsiZGlkIjoiNDIifSwiZXhwIjo0MTAyNDQ0ODAwfQ");

  EXPECT_EQ(token.GetDiscordId(), 42U);
}

TEST(CachedAuthTokenClaims, InvalidPayloadReturnsEmptyClaims) {
  CachedAuthToken token;
  token.token = MakeJwt("%%%%");

  EXPECT_TRUE(token.DecodeClaims().empty());
  EXPECT_EQ(token.GetDiscordId(), 0U);
  EXPECT_EQ(token.GetJwtExpiry(), 0U);
}

TEST(CachedAuthTokenClaims, MissingOptionalClaimsHaveSafeDefaults) {
  CachedAuthToken token;
  token.token = MakeJwt("e30");

  EXPECT_EQ(token.GetDiscordId(), 0U);
  EXPECT_EQ(token.GetJwtExpiry(), 0U);
}

TEST(CachedAuthTokenExpiry, ExpiredAndFutureTokensAreDistinguished) {
  CachedAuthToken expired;
  expired.token = "expired";
  expired.token_expiry = static_cast<uint64_t>(std::time(nullptr)) - 1;
  EXPECT_FALSE(expired.HasValidToken());

  CachedAuthToken future;
  future.token = "future";
  future.token_expiry = static_cast<uint64_t>(std::time(nullptr)) + 120;
  EXPECT_TRUE(future.HasValidToken());
}

TEST(DeviceAuthState, InitialStateIsUnauthenticated) {
  const TokenAuth::TestHook::DeviceAuthState state =
      TokenAuth::TestHook::InspectInitialDeviceAuth();

  EXPECT_FALSE(state.authenticated);
  EXPECT_TRUE(state.token.empty());
  EXPECT_EQ(state.discord_id, 0U);
  EXPECT_TRUE(state.username.empty());
}

TEST(DeviceAuthState, RefreshUpdateMakesValidTokenObservable) {
  CachedAuthToken refreshed;
  refreshed.token = MakeJwt("eyJ2cnMiOnsiZGlkIjoiNzc3In19");
  refreshed.token_expiry = static_cast<uint64_t>(std::time(nullptr)) + 3600;
  refreshed.refresh_token = "refresh-token";
  refreshed.refresh_token_expiry = static_cast<uint64_t>(std::time(nullptr)) + 7200;
  refreshed.user_id = "user-id";
  refreshed.username = "refreshed-player";

  const TokenAuth::TestHook::DeviceAuthState state =
      TokenAuth::TestHook::InspectDeviceAuthAfterRefresh(refreshed);

  EXPECT_TRUE(state.authenticated);
  EXPECT_EQ(state.token, refreshed.token);
  EXPECT_EQ(state.discord_id, 777U);
  EXPECT_EQ(state.username, "refreshed-player");
}

TEST(DeviceAuthState, ExpiredRefreshUpdateRemainsUnauthenticated) {
  CachedAuthToken refreshed;
  refreshed.token = MakeJwt("eyJkaWQiOiI4ODgifQ");
  refreshed.token_expiry = static_cast<uint64_t>(std::time(nullptr)) - 1;
  refreshed.username = "expired-player";

  const TokenAuth::TestHook::DeviceAuthState state =
      TokenAuth::TestHook::InspectDeviceAuthAfterRefresh(refreshed);

  EXPECT_FALSE(state.authenticated);
  EXPECT_EQ(state.token, refreshed.token);
  EXPECT_EQ(state.discord_id, 888U);
  EXPECT_EQ(state.username, "expired-player");
}

TEST(DeviceAuthState, SafelyRejectsLegacyAccessTokenFromExecutableLocalCache) {
  const uint64_t before = static_cast<uint64_t>(std::time(nullptr));
  nlohmann::json credentials;
  credentials["token"] = MakeJwt("eyJ2cnMiOnsiZGlkIjoiNDI0MiJ9fQ");
  credentials["token_expiry"] = before + 3600;
  credentials["username"] = "cached-player";
  const ExecutableCredentialCacheFixture cache(credentials);

  const CachedAuthToken loaded = LoadCachedAuthToken();
  const uint64_t after_load = static_cast<uint64_t>(std::time(nullptr));
  EXPECT_EQ(loaded.token, credentials.at("token").get<std::string>());
  EXPECT_GT(loaded.token_expiry, before);
  EXPECT_LE(loaded.token_expiry, after_load + kMaxDiskAccessTokenLifetimeSec);
  // The disk cap deliberately keeps a legacy bearer token below the normal
  // 60-second safety window, so DeviceAuth cannot adopt it without a refresh.
  EXPECT_FALSE(loaded.HasValidToken());

  const TokenAuth::TestHook::DeviceAuthState state =
      TokenAuth::TestHook::InspectDeviceAuthFromCache();

  EXPECT_FALSE(state.authenticated);
  EXPECT_TRUE(state.token.empty());
  EXPECT_EQ(state.discord_id, 0U);
  EXPECT_TRUE(state.username.empty());
}

// The background refresh thread's guard must consult the LIVE access-token
// expiry, not the credential cache.
//
// SaveAuthToken (core/auth_token.h) deliberately writes only refresh_token,
// refresh_token_expiry, user_id and username — the access token and its expiry
// are never persisted. So a guard reading token_expiry out of the cache reads 0
// in every fresh process, concludes "already expired", and issues an HTTP
// refresh on every 60-second wake for the whole life of a token nakama issued
// with a one-hour lifetime (evr_device_auth.go:289).
//
// The two sources are put in deliberate disagreement: disk is in the exact
// shape SaveAuthToken produces, memory holds a token good for another hour.
// Only a guard that reads memory can answer "not yet" — a test where both
// agree could not tell the two implementations apart.
TEST(RefreshThreadGuard, LiveTokenExpiryDecidesNotTheCredentialCache) {
  const uint64_t now = static_cast<uint64_t>(std::time(nullptr));

  nlohmann::json credentials;
  credentials["refresh_token"] = "refresh-token";
  credentials["refresh_token_expiry"] = now + 30 * 24 * 3600;
  credentials["user_id"] = "user-id";
  credentials["username"] = "cached-player";
  const ExecutableCredentialCacheFixture cache(credentials);

  // Precondition, asserted rather than assumed: nothing on disk states an
  // access-token expiry, so the cache cannot answer this question at all.
  const CachedAuthToken loaded = LoadCachedAuthToken();
  ASSERT_TRUE(loaded.token.empty());
  ASSERT_EQ(loaded.token_expiry, 0U);
  ASSERT_TRUE(loaded.HasValidRefreshToken());

  CachedAuthToken live;
  live.token = MakeJwt("eyJ2cnMiOnsiZGlkIjoiNzc3In19");
  live.token_expiry = now + 3600;
  live.refresh_token = "refresh-token";
  live.refresh_token_expiry = now + 30 * 24 * 3600;

  EXPECT_FALSE(TokenAuth::TestHook::InspectRefreshDecision(live, now));
}

// Pins the lead time itself. 300s against a 3600s token and a 60s wake interval
// is roughly five refresh attempts before the token dies; the boundary is
// asserted so a change to the constant cannot pass silently.
TEST(RefreshThreadGuard, RefreshesInsideTheLeadWindowAndNotOutsideIt) {
  const uint64_t now = static_cast<uint64_t>(std::time(nullptr));

  nlohmann::json credentials;
  credentials["refresh_token"] = "refresh-token";
  credentials["refresh_token_expiry"] = now + 30 * 24 * 3600;
  const ExecutableCredentialCacheFixture cache(credentials);

  CachedAuthToken live;
  live.token = MakeJwt("eyJ2cnMiOnsiZGlkIjoiNzc3In19");
  live.refresh_token = "refresh-token";
  live.refresh_token_expiry = now + 30 * 24 * 3600;

  live.token_expiry = now + 301;
  EXPECT_FALSE(TokenAuth::TestHook::InspectRefreshDecision(live, now));

  live.token_expiry = now + 300;
  EXPECT_TRUE(TokenAuth::TestHook::InspectRefreshDecision(live, now));

  live.token_expiry = now + 60;
  EXPECT_TRUE(TokenAuth::TestHook::InspectRefreshDecision(live, now));

  live.token_expiry = now - 1;
  EXPECT_TRUE(TokenAuth::TestHook::InspectRefreshDecision(live, now));

  // No live token at all: refresh, do not sit on an empty session.
  live.token_expiry = 0;
  EXPECT_TRUE(TokenAuth::TestHook::InspectRefreshDecision(live, now));
}

TEST(DevicePollResponse, VerifiedResponseExtractsEveryTokenField) {
  const TokenAuth::DevicePollResponse response = TokenAuth::ParseDevicePollResponse(
      "{\"status\":\"verified\",\"token\":\"token\",\"refresh_token\":\"refresh\","
      "\"user_id\":\"user\",\"username\":\"name\",\"expires_in\":3600}");

  EXPECT_EQ(response.status, TokenAuth::DevicePollStatus::Verified);
  EXPECT_EQ(response.access_token, "token");
  EXPECT_EQ(response.refresh_token, "refresh");
  EXPECT_EQ(response.user_id, "user");
  EXPECT_EQ(response.username, "name");
  ASSERT_TRUE(response.expires_in.has_value());
  EXPECT_EQ(*response.expires_in, 3600U);
}

TEST(DevicePollResponse, ZeroExpiresInUsesFutureJwtExpiry) {
  constexpr uint64_t kNow = 1000;
  constexpr uint64_t kJwtExpiry = 5000;
  const std::string accessToken = MakeJwt("eyJleHAiOjUwMDB9");
  const TokenAuth::DevicePollResponse response = TokenAuth::ParseDevicePollResponse(
      "{\"status\":\"verified\",\"token\":\"" + accessToken + "\",\"expires_in\":0}");

  ASSERT_EQ(response.status, TokenAuth::DevicePollStatus::Verified);
  ASSERT_TRUE(response.expires_in.has_value());
  EXPECT_EQ(*response.expires_in, 0U);
  EXPECT_EQ(TokenAuth::ResolveAccessTokenExpiry(kNow, response.access_token, response.expires_in),
            kJwtExpiry);
}

TEST(DevicePollResponse, PendingExpiredAndErrorResponsesRemainDistinct) {
  EXPECT_EQ(TokenAuth::ParseDevicePollResponse("{\"status\":\"authorization_pending\"}").status,
            TokenAuth::DevicePollStatus::Pending);
  EXPECT_EQ(TokenAuth::ParseDevicePollResponse("{\"status\":\"expired\"}").status,
            TokenAuth::DevicePollStatus::Expired);
  EXPECT_EQ(TokenAuth::ParseDevicePollResponse("{\"error\":\"access_denied\"}").status,
            TokenAuth::DevicePollStatus::Error);
  EXPECT_EQ(TokenAuth::ParseDevicePollResponse("not json").status,
            TokenAuth::DevicePollStatus::Error);
}

TEST(DevicePollResponse, WrongTypedVerifiedFieldsRemainErrorAndExpiryTypesStayAbsent) {
  for (const std::string field : {"access_token", "refresh_token", "user_id", "username"}) {
    const std::string body = "{\"status\":\"verified\",\"access_token\":\"access\","
                             "\"refresh_token\":\"refresh\",\"user_id\":\"user\","
                             "\"username\":\"name\",\"" + field + "\":7}";
    const TokenAuth::DevicePollResponse response = TokenAuth::ParseDevicePollResponse(body);
    EXPECT_EQ(response.status, TokenAuth::DevicePollStatus::Error) << field;
    EXPECT_TRUE(response.access_token.empty()) << field;
    EXPECT_TRUE(response.refresh_token.empty()) << field;
    EXPECT_TRUE(response.user_id.empty()) << field;
    EXPECT_TRUE(response.username.empty()) << field;
  }

  const TokenAuth::DevicePollResponse malformedLegacyToken = TokenAuth::ParseDevicePollResponse(
      "{\"status\":\"verified\",\"token\":7,\"refresh_token\":\"refresh\","
      "\"user_id\":\"user\",\"username\":\"name\"}");
  EXPECT_EQ(malformedLegacyToken.status, TokenAuth::DevicePollStatus::Error);
  EXPECT_TRUE(malformedLegacyToken.access_token.empty());
  EXPECT_TRUE(malformedLegacyToken.refresh_token.empty());

  const TokenAuth::DevicePollResponse wrongExpiry = TokenAuth::ParseDevicePollResponse(
      "{\"status\":\"verified\",\"access_token\":\"access\",\"expires_in\":\"3600\","
      "\"refresh_token_expires_in\":false}");
  EXPECT_EQ(wrongExpiry.status, TokenAuth::DevicePollStatus::Verified);
  EXPECT_FALSE(wrongExpiry.expires_in.has_value());
  EXPECT_FALSE(wrongExpiry.refresh_token_expires_in.has_value());
}

// The poll response carries the RFC 6749 §5.1 field `access_token` and the deprecated `token`.
// The server sends access_token and token with the same value, so a test where
// they are EQUAL cannot tell "read the new name" from "read the old one". They
// differ here specifically so preference is observable.
TEST(DevicePollResponse, PrefersRfcAccessTokenOverDeprecatedToken) {
  const TokenAuth::DevicePollResponse response = TokenAuth::ParseDevicePollResponse(
      "{\"status\":\"verified\",\"access_token\":\"rfc\",\"token\":\"deprecated\","
      "\"token_type\":\"Bearer\",\"expires_in\":3600,\"refresh_token\":\"refresh\","
      "\"refresh_token_expires_in\":2592000,\"user_id\":\"user\",\"username\":\"name\"}");

  EXPECT_EQ(response.status, TokenAuth::DevicePollStatus::Verified);
  EXPECT_EQ(response.access_token, "rfc");
  ASSERT_TRUE(response.expires_in.has_value());
  EXPECT_EQ(*response.expires_in, 3600U);
  ASSERT_TRUE(response.refresh_token_expires_in.has_value());
  EXPECT_EQ(*response.refresh_token_expires_in, 2592000U);
}

// Production nakama is the pre-f945f631d build until it is redeployed, and it
// sends neither access_token nor refresh_token_expires_in. A client that reads
// only the RFC names authenticates against nothing there.
TEST(DevicePollResponse, LegacyServerWithoutRfcFieldsStillAuthenticates) {
  const TokenAuth::DevicePollResponse response = TokenAuth::ParseDevicePollResponse(
      "{\"status\":\"verified\",\"token\":\"legacy\",\"refresh_token\":\"refresh\","
      "\"user_id\":\"user\",\"username\":\"name\"}");

  EXPECT_EQ(response.status, TokenAuth::DevicePollStatus::Verified);
  EXPECT_EQ(response.access_token, "legacy");
  EXPECT_EQ(response.refresh_token, "refresh");
  EXPECT_FALSE(response.expires_in.has_value());
  EXPECT_FALSE(response.refresh_token_expires_in.has_value());
}

// The server computes both fields as time.Until(deadline).Seconds(), which is
// negative once the deadline has passed — a verified entry stored by a nakama
// that predates the change carries expiry 0, so time.Until(1970) is a large
// negative. Adding that to `now` would place the expiry decades in the past and
// look like a measured value. Absent is the honest answer.
TEST(DevicePollResponse, NegativeExpiresInIsAbsentNotBackwards) {
  const TokenAuth::DevicePollResponse response = TokenAuth::ParseDevicePollResponse(
      "{\"status\":\"verified\",\"access_token\":\"tok\",\"expires_in\":-1757300000,"
      "\"refresh_token\":\"refresh\",\"refresh_token_expires_in\":-1757300000}");

  ASSERT_EQ(response.status, TokenAuth::DevicePollStatus::Verified);
  EXPECT_FALSE(response.expires_in.has_value());
  EXPECT_FALSE(response.refresh_token_expires_in.has_value());

  constexpr uint64_t kNow = 1000;
  EXPECT_EQ(TokenAuth::ResolveAccessTokenExpiry(kNow, response.access_token, response.expires_in),
            kNow + kFallbackAccessTokenLifetimeSec);
  EXPECT_GT(ResolveRefreshTokenExpirySec(kNow, response.refresh_token_expires_in), kNow);
}

// ReadExpiresInSeconds is reached while a refresh response is interpreted
// (nevr::auth::ApplyRefreshResponse), whose catch covers every json::exception; a
// non-object body must still yield absence here rather than a throw.
TEST(ReadExpiresInSeconds, NonObjectAndWrongTypedFieldsYieldAbsenceNotAThrow) {
  EXPECT_FALSE(ReadExpiresInSeconds(nlohmann::json::array({1, 2}), "expires_in").has_value());
  EXPECT_FALSE(ReadExpiresInSeconds(nlohmann::json("a string"), "expires_in").has_value());
  EXPECT_FALSE(ReadExpiresInSeconds(nlohmann::json(nullptr), "expires_in").has_value());
  EXPECT_FALSE(ReadExpiresInSeconds(nlohmann::json::parse("{\"expires_in\":\"3600\"}"),
                                    "expires_in")
                   .has_value());
  EXPECT_FALSE(ReadExpiresInSeconds(nlohmann::json::parse("{\"expires_in\":36.5}"), "expires_in")
                   .has_value());
  EXPECT_EQ(ReadExpiresInSeconds(nlohmann::json::parse("{\"expires_in\":3600}"), "expires_in"),
            std::optional<uint64_t>(3600));
}

// The 30-day constant is a guess at a server policy. It must apply only where the
// server said nothing, and never override a server that did speak.
TEST(RefreshTokenExpiry, ServerValueWinsAndFallbackOnlyFillsSilence) {
  constexpr uint64_t kNow = 1000;
  EXPECT_EQ(ResolveRefreshTokenExpirySec(kNow, 7200), kNow + 7200U);
  EXPECT_EQ(ResolveRefreshTokenExpirySec(kNow, std::nullopt),
            kNow + kFallbackRefreshTokenLifetimeSec);
  // A server-stated lifetime SHORTER than the fallback lifetime (30 days) must shorten
  // the client's belief — a fixed assumption would hide that.
  EXPECT_LT(ResolveRefreshTokenExpirySec(kNow, 86400),
            ResolveRefreshTokenExpirySec(kNow, std::nullopt));
}

// The refresh request body is built by nevr::auth::BuildRefreshBody (core/auth_refresh.h);
// both field names carry one value. A refresh_token-only body is rejected by a
// pre-f945f631d nakama with "invalid payload: token required".
TEST(RefreshRequestBody, CarriesBothFieldNamesWithTheSameValue) {
  const nlohmann::json body = nlohmann::json::parse(nevr::auth::BuildRefreshBody("rt"));

  EXPECT_EQ(body.value("refresh_token", ""), "rt");
  EXPECT_EQ(body.value("token", ""), "rt");
}

// Both ways of obtaining an access token must agree about when it dies. The
// refresh path must not hardcode now+60 while the poll path honours the JWT.
TEST(AccessTokenExpiry, RefreshAndPollPathsShareOneAuthorityOrder) {
  constexpr uint64_t kNow = 1000;
  const std::string jwt = MakeJwt("eyJleHAiOjUwMDB9");

  EXPECT_EQ(ResolveAccessTokenExpirySec(kNow, jwt, 10),
            TokenAuth::ResolveAccessTokenExpiry(kNow, jwt, 10));
  EXPECT_EQ(ResolveAccessTokenExpirySec(kNow, jwt, 10), 5000U);
  // No decodable exp: the server's expires_in is next, not a fixed 60 seconds.
  EXPECT_EQ(ResolveAccessTokenExpirySec(kNow, "opaque", 3600), kNow + 3600U);
  EXPECT_EQ(ResolveAccessTokenExpirySec(kNow, "opaque", std::nullopt),
            kNow + kFallbackAccessTokenLifetimeSec);
}

TEST(DevicePollResponse, JwtExpiryTakesPrecedenceThenFallsBack) {
  constexpr uint64_t kNow = 1000;
  const std::string jwt = MakeJwt("eyJleHAiOjUwMDB9");
  EXPECT_EQ(TokenAuth::ResolveAccessTokenExpiry(kNow, jwt, 10), 5000U);
  EXPECT_EQ(TokenAuth::ResolveAccessTokenExpiry(kNow, "not-a-jwt", 10), 1010U);
  EXPECT_EQ(TokenAuth::ResolveAccessTokenExpiry(kNow, "not-a-jwt", std::nullopt),
            kNow + kFallbackAccessTokenLifetimeSec);
}

namespace {

using FlowClock = TokenAuth::TestHook::DeviceAuthFlowOps::Clock;

struct FakeDeviceAuthFlow {
  FlowClock::time_point current{};
  std::string code = "device-code-secret-sentinel";
  intptr_t browser_result = 33;
  int ui_result = 1;
  FlowClock::duration ui_elapsed{};
  FlowClock::duration browser_elapsed{};
  FlowClock::duration poll_elapsed{};
  FlowClock::duration request_elapsed{};
  int request_calls = 0;
  int browser_calls = 0;
  int ui_calls = 0;
  int poll_calls = 0;
  int save_calls = 0;
  bool save_result = true;
  std::string browser_url;
  std::string ui_code;
  std::string ui_url;
  intptr_t ui_browser_result = -1;
  TokenAuth::DevicePollResponse poll_response;
  std::vector<TokenAuth::DevicePollResponse> poll_sequence;
  size_t poll_sequence_index = 0;
  std::vector<FlowClock::duration> sleep_durations;
  std::vector<std::pair<EchoVR::LogLevel, std::string>> logs;

  TokenAuth::TestHook::DeviceAuthFlowOps Ops() {
    TokenAuth::TestHook::DeviceAuthFlowOps ops;
    ops.now = [this]() { return current; };
    ops.request_device_code = [this]() {
      ++request_calls;
      current += request_elapsed;
      return code;
    };
    ops.open_browser = [this](const std::string& url) {
      ++browser_calls;
      browser_url = url;
      current += browser_elapsed;
      return browser_result;
    };
    ops.show_open_failure = [this](const std::string& shownCode, const std::string& url,
                                   intptr_t result) {
      ++ui_calls;
      ui_code = shownCode;
      ui_url = url;
      ui_browser_result = result;
      current += ui_elapsed;
      return ui_result;
    };
    ops.poll = [this](const std::string&) {
      ++poll_calls;
      current += poll_elapsed;
      if (poll_sequence_index < poll_sequence.size()) {
        return poll_sequence[poll_sequence_index++];
      }
      return poll_response;
    };
    ops.sleep = [this](FlowClock::duration duration) {
      sleep_durations.push_back(duration);
      current += duration;
    };
    ops.save = [this]() {
      ++save_calls;
      return save_result;
    };
    ops.log = [this](EchoVR::LogLevel level, const std::string& message) {
      logs.emplace_back(level, message);
    };
    return ops;
  }
};

TokenAuth::TestHook::DeviceAuthState ExistingDeviceAuthState() {
  TokenAuth::TestHook::DeviceAuthState state;
  state.token = "existing-access-token";
  state.token_expiry = static_cast<uint64_t>(std::time(nullptr)) + 3600U;
  state.refresh_token = "existing-refresh-token";
  state.refresh_token_expiry = 2100000000U;
  state.user_id = "existing-user";
  state.username = "existing-name";
  state.discord_id = 77U;
  state.authenticated = true;
  return state;
}

void ExpectSameDeviceAuthState(const TokenAuth::TestHook::DeviceAuthState& actual,
                               const TokenAuth::TestHook::DeviceAuthState& expected) {
  EXPECT_EQ(actual.authenticated, expected.authenticated);
  EXPECT_EQ(actual.token, expected.token);
  EXPECT_EQ(actual.token_expiry, expected.token_expiry);
  EXPECT_EQ(actual.refresh_token, expected.refresh_token);
  EXPECT_EQ(actual.refresh_token_expiry, expected.refresh_token_expiry);
  EXPECT_EQ(actual.user_id, expected.user_id);
  EXPECT_EQ(actual.username, expected.username);
  EXPECT_EQ(actual.discord_id, expected.discord_id);
}

TokenAuth::DevicePollResponse VerifiedPollResponse() {
  return TokenAuth::ParseDevicePollResponse(
      "{\"status\":\"verified\",\"access_token\":\"" +
      MakeJwt("eyJ2cnMiOnsiZGlkIjoiNDIifSwiZXhwIjo0MTAyNDQ0ODAwfQ") +
      "\",\"refresh_token\":\"new-refresh\",\"user_id\":\"new-user\","
      "\"username\":\"new-name\",\"expires_in\":3600,\"refresh_token_expires_in\":7200}");
}

}  // namespace

TEST(DeviceAuthFlow, ServerRefusesBeforeAnyHttpBrowserUiOrPollOperation) {
  FakeDeviceAuthFlow fake;
  const auto original = ExistingDeviceAuthState();
  const auto result = TokenAuth::TestHook::RunDeviceAuthFlow(true, original, fake.Ops());

  EXPECT_FALSE(result.success);
  ExpectSameDeviceAuthState(result.state, original);
  EXPECT_EQ(fake.request_calls, 0);
  EXPECT_EQ(fake.browser_calls, 0);
  EXPECT_EQ(fake.ui_calls, 0);
  EXPECT_EQ(fake.poll_calls, 0);
  EXPECT_EQ(fake.save_calls, 0);
}

TEST(DeviceAuthFlow, BrowserResultBoundaryUsesTransientUiOnlyForZeroAndThirtyTwo) {
  for (const intptr_t resultCode : {0, 32, 33}) {
    FakeDeviceAuthFlow fake;
    fake.browser_result = resultCode;
    fake.ui_result = 0;
    const auto flow = TokenAuth::TestHook::RunDeviceAuthFlow(false, ExistingDeviceAuthState(), fake.Ops());

    EXPECT_FALSE(flow.success) << resultCode;
    EXPECT_EQ(fake.browser_calls, 1) << resultCode;
    EXPECT_NE(fake.browser_url.find("device-code-secret-sentinel"), std::string::npos) << resultCode;
    EXPECT_EQ(fake.ui_calls, resultCode > 32 ? 0 : 1) << resultCode;
    if (resultCode <= 32) {
      EXPECT_EQ(fake.ui_code, fake.code);
      EXPECT_EQ(fake.ui_url, "https://echovrce.com/login/device");
      EXPECT_EQ(fake.ui_browser_result, resultCode);
      EXPECT_EQ(fake.poll_calls, 0);
      EXPECT_TRUE(std::any_of(fake.logs.begin(), fake.logs.end(), [](const auto& entry) {
        return entry.first == EchoVR::LogLevel::Error &&
               entry.second.find("stopped because the browser could not be opened") != std::string::npos;
      }));
      for (const auto& [level, message] : fake.logs) {
        (void)level;
        EXPECT_EQ(message.find(fake.code), std::string::npos);
      }
    }
    EXPECT_EQ(fake.save_calls, 0);
  }
}

TEST(DeviceAuthFlow, DismissalContinuesButUiDeadlineUsesOriginalFiveMinuteBudget) {
  for (const auto elapsedSeconds : {299, 301}) {
    FakeDeviceAuthFlow fake;
    fake.browser_result = 32;
    fake.ui_elapsed = std::chrono::seconds(elapsedSeconds);
    fake.poll_response.status = TokenAuth::DevicePollStatus::Pending;
    const auto flow = TokenAuth::TestHook::RunDeviceAuthFlow(false, ExistingDeviceAuthState(), fake.Ops());

    EXPECT_FALSE(flow.success) << elapsedSeconds;
    EXPECT_EQ(fake.ui_calls, 1);
    EXPECT_EQ(fake.poll_calls, 0);
    EXPECT_EQ(fake.save_calls, 0);
    if (elapsedSeconds == 299) {
      ASSERT_EQ(fake.sleep_durations.size(), 1U);
      EXPECT_EQ(fake.sleep_durations.front(), std::chrono::seconds(1));
    } else {
      EXPECT_TRUE(fake.sleep_durations.empty());
    }
  }
}

TEST(DeviceAuthFlow, BrowserThatReturnsAfterDeadlineDoesNotStartPolling) {
  FakeDeviceAuthFlow fake;
  fake.browser_result = 33;
  fake.browser_elapsed = std::chrono::seconds(301);
  fake.poll_response = VerifiedPollResponse();
  const auto original = ExistingDeviceAuthState();

  const auto result = TokenAuth::TestHook::RunDeviceAuthFlow(false, original, fake.Ops());

  EXPECT_FALSE(result.success);
  EXPECT_EQ(fake.ui_calls, 0);
  EXPECT_EQ(fake.poll_calls, 0);
  EXPECT_EQ(fake.save_calls, 0);
  ExpectSameDeviceAuthState(result.state, original);
}

TEST(DeviceAuthFlow, PollResultAtOrAfterDeadlineThatIsNotVerifiedDoesNotMutateOrSaveAnyAuthField) {
  for (const char* body : {R"({"status":"pending"})", R"({"status":"expired"})"}) {
    for (const auto elapsedSeconds : {300, 301}) {
      FakeDeviceAuthFlow fake;
      fake.poll_response = TokenAuth::ParseDevicePollResponse(body);
      fake.poll_elapsed = std::chrono::seconds(elapsedSeconds - 3);
      const auto original = ExistingDeviceAuthState();
      const auto flow = TokenAuth::TestHook::RunDeviceAuthFlow(false, original, fake.Ops());

      EXPECT_FALSE(flow.success) << body << " " << elapsedSeconds;
      EXPECT_EQ(fake.poll_calls, 1);
      EXPECT_EQ(fake.save_calls, 0);
      ExpectSameDeviceAuthState(flow.state, original);
    }
  }
}

// The server deletes a device code when the poll that reports it verified returns the tokens
// (nakama evr_device_auth.go, verified branch of the poll RPC), so a verified answer whose poll
// returns at or after the deadline is the player's only copy of the login: it is applied and saved.
TEST(DeviceAuthFlow, VerifiedPollResultReturningAtOrAfterDeadlineIsApplied) {
  for (const auto elapsedSeconds : {300, 301}) {
    FakeDeviceAuthFlow fake;
    fake.poll_response = VerifiedPollResponse();
    fake.poll_elapsed = std::chrono::seconds(elapsedSeconds - 3);
    const auto original = ExistingDeviceAuthState();
    const auto flow = TokenAuth::TestHook::RunDeviceAuthFlow(false, original, fake.Ops());

    EXPECT_TRUE(flow.success) << elapsedSeconds;
    EXPECT_EQ(fake.poll_calls, 1);
    EXPECT_EQ(fake.save_calls, 1);
    EXPECT_EQ(flow.state.refresh_token, "new-refresh");
    EXPECT_EQ(flow.state.user_id, "new-user");
  }
}

TEST(DeviceAuthFlow, SleepIsCappedAtDeadlineAndTimelyVerificationAppliesAndSaves) {
  FakeDeviceAuthFlow fake;
  fake.browser_result = 0;
  fake.ui_elapsed = std::chrono::seconds(298);
  fake.ui_result = 1;
  fake.poll_response = VerifiedPollResponse();
  fake.save_result = false;  // Save failure must not undo successful in-memory auth.
  const auto result = TokenAuth::TestHook::RunDeviceAuthFlow(false, ExistingDeviceAuthState(), fake.Ops());

  EXPECT_FALSE(result.success);  // 298s + capped 2s reaches the deadline before polling.
  EXPECT_EQ(fake.poll_calls, 0);
  ASSERT_EQ(fake.sleep_durations.size(), 1U);
  EXPECT_EQ(fake.sleep_durations.front(), std::chrono::seconds(2));
  EXPECT_EQ(fake.save_calls, 0);

  FakeDeviceAuthFlow timely;
  timely.browser_result = 33;
  timely.poll_response = VerifiedPollResponse();
  timely.save_result = false;
  const auto original = ExistingDeviceAuthState();
  const uint64_t wallClockBefore = static_cast<uint64_t>(std::time(nullptr));
  const auto success = TokenAuth::TestHook::RunDeviceAuthFlow(false, original, timely.Ops());
  const uint64_t wallClockAfter = static_cast<uint64_t>(std::time(nullptr));
  EXPECT_TRUE(success.success);
  EXPECT_EQ(timely.save_calls, 1);
  EXPECT_NE(success.state.token, original.token);
  EXPECT_EQ(success.state.token_expiry, 4102444800ULL);
  EXPECT_EQ(success.state.refresh_token, "new-refresh");
  EXPECT_GE(success.state.refresh_token_expiry, wallClockBefore + 7200U);
  EXPECT_LE(success.state.refresh_token_expiry, wallClockAfter + 7200U);
  EXPECT_EQ(success.state.user_id, "new-user");
  EXPECT_EQ(success.state.username, "new-name");
  EXPECT_EQ(success.state.discord_id, 42U);
  EXPECT_TRUE(success.state.authenticated);
}

TEST(DeviceAuthFlow, DeadlineStartsAfterTheNonemptyDeviceCodeResponse) {
  FakeDeviceAuthFlow fake;
  fake.request_elapsed = std::chrono::seconds(60);
  fake.browser_result = 32;
  fake.ui_elapsed = std::chrono::seconds(290);
  fake.ui_result = 1;
  fake.poll_response = VerifiedPollResponse();

  const auto result = TokenAuth::TestHook::RunDeviceAuthFlow(false, ExistingDeviceAuthState(), fake.Ops());

  EXPECT_TRUE(result.success);
  ASSERT_EQ(fake.sleep_durations.size(), 1U);
  EXPECT_EQ(fake.sleep_durations.front(), std::chrono::seconds(3));
  EXPECT_EQ(fake.poll_calls, 1);
  EXPECT_EQ(fake.save_calls, 1);
}

TEST(DeviceAuthFlow, DeviceCodeIsNeverLoggedAtAnyLevelButReachesBrowserAndUi) {
  FakeDeviceAuthFlow uiFailure;
  uiFailure.browser_result = 32;
  uiFailure.ui_result = 0;
  (void)TokenAuth::TestHook::RunDeviceAuthFlow(false, ExistingDeviceAuthState(), uiFailure.Ops());

  FakeDeviceAuthFlow polling;
  polling.browser_result = 33;
  for (unsigned int index = 0; index < 10; ++index) {
    TokenAuth::DevicePollResponse pending;
    pending.status = TokenAuth::DevicePollStatus::Pending;
    polling.poll_sequence.push_back(pending);
  }
  TokenAuth::DevicePollResponse error;
  error.status = TokenAuth::DevicePollStatus::Error;
  polling.poll_sequence.push_back(error);
  (void)TokenAuth::TestHook::RunDeviceAuthFlow(false, ExistingDeviceAuthState(), polling.Ops());

  EXPECT_EQ(polling.browser_url, "https://echovrce.com/login/device?code=device-code-secret-sentinel");
  EXPECT_EQ(uiFailure.ui_code, uiFailure.code);
  EXPECT_EQ(uiFailure.ui_url, "https://echovrce.com/login/device");
  bool sawInfo = false;
  bool sawDebug = false;
  bool sawWarning = false;
  bool sawError = false;
  for (const auto* fake : {&uiFailure, &polling}) {
    ASSERT_FALSE(fake->logs.empty());
    for (const auto& [level, message] : fake->logs) {
      EXPECT_EQ(message.find(fake->code), std::string::npos) << "device code leaked to a persistent log";
      sawInfo = sawInfo || level == EchoVR::LogLevel::Info;
      sawDebug = sawDebug || level == EchoVR::LogLevel::Debug;
      sawWarning = sawWarning || level == EchoVR::LogLevel::Warning;
      sawError = sawError || level == EchoVR::LogLevel::Error;
    }
  }
  EXPECT_TRUE(sawInfo);
  EXPECT_TRUE(sawDebug);
  EXPECT_TRUE(sawWarning);
  EXPECT_TRUE(sawError);
}

// The game closing while the player has not signed in yet (#37): once cancelled, the flow stops at
// its next wake-up without polling again or touching the stored credentials.
TEST(DeviceAuthFlow, CancellationStopsTheWaitWithoutPollingOrSaving) {
  FakeDeviceAuthFlow fake;
  fake.browser_result = 33;
  fake.poll_response = TokenAuth::ParseDevicePollResponse("{\"status\":\"authorization_pending\"}");
  int wakeups = 0;
  auto ops = fake.Ops();
  ops.cancelled = [&wakeups]() { return ++wakeups >= 3; };  // cancelled at the third wake-up
  const auto original = ExistingDeviceAuthState();
  const auto result = TokenAuth::TestHook::RunDeviceAuthFlow(false, original, ops);

  EXPECT_FALSE(result.success);
  EXPECT_EQ(fake.poll_calls, 2) << "polled after the first two wake-ups, not after the cancelled one";
  EXPECT_EQ(fake.save_calls, 0);
  ExpectSameDeviceAuthState(result.state, original);
  const bool logged = std::any_of(fake.logs.begin(), fake.logs.end(), [](const auto& entry) {
    return entry.second.find("cancelled") != std::string::npos;
  });
  EXPECT_TRUE(logged);
}

// No cancellation hook (the existing callers) behaves as before: the wait runs to verification.
TEST(DeviceAuthFlow, WithoutACancellationHookTheFlowIsUnchanged) {
  FakeDeviceAuthFlow fake;
  fake.browser_result = 33;
  fake.poll_response = VerifiedPollResponse();
  const auto result = TokenAuth::TestHook::RunDeviceAuthFlow(false, ExistingDeviceAuthState(), fake.Ops());
  EXPECT_TRUE(result.success);
  EXPECT_EQ(fake.save_calls, 1);
}

// The sign-in wait (#37, G7): the flow runs on a worker while the caller keeps pumping, so the
// caller's window stays responsive; the caller still gets the flow's result before it continues.
TEST(OffThreadWait, FlowRunsOnAWorkerWhileTheCallerPumps) {
  std::atomic<int> pumps{0};
  const auto r = TokenAuth::RunWhilePumping(
      [&pumps]() {
        // Block until the caller has pumped a few times: proves the caller is not blocked on us.
        while (pumps.load() < 3) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return true;
      },
      [&pumps]() { ++pumps; }, std::chrono::milliseconds(1));
  EXPECT_TRUE(r.flowResult);
  EXPECT_TRUE(r.ranOnOtherThread);
  EXPECT_FALSE(r.ranInline);
  EXPECT_FALSE(r.flowThrew);
  EXPECT_GE(r.pumpCalls, 3U);
  EXPECT_EQ(r.pumpCalls, static_cast<unsigned>(pumps.load()));
}

TEST(OffThreadWait, FlowResultAndThrowAreReported) {
  const auto failed = TokenAuth::RunWhilePumping([]() { return false; }, []() {}, std::chrono::milliseconds(1));
  EXPECT_FALSE(failed.flowResult);
  EXPECT_FALSE(failed.flowThrew);
  const auto threw = TokenAuth::RunWhilePumping(
      []() -> bool { throw std::runtime_error("flow failed"); }, []() {}, std::chrono::milliseconds(1));
  EXPECT_FALSE(threw.flowResult);
  EXPECT_TRUE(threw.flowThrew);
}

// A flow that finishes before the first interval elapses needs no pumping at all.
TEST(OffThreadWait, QuickFlowNeedsNoPump) {
  int pumps = 0;
  const auto r = TokenAuth::RunWhilePumping([]() { return true; }, [&pumps]() { ++pumps; },
                                            std::chrono::milliseconds(10'000));
  EXPECT_TRUE(r.flowResult);
  EXPECT_EQ(pumps, 0);
  EXPECT_EQ(r.pumpCalls, 0U);
}

// A transient poll failure (#202: curl_code=28) must not cost the session: the wait keeps polling and
// completes when a later poll verifies; only a run of consecutive errors ends it.
TEST(DeviceAuthFlow, TransientPollErrorsAreRetriedUntilVerified) {
  FakeDeviceAuthFlow fake;
  fake.browser_result = 33;
  TokenAuth::DevicePollResponse error;
  error.status = TokenAuth::DevicePollStatus::Error;
  fake.poll_sequence = {error, error, error, error};
  fake.poll_response = VerifiedPollResponse();

  const auto result = TokenAuth::TestHook::RunDeviceAuthFlow(false, ExistingDeviceAuthState(), fake.Ops());

  EXPECT_TRUE(result.success);
  EXPECT_EQ(fake.poll_calls, 5);
  EXPECT_EQ(fake.save_calls, 1);
}

TEST(DeviceAuthFlow, ConsecutivePollErrorsEndTheWaitAfterTheLimit) {
  FakeDeviceAuthFlow fake;
  fake.browser_result = 33;
  fake.poll_response.status = TokenAuth::DevicePollStatus::Error;

  const auto result = TokenAuth::TestHook::RunDeviceAuthFlow(false, ExistingDeviceAuthState(), fake.Ops());

  EXPECT_FALSE(result.success);
  EXPECT_EQ(fake.poll_calls, 5);
  EXPECT_EQ(fake.save_calls, 0);
}

TEST(DeviceAuthFlow, AnAnsweredPollResetsTheErrorRun) {
  FakeDeviceAuthFlow fake;
  fake.browser_result = 33;
  TokenAuth::DevicePollResponse error;
  error.status = TokenAuth::DevicePollStatus::Error;
  TokenAuth::DevicePollResponse pending;
  pending.status = TokenAuth::DevicePollStatus::Pending;
  fake.poll_sequence = {error, error, error, error, pending, error, error, error, error};
  fake.poll_response = VerifiedPollResponse();

  const auto result = TokenAuth::TestHook::RunDeviceAuthFlow(false, ExistingDeviceAuthState(), fake.Ops());

  EXPECT_TRUE(result.success);
  EXPECT_EQ(fake.poll_calls, 10);
}

TEST(DeviceAuthFlow, MalformedVerifiedCandidateDoesNotChangeStateOrSave) {
  const auto original = ExistingDeviceAuthState();
  const std::vector<std::string> malformedBodies = {
      "{\"status\":\"verified\",\"access_token\":7,\"refresh_token\":\"refresh\","
      "\"user_id\":\"user\",\"username\":\"name\"}",
      "{\"status\":\"verified\",\"token\":7,\"refresh_token\":\"refresh\","
      "\"user_id\":\"user\",\"username\":\"name\"}",
      "{\"status\":\"verified\",\"access_token\":\"candidate\",\"refresh_token\":7,"
      "\"user_id\":\"user\",\"username\":\"name\"}",
      "{\"status\":\"verified\",\"access_token\":\"candidate\",\"refresh_token\":\"refresh\","
      "\"user_id\":7,\"username\":\"name\"}",
      "{\"status\":\"verified\",\"access_token\":\"candidate\",\"refresh_token\":\"refresh\","
      "\"user_id\":\"user\",\"username\":7}",
  };

  for (const std::string& body : malformedBodies) {
    FakeDeviceAuthFlow fake;
    fake.poll_response = TokenAuth::ParseDevicePollResponse(body);
    ASSERT_EQ(fake.poll_response.status, TokenAuth::DevicePollStatus::Error);
    const auto result = TokenAuth::TestHook::RunDeviceAuthFlow(false, original, fake.Ops());

    EXPECT_FALSE(result.success) << body;
    EXPECT_EQ(fake.save_calls, 0) << body;
    ExpectSameDeviceAuthState(result.state, original);
  }
}

TEST(TokenAuthModule, ServerHostSkipsDeviceAuthentication) {
  const NvrModuleContext context = MakeModuleContext(NEVR_MODULE_HOST_IS_SERVER);

  EXPECT_EQ(token_auth_Init(&context), 0);
  const auto snapshot = TokenAuth::GetAuthSnapshot();
  ASSERT_NE(snapshot, nullptr);
  EXPECT_EQ(snapshot->readiness, TokenAuth::AuthReadiness::Disabled);
  EXPECT_TRUE(TokenAuth::GetToken().empty());
  EXPECT_EQ(TokenAuth::GetDiscordId(), 0U);
  EXPECT_TRUE(TokenAuth::GetUsername().empty());
  token_auth_Shutdown();
  EXPECT_EQ(TokenAuth::GetAuthSnapshot()->readiness, TokenAuth::AuthReadiness::Disabled);
}

TEST(TokenAuthModule, ClientWithoutRequiredConfigDisablesCleanly) {
  const NvrModuleContext context = MakeModuleContext(NEVR_MODULE_HOST_IS_CLIENT);

  EXPECT_EQ(token_auth_Init(&context), 0);
  const auto snapshot = TokenAuth::GetAuthSnapshot();
  ASSERT_NE(snapshot, nullptr);
  EXPECT_EQ(snapshot->readiness, TokenAuth::AuthReadiness::Disabled);
  EXPECT_TRUE(TokenAuth::GetToken().empty());
  EXPECT_EQ(TokenAuth::GetDiscordId(), 0U);
  EXPECT_TRUE(TokenAuth::GetUsername().empty());
  token_auth_Shutdown();
}

TEST(TokenAuthModule, ReportsThePublishedModuleApiVersion) {
  EXPECT_EQ(token_auth_ApiVersion(), NEVR_MODULE_API_VERSION);
}

TEST(AuthSnapshotStore, PublishedSnapshotIsImmutableAndGetsANewGeneration) {
  TokenAuth::AuthSnapshotStore store;
  const auto initial = store.Read();
  ASSERT_NE(initial, nullptr);
  EXPECT_EQ(initial->generation, 0U);

  TokenAuth::AuthSnapshot first;
  first.readiness = TokenAuth::AuthReadiness::Ready;
  first.access_token = "token-first";
  first.access_expiry = 1000;
  first.discord_id = 11;
  first.user_id = "user-first";
  first.username = "name-first";
  const auto publishedFirst = store.Publish(std::move(first));

  TokenAuth::AuthSnapshot second;
  second.readiness = TokenAuth::AuthReadiness::Ready;
  second.access_token = "token-second";
  second.access_expiry = 2000;
  second.discord_id = 22;
  second.user_id = "user-second";
  second.username = "name-second";
  const auto publishedSecond = store.Publish(std::move(second));

  ASSERT_NE(publishedFirst, nullptr);
  ASSERT_NE(publishedSecond, nullptr);
  EXPECT_LT(publishedFirst->generation, publishedSecond->generation);
  EXPECT_EQ(store.Read(), publishedSecond);
  EXPECT_EQ(publishedFirst->access_token, "token-first");
  EXPECT_EQ(publishedFirst->access_expiry, 1000U);
  EXPECT_EQ(publishedFirst->discord_id, 11U);
  EXPECT_EQ(publishedFirst->user_id, "user-first");
  EXPECT_EQ(publishedFirst->username, "name-first");
}

TEST(AuthSnapshotStore, ConcurrentReadersNeverObserveMixedGenerations) {
  TokenAuth::AuthSnapshotStore store;
  std::atomic<bool> done{false};
  std::atomic<bool> mismatch{false};
  constexpr uint64_t kPublishCount = 20000;

  auto reader = [&store, &done, &mismatch] {
    while (!done.load(std::memory_order_acquire)) {
      const auto snapshot = store.Read();
      if (snapshot->generation == 0) continue;
      const std::string expectedToken = "token-" + std::to_string(snapshot->discord_id);
      const std::string expectedUser = "user-" + std::to_string(snapshot->discord_id);
      const std::string expectedName = "name-" + std::to_string(snapshot->discord_id);
      if (snapshot->access_token != expectedToken ||
          snapshot->access_expiry != snapshot->discord_id + 1000U ||
          snapshot->user_id != expectedUser || snapshot->username != expectedName) {
        mismatch.store(true, std::memory_order_release);
        return;
      }
    }
  };

  std::thread readerOne(reader);
  std::thread readerTwo(reader);
  for (uint64_t id = 1; id <= kPublishCount; ++id) {
    TokenAuth::AuthSnapshot snapshot;
    snapshot.readiness = TokenAuth::AuthReadiness::Ready;
    snapshot.access_token = "token-" + std::to_string(id);
    snapshot.access_expiry = id + 1000U;
    snapshot.discord_id = id;
    snapshot.user_id = "user-" + std::to_string(id);
    snapshot.username = "name-" + std::to_string(id);
    (void)store.Publish(std::move(snapshot));
  }
  done.store(true, std::memory_order_release);
  readerOne.join();
  readerTwo.join();
  EXPECT_FALSE(mismatch.load(std::memory_order_acquire));
}

TEST(AuthCancellation, StopRequestWakesWaitersAndRemainsObservable) {
  TokenAuth::AuthCancellation cancellation;
  std::atomic<bool> enteredWait{false};
  std::atomic<bool> wokeForStop{false};
  std::thread waiter([&] {
    enteredWait.store(true, std::memory_order_release);
    wokeForStop.store(cancellation.WaitFor(std::chrono::hours(1)), std::memory_order_release);
  });
  while (!enteredWait.load(std::memory_order_acquire)) std::this_thread::yield();
  cancellation.RequestStop();
  waiter.join();

  EXPECT_TRUE(wokeForStop.load(std::memory_order_acquire));
  EXPECT_TRUE(cancellation.IsStopRequested());
  EXPECT_TRUE(cancellation.WaitFor(std::chrono::milliseconds(0)));
}

// Issue #23: Load and Save must select the same credentials directory. Existing
// credentials take precedence over configs so refreshes do not move a cache.
class CredentialsDirFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("nevr-i21-" + std::to_string(GetCurrentProcessId()) + "-" +
             ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_ / "bin" / "win10");
    // GetExeDirectory's shape: absolute, trailing separator.
    exe_dir_ = (root_ / "bin" / "win10").string() + "\\";
  }
  void TearDown() override { std::filesystem::remove_all(root_); }

  void Write(const std::filesystem::path& file, const std::string& contents) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output.is_open()) << file.string();
    output << contents;
  }

  std::string Read(const std::filesystem::path& file) const {
    std::ifstream input(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }

  nlohmann::json Credentials(const std::string& refreshToken) const {
    return nlohmann::json{{"refresh_token", refreshToken}, {"refresh_token_expiry", 2000000000U}};
  }

  std::filesystem::path root_;
  std::string exe_dir_;
};

TEST_F(CredentialsDirFixture, YamlOnlySaveRotatesAndReloadsRefreshTokenWithoutChangingConfig) {
  const auto yamlPath = root_ / "bin" / "win10" / "_local" / "config.yaml";
  const std::string yamlContents = "services:\n  telemetry: https://example.test\n";
  Write(yamlPath, yamlContents);

  const CredentialCacheLocation location = SelectCredentialCacheLocation(exe_dir_);
  EXPECT_TRUE(std::filesystem::equivalent(location.directory, yamlPath.parent_path()));
  const std::string credentialSuffix = ".credentials.json";
  ASSERT_GE(location.credentialsPath.size(), credentialSuffix.size());
  EXPECT_EQ(location.credentialsPath.substr(location.credentialsPath.size() - credentialSuffix.size()),
            credentialSuffix);

  CachedAuthToken auth;
  auth.token = "in-memory-access-token";
  auth.token_expiry = 2000000000U;
  auth.refresh_token = "refresh-token-v1";
  auth.refresh_token_expiry = 2100000000U;
  auth.user_id = "account-17";
  auth.username = "cached-player";
  ASSERT_TRUE(SaveAuthToken(auth, exe_dir_));

  nlohmann::json expected = {{"refresh_token", "refresh-token-v1"},
                             {"refresh_token_expiry", 2100000000U},
                             {"user_id", "account-17"},
                             {"username", "cached-player"}};
  std::ifstream saved(location.credentialsPath, std::ios::binary);
  ASSERT_TRUE(saved.is_open());
  const nlohmann::json actual = nlohmann::json::parse(saved);
  EXPECT_EQ(actual, expected);
  EXPECT_FALSE(actual.contains("token"));
  EXPECT_FALSE(actual.contains("token_expiry"));
  EXPECT_EQ(Read(yamlPath), yamlContents);

  CachedAuthToken loaded = LoadCachedAuthToken(exe_dir_);
  EXPECT_TRUE(loaded.token.empty());
  EXPECT_EQ(loaded.refresh_token, "refresh-token-v1");
  EXPECT_EQ(loaded.user_id, "account-17");

  auth.refresh_token = "refresh-token-v2";
  auth.refresh_token_expiry = 2200000000U;
  ASSERT_TRUE(SaveAuthToken(auth, exe_dir_));
  loaded = LoadCachedAuthToken(exe_dir_);
  EXPECT_EQ(loaded.refresh_token, "refresh-token-v2");
  EXPECT_EQ(loaded.refresh_token_expiry, 2200000000U);
  EXPECT_EQ(Read(yamlPath), yamlContents);
}

TEST_F(CredentialsDirFixture, NearExistingCredentialsBeatFartherYaml) {
  const auto nearCredentials = root_ / "bin" / "win10" / "_local" / ".credentials.json";
  const auto fartherYaml = root_ / "_local" / "config.yaml";
  Write(nearCredentials, Credentials("near-existing").dump());
  const std::string fartherContents = "services: farther\n";
  Write(fartherYaml, fartherContents);

  const CredentialCacheLocation location = SelectCredentialCacheLocation(exe_dir_);
  EXPECT_TRUE(std::filesystem::equivalent(location.credentialsPath, nearCredentials));
  EXPECT_EQ(LoadCachedAuthToken(exe_dir_).refresh_token, "near-existing");

  CachedAuthToken auth;
  auth.refresh_token = "rotated-near";
  auth.refresh_token_expiry = 2100000000U;
  ASSERT_TRUE(SaveAuthToken(auth, exe_dir_));
  EXPECT_EQ(LoadCachedAuthToken(exe_dir_).refresh_token, "rotated-near");
  EXPECT_EQ(Read(fartherYaml), fartherContents);
}

TEST_F(CredentialsDirFixture, FartherExistingCredentialsBeatNearYaml) {
  const auto nearYaml = root_ / "bin" / "win10" / "_local" / "config.yaml";
  const auto fartherCredentials = root_ / "bin" / "_local" / ".credentials.json";
  const std::string nearContents = "services: near\n";
  Write(nearYaml, nearContents);
  Write(fartherCredentials, Credentials("farther-existing").dump());

  const CredentialCacheLocation location = SelectCredentialCacheLocation(exe_dir_);
  EXPECT_TRUE(std::filesystem::equivalent(location.credentialsPath, fartherCredentials));
  EXPECT_EQ(LoadCachedAuthToken(exe_dir_).refresh_token, "farther-existing");
  EXPECT_EQ(Read(nearYaml), nearContents);
}

TEST_F(CredentialsDirFixture, FirstYamlDirectoryInSuffixOrderWinsWhenNoCredentialsExist) {
  const auto nearYaml = root_ / "bin" / "win10" / "_local" / "config.yaml";
  const auto fartherYaml = root_ / "bin" / "_local" / "config.yaml";
  const std::string nearContents = "services: near\n";
  Write(nearYaml, nearContents);
  Write(fartherYaml, "services: farther\n");

  const CredentialCacheLocation location = SelectCredentialCacheLocation(exe_dir_);
  EXPECT_TRUE(std::filesystem::equivalent(location.directory, nearYaml.parent_path()));
  CachedAuthToken auth;
  auth.refresh_token = "near-yaml-refresh";
  auth.refresh_token_expiry = 2100000000U;
  ASSERT_TRUE(SaveAuthToken(auth, exe_dir_));
  EXPECT_TRUE(std::filesystem::exists(nearYaml.parent_path() / ".credentials.json"));
  EXPECT_FALSE(std::filesystem::exists(fartherYaml.parent_path() / ".credentials.json"));
  EXPECT_EQ(Read(nearYaml), nearContents);
}

TEST_F(CredentialsDirFixture, FirstExistingCredentialsInSuffixOrderWins) {
  const auto nearCredentials = root_ / "bin" / "win10" / "_local" / ".credentials.json";
  const auto parentCredentials = root_ / "bin" / "_local" / ".credentials.json";
  const auto rootCredentials = root_ / "_local" / ".credentials.json";
  Write(nearCredentials, Credentials("near").dump());
  Write(parentCredentials, Credentials("parent").dump());
  Write(rootCredentials, Credentials("root").dump());

  const CredentialCacheLocation location = SelectCredentialCacheLocation(exe_dir_);
  EXPECT_TRUE(std::filesystem::equivalent(location.credentialsPath, nearCredentials));
  EXPECT_EQ(LoadCachedAuthToken(exe_dir_).refresh_token, "near");
}

TEST_F(CredentialsDirFixture, MalformedSelectedCredentialsDoNotFallThroughAndSaveRepairsThem) {
  const auto selected = root_ / "bin" / "win10" / "_local" / ".credentials.json";
  const auto farther = root_ / "bin" / "_local" / ".credentials.json";
  Write(selected, "{ malformed");
  const std::string fartherContents = Credentials("farther-valid").dump();
  Write(farther, fartherContents);

  const CredentialCacheLocation location = SelectCredentialCacheLocation(exe_dir_);
  EXPECT_TRUE(std::filesystem::equivalent(location.credentialsPath, selected));
  EXPECT_TRUE(LoadCachedAuthToken(exe_dir_).refresh_token.empty());

  CachedAuthToken auth;
  auth.refresh_token = "repaired-selected";
  auth.refresh_token_expiry = 2100000000U;
  ASSERT_TRUE(SaveAuthToken(auth, exe_dir_));
  EXPECT_EQ(LoadCachedAuthToken(exe_dir_).refresh_token, "repaired-selected");
  EXPECT_EQ(Read(farther), fartherContents);
}

TEST_F(CredentialsDirFixture, NoCredentialsOrYamlFallsBackBesideExecutable) {
  const auto fallback = root_ / "bin" / "win10" / "_local" / ".credentials.json";
  const CredentialCacheLocation location = SelectCredentialCacheLocation(exe_dir_);
  EXPECT_EQ(std::filesystem::path(location.directory).lexically_normal(), fallback.parent_path().lexically_normal());

  CachedAuthToken auth;
  auth.refresh_token = "fallback-refresh";
  auth.refresh_token_expiry = 2100000000U;
  ASSERT_TRUE(SaveAuthToken(auth, exe_dir_));
  EXPECT_EQ(LoadCachedAuthToken(exe_dir_).refresh_token, "fallback-refresh");
  EXPECT_TRUE(std::filesystem::exists(fallback));
}

TEST(CredentialCachePath, JoinHandlesTrailingAndMissingSeparators) {
  EXPECT_EQ(JoinCredentialPath("C:\\games\\bin\\", "_local"), "C:\\games\\bin\\_local");
  EXPECT_EQ(JoinCredentialPath("C:/games/bin/", "_local"), "C:/games/bin/_local");
  EXPECT_EQ(JoinCredentialPath("C:\\games\\bin", "_local"), "C:\\games\\bin\\_local");
}

// #202: a transient refresh failure must be retried, bounded, with pauses only between attempts.
TEST(BoundedRetry, SucceedsOnTheSecondTryAndPausesOnce) {
  int calls = 0;
  std::vector<int> pauses;
  const auto r = nevr::RetryBounded(3, 2000, [&] { return ++calls == 2; }, [&](int ms) { pauses.push_back(ms); });
  EXPECT_TRUE(r.ok);
  EXPECT_EQ(r.attempts, 2);
  EXPECT_EQ(pauses, std::vector<int>({2000}));
}

TEST(BoundedRetry, GivesUpAfterTheLastAttemptWithoutAFinalPause) {
  int calls = 0;
  std::vector<int> pauses;
  const auto r = nevr::RetryBounded(3, 2000, [&] { ++calls; return false; }, [&](int ms) { pauses.push_back(ms); });
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(r.attempts, 3);
  EXPECT_EQ(calls, 3);
  EXPECT_EQ(pauses.size(), 2u);
}

TEST(BoundedRetry, FirstSuccessDoesNotPause) {
  std::vector<int> pauses;
  const auto r = nevr::RetryBounded(3, 2000, [] { return true; }, [&](int ms) { pauses.push_back(ms); });
  EXPECT_TRUE(r.ok);
  EXPECT_EQ(r.attempts, 1);
  EXPECT_TRUE(pauses.empty());
}

TEST(BoundedRetry, ZeroAttemptsStillTriesOnce) {
  int calls = 0;
  const auto r = nevr::RetryBounded(0, 10, [&] { ++calls; return false; }, [](int) {});
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(calls, 1);
}
