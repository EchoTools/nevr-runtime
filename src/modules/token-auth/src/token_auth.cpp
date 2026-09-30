/* SYNTHESIS -- custom tool code, not from binary */
/* Module version — uses NvrModuleContext instead of config.h globals */
/* Log levels follow the N47 taxonomy: phase outcomes at Info, per-step detail
 * at Debug. Merge 2f29312 once reverted nine Debug demotions here by taking a
 * stale branch copy wholesale — a revert-by-merge diffs clean against both
 * parents, so the N94 verify sensor pins those nine lines at Debug instead. */

#include "token_auth.h"
#include "core/curl_global.h"
#include "device_poll_response.h"
#include "extension/module_interface.h"
#include "abi/echovr_functions.h"
#include "core/logging.h"

#include "core/auth_token.h"
#include "auth_token_refresh.h"
#include "nevr_curl.h"
#include "runtime/log/url_diagnostics.h"
#include "runtime/log/security_diagnostics.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <exception>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif


namespace {

struct InternalDeviceAuthFlowOps {
    using Clock = std::chrono::steady_clock;
    std::function<Clock::time_point()> now;
    std::function<std::string()> requestDeviceCode;
    std::function<intptr_t(const std::string&)> openBrowser;
    std::function<int(const std::string&, const std::string&, intptr_t)> showOpenFailure;
    std::function<TokenAuth::DevicePollResponse(const std::string&)> poll;
    std::function<void(Clock::duration)> sleep;
    std::function<bool()> save;
    std::function<void(EchoVR::LogLevel, const std::string&)> log;
};

static constexpr InternalDeviceAuthFlowOps::Clock::duration kDeviceAuthLifetime = std::chrono::minutes(5);
static constexpr InternalDeviceAuthFlowOps::Clock::duration kDeviceAuthPollInterval = std::chrono::seconds(3);
static constexpr char kDeviceLoginUrl[] = "https://echovrce.com/login/device";

// ---------------------------------------------------------------------------
// DeviceAuth — adapted from plugins/token-auth/src/device_auth.{h,cpp}
// ---------------------------------------------------------------------------

class DeviceAuth {
public:
    void Configure(const std::string& url, const std::string& httpKey, const std::string& serverKey);
    bool TryLoadCachedToken();
    bool RunDeviceAuthFlow(bool is_server);
    bool RunDeviceAuthFlow(bool is_server, const InternalDeviceAuthFlowOps& ops);
    bool SaveToken();
    bool IsAuthenticated() const;
    std::string GetTokenValue() const { return m_token; }
    uint64_t GetDiscordIdValue() const { return m_discordId; }
    const std::string& GetRefreshTokenValue() const { return m_refreshToken; }
    uint64_t GetRefreshTokenExpiryValue() const { return m_refreshTokenExpiry; }
    const std::string& GetUserIdValue() const { return m_userId; }
    // Absolute unix expiry of the access token this instance is holding, or 0
    // when it holds none. This is the ONLY authority for when the live token
    // dies: the access token is never persisted (core/auth_token.h
    // SaveAuthToken), so no on-disk field tracks it.
    uint64_t GetTokenExpiryValue() const { return m_tokenExpiry; }
    std::string GetUsernameValue() const { return m_username; }
    void UpdateFromRefresh(const CachedAuthToken& auth);

private:
    std::string RequestDeviceCode();
    TokenAuth::DevicePollResponse PollDeviceCode(const std::string& code);
    void ApplyVerifiedPollResponse(const TokenAuth::DevicePollResponse& response,
                                   const InternalDeviceAuthFlowOps& ops);
    std::string HttpPostPublic(const std::string& url, const std::string& body);
    void DisplayLinkingCode(const InternalDeviceAuthFlowOps& ops);

#ifdef NEVR_TEST_HOOKS
public:
    void SetStateForTest(const TokenAuth::TestHook::DeviceAuthState& state);
#endif

private:

