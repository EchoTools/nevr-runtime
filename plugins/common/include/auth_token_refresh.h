/* SYNTHESIS -- custom tool code, not from binary */

#pragma once

#include "core/auth_token.h"
#include "nevr_curl.h"

#include <nlohmann/json.hpp>
#include <cstdio>
#include <string>
#include <vector>

// Refresh an expired access token using the refresh token.
// Calls the custom device/auth/refresh RPC (not the standard Nakama session
// refresh, which uses a different signing key and requires session cache).
// On success, updates auth in-place and saves to disk. Returns true on success.
inline bool RefreshAuthToken(CachedAuthToken& auth,
                             const std::string& nakama_url,
                             const std::string& http_key) {
    if (auth.refresh_token.empty()) return false;

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::string url = nakama_url + "/v2/rpc/device/auth/refresh?http_key=" + http_key + "&unwrap";
    nlohmann::json body;
    // RFC 6749 §6 names this field `refresh_token`, and the RPC prefers it
    // (EchoTools/nakama f945f631d). Both are sent with the SAME value because the
    // two ends deploy on different days: a nakama older than that commit reads
    // only `token` and would reject a refresh_token-only body with
    // "invalid payload: token required".
    //
    // TEMPORARY. Delete the `token` line once no nakama older than f945f631d is
    // deployed — the new server ignores it whenever refresh_token is present, so
    // removing it is a no-op against current production and the only thing it can
    // still break is a rollback.
    body["refresh_token"] = auth.refresh_token;
    body["token"] = auth.refresh_token;  // deprecated: pre-RFC field name

    std::string post_data = body.dump();
    std::string response;

    // No Basic auth needed — the RPC uses http_key in the query param

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, nevr::CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
#ifdef NEVR_INSECURE_SKIP_TLS_VERIFY
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
#else
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
#endif

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        fprintf(stderr, "[NEVR.AUTH] Token refresh failed: %s\n", curl_easy_strerror(res));
        return false;
    }

    if (http_code != 200) {
        fprintf(stderr, "[NEVR.AUTH] Token refresh HTTP %ld: %s\n", http_code,
            response.empty() ? "(empty)" : response.substr(0, 200).c_str());
        return false;
    }

    try {
        auto j = nlohmann::json::parse(response);

        // RFC 6749 §5.1 `access_token`, falling back to the deprecated `token`.
        // The fallback is required, not defensive: a nakama older than
        // EchoTools/nakama f945f631d returns only `token`, so reading
        // access_token alone yields an empty token and fails every refresh
        // against a server that has not been redeployed yet.
        std::string new_token = j.value("access_token", "");
        if (new_token.empty()) new_token = j.value("token", "");
        std::string new_refresh = j.value("refresh_token", "");

        if (new_token.empty()) {
            fprintf(stderr, "[NEVR.AUTH] Token refresh returned empty token\n");
            return false;
        }

        const uint64_t now = static_cast<uint64_t>(time(nullptr));

        // Was `now + 60` unconditionally. That discarded what the issuer said:
        // the JWT carries its own `exp`, and the server now also states
        // `expires_in`, so a fixed 60s forced a refresh every minute for a token
        // that was valid for an hour. Same authority order as the device-poll
        // path (core/auth_token.h ResolveAccessTokenExpirySec) so the two ways of
        // obtaining an access token cannot disagree about when it dies.
        auth.token = new_token;
        auth.token_expiry = ResolveAccessTokenExpirySec(now, new_token, ReadExpiresInSeconds(j, "expires_in"));

        if (!new_refresh.empty()) {
            auth.refresh_token = new_refresh;
            // The server states `refresh_token_expires_in` as of f945f631d. When
            // it is absent — older server — this falls back to a constant that is
            // a GUESS at the server's policy, not a measurement of it; see
            // kFallbackRefreshTokenLifetimeSec.
            auth.refresh_token_expiry =
                ResolveRefreshTokenExpirySec(now, ReadExpiresInSeconds(j, "refresh_token_expires_in"));
        }

        SaveAuthToken(auth);
        fprintf(stderr, "[NEVR.AUTH] Token refreshed successfully\n");
        return true;
    } catch (const nlohmann::json::parse_error& e) {
        fprintf(stderr, "[NEVR.AUTH] Token refresh response parse error: %s\n", e.what());
        return false;
    }
}
