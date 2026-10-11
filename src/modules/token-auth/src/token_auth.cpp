/* SYNTHESIS -- custom tool code, not from binary */
/* Module version — uses NvrModuleContext instead of config.h globals */
/* Log levels follow the N47 taxonomy: phase outcomes at Info, per-step detail
 * at Debug. Merge 2f29312 once reverted nine Debug demotions here by taking a
 * stale branch copy wholesale — a revert-by-merge diffs clean against both
 * parents, so the N94 verify sensor pins those nine lines at Debug instead. */

#include "token_auth.h"
#include "core/curl_global.h"
#include "device_poll_response.h"
#include "off_thread_wait.h"
#include "extension/module_interface.h"
#include "abi/echovr_functions.h"
#include "core/logging.h"

#include "core/auth_token.h"
#include "core/auth_refresh.h"
#include "core/device_auth_flow.h"
#include "core/signin_dialog_text.h"
#include "core/signin_dialog_lifecycle.h"
#include "auth_token_refresh.h"
#include "core/bounded_retry.h"
#include "nevr_curl.h"
#include "runtime/log/url_diagnostics.h"
#include "runtime/log/security_diagnostics.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <vector>
#endif


namespace {

struct InternalDeviceAuthFlowOps {
    using Clock = std::chrono::steady_clock;
    std::function<Clock::time_point()> now;
    std::function<std::string()> requestDeviceCode;
    std::function<intptr_t(const std::string&)> openBrowser;
    std::function<int(const std::string&, const std::string&, intptr_t)> showOpenFailure;
    std::function<nevr_token_auth::DevicePollResponse(const std::string&)> poll;
    std::function<void(Clock::duration)> sleep;
    std::function<bool()> save;
    std::function<void(EchoVR::LogLevel, const std::string&)> log;
    // Optional: true once the flow should stop (the game is closing while it waits, #37).
    std::function<bool()> cancelled;
    // Optional (#397): the code is issued / the wait ended. The Windows client shows them in a dialog.
    std::function<void(const std::string& code, const std::string& loginUrl)> onCodeIssued;
    std::function<void(nevr::auth::FlowEnd)> onEnd;
};

// What the sign-in wait shows the player: set by the off-bootstrap wait, null in tests and on servers.
struct SignInPresenter {
    std::function<void(const std::string& code, const std::string& loginUrl)> codeIssued;
    std::function<void(nevr::auth::FlowEnd)> ended;
};

// Consecutive failed polls (timeout, transport error) the wait tolerates before it gives up (#202).
static constexpr unsigned kMaxConsecutivePollErrors = 5;
static constexpr char kDeviceLoginUrl[] = "https://echovrce.com/login/device";

// ---------------------------------------------------------------------------
// DeviceAuth — adapted from plugins/token-auth/src/device_auth.{h,cpp}
// ---------------------------------------------------------------------------

class DeviceAuth {
public:
    void Configure(const std::string& url, const std::string& httpKey, const std::string& serverKey);
    bool TryLoadCachedToken();
    bool RunDeviceAuthFlow(bool is_server);
    // Sleeps wait on `cancel` and the flow stops once it is requested (nullptr: plain sleeps).
    bool RunDeviceAuthFlow(bool is_server, nevr_token_auth::AuthCancellation* cancel,
                           const SignInPresenter* ui = nullptr);
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
    nevr_token_auth::DevicePollResponse PollDeviceCode(const std::string& code);
    void ApplyVerifiedPollResponse(const nevr_token_auth::DevicePollResponse& response,
                                   const InternalDeviceAuthFlowOps& ops);
    std::string HttpPostPublic(const std::string& url, const std::string& body, long* httpCode = nullptr);

#ifdef NEVR_TEST_HOOKS
public:
    void SetStateForTest(const nevr_token_auth::test_hook::DeviceAuthState& state);
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
        nevr_log_diagnostics::FormatRedactedUrlDiagnostic("[NEVR.AUTH] Configured: url=", url);
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
        // One slow or dropped request must not throw away a good cached login: three tries, 2 s apart
        // (each try is bounded by the request's own 10 s timeout). #202
        const nevr::RetryResult refreshed = nevr::RetryBounded(3, 2000, [&] {
            return RefreshAuthToken(auth, m_url, m_httpKey);
        });
        if (refreshed.attempts > 1) {
            Log(EchoVR::LogLevel::Info, "[NEVR.AUTH] token refresh during cache load: %s after %d attempts",
                refreshed.ok ? "succeeded" : "failed", refreshed.attempts);
        }
        if (refreshed.ok) {
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

std::string DeviceAuth::HttpPostPublic(const std::string& url, const std::string& body, long* httpCode) {
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
    if (httpCode != nullptr && res == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, httpCode);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        const std::string diagnostic = nevr_log_diagnostics::FormatRedactedUrlDiagnostic(
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

nevr_token_auth::DevicePollResponse DeviceAuth::PollDeviceCode(const std::string& code) {
    std::string url = m_url + "/v2/rpc/device/auth/poll?http_key=" + m_httpKey + "&unwrap";
    nlohmann::json reqBody;
    reqBody["code"] = code;
    long httpCode = 0;
    std::string response = HttpPostPublic(url, reqBody.dump(), &httpCode);
    if (response.empty()) {
        nevr_token_auth::DevicePollResponse none;
        none.http_code = httpCode;
        return none;
    }
    nevr_token_auth::DevicePollResponse parsed = nevr_token_auth::ParseDevicePollResponse(response);
    parsed.http_code = httpCode;
    parsed.body_prefix = nevr_token_auth::PollBodyPrefix(response, code);
    return parsed;
}

void DeviceAuth::ApplyVerifiedPollResponse(const nevr_token_auth::DevicePollResponse& response,
                                          const InternalDeviceAuthFlowOps& ops) {
    const uint64_t now = static_cast<uint64_t>(time(nullptr));
    std::string token = response.access_token;
    const uint64_t tokenExpiry = nevr_token_auth::ResolveAccessTokenExpiry(now, token, response.expires_in);
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

bool DeviceAuth::RunDeviceAuthFlow(bool is_server) { return RunDeviceAuthFlow(is_server, nullptr); }

bool DeviceAuth::RunDeviceAuthFlow(bool is_server, nevr_token_auth::AuthCancellation* cancel,
                                   const SignInPresenter* ui) {
    InternalDeviceAuthFlowOps ops;
    if (ui != nullptr) {
        ops.onCodeIssued = ui->codeIssued;
        ops.onEnd = ui->ended;
    }
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
    if (cancel != nullptr) {
        ops.sleep = [cancel](InternalDeviceAuthFlowOps::Clock::duration duration) { (void)cancel->WaitFor(duration); };
        ops.cancelled = [cancel]() { return cancel->IsStopRequested(); };
    } else {
        ops.sleep = [](InternalDeviceAuthFlowOps::Clock::duration duration) { std::this_thread::sleep_for(duration); };
    }
    ops.save = [this]() { return SaveToken(); };
    ops.log = [](EchoVR::LogLevel level, const std::string& message) { Log(level, "%s", message.c_str()); };
    return RunDeviceAuthFlow(is_server, ops);
}

// nevr_token_auth::Init runs this on a worker thread and waits for it before module
// initialization returns, pumping the bootstrap thread's messages meanwhile
// (#37). HTTP, ShellExecuteA, and the fallback modal MessageBoxA can block
// beyond the five-minute deadline. When those calls return after it, the
// result is rejected unless it is a poll that answers "verified": the server
// has then deleted the code and handed over the only copy of the tokens, so
// that answer is applied. The deadline does not cancel or bound the calls
// themselves.
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

    // The loop itself (request, open the login page, poll until verified / expired /
    // error / deadline / cancelled) is the platform-neutral nevr::auth core, shared
    // with the Quest shim; this adapter maps the module's ops onto it.
    nevr::auth::DeviceFlowOps core;
    core.now = ops.now;
    core.request_device_code = ops.requestDeviceCode;
    core.open_browser = ops.openBrowser;
    core.show_open_failure = ops.showOpenFailure;
    core.poll = ops.poll;
    core.sleep = ops.sleep;
    core.cancelled = ops.cancelled;
    core.on_code_issued = ops.onCodeIssued;
    core.on_end = ops.onEnd;
    core.max_consecutive_poll_errors = kMaxConsecutivePollErrors;
    core.log = [&ops](nevr::auth::LogLevel level, const std::string& message) {
        ops.log(nevr::auth::ToEchoLogLevel(level), message);
    };
    const nevr::auth::DeviceFlowResult flow = nevr::auth::RunDeviceCodeFlow(core, kDeviceLoginUrl);
    if (!flow.verified) return false;

    ApplyVerifiedPollResponse(flow.response, ops);
    log(EchoVR::LogLevel::Info, "[NEVR.AUTH] Device authorization completed");
    try {
        (void)ops.save();
    } catch (const std::exception&) {
        log(EchoVR::LogLevel::Warning,
            "[NEVR.AUTH] credential cache save failed; in-memory authentication remains active");
    }
    return true;
}

#ifdef NEVR_TEST_HOOKS
void DeviceAuth::SetStateForTest(const nevr_token_auth::test_hook::DeviceAuthState& state) {
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

static nevr_token_auth::AuthSnapshotStore s_snapshotStore;
static DeviceAuth* s_auth = nullptr;
static bool s_authAttempted = false;
static std::thread* s_refreshThread = nullptr;
static std::atomic<bool> s_refreshRunning{false};
static std::mutex s_tokenMutex;
// N133 S5: the host's config accessor (ctx->config_get), stored at init. Reads
// config.yaml through the same path the runtime uses instead of the game JSON
// (early_config). May be NULL if loaded by a pre-v2 host.
static const char* (*s_configGet)(const char*) = nullptr;

static std::shared_ptr<const nevr_token_auth::AuthSnapshot> PublishAuthSnapshot(
    const DeviceAuth* auth, nevr_token_auth::AuthReadiness emptyState = nevr_token_auth::AuthReadiness::Failed) {
    nevr_token_auth::AuthSnapshot snapshot;
    if (auth != nullptr) {
        snapshot.access_token = auth->GetTokenValue();
        snapshot.access_expiry = auth->GetTokenExpiryValue();
        snapshot.discord_id = auth->GetDiscordIdValue();
        snapshot.user_id = auth->GetUserIdValue();
        snapshot.username = auth->GetUsernameValue();
        if (auth->IsAuthenticated()) {
            snapshot.readiness = nevr_token_auth::AuthReadiness::Ready;
        } else if (!snapshot.access_token.empty()) {
            snapshot.readiness = nevr_token_auth::AuthReadiness::Expired;
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
        // config_get returns NULL for an absent/unmapped key; token_auth treats that
        // as a missing key (warn + disable).
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
// The lead is nevr::auth::kRefreshLeadSec (core/auth_refresh.h). It equals
// kFallbackAccessTokenLifetimeSec (300, core/auth_token_model.h), the lifetime
// assumed for a token carrying neither a decodable `exp` nor a server
// `expires_in`: such a token is due for refresh the moment it is issued, so the
// every-60s loop refreshes it on each wake. Production nakama always signs a JWT
// with `exp`, so that case is not reached; a lead ABOVE the fallback is rejected
// at compile time by the static_assert in core/auth_refresh.h.

// The refresh thread's guard, split out of RefreshThreadFunc so it can be
// asserted in-process without the 60-second sleep.
//
// Reads the LIVE expiry off the running DeviceAuth, not token_expiry out of
// LoadCachedAuthToken(): that field is structurally always 0, because
// SaveAuthToken (core/auth_token.h) writes only the refresh token and
// identity — the access token is deliberately never persisted. A guard on the
// disk value would compare 0 against now+300, never hold, and the thread would
// issue an HTTP refresh every 60 seconds for the entire hour a perfectly valid
// token is alive. The disk value is for the load path only.
static bool ShouldRefreshAccessToken(const DeviceAuth& auth, uint64_t now) {
    return nevr::auth::AccessTokenNeedsRefresh(auth.GetTokenExpiryValue(), now);
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
                (void)PublishAuthSnapshot(auth, nevr_token_auth::AuthReadiness::Expired);
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

std::string nevr_token_auth::GetToken() {
    const std::shared_ptr<const AuthSnapshot> snapshot = GetAuthSnapshot();
    if (!snapshot || snapshot->readiness != AuthReadiness::Ready ||
        snapshot->access_expiry <= static_cast<uint64_t>(time(nullptr))) {
        return "";
    }
    return snapshot->access_token;
}

uint64_t nevr_token_auth::GetDiscordId() {
    const std::shared_ptr<const AuthSnapshot> snapshot = GetAuthSnapshot();
    return snapshot ? snapshot->discord_id : 0;
}

// N123. The username was already parsed from the auth response and already
// persisted to the credential cache — it had simply never been exposed, so the
// login payload sent a hardcoded literal instead. Deliberately does NOT require
// IsAuthenticated(): a cached username from a previous session is still a truer
// answer than a constant, and the caller falls back on empty.
std::string nevr_token_auth::GetUsername() {
    const std::shared_ptr<const AuthSnapshot> snapshot = GetAuthSnapshot();
    return snapshot ? snapshot->username : "";
}

std::shared_ptr<const nevr_token_auth::AuthSnapshot> nevr_token_auth::GetAuthSnapshot() {
    return s_snapshotStore.Read();
}

#ifdef NEVR_TEST_HOOKS
namespace nevr_token_auth::test_hook {
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
                                       const nevr_token_auth::test_hook::DeviceAuthFlowOps& injected) {
    DeviceAuth auth;
    auth.SetStateForTest(initial);
    InternalDeviceAuthFlowOps ops;
    ops.now = injected.now;
    ops.requestDeviceCode = injected.request_device_code;
    ops.openBrowser = injected.open_browser;
    ops.showOpenFailure = injected.show_open_failure;
    ops.poll = injected.poll;
    ops.sleep = injected.sleep;
    ops.onCodeIssued = injected.on_code_issued;
    ops.onEnd = injected.on_end;
    ops.save = injected.save;
    ops.log = injected.log;
    ops.cancelled = injected.cancelled;

    DeviceAuthFlowResult result;
    result.success = auth.RunDeviceAuthFlow(is_server, ops);
    result.state = SnapshotDeviceAuth(auth);
    return result;
}

}  // namespace nevr_token_auth::test_hook
#endif  // NEVR_TEST_HOOKS

// ---------------------------------------------------------------------------
// The sign-in wait (#37, beta gate G7)
// ---------------------------------------------------------------------------

#ifdef _WIN32
namespace {

constexpr wchar_t kSignInWindowTitle[] = L"Echo VR - sign in with Discord in your browser to continue";
constexpr std::chrono::milliseconds kSignInPumpInterval{50};

// While the device flow runs on a worker, keeps the bootstrap thread's message queue drained so Windows
// does not mark the game window "Not Responding", and titles that thread's windows with what the player
// has to do. Posted messages go to DefWindowProcW, not the game's window procedure (the game has not
// initialised past its command line yet); sent messages reach the window procedure inside PeekMessage,
// as in any pumping thread. Thread messages are re-posted and a WM_QUIT re-issued when the wait ends.
class SignInWindowWait {
public:
    explicit SignInWindowWait(nevr_token_auth::AuthCancellation& cancel) : m_cancel(cancel) {
        EnumThreadWindows(GetCurrentThreadId(), &SignInWindowWait::CollectThreadWindow,
                          reinterpret_cast<LPARAM>(this));
        EnumWindows(&SignInWindowWait::CountProcessWindow, reinterpret_cast<LPARAM>(this));
        for (Retitled& w : m_windows) SetWindowTextW(w.hwnd, kSignInWindowTitle);
    }

    ~SignInWindowWait() {
        for (const Retitled& w : m_windows) {
            if (IsWindow(w.hwnd)) SetWindowTextW(w.hwnd, w.title.c_str());
        }
        for (const MSG& m : m_threadMessages) PostThreadMessageW(GetCurrentThreadId(), m.message, m.wParam, m.lParam);
        if (m_quit) PostQuitMessage(m_quitCode);
    }

    SignInWindowWait(const SignInWindowWait&) = delete;
    SignInWindowWait& operator=(const SignInWindowWait&) = delete;

    void Pump() {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ++m_messages;
            if (msg.message == WM_QUIT) {
                m_quit = true;
                m_quitCode = static_cast<int>(msg.wParam);
                m_cancel.RequestStop();
            } else if (msg.hwnd == nullptr) {
                m_threadMessages.push_back(msg);
            } else {
                DefWindowProcW(msg.hwnd, msg.message, msg.wParam, msg.lParam);
            }
        }
    }

    size_t ThreadWindows() const { return m_windows.size(); }
    size_t ProcessWindows() const { return m_processWindows; }
    unsigned Messages() const { return m_messages; }
    bool QuitSeen() const { return m_quit; }

private:
    struct Retitled {
        HWND hwnd;
        std::wstring title;
    };

    static BOOL CALLBACK CollectThreadWindow(HWND hwnd, LPARAM self) {
        if (!IsWindowVisible(hwnd) || GetParent(hwnd) != nullptr) return TRUE;
        wchar_t title[256] = {};
        GetWindowTextW(hwnd, title, 256);
        reinterpret_cast<SignInWindowWait*>(self)->m_windows.push_back({hwnd, title});
        return TRUE;
    }

    static BOOL CALLBACK CountProcessWindow(HWND hwnd, LPARAM self) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd)) ++reinterpret_cast<SignInWindowWait*>(self)->m_processWindows;
        return TRUE;
    }

    nevr_token_auth::AuthCancellation& m_cancel;
    std::vector<Retitled> m_windows;
    std::vector<MSG> m_threadMessages;
    size_t m_processWindows = 0;
    unsigned m_messages = 0;
    bool m_quit = false;
    int m_quitCode = 0;
};

// COM for the worker thread: ShellExecute is documented to need it on the calling thread.
class ComApartment {
public:
    ComApartment() : m_hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}
    ~ComApartment() {
        if (SUCCEEDED(m_hr)) CoUninitialize();
    }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

private:
    HRESULT m_hr;
};

// The sign-in window (#397): the code and the page, on a thread of its own so the bootstrap thread's
// pump (which dispatches nothing to windows it does not own) and the game never have to know about it.
// It is informational: closing it does not cancel the sign-in. When the wait ends it shows how, and
// closes by itself (signed in) or when the player dismisses it or after kSignInDialogLinger.
constexpr UINT kDialogApply = WM_APP + 1;
constexpr UINT kDialogClose = WM_APP + 2;
constexpr UINT_PTR kDialogLingerTimer = 1;
constexpr UINT kSignInDialogLingerMs = 30000;
constexpr int kDialogCloseButton = 100;

std::wstring WideFromUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

class SignInDialog {
public:
    SignInDialog() : m_state(std::make_shared<State>()) {}
    ~SignInDialog() {
        // A wait that never reported its end must not leave the dialog up; one that did keeps its message
        // until the player dismisses it (or kSignInDialogLingerMs). A window that does not exist yet closes
        // itself when it does (SignInDialogLifecycle).
        if (m_started && !m_ended) Abandon();
    }
    SignInDialog(const SignInDialog&) = delete;
    SignInDialog& operator=(const SignInDialog&) = delete;

    // Starts the window thread and waits (bounded) for the window to exist. False: no window was made.
    bool Show(const nevr::auth::SignInDialogContent& content) {
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            m_state->content = content;
        }
        if (!m_started) {
            m_started = true;
            std::shared_ptr<State> state = m_state;
            std::thread([state]() { WindowThread(state); }).detach();
            std::unique_lock<std::mutex> lock(m_state->mutex);
            m_state->ready.wait_for(lock, std::chrono::seconds(3), [this]() { return m_state->created || m_state->failed; });
            if (m_state->created) return true;
            // Gave up waiting: a window that appears later closes itself instead of staying topmost forever.
            (void)m_state->lifecycle.Abandon();
            return false;
        }
        PostIfCreated(kDialogApply, 0);
        return true;
    }

    // The wait ended: the window says how (and closes itself when `content.closes_by_itself`).
    void Ended(const nevr::auth::SignInDialogContent& content) {
        m_ended = true;
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            m_state->content = content;
        }
        PostIfCreated(kDialogApply, content.closes_by_itself ? 1 : 2);
    }

private:
    struct State {
        std::mutex mutex;
        std::condition_variable ready;
        nevr::auth::SignInDialogContent content;
        HWND hwnd = nullptr;
        HWND instruction = nullptr;
        HWND code = nullptr;
        HWND button = nullptr;
        HFONT codeFont = nullptr;
        bool created = false;
        bool failed = false;
        nevr::auth::SignInDialogLifecycle lifecycle;
    };

    // The wait is over without a result to show: close the window now, or have it close on creation.
    void Abandon() {
        bool closeNow = false;
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            closeNow = !m_state->lifecycle.Abandon();
        }
        if (closeNow) PostIfCreated(kDialogClose, 0);
    }

    void PostIfCreated(UINT message, WPARAM wParam) {
        HWND hwnd = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            if (m_state->created) hwnd = m_state->hwnd;
        }
        if (hwnd != nullptr) PostMessageW(hwnd, message, wParam, 0);
    }