    std::string m_url;
    std::string m_httpKey;
    std::string m_serverKey;
    std::string m_token;
    uint64_t m_tokenExpiry = 0;
    std::string m_refreshToken;
    uint64_t m_refreshTokenExpiry = 0;
    std::string m_userId;
    std::string m_username;
    uint64_t m_discordId = 0;
    bool m_configured = false;
};

void DeviceAuth::Configure(const std::string& url, const std::string& httpKey, const std::string& serverKey) {
    m_url = url;
    m_httpKey = httpKey;
    m_serverKey = serverKey;
    m_configured = true;
    const std::string diagnostic =
        LogDiagnostics::FormatRedactedUrlDiagnostic("[NEVR.AUTH] Configured: url=", url);
    Log(EchoVR::LogLevel::Info, "%s", diagnostic.c_str());
}

bool DeviceAuth::IsAuthenticated() const {
    return !m_token.empty() && static_cast<uint64_t>(time(nullptr)) < m_tokenExpiry;
}

bool DeviceAuth::TryLoadCachedToken() {
    auto auth = LoadCachedAuthToken();
    // The access token is deliberately NOT persisted to disk — only the
    // refresh token is saved.  Check for either token before bailing so
    // the refresh path below is reachable when only the refresh token exists.
    if (auth.token.empty() && auth.refresh_token.empty()) return false;

    if (auth.HasValidToken()) {
        m_token = auth.token;
        m_tokenExpiry = auth.token_expiry;
        m_refreshToken = auth.refresh_token;
        m_refreshTokenExpiry = auth.refresh_token_expiry;
        m_userId = auth.user_id;
        m_username = auth.username;
        m_discordId = auth.GetDiscordId();

        uint64_t remaining = (auth.token_expiry - static_cast<uint64_t>(time(nullptr))) / 60;
        Log(EchoVR::LogLevel::Debug, "[NEVR.AUTH] Loaded cached token (expires in %llum)",
            (unsigned long long)remaining);
        return true;
    }

    if (auth.HasValidRefreshToken() && m_configured) {
        Log(EchoVR::LogLevel::Debug, "[NEVR.AUTH] Access token expired, attempting refresh...");
        if (RefreshAuthToken(auth, m_url, m_httpKey)) {
            m_token = auth.token;
            m_tokenExpiry = auth.token_expiry;
            m_refreshToken = auth.refresh_token;
            m_refreshTokenExpiry = auth.refresh_token_expiry;
            m_userId = auth.user_id;
            m_username = auth.username;
            m_discordId = auth.GetDiscordId();
            Log(EchoVR::LogLevel::Info,
                "[NEVR.AUTH] token refresh succeeded during cache load, expires_in=%llus",
                (unsigned long long)(auth.token_expiry - static_cast<uint64_t>(time(nullptr))));
            return true;
        }
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.AUTH] token refresh failed during cache load, falling back to full device-code re-authentication");
    } else if (!auth.refresh_token.empty()) {
        Log(EchoVR::LogLevel::Debug, "[NEVR.AUTH] Both tokens expired -- will re-authenticate");
    } else {
        Log(EchoVR::LogLevel::Debug, "[NEVR.AUTH] Cached token expired, no refresh token -- will re-authenticate");
    }

    return false;
}

void DeviceAuth::UpdateFromRefresh(const CachedAuthToken& auth) {
    m_token = auth.token;
    m_tokenExpiry = auth.token_expiry;
    if (!auth.refresh_token.empty()) {
        m_refreshToken = auth.refresh_token;
        m_refreshTokenExpiry = auth.refresh_token_expiry;
    }
    if (!auth.user_id.empty()) m_userId = auth.user_id;
    if (!auth.username.empty()) m_username = auth.username;
    m_discordId = auth.GetDiscordId();
}

bool DeviceAuth::SaveToken() {
    if (m_refreshToken.empty()) return false;

    CachedAuthToken auth;
    // Access token deliberately NOT set — SaveAuthToken only persists
    // refresh_token + user_id + username.
    auth.refresh_token = m_refreshToken;
    auth.refresh_token_expiry = m_refreshTokenExpiry;
    auth.user_id = m_userId;
    auth.username = m_username;

    if (!SaveAuthToken(auth)) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.AUTH] failed to write .credentials.json — refresh token not persisted, next launch will require full re-authentication");
        return false;
    }

    Log(EchoVR::LogLevel::Debug, "[NEVR.AUTH] Refresh token saved to .credentials.json");
    return true;
}

std::string DeviceAuth::HttpPostPublic(const std::string& url, const std::string& body) {
    nevr::EnsureCurlGlobalInit();
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, nevr::CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
#ifdef NEVR_INSECURE_SKIP_TLS_VERIFY
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
#endif

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        const std::string diagnostic = LogDiagnostics::FormatRedactedUrlDiagnostic(
            "[NEVR.AUTH] POST ", url, " failed");
        Log(EchoVR::LogLevel::Warning, "%s curl_code=%d", diagnostic.c_str(), static_cast<int>(res));
        return "";
    }
    return response;
}

