/* SYNTHESIS -- custom tool code, not from binary */

#include <gtest/gtest.h>
#include "auth_token.h"

#include <fstream>
#include <cstdio>
#include <ctime>
#include <string>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(d) _mkdir(d)
#define RMDIR(d) _rmdir(d)
#else
#include <sys/stat.h>
#define MKDIR(d) mkdir(d, 0755)
#define RMDIR(d) rmdir(d)
#endif

// Helper: write a string to a file
static void WriteFile(const char* path, const std::string& content) {
    std::ofstream f(path, std::ios::trunc);
    ASSERT_TRUE(f.is_open()) << "Failed to create " << path;
    f << content;
}

// Helper: create _local dir and clean up after test
class AuthTokenTest : public ::testing::Test {
protected:
    void SetUp() override {
        MKDIR("_local");
    }
    void TearDown() override {
        std::remove("_local/.credentials.json");
        std::remove("_local/config.json");
        RMDIR("_local");
    }
};

// --- LoadCachedAuthToken tests ---

TEST_F(AuthTokenTest, LoadCachedAuthToken_MissingFile) {
    std::remove("_local/.credentials.json");
    auto auth = LoadCachedAuthToken();
    EXPECT_TRUE(auth.token.empty());
    EXPECT_EQ(auth.token_expiry, 0u);
}

TEST_F(AuthTokenTest, LoadCachedAuthToken_MalformedJson) {
    WriteFile("_local/.credentials.json", "not json at all {{{");
    auto auth = LoadCachedAuthToken();
    EXPECT_TRUE(auth.token.empty());
}

TEST_F(AuthTokenTest, LoadCachedAuthToken_EmptyToken) {
    WriteFile("_local/.credentials.json", R"({"token":"","token_expiry":9999999999})");
    auto auth = LoadCachedAuthToken();
    EXPECT_TRUE(auth.token.empty());
}

TEST_F(AuthTokenTest, LoadCachedAuthToken_ValidToken) {
    uint64_t future = static_cast<uint64_t>(time(nullptr)) + 3600;
    uint64_t refresh_future = static_cast<uint64_t>(time(nullptr)) + (30 * 24 * 3600);

    nlohmann::json j;
    j["token"] = "access_jwt_here";
    j["token_expiry"] = future;
    j["refresh_token"] = "refresh_jwt_here";
    j["refresh_token_expiry"] = refresh_future;
    j["user_id"] = "user-123";
    j["username"] = "testplayer";

    WriteFile("_local/.credentials.json", j.dump(2));

    auto auth = LoadCachedAuthToken();
    EXPECT_EQ(auth.token, "access_jwt_here");
    EXPECT_EQ(auth.token_expiry, future);
    EXPECT_EQ(auth.refresh_token, "refresh_jwt_here");
    EXPECT_EQ(auth.refresh_token_expiry, refresh_future);
    EXPECT_EQ(auth.user_id, "user-123");
    EXPECT_EQ(auth.username, "testplayer");
    EXPECT_TRUE(auth.HasValidToken());
    EXPECT_TRUE(auth.HasValidRefreshToken());
}

TEST_F(AuthTokenTest, LoadCachedAuthToken_ExpiredAccessValidRefresh) {
    uint64_t past = static_cast<uint64_t>(time(nullptr)) - 100;
    uint64_t refresh_future = static_cast<uint64_t>(time(nullptr)) + (30 * 24 * 3600);

    nlohmann::json j;
    j["token"] = "expired_access";
    j["token_expiry"] = past;
    j["refresh_token"] = "valid_refresh";
    j["refresh_token_expiry"] = refresh_future;

    WriteFile("_local/.credentials.json", j.dump(2));

    auto auth = LoadCachedAuthToken();
    EXPECT_EQ(auth.token, "expired_access");
    EXPECT_FALSE(auth.HasValidToken());
    EXPECT_TRUE(auth.HasValidRefreshToken());
}

TEST_F(AuthTokenTest, LoadCachedAuthToken_BackwardsCompatOldExpiryField) {
    uint64_t future = static_cast<uint64_t>(time(nullptr)) + 3600;

    nlohmann::json j;
    j["token"] = "old_format_token";
    j["expiry"] = future;  // old field name

    WriteFile("_local/.credentials.json", j.dump(2));

    auto auth = LoadCachedAuthToken();
    EXPECT_EQ(auth.token, "old_format_token");
    EXPECT_EQ(auth.token_expiry, future);
    EXPECT_TRUE(auth.HasValidToken());
}

// --- SaveAuthToken tests ---

