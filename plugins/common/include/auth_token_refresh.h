/* SYNTHESIS -- custom tool code, not from binary */

#pragma once

#include "auth_token.h"
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

    std::string url = nakama_url + "/v2/rpc/device/auth/refresh?http_key=" + http_key;
    nlohmann::json body;
    body["token"] = auth.refresh_token;

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

        std::string new_token = j.value("token", "");
        std::string new_refresh = j.value("refresh_token", "");

        if (new_token.empty()) {
            fprintf(stderr, "[NEVR.AUTH] Token refresh returned empty token\n");
            return false;
        }

        auth.token = new_token;
        // Default 1hr access token; TODO: parse exp from JWT claims
        auth.token_expiry = static_cast<uint64_t>(time(nullptr)) + 3600;

        if (!new_refresh.empty()) {
            auth.refresh_token = new_refresh;
            // Default 30-day refresh token; TODO: parse exp from JWT claims
            auth.refresh_token_expiry = static_cast<uint64_t>(time(nullptr)) + (30 * 24 * 3600);
        }

        SaveAuthToken(auth);
        fprintf(stderr, "[NEVR.AUTH] Token refreshed successfully\n");
        return true;
    } catch (const nlohmann::json::parse_error& e) {
        fprintf(stderr, "[NEVR.AUTH] Token refresh response parse error: %s\n", e.what());
        return false;
    }
}