std::string DeviceAuth::RequestDeviceCode() {
    // STRANDED FIX, recovered 2026-07-27. Commit 7a03d8b ("fix: Nakama RPC unwrap
    // and LoadLibraryA fallbacks for hook install", 2026-04-10) added `&unwrap` to
    // both device-auth endpoints in src/runtime/token_auth.cpp. This module was
    // extracted the SAME DAY and the fix never crossed. That commit fixed two
    // things; the LoadLibraryA half reached platform-compat, this half did not.
    //
    // Without &unwrap, Nakama wraps an RPC response as {"payload":"<json string>"},
    // so the parse below looks for "token"/"status" at the top level and finds
    // nothing — device auth silently never completes.
    std::string url = m_url + "/v2/rpc/device/auth/request?http_key=" + m_httpKey + "&unwrap";
    std::string response = HttpPostPublic(url, "{}");
    if (response.empty()) return "";

    try {
        auto j = nlohmann::json::parse(response);
        return j.value("code", "");
    } catch (const nlohmann::json::exception&) {
        Log(EchoVR::LogLevel::Warning, "[NEVR.AUTH] device code request: malformed JSON response");
        return "";
    }
}

TokenAuth::DevicePollResponse DeviceAuth::PollDeviceCode(const std::string& code) {
    std::string url = m_url + "/v2/rpc/device/auth/poll?http_key=" + m_httpKey + "&unwrap";
    nlohmann::json reqBody;
    reqBody["code"] = code;
    std::string response = HttpPostPublic(url, reqBody.dump());
    if (response.empty()) return {};
    return TokenAuth::ParseDevicePollResponse(response);
}

void DeviceAuth::DisplayLinkingCode(const InternalDeviceAuthFlowOps& ops) {
    ops.log(EchoVR::LogLevel::Info, "[NEVR.AUTH] Device authorization started; browser opening requested");
    ops.log(EchoVR::LogLevel::Info, "[NEVR.AUTH] Device code expires in 5 minutes");
}

void DeviceAuth::ApplyVerifiedPollResponse(const TokenAuth::DevicePollResponse& response,
                                          const InternalDeviceAuthFlowOps& ops) {
    const uint64_t now = static_cast<uint64_t>(time(nullptr));
    std::string token = response.access_token;
    const uint64_t tokenExpiry = TokenAuth::ResolveAccessTokenExpiry(now, token, response.expires_in);
    CachedAuthToken tokenClaims;
    tokenClaims.token = token;
    const uint64_t jwtExpiry = tokenClaims.GetJwtExpiry();
    const uint64_t refreshTokenExpiry =
        ResolveRefreshTokenExpirySec(now, response.refresh_token_expires_in);
    CachedAuthToken claims;
    claims.token = token;
    const uint64_t discordId = claims.GetDiscordId();
    std::string refreshToken = response.refresh_token;
    std::string userId = response.user_id;
    std::string username = response.username;

    m_token.swap(token);
    m_tokenExpiry = tokenExpiry;
    m_refreshToken.swap(refreshToken);
    m_refreshTokenExpiry = refreshTokenExpiry;
    m_userId.swap(userId);
    m_username.swap(username);
    m_discordId = discordId;

    if (jwtExpiry > now) {
        ops.log(EchoVR::LogLevel::Info,
                "[NEVR.AUTH] token expiry from JWT exp: " + std::to_string(jwtExpiry) + " (" +
                    std::to_string(jwtExpiry - now) + "s from now)");
    } else if (response.expires_in.has_value()) {
        ops.log(EchoVR::LogLevel::Info,
                "[NEVR.AUTH] token expiry from expires_in: " + std::to_string(*response.expires_in) + "s");
    } else {
        ops.log(EchoVR::LogLevel::Warning,
                "[NEVR.AUTH] token carries no exp and no expires_in — falling back to " +
                    std::to_string(kFallbackAccessTokenLifetimeSec) + "s");
    }
    if (response.refresh_token_expires_in.has_value()) {
        ops.log(EchoVR::LogLevel::Info,
                "[NEVR.AUTH] refresh token expiry from refresh_token_expires_in: " +
                    std::to_string(*response.refresh_token_expires_in) + "s");
    } else {
        ops.log(EchoVR::LogLevel::Warning,
                "[NEVR.AUTH] server sent no refresh_token_expires_in — assuming " +
                    std::to_string(kFallbackRefreshTokenLifetimeSec) +
                    "s, which is a guess at its policy, not a measurement");
    }
}

