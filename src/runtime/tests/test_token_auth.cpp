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
#include "core/credential_cache_stats.h"
#include "runtime/tests/digest_for_test.h"
#include "core/auth_refresh.h"
#include "core/bounded_retry.h"
#include "core/signin_dialog_lifecycle.h"
#include "core/signin_dialog_text.h"
#include "core/system_info.h"
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
  const nevr_token_auth::test_hook::DeviceAuthState state =
      nevr_token_auth::test_hook::InspectInitialDeviceAuth();

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

  const nevr_token_auth::test_hook::DeviceAuthState state =
      nevr_token_auth::test_hook::InspectDeviceAuthAfterRefresh(refreshed);

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

  const nevr_token_auth::test_hook::DeviceAuthState state =
      nevr_token_auth::test_hook::InspectDeviceAuthAfterRefresh(refreshed);

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

  const nevr_token_auth::test_hook::DeviceAuthState state =
      nevr_token_auth::test_hook::InspectDeviceAuthFromCache();

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

  EXPECT_FALSE(nevr_token_auth::test_hook::InspectRefreshDecision(live, now));
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
  EXPECT_FALSE(nevr_token_auth::test_hook::InspectRefreshDecision(live, now));

  live.token_expiry = now + 300;
  EXPECT_TRUE(nevr_token_auth::test_hook::InspectRefreshDecision(live, now));

  live.token_expiry = now + 60;
  EXPECT_TRUE(nevr_token_auth::test_hook::InspectRefreshDecision(live, now));

  live.token_expiry = now - 1;
  EXPECT_TRUE(nevr_token_auth::test_hook::InspectRefreshDecision(live, now));

  // No live token at all: refresh, do not sit on an empty session.
  live.token_expiry = 0;
  EXPECT_TRUE(nevr_token_auth::test_hook::InspectRefreshDecision(live, now));
}

TEST(DevicePollResponse, VerifiedResponseExtractsEveryTokenField) {
  const nevr_token_auth::DevicePollResponse response = nevr_token_auth::ParseDevicePollResponse(
      "{\"status\":\"verified\",\"token\":\"token\",\"refresh_token\":\"refresh\","
      "\"user_id\":\"user\",\"username\":\"name\",\"expires_in\":3600}");

  EXPECT_EQ(response.status, nevr_token_auth::DevicePollStatus::Verified);
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
  const nevr_token_auth::DevicePollResponse response = nevr_token_auth::ParseDevicePollResponse(
      "{\"status\":\"verified\",\"token\":\"" + accessToken + "\",\"expires_in\":0}");

  ASSERT_EQ(response.status, nevr_token_auth::DevicePollStatus::Verified);
  ASSERT_TRUE(response.expires_in.has_value());
  EXPECT_EQ(*response.expires_in, 0U);
  EXPECT_EQ(nevr_token_auth::ResolveAccessTokenExpiry(kNow, response.access_token, response.expires_in),
            kJwtExpiry);
}

TEST(DevicePollResponse, PendingExpiredAndErrorResponsesRemainDistinct) {
  EXPECT_EQ(nevr_token_auth::ParseDevicePollResponse("{\"status\":\"authorization_pending\"}").status,
            nevr_token_auth::DevicePollStatus::Pending);
  EXPECT_EQ(nevr_token_auth::ParseDevicePollResponse("{\"status\":\"expired\"}").status,
            nevr_token_auth::DevicePollStatus::Expired);
  EXPECT_EQ(nevr_token_auth::ParseDevicePollResponse("{\"error\":\"access_denied\"}").status,
            nevr_token_auth::DevicePollStatus::Error);
  EXPECT_EQ(nevr_token_auth::ParseDevicePollResponse("not json").status,
            nevr_token_auth::DevicePollStatus::Error);
}

TEST(DevicePollResponse, WrongTypedVerifiedFieldsRemainErrorAndExpiryTypesStayAbsent) {
  for (const std::string field : {"access_token", "refresh_token", "user_id", "username"}) {
    const std::string body = "{\"status\":\"verified\",\"access_token\":\"access\","
                             "\"refresh_token\":\"refresh\",\"user_id\":\"user\","
                             "\"username\":\"name\",\"" + field + "\":7}";
    const nevr_token_auth::DevicePollResponse response = nevr_token_auth::ParseDevicePollResponse(body);
    EXPECT_EQ(response.status, nevr_token_auth::DevicePollStatus::Error) << field;
    EXPECT_TRUE(response.access_token.empty()) << field;
    EXPECT_TRUE(response.refresh_token.empty()) << field;
    EXPECT_TRUE(response.user_id.empty()) << field;
    EXPECT_TRUE(response.username.empty()) << field;
  }

  const nevr_token_auth::DevicePollResponse malformedLegacyToken = nevr_token_auth::ParseDevicePollResponse(
      "{\"status\":\"verified\",\"token\":7,\"refresh_token\":\"refresh\","
      "\"user_id\":\"user\",\"username\":\"name\"}");
  EXPECT_EQ(malformedLegacyToken.status, nevr_token_auth::DevicePollStatus::Error);
  EXPECT_TRUE(malformedLegacyToken.access_token.empty());
  EXPECT_TRUE(malformedLegacyToken.refresh_token.empty());

  const nevr_token_auth::DevicePollResponse wrongExpiry = nevr_token_auth::ParseDevicePollResponse(
      "{\"status\":\"verified\",\"access_token\":\"access\",\"expires_in\":\"3600\","
      "\"refresh_token_expires_in\":false}");
  EXPECT_EQ(wrongExpiry.status, nevr_token_auth::DevicePollStatus::Verified);
  EXPECT_FALSE(wrongExpiry.expires_in.has_value());
  EXPECT_FALSE(wrongExpiry.refresh_token_expires_in.has_value());
}

// The poll response carries the RFC 6749 §5.1 field `access_token` and the deprecated `token`.
// The server sends access_token and token with the same value, so a test where
// they are EQUAL cannot tell "read the new name" from "read the old one". They
// differ here specifically so preference is observable.
TEST(DevicePollResponse, PrefersRfcAccessTokenOverDeprecatedToken) {
  const nevr_token_auth::DevicePollResponse response = nevr_token_auth::ParseDevicePollResponse(
      "{\"status\":\"verified\",\"access_token\":\"rfc\",\"token\":\"deprecated\","
      "\"token_type\":\"Bearer\",\"expires_in\":3600,\"refresh_token\":\"refresh\","
      "\"refresh_token_expires_in\":2592000,\"user_id\":\"user\",\"username\":\"name\"}");

  EXPECT_EQ(response.status, nevr_token_auth::DevicePollStatus::Verified);
  EXPECT_EQ(response.access_token, "rfc");
  ASSERT_TRUE(response.expires_in.has_value());
  EXPECT_EQ(*response.expires_in, 3600U);
  ASSERT_TRUE(response.refresh_token_expires_in.has_value());
  EXPECT_EQ(*response.refresh_token_expires_in, 2592000U);
}