    static void Apply(State& s, WPARAM how) {
        nevr::auth::SignInDialogContent content;
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            content = s.content;
        }
        SetWindowTextW(s.hwnd, WideFromUtf8(content.title).c_str());
        SetWindowTextW(s.instruction, WideFromUtf8(content.instruction).c_str());
        SetWindowTextW(s.code, WideFromUtf8(content.code).c_str());
        if (how == 1) {  // closes by itself: a moment to read it
            SetTimer(s.hwnd, kDialogLingerTimer, 1500, nullptr);
        } else if (how == 2) {
            ShowWindow(s.button, SW_SHOW);
            SetTimer(s.hwnd, kDialogLingerTimer, kSignInDialogLingerMs, nullptr);
        }
        nevr::auth::WipeSecret(content.code);
    }

    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        State* s = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (message) {
            case WM_NCCREATE: {
                const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
                break;
            }
            case kDialogApply:
                if (s != nullptr) Apply(*s, wParam);
                return 0;
            case kDialogClose:
            case WM_TIMER:
                DestroyWindow(hwnd);
                return 0;
            case WM_COMMAND:
                if (LOWORD(wParam) == kDialogCloseButton) DestroyWindow(hwnd);
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            default:
                break;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    static void WindowThread(std::shared_ptr<State> state) {
        static const wchar_t kClass[] = L"NevrSignInDialog";
        HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSW wc = {};
        wc.lpfnWndProc = &SignInDialog::Proc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = kClass;
        RegisterClassW(&wc);  // fails harmlessly with ERROR_CLASS_ALREADY_EXISTS on a second dialog

        constexpr int kWidth = 520;
        constexpr int kHeight = 330;
        const int x = (GetSystemMetrics(SM_CXSCREEN) - kWidth) / 2;
        const int y = (GetSystemMetrics(SM_CYSCREEN) - kHeight) / 3;
        HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, kClass, L"Echo VR - sign in",
                                    WS_CAPTION | WS_SYSMENU | WS_VISIBLE, x, y, kWidth, kHeight, nullptr, nullptr,
                                    instance, state.get());
        if (hwnd == nullptr) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->failed = true;
            state->ready.notify_all();
            return;
        }
        HFONT guiFont = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        state->hwnd = hwnd;
        state->instruction = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTER, 20, 16, kWidth - 56,
                                             110, hwnd, nullptr, instance, nullptr);
        state->codeFont = CreateFontW(-40, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                      CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        state->code = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTER, 20, 140, kWidth - 56, 56,
                                      hwnd, nullptr, instance, nullptr);
        state->button = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | BS_DEFPUSHBUTTON, (kWidth - 96) / 2, 220, 96, 30,
                                        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDialogCloseButton)),
                                        instance, nullptr);
        SendMessageW(state->instruction, WM_SETFONT, reinterpret_cast<WPARAM>(guiFont), TRUE);
        SendMessageW(state->code, WM_SETFONT, reinterpret_cast<WPARAM>(state->codeFont), TRUE);
        SendMessageW(state->button, WM_SETFONT, reinterpret_cast<WPARAM>(guiFont), TRUE);
        Apply(*state, 0);
        SetForegroundWindow(hwnd);
        bool closeAtOnce = false;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->created = true;
            closeAtOnce = state->lifecycle.OnCreated();
        }
        state->ready.notify_all();
        if (closeAtOnce) DestroyWindow(hwnd);  // created after the caller stopped waiting

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->created = false;
            state->hwnd = nullptr;
            nevr::auth::WipeSecret(state->content.code);
        }
        if (state->codeFont != nullptr) DeleteObject(state->codeFont);
    }

    std::shared_ptr<State> m_state;
    bool m_started = false;
    bool m_ended = false;
};

}  // namespace
#endif  // _WIN32