bool DeviceAuth::RunDeviceAuthFlow(bool is_server) {
    InternalDeviceAuthFlowOps ops;
    ops.now = []() { return InternalDeviceAuthFlowOps::Clock::now(); };
    ops.requestDeviceCode = [this]() { return RequestDeviceCode(); };
#ifdef _WIN32
    ops.openBrowser = [](const std::string& url) {
        return reinterpret_cast<intptr_t>(ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    };
    ops.showOpenFailure = [](const std::string& code, const std::string& url, intptr_t result) {
        const std::string message = "The browser could not be opened (code " + std::to_string(result) +
                                   "). Visit " + url + " and enter device code: " + code;
        return static_cast<int>(MessageBoxA(nullptr, message.c_str(), "Echo VR device authorization", MB_OK));
    };
#else
    ops.openBrowser = [](const std::string&) { return static_cast<intptr_t>(0); };
    ops.showOpenFailure = [](const std::string&, const std::string&, intptr_t) { return 0; };
#endif
    ops.poll = [this](const std::string& code) { return PollDeviceCode(code); };
    ops.sleep = [](InternalDeviceAuthFlowOps::Clock::duration duration) { std::this_thread::sleep_for(duration); };
    ops.save = [this]() { return SaveToken(); };
    ops.log = [](EchoVR::LogLevel level, const std::string& message) { Log(level, "%s", message.c_str()); };
    return RunDeviceAuthFlow(is_server, ops);
}

// TokenAuth::Init calls this synchronously before module initialization
// returns. HTTP, ShellExecuteA, and the fallback modal MessageBoxA can block
// beyond the five-minute deadline; the deadline rejects any late result after
// those calls return, but does not cancel or bound the calls themselves.
bool DeviceAuth::RunDeviceAuthFlow(bool is_server, const InternalDeviceAuthFlowOps& ops) {
    const auto log = [&ops](EchoVR::LogLevel level, const std::string& message) {
        if (ops.log) ops.log(level, message);
    };
    if (is_server) {
        log(EchoVR::LogLevel::Info, "[NEVR.AUTH] Server mode disables device-code authentication");
        return false;
    }
    if (!m_configured) {
        log(EchoVR::LogLevel::Warning, "[NEVR.AUTH] Cannot run device auth -- not configured");
        return false;
    }
    if (!ops.now || !ops.requestDeviceCode || !ops.openBrowser || !ops.showOpenFailure || !ops.poll ||
        !ops.sleep || !ops.save || !ops.log) {
        log(EchoVR::LogLevel::Error, "[NEVR.AUTH] Device authorization operations are unavailable");
        return false;
    }

    std::string code = ops.requestDeviceCode();
    if (code.empty()) {
        log(EchoVR::LogLevel::Warning,
            "[NEVR.AUTH] device code request failed, cannot start device-auth flow");
        return false;
    }

    const InternalDeviceAuthFlowOps::Clock::time_point deadline = ops.now() + kDeviceAuthLifetime;
    DisplayLinkingCode(ops);

    const std::string loginUrl = std::string(kDeviceLoginUrl) + "?code=" + code;
    const intptr_t browserResult = ops.openBrowser(loginUrl);
    // The code is a credential for this session, so the log says where the browser was sent and
    // what the open returned, with the code masked (ShellExecute reports success above 32).
    log(EchoVR::LogLevel::Info,
        std::string("[NEVR.AUTH] browser open requested url=") + kDeviceLoginUrl + "?code=<" +
            std::to_string(code.size()) + " chars masked> shellexecute_result=" + std::to_string(browserResult) +
            (browserResult <= 32 ? " (failed)" : " (accepted)"));
    int uiResult = 1;
    if (browserResult <= 32) {
        uiResult = ops.showOpenFailure(code, kDeviceLoginUrl, browserResult);
    }

    const bool expiredAfterBrowserOrUi = ops.now() >= deadline;
    if (browserResult <= 32 && uiResult == 0) {
        log(EchoVR::LogLevel::Error,
            "[NEVR.AUTH] device authorization stopped because the browser could not be opened");
        return false;
    }
    if (expiredAfterBrowserOrUi) {
        log(EchoVR::LogLevel::Warning, "[NEVR.AUTH] Device auth timed out after 5 minutes");
        return false;
    }

    unsigned int pollCount = 0;
    while (true) {
        const InternalDeviceAuthFlowOps::Clock::time_point beforeSleep = ops.now();
        if (beforeSleep >= deadline) {
            log(EchoVR::LogLevel::Warning, "[NEVR.AUTH] Device auth timed out after 5 minutes");
            return false;
        }
        const InternalDeviceAuthFlowOps::Clock::duration remaining = deadline - beforeSleep;
        const InternalDeviceAuthFlowOps::Clock::duration wait =
            remaining < kDeviceAuthPollInterval ? remaining : kDeviceAuthPollInterval;
        if (wait > InternalDeviceAuthFlowOps::Clock::duration::zero()) ops.sleep(wait);
        if (ops.now() >= deadline) {
            log(EchoVR::LogLevel::Warning, "[NEVR.AUTH] Device auth timed out after 5 minutes");
            return false;
        }

        const TokenAuth::DevicePollResponse response = ops.poll(code);
        if (ops.now() >= deadline) {
            log(EchoVR::LogLevel::Warning, "[NEVR.AUTH] Device auth timed out after 5 minutes");
            return false;
        }

        switch (response.status) {
            case TokenAuth::DevicePollStatus::Verified:
                ApplyVerifiedPollResponse(response, ops);
                log(EchoVR::LogLevel::Info, "[NEVR.AUTH] Device authorization completed");
                try {
                    (void)ops.save();
                } catch (const std::exception&) {
                    log(EchoVR::LogLevel::Warning,
                        "[NEVR.AUTH] credential cache save failed; in-memory authentication remains active");
                }
                return true;
            case TokenAuth::DevicePollStatus::Expired:
                log(EchoVR::LogLevel::Warning,
                    "[NEVR.AUTH] Device code expired. Please restart to try again.");
                return false;
            case TokenAuth::DevicePollStatus::Pending:
                ++pollCount;
                if (pollCount % 10U == 0U) {
                    const auto left = std::chrono::duration_cast<std::chrono::seconds>(deadline - ops.now());
                    log(EchoVR::LogLevel::Debug, "[NEVR.AUTH] Still waiting for authorization (" +
                                                       std::to_string(left.count()) + "s remaining)");
                }
                break;
            case TokenAuth::DevicePollStatus::Error:
                log(EchoVR::LogLevel::Warning,
                    "[NEVR.AUTH] polling aborted after single error (no retry)");
                return false;
        }
    }
}

#ifdef NEVR_TEST_HOOKS
void DeviceAuth::SetStateForTest(const TokenAuth::TestHook::DeviceAuthState& state) {
    m_token = state.token;
    m_tokenExpiry = state.token_expiry;
    m_refreshToken = state.refresh_token;
    m_refreshTokenExpiry = state.refresh_token_expiry;
    m_userId = state.user_id;
    m_username = state.username;
    m_discordId = state.discord_id;
    m_configured = true;
}
#endif

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static TokenAuth::AuthSnapshotStore s_snapshotStore;
static DeviceAuth* s_auth = nullptr;
static bool s_authAttempted = false;
static std::thread* s_refreshThread = nullptr;
static std::atomic<bool> s_refreshRunning{false};
static std::mutex s_tokenMutex;
// N133 S5: the host's config accessor (ctx->config_get), stored at init. Reads
// config.yaml through the same path the runtime uses instead of the game JSON
// (early_config). May be NULL if loaded by a pre-v2 host.
static const char* (*s_configGet)(const char*) = nullptr;

static std::shared_ptr<const TokenAuth::AuthSnapshot> PublishAuthSnapshot(
    const DeviceAuth* auth, TokenAuth::AuthReadiness emptyState = TokenAuth::AuthReadiness::Failed) {
    TokenAuth::AuthSnapshot snapshot;
    if (auth != nullptr) {
        snapshot.access_token = auth->GetTokenValue();
        snapshot.access_expiry = auth->GetTokenExpiryValue();
        snapshot.discord_id = auth->GetDiscordIdValue();
        snapshot.user_id = auth->GetUserIdValue();
        snapshot.username = auth->GetUsernameValue();
        if (auth->IsAuthenticated()) {
            snapshot.readiness = TokenAuth::AuthReadiness::Ready;
        } else if (!snapshot.access_token.empty()) {
            snapshot.readiness = TokenAuth::AuthReadiness::Expired;
        } else {
            snapshot.readiness = emptyState;
        }
    } else {
        snapshot.readiness = emptyState;
    }
    return s_snapshotStore.Publish(std::move(snapshot));
}

struct AuthConfig {
    std::string url;
    std::string httpKey;
    std::string serverKey;
};

static AuthConfig LoadAuthConfig() {
    AuthConfig cfg;

    if (s_configGet) {
        // config_get returns NULL for an absent/unmapped key — same "missing"
        // signal the old JsonValueAsString(..., NULL, false) form returned, so
        // an absent key keeps token_auth's existing behaviour (warn + disable).
        const char* url  = s_configGet("nevr_http_uri");
        const char* key  = s_configGet("nevr_http_key");
        const char* skey = s_configGet("nevr_server_key");
        if (url)  cfg.url = url;
        if (key)  cfg.httpKey = key;
        if (skey) cfg.serverKey = skey;
    }

    return cfg;
}

} // anonymous namespace