TEST_F(AuthTokenTest, SaveAuthToken_WritesValidJson) {
    // Create config.json so SaveAuthToken finds _local/
    WriteFile("_local/config.json", "{}");

    CachedAuthToken auth;
    auth.token = "saved_token";
    auth.token_expiry = 1234567890;
    auth.refresh_token = "saved_refresh";
    auth.refresh_token_expiry = 9876543210ULL;
    auth.user_id = "uid-456";
    auth.username = "saveduser";

    ASSERT_TRUE(SaveAuthToken(auth));

    // Read it back
    auto loaded = LoadCachedAuthToken();
    EXPECT_EQ(loaded.token, "saved_token");
    EXPECT_EQ(loaded.token_expiry, 1234567890u);
    EXPECT_EQ(loaded.refresh_token, "saved_refresh");
    EXPECT_EQ(loaded.refresh_token_expiry, 9876543210ULL);
    EXPECT_EQ(loaded.user_id, "uid-456");
    EXPECT_EQ(loaded.username, "saveduser");
}

TEST_F(AuthTokenTest, SaveAuthToken_EmptyTokenReturnsFalse) {
    CachedAuthToken auth;
    auth.token = "";
    EXPECT_FALSE(SaveAuthToken(auth));
}

TEST_F(AuthTokenTest, SaveAuthToken_MinimalFields) {
    WriteFile("_local/config.json", "{}");

    CachedAuthToken auth;
    auth.token = "minimal";
    auth.token_expiry = 999;

    ASSERT_TRUE(SaveAuthToken(auth));

    // Verify written JSON doesn't include empty optional fields
    std::ifstream f("_local/.credentials.json");
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    auto j = nlohmann::json::parse(content);
    EXPECT_TRUE(j.contains("token"));
    EXPECT_TRUE(j.contains("token_expiry"));
    EXPECT_FALSE(j.contains("refresh_token"));
    EXPECT_FALSE(j.contains("user_id"));
}

// --- HasValidToken / HasValidRefreshToken edge cases ---

TEST(CachedAuthTokenStruct, HasValidToken_WellExpired) {
    CachedAuthToken auth;
    auth.token = "test";
    auth.token_expiry = static_cast<uint64_t>(time(nullptr)) + 10;  // only 10s left
    EXPECT_FALSE(auth.HasValidToken());  // needs >60s remaining
}

TEST(CachedAuthTokenStruct, HasValidToken_WellValid) {
    CachedAuthToken auth;
    auth.token = "test";
    auth.token_expiry = static_cast<uint64_t>(time(nullptr)) + 3600;  // 1hr
    EXPECT_TRUE(auth.HasValidToken());
}

// --- Config parsing tests (verify nlohmann-json migration didn't break parsing) ---

TEST(ConfigParse, BroadcasterBridgeConfig) {
    std::string json = R"({
        "udp_debug_target": "192.168.1.100:9999",
        "listen_port": 8888,
        "mirror_send": true,
        "mirror_receive": false,
        "log_messages": true
    })";
    auto j = nlohmann::json::parse(json);
    EXPECT_EQ(j.value("udp_debug_target", ""), "192.168.1.100:9999");
    EXPECT_EQ(j.value("listen_port", 0), 8888);
    EXPECT_EQ(j.value("mirror_send", false), true);
    EXPECT_EQ(j.value("mirror_receive", true), false);
    EXPECT_EQ(j.value("log_messages", false), true);
}

TEST(ConfigParse, LogFilterConfig) {
    std::string json = R"({
        "min_level": 2,
        "suppress_channels": ["AUDIO", "PHYSICS"],
        "timestamps": true,
        "file_enabled": false,
        "truncate_patterns": [
            {"prefix": "[RENDER]", "max_length": 100}
        ]
    })";
    auto j = nlohmann::json::parse(json);
    EXPECT_EQ(j.value("min_level", 0u), 2u);
    EXPECT_EQ(j["suppress_channels"].size(), 2u);
    EXPECT_EQ(j["suppress_channels"][0].get<std::string>(), "AUDIO");
    EXPECT_EQ(j.value("timestamps", false), true);
    EXPECT_EQ(j["truncate_patterns"][0].value("prefix", ""), "[RENDER]");
    EXPECT_EQ(j["truncate_patterns"][0].value("max_length", 0), 100);
}

TEST(ConfigParse, TokenAuthConfig) {
    std::string json = R"({
        "nevr_url": "https://api.echovrce.com",
        "nevr_http_key": "test-key-123"
    })";
    auto j = nlohmann::json::parse(json);
    EXPECT_EQ(j.value("nevr_url", ""), "https://api.echovrce.com");
    EXPECT_EQ(j.value("nevr_http_key", ""), "test-key-123");
}