// A nakama older than 80578f3eb (EchoTools/nakama) sends neither access_token
// nor refresh_token_expires_in. A client that reads
// only the RFC names authenticates against nothing there.
TEST(DevicePollResponse, LegacyServerWithoutRfcFieldsStillAuthenticates) {
  const nevr_token_auth::DevicePollResponse response = nevr_token_auth::ParseDevicePollResponse(
      "{\"status\":\"verified\",\"token\":\"legacy\",\"refresh_token\":\"refresh\","
      "\"user_id\":\"user\",\"username\":\"name\"}");

  EXPECT_EQ(response.status, nevr_token_auth::DevicePollStatus::Verified);
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
  const nevr_token_auth::DevicePollResponse response = nevr_token_auth::ParseDevicePollResponse(
      "{\"status\":\"verified\",\"access_token\":\"tok\",\"expires_in\":-1757300000,"
      "\"refresh_token\":\"refresh\",\"refresh_token_expires_in\":-1757300000}");

  ASSERT_EQ(response.status, nevr_token_auth::DevicePollStatus::Verified);
  EXPECT_FALSE(response.expires_in.has_value());
  EXPECT_FALSE(response.refresh_token_expires_in.has_value());

  constexpr uint64_t kNow = 1000;
  EXPECT_EQ(nevr_token_auth::ResolveAccessTokenExpiry(kNow, response.access_token, response.expires_in),
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

// The refresh request body is built by nevr::auth::BuildRefreshBody (core/auth_refresh.h):
// the RFC 6749 field alone (EchoTools/nakama 80578f3eb reads it).
TEST(RefreshRequestBody, CarriesOnlyTheRfcFieldName) {
  const nlohmann::json body = nlohmann::json::parse(nevr::auth::BuildRefreshBody("rt"));

  EXPECT_EQ(body, nlohmann::json({{"refresh_token", "rt"}}));
}

// Both ways of obtaining an access token must agree about when it dies. The
// refresh path must not hardcode now+60 while the poll path honours the JWT.
TEST(AccessTokenExpiry, RefreshAndPollPathsShareOneAuthorityOrder) {
  constexpr uint64_t kNow = 1000;
  const std::string jwt = MakeJwt("eyJleHAiOjUwMDB9");

  EXPECT_EQ(ResolveAccessTokenExpirySec(kNow, jwt, 10),
            nevr_token_auth::ResolveAccessTokenExpiry(kNow, jwt, 10));
  EXPECT_EQ(ResolveAccessTokenExpirySec(kNow, jwt, 10), 5000U);
  // No decodable exp: the server's expires_in is next, not a fixed 60 seconds.
  EXPECT_EQ(ResolveAccessTokenExpirySec(kNow, "opaque", 3600), kNow + 3600U);
  EXPECT_EQ(ResolveAccessTokenExpirySec(kNow, "opaque", std::nullopt),
            kNow + kFallbackAccessTokenLifetimeSec);
}

TEST(DevicePollResponse, JwtExpiryTakesPrecedenceThenFallsBack) {
  constexpr uint64_t kNow = 1000;
  const std::string jwt = MakeJwt("eyJleHAiOjUwMDB9");
  EXPECT_EQ(nevr_token_auth::ResolveAccessTokenExpiry(kNow, jwt, 10), 5000U);
  EXPECT_EQ(nevr_token_auth::ResolveAccessTokenExpiry(kNow, "not-a-jwt", 10), 1010U);
  EXPECT_EQ(nevr_token_auth::ResolveAccessTokenExpiry(kNow, "not-a-jwt", std::nullopt),
            kNow + kFallbackAccessTokenLifetimeSec);
}

namespace {

using FlowClock = nevr_token_auth::test_hook::DeviceAuthFlowOps::Clock;

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
  nevr_token_auth::DevicePollResponse poll_response;
  std::vector<nevr_token_auth::DevicePollResponse> poll_sequence;
  size_t poll_sequence_index = 0;
  std::vector<FlowClock::duration> sleep_durations;
  std::vector<std::pair<EchoVR::LogLevel, std::string>> logs;
  std::vector<std::pair<std::string, std::string>> issued;  // (code, login url) the player was shown
  std::vector<nevr::auth::FlowEnd> ends;

  nevr_token_auth::test_hook::DeviceAuthFlowOps Ops() {
    nevr_token_auth::test_hook::DeviceAuthFlowOps ops;
    ops.on_code_issued = [this](const std::string& issuedCode, const std::string& url) {
      issued.emplace_back(issuedCode, url);
    };
    ops.on_end = [this](nevr::auth::FlowEnd end) { ends.push_back(end); };
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

nevr_token_auth::test_hook::DeviceAuthState ExistingDeviceAuthState() {
  nevr_token_auth::test_hook::DeviceAuthState state;
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

void ExpectSameDeviceAuthState(const nevr_token_auth::test_hook::DeviceAuthState& actual,
                               const nevr_token_auth::test_hook::DeviceAuthState& expected) {
  EXPECT_EQ(actual.authenticated, expected.authenticated);
  EXPECT_EQ(actual.token, expected.token);
  EXPECT_EQ(actual.token_expiry, expected.token_expiry);
  EXPECT_EQ(actual.refresh_token, expected.refresh_token);
  EXPECT_EQ(actual.refresh_token_expiry, expected.refresh_token_expiry);
  EXPECT_EQ(actual.user_id, expected.user_id);
  EXPECT_EQ(actual.username, expected.username);
  EXPECT_EQ(actual.discord_id, expected.discord_id);
}

nevr_token_auth::DevicePollResponse VerifiedPollResponse() {
  return nevr_token_auth::ParseDevicePollResponse(
      "{\"status\":\"verified\",\"access_token\":\"" +
      MakeJwt("eyJ2cnMiOnsiZGlkIjoiNDIifSwiZXhwIjo0MTAyNDQ0ODAwfQ") +
      "\",\"refresh_token\":\"new-refresh\",\"user_id\":\"new-user\","
      "\"username\":\"new-name\",\"expires_in\":3600,\"refresh_token_expires_in\":7200}");
}

}  // namespace

TEST(DeviceAuthFlow, ServerRefusesBeforeAnyHttpBrowserUiOrPollOperation) {
  FakeDeviceAuthFlow fake;
  const auto original = ExistingDeviceAuthState();
  const auto result = nevr_token_auth::test_hook::RunDeviceAuthFlow(true, original, fake.Ops());

  EXPECT_FALSE(result.success);
  ExpectSameDeviceAuthState(result.state, original);
  EXPECT_EQ(fake.request_calls, 0);
  EXPECT_EQ(fake.browser_calls, 0);
  EXPECT_EQ(fake.ui_calls, 0);
  EXPECT_EQ(fake.poll_calls, 0);
  EXPECT_EQ(fake.save_calls, 0);
}

// ---- #1532: a server with no auth.password signs in by device code ----------------------------------------

// The same flow a client runs, in server mode: the code is requested, shown once through the presenter, the poll
// answers verified, the token is kept and the refresh token saved. No browser box is raised for a host with no screen.
TEST(DeviceAuthFlow, AServerWithNoPasswordRunsTheFlowAndKeepsTheToken) {
  FakeDeviceAuthFlow fake;
  fake.poll_response = VerifiedPollResponse();
  auto ops = fake.Ops();
  ops.server_sign_in = true;
  const auto result = nevr_token_auth::test_hook::RunDeviceAuthFlow(true, ExistingDeviceAuthState(), ops);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.state.refresh_token, "new-refresh");
  EXPECT_EQ(fake.request_calls, 1);
  EXPECT_EQ(fake.ui_calls, 0);
  EXPECT_EQ(fake.save_calls, 1);
  ASSERT_EQ(fake.issued.size(), 1U);
  EXPECT_EQ(fake.issued[0].first, fake.code);
  ASSERT_EQ(fake.ends.size(), 1U);
  EXPECT_EQ(fake.ends[0], nevr::auth::FlowEnd::Verified);
}

// The password path: a server that has one never asks for a code (the default of the hook's flag is the refusal).
TEST(DeviceAuthFlow, AServerWithAPasswordStillRunsNoFlow) {
  FakeDeviceAuthFlow fake;
  fake.poll_response = VerifiedPollResponse();
  const auto result = nevr_token_auth::test_hook::RunDeviceAuthFlow(true, ExistingDeviceAuthState(), fake.Ops());
  EXPECT_FALSE(result.success);
  EXPECT_EQ(fake.request_calls, 0);
  EXPECT_TRUE(fake.issued.empty());
}

TEST(ServerSignInPrompt, TheConsoleLineCarriesTheCodeTheFullPageAndTheExpiry) {
  const std::string line = nevr_token_auth::ServerSignInPromptLine("K7RX", "https://echovrce.com/device");
  const nlohmann::json parsed = nlohmann::json::parse(line);
  EXPECT_EQ(parsed.at("tag"), "NEVR.AUTH");
  EXPECT_EQ(parsed.at("event"), "server_sign_in_code");
  EXPECT_EQ(parsed.at("code"), "K7RX");
  EXPECT_EQ(parsed.at("url"), "https://echovrce.com/device?code=K7RX");
  EXPECT_EQ(parsed.at("expires_in_s"), 300);
  EXPECT_NE(parsed.at("msg").get<std::string>().find("K7RX"), std::string::npos);
  EXPECT_EQ(line.find('\n'), std::string::npos);
}

namespace {
struct RenewHarness {
  std::vector<nevr::auth::FlowEnd> script;  // what each attempt does, in order; the last is repeated
  size_t attempts = 0;
  std::vector<FlowClock::duration> sleeps;
  std::vector<std::pair<nevr::auth::LogLevel, std::string>> logs;
  nevr::auth::RenewPolicy policy;

  bool Run() {
    return nevr::auth::RunUntilApproved(
        [this]() {
          const nevr::auth::FlowEnd end = script[std::min(attempts, script.size() - 1)];
          ++attempts;
          nevr::auth::SignInAttempt attempt;
          attempt.verified = end == nevr::auth::FlowEnd::Verified;
          attempt.end = end;
          return attempt;
        },
        [this](FlowClock::duration d) { sleeps.push_back(d); },
        [this](nevr::auth::LogLevel level, const std::string& message) { logs.emplace_back(level, message); }, policy);
  }
};
}  // namespace

// A code that runs out is not the end of the wait: the server asks for a fresh one, as long as the host takes.
TEST(ServerSignInWait, ACodeThatRunsOutIsReplacedUntilTheHostApproves) {
  RenewHarness h;
  h.script = {nevr::auth::FlowEnd::TimedOut, nevr::auth::FlowEnd::CodeExpired, nevr::auth::FlowEnd::TimedOut,
              nevr::auth::FlowEnd::TimedOut, nevr::auth::FlowEnd::TimedOut, nevr::auth::FlowEnd::Verified};
  EXPECT_TRUE(h.Run());
  EXPECT_EQ(h.attempts, 6U);
  EXPECT_TRUE(h.sleeps.empty());
}

// A service that does not answer ends the wait after the policy's failures in a row, with a pause between tries; an
// expiry in between resets the count.
TEST(ServerSignInWait, ASilentServiceEndsTheWaitAfterTheFailureLimit) {
  RenewHarness h;
  h.script = {nevr::auth::FlowEnd::NoCode};
  EXPECT_FALSE(h.Run());
  EXPECT_EQ(h.attempts, 3U);
  EXPECT_EQ(h.sleeps.size(), 2U);
  EXPECT_EQ(h.logs.back().first, nevr::auth::LogLevel::Error);
}

TEST(ServerSignInWait, AnExpiryBetweenFailuresResetsTheCount) {
  RenewHarness h;
  h.script = {nevr::auth::FlowEnd::NoCode, nevr::auth::FlowEnd::NoCode, nevr::auth::FlowEnd::TimedOut,
              nevr::auth::FlowEnd::NoCode, nevr::auth::FlowEnd::Verified};
  EXPECT_TRUE(h.Run());
  EXPECT_EQ(h.attempts, 5U);
}

TEST(ServerSignInWait, ACancelledWaitIsNotRetried) {
  RenewHarness h;
  h.script = {nevr::auth::FlowEnd::Cancelled};
  EXPECT_FALSE(h.Run());
  EXPECT_EQ(h.attempts, 1U);
}

// ---- #397: what the player is shown while sign-in is pending ----------------------------------------

namespace {
bool LogHas(const FakeDeviceAuthFlow& fake, EchoVR::LogLevel level, const std::string& needle) {
  return std::any_of(fake.logs.begin(), fake.logs.end(), [&](const auto& entry) {
    return entry.first == level && entry.second.find(needle) != std::string::npos;
  });
}
}  // namespace

TEST(SignInDialogText, WaitingTextNamesThePageTheCodeAndTheBrowser) {
  const auto c = nevr::auth::SignInWaitingContent("https://echovrce.com/device", "ABCD-1234");
  EXPECT_EQ(c.title, "Echo VR - sign in");
  EXPECT_EQ(c.code, "ABCD-1234");
  EXPECT_FALSE(c.closes_by_itself);
  EXPECT_EQ(c.instruction,
            "Sign in to play: on a phone or computer, open\nechovrce.com/device\nand enter the code below.\n"
            "Your browser was also opened to that page. The code lasts 5 minutes.");
  // What the player types has no scheme and no query: the code is never part of the address line.
  EXPECT_EQ(nevr::auth::SignInDisplayUrl("http://localhost:7350/login/device"), "localhost:7350/login/device");
  EXPECT_EQ(nevr::auth::SignInDisplayUrl("echovrce.com/device"), "echovrce.com/device");
}

TEST(SignInDialogText, EveryEndingSaysWhichOneAndDropsTheCode) {
  using nevr::auth::FlowEnd;
  std::vector<std::string> seen;
  for (const FlowEnd end : {FlowEnd::Verified, FlowEnd::NoCode, FlowEnd::BrowserFailed, FlowEnd::CodeExpired,
                            FlowEnd::TimedOut, FlowEnd::PollErrors, FlowEnd::Cancelled}) {
    const auto c = nevr::auth::SignInEndedContent(end);
    EXPECT_FALSE(c.instruction.empty());
    EXPECT_TRUE(c.code.empty()) << "the code is not shown once the wait has ended";
    seen.push_back(c.instruction);
    // Only a success (or the game closing) vanishes on its own; a failure stays until it is read.
    EXPECT_EQ(c.closes_by_itself, end == FlowEnd::Verified || end == FlowEnd::Cancelled);
  }
  std::sort(seen.begin(), seen.end());
  // Every ending reads differently: a timeout names itself, and an expired code names the code.
  EXPECT_EQ(std::unique(seen.begin(), seen.end()) - seen.begin(), 7);
  EXPECT_EQ(nevr::auth::SignInEndedContent(FlowEnd::Verified).instruction, "Signed in. Starting Echo VR.");
  EXPECT_NE(nevr::auth::SignInEndedContent(FlowEnd::CodeExpired).instruction.find("expired"), std::string::npos);
}

// #1446: a timeout is what the player must act on, so the window says it in one line.
TEST(SignInDialogText, ATimedOutSignInSaysSoInOneLine) {
  EXPECT_EQ(nevr::auth::SignInEndedContent(nevr::auth::FlowEnd::TimedOut).instruction,
            "Sign-in timed out. Restart Echo VR to get a new code.");
}

// #1446: a failure stays on screen until the player closes it; only a success or a cancel closes after a moment.
TEST(SignInDialogText, AFailedEndingStaysUntilThePlayerClosesIt) {
  using nevr::auth::FlowEnd;
  for (const FlowEnd end : {FlowEnd::CodeExpired, FlowEnd::TimedOut, FlowEnd::PollErrors, FlowEnd::NoCode,
                            FlowEnd::BrowserFailed}) {
    EXPECT_EQ(nevr::auth::SignInEndedLingerMs(nevr::auth::SignInEndedContent(end)), 0u)
        << "ending " << static_cast<int>(end) << " must not close itself";
  }
  EXPECT_EQ(nevr::auth::SignInEndedLingerMs(nevr::auth::SignInEndedContent(FlowEnd::Verified)), 1500u);
  EXPECT_EQ(nevr::auth::SignInEndedLingerMs(nevr::auth::SignInEndedContent(FlowEnd::Cancelled)), 1500u);
  EXPECT_EQ(nevr::auth::SignInEndedLingerMs(nevr::auth::SignInWaitingContent("https://echovrce.com/device", "K7RX")),
            0u)
      << "the waiting dialog has no end to close after";
}

TEST(DeviceAuthFlow, TheCodeAndPageAreHandedToTheUiBeforeTheBrowserAndTheEndIsReportedOnce) {
  using nevr::auth::FlowEnd;
  struct Case {
    const char* name;
    std::function<void(FakeDeviceAuthFlow&)> arrange;
    FlowEnd expected;
  };
  const std::vector<Case> cases = {
      {"verified", [](FakeDeviceAuthFlow& f) { f.poll_response = VerifiedPollResponse(); }, FlowEnd::Verified},
      {"expired",
       [](FakeDeviceAuthFlow& f) {
         f.poll_response = nevr_token_auth::ParseDevicePollResponse("{\"status\":\"expired\"}");
       },
       FlowEnd::CodeExpired},
      {"deadline",
       [](FakeDeviceAuthFlow& f) {
         f.poll_response = nevr_token_auth::ParseDevicePollResponse("{\"status\":\"authorization_pending\"}");
       },
       FlowEnd::TimedOut},
      {"poll errors", [](FakeDeviceAuthFlow&) {}, FlowEnd::PollErrors},
      {"browser failed",
       [](FakeDeviceAuthFlow& f) {
         f.browser_result = 0;
         f.ui_result = 0;
       },
       FlowEnd::BrowserFailed},
  };
  for (const Case& c : cases) {
    FakeDeviceAuthFlow fake;
    c.arrange(fake);
    (void)nevr_token_auth::test_hook::RunDeviceAuthFlow(false, ExistingDeviceAuthState(), fake.Ops());
    ASSERT_EQ(fake.issued.size(), 1U) << c.name;
    EXPECT_EQ(fake.issued[0].first, fake.code) << c.name;
    EXPECT_EQ(fake.issued[0].second, "https://echovrce.com/device") << c.name;
    ASSERT_EQ(fake.ends.size(), 1U) << c.name;
    EXPECT_EQ(fake.ends[0], c.expected) << c.name;
  }
}

// #394: a 4-character dashless code (nakama server/evr_device_auth.go) goes through the whole PC flow as
// issued: the browser URL, the dialog, the masked log.
TEST(DeviceAuthFlow, AFourCharacterDashlessCodeIsUsedAsIssued) {
  FakeDeviceAuthFlow fake;
  fake.code = "K7RX";
  fake.browser_result = 33;
  fake.poll_response = VerifiedPollResponse();
  const auto flow = nevr_token_auth::test_hook::RunDeviceAuthFlow(false, ExistingDeviceAuthState(), fake.Ops());
  EXPECT_TRUE(flow.success);
  EXPECT_EQ(fake.browser_url, "https://echovrce.com/device?code=K7RX");
  ASSERT_EQ(fake.issued.size(), 1U);
  EXPECT_EQ(fake.issued[0].first, "K7RX");
  EXPECT_TRUE(LogHas(fake, EchoVR::LogLevel::Info, "<4 chars masked>"));
  for (const auto& [level, message] : fake.logs) {
    (void)level;
    EXPECT_EQ(message.find("K7RX"), std::string::npos) << message;
  }
  const auto shown = nevr::auth::SignInWaitingContent("https://echovrce.com/device", "K7RX");
  EXPECT_EQ(shown.code, "K7RX");
  EXPECT_NE(shown.instruction.find("echovrce.com/device"), std::string::npos);
}

TEST(DeviceAuthFlow, NoCodeIsReportedAndAServerShowsNothing) {
  FakeDeviceAuthFlow noCode;
  noCode.code.clear();
  (void)nevr_token_auth::test_hook::RunDeviceAuthFlow(false, ExistingDeviceAuthState(), noCode.Ops());
  EXPECT_TRUE(noCode.issued.empty());
  ASSERT_EQ(noCode.ends.size(), 1U);
  EXPECT_EQ(noCode.ends[0], nevr::auth::FlowEnd::NoCode);

  FakeDeviceAuthFlow server;
  (void)nevr_token_auth::test_hook::RunDeviceAuthFlow(true, ExistingDeviceAuthState(), server.Ops());
  EXPECT_TRUE(server.issued.empty());
  EXPECT_TRUE(server.ends.empty());
}

// The wait used to be silent while the server answered "pending": a loop that polls and is told pending
// could not be told from one that does not run (#397).
TEST(DeviceAuthFlow, EveryPollIsOneInfoLineWithHttpCodeStatusAndAMaskedBody) {
  FakeDeviceAuthFlow fake;
  nevr_token_auth::DevicePollResponse pending =
      nevr_token_auth::ParseDevicePollResponse("{\"status\":\"authorization_pending\"}");
  pending.http_code = 200;
  pending.body_prefix = nevr_token_auth::PollBodyPrefix("{\"status\":\"authorization_pending\"}", fake.code);
  nevr_token_auth::DevicePollResponse expired = nevr_token_auth::ParseDevicePollResponse("{\"status\":\"expired\"}");
  expired.http_code = 200;
  expired.body_prefix = "{\"status\":\"expired\",\"code\":\"" + fake.code + "\"}";
  expired.body_prefix = nevr_token_auth::PollBodyPrefix(expired.body_prefix, fake.code);
  fake.poll_sequence = {pending, pending, expired};
  (void)nevr_token_auth::test_hook::RunDeviceAuthFlow(false, ExistingDeviceAuthState(), fake.Ops());

  EXPECT_TRUE(LogHas(fake, EchoVR::LogLevel::Info,
                     "poll #1 http=200 status=pending server_status=\"authorization_pending\" "
                     "body=\"{\"status\":\"authorization_pending\"}\""));
  EXPECT_TRUE(LogHas(fake, EchoVR::LogLevel::Info, "poll #2 http=200 status=pending"));
  EXPECT_TRUE(LogHas(fake, EchoVR::LogLevel::Info,
                     "poll #3 http=200 status=expired server_status=\"expired\" "
                     "body=\"{\"status\":\"expired\",\"code\":\"<code>\"}\""));
  for (const auto& [level, message] : fake.logs) {
    (void)level;
    EXPECT_EQ(message.find(fake.code), std::string::npos) << message;
  }
  EXPECT_FALSE(LogHas(fake, EchoVR::LogLevel::Warning, "unknown status"));
}

TEST(DeviceAuthFlow, AnUnknownStatusIsAWarningAndStillPending) {
  FakeDeviceAuthFlow fake;
  nevr_token_auth::DevicePollResponse odd = nevr_token_auth::ParseDevicePollResponse("{\"status\":\"weird\"}");
  odd.http_code = 200;
  odd.body_prefix = "{\"status\":\"weird\"}";
  nevr_token_auth::DevicePollResponse expired = nevr_token_auth::ParseDevicePollResponse("{\"status\":\"expired\"}");
  fake.poll_sequence = {odd, expired};
  (void)nevr_token_auth::test_hook::RunDeviceAuthFlow(false, ExistingDeviceAuthState(), fake.Ops());
  EXPECT_TRUE(LogHas(fake, EchoVR::LogLevel::Warning, "poll answered an unknown status \"weird\""));
  EXPECT_EQ(fake.poll_calls, 2) << "an unknown status is treated as pending: the loop goes on";
  EXPECT_TRUE(nevr_token_auth::IsKnownPollStatus("authorization_pending"));
  EXPECT_TRUE(nevr_token_auth::IsKnownPollStatus("verified"));
  EXPECT_FALSE(nevr_token_auth::IsKnownPollStatus(""));
  EXPECT_FALSE(nevr_token_auth::IsKnownPollStatus("weird"));
}

// A caller's own pending (an outage it waits out) is not the server's answer: no warning per poll.
TEST(DeviceAuthFlow, ASynthesizedPendingIsNotAnUnknownStatus) {
  FakeDeviceAuthFlow fake;
  nevr_token_auth::DevicePollResponse outage;  // what the Quest session returns while the network is down
  outage.status = nevr_token_auth::DevicePollStatus::Pending;
  outage.http_code = 503;
  nevr_token_auth::DevicePollResponse expired = nevr_token_auth::ParseDevicePollResponse("{\"status\":\"expired\"}");
  fake.poll_sequence = {outage, expired};
  (void)nevr_token_auth::test_hook::RunDeviceAuthFlow(false, ExistingDeviceAuthState(), fake.Ops());
  EXPECT_TRUE(LogHas(fake, EchoVR::LogLevel::Info, "poll #1 http=503 status=pending"));
  EXPECT_FALSE(LogHas(fake, EchoVR::LogLevel::Warning, "unknown status"));
}

// #404 review: the window can appear after the caller gave up waiting for it, and then nothing closed it.
TEST(SignInDialogLifecycle, AWindowCreatedAfterTheWaitTimedOutIsToldToCloseAtOnce) {
  nevr::auth::SignInDialogLifecycle lifecycle;
  EXPECT_TRUE(lifecycle.Abandon()) << "the window does not exist yet: it must close itself when it does";
  EXPECT_TRUE(lifecycle.OnCreated());
}

TEST(SignInDialogLifecycle, AWindowThatExistsWhenTheCallerLetsGoIsClosedDirectly) {
  nevr::auth::SignInDialogLifecycle lifecycle;
  EXPECT_FALSE(lifecycle.OnCreated()) << "wanted: it stays up";
  EXPECT_FALSE(lifecycle.Abandon()) << "it exists: the caller closes it, nothing is deferred";
  EXPECT_TRUE(lifecycle.created());
  EXPECT_TRUE(lifecycle.abandoned());
}

TEST(DeviceAuthFlow, ATransportFailureIsLoggedAsHttpNoneNotSilence) {
  FakeDeviceAuthFlow fake;  // default response: Error, http_code 0
  (void)nevr_token_auth::test_hook::RunDeviceAuthFlow(false, ExistingDeviceAuthState(), fake.Ops());
  EXPECT_TRUE(LogHas(fake, EchoVR::LogLevel::Info, "poll #1 http=none status=error"));
  EXPECT_FALSE(LogHas(fake, EchoVR::LogLevel::Warning, "unknown status")) << "an error is not an unknown status";
}

TEST(PollBodyPrefix, MasksTheCodeCapsTheLengthAndNeverEchoesTokens) {
  EXPECT_EQ(nevr_token_auth::PollBodyPrefix("{\"code\":\"SECRET-1\",\"status\":\"x\"}", "SECRET-1"),
            "{\"code\":\"<code>\",\"status\":\"x\"}");
  EXPECT_EQ(nevr_token_auth::PollBodyPrefix(std::string(500, 'a'), "zzz").size(), 120U);
  EXPECT_EQ(nevr_token_auth::PollBodyPrefix(std::string(500, 'a'), "zzz", 10).size(), 10U);
  EXPECT_EQ(nevr_token_auth::PollBodyPrefix("a\nb\x01", ""), "a.b.");
  const std::string verified =
      "{\"status\":\"verified\",\"access_token\":\"eyJhbGciOiJIUzI1NiJ9.payload.sig\",\"refresh_token\":\"r\"}";
  const std::string described = nevr_token_auth::PollBodyPrefix(verified, "SECRET-1");
  EXPECT_EQ(described.find("eyJ"), std::string::npos);
  EXPECT_EQ(described.find("access_token"), std::string::npos);
  EXPECT_NE(described.find(std::to_string(verified.size())), std::string::npos);
  EXPECT_EQ(nevr_token_auth::PollBodyPrefix("{\"token\":\"legacy-secret\"}", "").find("legacy-secret"),
            std::string::npos);
}

TEST(DeviceAuthFlow, BrowserResultBoundaryUsesTransientUiOnlyForZeroAndThirtyTwo) {
  for (const intptr_t resultCode : {0, 32, 33}) {
    FakeDeviceAuthFlow fake;
    fake.browser_result = resultCode;
    fake.ui_result = 0;
    const auto flow = nevr_token_auth::test_hook::RunDeviceAuthFlow(false, ExistingDeviceAuthState(), fake.Ops());

    EXPECT_FALSE(flow.success) << resultCode;
    EXPECT_EQ(fake.browser_calls, 1) << resultCode;
    EXPECT_NE(fake.browser_url.find("device-code-secret-sentinel"), std::string::npos) << resultCode;
    EXPECT_EQ(fake.ui_calls, resultCode > 32 ? 0 : 1) << resultCode;
    if (resultCode <= 32) {
      EXPECT_EQ(fake.ui_code, fake.code);
      EXPECT_EQ(fake.ui_url, "https://echovrce.com/device");
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
    fake.poll_response.status = nevr_token_auth::DevicePollStatus::Pending;
    const auto flow = nevr_token_auth::test_hook::RunDeviceAuthFlow(false, ExistingDeviceAuthState(), fake.Ops());

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

  const auto result = nevr_token_auth::test_hook::RunDeviceAuthFlow(false, original, fake.Ops());

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
      fake.poll_response = nevr_token_auth::ParseDevicePollResponse(body);
      fake.poll_elapsed = std::chrono::seconds(elapsedSeconds - 3);
      const auto original = ExistingDeviceAuthState();
      const auto flow = nevr_token_auth::test_hook::RunDeviceAuthFlow(false, original, fake.Ops());

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
    const auto flow = nevr_token_auth::test_hook::RunDeviceAuthFlow(false, original, fake.Ops());

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
  const auto result = nevr_token_auth::test_hook::RunDeviceAuthFlow(false, ExistingDeviceAuthState(), fake.Ops());

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
  const auto success = nevr_token_auth::test_hook::RunDeviceAuthFlow(false, original, timely.Ops());
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

  const auto result = nevr_token_auth::test_hook::RunDeviceAuthFlow(false, ExistingDeviceAuthState(), fake.Ops());

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
  (void)nevr_token_auth::test_hook::RunDeviceAuthFlow(false, ExistingDeviceAuthState(), uiFailure.Ops());

  FakeDeviceAuthFlow polling;
  polling.browser_result = 33;
  for (unsigned int index = 0; index < 10; ++index) {
    nevr_token_auth::DevicePollResponse pending;
    pending.status = nevr_token_auth::DevicePollStatus::Pending;
    polling.poll_sequence.push_back(pending);
  }
  nevr_token_auth::DevicePollResponse error;
  error.status = nevr_token_auth::DevicePollStatus::Error;
  polling.poll_sequence.push_back(error);
  (void)nevr_token_auth::test_hook::RunDeviceAuthFlow(false, ExistingDeviceAuthState(), polling.Ops());

  EXPECT_EQ(polling.browser_url, "https://echovrce.com/device?code=device-code-secret-sentinel");
  EXPECT_EQ(uiFailure.ui_code, uiFailure.code);
  EXPECT_EQ(uiFailure.ui_url, "https://echovrce.com/device");
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
  fake.poll_response = nevr_token_auth::ParseDevicePollResponse("{\"status\":\"authorization_pending\"}");
  int wakeups = 0;
  auto ops = fake.Ops();
  ops.cancelled = [&wakeups]() { return ++wakeups >= 3; };  // cancelled at the third wake-up
  const auto original = ExistingDeviceAuthState();
  const auto result = nevr_token_auth::test_hook::RunDeviceAuthFlow(false, original, ops);

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
  const auto result = nevr_token_auth::test_hook::RunDeviceAuthFlow(false, ExistingDeviceAuthState(), fake.Ops());
  EXPECT_TRUE(result.success);
  EXPECT_EQ(fake.save_calls, 1);
}

// The sign-in wait (#37, G7): the flow runs on a worker while the caller keeps pumping, so the
// caller's window stays responsive; the caller still gets the flow's result before it continues.
TEST(OffThreadWait, FlowRunsOnAWorkerWhileTheCallerPumps) {
  std::atomic<int> pumps{0};
  const auto r = nevr_token_auth::RunWhilePumping(
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
  const auto failed = nevr_token_auth::RunWhilePumping([]() { return false; }, []() {}, std::chrono::milliseconds(1));
  EXPECT_FALSE(failed.flowResult);
  EXPECT_FALSE(failed.flowThrew);
  const auto threw = nevr_token_auth::RunWhilePumping(
      []() -> bool { throw std::runtime_error("flow failed"); }, []() {}, std::chrono::milliseconds(1));
  EXPECT_FALSE(threw.flowResult);
  EXPECT_TRUE(threw.flowThrew);
}

// A flow that finishes before the first interval elapses needs no pumping at all.
TEST(OffThreadWait, QuickFlowNeedsNoPump) {
  int pumps = 0;
  const auto r = nevr_token_auth::RunWhilePumping([]() { return true; }, [&pumps]() { ++pumps; },
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
  nevr_token_auth::DevicePollResponse error;
  error.status = nevr_token_auth::DevicePollStatus::Error;
  fake.poll_sequence = {error, error, error, error};
  fake.poll_response = VerifiedPollResponse();

  const auto result = nevr_token_auth::test_hook::RunDeviceAuthFlow(false, ExistingDeviceAuthState(), fake.Ops());

  EXPECT_TRUE(result.success);
  EXPECT_EQ(fake.poll_calls, 5);
  EXPECT_EQ(fake.save_calls, 1);
}

TEST(DeviceAuthFlow, ConsecutivePollErrorsEndTheWaitAfterTheLimit) {
  FakeDeviceAuthFlow fake;
  fake.browser_result = 33;
  fake.poll_response.status = nevr_token_auth::DevicePollStatus::Error;

  const auto result = nevr_token_auth::test_hook::RunDeviceAuthFlow(false, ExistingDeviceAuthState(), fake.Ops());

  EXPECT_FALSE(result.success);
  EXPECT_EQ(fake.poll_calls, 5);
  EXPECT_EQ(fake.save_calls, 0);
}

TEST(DeviceAuthFlow, AnAnsweredPollResetsTheErrorRun) {
  FakeDeviceAuthFlow fake;
  fake.browser_result = 33;
  nevr_token_auth::DevicePollResponse error;
  error.status = nevr_token_auth::DevicePollStatus::Error;
  nevr_token_auth::DevicePollResponse pending;
  pending.status = nevr_token_auth::DevicePollStatus::Pending;
  fake.poll_sequence = {error, error, error, error, pending, error, error, error, error};
  fake.poll_response = VerifiedPollResponse();

  const auto result = nevr_token_auth::test_hook::RunDeviceAuthFlow(false, ExistingDeviceAuthState(), fake.Ops());

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
    fake.poll_response = nevr_token_auth::ParseDevicePollResponse(body);
    ASSERT_EQ(fake.poll_response.status, nevr_token_auth::DevicePollStatus::Error);
    const auto result = nevr_token_auth::test_hook::RunDeviceAuthFlow(false, original, fake.Ops());

    EXPECT_FALSE(result.success) << body;
    EXPECT_EQ(fake.save_calls, 0) << body;
    ExpectSameDeviceAuthState(result.state, original);
  }
}

TEST(TokenAuthModule, ServerHostSkipsDeviceAuthentication) {
  const NvrModuleContext context = MakeModuleContext(NEVR_MODULE_HOST_IS_SERVER);

  EXPECT_EQ(token_auth_Init(&context), 0);
  const auto snapshot = nevr_token_auth::GetAuthSnapshot();
  ASSERT_NE(snapshot, nullptr);
  EXPECT_EQ(snapshot->readiness, nevr_token_auth::AuthReadiness::Disabled);
  EXPECT_TRUE(nevr_token_auth::GetToken().empty());
  EXPECT_EQ(nevr_token_auth::GetDiscordId(), 0U);
  EXPECT_TRUE(nevr_token_auth::GetUsername().empty());
  token_auth_Shutdown();
  EXPECT_EQ(nevr_token_auth::GetAuthSnapshot()->readiness, nevr_token_auth::AuthReadiness::Disabled);
}

TEST(TokenAuthModule, ClientWithoutRequiredConfigDisablesCleanly) {
  const NvrModuleContext context = MakeModuleContext(NEVR_MODULE_HOST_IS_CLIENT);

  EXPECT_EQ(token_auth_Init(&context), 0);
  const auto snapshot = nevr_token_auth::GetAuthSnapshot();
  ASSERT_NE(snapshot, nullptr);
  EXPECT_EQ(snapshot->readiness, nevr_token_auth::AuthReadiness::Disabled);
  EXPECT_TRUE(nevr_token_auth::GetToken().empty());
  EXPECT_EQ(nevr_token_auth::GetDiscordId(), 0U);
  EXPECT_TRUE(nevr_token_auth::GetUsername().empty());
  // #1446: a client that never ran sign-in has not ended it, so the bridge keeps asking the service as before.
  EXPECT_FALSE(nevr_token_auth::SignInEndedWithoutToken());
  token_auth_Shutdown();
}

// #1446: the real function, driven through each published readiness. Only Failed means a sign-in ended without a token.
TEST(SignInEndedWithoutTokenState, OnlyAFailedSnapshotMeansASignInEndedWithoutAToken) {
  using nevr_token_auth::AuthReadiness;
  const AuthReadiness others[] = {AuthReadiness::Starting,     AuthReadiness::Disabled,      AuthReadiness::Refreshing,
                                  AuthReadiness::AwaitingUser, AuthReadiness::Ready,         AuthReadiness::Expired,
                                  AuthReadiness::Stopping};
  for (const AuthReadiness readiness : others) {
    nevr_token_auth::test_hook::PublishReadinessForTest(readiness);
    EXPECT_FALSE(nevr_token_auth::SignInEndedWithoutToken()) << "readiness " << static_cast<int>(readiness);
  }
  nevr_token_auth::test_hook::PublishReadinessForTest(AuthReadiness::Failed);
  EXPECT_TRUE(nevr_token_auth::SignInEndedWithoutToken());
  nevr_token_auth::test_hook::PublishReadinessForTest(AuthReadiness::Disabled);  // the snapshot the module ends in
}

TEST(TokenAuthModule, ReportsThePublishedModuleApiVersion) {
  EXPECT_EQ(token_auth_ApiVersion(), NEVR_MODULE_API_VERSION);
}

TEST(AuthSnapshotStore, PublishedSnapshotIsImmutableAndGetsANewGeneration) {
  nevr_token_auth::AuthSnapshotStore store;
  const auto initial = store.Read();
  ASSERT_NE(initial, nullptr);
  EXPECT_EQ(initial->generation, 0U);

  nevr_token_auth::AuthSnapshot first;
  first.readiness = nevr_token_auth::AuthReadiness::Ready;
  first.access_token = "token-first";
  first.access_expiry = 1000;
  first.discord_id = 11;
  first.user_id = "user-first";
  first.username = "name-first";
  const auto publishedFirst = store.Publish(std::move(first));

  nevr_token_auth::AuthSnapshot second;
  second.readiness = nevr_token_auth::AuthReadiness::Ready;
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
  nevr_token_auth::AuthSnapshotStore store;
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
    nevr_token_auth::AuthSnapshot snapshot;
    snapshot.readiness = nevr_token_auth::AuthReadiness::Ready;
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
  nevr_token_auth::AuthCancellation cancellation;
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

namespace {

// The sign-in cache is the nevr root's (ADR 0012), and the root comes from %LOCALAPPDATA%. The tests below that exercise the
// older search (the install's _local folders) run without one, so they never read or write the cache of whoever runs them.
class NoLocalAppDataEnvironment : public ::testing::Environment {
 public:
  void SetUp() override {
    wchar_t saved[MAX_PATH * 2] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", saved, static_cast<DWORD>(sizeof(saved) / sizeof(saved[0])));
    if (n != 0 && n < sizeof(saved) / sizeof(saved[0])) saved_.assign(saved, n);
    SetEnvironmentVariableW(L"LOCALAPPDATA", nullptr);
  }
  void TearDown() override {
    if (!saved_.empty()) SetEnvironmentVariableW(L"LOCALAPPDATA", saved_.c_str());
  }

 private:
  std::wstring saved_;
};

[[maybe_unused]] const ::testing::Environment* const kNoLocalAppData = ::testing::AddGlobalTestEnvironment(new NoLocalAppDataEnvironment);

// Sets %LOCALAPPDATA% for one test and puts it back.
class ScopedLocalAppData {
 public:
  explicit ScopedLocalAppData(const std::wstring& value) { SetEnvironmentVariableW(L"LOCALAPPDATA", value.c_str()); }
  ~ScopedLocalAppData() { SetEnvironmentVariableW(L"LOCALAPPDATA", nullptr); }
};

}  // namespace

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
  EXPECT_FALSE(std::filesystem::exists(location.credentialsPath + ".tmp")) << "the temp file is renamed away";

  nlohmann::json expected = {{"refresh_token", "refresh-token-v1"},
                             {"refresh_token_expiry", 2100000000U},
                             {"user_id", "account-17"},
                             {"username", "cached-player"}};
  nlohmann::json actual;
  {
    // Closed before the next save: a handle without delete sharing blocks the rename, as it would on Windows.
    std::ifstream saved(location.credentialsPath, std::ios::binary);
    ASSERT_TRUE(saved.is_open());
    actual = nlohmann::json::parse(saved);
  }
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

namespace {
CachedAuthToken* g_midWriteObserved = nullptr;
const std::string* g_midWriteExeDir = nullptr;
void ObserveCacheDuringWrite() {
  if (g_midWriteObserved != nullptr && g_midWriteExeDir != nullptr) {
    *g_midWriteObserved = LoadCachedAuthToken(*g_midWriteExeDir);
  }
}

CachedAuthToken TokenWith(const std::string& refresh) {
  CachedAuthToken auth;
  auth.refresh_token = refresh;
  auth.refresh_token_expiry = 2100000000U;
  auth.user_id = "account-17";
  return auth;
}
}  // namespace

// #562: under Wine the Windows DACL calls do not set the host mode (measured), so the file is protected with
// the host's chmod and the mode is read back; the token is written only after that.
TEST_F(CredentialsDirFixture, WineCacheWriteSetsAndVerifiesHostMode600) {
  if (!nevr_system_info::Get().IsWine()) GTEST_SKIP() << "Wine host permission behavior";
  const auto path = root_ / "bin" / "win10" / "_local" / ".credentials.json";
  std::filesystem::create_directories(path.parent_path());
  ASSERT_TRUE(SaveAuthToken(TokenWith("wine-refresh"), exe_dir_));
  EXPECT_EQ(LoadCachedAuthToken(exe_dir_).refresh_token, "wine-refresh");
  EXPECT_EQ(nevr_credential_write::WineHostMode(path.string()), "600");
  EXPECT_FALSE(std::filesystem::exists(path.string() + ".tmp"));
}

// A crash can leave a temp file with a wrong mode and stale bytes: it is re-protected before being reused.
TEST_F(CredentialsDirFixture, AStaleTempFileDoesNotWeakenTheProtection) {
  if (!nevr_system_info::Get().IsWine()) GTEST_SKIP() << "Wine host permission behavior";
  const auto path = root_ / "bin" / "win10" / "_local" / ".credentials.json";
  Write(path.string() + ".tmp", "stale-bytes-from-a-crashed-write");
  ASSERT_TRUE(SaveAuthToken(TokenWith("after-crash"), exe_dir_));
  EXPECT_EQ(LoadCachedAuthToken(exe_dir_).refresh_token, "after-crash");
  EXPECT_EQ(nevr_credential_write::WineHostMode(path.string()), "600");
  EXPECT_FALSE(std::filesystem::exists(path.string() + ".tmp"));
}

// A leftover, read-only result file holding "600" must never be read as this call's answer: the helper's
// result path must not exist beforehand, and nothing is written when it does.
TEST_F(CredentialsDirFixture, AStaleReadOnlyResultFileWritesNothing) {
  if (!nevr_system_info::Get().IsWine()) GTEST_SKIP() << "Wine host permission behavior";
  const auto path = root_ / "bin" / "win10" / "_local" / ".credentials.json";
  std::filesystem::create_directories(path.parent_path());
  const std::string stale = path.string() + ".stale.mode";
  Write(stale, "600\n");
  ASSERT_NE(SetFileAttributesA(stale.c_str(), FILE_ATTRIBUTE_READONLY), 0);

  nevr_credential_write::g_forceResultPath = stale;
  const bool saved = SaveAuthToken(TokenWith("must-not-land"), exe_dir_);
  nevr_credential_write::g_forceResultPath.clear();
  SetFileAttributesA(stale.c_str(), FILE_ATTRIBUTE_NORMAL);

  EXPECT_FALSE(saved);
  EXPECT_FALSE(std::filesystem::exists(path)) << "a token landed behind a stale 600";
  EXPECT_FALSE(std::filesystem::exists(path.string() + ".tmp"));
  EXPECT_EQ(Read(stale), "600\n") << "the stale file was left as found";
}

// A host chmod that "succeeds" but leaves a mode that is not 600 must stop the write before a byte lands.
TEST_F(CredentialsDirFixture, AHostModeThatIsNot600WritesNothing) {
  if (!nevr_system_info::Get().IsWine()) GTEST_SKIP() << "Wine host permission behavior";
  const auto path = root_ / "bin" / "win10" / "_local" / ".credentials.json";
  std::filesystem::create_directories(path.parent_path());

  nevr_credential_write::g_forcedHostMode = "644";
  const bool saved = SaveAuthToken(TokenWith("must-not-land"), exe_dir_);
  nevr_credential_write::g_forcedHostMode.clear();

  EXPECT_FALSE(saved);
  EXPECT_FALSE(std::filesystem::exists(path));
  EXPECT_FALSE(std::filesystem::exists(path.string() + ".tmp"));
}

namespace {
std::vector<std::string> g_cacheLog;
void CaptureCacheLog(EchoVR::LogLevel, const CHAR* message) { g_cacheLog.push_back(message); }
}  // namespace

// The digests used below are checked against the published test vectors, so the secrets test cannot pass on a wrong hash.
TEST(DigestForTest, MatchesThePublishedVectors) {
  EXPECT_EQ(nevr_test_digest::Sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(nevr_test_digest::Sha1Hex("abc"), "a9993e364706816aba3e25717850c26c9cd0d89d");
  EXPECT_EQ(nevr_test_digest::Sha256Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(nevr_test_digest::Sha1Hex(""), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
}

namespace {
// What a log line must never carry about a secret: its bytes, its length as a number, or a digest of it (L3 of the
// #562 review). `allowed` is text the line is meant to contain (the path), removed before the digits are scanned.
void ExpectNothingOfTheSecret(const std::string& line, const std::string& secret, const std::string& allowed) {
  EXPECT_EQ(line.find(secret), std::string::npos) << "the secret reached the log: " << line;
  for (std::size_t n = 6; n <= 8 && n <= secret.size(); ++n) {  // every piece of 6 to 8 bytes: longer ones contain them
    for (std::size_t at = 0; at + n <= secret.size(); ++at) {
      EXPECT_EQ(line.find(secret.substr(at, n)), std::string::npos) << "a piece of the secret reached the log: " << line;
    }
  }
  std::string scanned = line;
  for (std::size_t at = scanned.find(allowed); !allowed.empty() && at != std::string::npos; at = scanned.find(allowed))
    scanned.erase(at, allowed.size());
  const std::string length = std::to_string(secret.size());
  for (std::size_t i = 0; i < scanned.size();) {
    if (scanned[i] < '0' || scanned[i] > '9') { ++i; continue; }
    std::size_t j = i;
    while (j < scanned.size() && scanned[j] >= '0' && scanned[j] <= '9') ++j;
    EXPECT_NE(scanned.substr(i, j - i), length) << "the secret's length reached the log: " << line;
    i = j;
  }
  for (const std::string& digest : {nevr_test_digest::Sha256Hex(secret), nevr_test_digest::Sha1Hex(secret)}) {
    std::string upper = digest;
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (const std::string& d : {digest, upper}) {
      EXPECT_EQ(line.find(d.substr(0, 16)), std::string::npos) << "a digest of the secret reached the log: " << line;
    }
  }
}
}  // namespace

// #562: a save that wrote the cache is counted and logged at Info with the path and how the file is protected,
// and never with a token byte, its length or a digest of it. (Wine: the host mode was read back as 600.)
TEST_F(CredentialsDirFixture, ASavedCacheIsCountedAndLoggedWithPathAndProtectionButNothingOfTheToken) {
  const auto path = root_ / "bin" / "win10" / "_local" / ".credentials.json";
  std::filesystem::create_directories(path.parent_path());
  nevr_credential_stats::ResetForTest();
  g_cacheLog.clear();
  const std::string token = "sekret-refresh-value-for-the-test-31";  // 36 characters: a length no path digit run equals
  SetBootLogSink(&CaptureCacheLog);
  const bool saved = SaveAuthToken(TokenWith(token), exe_dir_);
  SetBootLogSink(nullptr);
  ASSERT_TRUE(saved);

  const nevr_credential_stats::Snapshot snap = nevr_credential_stats::Get();
  EXPECT_EQ(snap.saved, 1u);
  EXPECT_EQ(snap.refused, 0u);
  EXPECT_FALSE(snap.lastProtection.empty());
  const bool wine = nevr_system_info::Get().IsWine();
  EXPECT_EQ(snap.lastProtection, wine ? "host_mode_600" : "dacl");

  bool found = false;
  for (const std::string& line : g_cacheLog) {
    if (line.find("[NEVR.AUTH] Credentials written to ") != std::string::npos) {
      found = true;
      EXPECT_NE(line.find(path.string()), std::string::npos) << line;
      EXPECT_NE(line.find(std::string("protection=") + snap.lastProtection), std::string::npos) << line;
    }
    ExpectNothingOfTheSecret(line, token, path.string());
  }
  EXPECT_TRUE(found) << "no Info line says the cache was written";
}

// #562: a save that wrote nothing is counted with its reason.
TEST_F(CredentialsDirFixture, ARefusedSaveIsCountedWithItsReason) {
  if (!nevr_system_info::Get().IsWine()) GTEST_SKIP() << "Wine host permission behavior";
  const auto path = root_ / "bin" / "win10" / "_local" / ".credentials.json";
  std::filesystem::create_directories(path.parent_path());
  nevr_credential_stats::ResetForTest();
  nevr_credential_write::g_forcedHostMode = "644";
  const bool saved = SaveAuthToken(TokenWith("must-not-land"), exe_dir_);
  nevr_credential_write::g_forcedHostMode.clear();
  EXPECT_FALSE(saved);
  const nevr_credential_stats::Snapshot snap = nevr_credential_stats::Get();
  EXPECT_EQ(snap.saved, 0u);
  EXPECT_EQ(snap.refused, 1u);
  EXPECT_NE(snap.lastRefusal.find("host mode is 644, not 600"), std::string::npos) << snap.lastRefusal;
}

// #563: a write that fails part-way must leave the previous token exactly as it was.
TEST_F(CredentialsDirFixture, AFailedWriteLeavesTheOldTokenIntact) {
  const auto path = root_ / "bin" / "win10" / "_local" / ".credentials.json";
  std::filesystem::create_directories(path.parent_path());
  ASSERT_TRUE(SaveAuthToken(TokenWith("old-refresh-token"), exe_dir_));
  const std::string before = Read(path);
  ASSERT_FALSE(before.empty());

  nevr_credential_write::g_failAfterBytes = 20;  // inside the JSON, after the first chunks are on disk
  const bool saved = SaveAuthToken(TokenWith("new-refresh-token-that-must-not-land"), exe_dir_);
  nevr_credential_write::g_failAfterBytes = SIZE_MAX;

  EXPECT_FALSE(saved);
  EXPECT_EQ(Read(path), before) << "the failed write changed the cache";
  EXPECT_EQ(LoadCachedAuthToken(exe_dir_).refresh_token, "old-refresh-token");
  EXPECT_FALSE(std::filesystem::exists(path.string() + ".tmp")) << "the temp file is removed on failure";
}

// #563: a reader opening the cache while a write is in flight is never blocked and never sees a partial file.
TEST_F(CredentialsDirFixture, AReaderDuringAWriteSeesTheOldTokenNotABlockedOrPartialFile) {
  const auto path = root_ / "bin" / "win10" / "_local" / ".credentials.json";
  std::filesystem::create_directories(path.parent_path());
  ASSERT_TRUE(SaveAuthToken(TokenWith("old-refresh"), exe_dir_));

  CachedAuthToken observed;
  g_midWriteObserved = &observed;
  g_midWriteExeDir = &exe_dir_;
  nevr_credential_write::g_midWriteHook = &ObserveCacheDuringWrite;
  const bool saved = SaveAuthToken(TokenWith("new-refresh"), exe_dir_);
  nevr_credential_write::g_midWriteHook = nullptr;
  g_midWriteObserved = nullptr;
  g_midWriteExeDir = nullptr;

  ASSERT_TRUE(saved);
  EXPECT_EQ(observed.refresh_token, "old-refresh") << "the reader met a locked or half-written cache";
  EXPECT_EQ(LoadCachedAuthToken(exe_dir_).refresh_token, "new-refresh");
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

// ADR 0012 / #857: the sign-in cache is one file in the nevr root.
TEST_F(CredentialsDirFixture, TheRootsCacheIsSelectedAndOutranksALegacyOne) {
  const std::string nevrRoot = (root_ / "appdata" / "nevr" / "echovr").string();
  Write(root_ / "appdata" / "nevr" / "echovr" / ".credentials.json", Credentials("in-root").dump());
  Write(root_ / "bin" / "win10" / "_local" / ".credentials.json", Credentials("in-local").dump());

  const CredentialCacheLocation location = SelectCredentialCacheLocation(exe_dir_, nevrRoot);
  EXPECT_EQ(std::filesystem::path(location.credentialsPath).lexically_normal(),
            (std::filesystem::path(nevrRoot) / ".credentials.json").lexically_normal());
  EXPECT_EQ(LoadCachedAuthToken(exe_dir_, nevrRoot).refresh_token, "in-root");
}

TEST_F(CredentialsDirFixture, ALegacyCacheTheMoveLeftIsStillReadAndWrittenBackWhereItIs) {
  const std::string nevrRoot = (root_ / "appdata" / "nevr" / "echovr").string();
  const auto legacy = root_ / "bin" / "_local" / ".credentials.json";
  Write(legacy, Credentials("left-behind").dump());

  EXPECT_EQ(LoadCachedAuthToken(exe_dir_, nevrRoot).refresh_token, "left-behind") << "the login is not lost while the move cannot run";
  CachedAuthToken auth;
  auth.refresh_token = "rotated";
  auth.refresh_token_expiry = 2100000000U;
  ASSERT_TRUE(SaveAuthToken(auth, exe_dir_, nevrRoot));
  EXPECT_EQ(LoadCachedAuthToken(exe_dir_, nevrRoot).refresh_token, "rotated");
  EXPECT_NE(Read(legacy).find("rotated"), std::string::npos) << "written where it was found, so one cache exists until the move";
  EXPECT_FALSE(std::filesystem::exists(root_ / "appdata"));
}

TEST_F(CredentialsDirFixture, WithNoCacheAnywhereTheRootIsCreatedAndLocalIsNot) {
  const std::string nevrRoot = (root_ / "appdata" / "nevr" / "echovr").string();
  CachedAuthToken auth;
  auth.refresh_token = "first-login";
  auth.refresh_token_expiry = 2100000000U;
  ASSERT_TRUE(SaveAuthToken(auth, exe_dir_, nevrRoot));
  EXPECT_TRUE(std::filesystem::exists(root_ / "appdata" / "nevr" / "echovr" / ".credentials.json"));
  EXPECT_FALSE(std::filesystem::exists(root_ / "bin" / "win10" / "_local"));
  EXPECT_EQ(LoadCachedAuthToken(exe_dir_, nevrRoot).refresh_token, "first-login");
}

TEST_F(CredentialsDirFixture, WithoutARootTheOlderSearchDecides) {
  Write(root_ / "bin" / "_local" / ".credentials.json", Credentials("legacy").dump());
  EXPECT_EQ(LoadCachedAuthToken(exe_dir_, std::string()).refresh_token, "legacy");
  EXPECT_EQ(SelectCredentialCacheLocation(exe_dir_, std::string()).credentialsPath,
            SelectCredentialCacheLocation(exe_dir_).credentialsPath) << "no %LOCALAPPDATA% in these tests";
}

TEST_F(CredentialsDirFixture, TheOneArgumentFormsUseLocalAppData) {
  const std::wstring appdata = (root_ / "appdata").wstring();
  ScopedLocalAppData scoped(appdata);
  CachedAuthToken auth;
  auth.refresh_token = "via-env";
  auth.refresh_token_expiry = 2100000000U;
  ASSERT_TRUE(SaveAuthToken(auth, exe_dir_));
  EXPECT_TRUE(std::filesystem::exists(root_ / "appdata" / "nevr" / "echovr" / ".credentials.json"));
  EXPECT_EQ(LoadCachedAuthToken(exe_dir_).refresh_token, "via-env");
  EXPECT_EQ(std::filesystem::path(DefaultCredentialRoot()).lexically_normal(),
            (root_ / "appdata" / "nevr" / "echovr").lexically_normal());
}

TEST_F(CredentialsDirFixture, ARootThatIsNotAnsiGivesNoRootSoTheCacheStaysWhereItWorks) {
  if (GetACP() == CP_UTF8) GTEST_SKIP() << "the ANSI code page is UTF-8 here, so every path converts";
  ScopedLocalAppData scoped(L"C:\\Users\\\U0001F600\\AppData\\Local");
  EXPECT_TRUE(DefaultCredentialRoot().empty());
}

TEST_F(CredentialsDirFixture, NoLocalAppDataGivesNoRoot) { EXPECT_TRUE(DefaultCredentialRoot().empty()); }

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