// How long before expiry the background thread refreshes. Nakama issues a
// one-hour access token (EchoTools/nakama server/evr_device_auth.go:289, :386)
// and this thread wakes every 60s, so 300s is about five refresh attempts
// before the token actually dies — each failed attempt logs and retries on the
// next wake.
//
// CAUTION: this must stay BELOW kFallbackAccessTokenLifetimeSec (also 300,
// core/auth_token.h), which is the lifetime assumed for a token carrying
// neither a decodable `exp` nor a server `expires_in`. At equal values such a
// token satisfies this guard the instant it is issued and the every-60s refresh
// loop returns for that case. Production nakama always signs a JWT with `exp`,
// so nothing hits it today; raising either constant without the other would.
static constexpr uint64_t kRefreshLeadSec = 300;

// The refresh thread's guard, split out of RefreshThreadFunc so it can be
// asserted in-process without the 60-second sleep.
//
// Reads the LIVE expiry off the running DeviceAuth. It previously read
// token_expiry out of LoadCachedAuthToken(), and that field is structurally
// always 0: SaveAuthToken (core/auth_token.h) writes only the refresh token and
// identity — the access token is deliberately never persisted. So the guard
// compared 0 against now+300, never held, and the thread issued an HTTP refresh
// every 60 seconds for the entire hour a perfectly valid token was alive. The
// disk behaviour is correct; consulting disk for a memory-only value was not.
static bool ShouldRefreshAccessToken(const DeviceAuth& auth, uint64_t now) {
    return auth.GetTokenExpiryValue() <= now + kRefreshLeadSec;
}