// Runs the device flow without freezing the game window: on a worker thread, while the bootstrap
// thread pumps its messages (Windows), and returns once the flow has finished.
static bool RunDeviceAuthFlowOffBootstrapThread(DeviceAuth& auth) {
    nevr_token_auth::AuthCancellation cancel;
#ifdef _WIN32
    SignInWindowWait wait(cancel);
    Log(EchoVR::LogLevel::Info,
        "[NEVR.AUTH] sign-in wait started off the bootstrap thread: windows on this thread=%zu in process=%zu "
        "(retitled while waiting)",
        wait.ThreadWindows(), wait.ProcessWindows());
    const auto start = std::chrono::steady_clock::now();
    SignInDialog dialog;
    SignInPresenter ui;
    ui.codeIssued = [&dialog](const std::string& code, const std::string& loginUrl) {
        const bool shown = dialog.Show(nevr::auth::SignInWaitingContent(loginUrl, code));
        Log(shown ? EchoVR::LogLevel::Info : EchoVR::LogLevel::Warning,
            shown ? "[NEVR.AUTH] sign-in dialog shown: the page and the code are on screen"
                  : "[NEVR.AUTH] sign-in dialog could not be created; the browser page is the only prompt");
    };
    ui.ended = [&dialog](nevr::auth::FlowEnd end) { dialog.Ended(nevr::auth::SignInEndedContent(end)); };
    const nevr_token_auth::OffThreadWaitResult r = nevr_token_auth::RunWhilePumping(
        [&auth, &cancel, &ui]() {
            ComApartment com;
            return auth.RunDeviceAuthFlow(false, &cancel, &ui);
        },
        [&wait]() { wait.Pump(); }, kSignInPumpInterval);
    const long long seconds =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
    Log(r.flowResult ? EchoVR::LogLevel::Info : EchoVR::LogLevel::Warning,
        "[NEVR.AUTH] sign-in wait ended: result=%s seconds=%lld messages_pumped=%u pump_calls=%u worker=%s quit_seen=%d",
        r.flowResult ? "ok" : (r.flowThrew ? "threw" : "failed"), seconds, wait.Messages(), r.pumpCalls,
        r.ranInline ? "inline (thread start failed)" : (r.ranOnOtherThread ? "separate" : "same"),
        wait.QuitSeen() ? 1 : 0);
    return r.flowResult;
#else
    return auth.RunDeviceAuthFlow(false, &cancel);
#endif
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void nevr_token_auth::Init(uintptr_t /*base_addr*/, bool is_server) {
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
        (void)PublishAuthSnapshot(nullptr, AuthReadiness::AwaitingUser);
        if (!RunDeviceAuthFlowOffBootstrapThread(*s_auth)) {
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

void nevr_token_auth::Shutdown() {
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
    nevr_token_auth::Init(ctx->base_addr, is_server);

    // Carry the real outcome, matching the richer sibling pattern in
    // platform_compat_Init (tls=%s createdir=%s msxml6=%s). Servers skip
    // token auth entirely (early return in nevr_token_auth::Init), hence "n/a".
    const bool authOk = !nevr_token_auth::GetToken().empty();
    Log(EchoVR::LogLevel::Info, "[NEVR.MODULE] token_auth initialized mode=%s auth=%s",
        is_server ? "server" : "client",
        is_server ? "n/a" : (authOk ? "ok" : "failed"));
    return 0;
}

NEVR_MODULE_API void token_auth_Shutdown(void) {
    nevr_token_auth::Shutdown();
}

// C exports for cross-module resolution (ws_bridge reads these via get_proc)
NEVR_MODULE_API const char* TokenAuth_GetToken(void) {
    s_tokenBuf = nevr_token_auth::GetToken();
    return s_tokenBuf.c_str();
}

NEVR_MODULE_API uint64_t TokenAuth_GetDiscordId(void) {
    return nevr_token_auth::GetDiscordId();
}

// N123. Returns "" when unknown — the caller decides what to do with an absent
// name. Same static-buffer shape as TokenAuth_GetToken above.
NEVR_MODULE_API const char* TokenAuth_GetUsername(void) {
    s_usernameBuf = nevr_token_auth::GetUsername();
    return s_usernameBuf.c_str();
}
