#include <gtest/gtest.h>

#include <cstdint>
#include <ctime>
#include <fstream>
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