static void RefreshThreadFunc(std::string url, std::string httpKey) {
    // Distinguishes a first refresh failure from a sustained one in the Warning
    // below, without a full escalation framework — reset on every success. A
    // plain local (not `static`) is correct here: this function IS the thread
    // body, so a fresh call (fresh thread start) already starts the streak at 0.
    int consecutiveFailures = 0;

    while (s_refreshRunning) {
        // Sleep 60 seconds between checks
        for (int i = 0; i < 60 && s_refreshRunning; i++) {
#ifdef _WIN32
            Sleep(1000);
#else
            struct timespec ts = {1, 0};
            nanosleep(&ts, nullptr);
#endif
        }
        if (!s_refreshRunning) break;

        uint64_t now = static_cast<uint64_t>(time(nullptr));
        DeviceAuth* auth = nullptr;
        {
            std::lock_guard<std::mutex> lk(s_tokenMutex);
            auth = s_auth;
            if (!auth) continue;
            if (auth->GetTokenExpiryValue() <= now) {
                (void)PublishAuthSnapshot(auth, TokenAuth::AuthReadiness::Expired);
            }
            if (!ShouldRefreshAccessToken(*auth, now)) continue;  // Still valid for >5 min
        }

        // The refresh TOKEN is read from disk on purpose: it is the one
        // credential SaveAuthToken persists, and RefreshAuthToken updates this
        // local copy in place and writes it back. Disk IO and HTTP happen with
        // no token-state lock held.
        auto cached = LoadCachedAuthToken();

        // Both branches report the LIVE expiry. Reading cached.token_expiry
        // here printed "Token expired <unix-time-now>s ago" on every wake,
        // because the field is always 0 — a log line that looked like a
        // measurement and was an artefact of the same defect as the guard.
        const uint64_t liveExpiry = auth->GetTokenExpiryValue();
        if (liveExpiry > now) {
            Log(EchoVR::LogLevel::Debug, "[NEVR.AUTH] Token expires in %llus — refreshing",
                (unsigned long long)(liveExpiry - now));
        } else {
            Log(EchoVR::LogLevel::Debug, "[NEVR.AUTH] Token expired %llus ago — refreshing",
                (unsigned long long)(now - liveExpiry));
        }

        if (cached.HasValidRefreshToken()) {
            if (RefreshAuthToken(cached, url, httpKey)) {
                Log(EchoVR::LogLevel::Info, "[NEVR.AUTH] Token refreshed successfully expires_in=%llus",
                    (unsigned long long)(cached.token_expiry - now));
                // The auth object remains alive until this thread is joined.
                // Publish the complete updated identity as one generation.
                {
                    std::lock_guard<std::mutex> lk(s_tokenMutex);
                    if (s_auth != auth) continue;
                    auth->UpdateFromRefresh(cached);
                    (void)PublishAuthSnapshot(auth);
                }
                consecutiveFailures = 0;
            } else {
                consecutiveFailures++;
                Log(EchoVR::LogLevel::Warning,
                    "[NEVR.AUTH] token refresh failed (%d consecutive attempt%s) — will retry in 60s",
                    consecutiveFailures, consecutiveFailures == 1 ? "" : "s");
            }
        }
    }
}

std::string TokenAuth::GetToken() {
    const std::shared_ptr<const AuthSnapshot> snapshot = GetAuthSnapshot();
    if (!snapshot || snapshot->readiness != AuthReadiness::Ready ||
        snapshot->access_expiry <= static_cast<uint64_t>(time(nullptr))) {
        return "";
    }
    return snapshot->access_token;
}

uint64_t TokenAuth::GetDiscordId() {
    const std::shared_ptr<const AuthSnapshot> snapshot = GetAuthSnapshot();
    return snapshot ? snapshot->discord_id : 0;
}

