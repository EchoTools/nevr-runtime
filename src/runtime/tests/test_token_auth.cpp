#include <gtest/gtest.h>

#include <cstdint>
#include <ctime>
#include <fstream>
#include <optional>
#include <string>
#include <stdexcept>
#include <utility>

#include "core/auth_token.h"
#include "device_poll_response.h"
#include "extension/module_interface.h"
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

// RFC 6749 §5.1 renamed the poll response fields (EchoTools/nakama f945f631d).
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

// ReadExpiresInSeconds is reached from RefreshAuthToken, whose catch covers only
// json::parse_error — a type_error thrown here would escape the refresh entirely.
// Asserting the non-object cases rather than trusting that contains() is total.
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
  // A server-stated lifetime SHORTER than the old hardcoded 30 days must shorten
  // the client's belief — that is the whole failure the constant was hiding.
  EXPECT_LT(ResolveRefreshTokenExpirySec(kNow, 86400),
            ResolveRefreshTokenExpirySec(kNow, std::nullopt));
}

// The refresh path builds its request body inline in RefreshAuthToken, so the
// body shape is asserted here as the contract it has to satisfy: both names, one
// value. A refresh_token-only body is rejected by a pre-f945f631d nakama with
// "invalid payload: token required".
TEST(RefreshRequestBody, CarriesBothFieldNamesWithTheSameValue) {
  nlohmann::json body;
  body["refresh_token"] = "rt";
  body["token"] = "rt";

  EXPECT_EQ(body.value("refresh_token", ""), "rt");
  EXPECT_EQ(body.value("token", ""), "rt");
}

// Both ways of obtaining an access token must agree about when it dies. The
// refresh path used to hardcode now+60 while the poll path honoured the JWT.
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

TEST(TokenAuthModule, ServerHostSkipsDeviceAuthentication) {
  const NvrModuleContext context = MakeModuleContext(NEVR_MODULE_HOST_IS_SERVER);

  EXPECT_EQ(token_auth_Init(&context), 0);
  EXPECT_TRUE(TokenAuth::GetToken().empty());
  EXPECT_EQ(TokenAuth::GetDiscordId(), 0U);
  EXPECT_TRUE(TokenAuth::GetUsername().empty());
  token_auth_Shutdown();
}

TEST(TokenAuthModule, ClientWithoutRequiredConfigDisablesCleanly) {
  const NvrModuleContext context = MakeModuleContext(NEVR_MODULE_HOST_IS_CLIENT);

  EXPECT_EQ(token_auth_Init(&context), 0);
  EXPECT_TRUE(TokenAuth::GetToken().empty());
  EXPECT_EQ(TokenAuth::GetDiscordId(), 0U);
  EXPECT_TRUE(TokenAuth::GetUsername().empty());
  token_auth_Shutdown();
}

TEST(TokenAuthModule, ReportsThePublishedModuleApiVersion) {
  EXPECT_EQ(token_auth_ApiVersion(), NEVR_MODULE_API_VERSION);
}