// N123. The username was already parsed from the auth response and already
// persisted to the credential cache — it had simply never been exposed, so the
// login payload sent a hardcoded literal instead. Deliberately does NOT require
// IsAuthenticated(): a cached username from a previous session is still a truer
// answer than a constant, and the caller falls back on empty.
std::string TokenAuth::GetUsername() {
    const std::shared_ptr<const AuthSnapshot> snapshot = GetAuthSnapshot();
    return snapshot ? snapshot->username : "";
}

std::shared_ptr<const TokenAuth::AuthSnapshot> TokenAuth::GetAuthSnapshot() {
    return s_snapshotStore.Read();
}

#ifdef NEVR_TEST_HOOKS
namespace TokenAuth::TestHook {
namespace {

DeviceAuthState SnapshotDeviceAuth(const DeviceAuth& auth) {
    DeviceAuthState state;
    state.authenticated = auth.IsAuthenticated();
    state.token = auth.GetTokenValue();
    state.token_expiry = auth.GetTokenExpiryValue();
    state.refresh_token = auth.GetRefreshTokenValue();
    state.refresh_token_expiry = auth.GetRefreshTokenExpiryValue();
    state.user_id = auth.GetUserIdValue();
    state.discord_id = auth.GetDiscordIdValue();
    state.username = auth.GetUsernameValue();
    return state;
}

}  // namespace

DeviceAuthState InspectInitialDeviceAuth() {
    DeviceAuth auth;
    return SnapshotDeviceAuth(auth);
}

DeviceAuthState InspectDeviceAuthAfterRefresh(const CachedAuthToken& cached) {
    DeviceAuth auth;
    // Configure exercises the production setup path without starting a device
    // flow.  The test-only endpoint is never contacted by UpdateFromRefresh.
    auth.Configure("https://test.invalid", "test-http-key", "test-server-key");
    auth.UpdateFromRefresh(cached);
    return SnapshotDeviceAuth(auth);
}

DeviceAuthState InspectDeviceAuthFromCache() {
    DeviceAuth auth;
    // This deliberately calls the same executable-relative LoadCachedAuthToken
    // path as Init(). Do not Configure: an expired legacy access token must
    // not attempt a refresh endpoint during this hermetic test.
    auth.TryLoadCachedToken();
    return SnapshotDeviceAuth(auth);
}

bool InspectRefreshDecision(const CachedAuthToken& live, uint64_t now) {
    DeviceAuth auth;
    // Configure exercises the production setup path without starting a device
    // flow. UpdateFromRefresh is how RefreshThreadFunc itself installs a new
    // token into the live instance, so this leaves DeviceAuth in exactly the
    // state the running thread would observe.
    auth.Configure("https://test.invalid", "test-http-key", "test-server-key");
    auth.UpdateFromRefresh(live);
    return ShouldRefreshAccessToken(auth, now);
}

DeviceAuthFlowResult RunDeviceAuthFlow(bool is_server, const DeviceAuthState& initial,
                                       const TokenAuth::TestHook::DeviceAuthFlowOps& injected) {
    DeviceAuth auth;
    auth.SetStateForTest(initial);
    InternalDeviceAuthFlowOps ops;
    ops.now = injected.now;
    ops.requestDeviceCode = injected.request_device_code;
    ops.openBrowser = injected.open_browser;
    ops.showOpenFailure = injected.show_open_failure;
    ops.poll = injected.poll;
    ops.sleep = injected.sleep;
    ops.save = injected.save;
    ops.log = injected.log;

    DeviceAuthFlowResult result;
    result.success = auth.RunDeviceAuthFlow(is_server, ops);
    result.state = SnapshotDeviceAuth(auth);
    return result;
}

}  // namespace TokenAuth::TestHook
#endif  // NEVR_TEST_HOOKS

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void TokenAuth::Init(uintptr_t /*base_addr*/, bool is_server) {
    if (s_authAttempted) return;
    AuthSnapshot initialSnapshot;
    initialSnapshot.readiness = AuthReadiness::Starting;
    (void)s_snapshotStore.Publish(std::move(initialSnapshot));
    // Always enter the device flow with the module's actual mode. The flow's
    // own first guard ensures server mode cannot issue HTTP, open a browser, or
    // display UI even if a future caller reaches it directly.
    if (is_server) {
        DeviceAuth serverAuth;
        (void)serverAuth.RunDeviceAuthFlow(is_server);
        (void)PublishAuthSnapshot(nullptr, AuthReadiness::Disabled);
        s_authAttempted = true;
        return;
    }

    AuthConfig cfg = LoadAuthConfig();
    if (cfg.url.empty() || cfg.httpKey.empty()) {
        Log(EchoVR::LogLevel::Warning, "[NEVR.AUTH] Missing nevr_http_uri or nevr_http_key -- token auth disabled");
        (void)PublishAuthSnapshot(nullptr, AuthReadiness::Disabled);
        s_authAttempted = true;
        return;
    }

    s_auth = new DeviceAuth();
    s_auth->Configure(cfg.url, cfg.httpKey, cfg.serverKey);

    // Try cached token first (with refresh if expired)
    if (s_auth->TryLoadCachedToken()) {
        Log(EchoVR::LogLevel::Info, "[NEVR.AUTH] Using cached credentials -- no login needed");
        s_authAttempted = true;
        // Fall through to start refresh thread below
    } else {
        // No cached credentials — run device auth now, before game connections start.
        s_authAttempted = true;
        Log(EchoVR::LogLevel::Info, "[NEVR.AUTH] No cached credentials — starting device code auth...");
        if (!s_auth->RunDeviceAuthFlow(is_server)) {
            Log(EchoVR::LogLevel::Warning, "[NEVR.AUTH] Authentication failed -- social features may be limited");
        }
    }

    (void)PublishAuthSnapshot(s_auth);

    // Start background refresh thread (both cached and fresh auth paths)
    if (s_auth->IsAuthenticated() && !cfg.httpKey.empty()) {
        s_refreshRunning = true;
        s_refreshThread = new std::thread(RefreshThreadFunc, cfg.url, cfg.httpKey);
    }
}

void TokenAuth::Shutdown() {
    // On a server, s_auth is never created (early return in Init above), so
    // this is a structurally-guaranteed no-op there — say so instead of
    // logging the same "complete" line regardless of whether anything ran.
    const bool wasActive = (s_auth != nullptr);
    AuthSnapshot stoppingSnapshot;
    stoppingSnapshot.readiness = AuthReadiness::Stopping;
    (void)s_snapshotStore.Publish(std::move(stoppingSnapshot));

    s_refreshRunning = false;
    if (s_refreshThread) {
        s_refreshThread->join();
        delete s_refreshThread;
        s_refreshThread = nullptr;
    }
    {
        std::lock_guard<std::mutex> lk(s_tokenMutex);
        delete s_auth;
        s_auth = nullptr;
    }
    s_authAttempted = false;
    (void)PublishAuthSnapshot(nullptr, AuthReadiness::Disabled);
    if (wasActive) {
        Log(EchoVR::LogLevel::Info, "[NEVR.AUTH] shutdown complete (was active: refresh thread stopped)");
    } else {
        Log(EchoVR::LogLevel::Info,
            "[NEVR.AUTH] shutdown complete (was inactive — token auth was disabled or never authenticated)");
    }
}

// ---------------------------------------------------------------------------
// Module interface
// ---------------------------------------------------------------------------

// Thread-local buffer for GetToken C export
static thread_local std::string s_tokenBuf;
static thread_local std::string s_usernameBuf;  // N123, same lifetime contract as s_tokenBuf

NEVR_MODULE_API uint32_t token_auth_ApiVersion(void) {
    return NEVR_MODULE_API_VERSION;
}

NEVR_MODULE_API int token_auth_Init(const NvrModuleContext* ctx) {
    EchoVR::g_GameBaseAddress = (CHAR*)ctx->base_addr;
    // Function pointers are host-owned and may already contain MinHook
    // trampolines. Reinitializing them here would discard those trampolines and
    // recurse through the corresponding game detours.
    s_configGet = ctx->config_get;  // N133 S5: read config.yaml, not early_config JSON

    bool is_server = (ctx->flags & NEVR_MODULE_HOST_IS_SERVER) != 0;
    TokenAuth::Init(ctx->base_addr, is_server);

    // Carry the real outcome, matching the richer sibling pattern in
    // platform_compat_Init (tls=%s createdir=%s winhttp=%s). Servers skip
    // token auth entirely (early return in TokenAuth::Init), hence "n/a".
    const bool authOk = !TokenAuth::GetToken().empty();
    Log(EchoVR::LogLevel::Info, "[NEVR.MODULE] token_auth initialized mode=%s auth=%s",
        is_server ? "server" : "client",
        is_server ? "n/a" : (authOk ? "ok" : "failed"));
    return 0;
}

NEVR_MODULE_API void token_auth_Shutdown(void) {
    TokenAuth::Shutdown();
}

// C exports for cross-module resolution (ws_bridge reads these via get_proc)
NEVR_MODULE_API const char* TokenAuth_GetToken(void) {
    s_tokenBuf = TokenAuth::GetToken();
    return s_tokenBuf.c_str();
}

NEVR_MODULE_API uint64_t TokenAuth_GetDiscordId(void) {
    return TokenAuth::GetDiscordId();
}

// N123. Returns "" when unknown — the caller decides what to do with an absent
// name. Same static-buffer shape as TokenAuth_GetToken above.
NEVR_MODULE_API const char* TokenAuth_GetUsername(void) {
    s_usernameBuf = TokenAuth::GetUsername();
    return s_usernameBuf.c_str();
}
